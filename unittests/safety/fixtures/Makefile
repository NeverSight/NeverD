# Rebuild recipes for the memory-safety fixtures.
#
# The analysis is format-neutral: the same source produces an equivalent
# binary for every target.  The checked-in outputs keep the full matrix
# available on every CI host; these recipes reproduce each file.
#
# The copies are intentionally unsafe.  The program is only ever analysed, so
# nothing here is executed.

CFLAGS := -O0 -g -fno-stack-protector -fno-builtin -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
REPRO_DEBUG := -fdebug-compilation-dir=. -fcoverage-compilation-dir=. \
	-fdebug-prefix-map=$(CURDIR)=.
PE_CFLAGS := -O0 -gcodeview -gno-codeview-command-line \
	-fno-stack-protector -fno-builtin $(REPRO_DEBUG)

SRC := safety_cases.c
DEF := safety_runtime.def
OUT := binaries
BUILD := .fixture-build

CLANG ?= clang
DARWIN_CLANG ?= /usr/bin/clang
DSYMUTIL ?= dsymutil
LLVM_DLLTOOL ?= llvm-dlltool
LLD_LINK ?= lld-link

.PHONY: all matrix clean clean-matrix native elf-x64 elf-arm64 macho-x64 \
	macho-arm64 pe-x64 pe-arm64

all: native

matrix: elf-x64 elf-arm64 macho-x64 macho-arm64 pe-x64 pe-arm64

$(OUT) $(BUILD):
	mkdir -p $@

# Host-native binary (Mach-O on macOS, ELF on Linux, PE on Windows).
native: $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o safety_cases_native

# Every recipe also writes a linker MAP beside the image.  A MAP is the only
# identity a stripped build keeps, so the matrix carries one per target to pin
# what the analysis may still say when there is no PDB and no DWARF.

# ELF (Linux, or a cross clang with a sysroot).
$(BUILD)/safety_cases_elf_x64.o: $(SRC) | $(BUILD)
	$(CLANG) --target=x86_64-unknown-linux-gnu $(CFLAGS) -fPIC \
	  $(REPRO_DEBUG) -c $(SRC) -o $@

elf-x64: $(OUT)/safety_cases_elf_x64
$(OUT)/safety_cases_elf_x64: $(BUILD)/safety_cases_elf_x64.o | $(OUT)
	$(CLANG) --target=x86_64-unknown-linux-gnu \
	  -nostdlib -shared -fuse-ld=lld -Wl,--unresolved-symbols=ignore-all \
	  -Wl,-Map,$@.map \
	  $< -o $@

$(BUILD)/safety_cases_elf_arm64.o: $(SRC) | $(BUILD)
	$(CLANG) --target=aarch64-unknown-linux-gnu $(CFLAGS) -fPIC \
	  $(REPRO_DEBUG) -c $(SRC) -o $@

elf-arm64: $(OUT)/safety_cases_elf_arm64
$(OUT)/safety_cases_elf_arm64: $(BUILD)/safety_cases_elf_arm64.o | $(OUT)
	$(CLANG) --target=aarch64-unknown-linux-gnu \
	  -nostdlib -shared -fuse-ld=lld -Wl,--unresolved-symbols=ignore-all \
	  -Wl,-Map,$@.map \
	  $< -o $@

# Mach-O (macOS host).  Each phony target rebuilds the executable and its
# companion together so a missing or stale dSYM cannot survive unnoticed.
macho-x64: $(SRC) | $(OUT) $(BUILD)
	$(DARWIN_CLANG) -arch x86_64 $(CFLAGS) $(REPRO_DEBUG) -c $(SRC) \
	  -o $(BUILD)/safety_cases_macho_x64.o
	$(DARWIN_CLANG) -arch x86_64 $(BUILD)/safety_cases_macho_x64.o \
	  -Wl,-map,$(OUT)/safety_cases_macho_x64.map \
	  -o $(OUT)/safety_cases_macho_x64
	$(DSYMUTIL) $(OUT)/safety_cases_macho_x64 \
	  -o $(OUT)/safety_cases_macho_x64.dSYM

macho-arm64: $(SRC) | $(OUT) $(BUILD)
	$(DARWIN_CLANG) -arch arm64 $(CFLAGS) $(REPRO_DEBUG) -c $(SRC) \
	  -o $(BUILD)/safety_cases_macho_arm64.o
	$(DARWIN_CLANG) -arch arm64 $(BUILD)/safety_cases_macho_arm64.o \
	  -Wl,-map,$(OUT)/safety_cases_macho_arm64.map \
	  -o $(OUT)/safety_cases_macho_arm64
	$(DSYMUTIL) $(OUT)/safety_cases_macho_arm64 \
	  -o $(OUT)/safety_cases_macho_arm64.dSYM

# PE/COFF.  A small import library keeps the image limited to the fixture.
$(BUILD)/safety_runtime_x64.lib: $(DEF) | $(BUILD)
	$(LLVM_DLLTOOL) -m i386:x86-64 -d $(DEF) -l $@

$(BUILD)/safety_runtime_arm64.lib: $(DEF) | $(BUILD)
	$(LLVM_DLLTOOL) -m arm64 -d $(DEF) -l $@

pe-x64: $(SRC) $(BUILD)/safety_runtime_x64.lib | $(OUT)
	$(CLANG) --target=x86_64-pc-windows-msvc $(PE_CFLAGS) \
	  -c $(SRC) -o $(BUILD)/safety_cases_pe_x64.obj
	$(LLD_LINK) /nologo /Brepro /entry:main /subsystem:console /nodefaultlib \
	  /pdbsourcepath:. \
	  /debug:full /pdb:$(OUT)/safety_cases_pe_x64.pdb \
	  /pdbaltpath:safety_cases_pe_x64.pdb \
	  /lldmap:$(OUT)/safety_cases_pe_x64.exe.map \
	  /out:$(OUT)/safety_cases_pe_x64.exe \
	  $(BUILD)/safety_cases_pe_x64.obj $(BUILD)/safety_runtime_x64.lib

pe-arm64: $(SRC) $(BUILD)/safety_runtime_arm64.lib | $(OUT)
	$(CLANG) --target=aarch64-pc-windows-msvc $(PE_CFLAGS) \
	  -c $(SRC) -o $(BUILD)/safety_cases_pe_arm64.obj
	$(LLD_LINK) /nologo /Brepro /machine:arm64 /entry:main /subsystem:console \
	  /nodefaultlib /pdbsourcepath:. /debug:full \
	  /pdb:$(OUT)/safety_cases_pe_arm64.pdb \
	  /pdbaltpath:safety_cases_pe_arm64.pdb \
	  /lldmap:$(OUT)/safety_cases_pe_arm64.exe.map \
	  /out:$(OUT)/safety_cases_pe_arm64.exe \
	  $(BUILD)/safety_cases_pe_arm64.obj $(BUILD)/safety_runtime_arm64.lib

clean:
	rm -rf $(BUILD) safety_cases_native

clean-matrix: clean
	rm -rf $(OUT)
