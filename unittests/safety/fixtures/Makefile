# Rebuild recipes for the memory-safety fixtures.
#
# The analysis is format-neutral: the same source produces an equivalent
# binary for every target, and the audit and hunt verdicts must match across
# them.  A binary is built where its toolchain exists; the unit test builds the
# host-native binary automatically, and these recipes reproduce the others.
#
# The copies are intentionally unsafe.  The program is only ever analysed, so
# nothing here is executed.

CFLAGS := -O0 -g -fno-stack-protector -fno-builtin -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0

SRC := safety_cases.c

.PHONY: all clean native elf-x64 elf-arm64 macho-x64 macho-arm64 pe-x64

all: native

# Host-native binary (Mach-O on macOS, ELF on Linux, PE on Windows).
native: $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o safety_cases_native

# ELF (Linux, or a cross clang with a sysroot).
elf-x64: $(SRC)
	clang --target=x86_64-linux-gnu $(CFLAGS) $(SRC) -o safety_cases_elf_x64
elf-arm64: $(SRC)
	clang --target=aarch64-linux-gnu $(CFLAGS) $(SRC) -o safety_cases_elf_arm64

# Mach-O (macOS host).
macho-x64: $(SRC)
	clang -arch x86_64 $(CFLAGS) $(SRC) -o safety_cases_macho_x64
macho-arm64: $(SRC)
	clang -arch arm64 $(CFLAGS) $(SRC) -o safety_cases_macho_arm64

# PE/COFF (mingw-w64 or clang targeting Windows).
pe-x64: $(SRC)
	x86_64-w64-mingw32-gcc $(CFLAGS) $(SRC) -o safety_cases_pe_x64.exe

clean:
	rm -f safety_cases_native safety_cases_elf_* safety_cases_macho_* \
	      safety_cases_pe_*
