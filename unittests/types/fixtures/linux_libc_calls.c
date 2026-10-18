// A Linux program calling routines the GNU C Library exports, which
// NeverD's linux-* libraries declare the prototypes of: their calls pass
// what the prototypes take, typed as glibc declares them, and strtol, which
// glibc 2.38 renames __isoc23_strtol for _GNU_SOURCE, takes its prototype
// under the name the program imports.  Built against glibc 2.43 with clang
// and lld, the ARM ones without GCC's runtime files:
//   clang -O1 -fno-stack-protector -fuse-ld=lld \
//     -o linux_libc_calls_x64 linux_libc_calls.c
//   clang -m32 -O1 -fno-stack-protector -fuse-ld=lld \
//     -o linux_libc_calls_x86 linux_libc_calls.c
//   L=/usr/aarch64-linux-gnu/lib
//   clang --target=aarch64-linux-gnu --sysroot=/usr/aarch64-linux-gnu -O1 \
//     -fno-stack-protector -fuse-ld=lld -nostartfiles -nodefaultlibs \
//     -Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1 $L/crt1.o $L/crti.o \
//     -o linux_libc_calls_arm64 linux_libc_calls.c $L/libc.so.6 \
//     $L/libc_nonshared.a $L/crtn.o
// and for linux_libc_calls_arm the same with --target=arm-linux-gnueabihf,
// the libc6-dev-armhf-cross and libc6-armhf-cross packages' sysroot and
// -Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3.
#define _GNU_SOURCE
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

__attribute__((noinline)) long Size(const char *Path) {
  struct stat Info;
  if (stat(Path, &Info) != 0)
    return -1;
  return Info.st_size;
}

__attribute__((noinline)) long Read(int File, char *Buffer) {
  return read(File, Buffer, 64);
}

__attribute__((noinline)) long Number(const char *Text) {
  return strtol(Text, 0, 16);
}

int main(int Count, char **Arguments) {
  char Buffer[64];
  return (int)(Size(Arguments[0]) + Read(Count, Buffer) + Number(Arguments[0]));
}
