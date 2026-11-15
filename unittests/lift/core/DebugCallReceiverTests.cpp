//===- DebugCallReceiverTests.cpp - Scoped receiver evidence -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Register copies, partial writes, call identity and conflicting declarations.
//===----------------------------------------------------------------------===//

#include "../../../lib/pipeline/PipelineDebugReceivers.h"
#include "gtest/gtest.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

namespace {
using namespace neverd;

struct VirtualArgumentsFixture : NullDebugContext {
  BinaryImage Image;
  LowFunc Function;
  FunctionSym Entry, Method;
  unsigned First, Scratch;

  explicit VirtualArgumentsFixture(Arch Architecture) {
    Image.Arch = Architecture;
    Image.Format = BinaryFormat::ELF;
    Image.Bits = Bitness::Bits64;
    Segment Table;
    Table.VA = 0x6000;
    Table.Size = Table.FileSz = 24;
    Table.Flags = SegmentFlags::Readable;
    Table.Data.resize(24);
    writeLE<uint64_t>(Table.Data.data() + 16, 0x8000);
    Segment Code;
    Code.VA = 0x8000;
    Code.Size = Code.FileSz = 16;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Code.Data.resize(16);
    Image.Segments = {Table, Code};
    Image.CodePtrRelocSlots.insert(0x6010);
    Image.Symbols = {{"_ZTV6Device", 0x6000, 24, false},
                     {"_ZN6Device6updateEyfd", 0x8000, 16, true}};
    Entry.Addr = Function.Entry = 0x1000;
    Entry.ReturnType = NdType::makeVoid();
    Entry.PlatformConvention = true;
    Entry.Params = {
        {"object", NdType::makePtr(NdType::makeNamedRecord("Device", 8))},
        {"value", NdType::makeInt(8, false)},
        {"delta", NdType::makeFloat(4)},
        {"weight", NdType::makeFloat(8)}};
    Method = Entry;
    Method.Addr = 0x8000;
    const auto &TRI = getTargetRegInfo(Architecture);
    First = TRI.integerParamRegs(Image.Format).front();
    Scratch = TRI.integerParamRegs(Image.Format)[4];
    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = Function.Entry;
    LowOp Vptr;
    Vptr.Opcode = NdOp::LOAD;
    Vptr.Addr = 0x1000;
    Vptr.Output = NdVar::reg(Scratch, 8);
    Vptr.addInput(NdVar::reg(First, 8));
    LowOp Target = Vptr;
    Target.Addr = 0x1004;
    Target.Inputs[0] = Vptr.Output;
    LowOp Call;
    Call.Opcode = NdOp::INDIR_CALL;
    Call.Addr = 0x1008;
    Call.addInput(Target.Output);
    Block.Ops = {Vptr, Target, Call};
    Function.Blocks = {Block};
  }

  std::optional<FunctionSym> resolveFunction(va_t Address) const override {
    if (Address == Entry.Addr)
      return Entry;
    if (Address == Method.Addr)
      return Method;
    return {};
  }

  DebugVirtualCallArguments run() {
    const ItaniumVTableIndex Tables(Image);
    return debugVirtualCallArguments(
        Image, Function, *this,
        [&]() -> const ItaniumVTableIndex & { return Tables; });
  }
};

TEST(DebugCallReceivers,
     VirtualSignaturesIncludeUntouchedIntegerAndFloatInputs) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    VirtualArgumentsFixture F(Architecture);
    const auto Calls = F.run();
    ASSERT_EQ(Calls.Floating.size(), 1u);
    const auto &Parameters = Calls.Floating.begin()->second.Scalars;
    ASSERT_EQ(Parameters.size(), 4u);
    EXPECT_TRUE(Parameters[0].Pointer);
    EXPECT_FALSE(Parameters[1].Floating);
    EXPECT_EQ(Parameters[1].Bytes, 8u);
    EXPECT_TRUE(Parameters[2].Floating);
    EXPECT_EQ(Parameters[2].Bytes, 4u);
    EXPECT_TRUE(Parameters[3].Floating);
    EXPECT_EQ(Parameters[3].Bytes, 8u);
    F.Entry.Params.resize(2);
    F.Method.Params.resize(2);
    const auto Integer = F.run();
    ASSERT_EQ(Integer.Integers.size(), 1u);
    EXPECT_EQ(Integer.Integers.begin()->second.Parameters.size(), 2u);
  }
}

TEST(DebugCallReceivers, VirtualSignaturesRequireReceiverAndSlotEvidence) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Changed = 0; Changed < 8; ++Changed) {
      VirtualArgumentsFixture F(Architecture);
      if (Changed == 0)
        F.Image.CodePtrRelocSlots.clear();
      if (Changed == 1)
        F.Method.IsVariadic = true;
      if (Changed == 2)
        F.Method.PlatformConvention = false;
      if (Changed == 3) {
        LowOp Write;
        Write.Opcode = NdOp::COPY;
        Write.Addr = 0x1006;
        Write.Output = NdVar::reg(F.First, 4);
        Write.addInput(NdVar::cst(0, 4));
        F.Function.Blocks[0].Ops.insert(F.Function.Blocks[0].Ops.begin() + 2,
                                        Write);
      }
      if (Changed == 4) {
        F.Function.Blocks[0].Ops[0].Opcode = NdOp::INTRINSIC;
      }
      if (Changed == 5)
        F.Method.CallConv = DebugCallConv::Fastcall;
      if (Changed == 6)
        F.Method.CallConv = DebugCallConv::Thiscall;
      if (Changed == 7)
        F.Method.ParamEntryLocations.resize(F.Method.Params.size());
      const auto Calls = F.run();
      EXPECT_TRUE(Calls.Integers.empty()) << Changed;
      EXPECT_TRUE(Calls.Floating.empty()) << Changed;
    }
}

TEST(DebugCallReceivers, MixedSignaturesValidateTheirIntegerReturnRegister) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &Registers = getTargetRegInfo(Architecture);
    for (uint16_t Bytes : {4, 8}) {
      const auto Signature = floatingCallArguments(
          Architecture, BinaryFormat::ELF, NdType::makeInt(Bytes),
          {NdType::makePtr(), NdType::makeFloat(4), NdType::makeInt(4)});
      ASSERT_TRUE(Signature);
      MedOp Call;
      Call.Opcode = NdOp::INDIR_CALL;
      Call.ScalarCallSignature =
          std::make_shared<FloatingCallArguments>(*Signature);
      Call.addInput(MedVar::makeConst(0x1000, 8));
      for (const auto &Parameter : Signature->Scalars)
        Call.addInput(MedVar::makeConst(0, Parameter.Bytes));
      Call.Output.Kind = MedVar::Reg;
      Call.Output.Id = 1;
      Call.Output.RegOff = Registers.IntReturnReg;
      Call.Output.Size = Bytes;
      ASSERT_TRUE(
          boundFloatingCallArguments(Call, Architecture, BinaryFormat::ELF));
      auto Broken = Call;
      Broken.Output.RegOff += Registers.FullRegWidth;
      EXPECT_FALSE(
          boundFloatingCallArguments(Broken, Architecture, BinaryFormat::ELF));
      Broken = Call;
      Broken.Output.Size = 2;
      EXPECT_FALSE(
          boundFloatingCallArguments(Broken, Architecture, BinaryFormat::ELF));
    }
  }
}

struct Fixture : NullDebugContext {
  BinaryImage Image;
  LowFunc Low;
  MedFunc Med;
  HighFunc High;
  TypeRef Pointer = NdType::makePtr(NdType::makeNamedRecord("Extended", 16));
  uint64_t First, Saved;
  va_t ScopeBegin = 0x1020, ScopeEnd = 0x1040;
  bool Conflict = false;

  explicit Fixture(Arch A) {
    Image.Arch = A;
    Image.Format = BinaryFormat::ELF;
    const auto &TRI = getTargetRegInfo(A);
    Pointer->Size = TRI.PointerSize;
    First =
        TRI.IntParamRegs.empty() ? TRI.IntReturnReg : TRI.IntParamRegs.front();
    Saved = TRI.IntParamRegs.empty() ? First + TRI.FullRegWidth
                                     : TRI.IntParamRegs.back();
    Low.Entry = Med.Entry = High.Entry = 0x1000;
    LowBlock LB;
    LB.Id = 0;
    LowOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Output = NdVar::reg(First, Pointer->Size);
    Copy.Inputs[0] = NdVar::reg(Saved, Pointer->Size);
    Copy.NumInputs = 1;
    Copy.Addr = 0x1020;
    LowOp Call;
    Call.Opcode = NdOp::INDIR_CALL;
    Call.Inputs[0] = NdVar::reg(0x300, Pointer->Size);
    Call.NumInputs = 1;
    Call.Addr = 0x1030;
    LB.Ops = {Copy, Call};
    Low.Blocks = {LB};

    MedBlock MB;
    MB.Id = 0;
    MedOp MC;
    MC.Opcode = NdOp::COPY;
    MC.Output.Kind = MedVar::Reg;
    MC.Output.RegOff = First;
    MC.Output.Size = Pointer->Size;
    MC.Inputs[0].Kind = MedVar::Temp;
    MC.Inputs[0].Id = 7;
    MC.Inputs[0].Size = Pointer->Size;
    MC.NumInputs = 1;
    MC.Addr = Copy.Addr;
    MedOp MI;
    MI.Opcode = NdOp::INDIR_CALL;
    MI.Addr = Call.Addr;
    MI.OriginSeq = Call.Seq;
    MB.Ops = {MC, MI};
    Med.Blocks = {MB};
    MedCallInfo Info;
    Info.BlockId = 0;
    Info.OpIdx = 1;
    Info.IsIndirect = true;
    Info.Args = {MC.Inputs[0]};
    Med.CallInfos = {Info};
    HighStmt S;
    S.Kind = StmtKind::Call;
    S.CallExpr =
        HighExpr::makeCall("", Call.Addr, {HighExpr::makeVar(MC.Inputs[0])});
    S.CallExpr->IsIndirectCall = true;
    S.CallExpr->CallSiteAddr = Call.Addr;
    High.Body = {S};
  }

  bool hasAuthenticatedObjectExtents() const override { return true; }
  VariableExtentLookup resolveRegisterVariableAt(va_t Entry, va_t PC,
                                                 uint64_t R) const override {
    EXPECT_EQ(Entry, Low.Entry);
    if (PC < ScopeBegin || PC >= ScopeEnd)
      return VariableExtentLookup::notFound();
    if (R == Saved)
      return VariableExtentLookup::unique({"child", Pointer});
    if (Conflict && R == First)
      return VariableExtentLookup::unique(
          {"other", NdType::makePtr(NdType::makeNamedRecord("Other", 16))});
    return VariableExtentLookup::notFound();
  }

  std::map<va_t, TypeRef> run() {
    return debugCallReceiverTypes(Image, Low, Med, High, *this);
  }
};

TEST(DebugCallReceivers, CopiesUseTheCallScopeOnBothPointerWidths) {
  for (Arch A : {Arch::AArch64, Arch::X64, Arch::ARM}) {
    Fixture F(A);
    auto Facts = F.run();
    ASSERT_EQ(Facts.size(), 1u) << int(A);
    EXPECT_EQ(Facts.at(0x1030), F.Pointer);
    F.ScopeEnd = 0x1030;
    EXPECT_TRUE(F.run().empty());
    F.ScopeEnd = 0x1040;
    F.ScopeBegin = 0x1031;
    EXPECT_TRUE(F.run().empty());
  }
}

TEST(DebugCallReceivers,
     PointerOperandsRequireTheOriginalOperationAndUseScope) {
  for (Arch A : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    Fixture F(A);
    LowOp Native;
    Native.Opcode = NdOp::INT_ADD;
    Native.Addr = F.ScopeBegin;
    Native.Seq = 3;
    Native.addInput(NdVar::reg(F.Saved, F.Pointer->Size));
    Native.addInput(NdVar::cst(8, F.Pointer->Size));
    F.Low.Blocks[0].Ops = {Native};
    MedOp Op;
    Op.Opcode = Native.Opcode;
    Op.Addr = Native.Addr;
    Op.OriginSeq = Native.Seq;
    MedVar Value;
    Value.Kind = MedVar::Reg;
    Value.Id = 9;
    Value.SSAVer = 2;
    Value.RegOff = F.Saved;
    Value.Size = F.Pointer->Size;
    Op.addInput(Value);
    Op.addInput(MedVar::makeConst(8, F.Pointer->Size));
    F.Med.Blocks[0].Ops = {Op};
    auto Run = [&] {
      return debugPointerOperandTypes(F.Image, F.Low, F.Med, F);
    };
    const auto Facts = Run();
    ASSERT_EQ(Facts.size(), 1u);
    ASSERT_EQ(Facts.begin()->second.size(), 1u);
    EXPECT_EQ(Facts.begin()->second[0].Value, Value);
    EXPECT_EQ(Facts.begin()->second[0].Type, F.Pointer);
    for (unsigned Change = 0; Change < 8; ++Change) {
      auto &Changed = F.Med.Blocks[0].Ops[0];
      Changed = Op;
      F.Low.Blocks[0].Ops = {Native};
      if (Change == 0)
        Changed.Inputs[1].ConstVal += 1;
      if (Change == 1)
        Changed.Inputs[0].Size /= 2;
      if (Change == 2)
        Changed.OriginSeq += 1;
      if (Change == 3)
        Changed.Dead = true;
      if (Change == 4)
        Changed.Opcode = NdOp::INT_SUB;
      if (Change == 5) {
        Changed.Addr = F.ScopeEnd;
        F.Low.Blocks[0].Ops[0].Addr = F.ScopeEnd;
      }
      if (Change == 6)
        F.Low.Blocks[0].Ops[0].Inputs[0].Size /= 2;
      if (Change == 7) {
        LowOp Partial;
        Partial.Addr = Native.Addr;
        Partial.Opcode = NdOp::COPY;
        Partial.Output = NdVar::reg(F.Saved + F.Pointer->Size - 1, 1);
        Partial.addInput(NdVar::cst(0, 1));
        F.Low.Blocks[0].Ops.insert(F.Low.Blocks[0].Ops.begin(), Partial);
      }
      EXPECT_TRUE(Run().empty()) << int(A) << ':' << Change;
    }
  }
}

TEST(DebugCallReceivers,
     PointerOperandsSurviveSSAAndEliminatedNativeReadCopies) {
  for (Arch A : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    Fixture F(A);
    const auto Bytes = F.Pointer->Size;
    LowOp Read;
    Read.Opcode = NdOp::COPY;
    Read.Addr = F.ScopeBegin;
    Read.Output = NdVar::tmp(65536, Bytes);
    Read.addInput(NdVar::reg(F.Saved, Bytes));
    LowOp Add;
    Add.Opcode = NdOp::INT_ADD;
    Add.Addr = Read.Addr;
    Add.Seq = 1;
    Add.addInput(Read.Output);
    Add.addInput(NdVar::cst(8, Bytes));
    F.Low.Blocks[0].Ops = {Read, Add};
    MedVar Captured;
    Captured.Kind = MedVar::Temp;
    Captured.Id = 19;
    Captured.SSAVer = 2;
    Captured.Size = Bytes;
    MedOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Addr = Read.Addr;
    Copy.OriginSeq = Read.Seq;
    Copy.Output.Kind = MedVar::Temp;
    Copy.Output.Id = 4;
    Copy.Output.Size = Bytes;
    Copy.addInput(Captured);
    MedOp Address;
    Address.Opcode = NdOp::INT_ADD;
    Address.Addr = Add.Addr;
    Address.OriginSeq = Add.Seq;
    Address.addInput(Captured);
    Address.addInput(MedVar::makeConst(8, Bytes));
    F.Med.Blocks[0].Ops = {Copy, Address};
    auto Run = [&] {
      return debugPointerOperandTypes(F.Image, F.Low, F.Med, F);
    };
    auto Facts = Run();
    ASSERT_EQ(Facts.size(), 1u);
    ASSERT_EQ(Facts.begin()->second.size(), 1u);
    EXPECT_EQ(Facts.begin()->second[0].Value, Captured);
    // The surviving address operation binds the final SSA value even when
    // every intermediate register copy has been removed by DCE.
    F.Med.Blocks[0].Ops = {Address};
    const auto Pruned = Run();
    ASSERT_EQ(Pruned.size(), 1u);
    EXPECT_EQ(Pruned.begin()->second[0].Value, Captured);
    for (unsigned Change = 0; Change < 8; ++Change) {
      F.Low.Blocks[0].Ops = {Read, Add};
      F.Med.Blocks[0].Ops = {Copy, Address};
      if (Change == 0)
        F.Med.Blocks[0].Ops[1].Inputs[1].ConstVal += 1;
      if (Change == 1)
        F.Med.Blocks[0].Ops[1].Inputs[0].Size /= 2;
      if (Change == 2)
        F.Low.Blocks[0].Ops[0].Opcode = NdOp::INT_ADD;
      if (Change == 3) {
        F.Low.Blocks[0].Ops[1].Addr += 4;
        F.Med.Blocks[0].Ops[1].Addr += 4;
      }
      if (Change == 4)
        F.Low.Blocks[0].Ops[0].Output.Size /= 2;
      if (Change == 5)
        F.Med.Blocks[0].Ops[1].Opcode = NdOp::INT_SUB;
      if (Change == 6)
        F.Med.Blocks[0].Ops.push_back(Address);
      if (Change == 7) {
        F.Low.Blocks[0].Ops[1].Inputs[1] = NdVar::reg(F.First, Bytes);
        F.Med.Blocks[0].Ops[1].Inputs[1] = Captured;
      }
      EXPECT_TRUE(Run().empty()) << int(A) << ':' << Change;
    }
  }
}

TEST(DebugCallReceivers, HighArgumentCollectionRequiresTheSameReceiverValue) {
  Fixture F(Arch::X64);
  F.Med.CallInfos.clear();
  ASSERT_EQ(F.run().size(), 1u);
  auto &Call = F.High.Body.front().CallExpr;
  auto &Argument = Call->Operands.front();
  ++Argument->Var.Id;
  EXPECT_TRUE(F.run().empty());
  --Argument->Var.Id;
  auto Other = F.High.Body.front();
  Other.CallExpr = std::make_shared<HighExpr>(*Call);
  Other.CallExpr->Operands = {HighExpr::makeConst(0, 8)};
  F.High.Body.push_back(Other);
  EXPECT_TRUE(F.run().empty());
  F.High.Body.pop_back();
  F.High.Body.front().CallExpr->CallSiteAddr = 0;
  EXPECT_TRUE(F.run().empty());
}

TEST(DebugCallReceivers, ChangedRegistersArgumentsAndDeclarationsStayUnknown) {
  for (unsigned Change = 0; Change != 8; ++Change) {
    SCOPED_TRACE(Change);
    Fixture F(Arch::AArch64);
    auto Write = F.Low.Blocks[0].Ops.front();
    Write.Addr += 4;
    Write.Inputs[0] = NdVar::cst(0, F.Pointer->Size);
    if (Change == 0 || Change == 1) {
      Write.Output = NdVar::reg(Change == 0 ? F.Saved : F.First, 4);
      F.Low.Blocks[0].Ops.insert(F.Low.Blocks[0].Ops.begin() + 1, Write);
    } else if (Change == 2) {
      F.Low.Blocks[0].Ops.front().Opcode = NdOp::INT_ADD;
    } else if (Change == 3) {
      Write.Opcode = NdOp::INTRINSIC;
      F.Low.Blocks[0].Ops.insert(F.Low.Blocks[0].Ops.begin() + 1, Write);
    } else if (Change == 4) {
      F.Med.CallInfos.front().Args.front().Id++;
    } else if (Change == 5) {
      F.Med.CallInfos.front().ArgumentsInCalleeRegisterOrder = true;
    } else if (Change == 6) {
      F.Conflict = true;
    } else {
      F.Med.Blocks[0].Ops.back().OriginSeq++;
    }
    EXPECT_TRUE(F.run().empty());
  }
}
} // namespace
