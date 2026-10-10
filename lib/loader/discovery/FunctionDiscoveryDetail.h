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
  std::set<va_t> Entries;
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

} // namespace neverd

#endif // NEVERD_LOADER_DISCOVERY_FUNCTIONDISCOVERYDETAIL_H
