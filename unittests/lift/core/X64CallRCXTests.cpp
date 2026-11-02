//===- X64CallRCXTests.cpp - Native call result transport ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Static IR/source checks. No original or generated native code is run.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/render/X86/X64CallRCX.h"
#include "../../../lib/pipeline/X86/X64CallRCX.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {
using namespace neverd;
BinaryImage image() {
  BinaryImage I;
  I.Arch = Arch::X64;
  I.Bits = Bitness::Bits64;
  I.Format = BinaryFormat::COFF;
  I.Base = I.Entry = 0x1000;
  Segment S;
  S.VA = 0x2000;
  S.Size = 1;
  S.Data = {0xc3};
  S.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  I.Segments.push_back(S);
  auto Symbol = Symbol::makeFunc(0x2000, 1);
  Symbol.Name = "native_target";
  I.Symbols.push_back(Symbol);
  return I;
}
MedVar reg(int Id, uint64_t Offset) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = Id;
  V.Size = 8;
  V.TheArch = Arch::X64;
  V.RegOff = Offset;
  V.SSAVer = 1;
  return V;
}
MedFunc caller() {
  MedFunc F;
  F.Entry = 0x1000;
  F.Name = "capture_rcx";
  F.CC = CallingConv::Win64;
  F.ReturnType = NdType::makeInt(8, false);
  F.Blocks.resize(1);
  auto &B = F.Blocks.front();
  B.Id = 0;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Output = reg(1, x86reg::RAX);
  Call.CallSiteId = 1;
  Call.Addr = 0x1008;
  Call.NumInputs = 5;
  Call.Inputs.resize(5);
  Call.Inputs[0] =
      MedVar::makeConst(0x2000, 8, ConstantAddressProvenance::CodeAddress);
  for (unsigned I = 1; I < 5; ++I)
    Call.Inputs[I] =
        MedVar::makeConst(10 + I, 8, ConstantAddressProvenance::Scalar);
  B.Ops.push_back(Call);
  const auto Result = reg(2, x86reg::RCX);
  F.CallClobbers.push_back({Result, 1});
  MedOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.NumInputs = 1;
  Ret.Inputs[0] = Result;
  B.Ops.push_back(Ret);
  MedCallInfo C;
  C.BlockId = 0;
  C.OpIdx = 0;
  C.TargetAddr = 0x2000;
  C.Args.assign(Call.Inputs.begin() + 1, Call.Inputs.end());
  F.CallInfos.push_back(C);
  return F;
}

TEST(X64CallRCX, CapturesTheSameOccurrenceAndCurrentSourceArguments) {
  auto I = image();
  auto F = caller();
  F.CallInfos.front().Args[0] = MedVar::makeConst(73, 8);
  captureX64CallRCX(F, I, {}, {});
  ASSERT_TRUE(F.CallClobbers.empty());
  EXPECT_TRUE(F.CallInfos.empty());
  const auto &Capture = F.Blocks[0].Ops[0];
  EXPECT_TRUE(x64CallRCXShapeIsValid(x64CallRCXMedShape(Capture, Arch::X64)));
  EXPECT_EQ(Capture.Inputs[2].ConstVal, 73u);
  EXPECT_EQ(Capture.Inputs[1].Provenance,
            ConstantAddressProvenance::CodeAddress);
  EXPECT_EQ(Capture.Addr, 0x1008u);
  ASSERT_EQ(F.Blocks[0].Ops.size(), 3u);
  EXPECT_EQ(F.Blocks[0].Ops[1].Output, reg(2, x86reg::RCX));
  EXPECT_EQ(F.Blocks[0].Ops[1].Inputs[0], Capture.IntrinsicOutputs[0]);
  auto Truncated = Capture;
  Truncated.Inputs.resize(2);
  EXPECT_FALSE(
      x64CallRCXShapeIsValid(x64CallRCXMedShape(Truncated, Arch::X64)));
  captureX64CallRCX(F, I, {}, {});
  EXPECT_EQ(F.Blocks[0].Ops.size(), 3u);
}

TEST(X64CallRCX, MissingRegisterSlotsRequireIndependentUnreadProof) {
  for (bool Proof : {false, true}) {
    auto I = image();
    auto F = caller();
    F.CallInfos[0].Args.resize(3);
    std::map<va_t, GPRFamilyMask> Unread;
    if (Proof)
      Unread[0x2000] = GPRFamilyMask{1} << 9;
    captureX64CallRCX(F, I, Unread, {});
    EXPECT_EQ(F.CallClobbers.empty(), Proof);
  }
}

TEST(X64CallRCX, RemapsLaterCallsWithoutRebindingTheirArguments) {
  auto I = image();
  auto F = caller();
  auto Later = F.Blocks[0].Ops.front();
  Later.CallSiteId = 2;
  Later.Output = reg(30, x86reg::RAX);
  F.Blocks[0].Ops.insert(F.Blocks[0].Ops.begin() + 1, Later);
  auto Info = F.CallInfos.front();
  Info.OpIdx = 1;
  Info.Args[0] = F.CallClobbers.front().Value;
  F.CallInfos.push_back(Info);
  captureX64CallRCX(F, I, {}, {});
  ASSERT_EQ(F.CallInfos.size(), 1u);
  EXPECT_EQ(F.CallInfos[0].OpIdx, 2);
  EXPECT_EQ(F.CallInfos[0].Args, Info.Args);
  EXPECT_EQ(F.Blocks[0].Ops[2].CallSiteId, 2u);
}

TEST(X64CallRCX, PartialRegisterMaskChainsAloneDoNotAddABridge) {
  auto I = image();
  auto F = caller();
  auto Mask = F.Blocks[0].Ops.back();
  Mask.Opcode = NdOp::INT_AND;
  Mask.NumInputs = 2;
  Mask.Inputs[1] = MedVar::makeConst(~uint64_t{255}, 8);
  Mask.Output = reg(20, x86reg::RCX);
  F.Blocks[0].Ops.insert(F.Blocks[0].Ops.end() - 1, Mask);
  F.Blocks[0].Ops.back().Inputs[0] = Mask.Output;
  captureX64CallRCX(F, I, {}, {});
  EXPECT_EQ(F.Blocks[0].Ops[0].Opcode, NdOp::CALL);
}

TEST(X64CallRCX, UnsupportedCallContractsRemainExplicit) {
  for (unsigned Mode = 0; Mode < 20; ++Mode) {
    SCOPED_TRACE(Mode);
    auto I = image();
    auto F = caller();
    auto &C = F.Blocks[0].Ops[0];
    std::set<va_t> Emitted;
    switch (Mode) {
    case 0:
      I.Arch = Arch::X86;
      break;
    case 1:
      I.Format = BinaryFormat::ELF;
      break;
    case 2:
      F.SkippedSSA = true;
      break;
    case 3:
      F.AnalysisOnly = true;
      break;
    case 4:
      F.SourceParametersBound = true;
      break;
    case 5:
      C.IsTailCall = true;
      break;
    case 6:
      C.DoesNotReturn = true;
      break;
    case 7:
      F.Blocks[0].ExceptionalSuccs.push_back({-1, 0});
      break;
    case 8:
      F.CallInfos[0].IsIndirect = true;
      break;
    case 9:
      F.CallInfos[0].Args.push_back(MedVar::makeConst(1, 8));
      break;
    case 10:
      F.CallInfos[0].ArgumentsInCalleeRegisterOrder = true;
      break;
    case 11:
      F.CallInfos[0].FloatingRegisterArgCount = 1;
      break;
    case 12:
      F.CallInfos[0].TargetAddr += 1;
      break;
    case 13:
      Emitted.insert(0x2000);
      break;
    case 14:
      F.CallClobbers[0].PreservedPrefixSize = 1;
      break;
    case 15:
      C.Output.RegOff = x86reg::XMM0;
      break;
    case 16:
      F.Blocks[0].Ops.back().Inputs[0].Size = 32;
      break;
    case 17: {
      MedOp Other;
      Other.Opcode = NdOp::INTRINSIC;
      Other.NumInputs = 1;
      Other.Inputs[0] = MedVar::makeConst(uint64_t(Intrinsic::None), 2);
      F.Blocks[0].Ops.push_back(Other);
      break;
    }
    case 18:
      F.CallInfos[0].Args[0].Size = 4;
      break;
    case 19:
      F.CallInfos.push_back(F.CallInfos.front());
      break;
    }
    captureX64CallRCX(F, I, {}, Emitted);
    EXPECT_EQ(F.CallClobbers.size(), 1u);
    EXPECT_EQ(F.Blocks[0].Ops[0].Opcode, NdOp::CALL);
  }
}

TEST(X64CallRCX, DeadCopiesAndSettledSpillsDoNotRequireNativeTransport) {
  for (bool Observed : {false, true}) {
    auto I = image();
    auto F = caller();
    const auto Result = F.CallClobbers.front().Value;
    auto Copy = F.Blocks[0].Ops.back();
    Copy.Opcode = NdOp::COPY;
    Copy.Output = reg(20, x86reg::RDX);
    F.Blocks[0].Ops.back() = Copy;
    MedOp Store;
    Store.Opcode = NdOp::STORE;
    Store.NumInputs = 2;
    Store.Inputs[0] = MedVar::makeConst(0x7000, 8);
    Store.Inputs[1] = Result;
    Store.PrologueSave = !Observed;
    F.Blocks[0].Ops.push_back(Store);
    captureX64CallRCX(F, I, {}, {});
    EXPECT_EQ(F.CallClobbers.empty(), Observed);
  }
}

TEST(X64CallRCX, DeadPhiCyclesAreNotMachineObservations) {
  for (bool Live : {false, true}) {
    auto I = image();
    auto F = caller();
    auto Other = reg(8, x86reg::RDX);
    F.CallClobbers.push_back({Other, 1});
    PhiNode A, B;
    A.Output.Id = 9;
    A.Output.Size = 8;
    B.Output.Id = 10;
    B.Output.Size = 8;
    A.Args = {{0, Other}, {1, B.Output}};
    B.Args = {{0, A.Output}};
    F.Blocks[0].Phis = {A, B};
    if (Live) {
      MedOp Store;
      Store.Opcode = NdOp::STORE;
      Store.NumInputs = 2;
      Store.Inputs[0] = MedVar::makeConst(0x7000, 8);
      Store.Inputs[1] = B.Output;
      F.Blocks[0].Ops.insert(F.Blocks[0].Ops.begin() + 1, Store);
    }
    captureX64CallRCX(F, I, {}, {});
    EXPECT_EQ(F.CallClobbers.empty(), !Live);
    EXPECT_EQ(F.Blocks[0].Phis.empty(), !Live);
  }
}

TEST(X64CallRCX, LLVMAndCRetainTheCallItsMemoryAndUnwindEffects) {
  auto I = image();
  auto F = caller();
  captureX64CallRCX(F, I, {}, {});
  llvm::LLVMContext Context;
  MedLLVMEmitter Emitter;
  Emitter.setOriginalImageStorageForSource(true);
  MedFunc Declaration;
  Declaration.Entry = 0x2000;
  Declaration.Name = "native_target";
  Declaration.CC = CallingConv::Win64;
  Declaration.ReturnType = NdType::makeInt(8, false);
  const std::vector<MedFunc> Declarations{Declaration};
  Emitter.setSourceCalleeDeclarations(Declarations);
  auto M = Emitter.emit({F}, Context, "capture", Arch::X64, {}, &I,
                        BinaryFormat::COFF);
  ASSERT_TRUE(M);
  EXPECT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
  unsigned Calls = 0;
  const llvm::CallInst *NativeCall = nullptr;
  for (auto &Fn : *M)
    for (auto &B : Fn)
      for (auto &Inst : B)
        if (auto *C = llvm::dyn_cast<llvm::CallInst>(&Inst))
          if (isX64CallRCX(Arch::X64, *C)) {
            ++Calls;
            NativeCall = C;
            EXPECT_FALSE(Fn.hasFnAttribute(llvm::Attribute::NoUnwind));
            C->addFnAttr(llvm::Attribute::NoUnwind);
            EXPECT_THROW(isX64CallRCX(Arch::X64, *C), std::invalid_argument);
            C->removeFnAttr(llvm::Attribute::NoUnwind);
          }
  EXPECT_EQ(Calls, 1u);
  std::string C;
  llvm::raw_string_ostream OS(C);
  EXPECT_TRUE(LLVMCEmitter().emit(*M, OS));
  EXPECT_NE(C.find("callq *%%rax"), std::string::npos) << C;
  EXPECT_NE(C.find("\"unwind\""), std::string::npos) << C;
  EXPECT_NE(C.find("-fexceptions"), std::string::npos) << C;
  EXPECT_EQ(C.find("__builtin_trap"), std::string::npos) << C;
  EXPECT_NE(C.find("unk_2000"), std::string::npos) << C;
  if (const auto *Directory = std::getenv("NEVERD_TEST_X64_CALL_RCX_EXPORT")) {
    std::error_code Error;
    llvm::raw_fd_ostream IR(std::string(Directory) + "/native.ll", Error);
    ASSERT_FALSE(Error);
    M->print(IR, nullptr);
    llvm::raw_fd_ostream Source(std::string(Directory) + "/native.c", Error);
    ASSERT_FALSE(Error);
    Source << C;
    ASSERT_NE(NativeCall, nullptr);
    llvm::raw_fd_ostream Probe(std::string(Directory) + "/probe-llvm.c", Error);
    ASSERT_FALSE(Error);
    Probe << "#include <stdint.h>\n"
          << X64CallRCXCompilerContract
          << "__attribute__((noinline,target(\"arch=x86-64,no-avx,no-avx2,no-"
             "avx512f\")))\n"
             "uint64_t capture(uint64_t target, uint64_t a0, uint64_t a1, "
             "uint64_t a2, uint64_t a3, uint64_t *out) {\n"
             "struct { uint64_t field_0, field_1, field_2, field_3, field_4; } "
             "results;\n"
          << renderX64CallRCX(Arch::X64, *NativeCall, "results", true,
                              {"target", "a0", "a1", "a2", "a3"})
          << "\n*out = results.field_1; return results.field_0; }\n";
  }
}

TEST(X64CallRCX, HighCUsesTheSharedAssemblyAndOneEvaluationPerInput) {
  auto E = HighExpr::makeCall("__neverd_win64_call_rcx", 0, {});
  E->IntrinsicId = Intrinsic::X64CallRCX;
  E->Type = NdType::makeInt(8, false);
  for (unsigned I = 0; I < X64CallRCXArguments; ++I)
    E->Operands.push_back(HighExpr::makeConst(I, 8));
  MedVar Aux;
  Aux.Id = 7;
  Aux.Size = 8;
  E->IntrinsicOutputs.push_back(Aux);
  unsigned Reads = 0;
  const auto C = renderX64CallRCXExpression(
      Arch::X64, *E,
      [&](const HighExpr &V) {
        ++Reads;
        return "input_" + std::to_string(V.ConstVal) + "()";
      },
      [](const MedVar &) { return "extra_result"; },
      [](const MedVar &) { return true; });
  EXPECT_EQ(Reads, X64CallRCXArguments);
  EXPECT_NE(C.find("callq *%%rax"), std::string::npos);
  EXPECT_NE(C.find("extra_result = "), std::string::npos);
  EXPECT_NE(C.find("\"unwind\""), std::string::npos);
  HighFunc F;
  F.Entry = 0x1000;
  F.Name = "source_profile";
  F.ReturnType = NdType::makeInt(8, false);
  F.AnalysisOnly = true;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = E;
  F.Body.push_back(Return);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  std::string Full;
  llvm::raw_string_ostream OS(Full);
  HighCEmitter().emit({F}, OS, Options);
  EXPECT_NE(Full.find("NEVERD_X64_CALL_RCX"), std::string::npos) << Full;
  EXPECT_NE(Full.find("no-avx"), std::string::npos) << Full;
  EXPECT_NE(Full.find("neverd.analysis-only"), std::string::npos) << Full;
  if (const auto *Directory = std::getenv("NEVERD_TEST_X64_CALL_RCX_EXPORT")) {
    const char *Names[] = {"target", "a0", "a1", "a2", "a3"};
    const auto Body = renderX64CallRCXExpression(
        Arch::X64, *E, [&](const HighExpr &V) { return Names[V.ConstVal]; },
        [](const MedVar &) { return "*out"; },
        [](const MedVar &) { return true; });
    std::error_code Error;
    llvm::raw_fd_ostream Source(std::string(Directory) + "/probe.c", Error);
    ASSERT_FALSE(Error);
    Source << "#include <stdint.h>\n"
           << X64CallRCXCompilerContract
           << "__attribute__((noinline,target(\"arch=x86-64,no-avx,no-avx2,no-"
              "avx512f\")))\n"
              "uint64_t capture(uint64_t target, uint64_t a0, uint64_t a1, "
              "uint64_t a2, uint64_t a3, uint64_t *out) { return "
           << Body << "; }\n";
  }
  E->IntrinsicOutputs[0].Size = 4;
  EXPECT_THROW(renderX64CallRCXExpression(
                   Arch::X64, *E, [](const HighExpr &) { return "0"; },
                   [](const MedVar &) { return "x"; },
                   [](const MedVar &) { return true; }),
               std::invalid_argument);
}
} // namespace
