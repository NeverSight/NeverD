//===- X86SgxKeyReport.cpp - SGX1 key and report operands ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Ordered ENCLU operand checks for the imported, non-CET SGX1 processor.
//===----------------------------------------------------------------------===//

#include "X86ModeState.h"
#include "X86SgxExecution.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::analysis::detail {
using namespace translate;
namespace {
constexpr unsigned RequestSize = 512, ReportDataSize = 64, ReportSize = 432;
constexpr unsigned ReportDataAlignment = 128, ReportAlignment = 512;
constexpr unsigned KeySize = 16;
constexpr uint64_t InitTokenAttribute = 1u << 5, ProvisionAttribute = 1u << 4;
constexpr uint64_t InvalidAttribute = 2, InvalidKeyName = 256;

bool hasReservedBytes(llvm::ArrayRef<uint8_t> Bytes) {
  return std::any_of(Bytes.begin(), Bytes.end(),
                     [](uint8_t B) { return B != 0; });
}

bool reservedTargetInfo(llvm::ArrayRef<uint8_t> Target) {
  // TARGETINFO.CET_ATTRIBUTES is reserved in this CPU profile.
  return hasReservedBytes(Target.slice(48, 2)) ||
         hasReservedBytes(Target.slice(56, 8)) ||
         hasReservedBytes(Target.drop_front(128));
}

bool reservedKeyRequest(llvm::ArrayRef<uint8_t> Request, uint64_t Attributes) {
  const auto Policy = llvm::support::endian::read16le(Request.data() + 2);
  // CET_ATTRIBUTES_MASK and the following byte are reserved without CET.
  if ((Policy & ~0x3fu) || hasReservedBytes(Request.slice(6, 2)) ||
      hasReservedBytes(Request.drop_front(78)))
    return true;
  constexpr uint64_t KSS = 1u << 7;
  return !(Attributes & KSS) &&
         ((Policy & 0x3c) ||
          llvm::support::endian::read16le(Request.data() + 76));
}
} // namespace

llvm::Expected<X86ModeExecutionResult> X86SgxExecution::keyOrReport(
    X86ModeState &S, X86PagedMemory &M, X86SgxInstructionProgress &Progress,
    unsigned InstructionSize, X86ModeExecutionResult Out) {
  auto &EPC = *M.sgxMemory();
  auto &R = modeRegisters(S);
  const bool Key = uint32_t(R.GPR[0]) == 1;
  const auto GP = [&] { Out.Exception = X86MemoryException{13, 0, {}}; };
  const auto Admits = [&](uint64_t Address, unsigned Alignment) {
    if (EPC.Active && EPC.Active->contains(Address) &&
        !(Address & (Alignment - 1)))
      return true;
    GP();
    return false;
  };
  const auto Accessed = [&](llvm::Expected<X86MemoryAccessResult> Access)
      -> llvm::Expected<bool> {
    if (!Access)
      return Access.takeError();
    Out.SelfModifications.insert(Out.SelfModifications.end(),
                                 Access->SelfModifications.begin(),
                                 Access->SelfModifications.end());
    Out.Exception = Access->Exception;
    Out.BackingFault = Access->BackingFault;
    if (Out.BackingFault)
      Out.Status = X86LegacyExecutionStatus::BackingFault;
    return Access->Status == X86PageWalkStatus::Completed;
  };
  if (Progress.Request.empty()) {
    if (!Admits(R.GPR[3], RequestSize))
      return Out;
    std::vector<uint8_t> Request(RequestSize);
    auto Read =
        Accessed(M.load({R.GPR[3], MemoryAccessKind::Read, 3}, Request));
    if (!Read)
      return Read.takeError();
    if (!*Read)
      return Out;
    Progress.Request = std::move(Request);
  }
  if (!Key && !Progress.ReportDataChecked) {
    if (reservedTargetInfo(Progress.Request)) {
      GP();
      return Out;
    }
    if (!Admits(R.GPR[1], ReportDataAlignment))
      return Out;
    auto Read = Accessed(
        M.probe({R.GPR[1], MemoryAccessKind::Read, 3}, ReportDataSize));
    if (!Read)
      return Read.takeError();
    if (!*Read)
      return Out;
    Progress.ReportDataChecked = true;
  }
  const auto Output = Key ? R.GPR[1] : R.GPR[2];
  if (!Admits(Output, Key ? KeySize : ReportAlignment))
    return Out;
  auto Write = Accessed(M.probe({Output, MemoryAccessKind::Write, 3},
                                Key ? KeySize : ReportSize));
  if (!Write)
    return Write.takeError();
  if (!*Write)
    return Out;

  if (Key) {
    const auto Attributes = llvm::support::endian::read64le(
        EPC.EPC.at(EPC.Active->SECS).Bytes.data() + 48);
    if (reservedKeyRequest(Progress.Request, Attributes)) {
      GP();
      return Out;
    }
    const uint16_t Name =
        llvm::support::endian::read16le(Progress.Request.data());
    if (Name > 4 || (Name == 0 && !(Attributes & InitTokenAttribute)) ||
        ((Name == 1 || Name == 2) && !(Attributes & ProvisionAttribute))) {
      R.GPR[0] = Name > 4 ? InvalidKeyName : InvalidAttribute;
      // These outcomes retire, set ZF, clear the other arithmetic flags and
      // leave the output bytes intact. RF clears on retirement.
      R.RFlags = (R.RFlags & ~uint64_t{0x108d5}) | 0x40;
      R.PC += InstructionSize;
      Out.InstructionCompleted = true;
      if (x86InstructionDebugStep(R.RFlags & 0x100,
                                  S.Debug ? S.Debug->DebugCTL : 0, false)) {
        x86PublishInstructionDebug(*S.Debug, true);
        Out.Exception = X86MemoryException{1, 0, {}};
      }
      return Out;
    }
  }
  Out.Status = X86LegacyExecutionStatus::ModeUnavailable;
  Out.OwnerError = "enclave EREPORT and EGETKEY require platform key state";
  return Out;
}
} // namespace neverd::analysis::detail
