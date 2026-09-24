#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCBlockCallHints.h"

using namespace neverd;

namespace {
LowOp op(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs,
         va_t Address) {
  LowOp Result;
  Result.Opcode = Opcode;
  Result.Output = Output;
  Result.Addr = Address;
  for (const auto &V : Inputs)
    Result.addInput(V);
  return Result;
}

struct Fixture {
  BinaryImage Image;
  LowFunc Low;
  SourceFunctionTypeHint Entry;
  uint64_t R0, R1, R2, R3, Target;

  explicit Fixture(Arch Architecture) {
    Image.Arch = Architecture;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::MachO;
    const auto &TRI = getTargetRegInfo(Architecture);
    R0 = TRI.IntParamRegs[0];
    R1 = TRI.IntParamRegs[1];
    R2 = TRI.IntParamRegs[2];
    R3 = TRI.IntParamRegs[3];
    Target = Architecture == Arch::AArch64 ? 64 : 0;
    auto Pointer = NdType::makePtr(NdType::makeVoid());
    Entry.ReturnType = NdType::makeInt(4);
    Entry.Parameters = {{"self", Pointer},
                        {"cmd", Pointer},
                        {"block", Pointer},
                        {"value", NdType::makeInt(4)}};
    std::string Error;
    EXPECT_TRUE(assignDarwinObjCSourceABI(Entry, Architecture, Error)) << Error;
    Low.Name = "arbitrary_method_identity";
    Low.Entry = 0x1000;
    Low.Blocks.emplace_back();
    auto &B = Low.Blocks[0];
    B.Id = 0;
    B.StartAddr = 0x1000;
    B.EndAddr = 0x1020;
    B.Ops = {op(NdOp::COPY, NdVar::reg(R1, 8), {NdVar::reg(R3, 8)}, 0x1000),
             op(NdOp::COPY, NdVar::reg(R0, 8), {NdVar::reg(R2, 8)}, 0x1004),
             op(NdOp::INT_ADD, NdVar::tmp(0, 8),
                {NdVar::reg(R2, 8), NdVar::cst(16, 8)}, 0x1008),
             op(NdOp::LOAD, NdVar::reg(Target, 8), {NdVar::tmp(0, 8)}, 0x1008),
             op(NdOp::INDIR_CALL, NdVar::reg(TRI.IntReturnReg, 8),
                {NdVar::reg(Target, 8)}, 0x100c),
             op(NdOp::RETURN, {}, {NdVar::reg(TRI.IntReturnReg, 4)}, 0x1010)};
  }
  std::map<va_t, SourceCallTypeHint> hints() const {
    return buildObjCBlockCallHints(Image, Low, &Entry);
  }
};

TEST(ObjCBlockCallHints, ProvesSameReceiverAndKeepsOnlyWrittenScalarArguments) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    Fixture F(Architecture);
    const auto Hints = F.hints();
    ASSERT_EQ(Hints.size(), 1U);
    const auto &Hint = Hints.at(0x100c);
    EXPECT_EQ(Hint.CallKind, SourceCallTypeHint::Kind::BlockInvoke);
    EXPECT_EQ(Hint.Signature.Origin,
              SourceFunctionTypeHint::OriginKind::NativeAnalysis);
    ASSERT_EQ(Hint.Signature.Parameters.size(), 2U);
    EXPECT_EQ(Hint.Signature.Parameters[0].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Hint.Signature.Parameters[1].Type->Size, 4U);
    EXPECT_EQ(Hint.Signature.Parameters[1].Location.RegisterOffset, F.R1);
    EXPECT_EQ(Hint.Signature.ReturnType->Size, 4U);
    EXPECT_EQ(Hint.TargetAddress, 0U);
  }
}

TEST(ObjCBlockCallHints, ProvesInvokeInMultiBlockEntryPrefixOnly) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    Fixture F(Architecture);
    F.Low.Blocks.reserve(2);
    auto &Entry = F.Low.Blocks[0];
    Entry.Ops.back() = op(
        NdOp::COPY, NdVar::tmp(1, 4),
        {NdVar::reg(getTargetRegInfo(Architecture).IntReturnReg, 4)}, 0x1010);
    Entry.Ops.push_back(op(NdOp::BRANCH, {}, {NdVar::cst(0x1020, 8)}, 0x1014));
    Entry.Succs = {1};
    Entry.EndAddr = 0x1018;
    F.Low.Blocks.emplace_back();
    auto &Successor = F.Low.Blocks[1];
    Successor.Id = 1;
    Successor.StartAddr = 0x1020;
    Successor.EndAddr = 0x1024;
    Successor.Preds = {0};
    Successor.Ops = {op(NdOp::RETURN, {}, {}, 0x1020)};

    auto Hints = F.hints();
    ASSERT_EQ(Hints.size(), 1U);
    EXPECT_EQ(Hints.at(0x100c).Signature.ReturnType->Size, 4U);

    Entry.Preds = {1};
    EXPECT_TRUE(F.hints().empty());
    Entry.Preds.clear();
    Entry.StartAddr = 0x1004;
    EXPECT_TRUE(F.hints().empty());
    Entry.StartAddr = F.Low.Entry;
    Entry.Ops.insert(Entry.Ops.begin(),
                     op(NdOp::BRANCH, {}, {NdVar::cst(0x1020, 8)}, 0xffc));
    EXPECT_TRUE(F.hints().empty());
  }
}

TEST(ObjCBlockCallHints, RejectsSuccessorWithoutUniqueEntryPath) {
  Fixture F(Arch::AArch64);
  const auto InvokeOps = F.Low.Blocks[0].Ops;
  F.Low.Blocks[0].Ops = {op(NdOp::BRANCH, {}, {NdVar::cst(0x1020, 8)}, 0x1000)};
  F.Low.Blocks[0].Succs = {1};
  F.Low.Blocks.emplace_back();
  auto &Successor = F.Low.Blocks[1];
  Successor.Id = 1;
  Successor.StartAddr = 0x1020;
  Successor.EndAddr = 0x1040;
  Successor.Preds = {0, 2};
  Successor.Ops = InvokeOps;
  EXPECT_TRUE(F.hints().empty());
}

TEST(ObjCBlockCallHints, ResultUseAcrossBranchDoesNotInferBlockReturn) {
  Fixture F(Arch::AArch64);
  F.Low.Blocks[0].Ops.back() =
      op(NdOp::BRANCH, {}, {NdVar::cst(0x1020, 8)}, 0x1010);
  F.Low.Blocks[0].Succs = {1};
  F.Low.Blocks.emplace_back();
  auto &Successor = F.Low.Blocks[1];
  Successor.Id = 1;
  Successor.StartAddr = 0x1020;
  Successor.Preds = {0};
  Successor.Ops = {
      op(NdOp::COPY, NdVar::tmp(1, 8), {NdVar::reg(F.R0, 8)}, 0x1020),
      op(NdOp::RETURN, {}, {NdVar::tmp(1, 8)}, 0x1024)};
  EXPECT_TRUE(F.hints().empty());
}

TEST(ObjCBlockCallHints, TraversesIntermediateBlockWithoutInvoke) {
  Fixture F(Arch::AArch64);
  F.Low.Blocks.reserve(3);
  auto InvokeOps = F.Low.Blocks[0].Ops;
  for (auto &Op : InvokeOps)
    Op.Addr += 0x20;
  F.Low.Blocks[0].Ops = {op(NdOp::BRANCH, {}, {NdVar::cst(0x1010, 8)}, 0x1000)};
  F.Low.Blocks[0].Succs = {1};
  F.Low.Blocks.emplace_back();
  auto &Middle = F.Low.Blocks[1];
  Middle.Id = 1;
  Middle.StartAddr = 0x1010;
  Middle.Preds = {0};
  Middle.Succs = {2};
  Middle.Ops = {op(NdOp::BRANCH, {}, {NdVar::cst(0x1020, 8)}, 0x1010)};
  F.Low.Blocks.emplace_back();
  auto &Tail = F.Low.Blocks[2];
  Tail.Id = 2;
  Tail.StartAddr = 0x1020;
  Tail.Preds = {1};
  Tail.Ops = std::move(InvokeOps);

  const auto Hints = F.hints();
  ASSERT_EQ(Hints.size(), 1U);
  EXPECT_TRUE(Hints.count(0x102c));
  Middle.Ops[0].Opcode = NdOp::INTRINSIC;
  EXPECT_TRUE(F.hints().empty());
}

TEST(ObjCBlockCallHints, ProvesInvokeAcrossConvergingOrdinaryPaths) {
  Fixture F(Arch::AArch64);
  F.Low.Blocks.reserve(4);
  auto InvokeOps = F.Low.Blocks[0].Ops;
  for (auto &Op : InvokeOps) {
    Op.Addr += 0x40;
    for (unsigned I = 0; I < Op.NumInputs; ++I)
      if (Op.Inputs[I].isReg() && Op.Inputs[I].Offset == F.R2)
        Op.Inputs[I] = NdVar::reg(a64reg::X19, 8);
  }
  F.Low.Blocks[0].Ops = {
      op(NdOp::COPY, NdVar::reg(a64reg::X19, 8), {NdVar::reg(F.R2, 8)}, 0x1000),
      op(NdOp::COND_BR, {}, {NdVar::cst(0x1020, 8)}, 0x1004)};
  F.Low.Blocks[0].Succs = {1, 2};
  F.Low.Blocks.emplace_back();
  auto &Left = F.Low.Blocks[1];
  Left.Id = 1;
  Left.StartAddr = 0x1010;
  Left.Preds = {0};
  Left.Succs = {3};
  Left.Ops = {op(NdOp::BRANCH, {}, {NdVar::cst(0x1040, 8)}, 0x1010)};
  F.Low.Blocks.emplace_back();
  auto &Right = F.Low.Blocks[2];
  Right.Id = 2;
  Right.StartAddr = 0x1020;
  Right.Preds = {0};
  Right.Succs = {3};
  Right.Ops = {op(NdOp::BRANCH, {}, {NdVar::cst(0x1040, 8)}, 0x1020)};
  F.Low.Blocks.emplace_back();
  auto &Join = F.Low.Blocks[3];
  Join.Id = 3;
  Join.StartAddr = 0x1040;
  Join.Preds = {1, 2};
  Join.Ops = std::move(InvokeOps);

  const auto Hints = F.hints();
  ASSERT_EQ(Hints.size(), 1U);
  EXPECT_EQ(Hints.at(0x104c).Signature.ReturnType->Size, 4U);

  Right.Ops[0].Opcode = NdOp::INTRINSIC;
  EXPECT_TRUE(F.hints().empty());
  Right.Ops[0].Opcode = NdOp::BRANCH;
  ExceptionalEdge Exceptional;
  Exceptional.BlockId = 2;
  Join.ExceptionalPreds = {Exceptional};
  EXPECT_TRUE(F.hints().empty());
}

TEST(ObjCBlockCallHints, FollowsUniquePredecessorToBlockInvoke) {
  Fixture F(Arch::AArch64);
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  SourceCallTypeHint Getter;
  Getter.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Getter.Signature.ReturnType = Pointer;
  Getter.Signature.Parameters = {{"self", Pointer}, {"cmd", Pointer}};
  SourceCallTypeHint Retain;
  Retain.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  Retain.TargetName = "objc_retainAutoreleasedReturnValue";
  Retain.Signature.ReturnType = Pointer;
  Retain.Signature.Parameters = {{"object", Pointer}};
  std::string Error;
  ASSERT_TRUE(assignDarwinObjCSourceABI(Getter.Signature, F.Image.Arch, Error))
      << Error;
  ASSERT_TRUE(
      assignDarwinScalarSourceABI(Retain.Signature, F.Image.Arch, Error))
      << Error;
  const std::map<va_t, SourceCallTypeHint> Calls{
      {0x1010, Getter}, {0x1014, Retain}, {0x1024, Retain}};

  F.Low.Blocks.reserve(3);
  F.Low.Blocks[0].Ops = {
      op(NdOp::COPY, NdVar::reg(a64reg::X19, 8), {NdVar::reg(F.R2, 8)}, 0x1000),
      op(NdOp::BRANCH, {}, {NdVar::cst(0x1010, 8)}, 0x1004)};
  F.Low.Blocks[0].Succs = {1};
  F.Low.Blocks.emplace_back();
  auto &Successor = F.Low.Blocks[1];
  Successor.Id = 1;
  Successor.StartAddr = 0x1010;
  Successor.EndAddr = 0x102c;
  Successor.Preds = {0};
  Successor.Ops = {
      op(NdOp::CALL, NdVar::reg(F.R0, 8), {NdVar::cst(0x2000, 8)}, 0x1010),
      op(NdOp::CALL, NdVar::reg(F.R0, 8), {NdVar::cst(0x3000, 8)}, 0x1014),
      op(NdOp::INT_ADD, NdVar::tmp(0, 8),
         {NdVar::reg(F.R0, 8), NdVar::cst(16, 8)}, 0x1018),
      op(NdOp::LOAD, NdVar::reg(F.Target, 8), {NdVar::tmp(0, 8)}, 0x1018),
      op(NdOp::COPY, NdVar::reg(F.R1, 8), {NdVar::reg(a64reg::X19, 8)}, 0x101c),
      op(NdOp::INDIR_CALL, NdVar::reg(F.R0, 8), {NdVar::reg(F.Target, 8)},
         0x1020),
      op(NdOp::CALL, NdVar::reg(F.R0, 8), {NdVar::cst(0x3000, 8)}, 0x1024),
      op(NdOp::RETURN, {}, {NdVar::reg(F.R0, 8)}, 0x1028)};

  auto Hints = buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &Calls);
  ASSERT_EQ(Hints.size(), 1U);
  EXPECT_EQ(Hints.at(0x1020).Signature.Parameters.size(), 2U);
  EXPECT_EQ(Hints.at(0x1020).Signature.ReturnType->Size, 8U);
  F.Low.Blocks[0].Ops.back().Opcode = NdOp::COND_BR;
  F.Low.Blocks[0].Succs.push_back(2);
  F.Low.Blocks.emplace_back();
  F.Low.Blocks[2].Id = 2;
  F.Low.Blocks[2].StartAddr = 0x1030;
  F.Low.Blocks[2].Preds = {0};
  EXPECT_EQ(buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &Calls).size(),
            1U);
  Successor.Preds.push_back(2);
  EXPECT_TRUE(
      buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &Calls).empty());
  Successor.Preds.pop_back();
  F.Low.Blocks[0].Ops.back().Opcode = NdOp::INDIR_BR;
  EXPECT_TRUE(
      buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &Calls).empty());
  F.Low.Blocks[0].Ops.back().Opcode = NdOp::COND_BR;
  F.Low.Blocks[0].Ops[0].Opcode = NdOp::INTRINSIC;
  EXPECT_TRUE(
      buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &Calls).empty());
}

TEST(ObjCBlockCallHints, BoundARCConsumerProvesEntryBlockInvokeBeforeBranch) {
  Fixture F(Arch::AArch64);
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  SourceCallTypeHint Retain;
  Retain.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  Retain.TargetName = "objc_retainAutoreleasedReturnValue";
  Retain.Signature.ReturnType = Pointer;
  Retain.Signature.Parameters = {{"object", Pointer}};
  std::string Error;
  ASSERT_TRUE(
      assignDarwinScalarSourceABI(Retain.Signature, F.Image.Arch, Error))
      << Error;
  F.Low.Blocks[0].Ops.back() =
      op(NdOp::CALL, NdVar::reg(F.R0, 8), {NdVar::cst(0x3000, 8)}, 0x1010);
  F.Low.Blocks[0].Ops.push_back(
      op(NdOp::BRANCH, {}, {NdVar::cst(0x1020, 8)}, 0x1014));
  F.Low.Blocks[0].Succs = {1};
  F.Low.Blocks.emplace_back();
  F.Low.Blocks[1].Id = 1;
  F.Low.Blocks[1].StartAddr = 0x1020;
  F.Low.Blocks[1].Preds = {0};
  const std::map<va_t, SourceCallTypeHint> Calls{{0x1010, Retain}};
  const auto Hints = buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &Calls);
  ASSERT_EQ(Hints.size(), 1U);
  EXPECT_EQ(Hints.at(0x100c).Signature.ReturnType->Size, 8U);
  EXPECT_TRUE(F.hints().empty());
}

TEST(ObjCBlockCallHints, BoundReleaseProvesUnobservedBlockReturn) {
  Fixture F(Arch::AArch64);
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  SourceCallTypeHint Release;
  Release.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  Release.TargetName = "objc_release";
  Release.Signature.ReturnType = NdType::makeVoid();
  Release.Signature.Parameters = {{"object", Pointer}};
  std::string Error;
  ASSERT_TRUE(
      assignDarwinScalarSourceABI(Release.Signature, F.Image.Arch, Error))
      << Error;
  auto &Ops = F.Low.Blocks[0].Ops;
  Ops.back() =
      op(NdOp::COPY, NdVar::reg(F.R0, 8), {NdVar::reg(F.R2, 8)}, 0x1010);
  Ops.push_back(op(NdOp::CALL, {}, {NdVar::cst(0x3000, 8)}, 0x1014));
  Ops.push_back(op(NdOp::RETURN, {}, {}, 0x1018));
  const std::map<va_t, SourceCallTypeHint> Calls{{0x1014, Release}};

  auto Hints = buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &Calls);
  ASSERT_EQ(Hints.size(), 1U);
  EXPECT_EQ(Hints.at(0x100c).Signature.ReturnType->Kind, NdTypeKind::Void);
  EXPECT_TRUE(F.hints().empty());
  auto WrongCalls = Calls;
  WrongCalls.at(0x1014).TargetName = "objc_retain";
  EXPECT_TRUE(
      buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &WrongCalls).empty());
  Ops.insert(Ops.end() - 2,
             op(NdOp::COPY, NdVar::tmp(1, 8),
                {NdVar::reg(getTargetRegInfo(F.Image.Arch).FPReturnReg, 8)},
                0x1012));
  EXPECT_TRUE(
      buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &Calls).empty());
  Ops.erase(Ops.end() - 3);
  Ops[3].Output = NdVar::reg(a64reg::X9, 8);
  Ops[4].Inputs[0] = NdVar::reg(a64reg::X9, 8);
  EXPECT_TRUE(
      buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &Calls).empty());
}

TEST(ObjCBlockCallHints, BoundReleaseProvesDiscardedResultAcrossJoin) {
  Fixture F(Arch::AArch64);
  F.Low.Blocks.reserve(3);
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  SourceCallTypeHint Release;
  Release.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  Release.TargetName = "objc_release";
  Release.Signature.ReturnType = NdType::makeVoid();
  Release.Signature.Parameters = {{"object", Pointer}};
  std::string Error;
  ASSERT_TRUE(
      assignDarwinScalarSourceABI(Release.Signature, F.Image.Arch, Error))
      << Error;
  const std::map<va_t, SourceCallTypeHint> Calls{{0x1024, Release}};
  F.Low.Blocks[0].Ops.back() =
      op(NdOp::BRANCH, {}, {NdVar::cst(0x1020, 8)}, 0x1010);
  F.Low.Blocks[0].Succs = {1};
  F.Low.Blocks.emplace_back();
  auto &Join = F.Low.Blocks[1];
  Join.Id = 1;
  Join.StartAddr = 0x1020;
  Join.Preds = {0, 2};
  Join.Ops = {
      op(NdOp::COPY, NdVar::reg(F.R0, 8), {NdVar::reg(F.R2, 8)}, 0x1020),
      op(NdOp::CALL, {}, {NdVar::cst(0x3000, 8)}, 0x1024),
      op(NdOp::RETURN, {}, {}, 0x1028)};
  F.Low.Blocks.emplace_back();
  F.Low.Blocks[2].Id = 2;
  F.Low.Blocks[2].StartAddr = 0x1030;
  F.Low.Blocks[2].Succs = {1};

  auto Hints = buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &Calls);
  ASSERT_EQ(Hints.size(), 1U);
  EXPECT_EQ(Hints.at(0x100c).Signature.ReturnType->Kind, NdTypeKind::Void);
  EXPECT_TRUE(F.hints().empty());
  Join.Preds = {2};
  EXPECT_TRUE(
      buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &Calls).empty());
  Join.Preds = {0, 2};
  Join.Ops.insert(
      Join.Ops.begin() + 1,
      op(NdOp::COPY, NdVar::tmp(2, 8),
         {NdVar::reg(getTargetRegInfo(F.Image.Arch).IntReturnRegs[1], 8)},
         0x1022));
  EXPECT_TRUE(
      buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &Calls).empty());
  Join.Ops.erase(Join.Ops.begin() + 1);
  F.Low.Blocks[0].Ops.back().Opcode = NdOp::COND_BR;
  F.Low.Blocks[0].Succs.push_back(2);
  F.Low.Blocks[2].Preds = {0};
  F.Low.Blocks[2].Ops = {op(NdOp::RETURN, {}, {}, 0x1030)};
  EXPECT_TRUE(
      buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &Calls).empty());
}

TEST(ObjCBlockCallHints, BoundIntegerMessageResultFeedsVoidBlockArgument) {
  Fixture F(Arch::AArch64);
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  SourceCallTypeHint IntegerGetter;
  IntegerGetter.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  IntegerGetter.Signature.ReturnType = NdType::makeInt(8, false);
  IntegerGetter.Signature.Parameters = {{"self", Pointer}, {"cmd", Pointer}};
  SourceCallTypeHint BlockGetter = IntegerGetter;
  BlockGetter.Signature.ReturnType = Pointer;
  SourceCallTypeHint Retain;
  Retain.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  Retain.TargetName = "objc_retainAutoreleasedReturnValue";
  Retain.Signature.ReturnType = Pointer;
  Retain.Signature.Parameters = {{"object", Pointer}};
  SourceCallTypeHint Release;
  Release.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  Release.TargetName = "objc_release";
  Release.Signature.ReturnType = NdType::makeVoid();
  Release.Signature.Parameters = {{"object", Pointer}};
  std::string Error;
  ASSERT_TRUE(
      assignDarwinObjCSourceABI(IntegerGetter.Signature, F.Image.Arch, Error))
      << Error;
  ASSERT_TRUE(
      assignDarwinObjCSourceABI(BlockGetter.Signature, F.Image.Arch, Error))
      << Error;
  ASSERT_TRUE(
      assignDarwinScalarSourceABI(Retain.Signature, F.Image.Arch, Error))
      << Error;
  ASSERT_TRUE(
      assignDarwinScalarSourceABI(Release.Signature, F.Image.Arch, Error))
      << Error;
  const std::map<va_t, SourceCallTypeHint> Calls{{0x1000, IntegerGetter},
                                                 {0x1008, BlockGetter},
                                                 {0x100c, Retain},
                                                 {0x1024, Release}};

  F.Low.Blocks[0].Ops = {
      op(NdOp::CALL, NdVar::reg(F.R0, 8), {NdVar::cst(0x2000, 8)}, 0x1000),
      op(NdOp::COPY, NdVar::reg(a64reg::X19, 8), {NdVar::reg(F.R0, 8)}, 0x1004),
      op(NdOp::CALL, NdVar::reg(F.R0, 8), {NdVar::cst(0x3000, 8)}, 0x1008),
      op(NdOp::CALL, NdVar::reg(F.R0, 8), {NdVar::cst(0x4000, 8)}, 0x100c),
      op(NdOp::INT_ADD, NdVar::tmp(0, 8),
         {NdVar::reg(F.R0, 8), NdVar::cst(16, 8)}, 0x1010),
      op(NdOp::LOAD, NdVar::reg(F.Target, 8), {NdVar::tmp(0, 8)}, 0x1010),
      op(NdOp::COPY, NdVar::reg(F.R1, 8), {NdVar::reg(a64reg::X19, 8)}, 0x1014),
      op(NdOp::INDIR_CALL, NdVar::reg(F.R0, 8), {NdVar::reg(F.Target, 8)},
         0x1018),
      op(NdOp::COPY, NdVar::reg(F.R0, 8), {NdVar::reg(F.R2, 8)}, 0x1020),
      op(NdOp::CALL, {}, {NdVar::cst(0x5000, 8)}, 0x1024),
      op(NdOp::RETURN, {}, {}, 0x1028)};

  auto Hints = buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &Calls);
  ASSERT_EQ(Hints.size(), 1U);
  EXPECT_EQ(Hints.at(0x1018).Signature.ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hints.at(0x1018).Signature.Parameters.size(), 2U);
  EXPECT_EQ(Hints.at(0x1018).Signature.Parameters[1].Type->Kind,
            NdTypeKind::Int);
  auto Missing = Calls;
  Missing.erase(0x1000);
  EXPECT_TRUE(
      buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &Missing).empty());
  Missing = Calls;
  Missing.at(0x1000).Signature.ReturnType = Pointer;
  EXPECT_TRUE(
      buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &Missing).empty());
}

TEST(ObjCBlockCallHints,
     RetainedObjectResultProvesBlockInvokeAcrossBoundCalls) {
  Fixture F(Arch::AArch64);
  const auto &TRI = getTargetRegInfo(F.Image.Arch);
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Getter = SourceCallTypeHint{};
  Getter.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Getter.Signature.ReturnType = Pointer;
  Getter.Signature.Parameters = {{"self", Pointer}, {"cmd", Pointer}};
  std::string Error;
  ASSERT_TRUE(assignDarwinObjCSourceABI(Getter.Signature, F.Image.Arch, Error))
      << Error;
  auto Retain = SourceCallTypeHint{};
  Retain.CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  Retain.TargetName = "objc_retainAutoreleasedReturnValue";
  Retain.Signature.ReturnType = Pointer;
  Retain.Signature.Parameters = {{"object", Pointer}};
  ASSERT_TRUE(
      assignDarwinScalarSourceABI(Retain.Signature, F.Image.Arch, Error))
      << Error;
  const std::map<va_t, SourceCallTypeHint> Calls{
      {0x1004, Getter}, {0x1008, Retain}, {0x1020, Retain}};
  auto &Ops = F.Low.Blocks[0].Ops;
  Ops = {
      op(NdOp::COPY, NdVar::reg(a64reg::X19, 8), {NdVar::reg(F.R2, 8)}, 0x1000),
      op(NdOp::CALL, NdVar::reg(F.R0, 8), {NdVar::cst(0x2000, 8)}, 0x1004),
      op(NdOp::CALL, NdVar::reg(F.R0, 8), {NdVar::cst(0x3000, 8)}, 0x1008),
      op(NdOp::INT_ADD, NdVar::tmp(0, 8),
         {NdVar::reg(F.R0, 8), NdVar::cst(16, 8)}, 0x100c),
      op(NdOp::LOAD, NdVar::reg(F.Target, 8), {NdVar::tmp(0, 8)}, 0x100c),
      op(NdOp::COPY, NdVar::reg(F.R1, 8), {NdVar::reg(a64reg::X19, 8)}, 0x1010),
      op(NdOp::COPY, NdVar::reg(F.R2, 8), {NdVar::reg(TRI.StackPointer, 8)},
         0x1014),
      op(NdOp::COPY, NdVar::reg(F.R3, 8), {NdVar::reg(TRI.StackPointer, 8)},
         0x1018),
      op(NdOp::INDIR_CALL, NdVar::reg(TRI.IntReturnReg, 8),
         {NdVar::reg(F.Target, 8)}, 0x101c),
      op(NdOp::CALL, NdVar::reg(F.R0, 8), {NdVar::cst(0x3000, 8)}, 0x1020),
      op(NdOp::RETURN, {}, {NdVar::reg(TRI.IntReturnReg, 8)}, 0x1024)};
  EXPECT_TRUE(F.hints().empty());
  const auto Hints = buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &Calls);
  ASSERT_EQ(Hints.size(), 1U);
  ASSERT_TRUE(Hints.count(0x101c));
  EXPECT_EQ(Hints.at(0x101c).CallKind, SourceCallTypeHint::Kind::BlockInvoke);
  EXPECT_EQ(Hints.at(0x101c).Signature.Parameters.size(), 4U);

  auto WrongCalls = Calls;
  WrongCalls.at(0x1008).TargetName = "objc_release";
  EXPECT_TRUE(
      buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &WrongCalls).empty());
  WrongCalls = Calls;
  WrongCalls.erase(0x1004);
  EXPECT_TRUE(
      buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &WrongCalls).empty());
  WrongCalls = Calls;
  WrongCalls.at(0x1004).CallKind = SourceCallTypeHint::Kind::SwiftRuntimeCall;
  EXPECT_TRUE(
      buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &WrongCalls).empty());
  WrongCalls = Calls;
  WrongCalls.erase(0x1020);
  EXPECT_TRUE(
      buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &WrongCalls).empty());
  Ops[3].Inputs[1] = NdVar::cst(8, 8);
  EXPECT_TRUE(
      buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &Calls).empty());
}

TEST(ObjCBlockCallHints,
     BoundCallStillInvalidatesPreviouslyLoadedInvokePointer) {
  Fixture F(Arch::AArch64);
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  SourceCallTypeHint Getter;
  Getter.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Getter.Signature.ReturnType = Pointer;
  Getter.Signature.Parameters = {{"self", Pointer}, {"cmd", Pointer}};
  std::string Error;
  ASSERT_TRUE(assignDarwinObjCSourceABI(Getter.Signature, F.Image.Arch, Error))
      << Error;
  const std::map<va_t, SourceCallTypeHint> Calls{{0x1014, Getter}};
  F.Low.Blocks[0].Ops = {
      op(NdOp::COPY, NdVar::reg(a64reg::X20, 8), {NdVar::reg(F.R2, 8)}, 0x1000),
      op(NdOp::COPY, NdVar::reg(a64reg::X21, 8), {NdVar::reg(F.R3, 8)}, 0x1004),
      op(NdOp::INT_ADD, NdVar::tmp(0, 8),
         {NdVar::reg(F.R2, 8), NdVar::cst(16, 8)}, 0x1010),
      op(NdOp::LOAD, NdVar::reg(a64reg::X19, 8), {NdVar::tmp(0, 8)}, 0x1010),
      op(NdOp::CALL, NdVar::reg(F.R0, 8), {NdVar::cst(0x2000, 8)}, 0x1014),
      op(NdOp::COPY, NdVar::reg(F.R0, 8), {NdVar::reg(a64reg::X20, 8)}, 0x1018),
      op(NdOp::COPY, NdVar::reg(F.R1, 8), {NdVar::reg(a64reg::X21, 8)}, 0x101c),
      op(NdOp::INDIR_CALL, NdVar::reg(F.R0, 8), {NdVar::reg(a64reg::X19, 8)},
         0x1020),
      op(NdOp::RETURN, {}, {NdVar::reg(F.R0, 8)}, 0x1024)};
  EXPECT_TRUE(
      buildObjCBlockCallHints(F.Image, F.Low, &F.Entry, &Calls).empty());
}

TEST(ObjCBlockCallHints,
     RejectsDifferentReceiverWrongOffsetWidthAndOrderedLoad) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
      Fixture F(Architecture);
      auto &Ops = F.Low.Blocks[0].Ops;
      if (Mutation == 0)
        Ops[1].Inputs[0] = NdVar::reg(F.R3, 8);
      if (Mutation == 1)
        Ops[2].Inputs[1] = NdVar::cst(8, 8);
      if (Mutation == 2)
        Ops[3].Output.Size = 4;
      if (Mutation == 3)
        Ops[3].MemoryOrdering = NdMemoryOrdering::Acquire;
      EXPECT_TRUE(F.hints().empty()) << int(Architecture) << ":" << Mutation;
    }
  }
}

TEST(ObjCBlockCallHints,
     InterveningMemoryOrCallsInvalidateLoadedTargetIdentity) {
  for (unsigned Mutation = 0; Mutation < 2; ++Mutation) {
    Fixture F(Arch::AArch64);
    auto &Ops = F.Low.Blocks[0].Ops;
    auto Invalidate = Mutation == 0
                          ? op(NdOp::STORE, {},
                               {NdVar::reg(F.R2, 8), NdVar::cst(0, 8)}, 0x100a)
                          : op(NdOp::CALL, {}, {NdVar::cst(0x2000, 8)}, 0x100a);
    Ops.insert(Ops.begin() + 4, Invalidate);
    EXPECT_TRUE(F.hints().empty()) << Mutation;
  }
}

TEST(ObjCBlockCallHints, RejectsUnmodelledFloatingBankGapsAndControlFlow) {
  for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
    Fixture F(Arch::AArch64);
    auto &Ops = F.Low.Blocks[0].Ops;
    if (Mutation == 0)
      Ops.insert(
          Ops.begin(),
          op(NdOp::COPY,
             NdVar::reg(getTargetRegInfo(F.Image.Arch).FPParamRegs[0], 8),
             {NdVar::cst(0, 8)}, 0xff0));
    if (Mutation == 1)
      Ops.insert(Ops.begin(), op(NdOp::COPY, NdVar::reg(F.R3, 8),
                                 {NdVar::cst(3, 8)}, 0xff0));
    if (Mutation == 2)
      F.Low.Blocks[0].Succs.push_back(0);
    EXPECT_TRUE(F.hints().empty()) << Mutation;
  }
}

TEST(ObjCBlockCallHints, TypedEntryDoesNotMakeAnUnwrittenRegisterAnArgument) {
  Fixture F(Arch::AArch64);
  F.Low.Blocks[0].Ops.erase(F.Low.Blocks[0].Ops.begin() + 1);
  EXPECT_TRUE(F.hints().empty());
  F.Entry.HasExplicitABI = false;
  EXPECT_TRUE(F.hints().empty());
}

TEST(ObjCBlockCallHints, ExactPrivateSpillsPreserveHiddenObjectAndNarrowValue) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    Fixture F(Architecture);
    const auto &TRI = getTargetRegInfo(Architecture);
    auto &Ops = F.Low.Blocks[0].Ops;
    Ops.erase(Ops.begin(), Ops.begin() + 2);
    Ops[0].Inputs[0] = NdVar::reg(F.R0, 8);
    const std::vector<LowOp> Prefix = {
        op(NdOp::INT_ADD, NdVar::tmp(0, 8),
           {NdVar::reg(TRI.StackPointer, 8), NdVar::cst(-16, 8)}, 0xff0),
        op(NdOp::STORE, {}, {NdVar::tmp(0, 8), NdVar::reg(F.R0, 8)}, 0xff0),
        op(NdOp::INT_ADD, NdVar::tmp(0, 8),
           {NdVar::reg(TRI.StackPointer, 8), NdVar::cst(-4, 8)}, 0xff4),
        op(NdOp::STORE, {}, {NdVar::tmp(0, 8), NdVar::reg(F.R1, 4)}, 0xff4),
        op(NdOp::INT_ADD, NdVar::tmp(0, 8),
           {NdVar::reg(TRI.StackPointer, 8), NdVar::cst(-16, 8)}, 0xff8),
        op(NdOp::LOAD, NdVar::reg(F.R0, 8), {NdVar::tmp(0, 8)}, 0xff8),
        op(NdOp::INT_ADD, NdVar::tmp(0, 8),
           {NdVar::reg(TRI.StackPointer, 8), NdVar::cst(-4, 8)}, 0xffc),
        op(NdOp::LOAD, NdVar::reg(F.R1, 4), {NdVar::tmp(0, 8)}, 0xffc),
        op(NdOp::INT_ZEXT, NdVar::reg(F.R1, 8), {NdVar::reg(F.R1, 4)}, 0xffc)};
    Ops.insert(Ops.begin(), Prefix.begin(), Prefix.end());
    auto Hints = buildObjCBlockCallHints(F.Image, F.Low);
    ASSERT_EQ(Hints.size(), 1U) << int(Architecture);
    EXPECT_EQ(Hints.begin()->second.Signature.Parameters[1].Type->Size, 4U);
    Ops.insert(Ops.begin() + 4,
               op(NdOp::STORE, {},
                  {NdVar::reg(F.R2, 8), NdVar::reg(TRI.StackPointer, 8)},
                  0xff6));
    EXPECT_TRUE(buildObjCBlockCallHints(F.Image, F.Low).empty());
  }
}

TEST(ObjCBlockCallHints, ForwardedA64ReturnKeepsFullMachineCarrier) {
  Fixture F(Arch::AArch64);
  F.Low.Blocks[0].Ops.back().Inputs[0] =
      NdVar::reg(240, 8); // LR, RET's branch target.
  auto Hints = F.hints();
  ASSERT_EQ(Hints.size(), 1U);
  EXPECT_EQ(Hints.begin()->second.Signature.ReturnType->Size, 8U);
}

TEST(ObjCBlockCallHints,
     LaterWideReadBeforeDefinitionPreventsFalseNarrowReturn) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    Fixture F(Architecture);
    const auto &TRI = getTargetRegInfo(Architecture);
    auto &Ops = F.Low.Blocks[0].Ops;
    Ops.insert(Ops.end() - 1, op(NdOp::COPY, NdVar::tmp(0, 4),
                                 {NdVar::reg(TRI.IntReturnReg, 4)}, 0x100e));
    Ops.back().Inputs[0].Size = 8;
    auto Hints = F.hints();
    ASSERT_EQ(Hints.size(), 1U);
    EXPECT_EQ(Hints.begin()->second.Signature.ReturnType->Size, 8U);
    Ops.insert(Ops.end() - 1, op(NdOp::COPY, NdVar::tmp(0, 8),
                                 {NdVar::reg(TRI.FPReturnReg, 8)}, 0x100f));
    EXPECT_TRUE(F.hints().empty());
    Ops.insert(Ops.end() - 2, op(NdOp::COPY, NdVar::reg(TRI.IntReturnReg, 8),
                                 {NdVar::cst(0, 8)}, 0x100e));
    EXPECT_TRUE(
        F.hints().empty()); // Overwriting the GPR does not kill the FP result.
  }
}

TEST(ObjCBlockCallHints, SourceOnlyPreSSAArgumentsAndReturnReachHighIR) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    Fixture F(Architecture);
    std::map<va_t, SourceFunctionTypeHint> Hints{{F.Low.Entry, F.Entry}};
    LowToMedConverter Converter;
    Converter.setBinaryImage(&F.Image);
    Converter.setSourceCallHintsEnabled(true);
    Converter.setSourceCalleeTypeHints(&Hints);
    auto Med = Converter.convert(F.Low, Architecture, BinaryFormat::MachO);
    recoverCallAbi(Med, Architecture, {}, &F.Image);
    ASSERT_EQ(Med.CallInfos.size(), 1U);
    ASSERT_TRUE(Med.CallInfos[0].SourceCallHint);
    ASSERT_EQ(Med.CallInfos[0].Args.size(), 2U);
    EXPECT_EQ(Med.CallInfos[0].Args[1].Size, 4U);
    auto High = MedToHighConverter().convert(Med, Architecture);
    unsigned Calls = 0;
    walkStmts(High.Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &Root) {
        std::vector<const HighExpr *> Work{Root.get()};
        while (!Work.empty()) {
          const auto *E = Work.back();
          Work.pop_back();
          if (!E)
            continue;
          if (E->SourceCallHint) {
            ++Calls;
            EXPECT_EQ(E->Operands.size(), 2U);
            EXPECT_EQ(E->Type->Size, 4U);
          }
          for (const auto &Operand : E->Operands)
            Work.push_back(Operand.get());
        }
      });
    });
    EXPECT_GT(Calls, 0U);
    Converter.setSourceCallHintsEnabled(false);
    auto Disabled = Converter.convert(F.Low, Architecture, BinaryFormat::MachO);
    for (const auto &B : Disabled.Blocks)
      for (const auto &Operation : B.Ops)
        EXPECT_FALSE(Operation.SourceCallHint);
  }
}
} // namespace
