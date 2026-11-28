//===- X86SgxExecution.cpp - Software enclave transfers -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// x64 enclave transfers over an initialized SECS and standard SSA frames.
/// Layout and transfer rules follow Intel SDM vol. 3D, chapters 38, 40 and 41.
//===----------------------------------------------------------------------===//

#include "X86SgxExecution.h"

#include "X86ModeState.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86/X86UndefinedOpcodeRules.h"

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

llvm::Expected<bool> X86SgxExecution::walkOperand(X86PagedMemory &M,
                                                  const X86LinearAccess &Access,
                                                  X86ModeExecutionResult &Out,
                                                  uint64_t &Physical) {
  auto Walk =
      walkX86PageTables(M.physicalMemory(), M.features(), M.controls(), Access);
  if (!Walk)
    return Walk.takeError();
  Out.SelfModifications.insert(Out.SelfModifications.end(),
                               Walk->SelfModifications.begin(),
                               Walk->SelfModifications.end());
  Out.Exception = Walk->Exception;
  Out.BackingFault = Walk->BackingFault;
  if (Out.BackingFault)
    Out.Status = Status::BackingFault;
  Physical = Walk->PhysicalAddress;
  return !Out.Exception && !Out.BackingFault;
}

llvm::Expected<bool>
X86SgxExecution::bindFrame(X86PagedMemory &M, const X86SgxEnclave &Enclave,
                           const X86SgxPage &TCS, uint32_t FramePages,
                           uint32_t Index, X86SgxFrameProgress &Progress,
                           X86ModeExecutionResult &Out) {
  const uint64_t OSSA = le::read64le(TCS.Bytes.data() + 16);
  const uint64_t FrameSize = uint64_t(FramePages) * PageSize;
  if (!FrameSize || FrameSize > Enclave.Size ||
      OSSA > Enclave.Size - FrameSize ||
      Index > (Enclave.Size - FrameSize - OSSA) / FrameSize) {
    Out.Exception = X86MemoryException{13, 0, {}};
    return false;
  }
  const uint64_t SSA = Enclave.Base + OSSA + Index * FrameSize;
  const auto BindPage =
      [&](uint64_t Address,
          std::optional<uint64_t> &Saved) -> llvm::Expected<bool> {
    if (Saved)
      return true;
    uint64_t Physical = 0;
    auto Ready =
        walkOperand(M, {Address, MemoryAccessKind::Write, 3}, Out, Physical);
    if (!Ready)
      return Ready.takeError();
    if (!*Ready)
      return false;
    auto &Pages = M.sgxMemory()->EPC;
    const auto I = Pages.find(Physical & ~(uint64_t(PageSize) - 1));
    if (I != Pages.end()) {
      const auto &P = I->second;
      if (P.Valid && !P.Blocked && !P.Modified && !P.Pending &&
          P.Type == X86SgxPageType::Regular && P.OwnerSECS == Enclave.SECS &&
          P.Linear == (Address & ~(uint64_t(PageSize) - 1)) &&
          hasPermission(P.Permissions, MemoryPermission::Write)) {
        Saved = P.Physical;
        return true;
      }
    }
    Out.Exception = X86MemoryException{14, 0x8007, Address};
    return false;
  };
  auto First = BindPage(SSA, Progress.XSavePage);
  if (!First)
    return First.takeError();
  if (!*First)
    return false;
  if (FramePages == 1)
    Progress.GprPage = Progress.XSavePage;
  return BindPage(SSA + FrameSize - PageSize + GprOffset, Progress.GprPage);
}

llvm::Expected<std::optional<uint8_t>> X86SgxExecution::instructionException(
    llvm::ArrayRef<uint8_t> Bytes, uint64_t PC, const X86PagedMemory &Pages) {
  const auto &Memory = *Pages.sgxMemory();
  if (!Memory.Running)
    return invalid("enclave instruction admission requires a running thread");
  // These spans fault during ordinary code64 execution. The paged owner
  // authenticates the bytes and resolves fetch faults before enclave exit.
  const auto Fault = x86PagedCode64FaultEncoding(Bytes.data(), Bytes.size());
  if (Fault && Fault->Size == Bytes.size())
    return std::optional<uint8_t>{};
  const uint64_t XFRM = Memory.Running->XFRM;
  Decoder D;
  if (!D.init(Arch::X64))
    return invalid("software enclave instruction decoder unavailable");
  DecodedInsn I{};
  if (D.decodeOne(Bytes.data(), Bytes.size(), PC, I) <= 0 ||
      I.Size != Bytes.size())
    return invalid(
        "software enclave requires an exact original instruction extent");
  const auto &Detail = *I.Raw->detail;
  const auto &X = Detail.x86;
  const auto Groups = llvm::ArrayRef(Detail.groups, Detail.groups_count);
  for (const auto Group : Groups) {
    if (Group == X86_GRP_AVX512)
      return uint8_t{6};
    if (!(XFRM & 4) && (Group == X86_GRP_AVX || Group == X86_GRP_AVX2 ||
                        Group == X86_GRP_F16C || Group == X86_GRP_FMA))
      return uint8_t{6};
  }
  // Vector AES and polynomial multiply encodings can lack an AVX group.
  // Scalar BMI instructions also use VEX and do not require vector state.
  if (X.opcode[0] == 0xc4 || X.opcode[0] == 0xc5 || X.opcode[0] == 0x62)
    for (const auto &Operand : llvm::ArrayRef(X.operands, X.op_count)) {
      const auto Reg = Operand.type == X86_OP_REG   ? Operand.reg
                       : Operand.type == X86_OP_MEM ? Operand.mem.index
                                                    : X86_REG_INVALID;
      if (Reg >= X86_REG_K0 && Reg <= X86_REG_K7)
        return uint8_t{6};
      if (Reg >= X86_REG_XMM0 && Reg <= X86_REG_ZMM31 &&
          (X.opcode[0] == 0x62 || !(XFRM & 4)))
        return uint8_t{6};
    }
  switch (I.Id) {
  case X86_INS_RDTSC:
  case X86_INS_RDTSCP:
    if (!Memory.Features.SGX2)
      return uint8_t{6};
    if (Pages.controls().CR4 & 4) // CR4.TSD in a CPL3 enclave.
      return uint8_t{13};
    return std::optional<uint8_t>{};
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
  case X86_INS_LDS:
  case X86_INS_LES:
  case X86_INS_LFS:
  case X86_INS_LGS:
  case X86_INS_LSS:
  case X86_INS_SYSCALL:
  case X86_INS_SYSENTER:
    return uint8_t{6};
  case X86_INS_INT1:
  case X86_INS_INT3:
    return uint8_t(Memory.Running->DebugOptIn ? (I.Id == X86_INS_INT1 ? 1 : 3)
                                              : 6);
  case X86_INS_MOV:
  case X86_INS_POP: {
    const auto &X = I.Raw->detail->x86;
    if (!X.op_count || X.operands[0].type != X86_OP_REG)
      return std::optional<uint8_t>{};
    const auto Reg = X.operands[0].reg;
    if (Reg == X86_REG_DS || Reg == X86_REG_ES || Reg == X86_REG_SS ||
        Reg == X86_REG_FS || Reg == X86_REG_GS)
      return uint8_t{6};
    return std::optional<uint8_t>{};
  }
  default:
    return std::optional<uint8_t>{};
  }
}

void X86SgxExecution::retireEnclaveLeaf(X86ModeState &S, unsigned Size,
                                        X86ModeExecutionResult &Out) {
  auto &R = modeRegisters(S);
  R.PC += Size;
  R.RFlags &= ~uint64_t{0x10000};
  Out.InstructionCompleted = true;
  if (x86InstructionDebugStep(R.RFlags & 0x100, S.Debug ? S.Debug->DebugCTL : 0,
                              false)) {
    x86PublishInstructionDebug(*S.Debug, true);
    Out.Exception = X86MemoryException{1, 0, {}};
  }
}

llvm::Expected<X86ModeExecutionResult>
X86SgxExecution::instruction(X86ModeState &S, X86PagedMemory &M, bool Fetched,
                             llvm::ArrayRef<uint8_t> ExpectedBytes,
                             std::optional<uint8_t> InstructionException,
                             X86SgxInstructionProgress &Progress,
                             uint32_t MXCSRMask, X86EventKind &FaultKind) {
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
  if ((R.RFlags & 0x100) && !S.Debug)
    return invalid("software enclave transfer with TF requires its debug bank");
  if (!Fetched) {
    auto OK =
        fetchX86SystemInstruction(S, M, ExpectedBytes, {}, Out, FaultKind);
    if (!OK)
      return OK.takeError();
    if (!*OK)
      return Out;
  }
  if (InstructionException)
    return Fault(*InstructionException);
  if (!EPC.enabled() || !(C.CR0 & 1) || (R.RFlags & 0x20000) ||
      (S.SMM && S.SMM->Active))
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
    const bool TF = T.DebugOptIn ? bool(R.RFlags & 0x100) : T.OutsideTF;
    if (TF && !S.Debug)
      return invalid("single-stepping EEXIT requires the CPU debug bank");
    restoreBases(V, T);
    R.PC = R.GPR[3];
    R.GPR[1] = T.AEP;
    R.RFlags = (R.RFlags & ~uint64_t(0x10100)) | (uint64_t(TF) << 8);
    if (TF) {
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
  if (Leaf == 9 && EPC.Features.DecrementSSA) {
    if (!EPC.Running || !EPC.Active)
      return Fault(13);
    auto &T = *EPC.Running;
    auto &TCS = EPC.EPC.at(T.TCS);
    const auto CSSA = le::read32le(TCS.Bytes.data() + 24);
    if (!CSSA)
      return Fault(13);
    const auto &SECS = EPC.EPC.at(EPC.Active->SECS);
    auto Ready =
        bindFrame(M, *EPC.Active, TCS, le::read32le(SECS.Bytes.data() + 16),
                  CSSA - 1, Progress.Frame, Out);
    if (!Ready)
      return Ready.takeError();
    if (!*Ready)
      return Out;
    T.XSavePage = *Progress.Frame.XSavePage;
    T.GprPage = *Progress.Frame.GprPage;
    le::write32le(TCS.Bytes.data() + 24, CSSA - 1);
    ++EPC.Generation;
    retireEnclaveLeaf(S, ExpectedBytes.size(), Out);
    return Out;
  }
  if (Leaf >= 5 && Leaf <= 7 && EPC.Features.SGX2)
    return dynamicMemory(S, M, Progress, ExpectedBytes.size(), std::move(Out));
  const auto Op = static_cast<KeyReportLeaf>(Leaf);
  if (Op == KeyReportLeaf::Report || Op == KeyReportLeaf::Key ||
      (EPC.Features.SGX256 &&
       (Op == KeyReportLeaf::Report2 || Op == KeyReportLeaf::Key256)))
    return keyOrReport(Op, S, M, Progress, ExpectedBytes.size(),
                       std::move(Out));
  if (Leaf > 4)
    return Fault(13);
  if (EPC.Active)
    return Fault(13);
  if (!EPC.X87)
    return invalid(
        "software SGX1 requires an explicit x87/SSE processor image");
  const uint64_t TCSLinear = R.GPR[3], AEP = R.GPR[1];
  if ((TCSLinear & (PageSize - 1)) || !canonical(TCSLinear, Bits))
    return Fault(13);

  uint64_t TCSPhysical = 0;
  if (Progress.TCSPhysical)
    TCSPhysical = *Progress.TCSPhysical;
  else {
    auto Ready = walkOperand(M, {TCSLinear, MemoryAccessKind::Write, 3}, Out,
                             TCSPhysical);
    if (!Ready)
      return Ready.takeError();
    if (!*Ready)
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
  const uint32_t FramePages = le::read32le(SECS + 16);
  const auto XFRM = le::read64le(SECS + 56);
  if ((XFRM & 3) != 3 || (XFRM & ~EPC.xcr0()) ||
      (XFRM != 3 && !(C.CR4 & (1u << 18))))
    return Fault(13);
  const auto MiscSelect = le::read32le(SECS + 20);
  if (!FramePages || (MiscSelect & ~1u) ||
      (Attributes & ~EPC.Features.allowedAttributes()))
    return invalid("unsupported initialized SECS attributes or SSA geometry");
  if (!std::has_single_bit(Enclave.Size) || Enclave.Size < 2 * PageSize ||
      (Enclave.Base & (Enclave.Size - 1)) || !Enclave.contains(TCSLinear))
    return invalid("TCS disagrees with the imported SECS address range");
  uint8_t *Thread = TCS->Bytes.data();
  const auto TcsFlags = le::read64le(Thread + 8);
  if (TcsFlags & ~uint64_t(3))
    return Fault(13);
  const bool DebugOptIn = TcsFlags & 1;
  if (DebugOptIn && !(Attributes & 2))
    return invalid("a debug opt-in TCS requires an imported debug enclave");
  const bool NotifyThread = TcsFlags & 2;
  if ((NotifyThread && !EPC.Features.AEXNotify) ||
      (!DebugOptIn && NotifyThread != bool(Attributes & X86SgxFeatures::Notify)))
    return Fault(13);
  const bool Resume = Leaf == 3;
  const auto CSSA = le::read32le(Thread + 24), NSSA = le::read32le(Thread + 28);
  if (le::read64le(Thread) || (Resume ? !CSSA || CSSA > NSSA : CSSA >= NSSA))
    return Fault(13);
  const uint64_t OSSA = le::read64le(Thread + 16);
  const uint64_t FSOffsetInEnclave = le::read64le(Thread + 48),
                 GSOffsetInEnclave = le::read64le(Thread + 56);
  if ((OSSA | FSOffsetInEnclave | GSOffsetInEnclave) & (PageSize - 1))
    return Fault(13);
  auto *Frame = &Progress.Frame;
  auto Ready = bindFrame(M, Enclave, *TCS, FramePages, Resume ? CSSA - 1 : CSSA,
                         *Frame, Out);
  if (!Ready)
    return Ready.takeError();
  if (!*Ready)
    return Out;
  uint8_t *GPR = EPC.EPC.at(*Frame->GprPage).Bytes.data() + GprOffset;
  const bool Notify = Resume && NotifyThread && (GPR[167] & 1);
  if (Notify) {
    if (CSSA >= NSSA)
      return Fault(13);
    Frame = &Progress.NotificationFrame;
    auto Next = bindFrame(M, Enclave, *TCS, FramePages, CSSA, *Frame, Out);
    if (!Next)
      return Next.takeError();
    if (!*Next)
      return Out;
    GPR = EPC.EPC.at(*Frame->GprPage).Bytes.data() + GprOffset;
  }
  uint8_t *FX = EPC.EPC.at(*Frame->XSavePage).Bytes.data();
  const bool Restore = Resume && !Notify;
  const auto Target = Restore ? le::read64le(GPR + IpOffset)
                              : Enclave.Base + le::read64le(Thread + 32);
  const auto FS =
      Restore ? le::read64le(GPR + FSOffset) : Enclave.Base + FSOffsetInEnclave;
  const auto GS =
      Restore ? le::read64le(GPR + GSOffset) : Enclave.Base + GSOffsetInEnclave;
  if (!canonical(Target, Bits) || !canonical(FS, Bits) || !canonical(GS, Bits))
    return Fault(13);
  if (Restore &&
      ((le::read64le(FX + 512) & ~XFRM) ||
       std::any_of(FX + 520, FX + 576, [](uint8_t B) { return B != 0; }) ||
       (le::read32le(FX + 24) & ~MXCSRMask)))
    return Fault(13);
  EPC.Running = X86SgxMemory::Thread{
      TCSPhysical,
      TCSLinear,
      *Frame->XSavePage,
      *Frame->GprPage,
      AEP,
      XFRM,
      {R.FSBase, V.FS.Limit, V.FS.Selector, V.FS.Attributes},
      {R.GSBase, V.GS.Limit, V.GS.Selector, V.GS.Attributes},
      uint8_t(V.Unusable & 0x30),
      bool(R.RFlags & 0x100),
      bool(C.CR4 & (1u << 18)),
      DebugOptIn};
  if (Restore) {
    for (unsigned I = 0; I != 16; ++I)
      R.GPR[I] = le::read64le(GPR + I * 8);
    const uint32_t Mask =
        0x254cd5u | ((R.RFlags & 0x3000) == 0x3000 ? 0x200u : 0u);
    R.RFlags = (R.RFlags & ~uint64_t(Mask | (DebugOptIn ? 0 : 0x100))) |
               (le::read64le(GPR + FlagsOffset) & Mask);
    const auto XStateBV = le::read64le(FX + 512);
    if (XStateBV & 1)
      std::copy_n(FX, EPC.X87->size(), EPC.X87->begin());
    else {
      EPC.X87->fill(0);
      le::write16le(EPC.X87->data(), 0x37f);
    }
    // Standard XRSTOR loads MXCSR even when SSE is in its initial state.
    V.Core.Core.Core.MXCSR = le::read32le(FX + 24);
    for (unsigned Part = 0; Part != (XFRM & 4 ? 2u : 1u); ++Part) {
      const auto Offset = Part ? 576 : 160;
      for (unsigned I = 0; I != 16; ++I) {
        auto *Dst = R.ZMM[I] + Part * 16;
        if (XStateBV & (2u << Part))
          std::copy_n(FX + Offset + I * 16, 16, Dst);
        else
          std::fill_n(Dst, 16, 0);
      }
    }
    le::write32le(Thread + 24, CSSA - 1);
  } else {
    le::write64le(GPR + UserSPOffset, R.GPR[4]);
    le::write64le(GPR + UserBPOffset, R.GPR[5]);
    R.GPR[1] = R.PC + ExpectedBytes.size();
    R.GPR[0] = CSSA;
    R.RFlags &= ~uint64_t(DebugOptIn ? 0x10000 : 0x10100);
    if (DebugOptIn && (R.RFlags & 0x100)) {
      x86PublishInstructionDebug(*S.Debug, true);
      Out.Exception = X86MemoryException{1, 0, {}};
    }
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
  for (unsigned I = 0; I != 16; ++I) {
    std::copy_n(R.ZMM[I], 16, FX + 160 + I * 16);
    if (T.XFRM & 4)
      std::copy_n(R.ZMM[I] + 16, 16, FX + 576 + I * 16);
  }
  le::write64le(FX + 512, T.XFRM);
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
    std::fill_n(R.ZMM[I], T.XFRM & 4 ? 32u : 16u, 0);
  std::fill(std::begin(R.GPR), std::end(R.GPR), 0);
  R.GPR[0] = 3;
  R.GPR[1] = T.AEP;
  R.GPR[3] = T.TCSLinear;
  R.GPR[4] = le::read64le(GPR + UserSPOffset);
  R.GPR[5] = le::read64le(GPR + UserBPOffset);
  const bool TF = T.DebugOptIn ? bool(R.RFlags & 0x100) : T.OutsideTF;
  R.RFlags = (R.RFlags & ~uint64_t(0x109d5)) | (uint64_t(TF) << 8);
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
