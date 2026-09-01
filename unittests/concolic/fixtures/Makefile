# Rebuild recipes for the verified LowIR concolic fixture matrix.
#
# One dependency-free C function is compiled into every native 64-bit format
# and architecture NeverD supports.  The checked-in images let every CI host
# exercise the complete matrix; `make matrix` reproduces them byte-for-byte.

SRC := concolic_cases.c
OUT := binaries
BUILD := .fixture-build

CLANG ?= clang
MACHO_CLANG ?= $(CLANG)
LLD_LINK ?= lld-link

COMMON_CFLAGS := -O1 -fomit-frame-pointer -fno-stack-protector -fno-builtin \
	-ffreestanding -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-fvisibility=hidden -fno-ident
ELF_CFLAGS := $(COMMON_CFLAGS) -fPIC
MACHO_CFLAGS := $(COMMON_CFLAGS) -fPIC -mmacosx-version-min=11.0
PE_CFLAGS := $(COMMON_CFLAGS)

.PHONY: all matrix clean elf-x64 elf-arm64 macho-x64 macho-arm64 pe-x64 \
	pe-arm64

all: matrix

matrix: elf-x64 elf-arm64 macho-x64 macho-arm64 pe-x64 pe-arm64

$(OUT) $(BUILD):
	mkdir -p $@

$(BUILD)/lowir_concolic_elf_x64.o: $(SRC) | $(BUILD)
	$(CLANG) --target=x86_64-unknown-linux-gnu $(ELF_CFLAGS) -c $< -o $@

elf-x64: $(BUILD)/lowir_concolic_elf_x64.o | $(OUT)
	$(CLANG) --target=x86_64-unknown-linux-gnu -nostdlib -shared \
	  -fuse-ld=lld -Wl,--build-id=none \
	  -Wl,-soname,lowir_concolic_elf_x64 \
	  $< -o $(OUT)/lowir_concolic_elf_x64

$(BUILD)/lowir_concolic_elf_arm64.o: $(SRC) | $(BUILD)
	$(CLANG) --target=aarch64-unknown-linux-gnu $(ELF_CFLAGS) -c $< -o $@

elf-arm64: $(BUILD)/lowir_concolic_elf_arm64.o | $(OUT)
	$(CLANG) --target=aarch64-unknown-linux-gnu -nostdlib -shared \
	  -fuse-ld=lld -Wl,--build-id=none \
	  -Wl,-soname,lowir_concolic_elf_arm64 \
	  $< -o $(OUT)/lowir_concolic_elf_arm64

macho-x64: $(SRC) | $(OUT) $(BUILD)
	$(MACHO_CLANG) --target=x86_64-apple-macos11 $(MACHO_CFLAGS) -c $< \
	  -o $(BUILD)/lowir_concolic_macho_x64.o
	$(MACHO_CLANG) --target=x86_64-apple-macos11 -nostdlib -dynamiclib \
	  -fuse-ld=lld -mmacosx-version-min=11.0 -Wl,-no_uuid \
	  -Wl,-install_name,@rpath/lowir_concolic \
	  -Wl,-exported_symbol,_concolic_branch \
	  $(BUILD)/lowir_concolic_macho_x64.o \
	  -o $(OUT)/lowir_concolic_macho_x64

macho-arm64: $(SRC) | $(OUT) $(BUILD)
	$(MACHO_CLANG) --target=aarch64-apple-macos11 $(MACHO_CFLAGS) -c $< \
	  -o $(BUILD)/lowir_concolic_macho_arm64.o
	$(MACHO_CLANG) --target=aarch64-apple-macos11 -nostdlib -dynamiclib \
	  -fuse-ld=lld -mmacosx-version-min=11.0 -Wl,-no_uuid \
	  -Wl,-install_name,@rpath/lowir_concolic \
	  -Wl,-exported_symbol,_concolic_branch \
	  $(BUILD)/lowir_concolic_macho_arm64.o \
	  -o $(OUT)/lowir_concolic_macho_arm64

pe-x64: $(SRC) | $(OUT) $(BUILD)
	$(CLANG) --target=x86_64-pc-windows-msvc $(PE_CFLAGS) -c $< \
	  -o $(BUILD)/lowir_concolic_pe_x64.obj
	$(LLD_LINK) /nologo /Brepro /timestamp:0 /dll /noentry /nodefaultlib \
	  /noimplib /machine:x64 /export:concolic_branch \
	  /out:$(OUT)/lowir_concolic_pe_x64 \
	  $(BUILD)/lowir_concolic_pe_x64.obj

pe-arm64: $(SRC) | $(OUT) $(BUILD)
	$(CLANG) --target=aarch64-pc-windows-msvc $(PE_CFLAGS) -c $< \
	  -o $(BUILD)/lowir_concolic_pe_arm64.obj
	$(LLD_LINK) /nologo /Brepro /timestamp:0 /dll /noentry /nodefaultlib \
	  /noimplib /machine:arm64 /export:concolic_branch \
	  /out:$(OUT)/lowir_concolic_pe_arm64 \
	  $(BUILD)/lowir_concolic_pe_arm64.obj

clean:
	rm -rf $(BUILD) $(OUT)
