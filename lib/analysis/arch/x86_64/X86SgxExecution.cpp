//===- X86SgxExecution.cpp - Software enclave transfers -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// SGX1 x64, one logical processor, initialized SECS and x87/SSE SSA frames.
/// Layout and transfer rules follow Intel SDM vol. 3D, chapters 38, 40 and 41.
//===----------------------------------------------------------------------===//

#include "X86SgxExecution.h"

#include "X86ModeState.h"

#include "neverd/decode/Decoder.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/Errc.h"

#include <algorithm>
#include <bit>

namespace neverd::analysis::detail {
using namespace translate;
namespace {
namespace le = llvm::support::endian;
constexpr unsigned PageSize = 4096, GprOffset = PageSize - 184;
constexpr unsigned FlagsOffset = 128, IpOffset = 136, UserSPOffset = 144,
                   UserBPOffset = 152;
constexpr unsigned ExitInfoOffset = 160, FSOffset = 168, GSOffset = 176;
using Status = X86LegacyExecutionStatus;

llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::not_supported, Message);
}

bool canonical(uint64_t V, unsigned Bits) {
  const auto Upper = V >> (Bits - 1);
  return !Upper || Upper == (UINT64_MAX >> (Bits - 1));
}

template <typename Saved>
void restoreBases(X64ContinuationStateV6 &V, const Saved &T) {
  auto &R = V.Core.Core.Core.Core;
  R.FSBase = T.FS.Base;
  R.GSBase = T.GS.Base;
  V.FS = {T.FS.Limit, T.FS.Selector, T.FS.Attributes};
  V.GS = {T.GS.Limit, T.GS.Selector, T.GS.Attributes};
  V.Unusable = (V.Unusable & ~0x30u) | T.Unusable;
}

uint32_t saveExitInfo(uint8_t *GPR, bool ExInfo,
                      const X64ContinuationEventV1 &E) {
  const bool Exception =
      E.Kind != X86EventKind::Interrupt && E.Kind != X86EventKind::NMI;
  if (ExInfo && Exception &&
      (E.Vector == 13 || (E.Vector == 14 && (E.ErrorCode & 4)))) {
    le::write64le(GPR - 16, E.Vector == 14 ? E.FaultAddress : 0);
    le::write32le(GPR - 8, E.ErrorCode);
    le::write32le(GPR - 4, 0);
    return 0x80000300u | E.Vector;
  }
  if (Exception)
    switch (E.Vector) {
    case 0:
    case 1:
    case 3:
    case 5:
    case 6:
    case 16:
    case 17:
    case 19:
      return 0x80000000u | ((E.Vector == 3 ? 6u : 3u) << 8) | E.Vector;
    default:
      break;
    }
  if (ExInfo)
    std::fill_n(GPR - 16, 16, 0);
  return 0;
}

} // namespace

llvm::Expected<bool>
X86SgxExecution::illegalInstruction(llvm::ArrayRef<uint8_t> Bytes,
                                    uint64_t PC) {
  Decoder D;
  if (!D.init(Arch::X64))
    return invalid("software enclave instruction decoder unavailable");
  DecodedInsn I{};
  if (D.decodeOne(Bytes.data(), Bytes.size(), PC, I) <= 0 ||
      I.Size != Bytes.size())
    return invalid(
        "software enclave requires an exact original instruction extent");
  switch (I.Id) {
  case X86_INS_CPUID:
  case X86_INS_GETSEC:
  case X86_INS_RDPMC:
  case X86_INS_SGDT:
  case X86_INS_SIDT:
  case X86_INS_SLDT:
  case X86_INS_STR:
  case X86_INS_VMCALL:
  case X86_INS_VMFUNC:
  case X86_INS_SMSW:
  case X86_INS_IN:
  case X86_INS_INSB:
  case X86_INS_INSD:
  case X86_INS_INSW:
  case X86_INS_OUT:
  case X86_INS_OUTSB:
  case X86_INS_OUTSD:
  case X86_INS_OUTSW:
  case X86_INS_LCALL:
  case X86_INS_LJMP:
  case X86_INS_RETF:
  case X86_INS_RETFQ:
  case X86_INS_IRET:
  case X86_INS_IRETD:
  case X86_INS_IRETQ:
  case X86_INS_INT:
  case X86_INS_INTO:
  case X86_INS_INT1:
  case X86_INS_INT3:
  case X86_INS_LDS:
  case X86_INS_LES:
  case X86_INS_LFS:
  case X86_INS_LGS:
  case X86_INS_LSS:
  case X86_INS_SYSCALL:
  case X86_INS_SYSENTER:
    return true;
  case X86_INS_MOV:
  case X86_INS_POP: {
    const auto &X = I.Raw->detail->x86;
    if (!X.op_count || X.operands[0].type != X86_OP_REG)
      return false;
    const auto Reg = X.operands[0].reg;
    return Reg == X86_REG_DS || Reg == X86_REG_ES || Reg == X86_REG_SS ||
           Reg == X86_REG_FS || Reg == X86_REG_GS;
  }
  default:
    return false;
  }
}

llvm::Expected<X86ModeExecutionResult>
X86SgxExecution::instruction(X86ModeState &S, X86PagedMemory &M, bool Fetched,
                             llvm::ArrayRef<uint8_t> ExpectedBytes,
                             bool Illegal, X86SgxInstructionProgress &Progress,
                             uint32_t MXCSRMask) {
  auto &EPC = *M.sgxMemory();
  auto &V = S.Core.Core;
  auto &R = modeRegisters(S);
  const auto &C = M.controls();
  const unsigned Bits = C.CR4 & 0x1000 ? 57 : 48;
  const uint8_t CPL = V.Core.CS.Selector & 3;
  X86ModeExecutionResult Out;
  const auto Fault = [&](uint8_t Vector, uint32_t Error = 0,
                         std::optional<uint64_t> CR2 = {}) {
    Out.Exception = X86MemoryException{Vector, Error, CR2};
    return Out;
  };
  if (((R.RFlags & 0x100) && !S.Debug) || (S.Debug && (S.Debug->DR7 & 0xff)))
    return invalid("software enclave transfer requires its TF producer and "
                   "disabled address comparators");
  if (!Fetched) {
    auto OK = fetchX86SystemInstruction(S, M, ExpectedBytes, {}, Out);
    if (!OK)
      return OK.takeError();
    if (!*OK)
      return Out;
  }
  if (Illegal)
    return Fault(6);
  if (!(C.CR0 & 1) || (R.RFlags & 0x20000) || (S.SMM && S.SMM->Active))
    return Fault(6);
  if (C.CR0 & 8)
    return Fault(7);
  if (CPL < 3)
    return Fault(6);
  if (!(C.CR0 & 0x80000000) || !(C.CR0 & 0x20))
    return Fault(13);
  const uint32_t Leaf = R.GPR[0];
  if (Leaf == 4) {
    if (!EPC.Active)
      return Fault(13);
    if (!EPC.Running)
      return invalid(
          "EEXIT requires the thread context saved by EENTER or ERESUME");
    if (!canonical(R.GPR[3], Bits))
      return Fault(13);
    const auto &T = *EPC.Running;
    if (T.OutsideTF && !S.Debug)
      return invalid("EEXIT requires the debug bank retained at opt-out entry");
    restoreBases(V, T);
    R.PC = R.GPR[3];
    R.GPR[1] = T.AEP;
    R.RFlags = (R.RFlags & ~uint64_t(0x10100)) | (uint64_t(T.OutsideTF) << 8);
    if (T.OutsideTF) {
      x86PublishInstructionDebug(*S.Debug, true);
      Out.Exception = X86MemoryException{1, 0, {}};
    }
    le::write64le(EPC.EPC.at(T.TCS).Bytes.data(), 0);
    EPC.Running.reset();
    EPC.Active.reset();
    ++EPC.Generation;
    Out.InstructionCompleted = true;
    return Out;
  }
  if (Leaf > 4)
    return Fault(13);
  if (Leaf != 2 && Leaf != 3)
    return invalid(
        "software enclave owner requires an EENTER, ERESUME or EEXIT leaf");
  if (EPC.Active)
    return Fault(13);
  if (!EPC.X87)
    return invalid(
        "software SGX1 requires an explicit x87/SSE processor image");
  const uint64_t TCSLinear = R.GPR[3], AEP = R.GPR[1];
  if ((TCSLinear & (PageSize - 1)) || !canonical(TCSLinear, Bits))
    return Fault(13);

  const auto Walk = [&](uint64_t Linear, uint64_t &Physical) -> llvm::Error {
    auto P = walkX86PageTables(M.physicalMemory(), M.features(), C,
                               {Linear, MemoryAccessKind::Write, CPL});
    if (!P)
      return P.takeError();
    Out.Exception = P->Exception;
    Out.BackingFault = P->BackingFault;
    if (P->BackingFault)
      Out.Status = Status::BackingFault;
    Physical = P->PhysicalAddress;
    return llvm::Error::success();
  };
  uint64_t TCSPhysical = 0;
  if (Progress.TCSPhysical)
    TCSPhysical = *Progress.TCSPhysical;
  else {
    if (auto E = Walk(TCSLinear, TCSPhysical))
      return std::move(E);
    if (Out.Exception || Out.BackingFault)
      return Out;
    Progress.TCSPhysical = TCSPhysical;
  }
  const auto Page = [&](uint64_t Physical, uint64_t Linear,
                        X86SgxPageType Type) -> X86SgxPage * {
    auto I = EPC.EPC.find(Physical);
    if (I == EPC.EPC.end())
      return nullptr;
    auto &P = I->second;
    return P.Valid && !P.Blocked && !P.Pending && !P.Modified &&
                   P.Type == Type && P.Linear == Linear
               ? &P
               : nullptr;
  };
  if (!EPC.EPC.contains(TCSPhysical))
    return Fault(14, 0x8007, TCSLinear);
  if (!canonical(AEP, Bits))
    return Fault(13);
  auto *TCS = Page(TCSPhysical, TCSLinear, X86SgxPageType::TCS);
  if (!TCS)
    return Fault(14, 0x8007, TCSLinear);
  const auto Root = EPC.EPC.find(TCS->OwnerSECS);
  if (Root == EPC.EPC.end() || Root->second.Type != X86SgxPageType::SECS ||
      !Root->second.Valid || Root->second.Blocked)
    return invalid("software TCS has no resident initialized SECS");
  const uint8_t *SECS = Root->second.Bytes.data();
  X86SgxEnclave Enclave{Root->first, le::read64le(SECS + 8),
                        le::read64le(SECS)};
  const auto Attributes = le::read64le(SECS + 48);
  if (!(Attributes & 1) || !(Attributes & 4) || !(C.CR4 & 0x200))
    return Fault(13);
  const uint64_t FramePages = le::read32le(SECS + 16);
  if (le::read64le(SECS + 56) != 3)
    return Fault(13);
  const auto MiscSelect = le::read32le(SECS + 20);
  if (!FramePages || (MiscSelect & ~1u) || (Attributes & ~uint64_t(7)))
    return invalid("unsupported initialized SECS attributes or SSA geometry");
  if (!std::has_single_bit(Enclave.Size) || Enclave.Size < 2 * PageSize ||
      (Enclave.Base & (Enclave.Size - 1)) || !Enclave.contains(TCSLinear))
    return invalid("TCS disagrees with the imported SECS address range");
  uint8_t *Thread = TCS->Bytes.data();
  const auto TcsFlags = le::read64le(Thread + 8);
  if (TcsFlags & ~uint64_t(3))
    return Fault(13);
  if (TcsFlags)
    return invalid(
        "software SGX1 requires an opt-out TCS without AEX notification");
  const bool Resume = Leaf == 3;
  const auto CSSA = le::read32le(Thread + 24), NSSA = le::read32le(Thread + 28);
  if (le::read64le(Thread) || (Resume ? !CSSA || CSSA > NSSA : CSSA >= NSSA))
    return Fault(13);
  const uint64_t OSSA = le::read64le(Thread + 16);
  const uint64_t FSOffsetInEnclave = le::read64le(Thread + 48),
                 GSOffsetInEnclave = le::read64le(Thread + 56);
  if ((OSSA | FSOffsetInEnclave | GSOffsetInEnclave) & (PageSize - 1))
    return Fault(13);
  const auto Frame = uint64_t(Resume ? CSSA - 1 : CSSA);
  const uint64_t FrameSize = FramePages * PageSize;
  if (FrameSize > Enclave.Size || OSSA > Enclave.Size - FrameSize ||
      Frame > (Enclave.Size - FrameSize - OSSA) / FrameSize)
    return Fault(13);
  const auto SSALinear = Enclave.Base + OSSA + Frame * FrameSize;
  const auto SsaPage = [&](uint64_t Physical, uint64_t Linear) -> X86SgxPage * {
    auto *P = Page(Physical & ~(PageSize - 1), Linear & ~(PageSize - 1),
                   X86SgxPageType::Regular);
    return P && P->OwnerSECS == Root->first &&
                   hasPermission(P->Permissions, MemoryPermission::Write)
               ? P
               : nullptr;
  };
  uint64_t XSavePhysical = 0;
  if (Progress.XSavePhysical)
    XSavePhysical = *Progress.XSavePhysical;
  else {
    if (auto E = Walk(SSALinear, XSavePhysical))
      return std::move(E);
    if (Out.Exception || Out.BackingFault)
      return Out;
  }
  auto *XSave = SsaPage(XSavePhysical, SSALinear);
  if (!XSave)
    return Fault(14, 0x8007, SSALinear);
  Progress.XSavePhysical = XSavePhysical;
  auto *GprPage = XSave;
  if (FramePages > 1) {
    const uint64_t GprLinear = SSALinear + FrameSize - PageSize + GprOffset;
    uint64_t GprPhysical = 0;
    if (auto E = Walk(GprLinear, GprPhysical))
      return std::move(E);
    if (Out.Exception || Out.BackingFault)
      return Out;
    GprPage = SsaPage(GprPhysical, GprLinear);
    if (!GprPage)
      return Fault(14, 0x8007, GprLinear);
  }
  uint8_t *FX = XSave->Bytes.data(), *GPR = GprPage->Bytes.data() + GprOffset;
  const auto Target = Resume ? le::read64le(GPR + IpOffset)
                             : Enclave.Base + le::read64le(Thread + 32);
  const auto FS =
      Resume ? le::read64le(GPR + FSOffset) : Enclave.Base + FSOffsetInEnclave;
  const auto GS =
      Resume ? le::read64le(GPR + GSOffset) : Enclave.Base + GSOffsetInEnclave;
  if (!canonical(Target, Bits) || !canonical(FS, Bits) || !canonical(GS, Bits))
    return Fault(13);
  if (Resume &&
      ((le::read64le(FX + 512) & ~uint64_t(3)) ||
       std::any_of(FX + 520, FX + 576, [](uint8_t B) { return B != 0; }) ||
       ((le::read64le(FX + 512) & 2) && (le::read32le(FX + 24) & ~MXCSRMask))))
    return Fault(13);
  EPC.Running = X86SgxMemory::Thread{
      TCSPhysical,
      TCSLinear,
      XSavePhysical,
      GprPage->Physical,
      AEP,
      {R.FSBase, V.FS.Limit, V.FS.Selector, V.FS.Attributes},
      {R.GSBase, V.GS.Limit, V.GS.Selector, V.GS.Attributes},
      uint8_t(V.Unusable & 0x30),
      bool(R.RFlags & 0x100)};
  if (Resume) {
    for (unsigned I = 0; I != 16; ++I)
      R.GPR[I] = le::read64le(GPR + I * 8);
    const uint32_t Mask =
        0x254cd5u | ((R.RFlags & 0x3000) == 0x3000 ? 0x200u : 0u);
    R.RFlags = (R.RFlags & ~uint64_t(Mask | 0x100)) |
               (le::read64le(GPR + FlagsOffset) & Mask);
    const auto XStateBV = le::read64le(FX + 512);
    if (XStateBV & 1)
      std::copy_n(FX, EPC.X87->size(), EPC.X87->begin());
    else {
      EPC.X87->fill(0);
      le::write16le(EPC.X87->data(), 0x37f);
    }
    V.Core.Core.Core.MXCSR = XStateBV & 2 ? le::read32le(FX + 24) : 0x1f80;
    for (unsigned I = 0; I != 16; ++I)
      if (XStateBV & 2)
        std::copy_n(FX + 160 + I * 16, 16, R.ZMM[I]);
      else
        std::fill_n(R.ZMM[I], 16, 0);
    le::write32le(Thread + 24, CSSA - 1);
  } else {
    le::write64le(GPR + UserSPOffset, R.GPR[4]);
    le::write64le(GPR + UserBPOffset, R.GPR[5]);
    R.GPR[1] = R.PC + 3;
    R.GPR[0] = CSSA;
    R.RFlags &= ~uint64_t(0x10100);
  }
  R.PC = Target;
  R.FSBase = FS;
  R.GSBase = GS;
  const auto DataAttributes = uint16_t(0xc091 | (V.DS.Attributes & 0x3062));
  V.FS = {le::read32le(Thread + 64), 0x0b, DataAttributes};
  V.GS = {le::read32le(Thread + 68), 0x0b, DataAttributes};
  V.Unusable &= ~0x30u;
  le::write64le(Thread, 1);
  EPC.Active = Enclave;
  ++EPC.Generation;
  Out.InstructionCompleted = true;
  return Out;
}

llvm::Error X86SgxExecution::asynchronousExit(X64ContinuationStateV6 &V,
                                              X86SgxMemory &EPC,
                                              X64ContinuationEventV1 &Event,
                                              uint64_t SavedFlags) {
  if (!EPC.Running || !EPC.X87)
    return invalid("software AEX requires a running TCS and cached SSA frame");
  const auto T = *EPC.Running;
  auto &R = V.Core.Core.Core.Core;
  auto *FX = EPC.EPC.at(T.XSavePage).Bytes.data();
  auto *GPR = EPC.EPC.at(T.GprPage).Bytes.data() + GprOffset;
  auto *TCS = EPC.EPC.at(T.TCS).Bytes.data();
  for (unsigned I = 0; I != 16; ++I)
    le::write64le(GPR + I * 8, R.GPR[I]);
  le::write64le(GPR + FlagsOffset, SavedFlags & ~uint64_t(0x100));
  le::write64le(GPR + IpOffset, Event.SavedPC);
  le::write64le(GPR + FSOffset, R.FSBase);
  le::write64le(GPR + GSOffset, R.GSBase);
  const auto &Secs = EPC.EPC.at(EPC.Active->SECS);
  const bool ExInfo = le::read32le(Secs.Bytes.data() + 20) & 1;
  le::write32le(GPR + ExitInfoOffset, saveExitInfo(GPR, ExInfo, Event));
  std::copy(EPC.X87->begin(), EPC.X87->end(), FX);
  le::write32le(FX + 24, V.Core.Core.Core.MXCSR);
  for (unsigned I = 0; I != 16; ++I)
    std::copy_n(R.ZMM[I], 16, FX + 160 + I * 16);
  le::write64le(FX + 512, 3);
  std::fill_n(FX + 520, 16, 0);
  const bool Exception =
      Event.Kind != X86EventKind::Interrupt && Event.Kind != X86EventKind::NMI;
  EPC.X87->fill(0);
  le::write16le(EPC.X87->data(),
                Exception && Event.Vector == 16 ? 0x37e : 0x37f);
  if (Exception && Event.Vector == 16)
    le::write16le(EPC.X87->data() + 2, 0x8081);
  V.Core.Core.Core.MXCSR = Exception && Event.Vector == 19 ? 0x1f01 : 0x1fb0;
  for (unsigned I = 0; I != 16; ++I)
    std::fill_n(R.ZMM[I], 16, 0);
  std::fill(std::begin(R.GPR), std::end(R.GPR), 0);
  R.GPR[0] = 3;
  R.GPR[1] = T.AEP;
  R.GPR[3] = T.TCSLinear;
  R.GPR[4] = le::read64le(GPR + UserSPOffset);
  R.GPR[5] = le::read64le(GPR + UserBPOffset);
  R.RFlags = (R.RFlags & ~uint64_t(0x109d5)) | (uint64_t(T.OutsideTF) << 8);
  Event.SavedPC = R.PC = T.AEP;
  if (Exception && Event.Vector == 14)
    Event.FaultAddress &= ~uint64_t(PageSize - 1);
  restoreBases(V, T);
  le::write32le(TCS + 24, le::read32le(TCS + 24) + 1);
  le::write64le(TCS, 0);
  EPC.Active.reset();
  EPC.Running.reset();
  ++EPC.Generation;
  return llvm::Error::success();
}

} // namespace neverd::analysis::detail
