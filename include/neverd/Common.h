//===- Common.h - Common types and constants -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file defines fundamental types, enumerations, and data structures
/// shared across the entire NeverD decompiler framework.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_COMMON_H
#define NEVERD_COMMON_H

#include "llvm/ADT/StringExtras.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace neverd {

inline constexpr const char *ProjectName = "NeverD";
inline constexpr const char *VersionString = "3389.0.1";
inline constexpr const char *ModuleName = "neverd_output";

using va_t = uint64_t;
constexpr va_t InvalidVA = ~va_t(0);

/// Default prefix for auto-generated function symbols (e.g. "sub_1234").
inline constexpr llvm::StringLiteral kAutoFuncPrefix("sub_");

/// Names of the synthetic frame-setup block and the initial stack-pointer value
/// the LLVM emitter materializes for a lifted function.  The optimizer keys
/// frame-pointer recovery (fixLiftedStackPointers) off these names, so the
/// emitter (producer) and the pipeline (consumer) must agree on the spelling.
inline constexpr llvm::StringLiteral kFrameSetupBlock("frame_setup");
inline constexpr llvm::StringLiteral kRspInitValue("rsp_init");

/// Prefix for auto-generated TLS callback symbols.
inline constexpr llvm::StringLiteral kTLSCallbackPrefix("TlsCallback_");

/// Prefix for ordinal-only import symbols.
inline constexpr llvm::StringLiteral kOrdinalPrefix("ord_");

/// Default module name for imports without a library name.
inline constexpr llvm::StringLiteral kExternModule("extern");

/// Prefix for unnamed ELF PT_LOAD segments.
inline constexpr llvm::StringLiteral kLoadSegmentPrefix("LOAD_");

/// Suffix appended to delay-load DLL names.
inline constexpr llvm::StringLiteral kDelayImportSuffix(" [delay]");

/// Prefix for ELF build-id stored in DynInfo.PDBPath.
inline constexpr llvm::StringLiteral kBuildIdPrefix("build-id:");

/// Prefix for synthesized global data symbols that encode original VA.
/// Used to produce relocatable references instead of absolute addresses.
inline constexpr llvm::StringLiteral kNdDataPrefix("__nd_data_");

/// Default synthetic code section appended by patch-mode rewriters (ELF/COFF).
inline constexpr llvm::StringLiteral kNdTextSection(".ndtext");
/// Mach-O spelling of \c kNdTextSection.
inline constexpr llvm::StringLiteral kNdTextSectionMachO("__ndtext");

/// Prefix for synthesized function-pointer dispatch tables recovered from a
/// `.data.rel.ro` code-pointer array (callback table / vtable / threaded
/// dispatch).  Each entry is `ptrtoint @func`, so the recompiled object's
/// indirect call reaches the recompiled function rather than the stale
/// original VA (which points nowhere after relinking).
inline constexpr llvm::StringLiteral kNdCodePtrPrefix("__nd_codeptr_");

/// Build the canonical `__nd_data_<hex VA>` symbol name that encodes an
/// absolute in-image address as a relocatable (symbol) reference. The rewrite
/// backend's address-model `resolve` callback maps it back to \p VA, letting
/// codegen emit PC-relative addressing instead of an ASLR-fragile absolute
/// constant.
inline std::string makeNdDataSymbol(va_t VA) {
  return (kNdDataPrefix + llvm::utohexstr(VA)).str();
}

/// Inverse of makeNdDataSymbol(): parse a `__nd_data_<hex>` symbol back to its
/// VA. Tolerates the Mach-O global-symbol underscore mangling (`_` + prefix)
/// and a trailing section suffix (`.data` / `.bss` / `.rodata`).
inline std::optional<va_t> parseNdDataSymbol(llvm::StringRef Name) {
  if (!Name.consume_front(kNdDataPrefix)) {
    if (!(Name.consume_front("_") && Name.consume_front(kNdDataPrefix)))
      return std::nullopt;
  }
  Name = Name.take_front(Name.find('.'));
  va_t VA = 0;
  if (Name.getAsInteger(16, VA))
    return std::nullopt;
  return VA;
}

/// Parse a `__nd_codeptr_<hex>` symbol back to its run-start VA. Tolerates the
/// Mach-O global-symbol underscore mangling (`_` + prefix). The encoded VA is
/// the start of the original (preserved) read-only-after-relocation run that
/// the code-pointer table mirror covers. In a real patch the original run
/// survives at this VA with its loader relocations intact, so a patch-time
/// externalized reference to it slides correctly under ASLR (its code-pointer
/// slots resolve to original function VAs, which carry trampolines into the
/// rewritten code).
inline std::optional<va_t> parseNdCodePtrSymbol(llvm::StringRef Name) {
  if (!Name.consume_front(kNdCodePtrPrefix)) {
    if (!(Name.consume_front("_") && Name.consume_front(kNdCodePtrPrefix)))
      return std::nullopt;
  }
  Name = Name.take_front(Name.find('.'));
  va_t VA = 0;
  if (Name.getAsInteger(16, VA))
    return std::nullopt;
  return VA;
}

/// Strip the leading underscore(s) a platform prepends to C symbol names
/// (Mach-O adds one `_`; runtime/intrinsic names such as `____chkstk_darwin`
/// carry several). Yields the bare C name the libc registries expect
/// (isKnownFunction / headerFor / varArgFixedCount / libcArity all take the
/// name with its leading underscores already removed). No-op on ELF/COFF.
inline llvm::StringRef stripLeadingUnderscores(llvm::StringRef Name) {
  while (Name.starts_with("_"))
    Name = Name.drop_front(1);
  return Name;
}

/// True if \p Name is the Apple/Darwin prologue stack-probe routine
/// (`____chkstk_darwin` and the like). The rewrite backend elides this
/// GOT-indirect probe call: it is invoked with its allocation size in a fixed
/// register the lifter does not model, so re-emitting it would walk off the
/// stack and fault, and the dynamic allocation it guards is independently
/// lowered to a real alloca.
inline bool isDarwinStackProbeName(llvm::StringRef Name) {
  return stripLeadingUnderscores(Name).contains("chkstk");
}

/// Four ISA targets: 64-bit (X64, AArch64) and 32-bit (X86, ARM).
/// Backend support (lift/codegen/patch): neverd/ir/arch_support.h.
enum class Arch : uint8_t { X64, AArch64, X86, ARM, Unknown };

enum class InstructionMode : uint8_t { Default, ARM, Thumb };

inline const char *getArchName(Arch A) {
  switch (A) {
  case Arch::X64:
    return "x86_64";
  case Arch::AArch64:
    return "aarch64";
  case Arch::X86:
    return "x86";
  case Arch::ARM:
    return "arm";
  default:
    return "unknown";
  }
}

enum class BinaryFormat : uint8_t { ELF, COFF, MachO, Unknown };

enum class Bitness : uint8_t { Bits32, Bits64, Unknown };

inline const char *getBitnessName(Bitness B) {
  switch (B) {
  case Bitness::Bits32:
    return "32";
  case Bitness::Bits64:
    return "64";
  default:
    return "unknown";
  }
}

inline bool is64Bit(Bitness B) { return B == Bitness::Bits64; }
inline bool is32Bit(Bitness B) { return B == Bitness::Bits32; }

enum class SegmentFlags : uint32_t {
  None = 0,
  Readable = 1 << 0,
  Writable = 1 << 1,
  Executable = 1 << 2,
};

inline SegmentFlags operator|(SegmentFlags A, SegmentFlags B) {
  return static_cast<SegmentFlags>(static_cast<uint32_t>(A) |
                                   static_cast<uint32_t>(B));
}
inline SegmentFlags operator&(SegmentFlags A, SegmentFlags B) {
  return static_cast<SegmentFlags>(static_cast<uint32_t>(A) &
                                   static_cast<uint32_t>(B));
}
inline bool hasFlag(SegmentFlags Flags, SegmentFlags Test) {
  return (static_cast<uint32_t>(Flags) & static_cast<uint32_t>(Test)) != 0;
}

struct Segment {
  std::string Name;
  va_t VA = 0;
  uint64_t Size = 0;
  uint64_t FileOff = 0;
  uint64_t FileSz = 0;
  SegmentFlags Flags = SegmentFlags::None;
  std::vector<uint8_t> Data;

  bool isExecutable() const { return hasFlag(Flags, SegmentFlags::Executable); }
  bool isWritable() const { return hasFlag(Flags, SegmentFlags::Writable); }
  bool isReadable() const { return hasFlag(Flags, SegmentFlags::Readable); }
  bool contains(va_t Addr) const {
    return Addr >= VA && Addr - VA < Size;
  }
};

struct Import {
  std::string Module;
  std::string Name;
  uint16_t Ordinal = 0;
  va_t IATAddr = 0;
};

struct Export {
  std::string Name;
  uint32_t Ordinal = 0;
  va_t Addr = 0;
};

struct Symbol {
  std::string Name;
  va_t Addr = 0;
  uint64_t Size = 0;
  bool IsFunc = false;

  static Symbol makeFunc(va_t Addr, uint64_t Size = 0) {
    Symbol S;
    S.Addr = Addr;
    S.Size = Size;
    S.IsFunc = true;
    S.Name = (kAutoFuncPrefix + llvm::utohexstr(Addr)).str();
    return S;
  }
};

/// Register offset constants for the unified register space.
/// Used across IR passes to identify frame-related registers.
namespace reg {
/// x86-64
constexpr uint64_t RBX = 24;
constexpr uint64_t RSP = 32;
constexpr uint64_t RBP = 40;
constexpr uint64_t R12 = 96;
constexpr uint64_t R13 = 104;
constexpr uint64_t R14 = 112;
constexpr uint64_t R15 = 120;
/// AArch64
constexpr uint64_t X28_A64 = 224;
constexpr uint64_t X29_A64 = 232;
constexpr uint64_t LR_A64 = 240;
constexpr uint64_t SP_A64 = 248;
/// ARM 32-bit
constexpr uint64_t R4_ARM = 16;
constexpr uint64_t R11_ARM = 44;
constexpr uint64_t SP_ARM = 52;
constexpr uint64_t LR_ARM = 56;
} // namespace reg

} // namespace neverd

#endif // NEVERD_COMMON_H
