//===- X86SgxKeyReport.cpp - SGX1 key and report operands ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Ordered ENCLU key and report operands for an imported non-CET processor.
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
constexpr unsigned ReportMACStructSize = 256;
constexpr unsigned ReportDataAlignment = 128, ReportAlignment = 512;
enum KeyName : uint16_t {
  InitToken = 0,
  Provision = 1,
  ProvisionSeal = 2,
  Report = 3,
  Seal = 4,
};
constexpr uint64_t InvalidAttribute = 2, InvalidReportMAC = 28,
                   InvalidCPUSVN = 32, InvalidISVSVN = 64, InvalidKeyName = 256,
                   InvalidReportSubtype = 33;

bool hasReservedBytes(llvm::ArrayRef<uint8_t> Bytes) {
  return std::any_of(Bytes.begin(), Bytes.end(),
                     [](uint8_t B) { return B != 0; });
}

llvm::Expected<uint64_t>
verifyReport2Status(X86SgxVerifyReport2Profile Profile,
                    const X86SgxKeyPlatform *Platform, const X86SgxPage &SECS,
                    std::span<const uint8_t, ReportMACStructSize> Input) {
  if (Profile == X86SgxVerifyReport2Profile::Unbound)
    return llvm::createStringError(
        "EVERIFYREPORT2 requires a bound verification profile");
  const bool Legacy = Profile == X86SgxVerifyReport2Profile::TDX002;
  if (Legacy && (Input[0] != 0x81 || hasReservedBytes({Input.data() + 1, 15}) ||
                 hasReservedBytes({Input.data() + 192, 32})))
    return InvalidReportMAC;
  if (!Platform)
    return llvm::createStringError(
        "EVERIFYREPORT2 requires current platform identity");
  const auto SVN = Input.subspan<16, 16>();
  if (Legacy) {
    if (!Platform->acceptsCPUSVN(SVN))
      return InvalidCPUSVN;
  } else {
    auto Identity = Platform->reportIdentity(SECS);
    if (!Identity)
      return Identity.takeError();
    if (!std::equal(SVN.begin(), SVN.end(), Identity->CPUSVN.begin()))
      return InvalidCPUSVN;
  }
  auto Matches =
      Platform->verifyReport2MAC(Input.first<224>(), Input.last<32>());
  if (!Matches)
    return Matches.takeError();
  return *Matches ? 0 : InvalidReportMAC;
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
                  const X86SgxKeyPlatform *Platform, bool WideKey) {
  namespace le = llvm::support::endian;
  const uint16_t Name = le::read16le(Request.data());
  const uint64_t Attributes = le::read64le(SECS.Bytes.data() + 48);
  if (Name > Seal || (WideKey && (Name == Provision || Name == Report)))
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
    KeyReportLeaf Leaf, X86ModeState &S, X86PagedMemory &M,
    X86SgxInstructionProgress &Progress, unsigned InstructionSize,
    X86ModeExecutionResult Out) {
  auto &EPC = *M.sgxMemory();
  auto &R = modeRegisters(S);
  const bool Report2 = Leaf == KeyReportLeaf::Report2;
  const bool VerifyReport = Leaf == KeyReportLeaf::VerifyReport2;
  const bool LegacyReport = Leaf == KeyReportLeaf::Report;
  const bool WideKey = Leaf == KeyReportLeaf::Key256;
  const bool Key = Leaf == KeyReportLeaf::Key || WideKey;
  const unsigned KeySize = WideKey ? 32 : 16;
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
  const auto Unavailable = [&](std::string Reason) {
    Out.Status = X86LegacyExecutionStatus::ModeUnavailable;
    Out.OwnerError = std::move(Reason);
    return Out;
  };
  const auto Retire = [&](uint64_t Status) {
    if (!LegacyReport) {
      const uint64_t Flags =
          (R.RFlags & ~uint64_t{0x8d5}) | (Status ? 0x40 : 0);
      if (VerifyReport && !EPC.Features.VerifyReport2Flags && Flags != R.RFlags)
        return Unavailable(
            "EVERIFYREPORT2 requires a bound status-flag result");
      R.GPR[0] = Status;
      if (!VerifyReport ||
          EPC.Features.VerifyReport2Flags != X86SgxVerifyReport2Flags::Preserve)
        R.RFlags = Flags;
    }
    retireEnclaveLeaf(S, InstructionSize, Out);
    return Out;
  };
  if (Progress.Request.empty()) {
    const unsigned InputSize = VerifyReport
                                   ? ReportMACStructSize
                                   : (Report2 ? ReportDataSize : RequestSize);
    const auto Profile = EPC.Features.VerifyReport2;
    const unsigned Alignment =
        VerifyReport && Profile == X86SgxVerifyReport2Profile::SDM093
            ? X86SgxPage::Size
            : InputSize;
    if (!Admits(R.GPR[3], Alignment))
      return Out;
    if (VerifyReport && Profile == X86SgxVerifyReport2Profile::Unbound &&
        (R.GPR[3] & (X86SgxPage::Size - 1)))
      return Unavailable("EVERIFYREPORT2 requires a bound operand alignment");
    std::vector<uint8_t> Request(InputSize);
    auto Read = M.load({R.GPR[3], MemoryAccessKind::Read, 3}, Request);
    if (!Read)
      return Read.takeError();
    if (!Accessed(*Read))
      return Out;
    Progress.Request = std::move(Request);
  }
  const std::span<const uint8_t> Input(Progress.Request);
  if (VerifyReport) {
    auto Status = verifyReport2Status(
        *EPC.Features.VerifyReport2, EPC.Keys.get(),
        EPC.EPC.at(EPC.Active->SECS), Input.first<ReportMACStructSize>());
    if (!Status)
      return Unavailable(llvm::toString(Status.takeError()));
    return Retire(*Status);
  }
  if (LegacyReport && !Progress.ReportData) {
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
  const auto Output = LegacyReport ? R.GPR[2] : R.GPR[1];
  const unsigned Alignment = Report2        ? Report2Size
                             : LegacyReport ? ReportAlignment
                                            : KeySize;
  if (!Admits(Output, Alignment))
    return Out;
  unsigned OutputSize = LegacyReport ? ReportSize : KeySize;
  if (Report2)
    OutputSize =
        EPC.Features.Report2Write == X86SgxReport2Write::ZeroReservedTail
            ? Report2Size
            : Report2PayloadSize;
  auto Write = M.prepareStore({Output, MemoryAccessKind::Write, 3}, OutputSize);
  if (!Write)
    return Write.takeError();
  if (!Accessed(*Write))
    return Out;

  if (Report2 && R.GPR[2])
    return Retire(InvalidReportSubtype);
  if (Report2 && !EPC.Features.Report2Write)
    return Unavailable("EREPORT2 requires a bound output write footprint");
  if (Report2 && !EPC.Features.Report2MACCoverage)
    return Unavailable("EREPORT2 requires a bound MAC prefix size");
  const auto &SECS = EPC.EPC.at(EPC.Active->SECS);
  if (Key) {
    if (reservedKeyRequest(Progress.Request, llvm::support::endian::read64le(
                                                 SECS.Bytes.data() + 48))) {
      GP();
      return Out;
    }
    if (auto Error =
            keyError(Input.first<RequestSize>(), SECS, EPC.Keys.get(), WideKey))
      return Retire(Error);
  }
  if (!EPC.Keys)
    return Unavailable(
        "enclave report and key derivation require platform key state");
  const auto Publish = [&](auto Bytes) {
    if (!Bytes)
      return Unavailable(llvm::toString(Bytes.takeError()));
    M.commitStore(*Write,
                  llvm::ArrayRef<uint8_t>(*Bytes).take_front(OutputSize));
    return Retire(0);
  };
  if (Report2)
    return Publish(buildReport2(*EPC.Keys, SECS, Input.first<ReportDataSize>(),
                                *EPC.Features.Report2MACCoverage));
  const auto Request = Input.first<RequestSize>();
  if (WideKey)
    return Publish(EPC.Keys->deriveKey256(SECS, Request));
  if (Key)
    return Publish(EPC.Keys->deriveKey(SECS, Request));
  return Publish(localReport(*EPC.Keys, SECS, Request, *Progress.ReportData));
}
} // namespace neverd::analysis::detail
