//===- RegistrationCookieABITests.cpp - Checked PE32 cookie ABI ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep finally register exchanges and inferred prototypes out of GS checks.

#include "../../../lib/ir/med/abi/MedABIPassDetail.h"
#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsRegistrationFrame.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/X86/X86RegistrationCall.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/IR/Verifier.h"

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <tuple>

namespace {
using namespace neverd;

MedVar reg(int Id, uint64_t Offset) {
  MedVar Value;
  Value.Kind = MedVar::Reg;
  Value.Id = Id;
  Value.SSAVer = 1;
  Value.RegOff = Offset;
  Value.Size = 4;
  Value.TheArch = Arch::X86;
  return Value;
}

struct CookieFixture {
  BinaryImage Image;
  MedFunc Function;
  MedVar Cookie = reg(1, x86reg::RCX);

  CookieFixture() {
    Image.Arch = Arch::X86;
    Image.Format = BinaryFormat::COFF;
    Function.Entry = 0x1000;
    auto &EH = Function.ExceptionMetadata.emplace();
    EH.ParseStatus = ExceptionParseStatus::Complete;
    EH.Encoding = ExceptionEncoding::X86ScopeTableEH4;
    EH.Personality = ExceptionPersonality::ExceptHandler4;
    EH.CodeRange = {0x1000, 0x1020};
    EH.Registration.emplace();
    auto &States = Function.RegistrationStates.emplace();
    States.SecurityCookiesComplete = true;
    States.CookieCheckVA = 0x2000;
    States.CookieChecks.push_back({0x1010, 0x1015, 3});
    auto &Block = Function.Blocks.emplace_back();
    Block.Id = 0;
    Block.StartAddr = 0x1000;
    Block.EndAddr = 0x1020;
    MedOp Decode;
    Decode.Opcode = NdOp::INT_XOR;
    Decode.Output = Cookie;
    Decode.addInput(MedVar::makeConst(0x12345678, 4));
    Decode.addInput(reg(4, x86reg::RBP));
    Block.Ops.push_back(Decode);
    // Finally register transport makes EDX live even when the checker has no
    // second argument. A private spill and an XMM value are incidental too.
    for (auto Offset : {x86reg::RDX, x86reg::XMM0}) {
      MedOp Copy;
      Copy.Opcode = NdOp::COPY;
      Copy.Output = reg(2 + Block.Ops.size(), Offset);
      Copy.addInput(MedVar::makeConst(99, 4));
      Block.Ops.push_back(Copy);
    }
    MedOp Spill;
    Spill.Opcode = NdOp::STORE;
    Spill.addInput(reg(5, x86reg::RSP));
    Spill.addInput(MedVar::makeConst(88, 4));
    Block.Ops.push_back(Spill);
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = 0x1010;
    Call.OriginSeq = 3;
    Call.addInput(MedVar::makeConst(0x2000, 4));
    Block.Ops.push_back(Call);
  }

  void recover() {
    std::map<va_t, int> RegisterArity{{0x2000, 2}};
    std::map<va_t, int> TotalArity{{0x2000, 3}};
    std::map<va_t, int> FPArity{{0x2000, 1}};
    std::map<va_t, std::vector<uint64_t>> RegisterOrder{
        {0x2000, {x86reg::RDX, x86reg::RCX}}};
    recoverCallAbi(Function, Arch::X86, {}, &Image, &RegisterArity, &TotalArity,
                   &FPArity, nullptr, nullptr, nullptr, nullptr, nullptr,
                   nullptr, &RegisterOrder);
  }

  void useCxxCookies(unsigned Count) {
    auto &EH = *Function.ExceptionMetadata;
    EH.Encoding = ExceptionEncoding::X86CxxFuncInfo;
    EH.Personality = ExceptionPersonality::CxxFrameHandler3;
    EH.Cxx.emplace();
    auto &States = *Function.RegistrationStates;
    States.SecurityCookieVA = 0x4000;
    for (unsigned I = 0; I != Count; ++I)
      States.CxxCookieConditions.push_back({-20 - int32_t(I) * 4, 0});
  }

  void addPhysicalChecker() {
    Image.Bits = Bitness::Bits32;
    Image.DynInfo.SecurityCookieRVA = 0x4000;
    Function.Name = "cookie_parent";
    Function.RegistrationStates->SecurityCookieVA = 0x4000;
    Segment Text;
    Text.VA = 0x1000;
    Text.Size = Text.FileSz = 0x1100;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(Text.Size, 0xcc);
    Text.Data[0x10] = 0xe8;
    writeLE<uint32_t>(Text.Data.data() + 0x11, 0x2000 - 0x1015);
    const uint8_t Checker[] = {0x3b, 0x0d, 0, 0x40, 0, 0, 0x75, 0x10, 0xc3};
    std::copy(std::begin(Checker), std::end(Checker),
              Text.Data.begin() + 0x1000);
    Image.Segments.push_back(std::move(Text));
    Section Code;
    Code.VA = 0x1000;
    Code.Size = Code.FileSz = 0x1100;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Image.Sections.push_back(Code);
    Segment Data;
    Data.VA = 0x4000;
    Data.Size = Data.FileSz = 4;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(4);
    writeLE<uint32_t>(Data.Data.data(), 0x12345678);
    Image.Segments.push_back(std::move(Data));
    Image.BaseRelocations.push_back(
        {0x2002, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
    auto &Block = Function.Blocks.front();
    Block.Ops.back().PreservesCallerSaved = true;
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = 0x1015;
    Return.OriginSeq = 0;
    Return.addInput(MedVar::makeConst(7, 4));
    Block.Ops.push_back(Return);
  }
};

void checkCookieArgument(const CookieFixture &Fixture) {
  ASSERT_EQ(Fixture.Function.CallInfos.size(), 1u);
  const auto &Call = Fixture.Function.CallInfos.front();
  ASSERT_EQ(Call.Args.size(), 1u);
  EXPECT_EQ(Call.Args.front(), Fixture.Cookie);
  EXPECT_EQ(Call.IntegerRegisterArgCount, 1);
  EXPECT_EQ(Call.FloatingRegisterArgCount, 0);
  EXPECT_TRUE(Call.StackArgumentOffsets.empty());
}

TEST(RegistrationCookieABI, CheckedECXOverridesIncidentalAndInferredArguments) {
  CookieFixture Fixture;
  for (unsigned Pass = 0; Pass != 2; ++Pass) {
    Fixture.recover();
    checkCookieArgument(Fixture);
  }
}

TEST(RegistrationCookieABI, RetainedCheckerUsesPhysicalFastcallAndVoidResult) {
  for (bool Cxx : {false, true}) {
    SCOPED_TRACE(Cxx);
    CookieFixture Fixture;
    if (Cxx)
      Fixture.useCxxCookies(1);
    Fixture.addPhysicalChecker();
    Fixture.recover();
    const auto ABI = registrationCookieCallABI(
        Fixture.Image, Fixture.Function, Fixture.Function.Blocks.front(), 4);
    ASSERT_TRUE(ABI);
    EXPECT_EQ(ABI->Argument, Fixture.Cookie);
    EXPECT_EQ(ABI->CheckVA, 0x2000u);
    EXPECT_EQ(ABI->SecurityCookieVA, 0x4000u);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Fixture.Function}, Context, "cookie", Arch::X86,
                              {}, &Fixture.Image, BinaryFormat::COFF);
    ASSERT_TRUE(Module);
    EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    const auto *Checker = Module->getFunction("__nd_registration_cookie_2000");
    ASSERT_TRUE(Checker);
    EXPECT_TRUE(hasX86RegistrationSecurityCheckABI(*Checker));
    auto Identity = rewrite_source::getOriginalVA(*Checker);
    ASSERT_TRUE(bool(Identity)) << llvm::toString(Identity.takeError());
    ASSERT_TRUE(*Identity);
    EXPECT_EQ(**Identity, 0x2000u);
    ASSERT_TRUE(Checker->hasOneUse());
    const auto *Call = llvm::dyn_cast<llvm::CallInst>(*Checker->user_begin());
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->getCallingConv(), llvm::CallingConv::X86_FastCall);
    EXPECT_TRUE(Call->getType()->isVoidTy());
    ASSERT_EQ(Call->arg_size(), 1u);
    EXPECT_TRUE(Call->paramHasAttr(0, llvm::Attribute::InReg));
  }
}

TEST(RegistrationCookieABI, PhysicalCheckerRechecksCurrentImageAndOccurrence) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CookieFixture Fixture;
    Fixture.useCxxCookies(1);
    Fixture.addPhysicalChecker();
    auto &States = *Fixture.Function.RegistrationStates;
    auto &Block = Fixture.Function.Blocks.front();
    ASSERT_TRUE(
        registrationCookieCallABI(Fixture.Image, Fixture.Function, Block, 4));
    switch (Mutation) {
    case 0:
      ++Fixture.Image.Segments.front().Data[0x11];
      break;
    case 1:
      Fixture.Image.Segments.front().Data[0x1006] = 0x74;
      break;
    case 2:
      ++Fixture.Image.Segments.front().Data[0x1002];
      break;
    case 3:
      Fixture.Image.BaseRelocations.front().Address = 0x2000;
      break;
    case 4:
      Fixture.Image.Segments.front().Flags = SegmentFlags::Readable |
                                             SegmentFlags::Writable |
                                             SegmentFlags::Executable;
      break;
    case 5:
      ++States.CookieChecks.front().EndAddress;
      break;
    case 6:
      ++Block.Ops[4].OriginSeq;
      break;
    case 7:
      ++States.SecurityCookieVA;
      break;
    case 8:
      Block.Ops.push_back(Block.Ops[4]);
      break;
    case 9:
      Fixture.Image.BaseRelocations.push_back(
          {0x1011, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
      break;
    }
    EXPECT_FALSE(
        registrationCookieCallABI(Fixture.Image, Fixture.Function, Block, 4));
  }
}

TEST(RegistrationCookieABI, ChangedPhysicalCookieCallHasNoGenericFallback) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CookieFixture Fixture;
    Fixture.addPhysicalChecker();
    Fixture.recover();
    switch (Mutation) {
    case 0:
      Fixture.Function.CallInfos.front().Args.front() =
          MedVar::makeConst(99, 4);
      break;
    case 1:
      Fixture.Image.Segments.front().Data[0x1006] = 0x74;
      break;
    case 2:
      Fixture.Function.Blocks.front().Ops[4].PreservesCallerSaved = false;
      break;
    case 3:
      Fixture.Function.Blocks.front().Ops[4].Output = reg(9, x86reg::RAX);
      break;
    case 4:
      ++Fixture.Function.CallInfos.front().TargetAddr;
      break;
    case 5:
      Fixture.Function.CallInfos.front().IsIndirect = true;
      break;
    }
    llvm::LLVMContext Context;
    EXPECT_FALSE(MedLLVMEmitter().emit({Fixture.Function}, Context,
                                       "changed-cookie", Arch::X86, {},
                                       &Fixture.Image, BinaryFormat::COFF));
  }
}

TEST(RegistrationCookieABI, SuccessEffectsReachSSAWithEveryVolatileGPRIntact) {
  for (bool Proven : {false, true}) {
    SCOPED_TRACE(Proven);
    LowFunc Low;
    Low.Entry = 0x1000;
    auto &State = Low.RegistrationStates.emplace();
    State.SecurityCookiesComplete = Proven;
    State.CookieCheckVA = 0x2000;
    State.CookieChecks.push_back({0x1010, 0x1015, 0});
    auto &Block = Low.Blocks.emplace_back();
    Block.Id = 0;
    Block.StartAddr = 0x1000;
    Block.EndAddr = 0x1021;
    auto Emit = [&](va_t Address, NdOp Opcode, NdVar Output,
                    std::initializer_list<NdVar> Inputs) {
      LowOp Op;
      Op.Addr = Address;
      Op.Seq = 0;
      Op.Opcode = Opcode;
      Op.Output = Output;
      for (const auto &Input : Inputs)
        Op.Inputs[Op.NumInputs++] = Input;
      Block.Ops.push_back(Op);
    };
    const auto AX = NdVar::reg(x86reg::RAX, 4);
    const auto CX = NdVar::reg(x86reg::RCX, 4);
    const auto DX = NdVar::reg(x86reg::RDX, 4);
    Emit(0x1000, NdOp::COPY, AX, {NdVar::scalar(11, 4)});
    Emit(0x1000, NdOp::COPY, CX, {NdVar::scalar(13, 4)});
    Emit(0x1000, NdOp::COPY, DX, {NdVar::scalar(17, 4)});
    for (uint64_t Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                          x86reg::SF, x86reg::OF})
      Emit(0x1000, NdOp::COPY, NdVar::reg(Flag, 1),
           {NdVar::scalar(Flag != x86reg::PF && Flag != x86reg::ZF, 1)});
    Emit(0x1000, NdOp::COPY, NdVar::reg(x86reg::DF, 1), {NdVar::scalar(1, 1)});
    Emit(0x1010, NdOp::CALL, AX, {NdVar::codeAddress(0x2000, 4)});
    Emit(0x1015, NdOp::INT_ADD, AX, {AX, CX});
    Emit(0x1015, NdOp::INT_ADD, AX, {AX, DX});
    unsigned NextTemp = 0;
    for (uint64_t Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                          x86reg::SF, x86reg::OF, x86reg::DF}) {
      const auto Wide = NdVar::tmp(NextTemp++, 4);
      Emit(0x1015, NdOp::INT_ZEXT, Wide, {NdVar::reg(Flag, 1)});
      Emit(0x1015, NdOp::INT_ADD, AX, {AX, Wide});
    }
    Emit(0x1020, NdOp::RETURN, {}, {AX});
    LowToMedConverter Converter;
    const auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
    const MedOp *Call = nullptr, *Return = nullptr;
    for (const auto &B : Med.Blocks)
      for (const auto &Op : B.Ops) {
        if (Op.Opcode == NdOp::CALL)
          Call = &Op;
        if (Op.Opcode == NdOp::RETURN)
          Return = &Op;
      }
    ASSERT_TRUE(Call && Return);
    EXPECT_EQ(Call->PreservesCallerSaved, Proven);
    if (Proven) {
      EXPECT_EQ(Call->Output.Size, 0u);
      EXPECT_TRUE(Med.CallClobbers.empty());
      ASSERT_EQ(Return->NumInputs, 1u);
      // Conversion need not fold arithmetic. Evaluate the surviving scalar
      // SSA expression rather than requiring a particular optimized shape.
      using Key = std::tuple<int, int, int>;
      auto KeyOf = [](const MedVar &V) {
        return Key{int(V.Kind), V.Id, V.SSAVer};
      };
      std::map<Key, std::optional<uint64_t>> Values;
      auto ValueOf = [&](const MedVar &V) -> std::optional<uint64_t> {
        if (V.isConst())
          return V.ConstVal;
        const auto It = Values.find(KeyOf(V));
        return It == Values.end() ? std::nullopt : It->second;
      };
      for (const auto &B : Med.Blocks)
        for (const auto &Op : B.Ops) {
          if (Op.Output.Size == 0)
            continue;
          LowOp Scalar;
          Scalar.Opcode = Op.Opcode;
          Scalar.Output = NdVar::tmp(0, Op.Output.Size);
          for (unsigned I = 0; I != Op.NumInputs; ++I)
            Scalar.Inputs[Scalar.NumInputs++] =
                NdVar::tmp(I, Op.Inputs[I].Size);
          Values[KeyOf(Op.Output)] =
              lowScalarConstantResult(Scalar, [&](const NdVar &V) {
                return ValueOf(Op.Inputs[V.Offset]);
              });
        }
      EXPECT_EQ(ValueOf(Return->Inputs[0]), std::optional<uint64_t>{44});
    } else
      EXPECT_FALSE(Med.CallClobbers.empty());
  }
}

TEST(RegistrationCookieABI, UsesCurrentCookiePhiAcrossAnEHBoundary) {
  CookieFixture Fixture;
  auto &Block = Fixture.Function.Blocks.front();
  Block.Ops.erase(Block.Ops.begin());
  PhiNode Phi;
  Phi.Output = Fixture.Cookie;
  Phi.Args = {{1, reg(6, x86reg::RCX)}, {2, reg(7, x86reg::RCX)}};
  Block.Phis.push_back(Phi);
  Block.Preds = {1, 2};
  Fixture.recover();
  checkCookieArgument(Fixture);
}

TEST(RegistrationCookieABI, CxxCookieReceiptsKeepTheSameSingleECXProtocol) {
  for (unsigned Count : {1u, 2u}) {
    SCOPED_TRACE(Count);
    CookieFixture Fixture;
    Fixture.useCxxCookies(Count);
    for (unsigned Pass = 0; Pass != 2; ++Pass) {
      Fixture.recover();
      checkCookieArgument(Fixture);
    }
  }
}

TEST(RegistrationCookieABI, CxxCallsNeedCompleteCookieAndPersonalityEvidence) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CookieFixture Fixture;
    Fixture.useCxxCookies(1);
    auto &Function = Fixture.Function;
    auto &States = *Function.RegistrationStates;
    switch (Mutation) {
    case 0:
      Function.ExceptionMetadata->Cxx.reset();
      break;
    case 1:
      Function.ExceptionMetadata->Personality =
          ExceptionPersonality::CxxFrameHandler4;
      break;
    case 2:
      States.CxxCookieConditions.clear();
      break;
    case 3:
      States.CxxCookieConditions.resize(3);
      break;
    case 4:
      States.SecurityCookieVA = 0;
      break;
    case 5:
      States.SecurityCookieVA = UINT32_MAX - 2;
      break;
    case 6:
      States.SecurityCookiesComplete = false;
      break;
    case 7:
      States.CookieChecks.clear();
      break;
    }
    const auto &Block = Function.Blocks.front();
    MedCallInfo Info;
    EXPECT_FALSE(
        recoverI386RegistrationCookieCall(Function, Block, Block.Ops.size() - 1,
                                          getTargetRegInfo(Arch::X86), Info));
  }
}

TEST(RegistrationCookieABI, UncheckedOccurrencesDoNotAcquireTheCookieProtocol) {
  for (unsigned Mutation = 0; Mutation != 18; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CookieFixture Fixture;
    auto &Function = Fixture.Function;
    auto &Block = Function.Blocks.front();
    auto &Call = Block.Ops.back();
    auto &States = *Function.RegistrationStates;
    switch (Mutation) {
    case 0:
      States.SecurityCookiesComplete = false;
      break;
    case 1:
      States.CookieChecks.clear();
      break;
    case 2:
      Call.Addr += 1;
      break;
    case 3:
      Call.OriginSeq += 1;
      break;
    case 4:
      Call.Inputs[0].ConstVal += 1;
      break;
    case 5:
      Call.Inputs[0].Size = 8;
      break;
    case 6:
      Call.Opcode = NdOp::INDIR_CALL;
      break;
    case 7:
      Call.addInput(Fixture.Cookie);
      break;
    case 8:
      Call.DoesNotReturn = true;
      break;
    case 9:
      Call.Dead = true;
      break;
    case 10:
      States.CookieChecks.push_back(States.CookieChecks.front());
      break;
    case 11:
      Block.Ops.insert(Block.Ops.begin(), Call);
      break;
    case 12:
      States.CookieChecks.front().EndAddress = Call.Addr;
      break;
    case 13:
      States.CookieChecks.front().EndAddress = Block.EndAddr + 1;
      break;
    case 14:
      Function.ExceptionMetadata->CodeRange.End = Call.Addr;
      break;
    case 15:
      Function.ExceptionMetadata->Encoding = ExceptionEncoding::X86CxxFuncInfo;
      break;
    case 16:
      Function.ExceptionMetadata->ParseStatus = ExceptionParseStatus::Partial;
      break;
    case 17:
      Function.SkippedSSA = true;
      break;
    }
    MedCallInfo Info;
    EXPECT_FALSE(
        recoverI386RegistrationCookieCall(Function, Block, Block.Ops.size() - 1,
                                          getTargetRegInfo(Arch::X86), Info));
  }
}

} // namespace
