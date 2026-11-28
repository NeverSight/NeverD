//===- X86SgxDynamicMemory.cpp - SGX2 enclave page operations -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Serialized EACCEPT, EMODPE and EACCEPTCOPY on an imported EPCM.
//===----------------------------------------------------------------------===//

#include "X86ModeState.h"
#include "X86SgxExecution.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>

namespace neverd::analysis::detail {
using namespace translate;
namespace {
namespace le = llvm::support::endian;
enum class Leaf : uint32_t { Accept = 5, Extend = 6, Copy = 7 };
constexpr uint64_t PageSize = 4096, SecInfoSize = 64;
constexpr uint64_t RWX = 7, Pending = 8, Modified = 16, Restricted = 32;
constexpr uint8_t TCS = 1, Regular = 2, Trimmed = 4;
constexpr unsigned NotTracked = 11, AttributeMismatch = 19;

bool allowsOperand(const X86SgxEnclave &E, uint64_t Address, uint64_t Size) {
  return E.contains(Address) && !(Address & (Size - 1));
}

bool ownsPage(const X86SgxPage &Page, const X86SgxEnclave &E) {
  return Page.Valid && !Page.Blocked && Page.OwnerSECS == E.SECS;
}

bool regularPage(const X86SgxPage &Page, const X86SgxEnclave &E,
                 uint64_t Address) {
  return ownsPage(Page, E) && !Page.Pending && !Page.Modified &&
         Page.Type == X86SgxPageType::Regular &&
         Page.Linear == llvm::alignDown(Address, PageSize);
}

bool acceptsRegularPages(uint8_t Type, uint64_t Flags) {
  return Type == Regular && (Flags & (Pending | Restricted)) &&
         !(Flags & Modified);
}

bool acceptsModifiedPages(uint8_t Type, uint64_t Flags) {
  return (Type == TCS || Type == Trimmed) && (Flags & Modified) &&
         !(Flags & (Pending | Restricted));
}

bool validNewTCS(const X86SgxPage &P, const X86SgxFeatures &Features) {
  const auto *B = P.Bytes.data();
  // STATE, DBGOPTIN and AEP must be zero. OCETSSA and PREVSSP are
  // reserved by this processor's non-CET enclave profile.
  return !le::read64le(B) &&
         !(le::read64le(B + 8) & ~(Features.AEXNotify ? 2ULL : 0ULL)) &&
         le::read32le(B + 24) < le::read32le(B + 28) && !le::read64le(B + 40) &&
         std::all_of(B + 72, B + PageSize, [](uint8_t V) { return V == 0; });
}
} // namespace

llvm::Expected<X86ModeExecutionResult> X86SgxExecution::dynamicMemory(
    X86ModeState &S, X86PagedMemory &M, X86SgxInstructionProgress &Progress,
    unsigned InstructionSize, X86ModeExecutionResult Out) {
  auto &EPC = *M.sgxMemory();
  auto &R = modeRegisters(S);
  const Leaf Op = static_cast<Leaf>(uint32_t(R.GPR[0]));
  const uint64_t Info = R.GPR[3], Dest = R.GPR[1], Source = R.GPR[2];
  const bool Copy = Op == Leaf::Copy;
  const auto DestAccess =
      Copy ? MemoryAccessKind::Write : MemoryAccessKind::Read;
  const auto GP = [&] {
    Out.Exception = X86MemoryException{13, 0, {}};
    return Out;
  };
  const auto PF = [&](uint64_t Address, MemoryAccessKind Kind) {
    Out.Exception = x86PageFault(0x8001, M.controls(), {Address, Kind, 3});
  };
  if (!EPC.Active || !allowsOperand(*EPC.Active, Info, SecInfoSize))
    return GP();
  const auto &E = *EPC.Active;
  // EACCEPT consumes SECINFO before checking RCX. The other two leaves
  // resolve every linear operand to an EPC page before reading SECINFO.
  if (Op != Leaf::Accept && (!allowsOperand(E, Dest, PageSize) ||
                             (Copy && !allowsOperand(E, Source, PageSize))))
    return GP();
  const auto Resolve =
      [&](uint64_t Address, MemoryAccessKind Kind,
          std::optional<uint64_t> &Saved) -> llvm::Expected<bool> {
    if (Saved)
      return true;
    uint64_t Physical;
    auto Walk = walkOperand(M, {Address, Kind, 3}, Out, Physical);
    if (!Walk)
      return Walk.takeError();
    if (!*Walk)
      return false;
    const uint64_t Page = llvm::alignDown(Physical, PageSize);
    if (!EPC.EPC.contains(Page)) {
      PF(Address, Kind);
      return false;
    }
    Saved = Page;
    return true;
  };
  auto GotInfo = Resolve(Info, MemoryAccessKind::Read, Progress.SecInfo);
  if (!GotInfo)
    return GotInfo.takeError();
  if (!*GotInfo)
    return Out;
  if (Op != Leaf::Accept) {
    auto Got = Resolve(Dest, DestAccess, Progress.Destination);
    if (!Got)
      return Got.takeError();
    if (!*Got)
      return Out;
    if (Copy) {
      Got = Resolve(Source, MemoryAccessKind::Read, Progress.Source);
      if (!Got)
        return Got.takeError();
      if (!*Got)
        return Out;
    }
  }
  if (Progress.Request.empty()) {
    const auto &P = EPC.EPC.at(*Progress.SecInfo);
    if (!regularPage(P, E, Info) ||
        !hasPermission(P.Permissions, MemoryPermission::Read)) {
      PF(Info, MemoryAccessKind::Read);
      return Out;
    }
    const auto *Bytes = P.Bytes.data() + (Info & (PageSize - 1));
    Progress.Request.assign(Bytes, Bytes + SecInfoSize);
  }
  const auto Flags = le::read64le(Progress.Request.data());
  if ((Flags & ~uint64_t{0xff3f}) ||
      std::any_of(Progress.Request.begin() + 8, Progress.Request.end(),
                  [](uint8_t V) { return V != 0; }))
    return GP();
  const uint8_t Type = Flags >> 8;
  const auto Mask = static_cast<MemoryPermission>(Flags & RWX);
  if (Op == Leaf::Accept) {
    if (!allowsOperand(E, Dest, PageSize))
      return GP();
    auto Got = Resolve(Dest, DestAccess, Progress.Destination);
    if (!Got)
      return Got.takeError();
    if (!*Got)
      return Out;
    if (!acceptsRegularPages(Type, Flags) && !acceptsModifiedPages(Type, Flags))
      return GP();
  }
  auto &P = EPC.EPC.at(*Progress.Destination);
  unsigned Error = 0;
  if (Op == Leaf::Extend) {
    if (!regularPage(P, E, Dest)) {
      PF(Dest, DestAccess);
      return Out;
    }
    const auto New = P.Permissions | Mask;
    if (hasPermission(New, MemoryPermission::Write) &&
        !hasPermission(New, MemoryPermission::Read))
      return GP();
    P.Permissions = New;
  } else if (Copy) {
    if (Type != Regular || (hasPermission(Mask, MemoryPermission::Write) &&
                            !hasPermission(Mask, MemoryPermission::Read)))
      return GP();
    const auto &Src = EPC.EPC.at(*Progress.Source);
    if (!regularPage(Src, E, Source) ||
        !hasPermission(Src.Permissions, MemoryPermission::Read)) {
      PF(Source, MemoryAccessKind::Read);
      return Out;
    }
    if (!ownsPage(P, E) || P.Linear != Dest || !P.Pending || P.Modified ||
        P.Type != X86SgxPageType::Regular ||
        P.Permissions != (MemoryPermission::Read | MemoryPermission::Write)) {
      Error = AttributeMismatch;
    } else {
      P.Bytes = Src.Bytes;
      P.Permissions = Mask;
      P.Pending = false;
    }
  } else {
    if (!ownsPage(P, E) ||
        (P.Type != X86SgxPageType::Regular && P.Type != X86SgxPageType::TCS &&
         P.Type != X86SgxPageType::Trimmed)) {
      PF(Dest, DestAccess);
      return Out;
    }
    const auto Expected = Type == Regular ? X86SgxPageType::Regular
                          : Type == TCS   ? X86SgxPageType::TCS
                                          : X86SgxPageType::Trimmed;
    // EAUG installs a fresh page. EMODPR and EMODT require a completed
    // tracking cycle before acceptance (SDM vol. 3D, 39.5.7-39.5.11).
    const bool NeedsTracking = P.PermissionRestricted || P.Modified;
    if (P.Linear != Dest || P.Pending != bool(Flags & Pending) ||
        P.Modified != bool(Flags & Modified) || P.Permissions != Mask ||
        P.Type != Expected) {
      Error = AttributeMismatch;
    } else if (NeedsTracking && !P.TrackingComplete) {
      Out.Status = X86LegacyExecutionStatus::ModeUnavailable;
      Out.OwnerError = "EACCEPT requires the destination page's tracking state";
      return Out;
    } else if (NeedsTracking && !*P.TrackingComplete) {
      Error = NotTracked;
    } else if (Type == TCS && !validNewTCS(P, EPC.Features)) {
      return GP();
    } else {
      P.Pending = P.Modified = P.PermissionRestricted = false;
    }
  }
  if (!Error)
    ++EPC.Generation;
  if (Op != Leaf::Extend) {
    R.GPR[0] = Error;
    R.RFlags = (R.RFlags & ~uint64_t{0x8d5}) | (Error ? 0x40 : 0);
  }
  retireEnclaveLeaf(S, InstructionSize, Out);
  return Out;
}
} // namespace neverd::analysis::detail
