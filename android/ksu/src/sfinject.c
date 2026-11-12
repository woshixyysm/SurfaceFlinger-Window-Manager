// SPDX-License-Identifier: AGPL-3.0-or-later
// sfinject.c - hot-load libsfwmsfhook.so into a running surfaceflinger.
//
// The KernelSU module can only take effect after a reboot, which is far too slow to iterate
// on. This attaches to the live process with ptrace, makes it call dlopen() once, and
// detaches: the library's constructor then starts its socket thread, exactly as it would
// when the module preloads it.
//
// Only dlopen is called and the original registers and stack contents are restored before
// detaching, so surfaceflinger keeps running. If anything goes wrong the process is left in
// its original state and the error is reported; the worst case is that SurfaceFlinger
// restarts itself, which a reboot would have done anyway.
//
// Build: see android/ksu/build.ps1. Run as root on the device:
//     sfinject <pid> <absolute path to .so>
#define _GNU_SOURCE
#include <asm/ptrace.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

// bionic exports dlopen from libdl.so (a weak alias of android_dlopen_ext); libc.so has no
// such symbol. On modern Android libdl.so is a symlink into the runtime APEX, which open()
// follows for us.
#define DLOPEN_LIB "/system/lib64/libdl.so"
#define DLOPEN_MAP "libdl.so"

static unsigned long find_symbol(const char *libpath, const char *name)
{
    const int fd = open(libpath, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    Elf64_Ehdr eh;
    if (pread(fd, &eh, sizeof eh, 0) != (ssize_t)sizeof eh) { close(fd); return 0; }
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64) {
        close(fd);
        return 0;
    }
    Elf64_Shdr *sh = calloc(eh.e_shnum, sizeof(Elf64_Shdr));
    if (!sh) { close(fd); return 0; }
    if (pread(fd, sh, (size_t)eh.e_shnum * sizeof(Elf64_Shdr), (off_t)eh.e_shoff) !=
        (ssize_t)((size_t)eh.e_shnum * sizeof(Elf64_Shdr))) {
        free(sh);
        close(fd);
        return 0;
    }
    unsigned long value = 0;
    for (int i = 0; i < eh.e_shnum && !value; i++) {
        if (sh[i].sh_type != SHT_DYNSYM) continue;
        const Elf64_Shdr *str = &sh[sh[i].sh_link];
        char *strtab = malloc(str->sh_size);
        Elf64_Sym *syms = malloc(sh[i].sh_size);
        if (!strtab || !syms) { free(strtab); free(syms); break; }
        if (pread(fd, strtab, str->sh_size, (off_t)str->sh_offset) == (ssize_t)str->sh_size &&
            pread(fd, syms, sh[i].sh_size, (off_t)sh[i].sh_offset) == (ssize_t)sh[i].sh_size) {
            const size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
            for (size_t k = 0; k < n; k++) {
                if (!syms[k].st_value) continue;
                const char *nm = strtab + syms[k].st_name;
                if (nm < strtab + str->sh_size && strcmp(nm, name) == 0) {
                    value = syms[k].st_value;
                    break;
                }
            }
        }
        free(strtab);
        free(syms);
    }
    free(sh);
    close(fd);
    return value;
}

// Base address of the mapping that backs `libname` in `pid`, from /proc/<pid>/maps.
static unsigned long find_base(pid_t pid, const char *libname)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    unsigned long best = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strstr(line, libname)) continue;
        unsigned long start = strtoul(line, NULL, 16);
        if (best == 0 || start < best) best = start;
    }
    fclose(f);
    return best;
}

static int poke(pid_t pid, unsigned long addr, const void *buf, size_t len)
{
    const size_t words = (len + sizeof(long) - 1) / sizeof(long);
    for (size_t i = 0; i < words; i++) {
        long word = 0;
        const size_t off = i * sizeof(long);
        if (off < len) {
            const size_t n = (len - off) < sizeof(long) ? (len - off) : sizeof(long);
            memcpy(&word, (const char *)buf + off, n);
        }
        if (ptrace(PTRACE_POKEDATA, pid, (void *)(addr + off), (void *)word) < 0) return -1;
    }
    return 0;
}

static int peek(pid_t pid, unsigned long addr, void *buf, size_t len)
{
    for (size_t off = 0; off < len; off += sizeof(long)) {
        errno = 0;
        const long word = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + off), NULL);
        if (errno) return -1;
        const size_t n = (len - off) < sizeof(long) ? (len - off) : sizeof(long);
        memcpy((char *)buf + off, &word, n);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: sfinject <pid> <path to .so>\n");
        return 2;
    }
    const pid_t pid = (pid_t)atoi(argv[1]);
    const char *sopath = argv[2];
    if (pid <= 0 || access(sopath, R_OK) != 0) {
        fprintf(stderr, "bad pid or unreadable %s\n", sopath);
        return 2;
    }

    const unsigned long sym = find_symbol(DLOPEN_LIB, "dlopen");
    if (!sym) { fprintf(stderr, "dlopen not found in %s\n", DLOPEN_LIB); return 3; }
    const unsigned long base = find_base(pid, DLOPEN_MAP);
    if (!base) { fprintf(stderr, "%s not mapped in %d\n", DLOPEN_MAP, pid); return 3; }
    const unsigned long dlopen_addr = base + sym;
    printf("%s base=0x%lx dlopen=0x%lx\n", DLOPEN_MAP, base, dlopen_addr);

    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) < 0) {
        perror("PTRACE_ATTACH");
        return 4;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); ptrace(PTRACE_DETACH, pid, NULL, NULL); return 4; }

    int rc = 0;
    struct user_pt_regs regs, saved;
    struct iovec iov = {&regs, sizeof regs};
    if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) < 0) { perror("GETREGSET"); rc = 5; goto detach; }
    saved = regs;

    // Scratch space below the stack pointer, past the 128-byte red zone.
    const unsigned long scratch = (unsigned long)regs.sp - 512;
    const size_t pathlen = strlen(sopath) + 1;
    unsigned char backup[512];
    if (peek(pid, scratch, backup, sizeof backup) < 0) { perror("peek"); rc = 5; goto detach; }
    if (poke(pid, scratch, sopath, pathlen) < 0) { perror("poke"); rc = 6; goto detach; }

    regs.regs[0] = scratch;            // x0 = path
    regs.regs[1] = 2;                  // x1 = RTLD_NOW
    regs.regs[30] = 0;                 // lr: returning jumps to 0, which traps for us
    regs.pc = dlopen_addr;
    iov.iov_len = sizeof regs;
    if (ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov) < 0) { perror("SETREGSET"); rc = 7; goto restore; }

    if (ptrace(PTRACE_CONT, pid, NULL, NULL) < 0) { perror("CONT"); rc = 8; goto restore; }
    if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); rc = 8; goto restore; }
    if (!WIFSTOPPED(status)) {
        fprintf(stderr, "target did not stop cleanly\n");
        rc = 9;
        goto restore;
    }
    // dlopen returned and jumped to 0: read the value it left in x0.
    iov.iov_len = sizeof regs;
    if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) == 0)
        printf("dlopen returned %p\n", (void *)regs.regs[0]);

restore:
    // Put everything back exactly as it was, including the stack we borrowed.
    poke(pid, scratch, backup, sizeof backup);
    iov.iov_len = sizeof saved;
    ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov);

detach:
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    if (rc == 0) printf("injected %s into %d\n", sopath, pid);
    return rc;
}
