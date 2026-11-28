# Rebuild recipes for the separate-debug-file fixtures.
#
# companion.so keeps only a build ID and a .gnu_debuglink naming
# companion.so.debug, the file objcopy --only-keep-debug split its DWARF
# into.  companion-nobuildid.so links without a build ID, so only its debug
# link's CRC identifies the same companion.debug, which it names.
# companion-zlib.debug is companion.so.debug with its sections compressed, as
# distribution debug packages ship them.
# Build the Mach-O image and its adjacent dSYM with `make macho-no-uuid`.

CLANG ?= clang
OBJCOPY ?= objcopy
LD64_LLD ?= ld64.lld
DSYMUTIL ?= dsymutil
STRIP ?= strip
MACHO_IMAGE := companion-no-uuid.dylib
MACHO_DWARF := $(MACHO_IMAGE).dSYM/Contents/Resources/DWARF
CFLAGS := --target=x86_64-unknown-linux-gnu -O1 -g -fPIC -nostdlib -shared \
	-fuse-ld=lld -fdebug-compilation-dir=. -fdebug-prefix-map=$(CURDIR)=.

.PHONY: all clean macho-no-uuid

all: companion.so companion.so.debug companion-nobuildid.so companion.debug \
	companion-zlib.debug

companion.full: companion.c
	$(CLANG) $(CFLAGS) -Wl,--build-id=sha1 $< -o $@

companion.so.debug: companion.full
	$(OBJCOPY) --only-keep-debug $< $@

companion.so: companion.full companion.so.debug
	$(OBJCOPY) --strip-debug --add-gnu-debuglink=companion.so.debug $< $@

companion-nobuildid.full: companion.c
	$(CLANG) $(CFLAGS) -Wl,--build-id=none $< -o $@

companion.debug: companion-nobuildid.full
	$(OBJCOPY) --only-keep-debug $< $@

companion-nobuildid.so: companion-nobuildid.full companion.debug
	$(OBJCOPY) --strip-debug --add-gnu-debuglink=companion.debug $< $@

companion-zlib.debug: companion.so.debug
	$(OBJCOPY) --compress-debug-sections=zlib $< $@

macho-no-uuid: companion.c
	$(CLANG) --target=x86_64-apple-macos11 -g -O1 -fdebug-compilation-dir=. \
	  -c $< -o companion-macho.o
	$(LD64_LLD) -arch x86_64 -dylib -platform_version macos 11.0 11.0 \
	  -install_name @rpath/$(MACHO_IMAGE) -no_uuid -oso_prefix $(CURDIR)/ \
	  companion-macho.o -o $(MACHO_IMAGE)
	mkdir -p $(MACHO_DWARF)
	$(DSYMUTIL) --flat $(MACHO_IMAGE) -o $(MACHO_DWARF)/$(MACHO_IMAGE)
	$(STRIP) -S $(MACHO_IMAGE)

clean:
	rm -f companion.full companion-nobuildid.full companion-macho.o
