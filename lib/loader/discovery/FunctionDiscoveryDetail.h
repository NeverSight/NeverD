//===- FunctionDiscoveryDetail.h - Per-arch thunk scanners -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal declarations shared between the architecture-generic function
/// discovery dispatcher (FunctionDiscovery.cpp) and the per-target import
/// thunk scanners (FunctionDiscoveryX86.cpp, FunctionDiscoveryAArch64.cpp,
/// FunctionDiscoveryARM.cpp).
///
/// This header is an implementation detail of the loader library and should
/// NOT be included by code outside lib/loader/discovery/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_DISCOVERY_FUNCTIONDISCOVERYDETAIL_H
#define NEVERD_LOADER_DISCOVERY_FUNCTIONDISCOVERYDETAIL_H

#include "neverd/loader/BinaryImage.h"

#include <map>
#include <memory>
#include <set>

namespace neverd {

/// Snapshot established function bodies before heuristic discovery mutates
/// symbols. Import instruction patterns inside a body are not new entries.
/// ImportStubRanges are intentionally absent: an ELF PLT may contain many
/// independently callable veneers.
class ImportThunkCandidates {
public:
  explicit ImportThunkCandidates(const BinaryImage &Img);
  bool allows(va_t Start, uint64_t Size) const;

private:
  std::vector<va_t> Entries;
  std::vector<std::pair<va_t, va_t>> Bodies;
};

/// Scan one executable segment \p Seg for an architecture's import thunk
/// pattern, registering each thunk whose resolved target is in \p Targets as
/// a function symbol in \p Img.  \p Existing tracks already-known symbol
/// addresses (updated in place) to avoid duplicates.  Returns the number of
/// functions added.
///
/// x86/x86-64: jmp [rip+disp32] (PE/IAT) or jmp [abs32].
size_t scanImportThunksX86(BinaryImage &Img, const Segment &Seg,
                           const std::map<va_t, size_t> &Targets,
                           std::set<va_t> &Existing,
                           const ImportThunkCandidates &Candidates);

/// AArch64: three-instruction import veneers and four-instruction ELF PLT
/// veneers that load and branch through x17.
size_t scanImportThunksAArch64(BinaryImage &Img, const Segment &Seg,
                               const std::map<va_t, size_t> &Targets,
                               std::set<va_t> &Existing,
                               const ImportThunkCandidates &Candidates);

/// ARM32: LDR pc,[pc,#-4] veneer followed by the absolute target word.
size_t scanImportThunksARM(BinaryImage &Img, const Segment &Seg,
                           const std::map<va_t, size_t> &Targets,
                           std::set<va_t> &Existing,
                           const ImportThunkCandidates &Candidates);

/// The string literals an x86 image keeps in executable sections, as MSVC
/// does in drivers and the kernel.  Function discovery must not take them for
/// functions: `W` reads as `push rdi` and `H` as a REX prefix, but text holds
/// no return, call or unconditional jump, so execution entered there can only
/// trap.
class X86StringLiteralRuns {
public:
  explicit X86StringLiteralRuns(const BinaryImage &Image);
  ~X86StringLiteralRuns();
  X86StringLiteralRuns(const X86StringLiteralRuns &) = delete;
  X86StringLiteralRuns &operator=(const X86StringLiteralRuns &) = delete;

  /// Whether a string literal begins at \p Offset of \p Seg, text and the
  /// zero that ends it, from which every path execution can take through the
  /// literals, their zeros and the int3 padding between them ends in a trap:
  /// an int3, or an opcode the processor rejects.
  bool at(const Segment &Seg, size_t Offset);

private:
  struct Decoder;
  const BinaryImage &Img;
  std::unique_ptr<Decoder> Dec;
};

} // namespace neverd

#endif // NEVERD_LOADER_DISCOVERY_FUNCTIONDISCOVERYDETAIL_H
