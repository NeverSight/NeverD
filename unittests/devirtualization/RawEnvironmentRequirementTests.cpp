//===- RawEnvironmentRequirementTests.cpp - Domain retention guards ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Raw environment dependencies must survive copying and refuse consumption
/// by engines that do not own the corresponding environment receipt.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/BinaryInterpreterSpecialization.h"
#include "neverd/analysis/InterpreterMachineState.h"
#include "neverd/analysis/InterpreterSpecialization.h"
#include "neverd/analysis/LowIRUndefinedIndependence.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/low/LowArchitecturalValue.h"
#include "neverd/ir/low/LowRawEnvironment.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MedRawEnvironment.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/symbolic/SymExec.h"

#include "llvm/IR/LLVMContext.h"

#include <stdexcept>

namespace {

using namespace neverd;
using namespace neverd::analysis;
using namespace neverd::symbolic;

constexpr auto Requirement = Intrinsic::RawEnvironmentRequirementV1;

LowOp marker() {
  return makeLowRawEnvironmentRequirement(
      {0x0123456789abcdef, 0xfedcba9876543210, 0, UINT64_MAX}, 0x100);
}

LowOp copy() {
  LowOp Op;
  Op.Opcode = NdOp::COPY;
  Op.Output = NdVar::reg(0, 8);
  Op.addInput(NdVar::scalar(42, 8));
  return Op;
}

LowFunc function(const LowOp &First) {
  LowFunc F;
  F.Name = "raw_environment_guard";
  F.Entry = 0x100;
  LowBlock B;
  B.Id = 0;
  B.StartAddr = F.Entry;
  B.EndAddr = F.Entry + 2;
  B.Ops.push_back(First);
  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  B.Ops.push_back(Ret);
  for (unsigned I = 0; I != B.Ops.size(); ++I) {
    auto &Op = B.Ops[I];
    Op.Addr = F.Entry + I;
    Op.Seq = 0;
    LowInstructionBoundary Boundary;
    Boundary.Address = Op.Addr;
    Boundary.Size = 1;
    Boundary.FirstOp = I;
    Boundary.OpCount = 1;
    if (Op.Opcode == NdOp::RETURN) {
      Boundary.Control = LowInstructionControl::Return;
      Boundary.ControlFlags = LowInstructionControlFlag::Return;
    }
    B.InstructionBoundaries.push_back(Boundary);
  }
  F.Blocks.push_back(std::move(B));
  return F;
}

std::vector<LowOp> malformedMarkers() {
  std::vector<LowOp> Result;
  auto Add = [&](auto Mutate) {
    auto Op = marker();
    Mutate(Op);
    Result.push_back(Op);
  };
  Add([](LowOp &Op) { Op.NumInputs = 0; });
  Add([](LowOp &Op) { Op.NumInputs = 1; });
  Add([](LowOp &Op) { Op.NumInputs = 6; });
  Add([](LowOp &Op) { Op.NumInputs = 255; });
  Add([](LowOp &Op) { Op.Output = NdVar::reg(0, 8); });
  Add([](LowOp &Op) { Op.Output = NdVar::cst(1, 0); });
  Add([](LowOp &Op) { Op.Inputs[0].Offset += uint64_t{1} << 32; });
  Add([](LowOp &Op) { Op.Inputs[0].Space = VnodeSpace::REG; });
  Add([](LowOp &Op) { Op.MemoryAddressSpace = NdMemoryAddressSpace::X86GS; });
  Add([](LowOp &Op) {
    Op.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  });
  for (unsigned I = 0; I != 5; ++I) {
    Add([&](LowOp &Op) { Op.Inputs[I].Size = 4; });
    Add([&](LowOp &Op) { Op.Inputs[I].Space = VnodeSpace::RAM; });
    Add([&](LowOp &Op) {
      Op.Inputs[I].Provenance = ConstantAddressProvenance::CodeAddress;
    });
    Add([&](LowOp &Op) { Op.Inputs[I].AddressOwnerVA = 0x100; });
  }
  return Result;
}

TEST(RawEnvironmentRequirement, ExactShapeRetainsFullScalarDigest) {
  const auto Op = marker();
  EXPECT_TRUE(lowRawEnvironmentRequirementShapeIsValid(Op));
  EXPECT_EQ(static_cast<unsigned>(Requirement), 2u);
  EXPECT_EQ(Op.Inputs[1].Offset, 0x0123456789abcdefu);
  EXPECT_EQ(Op.Inputs[2].Offset, 0xfedcba9876543210u);
  EXPECT_EQ(Op.Inputs[4].Offset, UINT64_MAX);
  for (const auto &Bad : malformedMarkers())
    EXPECT_FALSE(lowRawEnvironmentRequirementShapeIsValid(Bad));
  auto Missing = Op;
  Missing.NumInputs = 0;
  EXPECT_FALSE(lowRawEnvironmentRequirementShapeIsValid(Missing));
  Missing = Op;
  Missing.Opcode = NdOp::NOP;
  EXPECT_FALSE(lowRawEnvironmentRequirementShapeIsValid(Missing));
  Missing = Op;
  Missing.Inputs[0].Offset = static_cast<uint64_t>(Intrinsic::Syscall);
  EXPECT_FALSE(lowRawEnvironmentRequirementShapeIsValid(Missing));
}

TEST(RawEnvironmentRequirement, MetadataCannotDescribeARuntimeHelper) {
  EXPECT_TRUE(isSideeffectIntrinsic(Requirement));
  EXPECT_STREQ(intrinsicName(Requirement), "raw_environment_requirement_v1");
  EXPECT_EQ(intrinsicFromName("raw_environment_requirement_v1"), Requirement);
  EXPECT_EQ(intrinsicCName(Requirement), nullptr);
  EXPECT_EQ(intrinsicAsmMnemonic(Requirement), nullptr);
  EXPECT_EQ(intrinsicOutputCount(Requirement), 0u);
  EXPECT_FALSE(isUnconditionalTrapIntrinsic(Requirement));
}

TEST(RawEnvironmentRequirement, CopyAndUnreachableOccurrenceKeepRequirement) {
  const auto Original = function(marker());
  const LowFunc Copied = Original;
  EXPECT_TRUE(hasLowRawEnvironmentRequirement(Copied));
  auto Unreachable = function(copy());
  auto Other = Original.Blocks.front();
  Other.Id = 1;
  Other.StartAddr += 0x100;
  Other.EndAddr += 0x100;
  for (auto &Op : Other.Ops)
    Op.Addr += 0x100;
  for (auto &Boundary : Other.InstructionBoundaries)
    Boundary.Address += 0x100;
  Unreachable.Blocks.push_back(Other);
  EXPECT_TRUE(hasLowRawEnvironmentRequirement(Unreachable));
  EXPECT_FALSE(hasLowRawEnvironmentRequirement(function(copy())));
  for (const auto &F : {Copied, Unreachable})
    for (auto Layout : {InterpreterMachineStateLayout::InlineReturns,
                        InterpreterMachineStateLayout::SharedGPRExit}) {
      auto Source = wrapInterpreterMachineStateX64(
          F, BinaryFormat::ELF,
          InterpreterMachineStateProfile::UserX64NoFaultV1, std::nullopt,
          Layout);
      ASSERT_FALSE(static_cast<bool>(Source));
      EXPECT_NE(llvm::toString(Source.takeError()).find("raw environment"),
                std::string::npos);
      auto Model = modelInterpreterMachineStateX64(
          F, InterpreterMachineStateProfile::UserX64NoFaultV1, 65536,
          std::nullopt, Layout);
      ASSERT_FALSE(static_cast<bool>(Model));
      EXPECT_NE(llvm::toString(Model.takeError()).find("raw environment"),
                std::string::npos);
    }
}

TEST(RawEnvironmentRequirement, ConcreteModesRefuseBeforeApproximation) {
  auto Ops = malformedMarkers();
  Ops.push_back(marker());
  for (bool Strict : {false, true})
    for (const auto &Op : Ops) {
      BinaryImage Image;
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(Strict);
      Emulator.setRegister(0, 17);
      EXPECT_FALSE(Emulator.step(Op));
      EXPECT_EQ(Emulator.getRegister(0), 17u);
      EXPECT_EQ(Emulator.skips().ApproximatedOps, 0u);
      EXPECT_EQ(Emulator.run(std::vector<LowOp>{Op, copy()}), 0u);
      EXPECT_EQ(Emulator.getRegister(0), 17u);
    }
}

TEST(RawEnvironmentRequirement, SymbolicStepAndRunCannotApproximateDomain) {
  auto Ops = malformedMarkers();
  Ops.push_back(marker());
  for (const auto &Op : Ops) {
    SymContext Ctx;
    SymState State(Ctx);
    State.write(SymSpace::Register, 0, Ctx.mkConst(64, 17));
    SymExec Exec(Ctx, State);
    EXPECT_THROW(Exec.step(Op), std::invalid_argument);
    auto Value = Ctx.asConst(State.read(SymSpace::Register, 0, 8));
    ASSERT_TRUE(Value.has_value());
    EXPECT_EQ(Value->getZExtValue(), 17u);
    EXPECT_EQ(Exec.unmodelledCount(), 0u);
    EXPECT_EQ(Exec.opaqueOperationCount(), 0u);
    EXPECT_THROW(Exec.run(std::vector<LowOp>{Op, copy()}),
                 std::invalid_argument);
    Value = Ctx.asConst(State.read(SymSpace::Register, 0, 8));
    ASSERT_TRUE(Value.has_value());
    EXPECT_EQ(Value->getZExtValue(), 17u);
  }
}

TEST(RawEnvironmentRequirement, LowToMedRefusesBeforeBestEffortConversion) {
  auto Ops = malformedMarkers();
  Ops.push_back(marker());
  for (const auto &Op : Ops)
    EXPECT_THROW(LowToMedConverter().convert(function(Op), Arch::X64),
                 LowToMedConversionError);
}

TEST(RawEnvironmentRequirement, DirectLLVMRefusesIncludingWideIdAliases) {
  for (uint64_t Id :
       {uint64_t(Requirement), uint64_t(Requirement) + (uint64_t{1} << 32)}) {
    MedFunc F;
    F.Entry = 0x100;
    F.Name = "raw_environment_guard";
    F.CC = CallingConv::SysV_AMD64;
    F.ReturnType = NdType::makeVoid();
    MedBlock B;
    B.Id = 0;
    B.StartAddr = F.Entry;
    B.EndAddr = F.Entry + 2;
    MedOp Op;
    Op.Opcode = NdOp::INTRINSIC;
    Op.Addr = F.Entry;
    Op.addInput(MedVar::makeConst(Id, 8));
    for (unsigned I = 1; I != 5; ++I)
      Op.addInput(MedVar::makeConst(marker().Inputs[I].Offset, 8));
    B.Ops.push_back(Op);
    Op = {};
    Op.Opcode = NdOp::RETURN;
    Op.Addr = F.Entry + 1;
    B.Ops.push_back(Op);
    F.Blocks.push_back(B);
    for (unsigned Shape = 0; Shape != 6; ++Shape) {
      auto Altered = F;
      auto &Marker = Altered.Blocks.front().Ops.front();
      if (Shape == 1)
        Marker.NumInputs = 0;
      else if (Shape == 2)
        Marker.Inputs.front().Kind = MedVar::Reg;
      else if (Shape == 3)
        Marker.NumInputs = 255;
      else if (Shape >= 4) {
        Marker.Inputs.clear();
        Marker.NumInputs = Shape == 4 ? 0 : 5;
        EXPECT_FALSE(isMedRawEnvironmentRequirement(Marker));
      }
      const char *Error = Shape < 4 ? "raw environment requirement"
                                    : "invalid intrinsic ID shape";
      for (bool Masked : {false, true})
        EXPECT_DEATH(
            {
              llvm::LLVMContext Ctx;
              const std::vector<char> Mask{0};
              (void)MedLLVMEmitter().emit({Altered}, Ctx, F.Name, Arch::X64, {},
                                          nullptr, BinaryFormat::ELF, false,
                                          Masked ? &Mask : nullptr);
            },
            Error);
    }
  }
}

TEST(RawEnvironmentRequirement, DirectCRenderingRefusesAllTargetDispatches) {
  for (auto A : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    bool HasIntrinsics = false;
    EXPECT_THROW(renderIntrinsicCall(Requirement, A, {}, 0, HasIntrinsics),
                 std::invalid_argument);
    EXPECT_FALSE(HasIntrinsics);
    EXPECT_THROW(MultiOutputRender{}(A, Requirement, {}, {}, {}, {}),
                 std::invalid_argument);
  }
}

TEST(RawEnvironmentRequirement, DirectHighCRefusesBeforePublishingSource) {
  for (unsigned Placement = 0; Placement != 4; ++Placement) {
    HighFunc F;
    F.Entry = 0x100;
    F.Name = "raw_environment_guard";
    F.ReturnType = NdType::makeVoid();
    HighStmt Invoke;
    Invoke.Kind = StmtKind::ExprStmt;
    Invoke.Val = std::make_shared<HighExpr>();
    Invoke.Val->Kind = ExprKind::Call;
    Invoke.Val->Type = NdType::makeVoid();
    Invoke.Val->IntrinsicId = Requirement;
    if (Placement == 1) {
      HighStmt Branch;
      Branch.Kind = StmtKind::If;
      Branch.Cond = HighExpr::makeConst(0, 1);
      Branch.Body.push_back(Invoke);
      F.Body.push_back(Branch);
    } else if (Placement == 2) {
      HighStmt Try;
      Try.Kind = StmtKind::SEHTry;
      Try.EHClauses.resize(1);
      Try.EHClauses.front().FilterValue = Invoke.Val;
      Try.EHClauseBodies.resize(1);
      F.Body.push_back(Try);
    } else if (Placement == 3) {
      auto Outer = HighExpr::makeConst(0, 8);
      Outer->Operands.push_back(Invoke.Val);
      Invoke.Val = Outer; // Malformed hidden child must still retain refusal.
      F.Body.push_back(Invoke);
    } else
      F.Body.push_back(Invoke);
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    EXPECT_THROW(HighCEmitter().emit({F}, OS, Options), std::invalid_argument);
    EXPECT_TRUE(Text.empty());
  }
}

class MarkerProvider final : public SpecializationProvider {
public:
  llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor Cursor) override {
    auto F = function(marker());
    SpecializationInstruction I;
    I.Ops = {F.Blocks.front().Ops.front()};
    I.Origin = F.Blocks.front().InstructionBoundaries.front();
    I.Fallthrough = {Cursor.Address + 1};
    return I;
  }
};

TEST(RawEnvironmentRequirement, RespecializationRefusesWithoutResidual) {
  MarkerProvider Provider;
  auto Result = specializeInterpreter(Provider, {0x100});
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_FALSE(Result.complete());
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

void addUnreachableRequirement(LowFunc &F) {
  auto B = function(marker()).Blocks.front();
  B.Id = 0;
  for (const auto &Existing : F.Blocks)
    B.Id = std::max(B.Id, Existing.Id + 1);
  B.StartAddr += 0x3000;
  B.EndAddr += 0x3000;
  for (auto &Op : B.Ops)
    Op.Addr += 0x3000;
  for (auto &Boundary : B.InstructionBoundaries)
    Boundary.Address += 0x3000;
  F.Blocks.push_back(B);
}

TEST(RawEnvironmentRequirement, StaticProofRefusesUnreachableRequirement) {
  const auto Ordinary = function(copy());
  std::vector<LowIRUndefinedInstruction> Records;
  for (const auto &B : Ordinary.Blocks)
    for (const auto &Boundary : B.InstructionBoundaries) {
      LowIRUndefinedInstruction Record;
      Record.BlockId = B.Id;
      Record.Boundary = Boundary;
      Record.Effects.Coverage = LowUndefinedCoverage::Complete;
      Record.Effects.OpCount = Boundary.OpCount;
      Record.Effects.OperationDigest =
          lowUndefinedOperationDigest(llvm::ArrayRef<LowOp>(B.Ops).slice(
              Boundary.FirstOp, Boundary.OpCount));
      Records.push_back(Record);
    }
  LowIRIndependenceContract Contract;
  Contract.ReturnRegisters = {{0, 8}};
  const auto Control =
      checkLowIRUndefinedIndependence(Ordinary, Records, Contract);
  ASSERT_TRUE(Control.proved()) << Control.Diagnostic;
  auto Raw = Ordinary;
  addUnreachableRequirement(Raw);
  const auto Proof = checkLowIRUndefinedIndependence(Raw, Records, Contract);
  EXPECT_EQ(Proof.Status, LowIRIndependenceStatus::Unsupported);
  EXPECT_FALSE(Proof.Certificate);
  EXPECT_NE(Proof.Diagnostic.find("raw environment"), std::string::npos);
  const auto RelationControl =
      checkLowIRRefinement(Ordinary, Records, Ordinary, Contract);
  ASSERT_TRUE(RelationControl.proved()) << RelationControl.Diagnostic;
  for (bool Original : {false, true}) {
    const auto Relation =
        checkLowIRRefinement(Original ? Raw : Ordinary, Records,
                             Original ? Ordinary : Raw, Contract);
    EXPECT_EQ(Relation.Status, LowIRRefinementStatus::Unsupported);
    EXPECT_FALSE(Relation.Certificate);
    EXPECT_NE(Relation.Diagnostic.find("raw environment"), std::string::npos);
  }
}

TEST(RawEnvironmentRequirement, NativeCandidateCannotLoseUnusedRequirement) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Complete;
  Segment Code;
  Code.VA = 0x1000;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data = {0xb8, 42, 0, 0, 0, 0xc3}; // mov eax,42; ret
  Code.Size = Code.FileSz = Code.Data.size();
  Image.Segments.push_back(Code);
  SpecializationOptions Options;
  Options.ExplicitMachineState = true;
  Options.NormalNonfaultingExecution = true;
  Options.X64CetDisabled = true;
  Options.X64FlagsProfile = InterpreterMachineStateProfile::UserX64NoFaultV1;
  Options.FrameBaseRegister = SymRegisterRange{x86reg::RSP, 8};
  LowIRIndependenceContract Contract;
  Contract.X64FlagsProfile = Options.X64FlagsProfile;
  Contract.Frame = LowIRIndependenceFrame{{x86reg::RSP, 8}, -64, 8};
  Contract.ReturnRegisters = {{x86reg::RAX, 8}, {x86reg::RSP, 8}};
  auto Recovered = specializeBinaryInterpreter(Image, Code.VA, Options);
  ASSERT_TRUE(Recovered.complete()) << Recovered.Diagnostic;
  const auto Good = checkBinaryLowIRRefinement(Image, Code.VA, Options,
                                               Recovered.Residual, Contract);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  addUnreachableRequirement(Recovered.Residual);
  const auto Bad = checkBinaryLowIRRefinement(Image, Code.VA, Options,
                                              Recovered.Residual, Contract);
  EXPECT_EQ(Bad.Proof.Status, LowIRRefinementStatus::Unsupported);
  EXPECT_FALSE(Bad.Certificate);
  EXPECT_FALSE(Bad.Proof.Certificate);
  EXPECT_NE(Bad.Proof.Diagnostic.find("raw environment"), std::string::npos);
}

TEST(RawEnvironmentRequirement, OrdinaryOperationsKeepTheirExecution) {
  BinaryImage Image;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(false);
  EXPECT_TRUE(Emulator.step(copy()));
  EXPECT_EQ(Emulator.getRegister(0), 42u);
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  EXPECT_EQ(Exec.step(copy()), StepResult::Continue);
  EXPECT_EQ(Ctx.asConst(State.read(SymSpace::Register, 0, 8))->getZExtValue(),
            42u);
  auto Wrapped = wrapInterpreterMachineStateX64(function(copy()));
  EXPECT_TRUE(static_cast<bool>(Wrapped))
      << llvm::toString(Wrapped.takeError());
}

#include "ArchitecturalValueRequirementTests.inc"

} // namespace
