#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Swift/SwiftABI.h"
#include "neverd/loader/Swift/SwiftMetadata.h"

#include "llvm/Support/Endian.h"

using namespace neverd;
namespace {
struct Fixture {
  BinaryImage Image;
  Fixture() {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Segment Segment;
    Segment.VA = 0x1000;
    Segment.Size = 0x1000;
    Segment.FileSz = 0x1000;
    Segment.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Segment.Data.resize(0x1000);
    Image.Segments.push_back(std::move(Segment));
    Section Types;
    Types.Name = "__swift5_types";
    Types.VA = 0x1000;
    Types.Size = 4;
    Types.FileSz = 4;
    Types.Flags = SegmentFlags::Readable;
    Image.Sections.push_back(Types);
    Section Data;
    Data.Name = "__data";
    Data.VA = 0x1004;
    Data.Size = 0xffc;
    Data.FileOff = 4;
    Data.FileSz = 0xffc;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Image.Sections.push_back(Data);
    relative(0x1000, 0x1100);
    u32(0x1100, 0x51);
    relative(0x1104, 0x1200);
    relative(0x1108, 0x1130);
    relative(0x1110, 0x1300);
    u32(0x1114, 1);
    u32(0x1118, 2);
    text(0x1130, "Counter");
    u32(0x1200, 0);
    relative(0x1208, 0x1210);
    text(0x1210, "Demo");
    u32(0x1308, 12u << 16);
    u32(0x130c, 1);
    u32(0x1310, 2);
    relative(0x1314, 0x1340);
    relative(0x1318, 0x1350);
    text(0x1340, "s5Int64V");
    text(0x1350, "value");
    u64(0x1400, 0x200);
    u64(0x1408, 0x1100);
    u32(0x1410, 0);
    Symbol Metadata;
    Metadata.Name = "metadata";
    Metadata.Addr = 0x1400;
    Image.Symbols.push_back(Metadata);
  }
  void u32(va_t Address, uint32_t Value) {
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + Address - 0x1000, Value);
  }
  void u64(va_t Address, uint64_t Value) {
    llvm::support::endian::write64le(
        Image.Segments[0].Data.data() + Address - 0x1000, Value);
  }
  void relative(va_t Address, va_t Target) {
    u32(Address, static_cast<uint32_t>(Target - Address));
  }
  void text(va_t Address, const std::string &Value) {
    std::copy(Value.c_str(), Value.c_str() + Value.size() + 1,
              Image.Segments[0].Data.begin() + Address - 0x1000);
  }
  void symbolicType() {
    Image.Segments[0].Data[0x340] = 2;
    relative(0x1341, 0x1500);
    Image.Segments[0].Data[0x345] = 0;
    Image.DyldBindSlots[0x1500] = {"_$ss5Int64VMn", 0};
  }
  void emptyStruct() {
    text(0x1130, "Empty");
    u32(0x1114, 0);
    u32(0x1118, 0);
    u32(0x130c, 0);
    u64(0x13f8, 0x1600);
    u64(0x1640, 0);
    u64(0x1648, 1);
    u32(0x1650, 0);
    u32(0x1654, 0);
  }
  void separateValueWitnessSection() {
    Image.Sections[1].Size = Image.Sections[1].FileSz = 0x5fc;
    Section Table;
    Table.Name = "__const";
    Table.VA = 0x1600;
    Table.Size = Table.FileSz = 88;
    Table.FileOff = 0x600;
    Table.Flags = SegmentFlags::Readable;
    Image.Sections.push_back(Table);
  }
};

struct SelfFixture : Fixture {
  SwiftSourceSignature Signature;
  SourceFunctionTypeHint Hint;
  LowFunc Function;
  SelfFixture() {
    Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable |
                              SegmentFlags::Executable;
    Image.Sections[1].Size = Image.Sections[1].FileSz = 0x7fc;
    Section Code;
    Code.Name = "__text";
    Code.VA = 0x1800;
    Code.Size = Code.FileSz = 0x100;
    Code.FileOff = 0x800;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Code.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    Image.Sections.push_back(Code);
    Signature.Entry = 0x1800;
    Signature.MangledSymbol = "_$s4Demo7CounterV3addys5Int64VAFF";
    Signature.Module = "Demo";
    Signature.ContextKind = "struct";
    Signature.ContextName = "Counter";
    Signature.Name = "add";
    Signature.Parameters = {
        {"arg0", {SwiftSourceType::Kind::Integer, "Int64", 64, true, nullptr}}};
    Signature.ReturnType = Signature.Parameters[0].Type;
    Signature.Labels = {"_"};
    Signature.ContextLayoutKnown = true;
    Signature.ContextFields = recoverSwiftTypes(Image)[0].Fields;
    Symbol Symbol;
    Symbol.Name = Signature.MangledSymbol;
    Symbol.Addr = Signature.Entry;
    Symbol.IsFunc = true;
    Image.Symbols.push_back(Symbol);
    Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
    Hint.Architecture = Image.Arch;
    Hint.HasExplicitABI = true;
    Hint.Parameters = {
        {"arg0",
         NdType::makeInt(8, true),
         {SourceABICarrierKind::IntegerRegister, a64reg::X0, 0, 8}}};
    Hint.ReturnType = NdType::makeInt(8, true);
    Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister, a64reg::X0, 0,
                           8};
    Function.Entry = Signature.Entry;
    LowBlock Block;
    Block.StartAddr = Function.Entry;
    Function.Blocks.push_back(Block);
  }
  void op(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Opcode = Opcode;
    Op.Output = Output;
    for (auto Input : Inputs)
      Op.addInput(Input);
    Function.Blocks[0].Ops.push_back(Op);
  }
  SwiftSelfABIProof proof() {
    return recoverSwiftSelfABI(Signature, Image, Function, Hint);
  }
};
} // namespace

TEST(SwiftSelfABI, FixedWordValueSelfMustContributeToActualReturn) {
  SelfFixture F;
  F.op(NdOp::INT_ADD, NdVar::reg(a64reg::X0, 8),
       {NdVar::reg(a64reg::X0, 8), NdVar::reg(a64reg::X1, 8)});
  F.op(NdOp::RETURN, {}, {NdVar::reg(a64reg::X0, 8)});
  auto P = F.proof();
  ASSERT_TRUE(P.Proven) << P.Reason;
  EXPECT_FALSE(P.IsMutating);
  EXPECT_EQ(P.SelfConvention, "direct-fields");
  ASSERT_EQ(P.SelfParameters.size(), 1u);
  EXPECT_EQ(P.SelfParameters[0].Name, "swift_self_0");
  EXPECT_EQ(P.SelfParameters[0].Location.RegisterOffset, a64reg::X1);
  F.Function.Blocks[0].Ops[0].Output = NdVar::tmp(TmpBase, 8);
  EXPECT_FALSE(F.proof().Proven);
  F.Function.Blocks[0].Ops[0].Output = NdVar::reg(a64reg::X0, 8);
  F.Function.Blocks[0].Ops[0].Opcode = NdOp::INT_SUB;
  F.Function.Blocks[0].Ops[0].Inputs[0] = NdVar::reg(a64reg::X1, 8);
  EXPECT_FALSE(F.proof().Proven);
  F.Function.Blocks[0].Ops[0].Opcode = NdOp::INT_XOR;
  EXPECT_FALSE(F.proof().Proven);
}

TEST(SwiftSelfABI, MutableSelfRequiresExactFieldStoreAndBoundValue) {
  SelfFixture F;
  F.Hint.ReturnType = NdType::makeVoid();
  F.Hint.ReturnLocation = {};
  F.Signature.ReturnType = {};
  F.op(NdOp::LOAD, NdVar::tmp(TmpBase, 8), {NdVar::reg(a64reg::X20, 8)});
  F.op(NdOp::INT_ADD, NdVar::tmp(TmpBase, 8),
       {NdVar::tmp(TmpBase, 8), NdVar::reg(a64reg::X0, 8)});
  F.op(NdOp::STORE, {}, {NdVar::reg(a64reg::X20, 8), NdVar::tmp(TmpBase, 8)});
  F.op(NdOp::RETURN, {}, {});
  auto P = F.proof();
  ASSERT_TRUE(P.Proven) << P.Reason;
  EXPECT_TRUE(P.IsMutating);
  EXPECT_EQ(P.SelfConvention, "indirect-mutating");
  EXPECT_EQ(P.SelfParameters[0].Location.RegisterOffset, a64reg::X20);
  F.Function.Blocks[0].Ops[2].Inputs[1] = NdVar::reg(a64reg::X19, 8);
  EXPECT_FALSE(F.proof().Proven);
}

TEST(SwiftSelfABI, GetterMustConsumeTheNativeValueSelfOnBothArchitectures) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SelfFixture F;
    F.Image.Arch = F.Hint.Architecture = Architecture;
    const auto &Registers = getTargetRegInfo(Architecture);
    F.Signature.DeclarationKind = "getter";
    F.Signature.Parameters.clear();
    F.Signature.Labels.clear();
    F.Hint.Parameters.clear();
    F.Hint.ReturnLocation.RegisterOffset = Registers.IntReturnReg;
    F.op(NdOp::COPY, NdVar::reg(Registers.IntReturnReg, 8),
         {NdVar::reg(Registers.IntParamRegs[0], 8)});
    F.op(NdOp::RETURN, {}, {NdVar::reg(Registers.IntReturnReg, 8)});
    auto P = F.proof();
    ASSERT_TRUE(P.Proven) << P.Reason;
    EXPECT_FALSE(P.IsMutating);
    EXPECT_EQ(P.SelfConvention, "direct-fields");
    ASSERT_EQ(P.SelfParameters.size(), 1u);
    EXPECT_EQ(P.SelfParameters[0].Location.RegisterOffset,
              Registers.IntParamRegs[0]);
    F.Function.Blocks[0].Ops[0].Inputs[0] = NdVar::cst(42, 8);
    EXPECT_FALSE(F.proof().Proven); // A getter declaration does not prove that
                                    // self is passed.
    F.Function.Blocks[0].Ops[0].Inputs[0] =
        NdVar::reg(Registers.IntParamRegs[0], 8);
    F.Signature.Parameters = {{"arg0", F.Signature.ReturnType}};
    EXPECT_FALSE(F.proof().Proven);
    F.Signature.Parameters.clear();
    F.Signature.ReturnType = {};
    EXPECT_FALSE(F.proof().Proven);
  }
}

TEST(SwiftSelfABI, SetterRequiresNativeMutableFieldStoreOnBothArchitectures) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SelfFixture F;
    F.Image.Arch = F.Hint.Architecture = Architecture;
    const auto &Registers = getTargetRegInfo(Architecture);
    const auto Self = Architecture == Arch::AArch64 ? a64reg::X20 : reg::R13;
    F.Signature.DeclarationKind = "setter";
    F.Signature.ReturnType = {};
    F.Hint.ReturnType = NdType::makeVoid();
    F.Hint.ReturnLocation = {};
    F.Hint.Parameters[0].Location.RegisterOffset = Registers.IntParamRegs[0];
    F.op(NdOp::STORE, {},
         {NdVar::reg(Self, 8), NdVar::reg(Registers.IntParamRegs[0], 8)});
    F.op(NdOp::RETURN, {}, {});
    auto P = F.proof();
    ASSERT_TRUE(P.Proven) << P.Reason;
    EXPECT_TRUE(P.IsMutating);
    EXPECT_EQ(P.SelfConvention, "indirect-mutating");
    ASSERT_EQ(P.SelfParameters.size(), 1u);
    EXPECT_EQ(P.SelfParameters[0].Location.RegisterOffset, Self);
    F.Function.Blocks[0].Ops[0].Opcode = NdOp::NOP;
    EXPECT_FALSE(
        F.proof().Proven); // A setter declaration alone cannot select an ABI.
    F.Function.Blocks[0].Ops[0].Opcode = NdOp::STORE;
    F.Function.Blocks[0].Ops[0].Inputs[1] =
        NdVar::reg(Registers.IntParamRegs[2], 8);
    EXPECT_FALSE(F.proof().Proven);
    F.Function.Blocks[0].Ops[0].Inputs[1] =
        NdVar::reg(Registers.IntParamRegs[0], 8);
    F.u32(0x1310, 0);
    F.Signature.ContextFields[0].IsMutable = false;
    EXPECT_FALSE(F.proof().Proven);
    F.u32(0x1310, 2);
    F.Signature.ContextFields[0].IsMutable = true;
    F.Signature.ReturnType = F.Signature.Parameters[0].Type;
    EXPECT_FALSE(F.proof().Proven);
    F.Signature.ReturnType = {};
    F.Signature.Parameters.clear();
    EXPECT_FALSE(F.proof().Proven);
  }
}

TEST(SwiftSelfABI, LayoutClaimsCallsAndAmbiguousControlFlowAreNotProof) {
  SelfFixture F;
  F.op(NdOp::COPY, NdVar::reg(a64reg::X0, 8), {NdVar::reg(a64reg::X1, 8)});
  F.op(NdOp::RETURN, {}, {NdVar::reg(a64reg::X0, 8)});
  F.Signature.ContextFields[0].Offset = 8;
  EXPECT_FALSE(F.proof().Proven);
  F.Signature.ContextFields[0].Offset = 0;
  F.Function.Blocks[0].Ops[0].Opcode = NdOp::CALL;
  EXPECT_FALSE(F.proof().Proven);
  F.Function.Blocks[0].Ops[0].Opcode = NdOp::COPY;
  F.Function.Blocks.emplace_back();
  EXPECT_FALSE(F.proof().Proven);
}

namespace {
struct TailFixture : SelfFixture {
  std::vector<LowFunc> Targets;
  uint64_t First = a64reg::X0, Self = a64reg::X1, Return = a64reg::X0;
  explicit TailFixture(Arch Architecture = Arch::AArch64) {
    if (Architecture == Arch::X64) {
      Image.Arch = Hint.Architecture = Architecture;
      const auto &Registers = getTargetRegInfo(Architecture);
      First = Registers.IntParamRegs[0];
      Self = Registers.IntParamRegs[1];
      Return = Registers.IntReturnReg;
      Hint.Parameters[0].Location.RegisterOffset = First;
      Hint.ReturnLocation.RegisterOffset = Return;
    }
    op(NdOp::CALL, NdVar::reg(Return, 8), {NdVar::cst(0x1840, 8)});
    op(NdOp::RETURN, {}, {NdVar::reg(Return, 8)});
    for (auto &Op : Function.Blocks[0].Ops)
      Op.Addr = Function.Entry;
    LowInstructionBoundary Boundary;
    Boundary.Address = Function.Entry;
    Boundary.Size = 4;
    Boundary.FirstOp = 0;
    Boundary.OpCount = 2;
    Boundary.Control = LowInstructionControl::TailCall;
    Boundary.ControlFlags =
        LowInstructionControlFlag::Call | LowInstructionControlFlag::Return;
    Boundary.Immediate = 0x1840;
    Function.Blocks[0].InstructionBoundaries = {Boundary};
    LowFunc Callee;
    Callee.Entry = 0x1840;
    LowBlock Block;
    Block.StartAddr = Callee.Entry;
    LowOp Add;
    Add.Opcode = NdOp::INT_ADD;
    Add.Output = NdVar::reg(Return, 8);
    Add.addInput(NdVar::reg(First, 8));
    Add.addInput(NdVar::reg(Self, 8));
    Block.Ops.push_back(Add);
    Block.Ops.push_back(Function.Blocks[0].Ops[1]);
    Callee.Blocks.push_back(Block);
    Targets.push_back(Callee);
  }
  SwiftSelfABIProof proof() {
    return recoverSwiftSelfABI(Signature, Image, Function, Hint, Targets);
  }
};
} // namespace

TEST(SwiftSelfABI,
     ExactTailTransferInspectsActualCalleeReturnOnBothArchitectures) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    TailFixture F(Architecture);
    auto P = F.proof();
    ASSERT_TRUE(P.Proven) << P.Reason;
    EXPECT_EQ(P.SelfConvention, "direct-fields");
    EXPECT_FALSE(P.IsMutating);
    EXPECT_EQ(P.SelfParameters[0].Location.RegisterOffset, F.Self);
    EXPECT_EQ(P.Evidence.size(), 3u);
    F.Targets[0].Blocks[0].Ops[0].Inputs[1] = NdVar::cst(0, 8);
    EXPECT_FALSE(
        F.proof().Proven); // Passing a register is not evidence of consumption.
    F.Targets.clear();
    EXPECT_FALSE(F.proof().Proven);
  }
}

TEST(SwiftSelfABI, TailTransferRequiresExactBoundaryTargetAndBalancedFrame) {
  TailFixture Ordinary;
  Ordinary.Function.Blocks[0].InstructionBoundaries[0].Control =
      LowInstructionControl::Call;
  EXPECT_FALSE(Ordinary.proof().Proven);
  TailFixture Indirect;
  Indirect.Function.Blocks[0].Ops[0].Inputs[0] = NdVar::reg(a64reg::X8, 8);
  EXPECT_FALSE(Indirect.proof().Proven);
  TailFixture WrongTarget;
  WrongTarget.Function.Blocks[0].InstructionBoundaries[0].Immediate = 0x1850;
  EXPECT_FALSE(WrongTarget.proof().Proven);
  TailFixture Missing;
  Missing.Function.Blocks[0].InstructionBoundaries.clear();
  EXPECT_FALSE(Missing.proof().Proven);
  TailFixture Overlap;
  auto Boundary = Overlap.Function.Blocks[0].InstructionBoundaries[0];
  Boundary.FirstOp = 1;
  Boundary.OpCount = 1;
  Overlap.Function.Blocks[0].InstructionBoundaries.push_back(Boundary);
  EXPECT_FALSE(Overlap.proof().Proven);
  TailFixture Frame;
  LowOp Sub;
  Sub.Opcode = NdOp::INT_SUB;
  Sub.Output = NdVar::reg(a64reg::SP, 8);
  Sub.addInput(Sub.Output);
  Sub.addInput(NdVar::cst(16, 8));
  Frame.Function.Blocks[0].Ops.insert(Frame.Function.Blocks[0].Ops.begin(),
                                      Sub);
  Frame.Function.Blocks[0].InstructionBoundaries[0].FirstOp = 1;
  EXPECT_FALSE(Frame.proof().Proven);
}

TEST(SwiftSelfABI, TailTargetsCannotReuseCallerLocalsOrInventDependencies) {
  TailFixture Local;
  LowOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Output = NdVar::tmp(TmpBase, 8);
  Copy.addInput(NdVar::reg(Local.Self, 8));
  Local.Function.Blocks[0].Ops.insert(Local.Function.Blocks[0].Ops.begin(),
                                      Copy);
  Local.Function.Blocks[0].InstructionBoundaries[0].FirstOp = 1;
  Local.Targets[0].Blocks[0].Ops[0].Inputs[1] = Copy.Output;
  EXPECT_FALSE(Local.proof().Proven);
  TailFixture Unbound;
  Unbound.Targets[0].Blocks[0].Ops[0].Inputs[0] = NdVar::reg(a64reg::X19, 8);
  EXPECT_FALSE(Unbound.proof().Proven);
  TailFixture Recursive;
  Recursive.Targets[0].Blocks[0] = Recursive.Function.Blocks[0];
  Recursive.Targets[0].Blocks[0].StartAddr = Recursive.Targets[0].Entry;
  EXPECT_FALSE(Recursive.proof().Proven);
  TailFixture Ambiguous;
  Ambiguous.Targets.push_back(Ambiguous.Targets[0]);
  EXPECT_FALSE(Ambiguous.proof().Proven);
}

TEST(SwiftMetadata, StoredFieldLayoutUsesActualOffsetVector) {
  Fixture F;
  auto Types = recoverSwiftTypes(F.Image);
  ASSERT_EQ(Types.size(), 1u);
  ASSERT_EQ(Types[0].Status, "recovered") << Types[0].Reason;
  EXPECT_EQ(Types[0].Module, "Demo");
  EXPECT_EQ(Types[0].Name, "Counter");
  EXPECT_EQ(Types[0].Size, 8u);
  EXPECT_EQ(Types[0].Alignment, 8u);
  ASSERT_EQ(Types[0].Fields.size(), 1u);
  EXPECT_EQ(Types[0].Fields[0].Name, "value");
  EXPECT_EQ(Types[0].Fields[0].Type.Name, "Int64");
  EXPECT_EQ(Types[0].Fields[0].Offset, 0u);
  EXPECT_TRUE(Types[0].Fields[0].IsMutable);
}

TEST(SwiftMetadata, ExactImportBindingResolvesSymbolicFieldType) {
  Fixture F;
  F.symbolicType();
  auto Types = recoverSwiftTypes(F.Image);
  ASSERT_EQ(Types.size(), 1u);
  EXPECT_EQ(Types[0].Status, "recovered") << Types[0].Reason;
  F.Image.DyldBindSlots[0x1500].Addend = 8;
  EXPECT_NE(recoverSwiftTypes(F.Image)[0].Status, "recovered");
}

TEST(SwiftMetadata, ConflictingImportedDescriptorsCannotEstablishAType) {
  Fixture F;
  F.symbolicType();
  F.Image.ConflictingImportStorageSlots.insert(0x1500);
  auto Types = recoverSwiftTypes(F.Image);
  ASSERT_EQ(Types.size(), 1u);
  EXPECT_NE(Types[0].Status, "recovered");
  EXPECT_NE(Types[0].Reason.find("exact imported descriptor"),
            std::string::npos);
}

TEST(SwiftMetadata, ChainedPointerRequiresExactResolvedSlotEvidence) {
  Fixture F;
  F.Image.MachOHasChainedFixups = true;
  EXPECT_NE(recoverSwiftTypes(F.Image)[0].Status, "recovered");
  F.Image.MachOResolvedChainedPointerSlots.insert(0x1408);
  EXPECT_EQ(recoverSwiftTypes(F.Image)[0].Status, "recovered");
}

TEST(SwiftMetadata, GenericLayoutAndUnexplainedPaddingRemainUnrecovered) {
  Fixture Generic;
  Generic.u32(0x1100, 0xd1);
  EXPECT_NE(recoverSwiftTypes(Generic.Image)[0].Status, "recovered");
  Fixture Padding;
  Padding.u32(0x1410, 16);
  auto Types = recoverSwiftTypes(Padding.Image);
  EXPECT_NE(Types[0].Status, "recovered");
  EXPECT_NE(Types[0].Reason.find("gap"), std::string::npos);
}

TEST(SwiftMetadata, TruncatedFieldRecordsAndDuplicateMetadataAreRejected) {
  Fixture Truncated;
  Truncated.u32(0x130c, 50000);
  EXPECT_NE(recoverSwiftTypes(Truncated.Image)[0].Status, "recovered");
  Fixture Ambiguous;
  Ambiguous.u64(0x1600, 0x200);
  Ambiguous.u64(0x1608, 0x1100);
  Symbol Other;
  Other.Name = "second_metadata";
  Other.Addr = 0x1600;
  Ambiguous.Image.Symbols.push_back(Other);
  EXPECT_NE(recoverSwiftTypes(Ambiguous.Image)[0].Status, "recovered");
}

TEST(SwiftMetadata, EmptyStructUsesTheActualLocalValueWitnessLayout) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    Fixture F;
    F.Image.Arch = Architecture;
    F.emptyStruct();
    F.separateValueWitnessSection();
    auto Types = recoverSwiftTypes(F.Image);
    ASSERT_EQ(Types.size(), 1u);
    ASSERT_EQ(Types[0].Status, "recovered") << Types[0].Reason;
    EXPECT_EQ(Types[0].Descriptor, 0x1100u);
    EXPECT_EQ(Types[0].Metadata, 0x1400u);
    EXPECT_EQ(Types[0].Module, "Demo");
    EXPECT_EQ(Types[0].Kind, "struct");
    EXPECT_EQ(Types[0].Name, "Empty");
    EXPECT_EQ(Types[0].Size, 0u);
    EXPECT_EQ(Types[0].Alignment, 1u);
    EXPECT_TRUE(Types[0].Fields.empty());
    EXPECT_TRUE(Types[0].Reason.empty());
    // This reader validates the declared ABI, not witness implementations.
    // Neither null nor non-null function slots are native body evidence.
    for (unsigned Index = 0; Index < 8; ++Index)
      F.u64(0x1600 + Index * 8, 0x1800 + Index * 16);
    EXPECT_EQ(recoverSwiftTypes(F.Image)[0].Status, "recovered");
  }
}

TEST(SwiftMetadata, EmptyStructRejectsNontrivialValueWitnessStorageAndTraits) {
  struct Case {
    va_t Address;
    uint64_t Value;
    bool Wide;
  };
  const Case Cases[] = {
      {0x1640, 1, true},
      {0x1640, UINT64_MAX, true},
      {0x1648, 0, true},
      {0x1648, 2, true},
      {0x1648, UINT64_MAX, true},
      {0x1650, 1, false},
      {0x1650, 7, false},
      {0x1650, 15, false},
      {0x1650, 0x10000, false},
      {0x1650, 0x20000, false},
      {0x1650, 0x40000, false},
      {0x1650, 0x80000, false},
      {0x1650, 0x100000, false},
      {0x1650, 0x200000, false},
      {0x1650, 0x400000, false},
      {0x1650, 0x800000, false},
      {0x1650, 0x1000000, false},
      {0x1650, 0x2000000, false},
      {0x1650, 0x80000000, false},
      {0x1654, 1, false},
      {0x1654, UINT32_MAX, false},
  };
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const auto &C : Cases) {
      SCOPED_TRACE(C.Address);
      SCOPED_TRACE(C.Value);
      Fixture F;
      F.Image.Arch = Architecture;
      F.emptyStruct();
      if (C.Wide)
        F.u64(C.Address, C.Value);
      else
        F.u32(C.Address, static_cast<uint32_t>(C.Value));
      auto Types = recoverSwiftTypes(F.Image);
      ASSERT_EQ(Types.size(), 1u);
      EXPECT_EQ(Types[0].Status, "unrecovered");
      EXPECT_NE(Types[0].Reason.find("plain empty struct contract"),
                std::string::npos);
      // The field-derived values alone must not make this a recovered type.
      EXPECT_EQ(Types[0].Size, 0u);
      EXPECT_EQ(Types[0].Alignment, 1u);
      EXPECT_TRUE(Types[0].Fields.empty());
    }
  }
}

TEST(SwiftMetadata, EmptyStructRequiresAnAvailableExactValueWitnessPointer) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const uint64_t Pointer :
         {uint64_t(0), uint64_t(0x1601), uint64_t(0x3000), UINT64_MAX - 7}) {
      SCOPED_TRACE(Pointer);
      Fixture F;
      F.Image.Arch = Architecture;
      F.emptyStruct();
      F.u64(0x13f8, Pointer);
      auto Types = recoverSwiftTypes(F.Image);
      ASSERT_EQ(Types.size(), 1u);
      EXPECT_EQ(Types[0].Status, "unrecovered");
      EXPECT_NE(Types[0].Reason.find("value-witness table is unavailable"),
                std::string::npos);
    }
    Fixture Chained;
    Chained.Image.Arch = Architecture;
    Chained.emptyStruct();
    Chained.Image.MachOHasChainedFixups = true;
    Chained.Image.MachOResolvedChainedPointerSlots.insert(0x1408);
    Chained.Image.MachOResolvedChainedPointerSlots.insert(0x13f0);
    auto Types = recoverSwiftTypes(Chained.Image);
    ASSERT_EQ(Types.size(), 1u);
    EXPECT_EQ(Types[0].Status, "unrecovered");
    EXPECT_NE(Types[0].Reason.find("value-witness table is unavailable"),
              std::string::npos);
    Chained.Image.MachOResolvedChainedPointerSlots.insert(0x13f8);
    EXPECT_EQ(recoverSwiftTypes(Chained.Image)[0].Status, "recovered");
    Chained.Image.MachOResolvedChainedPointerSlots.erase(0x13f8);
    EXPECT_EQ(recoverSwiftTypes(Chained.Image)[0].Status, "unrecovered");
    Chained.Image.MachOResolvedChainedPointerSlots.insert(0x13f8);
    Chained.Image.MachOChainedFixupsAmbiguous = true;
    Types = recoverSwiftTypes(Chained.Image);
    ASSERT_EQ(Types.size(), 1u);
    EXPECT_EQ(Types[0].Status, "unrecovered");
    EXPECT_NE(Types[0].Reason.find("ambiguous fixups"), std::string::npos);
    Chained.Image.MachOChainedFixupsAmbiguous = false;
    EXPECT_EQ(recoverSwiftTypes(Chained.Image)[0].Status, "recovered");
  }
}

TEST(SwiftMetadata, EmptyStructRequiresTheEntireFileBackedValueWitnessTable) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
      SCOPED_TRACE(Mutation);
      Fixture F;
      F.Image.Arch = Architecture;
      F.emptyStruct();
      F.separateValueWitnessSection();
      auto &Table = F.Image.Sections.back();
      switch (Mutation) {
      case 0:
        Table.FileSz = 87;
        break;
      case 1:
        Table.Size = 87;
        break;
      case 2:
        Table.Flags = SegmentFlags::None;
        break;
      case 3:
        Table.Type = llvm::MachO::S_ZEROFILL;
        break;
      case 4:
        Table.FileOff = 0x601;
        break;
      case 5:
        F.Image.Segments[0].FileSz = 0x657;
        break;
      case 6:
        F.Image.Sections[1].Size = F.Image.Sections[1].FileSz = 0x3f4;
        Section Metadata;
        Metadata.Name = "__metadata";
        Metadata.VA = 0x1400;
        Metadata.Size = Metadata.FileSz = 16;
        Metadata.FileOff = 0x400;
        Metadata.Flags = SegmentFlags::Readable;
        F.Image.Sections.push_back(Metadata);
        break;
      }
      auto Types = recoverSwiftTypes(F.Image);
      ASSERT_EQ(Types.size(), 1u);
      EXPECT_EQ(Types[0].Status, "unrecovered");
      EXPECT_NE(Types[0].Reason.find("value-witness table is unavailable"),
                std::string::npos);
    }
  }
}

TEST(SwiftMetadata, EmptyStructCannotTreatImportedPayloadsAsLocalTables) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
      SCOPED_TRACE(Mutation);
      Fixture F;
      F.Image.Arch = Architecture;
      F.emptyStruct();
      switch (Mutation) {
      case 0:
        F.Image.DyldBindSlots[0x13f8] = {"_$sytWV", 0};
        break;
      case 1:
        F.Image.DyldBindSlots[0x13f8] = {"_$sytWV", 8};
        break;
      case 2:
        F.Image.ImportPtrSlots[0x13f8] = "_$sytWV";
        break;
      case 3:
        F.Image.ImportStorageSlots[0x13f8] = {"_$sytWV", 0};
        break;
      case 4:
        F.Image.ConflictingImportStorageSlots.insert(0x13f8);
        break;
      case 5:
        F.Image.DyldBindSlots[0x13f8] = {"_$sytWV", 0};
        F.Image.ImportPtrSlots[0x13f8] = "another_table";
        break;
      case 6:
        F.Image.DyldBindSlots[0x13f8] = {"", 0};
        break;
      }
      auto Types = recoverSwiftTypes(F.Image);
      ASSERT_EQ(Types.size(), 1u);
      EXPECT_EQ(Types[0].Status, "unrecovered");
      EXPECT_NE(Types[0].Reason.find("table is external or ambiguous"),
                std::string::npos);
    }
    Fixture OtherSlot;
    OtherSlot.Image.Arch = Architecture;
    OtherSlot.emptyStruct();
    OtherSlot.Image.DyldBindSlots[0x13f0] = {"_$sytWV", 0};
    EXPECT_EQ(recoverSwiftTypes(OtherSlot.Image)[0].Status, "recovered");
  }
}

TEST(SwiftMetadata, EmptyStructUsesAnExactStrongStandardRuntimeBinding) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool Chained : {false, true}) {
      SCOPED_TRACE(Chained);
      Fixture F;
      F.Image.Arch = Architecture;
      F.emptyStruct();
      F.Image.MachOTwoLevelNamespace = true;
      F.Image.MachOHasChainedFixups = Chained;
      if (Chained)
        F.Image.MachOResolvedChainedPointerSlots.insert(0x1408);
      ASSERT_TRUE(F.Image.recordDyldBindSlot(
          0x13f8, "_$sytWV", 0, "/usr/lib/swift/libswiftCore.dylib", false));
      // Imported pointers are not dereferenced as local VWT addresses. This
      // local table deliberately cannot establish the required declaration.
      F.u64(0x1640, 42);
      for (uint64_t Payload :
           {uint64_t(0), uint64_t(0x1600), uint64_t(0x8000000000000000)}) {
        SCOPED_TRACE(Payload);
        F.u64(0x13f8, Payload);
        auto Types = recoverSwiftTypes(F.Image);
        ASSERT_EQ(Types.size(), 1u);
        ASSERT_EQ(Types[0].Status, "recovered") << Types[0].Reason;
        EXPECT_EQ(Types[0].Descriptor, 0x1100u);
        EXPECT_EQ(Types[0].Metadata, 0x1400u);
        EXPECT_EQ(Types[0].Size, 0u);
        EXPECT_EQ(Types[0].Alignment, 1u);
        EXPECT_TRUE(Types[0].Fields.empty());
        EXPECT_TRUE(Types[0].Reason.empty());
      }
      F.Image.MachOHasChainedFixups = true;
      F.Image.MachOResolvedChainedPointerSlots.insert(0x1408);
      F.Image.MachOChainedFixupsAmbiguous = true;
      const auto Ambiguous = recoverSwiftTypes(F.Image);
      ASSERT_EQ(Ambiguous.size(), 1u);
      EXPECT_EQ(Ambiguous[0].Status, "unrecovered");
      EXPECT_NE(Ambiguous[0].Reason.find("ambiguous fixups"),
                std::string::npos);
    }
  }
}

TEST(SwiftMetadata, EmptyStructRejectsInexactStandardRuntimeBindingContracts) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 16; ++Mutation) {
      SCOPED_TRACE(Mutation);
      Fixture F;
      F.Image.Arch = Architecture;
      F.emptyStruct();
      F.Image.MachOTwoLevelNamespace = true;
      std::string Name = "_$sytWV";
      std::string Module = "/usr/lib/swift/libswiftCore.dylib";
      int64_t Addend = 0;
      bool Weak = false;
      switch (Mutation) {
      case 0:
        F.Image.MachOTwoLevelNamespace = false;
        break;
      case 1:
        Weak = true;
        break;
      case 2:
        Module.clear();
        break;
      case 3:
        Module = "libswiftCore";
        break;
      case 4:
        Module = "@rpath/libswiftCore.dylib";
        break;
      case 5:
        Module = "/other/libswiftCore.dylib";
        break;
      case 6:
        Name = "$sytWV";
        break;
      case 7:
        Name = "__$sytWV";
        break;
      case 8:
        Name = "_$sytWV_suffix";
        break;
      case 9:
        Addend = 1;
        break;
      case 10:
        Addend = -1;
        break;
      case 13:
        Addend = 256;
        break;
      case 14:
        Addend = INT64_MIN;
        break;
      case 15:
        Addend = INT64_MAX;
        break;
      default:
        break;
      }
      ASSERT_TRUE(
          F.Image.recordDyldBindSlot(0x13f8, Name, Addend, Module, Weak));
      if (Mutation == 11)
        F.Image.DyldBindSlots.erase(0x13f8);
      if (Mutation == 12) {
        F.Image.DyldBindSlots.erase(0x13f8);
        F.Image.ImportStorageSlots.erase(0x13f8);
        F.Image.ImportPtrSlots[0x13f8] = "_$sytWV";
      }
      // A global same-name import never supplies a missing slot provider.
      Import Global;
      Global.Name = "_$sytWV";
      Global.Module = "/usr/lib/swift/libswiftCore.dylib";
      Global.IATAddr = 0x13f0;
      F.Image.Imports.push_back(Global);
      auto Types = recoverSwiftTypes(F.Image);
      ASSERT_EQ(Types.size(), 1u);
      EXPECT_EQ(Types[0].Status, "unrecovered");
      EXPECT_NE(Types[0].Reason.find("table is external or ambiguous"),
                std::string::npos);
    }
  }
}

TEST(SwiftMetadata, EmptyStructStandardBindingRequiresReadableExactHeaderSlot) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    Fixture WrongSlot;
    WrongSlot.Image.Arch = Architecture;
    WrongSlot.emptyStruct();
    WrongSlot.Image.MachOTwoLevelNamespace = true;
    WrongSlot.u64(0x13f8, 0);
    ASSERT_TRUE(WrongSlot.Image.recordDyldBindSlot(
        0x13f0, "_$sytWV", 0, "/usr/lib/swift/libswiftCore.dylib", false));
    auto Types = recoverSwiftTypes(WrongSlot.Image);
    ASSERT_EQ(Types.size(), 1u);
    EXPECT_EQ(Types[0].Status, "unrecovered");
    EXPECT_NE(Types[0].Reason.find("value-witness table is unavailable"),
              std::string::npos);

    Fixture MissingHeader;
    MissingHeader.Image.Arch = Architecture;
    MissingHeader.emptyStruct();
    MissingHeader.Image.MachOTwoLevelNamespace = true;
    ASSERT_TRUE(MissingHeader.Image.recordDyldBindSlot(
        0x13f8, "_$sytWV", 0, "/usr/lib/swift/libswiftCore.dylib", false));
    MissingHeader.Image.Sections[1].Size =
        MissingHeader.Image.Sections[1].FileSz = 0x3f4;
    Section Metadata;
    Metadata.Name = "__metadata";
    Metadata.VA = 0x1400;
    Metadata.Size = Metadata.FileSz = 16;
    Metadata.FileOff = 0x400;
    Metadata.Flags = SegmentFlags::Readable;
    MissingHeader.Image.Sections.push_back(Metadata);
    Types = recoverSwiftTypes(MissingHeader.Image);
    ASSERT_EQ(Types.size(), 1u);
    EXPECT_EQ(Types[0].Status, "unrecovered");
    EXPECT_NE(Types[0].Reason.find("value-witness table is unavailable"),
              std::string::npos);
  }
}

TEST(SwiftMetadata, EmptyStructStandardBindingConflictsCannotBeResurrected) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      SCOPED_TRACE(Mutation);
      Fixture F;
      F.Image.Arch = Architecture;
      F.emptyStruct();
      F.Image.MachOTwoLevelNamespace = true;
      ASSERT_TRUE(F.Image.recordDyldBindSlot(
          0x13f8, "_$sytWV", 0, "/usr/lib/swift/libswiftCore.dylib", false));
      ASSERT_EQ(recoverSwiftTypes(F.Image)[0].Status, "recovered");
      switch (Mutation) {
      case 0:
        ASSERT_FALSE(F.Image.recordDyldBindSlot(
            0x13f8, "_$sytWV", 0, "/other/libswiftCore.dylib", false));
        break;
      case 1:
        ASSERT_FALSE(F.Image.recordDyldBindSlot(
            0x13f8, "_$sytWV", 0, "/usr/lib/swift/libswiftCore.dylib", true));
        break;
      case 2:
        F.Image.ImportStorageSlots[0x13f8] = {
            "other_table", 0, ImportStorageEvidence::LoaderBind};
        break;
      case 3:
        F.Image.ImportStorageSlots[0x13f8] = {
            "_$sytWV", 8, ImportStorageEvidence::LoaderBind};
        break;
      case 4:
        F.Image.ConflictingImportStorageSlots.insert(0x13f8);
        break;
      }
      auto Types = recoverSwiftTypes(F.Image);
      ASSERT_EQ(Types.size(), 1u);
      EXPECT_EQ(Types[0].Status, "unrecovered");
      EXPECT_NE(Types[0].Reason.find("table is external or ambiguous"),
                std::string::npos);
      if (Mutation < 2) {
        EXPECT_FALSE(F.Image.recordDyldBindSlot(
            0x13f8, "_$sytWV", 0, "/usr/lib/swift/libswiftCore.dylib", false));
        EXPECT_EQ(recoverSwiftTypes(F.Image)[0].Status, "unrecovered");
      }
    }
  }
}
