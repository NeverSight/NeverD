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

#include "neverd/translate/X86/X86SgxKeyPlatform.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::analysis::detail {
using namespace translate;
namespace {
constexpr unsigned RequestSize = 512, ReportDataSize = 64, ReportSize = 432;
constexpr unsigned ReportDataAlignment = 128, ReportAlignment = 512;
constexpr unsigned KeySize = 16;
enum KeyName : uint16_t {
  InitToken = 0,
  Provision = 1,
  ProvisionSeal = 2,
  Report = 3,
  Seal = 4,
};
constexpr uint64_t InvalidAttribute = 2, InvalidCPUSVN = 32, InvalidISVSVN = 64,
                   InvalidKeyName = 256;

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
  return !(Attributes & X86SgxFeatures::KSS) &&
         ((Policy & 0x3c) ||
          llvm::support::endian::read16le(Request.data() + 76));
}
// CPUSVN is a processor-defined relation. Do not compare its bytes as an
// integer. The report key selects current platform identity regardless of
// the requested versions.
uint64_t keyError(std::span<const uint8_t, 512> Request, const X86SgxPage &SECS,
                  const X86SgxKeyPlatform *Platform) {
  namespace le = llvm::support::endian;
  const uint16_t Name = le::read16le(Request.data());
  const uint64_t Attributes = le::read64le(SECS.Bytes.data() + 48);
  if (Name > Seal)
    return InvalidKeyName;
  if ((Name == InitToken && !(Attributes & X86SgxFeatures::InitToken)) ||
      ((Name == Provision || Name == ProvisionSeal) &&
       !(Attributes & X86SgxFeatures::Provision)))
    return InvalidAttribute;
  if (!Platform || Name == Report)
    return 0;
  if (!Platform->acceptsCPUSVN(Request.subspan<8, 16>()))
    return InvalidCPUSVN;
  if (le::read16le(Request.data() + 4) >
          le::read16le(SECS.Bytes.data() + 258) ||
      (Name == Seal && le::read16le(Request.data() + 76) >
                           le::read16le(SECS.Bytes.data() + 260)))
    return InvalidISVSVN;
  return 0;
}

llvm::Expected<std::array<uint8_t, ReportSize>>
localReport(const X86SgxKeyPlatform &Platform, const X86SgxPage &SECS,
            std::span<const uint8_t, 512> Target,
            const std::array<uint8_t, ReportDataSize> &Data) {
  auto Identity = Platform.reportIdentity(SECS);
  if (!Identity)
    return Identity.takeError();
  // SDM, Layout of Report. The reserved fields are zero, including
  // CET_ATTRIBUTES in this non-CET profile.
  std::array<uint8_t, ReportSize> Result{};
  auto *Body = Result.data();
  std::copy_n(Identity->CPUSVN.begin(), 16, Body);
  std::copy_n(SECS.Bytes.data() + 20, 4, Body + 16); // MISCSELECT.
  std::copy_n(Identity->ISVExtProdID.begin(), 16, Body + 32);
  std::copy_n(SECS.Bytes.data() + 48, 48, Body + 48); // ATTRIBUTES, MRENCLAVE.
  std::copy_n(SECS.Bytes.data() + 128, 32, Body + 128); // MRSIGNER.
  // CONFIGID, ISVPRODID, ISVSVN and CONFIGSVN have the same offsets.
  std::copy_n(SECS.Bytes.data() + 192, 70, Body + 192);
  std::copy_n(Identity->ISVFamilyID.begin(), 16, Body + 304);
  std::copy(Data.begin(), Data.end(), Body + 320);
  std::copy_n(Identity->ReportKeyID.begin(), 32, Body + 384);
  auto MAC = Platform.reportMAC(SECS, Target, Identity->ReportKeyID,
                                std::span(Result).first<384>());
  if (!MAC)
    return MAC.takeError();
  std::copy(MAC->begin(), MAC->end(), Body + 416);
  return Result;
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
  const auto Accessed = [&](const X86MemoryAccessResult &Access) {
    Out.SelfModifications.insert(Out.SelfModifications.end(),
                                 Access.SelfModifications.begin(),
                                 Access.SelfModifications.end());
    Out.Exception = Access.Exception;
    Out.BackingFault = Access.BackingFault;
    if (Out.BackingFault)
      Out.Status = X86LegacyExecutionStatus::BackingFault;
    return Access.Status == X86PageWalkStatus::Completed;
  };
  if (Progress.Request.empty()) {
    if (!Admits(R.GPR[3], RequestSize))
      return Out;
    std::vector<uint8_t> Request(RequestSize);
    auto Read = M.load({R.GPR[3], MemoryAccessKind::Read, 3}, Request);
    if (!Read)
      return Read.takeError();
    if (!Accessed(*Read))
      return Out;
    Progress.Request = std::move(Request);
  }
  const std::span<const uint8_t, RequestSize> Request(Progress.Request.data(),
                                                      RequestSize);
  if (!Key && !Progress.ReportData) {
    if (reservedTargetInfo(Progress.Request)) {
      GP();
      return Out;
    }
    if (!Admits(R.GPR[1], ReportDataAlignment))
      return Out;
    std::array<uint8_t, ReportDataSize> Data;
    auto Read = M.load({R.GPR[1], MemoryAccessKind::Read, 3}, Data);
    if (!Read)
      return Read.takeError();
    if (!Accessed(*Read))
      return Out;
    Progress.ReportData = Data;
  }
  const auto Output = Key ? R.GPR[1] : R.GPR[2];
  if (!Admits(Output, Key ? KeySize : ReportAlignment))
    return Out;
  auto Write = M.prepareStore({Output, MemoryAccessKind::Write, 3},
                              Key ? KeySize : ReportSize);
  if (!Write)
    return Write.takeError();
  if (!Accessed(*Write))
    return Out;

  const auto &SECS = EPC.EPC.at(EPC.Active->SECS);
  uint64_t ErrorCode = 0;
  if (Key) {
    if (reservedKeyRequest(Progress.Request, llvm::support::endian::read64le(
                                                 SECS.Bytes.data() + 48))) {
      GP();
      return Out;
    }
    ErrorCode = keyError(Request, SECS, EPC.Keys.get());
  }
  if (!ErrorCode) {
    Out.Status = X86LegacyExecutionStatus::ModeUnavailable;
    if (!EPC.Keys) {
      Out.OwnerError = "enclave EREPORT and EGETKEY require platform key state";
      return Out;
    }
    if (Key) {
      auto Bytes = EPC.Keys->deriveKey(SECS, Request);
      if (!Bytes) {
        Out.OwnerError = llvm::toString(Bytes.takeError());
        return Out;
      }
      M.commitStore(*Write, *Bytes);
    } else {
      auto Bytes = localReport(*EPC.Keys, SECS, Request, *Progress.ReportData);
      if (!Bytes) {
        Out.OwnerError = llvm::toString(Bytes.takeError());
        return Out;
      }
      M.commitStore(*Write, *Bytes);
    }
    Out.Status = X86LegacyExecutionStatus::Completed;
  }
  if (Key) {
    R.GPR[0] = ErrorCode;
    R.RFlags = (R.RFlags & ~uint64_t{0x8d5}) | (ErrorCode ? 0x40 : 0);
  }
  retireEnclaveLeaf(S, InstructionSize, Out);
  return Out;
}
} // namespace neverd::analysis::detail
