#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libvfs/vfs.h"

#define FOURCC(tag) (unsigned char)((tag) >> 24), (unsigned char)((tag) >> 16), (unsigned char)((tag) >> 8), (unsigned char)(tag)

static int
str2hex(int buflen, unsigned char *buf, const char *str)
{
    unsigned char *ptr = buf;
    int seq = -1;
    while (buflen > 0) {
        int nibble = *str++;
        if (nibble >= '0' && nibble <= '9') {
            nibble -= '0';
        } else {
            nibble |= 0x20;
            if (nibble < 'a' || nibble > 'f') {
                break;
            }
            nibble -= 'a' - 10;
        }
        if (seq >= 0) {
            *buf++ = (seq << 4) | nibble;
            buflen--;
            seq = -1;
        } else {
            seq = nibble;
        }
    }
    return buf - ptr;
}

static int
read_file_silent(const char *name, unsigned char **buf, size_t *size)
{
    void *p;
    size_t n, sz;
    FHANDLE in = file_open(name, O_RDONLY);
    if (!in) {
        return -1;
    }
    sz = in->length(in);
    if ((ssize_t)sz < 0) {
        in->close(in);
        return -1;
    }
    p = malloc(sz);
    if (!p) {
        in->close(in);
        return -1;
    }
    n = in->read(in, p, sz);
    in->close(in);
    if (n != sz) {
        free(p);
        return -1;
    }
    *buf = p;
    *size = sz;
    return 0;
}

static int
read_file(const char *name, unsigned char **buf, size_t *size)
{
    int rv = read_file_silent(name, buf, size);
    if (rv) {
        fprintf(stderr, "[e] cannot read '%s'\n", name);
    }
    return rv;
}

static int
write_file_silent(const char *name, void *buf, size_t size)
{
    size_t written;
    FHANDLE out = file_open(name, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (!out) {
        return -1;
    }
    written = out->write(out, buf, size);
    if (written != size) {
        out->close(out);
        return -1;
    }
    return out->close(out);
}

static int
write_file(const char *name, void *buf, size_t size)
{
    int rv = write_file_silent(name, buf, size);
    if (rv) {
        fprintf(stderr, "[e] cannot write '%s'\n", name);
    }
    return rv;
}

typedef struct {
    size_t off;
    int skip;
    uint8_t ov, nv;
} PATCH;

static int
apply_patch(FHANDLE fd, const char *patchfile, int force, int undo)
{
    FILE *f;
    char buf[BUFSIZ];
    PATCH *patches = NULL;
    unsigned i, max = 0, len = 0;
    size_t length;
    int rv = 0;

    length = fd->length(fd);
    if ((ssize_t)length < 0) {
        return -1;
    }

    f = fopen(patchfile, "rt");
    if (!f) {
        fprintf(stderr, "[e] cannot read '%s'\n", patchfile);
        return -1;
    }

    while (fgets(buf, sizeof(buf), f)) {
        PATCH patch;
        char *p, *q = buf;
        p = strchr(buf, '\n');
        if (p == NULL) {
            fprintf(stderr, "[e] patch: malformed line\n");
            rv = -1;
            break;
        }
        if (p > buf && p[-1] == '\r') {
            p--;
        }
        *p = '\0';
        buf[strcspn(buf, "#;")] = '\0';
        if (buf[0] == '\0') {
            continue;
        }
        p = q;
        errno = 0;
        patch.off = strtoull(p, &q, 0);
        if (errno || p == q) {
            fprintf(stderr, "[e] patch: malformed line\n");
            rv = -1;
            break;
        }
        p = q;
        errno = 0;
        patch.ov = strtoul(p, &q, 0);
        if (errno || p == q) {
            fprintf(stderr, "[e] patch: malformed line\n");
            rv = -1;
            break;
        }
        p = q;
        errno = 0;
        patch.nv = strtoul(q, &q, 0);
        if (errno || p == q) {
            fprintf(stderr, "[e] patch: malformed line\n");
            rv = -1;
            break;
        }
        if (patch.off > length) {
            fprintf(stderr, "[e] patch: offset 0x%zx too big\n", patch.off);
            rv = -1;
            break;
        }
        if (len >= max) {
            PATCH *tmp;
            if (max == 0) {
                max = 8;
            }
            max *= 2;
            tmp = realloc(patches, max * sizeof(PATCH));
            if (!tmp) {
                fprintf(stderr, "[e] patch: out of memory\n");
                rv = -1;
                break;
            }
            patches = tmp;
        }
        patch.skip = 0;
        if (undo) {
            uint8_t tv = patch.ov;
            patch.ov = patch.nv;
            patch.nv = tv;
        }
        patches[len++] = patch;
    }

    fclose(f);

    if (rv) {
        free(patches);
        return rv;
    }

    for (i = 0; i < len; i++) {
        size_t n, off;
        unsigned char cv, nv, ov;
        PATCH *p = &patches[i];
        off = fd->lseek(fd, p->off, SEEK_SET);
        if (off != p->off) {
            fprintf(stderr, "[e] patch: cannot seek to 0x%zx\n", p->off);
            rv = -1;
            break;
        }
        n = fd->read(fd, &cv, 1);
        if (n != 1) {
            fprintf(stderr, "[e] patch: cannot read from 0x%zx\n", off);
            rv = -1;
            break;
        }
        nv = p->nv;
        ov = p->ov;
        if (cv != ov) {
            if (cv == nv) {
                fprintf(stderr, "[w] patch: offset 0x%zx is already patched: %02x\n", off, cv);
            } else {
                fprintf(stderr, "[w] patch: offset 0x%zx has %02x, expected %02x\n", off, cv, ov);
                if (!force) {
                    rv = -1;
                }
                break;
            }
        }
        p->skip = (cv == nv);
    }

    if (rv) {
        free(patches);
        return rv;
    }

    for (i = 0; i < len; i++) {
        size_t n, off;
        PATCH *p = &patches[i];
        if (p->skip) {
            continue;
        }
        off = fd->lseek(fd, p->off, SEEK_SET);
        if (off != p->off) {
            fprintf(stderr, "[e] patch: cannot seek to 0x%zx\n", p->off);
            rv = -1;
            break;
        }
        n = fd->write(fd, &p->nv, 1);
        if (n != 1) {
            fprintf(stderr, "[e] patch: cannot patch 0x%zx\n", off);
            rv = -1;
            break;
        }
    }

    free(patches);
    return rv;
}

static void __attribute__((noreturn))
usage(const char *argv0)
{
    printf("usage: %s -i <input> [-o <output>] [-k <ivkey>] [GETTERS] [MODIFIERS]\n", argv0);
    printf("    -i <file>       read from <file>\n");
    printf("    -o <file>       write image to <file>\n");
    printf("    -k <ivkey>      use <ivkey> to decrypt\n");
    printf("getters:\n");
    printf("    -e <file>       write extra to <file>\n");
    printf("    -g <file>       write keybag to <file>\n");
    printf("    -m <file>       write ticket to <file>\n");
    printf("    -c <info>       check signature with <info>\n");
    printf("    -n              print nonce\n");
    printf("modifiers:\n");
    printf("    -T <fourcc>     set type <fourcc>\n");
    printf("    -P[f|u] <file>  apply patch from <file> (f=force, u=undo)\n");
    printf("    -E <file>       set extra from <file>\n");
    printf("    -M <file>       set ticket from <file>\n");
    printf("    -N <nonce>      set <nonce> if ticket is set/present\n");
    printf("    -D              leave IMG4 decrypted\n");
    printf("note: if no modifier is present and -o is specified, extract the bare image\n");
    printf("note: if modifiers are present and -o is not specified, modify the input file\n");
    printf("note: sigcheck info is: \"CHIP=8960,ECID=0x1122334455667788[,...]\"\n");
    exit(0);
}

int
main(int argc, char **argv)
{
    const char *argv0 = argv[0];
    const char *iname = NULL;
    const char *oname = NULL;
    const char *ik = NULL;
    const char *ename = NULL;
    const char *gname = NULL;
    const char *mname = NULL;
    char *cinfo = NULL;
    int get_nonce = 0;
    const char *set_type = NULL;
    const char *set_patch = NULL;
    int pf = 0;
    int pu = 0;
    const char *set_extra = NULL;
    const char *set_manifest = NULL;
    int set_nonce = 0;
    uint64_t nonce = 0;
    int set_decrypt = 0;

    int rv, rc = 0;
    unsigned char *buf;
    size_t sz;
    int modify;
    unsigned type;
    FHANDLE fd, orig = NULL;
    unsigned char *k, ivkey[16 + 32];

    while (--argc > 0) {
        const char *arg = *++argv;
        if (*arg == '-') switch (arg[1]) {
            case 'h':
                usage(argv0);
                continue;
            case 'n':
                get_nonce = 1;
                continue;
            case 'D':
                set_decrypt = 1;
                continue;
            case 'i':
                if (argc >= 2) { iname = *++argv; argc--; continue; }
            case 'o':
                if (argc >= 2) { oname = *++argv; argc--; continue; }
            case 'k':
                if (argc >= 2) { ik = *++argv; argc--; continue; }
            case 'e':
                if (argc >= 2) { ename = *++argv; argc--; continue; }
            case 'g':
                if (argc >= 2) { gname = *++argv; argc--; continue; }
            case 'm':
                if (argc >= 2) { mname = *++argv; argc--; continue; }
            case 'c':
                if (argc >= 2) { cinfo = *++argv; argc--; continue; }
            case 'T':
                if (argc >= 2) { set_type = *++argv; argc--; continue; }
            case 'P':
                if (argc >= 2) { set_patch = *++argv; argc--; pf = (!!strchr(arg, 'f')); pu = (!!strchr(arg, 'u')); continue; }
            case 'E':
                if (argc >= 2) { set_extra = *++argv; argc--; continue; }
            case 'M':
                if (argc >= 2) { set_manifest = *++argv; argc--; continue; }
            case 'N':
                if (argc >= 2) { set_nonce = 1; nonce = strtoull(*++argv, NULL, 16); argc--; continue; }
            /* fallthrough */
                fprintf(stderr, "[e] argument to '%s' is missing\n", arg);
                return -1;
            default:
                fprintf(stderr, "[e] illegal option '%s'\n", arg);
                return -1;
        }
        /* img4 -i input -o output -k ivkey <=> img4 -i input output ivkey */
        if (!oname) {
            oname = arg;
        } else {
            ik = arg;
        }
    }

    if (!iname) {
        fprintf(stderr, "[e] no input file name\n");
        return -1;
    }

    modify = set_type || set_patch || set_extra || set_manifest || set_nonce || set_decrypt;

    k = (unsigned char *)ik;
    if (ik) {
        if (str2hex(sizeof(ivkey), ivkey, ik) != sizeof(ivkey)) {
            fprintf(stderr, "[e] invalid ivkey\n");
            return -1;
        }
        k = ivkey;
    }

    // open

    if (!modify) {
        fd = img4_reopen(file_open(iname, O_RDONLY), k);
    } else if (!oname) {
        fd = img4_reopen(file_open(iname, O_RDWR), k);
    } else {
        fd = img4_reopen(orig = memory_open_from_file(iname, O_RDWR), k);
    }
    if (!fd) {
        fprintf(stderr, "[e] cannot open '%s'\n", iname);
        return -1;
    }

    // get stuff

    rv = fd->ioctl(fd, IOCTL_IMG4_GET_TYPE, &type);
    if (rv) {
        fprintf(stderr, "[e] cannot identify\n");
        fd->close(fd);
        return -1;
    }
    printf("%c%c%c%c\n", FOURCC(type));

    if (ename) {
        rv = fd->ioctl(fd, IOCTL_LZSS_GET_EXTRA, &buf, &sz);
        if (rv) {
            fprintf(stderr, "[e] cannot get extra\n");
        } else if (!sz) {
            fprintf(stderr, "[w] image has no extra\n");
        } else {
            rv = write_file(ename, buf, sz);
        }
        rc |= rv;
    }
    if (gname) {
        rv = fd->ioctl(fd, IOCTL_IMG4_GET_KEYBAG, &buf, &sz);
        if (rv) {
            fprintf(stderr, "[e] cannot get keybag\n");
        } else if (sz) {
            rv = write_file(gname, buf, sz);
        }
        rc |= rv;
    }
    if (mname) {
        rv = fd->ioctl(fd, IOCTL_IMG4_GET_MANIFEST, &buf, &sz);
        if (rv) {
            fprintf(stderr, "[e] cannot get ticket\n");
        } else {
            rv = write_file(mname, buf, sz);
        }
        rc |= rv;
    }
    if (cinfo) {
        rv = fd->ioctl(fd, IOCTL_IMG4_EVAL_TRUST, cinfo);
        if (rv) {
            fprintf(stderr, "[e] signature failed\n");
        }
        rc |= rv;
    }
    if (get_nonce) {
        uint64_t nonce = 0;
        rv = fd->ioctl(fd, IOCTL_IMG4_GET_NONCE, &nonce);
        if (rv == 0) {
            printf("nonce: %016llx\n", nonce);
        }
    }

    // set stuff

    if (set_type) {
        if (strlen(set_type) != 4) {
            fprintf(stderr, "[e] invalid type '%s'\n", set_type);
            rv = -1;
        } else {
            type = (set_type[0] << 24) | (set_type[1] << 16) | (set_type[2] << 8) | set_type[3];
            rv = fd->ioctl(fd, IOCTL_IMG4_SET_TYPE, type);
            if (rv) {
                fprintf(stderr, "[e] cannot set nonce\n");
            }
        }
        rc |= rv;
    }
    if (set_patch) {
        rv = apply_patch(fd, set_patch, pf, pu);
        if (rv) {
            fprintf(stderr, "[e] cannot apply patch\n");
        }
        rc |= rv;
    }
    if (set_extra) {
        rv = read_file(set_extra, &buf, &sz);
        if (rv == 0) {
            rv = fd->ioctl(fd, IOCTL_LZSS_SET_EXTRA, buf, sz);
            if (rv) {
                fprintf(stderr, "[e] cannot set extra\n");
            }
            free(buf);
        }
        rc |= rv;
    }
    if (set_manifest) {
        rv = read_file(set_manifest, &buf, &sz);
        if (rv == 0) {
            rv = fd->ioctl(fd, IOCTL_IMG4_SET_MANIFEST, buf, sz);
            if (rv) {
                fprintf(stderr, "[e] cannot set manifest\n");
            }
            free(buf);
        }
        rc |= rv;
    }
    if (set_nonce) {
        rv = fd->ioctl(fd, IOCTL_IMG4_SET_NONCE, nonce);
        if (rv) {
            fprintf(stderr, "[e] cannot set nonce %16llx\n", nonce);
        }
        rc |= rv;
    }
    if (set_decrypt) {
        rv = fd->ioctl(fd, IOCTL_ENC_SET_NOENC);
        if (rv) {
            fprintf(stderr, "[e] cannot set noenc\n");
        }
        rc |= rv;
    }

    // close

    if (orig) {
        rv = fd->fsync(fd);
        if (rv) {
            fprintf(stderr, "[e] cannot reassemble data\n");
        } else {
            rv = orig->ioctl(orig, IOCTL_MEM_GET_DATAPTR, &buf, &sz);
            if (rv) {
                fprintf(stderr, "[e] cannot retrieve data\n");
            } else {
                rv = write_file(oname, buf, sz);
            }
        }
        rc |= rv;
    } else if (oname) {
        rv = fd->ioctl(fd, IOCTL_MEM_GET_DATAPTR, &buf, &sz);
        if (rv) {
            fprintf(stderr, "[e] cannot retrieve data\n");
        } else {
            rv = write_file(oname, buf, sz);
        }
        rc |= rv;
    }

    return rc | fd->close(fd);
}