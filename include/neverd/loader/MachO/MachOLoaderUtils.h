//===- MachOLoaderUtils.h - Mach-O loader helpers -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Mach-O-specific loader utilities: entry-point extraction from
/// LC_MAIN / LC_UNIXTHREAD, dyld bind opcode parsing, and export trie
/// walking.  Layouts follow llvm/BinaryFormat/MachO.h.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_MACHO_MACHOLOADERUTILS_H
#define NEVERD_LOADER_MACHO_MACHOLOADERUTILS_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Object/MachO.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <filesystem>
#include <memory>

namespace neverd {
namespace macho_loader {

struct SectionInfo {
  std::string Name;
  std::string SegName;
  uint64_t Addr = 0;
  uint64_t Size = 0;
  uint32_t Reserved1 = 0;
  uint32_t Flags = 0;
  uint32_t StubSize = 0;
};

/// Offsets from LC_DYLD_INFO / LC_DYLD_INFO_ONLY
/// (llvm::MachO::dyld_info_command).
struct DyldInfoOffsets {
  uint32_t RebaseOff = 0;
  uint32_t RebaseSize = 0;
  uint32_t BindOff = 0;
  uint32_t BindSize = 0;
  uint32_t LazyBindOff = 0;
  uint32_t LazyBindSize = 0;
  uint32_t ExportOff = 0;
  uint32_t ExportSize = 0;
};

/// File offsets for LC_FUNCTION_STARTS (llvm::MachO::linkedit_data_command).
struct FunctionStartsInfo {
  uint32_t DataOff = 0;
  uint32_t DataSize = 0;
};

/// Recover the VA the Mach header is mapped at, which is what every
/// image-relative Mach-O offset (LC_MAIN entryoff, LC_FUNCTION_STARTS deltas,
/// `__unwind_info` function offsets, chained-fixup targets) is measured from.
/// This is *not* `Img.Base`: an executable's lowest segment is `__PAGEZERO` at
/// VA 0, which maps no file bytes.  The header lives in whichever segment maps
/// file offset 0 with a non-empty file range -- conventionally `__TEXT`, though
/// a packer may rename it, so the name is a preference and not a requirement.
va_t getMachHeaderVA(const BinaryImage &Img);

/// Collect dyld rebase/bind/export regions from load commands.
void parseDyldInfoLoadCommands(const llvm::object::MachOObjectFile &Obj,
                               DyldInfoOffsets &Out);

/// Collect LC_LOAD_DYLIB and related dependency paths.
void parseNeededLibraries(const llvm::object::MachOObjectFile &Obj,
                          BinaryImage &Img);

/// Decode LC_FUNCTION_STARTS ULEB128 deltas into function symbols.
void parseFunctionStarts(const uint8_t *BasePtr, size_t FileSize,
                         const FunctionStartsInfo &Info, uint64_t TextVMAddr,
                         BinaryImage &Img);

/// Open a thin or universal Mach-O executable and return the buffer plus a
/// MachOObjectFile for the host architecture (cf. llvm-objdump -arch).
llvm::Expected<std::pair<std::unique_ptr<llvm::MemoryBuffer>,
                         std::unique_ptr<llvm::object::MachOObjectFile>>>
openMachOFile(const std::filesystem::path &Path);

/// Resolve the image entry from LC_MAIN, LC_UNIXTHREAD, or LC_THREAD.  LC_MAIN
/// names the ordinary main function; thread-state entries are recorded as
/// loader/runtime functions.
void parseEntryPoint(const llvm::object::MachOObjectFile &Obj, BinaryImage &Img,
                     uint64_t TextVMAddr);

/// Record LC_ROUTINES / LC_ROUTINES_64 initialization entry points.
void parseRuntimeLoadCommands(const llvm::object::MachOObjectFile &Obj,
                              BinaryImage &Img);

/// Record loader-invoked functions from Mach-O initializer, terminator,
/// thread-local initializer, and compact initializer-offset sections.
void parseRuntimeFunctionSections(const std::vector<SectionInfo> &Sections,
                                  uint64_t TextVMAddr, BinaryImage &Img);

/// Walk LC_DYLD_INFO bind / lazy_bind bytecode, enrich \p Img imports, and
/// retain each pointer slot's symbol/addend in BinaryImage::DyldBindSlots.
void parseBindStreams(const uint8_t *BasePtr, size_t FileSize,
                      const DyldInfoOffsets &DyldInfo, BinaryImage &Img);

/// Walk LC_DYLD_INFO classic rebase bytecode and retain absolute pointer-slot
/// provenance for code and mapped data targets.
void parseRebaseStream(const uint8_t *BasePtr, size_t FileSize,
                       const DyldInfoOffsets &DyldInfo, BinaryImage &Img);

/// Parse the export trie from DyldInfo offsets and append to \p Img exports.
void parseExportTrie(const uint8_t *BasePtr, size_t FileSize,
                     const DyldInfoOffsets &DyldInfo, uint64_t TextVMAddr,
                     BinaryImage &Img);

/// Parse __stubs sections via the indirect symbol table and populate
/// Img.Imports and Img.Symbols with stub-to-import mappings.
void parseStubImports(const llvm::object::MachOObjectFile &Obj,
                      const std::vector<SectionInfo> &Sections,
                      const uint8_t *BasePtr, size_t FileSize, bool Is64,
                      BinaryImage &Img);

/// Map non-lazy / lazy pointer-table slots (__got, __la_symbol_ptr) to their
/// imported symbol via the indirect symbol table and record them in
/// Img.ImportPtrSlots.  Unlike __stubs these are pointer tables (slot size =
/// pointer size), and a GOT-indirect call to a slot has no call-site
/// relocation, so this is the only way to name routines invoked that way (e.g.
/// Darwin's ____chkstk_darwin stack probe).
void parseNonLazyPtrImports(const llvm::object::MachOObjectFile &Obj,
                            const std::vector<SectionInfo> &Sections,
                            const uint8_t *BasePtr, size_t FileSize, bool Is64,
                            BinaryImage &Img);

/// Reconstruct the complete non-lazy/lazy import pointer-slot map directly
/// from one thin Mach-O byte image.  This is the authoritative provenance
/// check for consumers that were handed a cached BinaryImage: a structurally
/// similar section table cannot substitute a different slot-to-symbol map.
llvm::Expected<std::map<va_t, std::string>>
parseImportPtrSlots(llvm::ArrayRef<uint8_t> Binary);

/// File offsets for LC_DYLD_CHAINED_FIXUPS
/// (llvm::MachO::linkedit_data_command).
struct ChainedFixupsInfo {
  uint32_t DataOff = 0;
  uint32_t DataSize = 0;
};

/// Parse LC_DYLD_CHAINED_FIXUPS to extract import names from its ordinal table.
/// The chain walker below joins concrete bind slots/addends to these names.
void parseChainedFixupsImports(const uint8_t *BasePtr, size_t FileSize,
                               const ChainedFixupsInfo &Info, BinaryImage &Img);

/// Walk the LC_DYLD_CHAINED_FIXUPS rebase chains (64-bit pointer formats) and
/// record each rebase slot that holds an absolute pointer into code in
/// Img.CodePtrRelocSlots (and into mapped data in Img.DataPtrRelocSlots),
/// plus each bind slot/name/addend in Img.DyldBindSlots.  Exact Mach-O section
/// instruction attributes distinguish code from data inside coarse RX __TEXT.
/// This is the chained-fixups analogue of the classic
/// R_*_64/ABS64 relocation recording the ELF loader does: a run of such slots
/// starting at a table base is the signature of a computed-goto / threaded-
/// dispatch jump table (no comparison guard bounds it), letting the jump-table
/// resolver recover it and rebuild it as relocatable control flow.  \p
/// TextVMAddr is the __TEXT segment vmaddr (the load-time image base for the
/// chains).
void parseChainedFixupsRebases(const uint8_t *BasePtr, size_t FileSize,
                               const ChainedFixupsInfo &Info, va_t TextVMAddr,
                               BinaryImage &Img);

/// Parse LC_UUID and store the hex-encoded UUID in Img.DynInfo.UUID.
void parseUUID(const llvm::object::MachOObjectFile &Obj, BinaryImage &Img);

/// Parse LC_BUILD_VERSION and store the min OS version string in
/// Img.DynInfo.MinOSVersion.
void parseBuildVersion(const llvm::object::MachOObjectFile &Obj,
                       BinaryImage &Img);

/// Apply architecture-specific relocations to mapped MH_OBJECT sections.
llvm::Error applyObjectRelocations(const llvm::object::MachOObjectFile &Obj,
                                   BinaryImage &Img);

} // namespace macho_loader
} // namespace neverd

#endif // NEVERD_LOADER_MACHO_MACHOLOADERUTILS_H
