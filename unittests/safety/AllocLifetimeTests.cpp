//===- AllocLifetimeTests.cpp - Heap lifetime defect detection -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/safety/AllocLifetime.h"

using namespace neverd;
using namespace neverd::safety;

namespace {

constexpr uint64_t kSP = 0x1000;

MedVar temp(int Id, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = Size;
  return V;
}

MedVar mkReg(uint64_t Off, int Ver, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = static_cast<int>(Off);
  V.RegOff = Off;
  V.SSAVer = Ver;
  V.Size = Size;
  return V;
}

LowOp lowOp(NdOp Opcode, NdVar Output, std::vector<NdVar> Inputs,
            va_t Address = 0) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.Addr = Address;
  if (Address != 0)
    Op.Seq = 0;
  for (NdVar &Input : Inputs)
    Op.addInput(Input);
  return Op;
}

LowFunc solverHeavyReturnedPath(va_t EntryVA) {
  constexpr uint64_t kInputA = 16;
  constexpr uint64_t kInputB = 24;
  constexpr uint64_t kAValue = 201;
  constexpr uint64_t kA = 202;
  constexpr uint64_t kBValue = 203;
  constexpr uint64_t kB = 204;
  constexpr uint64_t kNotB = 205;
  constexpr uint64_t kClause1 = 206;
  constexpr uint64_t kClause2 = 207;
  constexpr uint64_t kFlag = 208;

  LowFunc F;
  F.Entry = EntryVA;
  F.DecodedInstructionCount = 1;
  F.LiftedInstructionCount = 1;
  F.Blocks.resize(3);
  for (int I = 0; I < 3; ++I)
    F.Blocks[I].Id = I;

  LowBlock &Entry = F.Blocks[0];
  Entry.StartAddr = EntryVA;
  Entry.EndAddr = EntryVA + 0x10;
  Entry.Succs = {1, 2};
  Entry.Ops.push_back(lowOp(NdOp::INT_AND, NdVar::reg(kAValue, 8),
                            {NdVar::reg(kInputA, 8), NdVar::cst(1, 8)}));
  Entry.Ops.push_back(lowOp(NdOp::INT_NOTEQUAL, NdVar::reg(kA, 1),
                            {NdVar::reg(kAValue, 8), NdVar::cst(0, 8)}));
  Entry.Ops.push_back(lowOp(NdOp::INT_AND, NdVar::reg(kBValue, 8),
                            {NdVar::reg(kInputB, 8), NdVar::cst(1, 8)}));
  Entry.Ops.push_back(lowOp(NdOp::INT_NOTEQUAL, NdVar::reg(kB, 1),
                            {NdVar::reg(kBValue, 8), NdVar::cst(0, 8)}));
  Entry.Ops.push_back(
      lowOp(NdOp::INT_NOT, NdVar::reg(kNotB, 1), {NdVar::reg(kB, 1)}));
  Entry.Ops.push_back(lowOp(NdOp::INT_OR, NdVar::reg(kClause1, 1),
                            {NdVar::reg(kA, 1), NdVar::reg(kB, 1)}));
  Entry.Ops.push_back(lowOp(NdOp::INT_OR, NdVar::reg(kClause2, 1),
                            {NdVar::reg(kA, 1), NdVar::reg(kNotB, 1)}));
  Entry.Ops.push_back(
      lowOp(NdOp::INT_AND, NdVar::reg(kFlag, 1),
            {NdVar::reg(kClause1, 1), NdVar::reg(kClause2, 1)}));
  Entry.Ops.push_back(
      lowOp(NdOp::COND_BR, NdVar{},
            {NdVar::cst(EntryVA + 0x10, 8), NdVar::reg(kFlag, 1)}));

  LowBlock &Read = F.Blocks[1];
  Read.StartAddr = EntryVA + 0x10;
  Read.EndAddr = EntryVA + 0x20;
  Read.Preds = {0};
  Read.Ops.push_back(lowOp(NdOp::LOAD, NdVar::tmp(209, 8), {NdVar::reg(kSP, 8)},
                           EntryVA + 0x18));
  Read.Ops.push_back(lowOp(NdOp::RETURN, NdVar{}, {}));

  LowBlock &Exit = F.Blocks[2];
  Exit.StartAddr = EntryVA + 0x20;
  Exit.EndAddr = EntryVA + 0x30;
  Exit.Preds = {0};
  Exit.Ops.push_back(lowOp(NdOp::RETURN, NdVar{}, {}));
  return F;
}

LowFunc straightLineCallReturnPath(va_t EntryVA, va_t CallVA, va_t TargetVA,
                                   Arch A) {
  const TargetRegInfo &TRI = getTargetRegInfo(A);
  LowFunc F;
  F.Entry = EntryVA;
  F.DecodedInstructionCount = 2;
  F.LiftedInstructionCount = 2;
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = EntryVA;
  Block.EndAddr = CallVA + 8;
  Block.Ops.push_back(lowOp(NdOp::CALL,
                            NdVar::reg(TRI.IntReturnReg, TRI.PointerSize),
                            {NdVar::cst(TargetVA, TRI.PointerSize)}, CallVA));
  Block.Ops.push_back(lowOp(NdOp::RETURN, NdVar{}, {}, CallVA + 4));
  F.Blocks.push_back(std::move(Block));
  return F;
}

struct FB {
  MedFunc F;
  FB(const std::string &Name, va_t Entry) {
    F.Name = Name;
    F.Entry = Entry;
  }
  int block() {
    MedBlock B;
    B.Id = static_cast<int>(F.Blocks.size());
    F.Blocks.push_back(std::move(B));
    return F.Blocks.back().Id;
  }
  void succ(int From, int To) {
    F.Blocks[From].Succs.push_back(To);
    F.Blocks[To].Preds.push_back(From);
  }
  void op(int Blk, NdOp Op, MedVar Out, std::vector<MedVar> Ins,
          va_t Addr = 0) {
    MedOp O;
    O.Opcode = Op;
    O.Output = Out;
    O.Addr = Addr;
    if (Addr != 0)
      O.OriginSeq = 0;
    for (auto &I : Ins)
      O.addInput(I);
    F.Blocks[Blk].Ops.push_back(O);
  }
  void call(int Blk, const std::string &Name, MedVar Ret,
            std::vector<MedVar> Args, va_t Target = 0x9000, va_t Addr = 0) {
    int Idx = static_cast<int>(F.Blocks[Blk].Ops.size());
    MedOp O;
    O.Opcode = NdOp::CALL;
    O.Output = Ret;
    O.Addr = Addr;
    if (Addr != 0)
      O.OriginSeq = 0;
    O.addInput(MedVar::makeConst(Target, 8));
    F.Blocks[Blk].Ops.push_back(O);
    MedCallInfo CI;
    CI.BlockId = Blk;
    CI.OpIdx = Idx;
    CI.TargetAddr = Target;
    CI.TargetName = Name;
    CI.Args = std::move(Args);
    F.CallInfos.push_back(CI);
  }
  void ret(int Blk, std::vector<MedVar> Ins) {
    op(Blk, NdOp::RETURN, MedVar{}, Ins);
  }
};

class TypedFunctionDebug : public NullDebugContext {
public:
  TypedFunctionDebug(va_t Address, std::string Name,
                     std::vector<TypeRef> Params, TypeRef ReturnType = {},
                     bool AuthenticatedSignatures = true)
      : Address(Address), Name(std::move(Name)), Params(std::move(Params)),
        ReturnType(std::move(ReturnType)),
        AuthenticatedSignatures(AuthenticatedSignatures) {}

  std::optional<FunctionSym> resolveFunction(va_t Query) const override {
    if (Query != Address)
      return std::nullopt;
    FunctionSym Result;
    Result.Name = Name;
    Result.Addr = Address;
    Result.Size = 4;
    Result.ReturnType = ReturnType;
    for (size_t I = 0; I < Params.size(); ++I)
      Result.Params.emplace_back("arg" + std::to_string(I), Params[I]);
    return Result;
  }

  bool hasAuthenticatedFunctionSignatures() const override {
    return AuthenticatedSignatures;
  }
  AuthenticatedReturnValueState
  resolveAuthenticatedReturnValueState(va_t Query) const override {
    if (!AuthenticatedSignatures || Query != Address || !ReturnType)
      return {};
    using Kind = AuthenticatedReturnKind;
    switch (ReturnType->Kind) {
    case NdTypeKind::Void:
      return {Kind::NoValue, 0};
    case NdTypeKind::Ptr:
      return {Kind::Pointer, ReturnType->Size};
    case NdTypeKind::Int:
      return {Kind::Integer, ReturnType->Size};
    case NdTypeKind::Float:
      return {Kind::FloatingPoint, ReturnType->Size};
    case NdTypeKind::Array:
    case NdTypeKind::Struct:
      return {Kind::Aggregate, ReturnType->Size};
    case NdTypeKind::Func:
    case NdTypeKind::Unknown:
      return {};
    }
    return {};
  }
  bool hasInfo() const override { return true; }

private:
  va_t Address;
  std::string Name;
  std::vector<TypeRef> Params;
  TypeRef ReturnType;
  bool AuthenticatedSignatures = true;
};

std::vector<Finding> audit(std::vector<MedFunc> Funcs,
                           const BinaryImage *Image = nullptr,
                           bool StackRegs = false,
                           bool IncludeStackReads = false,
                           const DebugContext *Debug = nullptr) {
  static BinaryImage Img;
  AnalysisInput In;
  In.Img = Image ? Image : &Img;
  In.MedFuncs = &Funcs;
  In.Dbg = Debug;
  In.DebugKind = Debug ? DebugInfoKind::DWARF : DebugInfoKind::None;
  In.StackRegsKnown = StackRegs;
  In.StackPointerReg = kSP;
  return IncludeStackReads
             ? auditMemory(In, SinkCatalog::defaults(), SafetyBudgets{})
             : auditHeap(In, SinkCatalog::defaults(), SafetyBudgets{});
}

std::vector<Finding> auditWithLowIR(std::vector<MedFunc> Funcs,
                                    std::vector<LowFunc> LowFuncs,
                                    const BinaryImage *Image,
                                    const DebugContext *Debug = nullptr,
                                    bool StackRegs = false) {
  AnalysisInput In;
  In.Img = Image;
  In.MedFuncs = &Funcs;
  In.LowFuncs = &LowFuncs;
  In.Dbg = Debug;
  In.DebugKind = Debug ? DebugInfoKind::DWARF : DebugInfoKind::None;
  In.StackRegsKnown = StackRegs;
  In.StackPointerReg = kSP;
  return auditHeap(In, SinkCatalog::defaults(), SafetyBudgets{});
}

bool has(const std::vector<Finding> &Fs, VulnClass C) {
  for (const Finding &F : Fs)
    if (F.Class == C)
      return true;
  return false;
}

size_t count(const std::vector<Finding> &Fs, VulnClass C) {
  size_t N = 0;
  for (const Finding &F : Fs)
    if (F.Class == C)
      ++N;
  return N;
}

const Finding *find(const std::vector<Finding> &Fs, VulnClass C) {
  for (const Finding &F : Fs)
    if (F.Class == C)
      return &F;
  return nullptr;
}

const Finding *findInFunction(const std::vector<Finding> &Fs, VulnClass C,
                              va_t Entry) {
  for (const Finding &F : Fs)
    if (F.Class == C && F.FuncEntry == Entry)
      return &F;
  return nullptr;
}

} // namespace

TEST(AllocLifetime, LeakWhenNeitherFreedNorEscaped) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.ret(b0, {});
  auto Fs = audit({B.F});
  EXPECT_TRUE(has(Fs, VulnClass::HeapLeak));
}

TEST(AllocLifetime, TruncatedStackAddressDoesNotHideAHeapEscape) {
  FB B("f", 0x100);
  const int Block = B.block();
  B.call(Block, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(Block, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(Block, NdOp::COPY, temp(10, 4), {mkReg(kSP, 1)});
  B.op(Block, NdOp::STORE, MedVar{}, {temp(10, 4), temp(1)});
  B.ret(Block, {});

  const std::vector<Finding> Findings = audit({B.F}, nullptr, true);
  const Finding *Leak = find(Findings, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
  EXPECT_NE(Leak->Detail.find("heap handle may escape"), std::string::npos)
      << Leak->Detail;
}

TEST(AllocLifetime, NarrowEncodedStackImmediateStillKeepsLocalSpill) {
  FB B("f", 0x100);
  const int Block = B.block();
  B.call(Block, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(Block, NdOp::INT_SUB, mkReg(kSP, 1, 8),
       {mkReg(kSP, 0, 8), MedVar::makeConst(0x20, 4)});
  B.op(Block, NdOp::STORE, MedVar{}, {mkReg(kSP, 1, 8), temp(1)});
  B.ret(Block, {});

  EXPECT_TRUE(has(audit({B.F}, nullptr, true), VulnClass::HeapLeak));
}

TEST(AllocLifetime, EntrySelfCopyKeepsHeapIdentityAcrossLocalSpill) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  FB B("entry_live_in_heap_spill", 0x100);
  const int Block = B.block();
  B.F.Blocks[Block].StartAddr = B.F.Entry;
  B.op(Block, NdOp::COPY, mkReg(kSP, 0), {mkReg(kSP, 0)});
  B.op(Block, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.call(Block, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(Block, NdOp::STORE, MedVar{}, {mkReg(kSP, 1), temp(1)});
  B.op(Block, NdOp::LOAD, temp(2), {mkReg(kSP, 1)});
  B.call(Block, "free", MedVar{}, {temp(2)});
  B.call(Block, "free", MedVar{}, {temp(2)});
  B.op(Block, NdOp::LOAD, temp(3), {temp(2)});
  B.ret(Block, {});

  const std::vector<Finding> Findings = audit({B.F}, &Img, true);
  EXPECT_TRUE(has(Findings, VulnClass::DoubleFree));
  EXPECT_TRUE(has(Findings, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, StackPointerDefinitionsMustAuthenticateLocalSpills) {
  enum class Definition {
    DifferentOffsetPhi,
    SameOffsetPhi,
    UndefinedVersion,
    ZeroWidthOperation,
    UnsupportedOperation,
  };
  for (const Definition Kind :
       {Definition::DifferentOffsetPhi, Definition::SameOffsetPhi,
        Definition::UndefinedVersion, Definition::ZeroWidthOperation,
        Definition::UnsupportedOperation}) {
    SCOPED_TRACE(static_cast<unsigned>(Kind));
    BinaryImage Img;
    Img.Arch = Arch::X64;
    FB B("sp_definition", 0x100);
    const int Entry = B.block();
    B.call(Entry, "malloc", temp(1), {MedVar::makeConst(16, 8)});

    MedVar Address;
    if (Kind == Definition::DifferentOffsetPhi ||
        Kind == Definition::SameOffsetPhi) {
      const int Left = B.block();
      const int Right = B.block();
      const int Join = B.block();
      B.succ(Entry, Left);
      B.succ(Entry, Right);
      B.succ(Left, Join);
      B.succ(Right, Join);
      const MedVar LeftSP = mkReg(kSP, 1);
      const MedVar RightSP = mkReg(kSP, 2);
      B.op(Left, NdOp::INT_SUB, LeftSP,
           {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
      B.op(Right, NdOp::INT_SUB, RightSP,
           {mkReg(kSP, 0),
            MedVar::makeConst(Kind == Definition::SameOffsetPhi ? 0x20 : 0x40,
                              8)});
      Address = mkReg(kSP, 3);
      PhiNode StackPhi;
      StackPhi.Output = Address;
      StackPhi.Args = {{Left, LeftSP}, {Right, RightSP}};
      B.F.Blocks[Join].Phis.push_back(std::move(StackPhi));
      B.op(Join, NdOp::STORE, MedVar{}, {Address, temp(1)});
      if (Kind == Definition::SameOffsetPhi) {
        const MedVar DirectAddress = temp(20);
        B.op(Join, NdOp::INT_SUB, DirectAddress,
             {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
        B.op(Join, NdOp::LOAD, temp(21), {DirectAddress}, 0x408);
      }
      B.ret(Join, {});
    } else {
      const int Block = Entry;
      Address = mkReg(kSP, 4);
      if (Kind == Definition::ZeroWidthOperation)
        B.op(Block, NdOp::COPY, mkReg(kSP, 4, 0), {mkReg(kSP, 0)});
      else if (Kind == Definition::UnsupportedOperation)
        B.op(Block, NdOp::INT_XOR, Address,
             {mkReg(kSP, 0), MedVar::makeConst(0, 8)});
      B.op(Block, NdOp::STORE, MedVar{}, {Address, temp(1)});
      B.ret(Block, {});
    }

    const std::vector<Finding> Findings =
        audit({B.F}, &Img, /*StackRegs=*/true,
              /*IncludeStackReads=*/Kind == Definition::SameOffsetPhi);
    const Finding *Leak = find(Findings, VulnClass::HeapLeak);
    ASSERT_NE(Leak, nullptr);
    EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown) << Leak->Detail;
    const bool MustFailClosed = Kind != Definition::SameOffsetPhi;
    EXPECT_EQ(Leak->Detail.find("heap handle may escape") != std::string::npos,
              MustFailClosed)
        << Leak->Detail;
    if (Kind == Definition::SameOffsetPhi)
      EXPECT_EQ(find(Findings, VulnClass::UninitializedRead), nullptr);
  }
}

TEST(AllocLifetime, FindingUsesAllocatorIdentityOrigin) {
  BinaryImage Img;
  Import Malloc;
  Malloc.Name = "malloc";
  Malloc.IATAddr = 0x9000;
  Img.Imports.push_back(Malloc);

  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, 0x400);
  B.ret(b0, {});
  auto Fs = audit({B.F}, &Img);
  ASSERT_EQ(Fs.size(), 1u);
  EXPECT_EQ(Fs[0].Class, VulnClass::HeapLeak);
  EXPECT_EQ(Fs[0].Source, NameSource::Import);
}

TEST(AllocLifetime, ResolvedImportIdentityDrivesAllocationSemantics) {
  BinaryImage Img;
  Img.Imports.push_back({"runtime", "malloc", 0, 0x9000});

  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "sub_9000", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, 0x400);
  B.ret(b0, {});

  auto Fs = audit({B.F}, &Img);
  ASSERT_EQ(Fs.size(), 1u);
  EXPECT_EQ(Fs[0].Class, VulnClass::HeapLeak);
  EXPECT_EQ(Fs[0].Sink, "malloc");
  EXPECT_EQ(Fs[0].Source, NameSource::Import);
}

TEST(AllocLifetime, MicrosoftOperatorNamesDriveLifetimeAnalysis) {
  FB Released("released", 0x100);
  int ReleasedBlock = Released.block();
  Released.call(ReleasedBlock, "??2@YAPEAX_K@Z", temp(1),
                {MedVar::makeConst(16, 8)});
  Released.call(ReleasedBlock, "??3@YAXPEAX@Z", MedVar{}, {temp(1)});
  Released.ret(ReleasedBlock, {});
  EXPECT_FALSE(has(audit({Released.F}), VulnClass::HeapLeak));

  FB Leaked("leaked", 0x200);
  int LeakedBlock = Leaked.block();
  Leaked.call(LeakedBlock, "??_U@YAPEAX_K@Z", temp(2),
              {MedVar::makeConst(32, 8)});
  Leaked.ret(LeakedBlock, {});
  EXPECT_TRUE(has(audit({Leaked.F}), VulnClass::HeapLeak));
}

TEST(AllocLifetime, PlacementAndCustomOperatorsDoNotClaimHeapLifetime) {
  for (const char *Name : {"??2@YAPEAX_KPEAX@Z", "??2X@@SAPEAX_K@Z"}) {
    SCOPED_TRACE(Name);
    FB B("not_an_allocation", 0x100);
    int b0 = B.block();
    B.call(b0, Name, temp(1),
           {MedVar::makeConst(16, 8), MedVar::makeConst(0x1000, 8)});
    B.ret(b0, {});
    EXPECT_FALSE(has(audit({B.F}), VulnClass::HeapLeak));
  }

  for (const char *Name : {"??3@YAXPEAX0@Z", "??3X@@SAXPEAX@Z"}) {
    SCOPED_TRACE(Name);
    FB B("not_a_release", 0x200);
    int b0 = B.block();
    B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
    B.call(b0, Name, MedVar{}, {temp(1), MedVar::makeConst(0x1000, 8)});
    B.ret(b0, {});
    EXPECT_TRUE(has(audit({B.F}), VulnClass::HeapLeak));
  }
}

TEST(AllocLifetime, ItaniumAlignedOperatorNamesDriveLifetimeAnalysis) {
  FB Released("released", 0x100);
  int ReleasedBlock = Released.block();
  Released.call(ReleasedBlock, "_ZnwmSt11align_val_t", temp(1),
                {MedVar::makeConst(64, 8), MedVar::makeConst(64, 8)});
  Released.call(ReleasedBlock, "_ZdlPvmSt11align_val_t", MedVar{},
                {temp(1), MedVar::makeConst(64, 8), MedVar::makeConst(64, 8)});
  Released.ret(ReleasedBlock, {});
  EXPECT_FALSE(has(audit({Released.F}), VulnClass::HeapLeak));

  FB Leaked("leaked", 0x200);
  int LeakedBlock = Leaked.block();
  Leaked.call(LeakedBlock, "_ZnamSt11align_val_t", temp(2),
              {MedVar::makeConst(64, 8), MedVar::makeConst(64, 8)});
  Leaked.ret(LeakedBlock, {});
  EXPECT_TRUE(has(audit({Leaked.F}), VulnClass::HeapLeak));
}

TEST(AllocLifetime, StackAllocationIsNotAHeapLeak) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "alloca", temp(1), {MedVar::makeConst(16, 8)});
  B.ret(b0, {});
  auto Fs = audit({B.F});
  EXPECT_FALSE(has(Fs, VulnClass::HeapLeak));
}

TEST(AllocLifetime, NoLeakWhenFreed) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.ret(b0, {});
  auto Fs = audit({B.F});
  EXPECT_FALSE(has(Fs, VulnClass::HeapLeak));
}

TEST(AllocLifetime, ConflictingDebugAllocatorSignatureFailsClosed) {
  BinaryImage Img;
  Img.Arch = Arch::X64;

  struct SignatureCase {
    const char *Name;
    std::vector<TypeRef> Params;
    TypeRef ReturnType;
    bool Conflict;
  };
  const std::vector<SignatureCase> Cases = {
      {"compatible", {NdType::makeInt(8, false)}, NdType::makePtr(), false},
      {"parameter conflict", {NdType::makePtr()}, NdType::makePtr(), true},
      {"size width conflict",
       {NdType::makeInt(1, false)},
       NdType::makePtr(),
       true},
      {"return conflict",
       {NdType::makeInt(8, false)},
       NdType::makeInt(4, true),
       true},
      {"return-only conflict", {}, NdType::makeInt(4, true), true},
      {"missing types", {}, {}, false},
  };

  for (const SignatureCase &C : Cases) {
    SCOPED_TRACE(C.Name);
    FB B("f", 0x100);
    const int Block = B.block();
    B.call(Block, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000);
    B.call(Block, "free", MedVar{}, {temp(1)}, 0x9010);
    B.ret(Block, {});

    TypedFunctionDebug Debug(0x9000, "malloc", C.Params, C.ReturnType);
    const std::vector<Finding> Findings = audit(
        {B.F}, &Img, /*StackRegs=*/false, /*IncludeStackReads=*/false, &Debug);
    const Finding *Signature = find(Findings, VulnClass::Unknown);
    if (!C.Conflict) {
      EXPECT_EQ(Signature, nullptr);
      continue;
    }
    ASSERT_NE(Signature, nullptr);
    EXPECT_EQ(Signature->TheVerdict, Verdict::Unknown) << Signature->Detail;
    EXPECT_EQ(Signature->TheConfidence, Confidence::Low);
    EXPECT_EQ(Signature->Detail,
              "debug function signature conflicts with sink summary");
  }
}

TEST(AllocLifetime, FallibleWindowsReleasesRemainConditional) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Format = BinaryFormat::COFF;
  for (const char *Name : {"HeapFree", "LocalFree", "GlobalFree"}) {
    SCOPED_TRACE(Name);
    FB B("f", 0x100);
    int b0 = B.block();
    B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, 0x400);
    if (llvm::StringRef(Name) == "HeapFree")
      B.call(b0, Name, temp(2), {temp(9), MedVar::makeConst(0, 8), temp(1)},
             0x9100, 0x408);
    else
      B.call(b0, Name, temp(2), {temp(1)}, 0x9100, 0x408);
    B.op(b0, NdOp::LOAD, temp(3), {temp(1)}, 0x410);
    B.call(b0, "free", MedVar{}, {temp(1)}, 0x9200, 0x418);
    B.ret(b0, {});

    const auto Fs = audit({B.F}, &Img);
    const Finding *UAF = find(Fs, VulnClass::UseAfterFree);
    ASSERT_NE(UAF, nullptr);
    EXPECT_EQ(UAF->TheVerdict, Verdict::Unknown) << UAF->Detail;
    EXPECT_EQ(UAF->TheConfidence, Confidence::Low) << UAF->Detail;
    EXPECT_NE(UAF->Detail.find("conditional release"), std::string::npos);
    const Finding *Double = find(Fs, VulnClass::DoubleFree);
    ASSERT_NE(Double, nullptr);
    EXPECT_EQ(Double->TheVerdict, Verdict::Unknown) << Double->Detail;
    EXPECT_EQ(Double->TheConfidence, Confidence::Low) << Double->Detail;
    EXPECT_NE(Double->Detail.find("conditional release"), std::string::npos);
    EXPECT_FALSE(has(Fs, VulnClass::HeapLeak));
  }
}

TEST(AllocLifetime, FallibleWindowsReleaseDoesNotHidePossibleLeak) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Format = BinaryFormat::COFF;
  for (const char *Name : {"HeapFree", "LocalFree", "GlobalFree"}) {
    SCOPED_TRACE(Name);
    FB B("f", 0x100);
    int b0 = B.block();
    B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, 0x400);
    if (llvm::StringRef(Name) == "HeapFree")
      B.call(b0, Name, temp(2), {temp(9), MedVar::makeConst(0, 8), temp(1)},
             0x9100, 0x408);
    else
      B.call(b0, Name, temp(2), {temp(1)}, 0x9100, 0x408);
    B.ret(b0, {});

    const auto Fs = audit({B.F}, &Img);
    const Finding *Leak = find(Fs, VulnClass::HeapLeak);
    ASSERT_NE(Leak, nullptr);
    EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown) << Leak->Detail;
    EXPECT_EQ(Leak->TheConfidence, Confidence::Low) << Leak->Detail;
    EXPECT_EQ(Leak->Detail, "heap lifetime depends on a conditional release");
  }
}

TEST(AllocLifetime, MayAliasFreeDoesNotProveAllocationReleased) {
  FB B("f", 0x100);
  int entry = B.block();
  int allocated = B.block();
  int other = B.block();
  int join = B.block();
  B.call(entry, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.succ(entry, allocated);
  B.succ(entry, other);
  B.succ(allocated, join);
  B.succ(other, join);
  PhiNode Phi;
  Phi.Output = temp(2);
  Phi.Args = {{allocated, temp(1)}, {other, temp(9)}};
  B.F.Blocks[join].Phis.push_back(std::move(Phi));
  B.call(join, "free", MedVar{}, {temp(2)});
  B.ret(join, {});

  auto Fs = audit({B.F});
  const Finding *Leak = find(Fs, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
  EXPECT_FALSE(has(Fs, VulnClass::DoubleFree));
  EXPECT_FALSE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, NullCheckedConditionalFreeIsNotAHeapLeakWithoutReturnType) {
  constexpr va_t MallocVA = 0x108;
  constexpr va_t FreeVA = 0x118;
  FB B("guarded_free", 0x100);
  int Entry = B.block();
  int Release = B.block();
  int Exit = B.block();
  B.call(Entry, "malloc", temp(1), {MedVar::makeConst(32, 8)}, 0x9000,
         MallocVA);
  B.op(Entry, NdOp::INT_EQUAL, temp(5, 1), {temp(1), MedVar::makeConst(0, 8)});
  B.succ(Entry, Release);
  B.succ(Entry, Exit);
  B.call(Release, "free", temp(9), {temp(1)}, 0x9100, FreeVA);
  B.succ(Release, Exit);
  PhiNode Returned;
  Returned.Output = temp(2);
  Returned.Args = {{Entry, temp(1)}, {Release, temp(9)}};
  B.F.Blocks[Exit].Phis.push_back(std::move(Returned));
  B.ret(Exit, {temp(2)});

  LowFunc LF;
  LF.Entry = B.F.Entry;
  LF.DecodedInstructionCount = 6;
  LF.LiftedInstructionCount = 6;
  LF.Blocks.resize(3);

  LowBlock &LowEntry = LF.Blocks[Entry];
  LowEntry.Id = Entry;
  LowEntry.StartAddr = B.F.Entry;
  LowEntry.EndAddr = 0x110;
  LowEntry.Succs = {Release, Exit};
  LowEntry.Ops.push_back(
      lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9000, 8)}, MallocVA));
  LowEntry.Ops.push_back(lowOp(NdOp::INT_EQUAL, NdVar::reg(1, 1),
                               {NdVar::reg(0, 8), NdVar::cst(0, 8)}, 0x10A));
  LowEntry.Ops.push_back(lowOp(
      NdOp::COND_BR, NdVar{}, {NdVar::cst(0x120, 8), NdVar::reg(1, 1)}, 0x10C));

  LowBlock &LowRelease = LF.Blocks[Release];
  LowRelease.Id = Release;
  LowRelease.StartAddr = 0x110;
  LowRelease.EndAddr = 0x120;
  LowRelease.Preds = {Entry};
  LowRelease.Succs = {Exit};
  LowRelease.Ops.push_back(
      lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9100, 8)}, FreeVA));
  LowRelease.Ops.push_back(
      lowOp(NdOp::BRANCH, NdVar{}, {NdVar::cst(0x120, 8)}, 0x11C));

  LowBlock &LowExit = LF.Blocks[Exit];
  LowExit.Id = Exit;
  LowExit.StartAddr = 0x120;
  LowExit.EndAddr = 0x128;
  LowExit.Preds = {Entry, Release};
  LowExit.Ops.push_back(lowOp(NdOp::RETURN, NdVar{}, {}, 0x120));

  BinaryImage Img;
  Img.Arch = Arch::X64;
  std::vector<MedFunc> MedFuncs{B.F};
  std::vector<LowFunc> LowFuncs{std::move(LF)};
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &MedFuncs;
  In.LowFuncs = &LowFuncs;

  EXPECT_FALSE(has(auditHeap(In, SinkCatalog::defaults(), SafetyBudgets{}),
                   VulnClass::HeapLeak));
}

TEST(AllocLifetime, CatalogSourceResultCanCorroborateConditionalLeakPath) {
  constexpr va_t MallocVA = 0x108;
  constexpr va_t SourceVA = 0x110;
  constexpr va_t FreeVA = 0x120;
  FB B("leak_on_one_path", 0x100);
  const int Entry = B.block();
  const int Leak = B.block();
  const int Release = B.block();
  B.call(Entry, "malloc", temp(1), {MedVar::makeConst(32, 8)}, 0x9000,
         MallocVA);
  B.call(Entry, "getenv", temp(2), {MedVar::makeConst(0x8000, 8)}, 0x9010,
         SourceVA);
  B.op(Entry, NdOp::INT_NOTEQUAL, temp(3, 1),
       {temp(2), MedVar::makeConst(0, 8)}, 0x114);
  B.op(Entry, NdOp::COND_BR, MedVar{},
       {MedVar::makeConst(0x130, 8), temp(3, 1)}, 0x118);
  B.succ(Entry, Release);
  B.succ(Entry, Leak);
  B.ret(Leak, {});
  B.call(Release, "free", MedVar{}, {temp(1)}, 0x9020, FreeVA);
  B.ret(Release, {});

  LowFunc LF;
  LF.Entry = B.F.Entry;
  LF.DecodedInstructionCount = 7;
  LF.LiftedInstructionCount = 7;
  LF.Blocks.resize(3);

  LowBlock &LowEntry = LF.Blocks[Entry];
  LowEntry.Id = Entry;
  LowEntry.StartAddr = B.F.Entry;
  LowEntry.EndAddr = 0x120;
  LowEntry.Succs = {Release, Leak};
  LowEntry.Ops.push_back(
      lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9000, 8)}, MallocVA));
  LowEntry.Ops.push_back(
      lowOp(NdOp::CALL, NdVar::reg(1, 8), {NdVar::cst(0x9010, 8)}, SourceVA));
  LowEntry.Ops.push_back(lowOp(NdOp::INT_NOTEQUAL, NdVar::reg(2, 1),
                               {NdVar::reg(1, 8), NdVar::cst(0, 8)}, 0x114));
  LowEntry.Ops.push_back(lowOp(
      NdOp::COND_BR, NdVar{}, {NdVar::cst(0x130, 8), NdVar::reg(2, 1)}, 0x118));

  LowBlock &LowLeak = LF.Blocks[Leak];
  LowLeak.Id = Leak;
  LowLeak.StartAddr = 0x130;
  LowLeak.EndAddr = 0x138;
  LowLeak.Preds = {Entry};
  LowLeak.Ops.push_back(lowOp(NdOp::RETURN, NdVar{}, {}, 0x130));

  LowBlock &LowRelease = LF.Blocks[Release];
  LowRelease.Id = Release;
  LowRelease.StartAddr = 0x120;
  LowRelease.EndAddr = 0x130;
  LowRelease.Preds = {Entry};
  LowRelease.Ops.push_back(
      lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9020, 8)}, FreeVA));
  LowRelease.Ops.push_back(lowOp(NdOp::RETURN, NdVar{}, {}, 0x128));

  BinaryImage Img;
  Img.Arch = Arch::X64;
  std::vector<MedFunc> MedFuncs{B.F};
  std::vector<LowFunc> LowFuncs{std::move(LF)};
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &MedFuncs;
  In.LowFuncs = &LowFuncs;

  const std::vector<Finding> Findings =
      auditHeap(In, SinkCatalog::defaults(), SafetyBudgets{});
  const Finding *LeakFinding = find(Findings, VulnClass::HeapLeak);
  ASSERT_NE(LeakFinding, nullptr);
  EXPECT_EQ(LeakFinding->TheVerdict, Verdict::Unsafe)
      << LeakFinding->Corroboration << ": " << LeakFinding->Detail;
  EXPECT_EQ(LeakFinding->TheConfidence, Confidence::High);

  MedFuncs.front().CallInfos[1].TargetName = "scanf";
  const std::vector<Finding> OutputOnlyFindings =
      auditHeap(In, SinkCatalog::defaults(), SafetyBudgets{});
  const Finding *OutputOnlyLeak = find(OutputOnlyFindings, VulnClass::HeapLeak);
  ASSERT_NE(OutputOnlyLeak, nullptr);
  EXPECT_EQ(OutputOnlyLeak->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(OutputOnlyLeak->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, AdjustedPointerFreeDoesNotReleaseAllocation) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(b0, NdOp::INT_ADD, temp(2), {temp(1), MedVar::makeConst(8, 8)});
  B.call(b0, "free", MedVar{}, {temp(2)});
  B.ret(b0, {});

  auto Fs = audit({B.F});
  const Finding *Leak = find(Fs, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, RelocatedZeroDoesNotProveAllocationReleased) {
  for (const Arch A : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM}) {
    SCOPED_TRACE(static_cast<int>(A));
    const uint16_t PointerSize = (A == Arch::X86 || A == Arch::ARM) ? 4 : 8;
    BinaryImage Img;
    Img.Arch = A;

    FB B("f", 0x100);
    const int Block = B.block();
    B.call(Block, "malloc", temp(1, PointerSize),
           {MedVar::makeConst(16, PointerSize)});
    B.op(Block, NdOp::INT_ADD, temp(2, PointerSize),
         {temp(1, PointerSize),
          MedVar::makeConst(0, PointerSize,
                            ConstantAddressProvenance::DataAddress, 0)});
    B.call(Block, "free", MedVar{}, {temp(2, PointerSize)});
    B.ret(Block, {});

    const std::vector<Finding> Findings = audit({B.F}, &Img);
    const Finding *Leak = find(Findings, VulnClass::HeapLeak);
    ASSERT_NE(Leak, nullptr);
    EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
  }
}

TEST(AllocLifetime, SelectOfSamePointerCanReleaseAllocation) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(b0, NdOp::SELECT, temp(2), {temp(9), temp(1), temp(1)});
  B.call(b0, "free", MedVar{}, {temp(2)});
  B.ret(b0, {});

  EXPECT_FALSE(has(audit({B.F}), VulnClass::HeapLeak));
}

TEST(AllocLifetime, WidthChangingPhiDoesNotProveAllocationReleased) {
  FB B("f", 0x100);
  const int Block = B.block();
  B.call(Block, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  PhiNode Phi;
  Phi.Output = temp(2, 4);
  Phi.Args = {{Block, temp(1)}, {Block, temp(1)}};
  B.F.Blocks[Block].Phis.push_back(std::move(Phi));
  B.call(Block, "free", MedVar{}, {temp(2, 4)});
  B.ret(Block, {});

  const std::vector<Finding> Findings = audit({B.F});
  const Finding *Leak = find(Findings, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, UnknownReturnCarrierKeepsLeakUncertain) {
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(A));
    BinaryImage Img;
    Img.Arch = A;
    const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
    FB B("f", 0x100);
    int b0 = B.block();
    const MedVar Result = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
    B.call(b0, "malloc", Result, {MedVar::makeConst(16, TRI.PointerSize)});
    B.ret(b0,
          A == Arch::AArch64
              ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 1, TRI.PointerSize)}
              : std::vector<MedVar>{Result});
    const std::vector<Finding> Fs = audit({B.F}, &Img);
    const Finding *Leak = find(Fs, VulnClass::HeapLeak);
    ASSERT_NE(Leak, nullptr);
    EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
  }
}

TEST(AllocLifetime, HeuristicReturnTypeCannotPublishOwnedHeapSummary) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  FB Wrap("untyped_factory", 0x200);
  // Mirrors MedTypePass's default integer backend type when no declaration is
  // available.  The explicit evidence intentionally remains Unknown.
  Wrap.F.ReturnType = NdType::makeInt(TRI.PointerSize, false);
  const int WrapperBlock = Wrap.block();
  const MedVar Result = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
  Wrap.call(WrapperBlock, "malloc", Result,
            {MedVar::makeConst(16, TRI.PointerSize)});
  Wrap.ret(WrapperBlock, {Result});

  FB User("user", 0x100);
  const int UserBlock = User.block();
  User.call(UserBlock, "untyped_factory", temp(9), {}, Wrap.F.Entry);
  User.ret(UserBlock, {});

  const std::vector<Finding> Findings = audit({Wrap.F, User.F}, &Img);
  const Finding *CallerLeak = nullptr;
  const Finding *WrapperLeak = nullptr;
  for (const Finding &Finding : Findings) {
    if (Finding.Class != VulnClass::HeapLeak)
      continue;
    if (Finding.FuncEntry == User.F.Entry)
      CallerLeak = &Finding;
    if (Finding.FuncEntry == Wrap.F.Entry)
      WrapperLeak = &Finding;
  }
  // The heuristic type cannot establish definite pointer ownership, but the
  // live ABI return carrier still makes ownership possible.  Preserve that
  // May fact for callers instead of collapsing it to No.
  ASSERT_NE(CallerLeak, nullptr);
  EXPECT_EQ(CallerLeak->TheVerdict, Verdict::Unknown);
  EXPECT_NE(CallerLeak->Detail.find("callee may return heap ownership"),
            std::string::npos)
      << CallerLeak->Detail;
  ASSERT_NE(WrapperLeak, nullptr);
  EXPECT_EQ(WrapperLeak->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, NarrowReturnDoesNotProveAllocationEscaped) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  const MedVar NarrowResult = mkReg(TRI.IntReturnReg, 1, 1);
  B.op(b0, NdOp::SUBBYTES, NarrowResult, {temp(1), MedVar::makeConst(0, 8)});
  B.ret(b0, {NarrowResult});

  const std::vector<Finding> Fs = audit({B.F}, &Img);
  const Finding *Leak = find(Fs, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown) << Leak->Detail;
  EXPECT_EQ(Leak->TheConfidence, Confidence::Low) << Leak->Detail;
}

TEST(AllocLifetime, OverlappingHighByteWriteKillsStaleX64ReturnCarrier) {
  constexpr va_t MallocVA = 0x110;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  FB B("f", 0x100);
  B.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
  const int Block = B.block();
  const MedVar Result = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
  B.call(Block, "malloc", Result, {MedVar::makeConst(16, TRI.PointerSize)},
         0x9000, MallocVA);
  // x86 models AH at the second byte of the RAX range.  It is not a complete
  // ABI result, but it is a newer overlapping definition that kills Result.
  const MedVar HighByte = mkReg(TRI.IntReturnReg + 1, 2, 1);
  B.op(Block, NdOp::COPY, HighByte, {MedVar::makeConst(0, 1)});
  B.ret(Block, {Result});

  const std::vector<Finding> Findings = auditWithLowIR(
      {B.F},
      {straightLineCallReturnPath(B.F.Entry, MallocVA, 0x9000, Img.Arch)},
      &Img);
  const Finding *Leak = find(Findings, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, ZeroExtendingLowWriteKillsStaleX64ReturnCarrier) {
  constexpr va_t MallocVA = 0x110;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  ASSERT_TRUE(TRI.writeZeroExtends(TRI.IntReturnReg, 4));
  FB B("f", 0x100);
  B.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
  const int Block = B.block();
  const MedVar Result = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
  B.call(Block, "malloc", Result, {MedVar::makeConst(16, TRI.PointerSize)},
         0x9000, MallocVA);
  const MedVar ZeroExtended = mkReg(TRI.IntReturnReg, 2, 4);
  B.op(Block, NdOp::COPY, ZeroExtended, {MedVar::makeConst(0, 4)});
  B.ret(Block, {Result});

  const std::vector<Finding> Findings = auditWithLowIR(
      {B.F},
      {straightLineCallReturnPath(B.F.Entry, MallocVA, 0x9000, Img.Arch)},
      &Img);
  const Finding *Leak = find(Findings, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unsafe);
}

TEST(AllocLifetime, NarrowStoredValueDoesNotProveAllocationEscaped) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {MedVar::makeConst(0x8000, 8), temp(1, 1)});
  B.ret(b0, {});

  const std::vector<Finding> Fs = audit({B.F}, &Img);
  const Finding *Leak = find(Fs, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown) << Leak->Detail;
  EXPECT_EQ(Leak->TheConfidence, Confidence::Low) << Leak->Detail;
}

TEST(AllocLifetime, RuntimeAdjustedReturnDoesNotProveAllocationEscaped) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(b0, NdOp::INT_ADD, temp(2), {temp(1), temp(9)});
  const MedVar Result = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
  B.op(b0, NdOp::COPY, Result, {temp(2)});
  B.ret(b0, {Result});

  const std::vector<Finding> Findings = audit({B.F}, &Img);
  const Finding *Leak = find(Findings, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Leak->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, RuntimeAdjustedWrapperReturnIsOnlyAPotentialHeapSummary) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  FB Helper("helper", 0x200);
  Helper.F.Params.push_back(temp(9));
  const int HelperBlock = Helper.block();
  Helper.call(HelperBlock, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  Helper.op(HelperBlock, NdOp::INT_ADD, temp(2), {temp(1), temp(9)});
  const MedVar Result = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
  Helper.op(HelperBlock, NdOp::COPY, Result, {temp(2)});
  Helper.ret(HelperBlock, {Result});

  FB User("user", 0x100);
  const int UserBlock = User.block();
  User.call(UserBlock, "helper", temp(3), {temp(8)}, Helper.F.Entry);
  User.ret(UserBlock, {});

  const std::vector<Finding> Findings = audit({Helper.F, User.F}, &Img);
  const Finding *UserHeapFinding = nullptr;
  for (const Finding &Finding : Findings)
    if (Finding.FuncEntry == User.F.Entry &&
        Finding.Class == VulnClass::HeapLeak)
      UserHeapFinding = &Finding;
  ASSERT_NE(UserHeapFinding, nullptr);
  EXPECT_EQ(UserHeapFinding->TheVerdict, Verdict::Unknown);
  EXPECT_NE(UserHeapFinding->Detail.find("callee may return heap ownership"),
            std::string::npos)
      << UserHeapFinding->Detail;
  const Finding *HelperLeak = nullptr;
  for (const Finding &Finding : Findings)
    if (Finding.FuncEntry == Helper.F.Entry &&
        Finding.Class == VulnClass::HeapLeak) {
      HelperLeak = &Finding;
      break;
    }
  ASSERT_NE(HelperLeak, nullptr);
  EXPECT_EQ(HelperLeak->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, NonEscapingHelperDoesNotHideLeak) {
  FB Helper("inspect", 0x200);
  Helper.F.Params.push_back(temp(0));
  int h0 = Helper.block();
  Helper.op(h0, NdOp::LOAD, temp(4), {temp(0)});
  Helper.ret(h0, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(u0, "inspect", MedVar{}, {temp(1)}, 0x200);
  User.ret(u0, {});

  auto Fs = audit({Helper.F, User.F});
  bool UserLeak = false;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == 0x100)
      UserLeak = true;
  EXPECT_TRUE(UserLeak);
}

TEST(AllocLifetime, MayAliasFreeWrapperDoesNotProveCallerReleased) {
  FB Helper("maybe_free", 0x200);
  Helper.F.Params = {temp(0), temp(1)};
  int hEntry = Helper.block();
  int hFirst = Helper.block();
  int hSecond = Helper.block();
  int hJoin = Helper.block();
  Helper.succ(hEntry, hFirst);
  Helper.succ(hEntry, hSecond);
  Helper.succ(hFirst, hJoin);
  Helper.succ(hSecond, hJoin);
  PhiNode Phi;
  Phi.Output = temp(2);
  Phi.Args = {{hFirst, temp(0)}, {hSecond, temp(1)}};
  Helper.F.Blocks[hJoin].Phis.push_back(std::move(Phi));
  Helper.call(hJoin, "free", MedVar{}, {temp(2)});
  Helper.ret(hJoin, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(3), {MedVar::makeConst(16, 8)});
  User.call(u0, "maybe_free", MedVar{}, {temp(3), temp(9)}, 0x200);
  User.ret(u0, {});

  auto Fs = audit({Helper.F, User.F});
  const Finding *UserLeak = nullptr;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == User.F.Entry)
      UserLeak = &F;
  ASSERT_NE(UserLeak, nullptr);
  EXPECT_EQ(UserLeak->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, ConditionalFreeWrapperDoesNotProveCallerReleased) {
  FB Helper("conditional_free", 0x200);
  Helper.F.Params = {temp(0)};
  int hEntry = Helper.block();
  int hFree = Helper.block();
  int hSkip = Helper.block();
  int hJoin = Helper.block();
  Helper.succ(hEntry, hFree);
  Helper.succ(hEntry, hSkip);
  Helper.succ(hFree, hJoin);
  Helper.succ(hSkip, hJoin);
  Helper.call(hFree, "free", MedVar{}, {temp(0)});
  Helper.ret(hJoin, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(u0, "conditional_free", MedVar{}, {temp(1)}, 0x200);
  User.ret(u0, {});

  auto Fs = audit({Helper.F, User.F});
  const Finding *UserLeak = nullptr;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == User.F.Entry)
      UserLeak = &F;
  ASSERT_NE(UserLeak, nullptr);
  EXPECT_EQ(UserLeak->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, ConditionalFreeWrapperKeepsLaterUsesUncertain) {
  FB Helper("conditional_free", 0x200);
  Helper.F.Params = {temp(0)};
  int hEntry = Helper.block();
  int hFree = Helper.block();
  int hSkip = Helper.block();
  int hJoin = Helper.block();
  Helper.succ(hEntry, hFree);
  Helper.succ(hEntry, hSkip);
  Helper.succ(hFree, hJoin);
  Helper.succ(hSkip, hJoin);
  Helper.call(hFree, "free", MedVar{}, {temp(0)});
  Helper.ret(hJoin, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(u0, "conditional_free", MedVar{}, {temp(1)}, 0x200, 0x400);
  User.op(u0, NdOp::LOAD, temp(2), {temp(1)}, 0x408);
  User.call(u0, "free", MedVar{}, {temp(1)}, 0x9100, 0x410);
  User.ret(u0, {});

  const auto Fs = audit({Helper.F, User.F});
  const Finding *UAF = nullptr;
  const Finding *Double = nullptr;
  for (const Finding &F : Fs) {
    if (F.FuncEntry != User.F.Entry)
      continue;
    if (F.Class == VulnClass::UseAfterFree)
      UAF = &F;
    if (F.Class == VulnClass::DoubleFree)
      Double = &F;
  }
  ASSERT_NE(UAF, nullptr);
  EXPECT_EQ(UAF->TheVerdict, Verdict::Unknown) << UAF->Detail;
  ASSERT_NE(Double, nullptr);
  EXPECT_EQ(Double->TheVerdict, Verdict::Unknown) << Double->Detail;
}

TEST(AllocLifetime, AlternateFreeSitesProveWrapperReleased) {
  FB Helper("always_free", 0x200);
  Helper.F.Params = {temp(0)};
  int hEntry = Helper.block();
  int hLeft = Helper.block();
  int hRight = Helper.block();
  int hJoin = Helper.block();
  Helper.succ(hEntry, hLeft);
  Helper.succ(hEntry, hRight);
  Helper.succ(hLeft, hJoin);
  Helper.succ(hRight, hJoin);
  Helper.call(hLeft, "free", MedVar{}, {temp(0)});
  Helper.call(hRight, "free", MedVar{}, {temp(0)});
  Helper.ret(hJoin, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(u0, "always_free", MedVar{}, {temp(1)}, 0x200);
  User.ret(u0, {});

  EXPECT_FALSE(has(audit({Helper.F, User.F}), VulnClass::HeapLeak));
}

TEST(AllocLifetime, ConditionalEscapeWrapperDoesNotHideCallerLeak) {
  FB Helper("conditional_escape", 0x200);
  Helper.F.Params = {temp(0)};
  int hEntry = Helper.block();
  int hPublish = Helper.block();
  int hSkip = Helper.block();
  int hJoin = Helper.block();
  Helper.succ(hEntry, hPublish);
  Helper.succ(hEntry, hSkip);
  Helper.succ(hPublish, hJoin);
  Helper.succ(hSkip, hJoin);
  Helper.op(hPublish, NdOp::STORE, MedVar{},
            {MedVar::makeConst(0x5000, 8), temp(0)});
  Helper.ret(hJoin, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(u0, "conditional_escape", MedVar{}, {temp(1)}, 0x200);
  User.ret(u0, {});

  auto Fs = audit({Helper.F, User.F});
  const Finding *UserLeak = nullptr;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == User.F.Entry)
      UserLeak = &F;
  ASSERT_NE(UserLeak, nullptr);
  EXPECT_EQ(UserLeak->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, AlternateEscapeSitesProveWrapperEscaped) {
  FB Helper("always_escape", 0x200);
  Helper.F.Params = {temp(0)};
  int hEntry = Helper.block();
  int hLeft = Helper.block();
  int hRight = Helper.block();
  int hJoin = Helper.block();
  Helper.succ(hEntry, hLeft);
  Helper.succ(hEntry, hRight);
  Helper.succ(hLeft, hJoin);
  Helper.succ(hRight, hJoin);
  Helper.op(hLeft, NdOp::STORE, MedVar{},
            {MedVar::makeConst(0x5000, 8), temp(0)});
  Helper.op(hRight, NdOp::STORE, MedVar{},
            {MedVar::makeConst(0x6000, 8), temp(0)});
  Helper.ret(hJoin, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(u0, "always_escape", MedVar{}, {temp(1)}, 0x200);
  User.ret(u0, {});

  EXPECT_FALSE(has(audit({Helper.F, User.F}), VulnClass::HeapLeak));
}

TEST(AllocLifetime, ExceptionalExitDoesNotProveWrapperEscaped) {
  FB Helper("exceptional_escape", 0x200);
  Helper.F.Params = {temp(0)};
  int hEntry = Helper.block();
  int hPublish = Helper.block();
  int hHandler = Helper.block();
  Helper.succ(hEntry, hPublish);
  ExceptionalEdge Edge;
  Edge.BlockId = hHandler;
  Helper.F.Blocks[hEntry].ExceptionalSuccs.push_back(Edge);
  Helper.op(hPublish, NdOp::STORE, MedVar{},
            {MedVar::makeConst(0x5000, 8), temp(0)});
  Helper.ret(hPublish, {});
  Helper.ret(hHandler, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(u0, "exceptional_escape", MedVar{}, {temp(1)}, 0x200);
  User.ret(u0, {});

  auto Fs = audit({Helper.F, User.F});
  const Finding *UserLeak = nullptr;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == User.F.Entry)
      UserLeak = &F;
  ASSERT_NE(UserLeak, nullptr);
  EXPECT_EQ(UserLeak->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, ExceptionalExitDoesNotProveWrapperReleased) {
  FB Helper("exceptional_free", 0x200);
  Helper.F.Params = {temp(0)};
  int hEntry = Helper.block();
  int hFree = Helper.block();
  int hHandler = Helper.block();
  Helper.succ(hEntry, hFree);
  ExceptionalEdge Edge;
  Edge.BlockId = hHandler;
  Helper.F.Blocks[hEntry].ExceptionalSuccs.push_back(Edge);
  Helper.call(hFree, "free", MedVar{}, {temp(0)});
  Helper.ret(hFree, {});
  Helper.ret(hHandler, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(u0, "exceptional_free", MedVar{}, {temp(1)}, 0x200);
  User.ret(u0, {});

  auto Fs = audit({Helper.F, User.F});
  const Finding *UserLeak = nullptr;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == User.F.Entry)
      UserLeak = &F;
  ASSERT_NE(UserLeak, nullptr);
  EXPECT_EQ(UserLeak->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, ExceptionalUseAfterFreeFailsClosed) {
  FB B("f", 0x100);
  int Allocate = B.block();
  int MayThrow = B.block();
  int Handler = B.block();
  int Exit = B.block();
  B.call(Allocate, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(Allocate, "free", MedVar{}, {temp(1)});
  B.succ(Allocate, MayThrow);
  B.call(MayThrow, "opaque_may_throw", MedVar{}, {});
  B.succ(MayThrow, Exit);
  ExceptionalEdge Edge;
  Edge.BlockId = Handler;
  B.F.Blocks[MayThrow].ExceptionalSuccs.push_back(Edge);
  B.op(Handler, NdOp::LOAD, temp(2), {temp(1)}, 0x480);
  B.ret(Handler, {temp(2)});
  B.ret(Exit, {});

  auto Fs = audit({B.F});
  const Finding *UseAfterFree = find(Fs, VulnClass::UseAfterFree);
  ASSERT_NE(UseAfterFree, nullptr);
  EXPECT_EQ(UseAfterFree->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(UseAfterFree->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, ExceptionalDoubleFreeFailsClosed) {
  FB B("f", 0x100);
  int Allocate = B.block();
  int MayThrow = B.block();
  int Handler = B.block();
  int Exit = B.block();
  B.call(Allocate, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(Allocate, "free", MedVar{}, {temp(1)});
  B.succ(Allocate, MayThrow);
  B.call(MayThrow, "opaque_may_throw", MedVar{}, {});
  B.succ(MayThrow, Exit);
  ExceptionalEdge Edge;
  Edge.BlockId = Handler;
  B.F.Blocks[MayThrow].ExceptionalSuccs.push_back(Edge);
  B.call(Handler, "free", MedVar{}, {temp(1)});
  B.ret(Handler, {});
  B.ret(Exit, {});

  auto Fs = audit({B.F});
  const Finding *DoubleFree = find(Fs, VulnClass::DoubleFree);
  ASSERT_NE(DoubleFree, nullptr);
  EXPECT_EQ(DoubleFree->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(DoubleFree->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, ExceptionalOnlyLeakSurvivesNormalPathCorroboration) {
  FB User("user", 0x100);
  int Allocate = User.block();
  int MayThrow = User.block();
  int Release = User.block();
  int Handler = User.block();
  User.call(Allocate, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000,
            0x400);
  User.succ(Allocate, MayThrow);
  User.call(MayThrow, "helper", MedVar{}, {}, 0x200, 0x408);
  User.succ(MayThrow, Release);
  ExceptionalEdge MedEdge;
  MedEdge.BlockId = Handler;
  User.F.Blocks[MayThrow].ExceptionalSuccs.push_back(MedEdge);
  User.call(Release, "free", MedVar{}, {temp(1)}, 0x9100, 0x410);
  User.ret(Release, {});
  User.ret(Handler, {});

  FB Helper("helper", 0x200);
  int HelperBlock = Helper.block();
  Helper.ret(HelperBlock, {});

  LowFunc Low;
  Low.Entry = User.F.Entry;
  Low.DecodedInstructionCount = 4;
  Low.LiftedInstructionCount = 4;
  Low.Blocks.resize(4);
  for (int I = 0; I < 4; ++I)
    Low.Blocks[I].Id = I;
  Low.Blocks[Allocate].StartAddr = 0x400;
  Low.Blocks[Allocate].EndAddr = 0x408;
  Low.Blocks[Allocate].Succs = {MayThrow};
  Low.Blocks[Allocate].Ops.push_back(
      lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9000, 8)}, 0x400));
  Low.Blocks[MayThrow].StartAddr = 0x408;
  Low.Blocks[MayThrow].EndAddr = 0x410;
  Low.Blocks[MayThrow].Preds = {Allocate};
  Low.Blocks[MayThrow].Succs = {Release};
  Low.Blocks[MayThrow].ExceptionalSuccs.push_back(MedEdge);
  Low.Blocks[MayThrow].Ops.push_back(
      lowOp(NdOp::CALL, NdVar{}, {NdVar::cst(0x200, 8)}, 0x408));
  Low.Blocks[Release].StartAddr = 0x410;
  Low.Blocks[Release].EndAddr = 0x418;
  Low.Blocks[Release].Preds = {MayThrow};
  Low.Blocks[Release].Ops.push_back(
      lowOp(NdOp::CALL, NdVar{}, {NdVar::cst(0x9100, 8)}, 0x410));
  Low.Blocks[Release].Ops.push_back(lowOp(NdOp::RETURN, NdVar{}, {}));
  Low.Blocks[Handler].StartAddr = 0x418;
  Low.Blocks[Handler].EndAddr = 0x420;
  ExceptionalEdge LowPred = MedEdge;
  LowPred.BlockId = MayThrow;
  Low.Blocks[Handler].ExceptionalPreds.push_back(LowPred);
  Low.Blocks[Handler].Ops.push_back(lowOp(NdOp::RETURN, NdVar{}, {}));

  BinaryImage Img;
  Img.Arch = Arch::X64;
  std::vector<MedFunc> MedFuncs{User.F, Helper.F};
  std::vector<LowFunc> LowFuncs{std::move(Low)};
  AnalysisInput In;
  In.Img = &Img;
  In.LowFuncs = &LowFuncs;
  In.MedFuncs = &MedFuncs;
  auto Fs = auditMemory(In, SinkCatalog::defaults(), SafetyBudgets{});

  const Finding *Leak = find(Fs, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Leak->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, DoubleFreeSequential) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, 0x400);
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, 0x408);
  B.ret(b0, {});
  auto Fs = audit({B.F});
  EXPECT_TRUE(has(Fs, VulnClass::DoubleFree));
}

TEST(AllocLifetime, SiblingFreesAreNotDoubleFree) {
  FB B("f", 0x100);
  int b0 = B.block();
  int b1 = B.block();
  int b2 = B.block();
  int b3 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.succ(b0, b1);
  B.succ(b0, b2);
  B.call(b1, "free", MedVar{}, {temp(1)}, 0x9100, 0x410);
  B.call(b2, "free", MedVar{}, {temp(1)}, 0x9100, 0x420);
  B.succ(b1, b3);
  B.succ(b2, b3);
  B.ret(b3, {});
  auto Fs = audit({B.F});
  EXPECT_FALSE(has(Fs, VulnClass::DoubleFree));
}

TEST(AllocLifetime, RepeatedFreeSiteAcrossLoopBackedgeIsDoubleFree) {
  FB B("f", 0x100);
  int entry = B.block();
  int loop = B.block();
  int exit = B.block();
  B.call(entry, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.succ(entry, loop);
  B.call(loop, "free", MedVar{}, {temp(1)}, 0x9100, 0x408);
  B.succ(loop, loop);
  B.succ(loop, exit);
  B.ret(exit, {});

  EXPECT_TRUE(has(audit({B.F}), VulnClass::DoubleFree));
}

TEST(AllocLifetime, ReallocationOnLoopBackedgePreventsSameSiteDoubleFree) {
  FB B("f", 0x100);
  int loop = B.block();
  int exit = B.block();
  B.call(loop, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(loop, "free", MedVar{}, {temp(1)}, 0x9100, 0x408);
  B.succ(loop, loop);
  B.succ(loop, exit);
  B.ret(exit, {});

  EXPECT_FALSE(has(audit({B.F}), VulnClass::DoubleFree));
}

TEST(AllocLifetime, UseAfterFree) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, 0x400);
  // *p = load(p) after free.
  B.op(b0, NdOp::LOAD, temp(2), {temp(1)}, 0x408);
  B.ret(b0, {});
  auto Fs = audit({B.F});
  EXPECT_TRUE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, RuntimeAdjustedPointerUseAfterFreeFailsClosed) {
  constexpr va_t MallocVA = 0x400;
  constexpr va_t FreeVA = 0x408;
  constexpr va_t LoadVA = 0x410;
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, MallocVA);
  B.op(b0, NdOp::INT_ADD, temp(2), {temp(1), temp(9)});
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, FreeVA);
  B.op(b0, NdOp::LOAD, temp(3), {temp(2)}, LoadVA);
  B.ret(b0, {});

  LowFunc LF;
  LF.Entry = B.F.Entry;
  LF.DecodedInstructionCount = 1;
  LF.LiftedInstructionCount = 1;
  LowBlock LB;
  LB.Id = b0;
  LB.StartAddr = B.F.Entry;
  LB.EndAddr = LoadVA + 8;
  LB.Ops.push_back(
      lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9000, 8)}, MallocVA));
  LB.Ops.push_back(lowOp(NdOp::CALL, NdVar{}, {NdVar::cst(0x9100, 8)}, FreeVA));
  LB.Ops.push_back(
      lowOp(NdOp::LOAD, NdVar::reg(1, 8), {NdVar::reg(2, 8)}, LoadVA));
  LB.Ops.push_back(lowOp(NdOp::RETURN, NdVar{}, {}, LoadVA + 4));
  LF.Blocks.push_back(std::move(LB));

  BinaryImage Img;
  Img.Arch = Arch::X64;
  std::vector<MedFunc> MedFuncs{B.F};
  std::vector<LowFunc> LowFuncs{std::move(LF)};
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &MedFuncs;
  In.LowFuncs = &LowFuncs;
  const std::vector<Finding> Findings =
      auditHeap(In, SinkCatalog::defaults(), SafetyBudgets{});
  const Finding *Use = find(Findings, VulnClass::UseAfterFree);
  ASSERT_NE(Use, nullptr);
  EXPECT_EQ(Use->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Use->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, StringDuplicationReadsFreedSource) {
  for (const char *Name : {"strdup", "strndup", "wcsdup", "_wcsdup"}) {
    SCOPED_TRACE(Name);
    FB B("f", 0x100);
    int b0 = B.block();
    B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
    B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, 0x400);
    std::vector<MedVar> Args = {temp(1)};
    if (llvm::StringRef(Name) == "strndup")
      Args.push_back(MedVar::makeConst(1, 8));
    B.call(b0, Name, temp(2), std::move(Args), 0x9200, 0x408);
    B.call(b0, "free", MedVar{}, {temp(2)});
    B.ret(b0, {});

    EXPECT_TRUE(has(audit({B.F}), VulnClass::UseAfterFree));
  }
}

TEST(AllocLifetime, ZeroLengthStrndupDoesNotReadFreedSource) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, 0x400);
  B.call(b0, "strndup", temp(2), {temp(1), MedVar::makeConst(0, 8)}, 0x9200,
         0x408);
  B.call(b0, "free", MedVar{}, {temp(2)});
  B.ret(b0, {});

  EXPECT_FALSE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, NarrowZeroStrndupLimitDoesNotSuppressPossibleUse) {
  BinaryImage Img;
  Img.Arch = Arch::X64;

  FB B("f", 0x100);
  const int Block = B.block();
  B.call(Block, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(Block, "free", MedVar{}, {temp(1)});
  B.call(Block, "strndup", temp(2), {temp(1), MedVar::makeConst(0, 1)});
  B.ret(Block, {});

  const std::vector<Finding> Findings = audit({B.F}, &Img);
  const Finding *Use = find(Findings, VulnClass::UseAfterFree);
  ASSERT_NE(Use, nullptr);
  EXPECT_EQ(Use->TheVerdict, Verdict::Unknown) << Use->Detail;
  EXPECT_EQ(Use->TheConfidence, Confidence::Low) << Use->Detail;
}

TEST(AllocLifetime, RuntimeLengthStrndupUseAfterFreeFailsClosed) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, 0x400);
  B.call(b0, "strndup", temp(2), {temp(1), temp(8)}, 0x9200, 0x408);
  B.call(b0, "free", MedVar{}, {temp(2)});
  B.ret(b0, {});

  const std::vector<Finding> Fs = audit({B.F});
  const Finding *Use = find(Fs, VulnClass::UseAfterFree);
  ASSERT_NE(Use, nullptr);
  EXPECT_EQ(Use->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Use->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, AtomicMemoryUseAfterFree) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, 0x400);
  B.op(b0, NdOp::ATOMIC_ADD, temp(2), {temp(1), MedVar::makeConst(1, 8)},
       0x408);
  B.ret(b0, {});

  EXPECT_TRUE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, AtomicValueDoesNotDereferenceFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, 0x400);
  B.op(b0, NdOp::ATOMIC_XCHG, temp(2), {MedVar::makeConst(0x5000, 8), temp(1)},
       0x408);
  B.ret(b0, {});

  EXPECT_FALSE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, ZeroLengthMemcpyDoesNotUseFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "memcpy", temp(2), {temp(1), temp(3), MedVar::makeConst(0, 8)});
  B.ret(b0, {});

  auto Fs = audit({B.F});
  EXPECT_FALSE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, NarrowZeroCopyLengthDoesNotSuppressPossibleUse) {
  BinaryImage Img;
  Img.Arch = Arch::X64;

  FB B("f", 0x100);
  const int Block = B.block();
  B.call(Block, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(Block, "free", MedVar{}, {temp(1)});
  B.call(Block, "memcpy", temp(2), {temp(3), temp(1), MedVar::makeConst(0, 1)});
  B.ret(Block, {});

  const std::vector<Finding> Findings = audit({B.F}, &Img);
  const Finding *Use = find(Findings, VulnClass::UseAfterFree);
  ASSERT_NE(Use, nullptr);
  EXPECT_EQ(Use->TheVerdict, Verdict::Unknown) << Use->Detail;
  EXPECT_EQ(Use->TheConfidence, Confidence::Low) << Use->Detail;
}

TEST(AllocLifetime, RelocatedZeroLengthDoesNotSuppressFreedStorageUse) {
  for (const Arch A : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM}) {
    SCOPED_TRACE(static_cast<int>(A));
    const uint16_t PointerSize = (A == Arch::X86 || A == Arch::ARM) ? 4 : 8;
    BinaryImage Img;
    Img.Arch = A;

    FB B("f", 0x100);
    const int Block = B.block();
    B.call(Block, "malloc", temp(1, PointerSize),
           {MedVar::makeConst(16, PointerSize)});
    B.call(Block, "free", MedVar{}, {temp(1, PointerSize)});
    B.call(Block, "memcpy", temp(2, PointerSize),
           {temp(3, PointerSize), temp(1, PointerSize),
            MedVar::makeConst(0, PointerSize,
                              ConstantAddressProvenance::DataAddress, 0)});
    B.ret(Block, {});

    const std::vector<Finding> Findings = audit({B.F}, &Img);
    const Finding *Use = find(Findings, VulnClass::UseAfterFree);
    ASSERT_NE(Use, nullptr);
    EXPECT_EQ(Use->TheVerdict, Verdict::Unknown);
  }
}

TEST(AllocLifetime, ArmEabiMemoryHelpersHonorZeroLength) {
  for (const char *Name : {"__aeabi_memcpy4", "__aeabi_memmove8",
                           "__aeabi_memset", "__aeabi_memclr"}) {
    for (uint64_t Count : {uint64_t(0), uint64_t(1)}) {
      SCOPED_TRACE(Name);
      SCOPED_TRACE(Count);
      FB B("f", 0x100);
      int b0 = B.block();
      B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
      B.call(b0, "free", MedVar{}, {temp(1)});
      std::vector<MedVar> Args;
      if (llvm::StringRef(Name).contains("memcpy") ||
          llvm::StringRef(Name).contains("memmove"))
        Args = {temp(1), temp(3), MedVar::makeConst(Count, 8)};
      else if (llvm::StringRef(Name).contains("memset"))
        Args = {temp(1), MedVar::makeConst(Count, 8),
                MedVar::makeConst(0x5a, 8)};
      else
        Args = {temp(1), MedVar::makeConst(Count, 8)};
      B.call(b0, Name, temp(2), std::move(Args));
      B.ret(b0, {});

      EXPECT_EQ(has(audit({B.F}), VulnClass::UseAfterFree), Count != 0);
    }
  }
}

TEST(AllocLifetime, PositiveLengthMemcpyUsesFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "memcpy", temp(2), {temp(1), temp(3), MedVar::makeConst(1, 8)});
  B.ret(b0, {});

  auto Fs = audit({B.F});
  EXPECT_TRUE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, RejectedFortifiedCopyDoesNotUseFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "memcpy_chk", temp(2),
         {temp(1), temp(1), MedVar::makeConst(8, 8), MedVar::makeConst(4, 8)});
  B.ret(b0, {});

  EXPECT_FALSE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, AcceptedFortifiedCopyUsesFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "memcpy_chk", temp(2),
         {temp(1), temp(1), MedVar::makeConst(4, 8), MedVar::makeConst(8, 8)});
  B.ret(b0, {});

  EXPECT_TRUE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, NarrowFortifiedCapacityCannotSuppressPossibleUse) {
  BinaryImage Img;
  Img.Arch = Arch::X64;

  FB B("f", 0x100);
  const int Block = B.block();
  B.call(Block, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(Block, "free", MedVar{}, {temp(1)});
  B.call(Block, "memcpy_chk", temp(2),
         {temp(3), temp(1), MedVar::makeConst(8, 8), MedVar::makeConst(4, 1)});
  B.ret(Block, {});

  const std::vector<Finding> Findings = audit({B.F}, &Img);
  const Finding *Use = find(Findings, VulnClass::UseAfterFree);
  ASSERT_NE(Use, nullptr);
  EXPECT_EQ(Use->TheVerdict, Verdict::Unknown) << Use->Detail;
  EXPECT_EQ(Use->TheConfidence, Confidence::Low) << Use->Detail;
}

TEST(AllocLifetime, RejectedFortifiedStringCopyDoesNotUseFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "strncpy_chk", temp(2),
         {temp(1), temp(1), MedVar::makeConst(8, 8), MedVar::makeConst(4, 8)});
  B.ret(b0, {});

  EXPECT_FALSE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, AcceptedFortifiedStringCopyUsesFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "strncpy_chk", temp(2),
         {temp(1), temp(1), MedVar::makeConst(4, 8), MedVar::makeConst(8, 8)});
  B.ret(b0, {});

  EXPECT_TRUE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, ZeroLengthWideCopyDoesNotUseFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "wmemmove", temp(2), {temp(1), temp(3), MedVar::makeConst(0, 8)});
  B.ret(b0, {});

  auto Fs = audit({B.F});
  EXPECT_FALSE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, RuntimeLengthMemcpyUseAfterFreeFailsClosed) {
  constexpr va_t MallocVA = 0x400;
  constexpr va_t FreeVA = 0x408;
  constexpr va_t MemcpyVA = 0x410;
  FB B("f", 0x100);
  B.F.Params.push_back(temp(8));
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, MallocVA);
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, FreeVA);
  B.call(b0, "memcpy", temp(2), {temp(1), temp(3), temp(8)}, 0x9200, MemcpyVA);
  B.ret(b0, {});

  LowFunc LF;
  LF.Entry = B.F.Entry;
  LF.DecodedInstructionCount = 1;
  LF.LiftedInstructionCount = 1;
  LowBlock LB;
  LB.Id = b0;
  LB.StartAddr = B.F.Entry;
  LB.EndAddr = MemcpyVA + 8;
  LB.Ops.push_back(
      lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9000, 8)}, MallocVA));
  LB.Ops.push_back(lowOp(NdOp::CALL, NdVar{}, {NdVar::cst(0x9100, 8)}, FreeVA));
  LB.Ops.push_back(
      lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9200, 8)}, MemcpyVA));
  LB.Ops.push_back(lowOp(NdOp::RETURN, NdVar{}, {}, MemcpyVA + 4));
  LF.Blocks.push_back(std::move(LB));

  BinaryImage Img;
  Img.Arch = Arch::X64;
  std::vector<MedFunc> MedFuncs{B.F};
  std::vector<LowFunc> LowFuncs{std::move(LF)};
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &MedFuncs;
  In.LowFuncs = &LowFuncs;
  const std::vector<Finding> Fs =
      auditHeap(In, SinkCatalog::defaults(), SafetyBudgets{});
  const Finding *Use = find(Fs, VulnClass::UseAfterFree);
  ASSERT_NE(Use, nullptr);
  EXPECT_EQ(Use->TheVerdict, Verdict::Unknown) << Use->Detail;
  EXPECT_EQ(Use->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, ZeroLengthStrncpyDoesNotUseFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "strncpy", temp(2), {temp(1), temp(3), MedVar::makeConst(0, 8)});
  B.ret(b0, {});

  EXPECT_FALSE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, ZeroSizeStrlOperationsUseOnlyTheirSource) {
  for (const char *Name : {"strlcpy", "strlcat"}) {
    FB FreedDst("freed_dst", 0x100);
    int d0 = FreedDst.block();
    FreedDst.call(d0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
    FreedDst.call(d0, "free", MedVar{}, {temp(1)});
    FreedDst.call(d0, Name, temp(2),
                  {temp(1), temp(3), MedVar::makeConst(0, 8)});
    FreedDst.ret(d0, {});
    EXPECT_FALSE(has(audit({FreedDst.F}), VulnClass::UseAfterFree)) << Name;

    FB FreedSrc("freed_src", 0x200);
    int s0 = FreedSrc.block();
    FreedSrc.call(s0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
    FreedSrc.call(s0, "free", MedVar{}, {temp(1)});
    FreedSrc.call(s0, Name, temp(2),
                  {temp(3), temp(1), MedVar::makeConst(0, 8)});
    FreedSrc.ret(s0, {});
    EXPECT_TRUE(has(audit({FreedSrc.F}), VulnClass::UseAfterFree)) << Name;
  }
}

TEST(AllocLifetime, ZeroCountStrncatUsesOnlyItsDestination) {
  FB FreedSrc("freed_src", 0x100);
  int s0 = FreedSrc.block();
  FreedSrc.call(s0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  FreedSrc.call(s0, "free", MedVar{}, {temp(1)});
  FreedSrc.call(s0, "strncat", temp(2),
                {temp(3), temp(1), MedVar::makeConst(0, 8)});
  FreedSrc.ret(s0, {});
  EXPECT_FALSE(has(audit({FreedSrc.F}), VulnClass::UseAfterFree));

  FB FreedDst("freed_dst", 0x200);
  int d0 = FreedDst.block();
  FreedDst.call(d0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  FreedDst.call(d0, "free", MedVar{}, {temp(1)});
  FreedDst.call(d0, "strncat", temp(2),
                {temp(1), temp(3), MedVar::makeConst(0, 8)});
  FreedDst.ret(d0, {});
  EXPECT_TRUE(has(audit({FreedDst.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, ZeroSizeSnprintfUsesOnlyItsFormat) {
  FB FreedDst("freed_dst", 0x100);
  int d0 = FreedDst.block();
  FreedDst.call(d0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  FreedDst.call(d0, "free", MedVar{}, {temp(1)});
  FreedDst.call(d0, "snprintf", temp(2),
                {temp(1), MedVar::makeConst(0, 8), temp(3)});
  FreedDst.ret(d0, {});
  EXPECT_FALSE(has(audit({FreedDst.F}), VulnClass::UseAfterFree));

  FB FreedFormat("freed_format", 0x200);
  int f0 = FreedFormat.block();
  FreedFormat.call(f0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  FreedFormat.call(f0, "free", MedVar{}, {temp(1)});
  FreedFormat.call(f0, "snprintf", temp(2),
                   {temp(3), MedVar::makeConst(0, 8), temp(1)});
  FreedFormat.ret(f0, {});
  EXPECT_TRUE(has(audit({FreedFormat.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, PositiveSizeSnprintfUsesFreedDestination) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "snprintf", temp(2), {temp(1), MedVar::makeConst(1, 8), temp(3)});
  B.ret(b0, {});

  EXPECT_TRUE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, RejectedFortifiedSnprintfDoesNotUseFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "snprintf_chk", temp(2),
         {temp(1), MedVar::makeConst(8, 8), MedVar::makeConst(2, 4),
          MedVar::makeConst(4, 8), temp(1)});
  B.ret(b0, {});

  EXPECT_FALSE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, AcceptedFortifiedSnprintfUsesFreedStorage) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "snprintf_chk", temp(2),
         {temp(1), MedVar::makeConst(4, 8), MedVar::makeConst(2, 4),
          MedVar::makeConst(8, 8), temp(1)});
  B.ret(b0, {});

  EXPECT_TRUE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, AtomicReadResultPreservesHeapAlias) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), temp(1)});
  B.op(b0, NdOp::ATOMIC_XCHG, temp(2), {temp(10), MedVar::makeConst(0, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.op(b0, NdOp::LOAD, temp(3), {temp(2)});
  B.ret(b0, {});

  EXPECT_TRUE(
      has(audit({B.F}, nullptr, /*StackRegs=*/true), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, UseAfterFreeAcrossLoopBackedge) {
  FB B("f", 0x100);
  int entry = B.block();
  int loop = B.block();
  int exit = B.block();
  B.call(entry, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.succ(entry, loop);
  B.op(loop, NdOp::LOAD, temp(2), {temp(1)}, 0x400);
  B.call(loop, "free", MedVar{}, {temp(1)}, 0x9100, 0x408);
  B.succ(loop, loop);
  B.succ(loop, exit);
  B.ret(exit, {});

  EXPECT_TRUE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, UseBeforeFreeIsClean) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(b0, NdOp::LOAD, temp(2), {temp(1)}, 0x400); // use first
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, 0x408);
  B.ret(b0, {});
  auto Fs = audit({B.F});
  EXPECT_FALSE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, FreedPointerUsedAsAllocatorSizeIsNotAUseAfterFree) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "malloc", temp(2), {temp(1)});
  B.call(b0, "free", MedVar{}, {temp(2)});
  B.ret(b0, {});

  EXPECT_FALSE(has(audit({B.F}), VulnClass::UseAfterFree));
}

TEST(AllocLifetime, UnknownCallUseOfFreedPointerIsNotDefinite) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.call(b0, "observe_pointer", MedVar{}, {temp(1)}, 0x9200);
  B.ret(b0, {});

  const std::vector<Finding> Fs = audit({B.F});
  const Finding *Use = find(Fs, VulnClass::UseAfterFree);
  ASSERT_NE(Use, nullptr);
  EXPECT_EQ(Use->TheVerdict, Verdict::Unknown);
  EXPECT_NE(Use->Detail.find("may access"), std::string::npos);
}

TEST(AllocLifetime, ZeroLengthInputSourcesDoNotUseFreedOutputBuffer) {
  struct SourceCase {
    const char *Name;
    std::vector<MedVar> Args;
  };
  const auto Buffer = temp(1);
  const std::vector<SourceCase> Cases = {
      {"read", {temp(3), Buffer, MedVar::makeConst(0, 8)}},
      {"pread",
       {temp(3), Buffer, MedVar::makeConst(0, 8), MedVar::makeConst(0, 8)}},
      {"recv",
       {temp(3), Buffer, MedVar::makeConst(0, 8), MedVar::makeConst(0, 8)}},
      {"recvfrom",
       {temp(3), Buffer, MedVar::makeConst(0, 8), MedVar::makeConst(0, 8),
        temp(4), temp(5)}},
      {"fread",
       {Buffer, MedVar::makeConst(0, 8), MedVar::makeConst(4, 8), temp(3)}},
      {"fread",
       {Buffer, MedVar::makeConst(4, 8), MedVar::makeConst(0, 8), temp(3)}},
      {"ReadFile",
       {temp(3), Buffer, MedVar::makeConst(0, 8), temp(4), temp(5)}},
      {"GetEnvironmentVariableA", {temp(3), Buffer, MedVar::makeConst(0, 8)}}};

  for (const SourceCase &C : Cases) {
    SCOPED_TRACE(C.Name);
    FB B("f", 0x100);
    int b0 = B.block();
    B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
    B.call(b0, "free", MedVar{}, {temp(1)});
    B.call(b0, C.Name, temp(2), C.Args);
    B.ret(b0, {});

    BinaryImage Img;
    Img.Arch = Arch::X64;
    Img.Format = llvm::StringRef(C.Name).starts_with("ReadFile") ||
                         llvm::StringRef(C.Name).starts_with("GetEnvironment")
                     ? BinaryFormat::COFF
                     : BinaryFormat::ELF;
    EXPECT_FALSE(has(audit({B.F}, &Img), VulnClass::UseAfterFree));
  }
}

TEST(AllocLifetime, NarrowZeroInputBoundDoesNotSuppressPossibleUse) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Format = BinaryFormat::ELF;

  struct SourceCase {
    const char *Name;
    std::vector<MedVar> Args;
  } Cases[] = {
      {"read", {temp(3), temp(1), MedVar::makeConst(0, 1)}},
      {"fread",
       {temp(1), MedVar::makeConst(0, 1), MedVar::makeConst(4, 8), temp(3)}},
  };

  for (const SourceCase &C : Cases) {
    SCOPED_TRACE(C.Name);
    FB B("f", 0x100);
    const int Block = B.block();
    B.call(Block, "malloc", temp(1), {MedVar::makeConst(16, 8)});
    B.call(Block, "free", MedVar{}, {temp(1)});
    B.call(Block, C.Name, temp(2), C.Args);
    B.ret(Block, {});

    const std::vector<Finding> Findings = audit({B.F}, &Img);
    const Finding *Use = find(Findings, VulnClass::UseAfterFree);
    ASSERT_NE(Use, nullptr);
    EXPECT_EQ(Use->TheVerdict, Verdict::Unknown) << Use->Detail;
    EXPECT_EQ(Use->TheConfidence, Confidence::Low) << Use->Detail;
  }
}

TEST(AllocLifetime, Win32SourceCountsUseTheirLowCarrierBits) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Format = BinaryFormat::COFF;

  struct SourceCase {
    const char *Name;
    std::vector<MedVar> Args;
  } Cases[] = {
      {"_read",
       {temp(3), temp(1), MedVar::makeConst(UINT64_C(0x100000000), 8)}},
      {"ReadFile",
       {temp(3), temp(1), MedVar::makeConst(UINT64_C(0x100000000), 8), temp(4),
        temp(5)}},
      {"GetEnvironmentVariableA",
       {temp(3), temp(1), MedVar::makeConst(UINT64_C(0x100000000), 8)}},
  };

  for (const SourceCase &C : Cases) {
    SCOPED_TRACE(C.Name);
    FB B("f", 0x100);
    const int Block = B.block();
    B.call(Block, "malloc", temp(1), {MedVar::makeConst(16, 8)});
    B.call(Block, "free", MedVar{}, {temp(1)});
    B.call(Block, C.Name, temp(2), C.Args);
    B.ret(Block, {});

    EXPECT_FALSE(has(audit({B.F}, &Img), VulnClass::UseAfterFree));
  }
}

TEST(AllocLifetime, FallibleInputSourceUseRemainsUnknown) {
  constexpr va_t MallocVA = 0x400;
  constexpr va_t FreeVA = 0x408;
  constexpr va_t SourceVA = 0x410;
  struct SourceCase {
    const char *Name;
    std::vector<MedVar> Args;
  };
  for (const SourceCase &C :
       {SourceCase{"read", {temp(3), temp(1), MedVar::makeConst(1, 8)}},
        SourceCase{"fgets", {temp(1), MedVar::makeConst(16, 8), temp(3)}},
        SourceCase{"gets", {temp(1)}}}) {
    SCOPED_TRACE(C.Name);
    FB B("f", 0x100);
    int b0 = B.block();
    B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, MallocVA);
    B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, FreeVA);
    B.call(b0, C.Name, temp(2), C.Args, 0x9200, SourceVA);
    B.ret(b0, {});

    LowFunc LF;
    LF.Entry = B.F.Entry;
    LF.DecodedInstructionCount = 1;
    LF.LiftedInstructionCount = 1;
    LowBlock LB;
    LB.Id = b0;
    LB.StartAddr = B.F.Entry;
    LB.EndAddr = SourceVA + 8;
    LB.Ops.push_back(
        lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9000, 8)}, MallocVA));
    LB.Ops.push_back(
        lowOp(NdOp::CALL, NdVar{}, {NdVar::cst(0x9100, 8)}, FreeVA));
    LB.Ops.push_back(
        lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9200, 8)}, SourceVA));
    LB.Ops.push_back(lowOp(NdOp::RETURN, NdVar{}, {}, SourceVA + 4));
    LF.Blocks.push_back(std::move(LB));

    BinaryImage Img;
    Img.Arch = Arch::X64;
    std::vector<MedFunc> MedFuncs{B.F};
    std::vector<LowFunc> LowFuncs{std::move(LF)};
    AnalysisInput In;
    In.Img = &Img;
    In.MedFuncs = &MedFuncs;
    In.LowFuncs = &LowFuncs;
    const std::vector<Finding> Fs =
        auditHeap(In, SinkCatalog::defaults(), SafetyBudgets{});
    const Finding *Use = find(Fs, VulnClass::UseAfterFree);
    ASSERT_NE(Use, nullptr);
    EXPECT_EQ(Use->TheVerdict, Verdict::Unknown);
    EXPECT_EQ(Use->TheConfidence, Confidence::Low);
    EXPECT_NE(Use->Detail.find("may access"), std::string::npos);
  }
}

TEST(AllocLifetime, StringLengthCallDefinitelyUsesFreedStorage) {
  constexpr va_t MallocVA = 0x400;
  constexpr va_t FreeVA = 0x408;
  constexpr va_t StrlenVA = 0x410;
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, MallocVA);
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, FreeVA);
  B.call(b0, "strlen", temp(2), {temp(1)}, 0x9200, StrlenVA);
  B.ret(b0, {});

  LowFunc LF;
  LF.Entry = B.F.Entry;
  LF.DecodedInstructionCount = 1;
  LF.LiftedInstructionCount = 1;
  LowBlock LB;
  LB.Id = b0;
  LB.StartAddr = B.F.Entry;
  LB.EndAddr = StrlenVA + 8;
  LB.Ops.push_back(
      lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9000, 8)}, MallocVA));
  LB.Ops.push_back(lowOp(NdOp::CALL, NdVar{}, {NdVar::cst(0x9100, 8)}, FreeVA));
  LB.Ops.push_back(
      lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9200, 8)}, StrlenVA));
  LB.Ops.push_back(lowOp(NdOp::RETURN, NdVar{}, {}, StrlenVA + 4));
  LF.Blocks.push_back(std::move(LB));

  BinaryImage Img;
  Img.Arch = Arch::X64;
  std::vector<MedFunc> MedFuncs{B.F};
  std::vector<LowFunc> LowFuncs{std::move(LF)};
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &MedFuncs;
  In.LowFuncs = &LowFuncs;
  const std::vector<Finding> Fs =
      auditHeap(In, SinkCatalog::defaults(), SafetyBudgets{});
  const Finding *Use = find(Fs, VulnClass::UseAfterFree);
  ASSERT_NE(Use, nullptr);
  EXPECT_EQ(Use->TheVerdict, Verdict::Unsafe) << Use->Detail;
  EXPECT_EQ(Use->TheConfidence, Confidence::High);
  EXPECT_FALSE(Use->Corroboration.empty());
}

TEST(AllocLifetime, ScanfFixedPrefixDefinitelyUsesFreedStorage) {
  constexpr va_t MallocVA = 0x400;
  constexpr va_t FreeVA = 0x408;
  constexpr va_t ScanfVA = 0x410;
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, MallocVA);
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, FreeVA);
  B.call(b0, "sscanf", temp(2),
         {temp(1), MedVar::makeConst(0x5000, 8), temp(3)}, 0x9200, ScanfVA);
  B.ret(b0, {});

  LowFunc LF;
  LF.Entry = B.F.Entry;
  LF.DecodedInstructionCount = 1;
  LF.LiftedInstructionCount = 1;
  LowBlock LB;
  LB.Id = b0;
  LB.StartAddr = B.F.Entry;
  LB.EndAddr = ScanfVA + 8;
  LB.Ops.push_back(
      lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9000, 8)}, MallocVA));
  LB.Ops.push_back(lowOp(NdOp::CALL, NdVar{}, {NdVar::cst(0x9100, 8)}, FreeVA));
  LB.Ops.push_back(
      lowOp(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9200, 8)}, ScanfVA));
  LB.Ops.push_back(lowOp(NdOp::RETURN, NdVar{}, {}, ScanfVA + 4));
  LF.Blocks.push_back(std::move(LB));

  BinaryImage Img;
  Img.Arch = Arch::X64;
  std::vector<MedFunc> MedFuncs{B.F};
  std::vector<LowFunc> LowFuncs{std::move(LF)};
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &MedFuncs;
  In.LowFuncs = &LowFuncs;
  const std::vector<Finding> Fs =
      auditHeap(In, SinkCatalog::defaults(), SafetyBudgets{});
  const Finding *Use = find(Fs, VulnClass::UseAfterFree);
  ASSERT_NE(Use, nullptr);
  EXPECT_EQ(Use->TheVerdict, Verdict::Unsafe) << Use->Detail;
  EXPECT_EQ(Use->TheConfidence, Confidence::High);
  EXPECT_FALSE(Use->Corroboration.empty());
}

TEST(AllocLifetime, OverwrittenSpillDoesNotRemainAHeapAlias) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), temp(1)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0, 8)});
  B.op(b0, NdOp::LOAD, temp(2), {temp(10)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.op(b0, NdOp::LOAD, temp(3), {temp(2)});
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, DeepUnknownFrameWriteInvalidatesAHeapSpill) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), temp(1)});

  MedVar DeepAddress = temp(10);
  for (int I = 0; I < 70; ++I) {
    const MedVar Next = temp(100 + I);
    B.op(b0, NdOp::COPY, Next, {DeepAddress});
    DeepAddress = Next;
  }
  B.op(b0, NdOp::STORE, MedVar{}, {DeepAddress, MedVar::makeConst(0, 8)});
  B.op(b0, NdOp::LOAD, temp(2), {temp(10)});
  B.call(b0, "free", MedVar{}, {temp(1)});
  B.op(b0, NdOp::LOAD, temp(3), {temp(2)});
  B.ret(b0, {});

  const auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, UninitializedLocalStackLoadIsReported) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::LOAD, temp(11), {temp(10)}, 0x408);
  B.ret(b0, {temp(11)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->FuncEntry, 0x100u);
  EXPECT_EQ(Read->CallVA, 0x408u);
}

TEST(AllocLifetime, NarrowStackLoadAddressFailsClosed) {
  BinaryImage Img;
  Img.Arch = Arch::X64;

  FB B("f", 0x100);
  const int Block = B.block();
  B.op(Block, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(Block, NdOp::INT_ADD, temp(10),
       {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(Block, NdOp::LOAD, temp(11), {temp(10, 1)}, 0x408);
  B.ret(Block, {temp(11)});

  const std::vector<Finding> Findings =
      audit({B.F}, &Img, /*StackRegs=*/true, /*IncludeStackReads=*/true);
  const Finding *Read = find(Findings, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown) << Read->Detail;
  EXPECT_EQ(Read->TheConfidence, Confidence::Low) << Read->Detail;
  EXPECT_EQ(Read->Detail,
            "memory address has incompatible target pointer width");
}

TEST(AllocLifetime, RelocatedZeroCannotInitializeAStackSlot) {
  FB B("f", 0x100);
  const int Block = B.block();
  B.op(Block, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(Block, NdOp::INT_ADD, temp(10),
       {mkReg(kSP, 1),
        MedVar::makeConst(0, 8, ConstantAddressProvenance::DataAddress, 0)});
  B.op(Block, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(7, 8)});
  B.op(Block, NdOp::LOAD, temp(11), {mkReg(kSP, 1)}, 0x408);
  B.ret(Block, {temp(11)});

  const std::vector<Finding> Findings =
      audit({B.F}, nullptr, /*StackRegs=*/true,
            /*IncludeStackReads=*/true);
  const Finding *Read = find(Findings, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, UninitializedLocalStackLoadSurvivesPointerSpill) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x40, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::INT_ADD, temp(11),
       {mkReg(kSP, 1), MedVar::makeConst(0x28, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(11), temp(10)});
  B.op(b0, NdOp::LOAD, temp(12), {temp(11)});
  B.op(b0, NdOp::COPY, temp(13), {temp(12)});
  B.op(b0, NdOp::LOAD, temp(14), {temp(13)}, 0x408);
  B.ret(b0, {temp(14)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->CallVA, 0x408u);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, InitializedLocalStackLoadSurvivesPointerSpill) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x40, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0x1234, 8)});
  B.op(b0, NdOp::INT_ADD, temp(11),
       {mkReg(kSP, 1), MedVar::makeConst(0x28, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(11), temp(10)});
  B.op(b0, NdOp::LOAD, temp(12), {temp(11)});
  B.op(b0, NdOp::COPY, temp(13), {temp(12)});
  B.op(b0, NdOp::LOAD, temp(14), {temp(13)}, 0x408);
  B.ret(b0, {temp(14)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, UninitializedLocalStackMemcpySourceIsReported) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(b0, "memcpy", temp(11), {temp(12), temp(10), MedVar::makeConst(8, 8)},
         0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->Name, "memcpy");
  EXPECT_EQ(Read->CallVA, 0x408u);
}

TEST(AllocLifetime, NarrowStackCopySourceFailsClosed) {
  BinaryImage Img;
  Img.Arch = Arch::X64;

  FB B("f", 0x100);
  const int Block = B.block();
  B.op(Block, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(Block, NdOp::INT_ADD, temp(10),
       {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(Block, "memcpy", temp(11),
         {temp(12), temp(10, 1), MedVar::makeConst(8, 8)}, 0x9000, 0x408);
  B.ret(Block, {});

  const std::vector<Finding> Findings =
      audit({B.F}, &Img, /*StackRegs=*/true, /*IncludeStackReads=*/true);
  const Finding *Read = find(Findings, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown) << Read->Detail;
  EXPECT_EQ(Read->TheConfidence, Confidence::Low) << Read->Detail;
  EXPECT_EQ(Read->Detail, "call source pointer has incompatible target width");
}

TEST(AllocLifetime, StringDuplicationReadsUninitializedStackSource) {
  for (const char *Name : {"strdup", "strndup", "wcsdup", "_wcsdup"}) {
    SCOPED_TRACE(Name);
    FB B("f", 0x100);
    int b0 = B.block();
    B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
         {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
    B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
    std::vector<MedVar> Args = {temp(10)};
    if (llvm::StringRef(Name) == "strndup")
      Args.push_back(MedVar::makeConst(1, 8));
    B.call(b0, Name, temp(11), std::move(Args), 0x9000, 0x408);
    B.ret(b0, {});

    const std::vector<Finding> Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                                          /*IncludeStackReads=*/true);
    const Finding *Read = find(Fs, VulnClass::UninitializedRead);
    ASSERT_NE(Read, nullptr);
    EXPECT_EQ(Read->CallVA, 0x408u);
  }
}

TEST(AllocLifetime, WideStringDuplicationReadsPlatformSizedElement) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
    SCOPED_TRACE(static_cast<int>(Format));
    BinaryImage Img;
    Img.Format = Format;

    FB B("f", 0x100);
    int b0 = B.block();
    B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
         {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
    B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
    // A two-byte zero is a complete Windows wchar_t but only half of the
    // first wchar_t on the ELF and Mach-O ABIs.
    B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0, 2)});
    B.call(b0, Format == BinaryFormat::COFF ? "_wcsdup" : "wcsdup", temp(11),
           {temp(10)}, 0x9000, 0x408);
    B.ret(b0, {});

    const bool HasRead = has(audit({B.F}, &Img, /*StackRegs=*/true,
                                   /*IncludeStackReads=*/true),
                             VulnClass::UninitializedRead);
    EXPECT_EQ(HasRead, Format != BinaryFormat::COFF);
  }
}

TEST(AllocLifetime, ZeroLengthStrndupDoesNotReadUninitializedStackSource) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(b0, "strndup", temp(11), {temp(10), MedVar::makeConst(0, 8)}, 0x9000,
         0x408);
  B.ret(b0, {});

  EXPECT_FALSE(has(audit({B.F}, nullptr, /*StackRegs=*/true,
                         /*IncludeStackReads=*/true),
                   VulnClass::UninitializedRead));
}

TEST(AllocLifetime, RuntimeLengthStrndupStackReadFailsClosed) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(b0, "strndup", temp(11), {temp(10), temp(8)}, 0x9000, 0x408);
  B.ret(b0, {});

  const std::vector<Finding> Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                                        /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Read->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, InitializedLocalStackMemcpySourceIsClean) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0x1234, 8)});
  B.call(b0, "memcpy", temp(11), {temp(12), temp(10), MedVar::makeConst(8, 8)},
         0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, ZeroLengthMemcpyDoesNotReadUninitializedSource) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(b0, "memcpy", temp(11), {temp(12), temp(10), MedVar::makeConst(0, 8)},
         0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, NarrowZeroLengthDoesNotSuppressPossibleStackRead) {
  BinaryImage Img;
  Img.Arch = Arch::X64;

  FB B("f", 0x100);
  const int Block = B.block();
  B.op(Block, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(Block, NdOp::INT_ADD, temp(10),
       {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(Block, "memcpy", temp(11),
         {temp(12), temp(10), MedVar::makeConst(0, 1)}, 0x9000, 0x408);
  B.ret(Block, {});

  const std::vector<Finding> Findings =
      audit({B.F}, &Img, /*StackRegs=*/true, /*IncludeStackReads=*/true);
  const Finding *Read = find(Findings, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown) << Read->Detail;
  EXPECT_EQ(Read->TheConfidence, Confidence::Low) << Read->Detail;
  EXPECT_TRUE(Read->Corroboration.empty()) << Read->Corroboration;
}

TEST(AllocLifetime, RejectedFortifiedCopyDoesNotReadUninitializedSource) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(b0, "memcpy_chk", temp(11),
         {temp(12), temp(10), MedVar::makeConst(8, 8), MedVar::makeConst(4, 8)},
         0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, AcceptedFortifiedCopyReadsUninitializedSource) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(b0, "memcpy_chk", temp(11),
         {temp(12), temp(10), MedVar::makeConst(4, 8), MedVar::makeConst(8, 8)},
         0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_TRUE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, NarrowFortifiedCapacityMakesStackReadConditional) {
  BinaryImage Img;
  Img.Arch = Arch::X64;

  FB B("f", 0x100);
  const int Block = B.block();
  B.op(Block, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(Block, NdOp::INT_ADD, temp(10),
       {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(Block, "memcpy_chk", temp(11),
         {temp(12), temp(10), MedVar::makeConst(8, 8), MedVar::makeConst(4, 1)},
         0x9000, 0x408);
  B.ret(Block, {});

  const std::vector<Finding> Findings =
      audit({B.F}, &Img, /*StackRegs=*/true, /*IncludeStackReads=*/true);
  const Finding *Read = find(Findings, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown) << Read->Detail;
  EXPECT_EQ(Read->TheConfidence, Confidence::Low) << Read->Detail;
  EXPECT_TRUE(Read->Corroboration.empty()) << Read->Corroboration;
  EXPECT_NE(Read->Detail.find("may read"), std::string::npos) << Read->Detail;
}

TEST(AllocLifetime, WideCopyReadsPlatformSizedElements) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
    BinaryImage Img;
    Img.Format = Format;

    FB B("f", 0x100);
    int b0 = B.block();
    B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
         {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
    B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
    B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0x1234, 4)});
    B.call(b0, "wmemcpy", temp(11),
           {temp(12), temp(10), MedVar::makeConst(4, 8)}, 0x9000, 0x408);
    B.ret(b0, {});

    auto Fs = audit({B.F}, &Img, /*StackRegs=*/true,
                    /*IncludeStackReads=*/true);
    const Finding *Read = find(Fs, VulnClass::UninitializedRead);
    ASSERT_NE(Read, nullptr) << static_cast<int>(Format);
    EXPECT_EQ(Read->TheVerdict, Verdict::Unknown) << static_cast<int>(Format);
  }
}

TEST(AllocLifetime, BoundedStringCopyIsNotTreatedAsAnExactMemoryRead) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0, 1)});
  B.call(b0, "strncpy", temp(11), {temp(12), temp(10), MedVar::makeConst(8, 8)},
         0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, RuntimeLengthStackMemcpyReadFailsClosed) {
  FB B("f", 0x100);
  B.F.Params.push_back(temp(20));
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(b0, "memcpy", temp(11), {temp(12), temp(10), temp(20)}, 0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Read->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, RuntimeLengthPastInitializedPrefixFailsClosed) {
  FB B("f", 0x100);
  B.F.Params.push_back(temp(20));
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0x12, 1)});
  B.call(b0, "memcpy", temp(11), {temp(12), temp(10), temp(20)}, 0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Read->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, OverflowingWideCopyLengthFailsClosed) {
  BinaryImage Img;
  Img.Format = BinaryFormat::ELF;

  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0x12, 1)});
  B.call(b0, "wmemcpy", temp(11),
         {temp(12), temp(10), MedVar::makeConst(0x4000000000000000ULL, 8)},
         0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, &Img, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Read->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, RuntimeLengthStackMemcpyReadSurvivesPointerSpill) {
  FB B("f", 0x100);
  B.F.Params.push_back(temp(20));
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x40, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::INT_ADD, temp(13),
       {mkReg(kSP, 1), MedVar::makeConst(0x28, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(13), temp(10)});
  B.op(b0, NdOp::LOAD, temp(14), {temp(13)});
  B.op(b0, NdOp::COPY, temp(15), {temp(14)});
  B.call(b0, "memcpy", temp(11), {temp(12), temp(15), temp(20)}, 0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, InitializedStackMemcpyReadSurvivesPointerSpill) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x40, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0x1234, 8)});
  B.op(b0, NdOp::INT_ADD, temp(13),
       {mkReg(kSP, 1), MedVar::makeConst(0x28, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(13), temp(10)});
  B.op(b0, NdOp::LOAD, temp(14), {temp(13)});
  B.op(b0, NdOp::COPY, temp(15), {temp(14)});
  B.call(b0, "memcpy", temp(11), {temp(12), temp(15), MedVar::makeConst(8, 8)},
         0x9000, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, UninitializedLocalStackAtomicReadIsReported) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::ATOMIC_CMPXCHG, temp(11),
       {temp(10), MedVar::makeConst(0, 8), MedVar::makeConst(1, 8)}, 0x408);
  B.ret(b0, {temp(11)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->CallVA, 0x408u);
}

TEST(AllocLifetime, UninitializedLocalStackAtomicReadSurvivesPointerSpill) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x40, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::INT_ADD, temp(11),
       {mkReg(kSP, 1), MedVar::makeConst(0x28, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(11), temp(10)});
  B.op(b0, NdOp::LOAD, temp(12), {temp(11)});
  B.op(b0, NdOp::ATOMIC_CMPXCHG, temp(13),
       {temp(12), MedVar::makeConst(0, 8), MedVar::makeConst(1, 8)}, 0x408);
  B.ret(b0, {temp(13)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->CallVA, 0x408u);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, InitializedLocalStackAtomicReadIsClean) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0x1234, 8)});
  B.op(b0, NdOp::ATOMIC_ADD, temp(11), {temp(10), MedVar::makeConst(1, 8)},
       0x408);
  B.ret(b0, {temp(11)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, InitializedLocalStackLoadIsClean) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0x1234, 8)});
  B.op(b0, NdOp::LOAD, temp(11), {temp(10)}, 0x408);
  B.ret(b0, {temp(11)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, DeepForwardedUninitializedStackLoadIsReported) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});

  MedVar DeepAddress = temp(10);
  for (int I = 0; I < 120; ++I) {
    const MedVar Next = temp(100 + I);
    B.op(b0, NdOp::COPY, Next, {DeepAddress});
    DeepAddress = Next;
  }
  B.op(b0, NdOp::LOAD, temp(11), {DeepAddress}, 0x408);
  B.ret(b0, {temp(11)});

  const auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                        /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->CallVA, 0x408u);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, DeepPartialFrameWriteInvalidatesInitializationProof) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0x1234, 8)});

  MedVar DeepAddress = temp(10);
  for (int I = 0; I < 120; ++I) {
    const MedVar Next = temp(100 + I);
    B.op(b0, NdOp::COPY, Next, {DeepAddress});
    DeepAddress = Next;
  }
  B.op(b0, NdOp::STORE, MedVar{}, {DeepAddress, MedVar::makeConst(0xAA, 1)});
  B.op(b0, NdOp::LOAD, temp(11), {temp(10)}, 0x408);
  B.ret(b0, {temp(11)});

  const auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                        /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, ConditionalStackInitializationFailsClosed) {
  FB B("f", 0x100);
  int entry = B.block();
  int initialized = B.block();
  int uninitialized = B.block();
  int join = B.block();
  B.op(entry, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(entry, NdOp::INT_ADD, temp(10),
       {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.succ(entry, initialized);
  B.succ(entry, uninitialized);
  B.op(initialized, NdOp::STORE, MedVar{},
       {temp(10), MedVar::makeConst(0x1234, 8)});
  B.succ(initialized, join);
  B.succ(uninitialized, join);
  B.op(join, NdOp::LOAD, temp(11), {temp(10)}, 0x420);
  B.ret(join, {temp(11)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Read->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, ExceptionalEdgeDoesNotObserveLaterStackInitialization) {
  FB B("f", 0x100);
  const int Entry = B.block();
  const int NormalExit = B.block();
  const int Handler = B.block();
  B.op(Entry, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(Entry, NdOp::INT_ADD, temp(10),
       {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.call(Entry, "may_throw", MedVar{}, {}, 0x9000, 0x408);
  B.op(Entry, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0x1234, 8)});
  B.succ(Entry, NormalExit);
  B.ret(NormalExit, {});

  ExceptionalEdge ToHandler;
  ToHandler.BlockId = Handler;
  B.F.Blocks[Entry].ExceptionalSuccs.push_back(ToHandler);
  ExceptionalEdge FromEntry = ToHandler;
  FromEntry.BlockId = Entry;
  B.F.Blocks[Handler].ExceptionalPreds.push_back(FromEntry);
  B.op(Handler, NdOp::LOAD, temp(11), {temp(10)}, 0x420);
  B.ret(Handler, {temp(11)});

  const std::vector<Finding> Findings =
      audit({B.F}, nullptr, /*StackRegs=*/true, /*IncludeStackReads=*/true);
  const Finding *Read = find(Findings, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->CallVA, 0x420u);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, PartialStackInitializationFailsClosed) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(0x1234, 4)});
  B.op(b0, NdOp::LOAD, temp(11), {temp(10)}, 0x408);
  B.ret(b0, {temp(11)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Read->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, EveryBranchInitializingStackSlotIsClean) {
  FB B("f", 0x100);
  int entry = B.block();
  int left = B.block();
  int right = B.block();
  int join = B.block();
  B.op(entry, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(entry, NdOp::INT_ADD, temp(10),
       {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.succ(entry, left);
  B.succ(entry, right);
  B.op(left, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(1, 8)});
  B.op(right, NdOp::STORE, MedVar{}, {temp(10), MedVar::makeConst(2, 8)});
  B.succ(left, join);
  B.succ(right, join);
  B.op(join, NdOp::LOAD, temp(11), {temp(10)}, 0x420);
  B.ret(join, {temp(11)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, CallerStackArgumentIsNotTreatedAsUninitializedLocal) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 0), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::LOAD, temp(11), {temp(10)}, 0x408);
  B.ret(b0, {temp(11)});

  auto Fs = audit({B.F}, nullptr, /*StackRegs=*/true,
                  /*IncludeStackReads=*/true);
  EXPECT_FALSE(has(Fs, VulnClass::UninitializedRead));
}

TEST(AllocLifetime, ReachableUninitializedStackReadIsCorroborated) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.op(b0, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(b0, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.op(b0, NdOp::LOAD, temp(11), {temp(10)}, 0x408);
  B.ret(b0, {temp(11)});

  LowFunc LF;
  LF.Entry = B.F.Entry;
  LF.DecodedInstructionCount = 1;
  LF.LiftedInstructionCount = 1;
  LowBlock LB;
  LB.Id = b0;
  LB.StartAddr = B.F.Entry;
  LB.EndAddr = B.F.Entry + 0x10;
  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = NdVar::tmp(0, 8);
  Load.Addr = 0x408;
  Load.Seq = 0;
  Load.addInput(NdVar::reg(kSP, 8));
  LB.Ops.push_back(Load);
  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.Addr = 0x410;
  LB.Ops.push_back(Ret);
  LF.Blocks.push_back(std::move(LB));

  BinaryImage Img;
  Img.Arch = Arch::X64;
  std::vector<MedFunc> MedFuncs{B.F};
  std::vector<LowFunc> LowFuncs{std::move(LF)};
  AnalysisInput In;
  In.Img = &Img;
  In.LowFuncs = &LowFuncs;
  In.MedFuncs = &MedFuncs;
  In.StackRegsKnown = true;
  In.StackPointerReg = kSP;
  auto Fs = auditMemory(In, SinkCatalog::defaults(), SafetyBudgets{});

  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Read->TheConfidence, Confidence::High);
  EXPECT_FALSE(Read->Corroboration.empty());
}

TEST(AllocLifetime, SolverBudgetExhaustionIsReported) {
  FB B("f", 0x100);
  int Entry = B.block();
  int ReadBlock = B.block();
  int Exit = B.block();
  B.op(Entry, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.op(Entry, NdOp::INT_ADD, temp(10),
       {mkReg(kSP, 1), MedVar::makeConst(8, 8)});
  B.succ(Entry, ReadBlock);
  B.succ(Entry, Exit);
  B.op(ReadBlock, NdOp::LOAD, temp(11), {temp(10)}, 0x118);
  B.ret(ReadBlock, {temp(11)});
  B.ret(Exit, {});

  BinaryImage Img;
  Img.Arch = Arch::X64;
  std::vector<MedFunc> MedFuncs{B.F};
  std::vector<LowFunc> LowFuncs{solverHeavyReturnedPath(B.F.Entry)};
  AnalysisInput In;
  In.Img = &Img;
  In.LowFuncs = &LowFuncs;
  In.MedFuncs = &MedFuncs;
  In.StackRegsKnown = true;
  In.StackPointerReg = kSP;
  SafetyBudgets Budgets;
  Budgets.SolverConflicts = 1;

  auto Fs = auditMemory(In, SinkCatalog::defaults(), Budgets);
  const Finding *Read = find(Fs, VulnClass::UninitializedRead);
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Read->BudgetHit) << Read->Detail;
}

TEST(AllocLifetime, WrapperAllocationLeakIsInterprocedural) {
  // xmalloc(n){ return malloc(n); }  user(){ p = xmalloc(16); /* leak */ }
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(A));
    BinaryImage Img;
    Img.Arch = A;
    const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
    FB Wrap("xmalloc", 0x200);
    Wrap.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
    int w0 = Wrap.block();
    const MedVar Result = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
    Wrap.call(w0, "malloc", Result, {MedVar::makeConst(16, TRI.PointerSize)});
    Wrap.ret(w0, A == Arch::AArch64 ? std::vector<MedVar>{mkReg(
                                          TRI.LinkRegister, 1, TRI.PointerSize)}
                                    : std::vector<MedVar>{Result});

    FB User("user", 0x100);
    int u0 = User.block();
    User.call(u0, "xmalloc", temp(9), {MedVar::makeConst(16, TRI.PointerSize)},
              /*Target=*/0x200);
    User.ret(u0, {});

    auto Fs = audit({Wrap.F, User.F}, &Img);
    const Finding *UserLeak = nullptr;
    const Finding *WrapperLeak = nullptr;
    for (const Finding &F : Fs) {
      if (F.Class != VulnClass::HeapLeak)
        continue;
      if (F.FuncEntry == User.F.Entry)
        UserLeak = &F;
      if (F.FuncEntry == Wrap.F.Entry)
        WrapperLeak = &F;
    }
    ASSERT_NE(UserLeak, nullptr);
    // This helper supplies no LowIR for the caller, so the otherwise definite
    // candidate is expected to fail closed during symbolic corroboration.  Its
    // presence is what proves the owned-heap summary reached the caller.
    EXPECT_EQ(UserLeak->TheVerdict, Verdict::Unknown);
    EXPECT_EQ(UserLeak->Corroboration,
              "no LowIR path was available for corroboration");
    EXPECT_EQ(UserLeak->Detail.find("callee may return heap ownership"),
              std::string::npos)
        << UserLeak->Detail;
    EXPECT_EQ(WrapperLeak, nullptr);
  }
}

TEST(AllocLifetime, UnknownArchitecturePointerReturnPropagatesAsMay) {
  BinaryImage Img;
  Img.Arch = Arch::Unknown;

  FB Wrapper("unknown_arch_factory", 0x200);
  Wrapper.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
  const int WrapperBlock = Wrapper.block();
  Wrapper.call(WrapperBlock, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  Wrapper.ret(WrapperBlock, {});

  FB User("user", 0x100);
  const int UserBlock = User.block();
  User.call(UserBlock, "unknown_arch_factory", temp(9), {}, Wrapper.F.Entry);
  User.ret(UserBlock, {});

  const std::vector<Finding> Findings = audit({Wrapper.F, User.F}, &Img);
  const Finding *WrapperLeak =
      findInFunction(Findings, VulnClass::HeapLeak, Wrapper.F.Entry);
  ASSERT_NE(WrapperLeak, nullptr);
  EXPECT_EQ(WrapperLeak->TheVerdict, Verdict::Unknown);
  EXPECT_NE(WrapperLeak->Detail.find("heap handle may escape"),
            std::string::npos)
      << WrapperLeak->Detail;

  const Finding *CallerLeak =
      findInFunction(Findings, VulnClass::HeapLeak, User.F.Entry);
  ASSERT_NE(CallerLeak, nullptr);
  EXPECT_EQ(CallerLeak->TheVerdict, Verdict::Unknown);
  EXPECT_NE(CallerLeak->Detail.find("callee may return heap ownership"),
            std::string::npos)
      << CallerLeak->Detail;
}

TEST(AllocLifetime, ThumbWrapperAllocationUsesCanonicalCalleeEntry) {
  BinaryImage Img;
  Img.Arch = Arch::ARM;
  Img.Mode = InstructionMode::Thumb;
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  FB Wrap("xmalloc", 0x200);
  Wrap.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
  int WrapperBlock = Wrap.block();
  const MedVar Result = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
  Wrap.call(WrapperBlock, "malloc", Result,
            {MedVar::makeConst(16, TRI.PointerSize)});
  Wrap.ret(WrapperBlock, {mkReg(TRI.LinkRegister, 1, TRI.PointerSize)});

  FB User("user", 0x100);
  int UserBlock = User.block();
  User.call(UserBlock, "xmalloc", temp(9, 4), {MedVar::makeConst(16, 4)},
            0x201);
  User.ret(UserBlock, {});

  auto Fs = audit({User.F, Wrap.F}, &Img);
  bool UserLeak = false;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == User.F.Entry)
      UserLeak = true;
  EXPECT_TRUE(UserLeak);
}

TEST(AllocLifetime, ExternalPlaceholderZeroDoesNotBorrowLocalSummary) {
  FB Local("local_zero", 0);
  Local.F.Params.push_back(temp(0));
  int LocalBlock = Local.block();
  Local.op(LocalBlock, NdOp::STORE, MedVar{},
           {MedVar::makeConst(0x8000, 8), temp(0)});
  Local.ret(LocalBlock, {});

  FB User("user", 0x100);
  int UserBlock = User.block();
  User.call(UserBlock, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(UserBlock, "opaque_external", MedVar{}, {temp(1)}, 0);
  User.ret(UserBlock, {});

  auto Fs = audit({User.F, Local.F});
  const Finding *Leak = nullptr;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == User.F.Entry) {
      Leak = &F;
      break;
    }
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Leak->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, SyntheticExternalZeroDoesNotBorrowLocalSummary) {
  FB Local("local_zero", 0);
  Local.F.Params.push_back(temp(0));
  int LocalBlock = Local.block();
  Local.op(LocalBlock, NdOp::STORE, MedVar{},
           {MedVar::makeConst(0x8000, 8), temp(0)});
  Local.ret(LocalBlock, {});

  FB User("user", 0x100);
  int UserBlock = User.block();
  User.call(UserBlock, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(UserBlock, "sub_0", MedVar{}, {temp(1)}, 0);
  User.ret(UserBlock, {});

  auto Fs = audit({User.F, Local.F});
  const Finding *Leak = nullptr;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == User.F.Entry) {
      Leak = &F;
      break;
    }
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Leak->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, ImportedZeroDoesNotBorrowSameNamedLocalSummary) {
  FB Local("ambiguous", 0);
  Local.F.Params.push_back(temp(0));
  int LocalBlock = Local.block();
  Local.op(LocalBlock, NdOp::STORE, MedVar{},
           {MedVar::makeConst(0x8000, 8), temp(0)});
  Local.ret(LocalBlock, {});

  FB User("user", 0x100);
  int UserBlock = User.block();
  User.call(UserBlock, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(UserBlock, "ambiguous", MedVar{}, {temp(1)}, 0);
  User.ret(UserBlock, {});

  BinaryImage Img;
  Import External;
  External.Name = "ambiguous";
  External.IATAddr = 0;
  Img.Imports.push_back(std::move(External));

  auto Fs = audit({User.F, Local.F}, &Img);
  const Finding *Leak = nullptr;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == User.F.Entry) {
      Leak = &F;
      break;
    }
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Leak->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, ImportedAddressDoesNotBorrowSameAddressLocalSummary) {
  FB Local("ambiguous", 0x200);
  Local.F.Params.push_back(temp(0));
  const int LocalBlock = Local.block();
  Local.op(LocalBlock, NdOp::STORE, MedVar{},
           {MedVar::makeConst(0x8000, 8), temp(0)});
  Local.ret(LocalBlock, {});

  FB User("user", 0x100);
  const int UserBlock = User.block();
  User.call(UserBlock, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(UserBlock, "ambiguous", MedVar{}, {temp(1)}, 0x200);
  User.ret(UserBlock, {});

  BinaryImage Img;
  Img.Imports.push_back({"runtime", "ambiguous", 0, 0x200});

  const std::vector<Finding> Findings = audit({User.F, Local.F}, &Img);
  const Finding *Leak = nullptr;
  for (const Finding &Finding : Findings)
    if (Finding.Class == VulnClass::HeapLeak &&
        Finding.FuncEntry == User.F.Entry) {
      Leak = &Finding;
      break;
    }
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Leak->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, NamedLocalZeroEntryKeepsItsInternalSummary) {
  FB Local("local_zero", 0);
  Local.F.Params.push_back(temp(0));
  int LocalBlock = Local.block();
  Local.op(LocalBlock, NdOp::STORE, MedVar{},
           {MedVar::makeConst(0x8000, 8), temp(0)});
  Local.ret(LocalBlock, {});

  FB User("user", 0x100);
  int UserBlock = User.block();
  User.call(UserBlock, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(UserBlock, "local_zero", MedVar{}, {temp(1)}, 0);
  User.ret(UserBlock, {});

  auto Fs = audit({User.F, Local.F});
  for (const Finding &F : Fs)
    EXPECT_FALSE(F.Class == VulnClass::HeapLeak && F.FuncEntry == User.F.Entry)
        << F.Detail;
}

TEST(AllocLifetime, FreedAllocationIsNotReturnedAsOwnedHeap) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  FB Wrap("released_factory", 0x200);
  Wrap.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
  int w0 = Wrap.block();
  const MedVar Result = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
  Wrap.call(w0, "malloc", Result, {MedVar::makeConst(16, TRI.PointerSize)});
  Wrap.call(w0, "free", MedVar{}, {Result});
  Wrap.ret(w0, {Result});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "released_factory", temp(9), {}, 0x200);
  User.ret(u0, {});

  auto Fs = audit({Wrap.F, User.F}, &Img);
  for (const Finding &F : Fs)
    EXPECT_FALSE(F.Class == VulnClass::HeapLeak && F.FuncEntry == User.F.Entry)
        << F.Detail;
}

TEST(AllocLifetime, PartiallyFreedAllocationMayStillReturnOwnedHeap) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  FB Wrap("conditional_factory", 0x200);
  Wrap.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
  int wEntry = Wrap.block();
  int wFree = Wrap.block();
  int wKeep = Wrap.block();
  int wJoin = Wrap.block();
  const MedVar Result = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
  Wrap.call(wEntry, "malloc", Result, {MedVar::makeConst(16, TRI.PointerSize)});
  Wrap.succ(wEntry, wFree);
  Wrap.succ(wEntry, wKeep);
  Wrap.succ(wFree, wJoin);
  Wrap.succ(wKeep, wJoin);
  Wrap.call(wFree, "free", MedVar{}, {Result});
  Wrap.ret(wJoin, {Result});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "conditional_factory", temp(9), {}, 0x200);
  User.ret(u0, {});

  auto Fs = audit({Wrap.F, User.F}, &Img);
  bool UserLeak = false;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == User.F.Entry)
      UserLeak = true;
  EXPECT_TRUE(UserLeak);
}

TEST(AllocLifetime,
     FreedMixedCarrierBranchDoesNotPublishOwnedHeapForSiblingReturn) {
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(A));
    BinaryImage Img;
    Img.Arch = A;
    const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
    FB Wrap("conditional_factory", 0x200);
    Wrap.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
    const int Entry = Wrap.block();
    const int Freed = Wrap.block();
    const int Other = Wrap.block();
    const int Join = Wrap.block();
    Wrap.succ(Entry, Freed);
    Wrap.succ(Entry, Other);
    Wrap.succ(Freed, Join);
    Wrap.succ(Other, Join);
    const MedVar Result = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
    Wrap.call(Entry, "malloc", Result,
              {MedVar::makeConst(16, TRI.PointerSize)});
    Wrap.call(Freed, "free", MedVar{}, {Result});
    const MedVar FreedResult = mkReg(TRI.IntReturnReg, 2, TRI.PointerSize);
    Wrap.op(Freed, NdOp::COPY, FreedResult, {Result});
    const MedVar OtherResult = mkReg(TRI.IntReturnReg, 3, TRI.PointerSize);
    Wrap.op(Other, NdOp::COPY, OtherResult,
            {MedVar::makeConst(0, TRI.PointerSize)});
    Wrap.ret(Join, A == Arch::AArch64
                       ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 1,
                                                   TRI.PointerSize)}
                       : std::vector<MedVar>{OtherResult});

    FB User("user", 0x100);
    const int UserBlock = User.block();
    User.call(UserBlock, "conditional_factory", temp(9, TRI.PointerSize), {},
              Wrap.F.Entry);
    User.ret(UserBlock, {});

    const auto Findings = audit({Wrap.F, User.F}, &Img);
    for (const Finding &F : Findings)
      EXPECT_FALSE(F.Class == VulnClass::HeapLeak &&
                   F.FuncEntry == User.F.Entry)
          << F.Detail;
  }
}

TEST(AllocLifetime, HeapReturnCorrelatesCarrierAndFreeOnTheSameIncomingEdge) {
  enum class Case { UnfreedAliasEdge, FreedAliasEdge };
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    for (const Case C : {Case::UnfreedAliasEdge, Case::FreedAliasEdge}) {
      SCOPED_TRACE(static_cast<int>(A));
      SCOPED_TRACE(static_cast<int>(C));
      BinaryImage Img;
      Img.Arch = A;
      const TargetRegInfo &TRI = getTargetRegInfo(A);

      FB Factory("edge_factory", 0x200);
      Factory.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
      const int Entry = Factory.block();
      const int AliasEdge = Factory.block();
      const int ZeroEdge = Factory.block();
      const int Join = Factory.block();
      Factory.succ(Entry, AliasEdge);
      Factory.succ(Entry, ZeroEdge);
      Factory.succ(AliasEdge, Join);
      Factory.succ(ZeroEdge, Join);
      const MedVar Allocation = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
      Factory.call(Entry, "malloc", Allocation,
                   {MedVar::makeConst(16, TRI.PointerSize)});
      if (C == Case::FreedAliasEdge)
        Factory.call(AliasEdge, "free", MedVar{}, {Allocation});
      const MedVar AliasResult = mkReg(TRI.IntReturnReg, 2, TRI.PointerSize);
      Factory.op(AliasEdge, NdOp::COPY, AliasResult, {Allocation});

      if (C == Case::UnfreedAliasEdge)
        Factory.call(ZeroEdge, "free", MedVar{}, {Allocation});
      const MedVar ZeroResult = mkReg(TRI.IntReturnReg, 3, TRI.PointerSize);
      Factory.op(ZeroEdge, NdOp::COPY, ZeroResult,
                 {MedVar::makeConst(0, TRI.PointerSize)});
      Factory.ret(Join, A == Arch::AArch64
                            ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 1,
                                                        TRI.PointerSize)}
                            : std::vector<MedVar>{ZeroResult});

      FB User("user", 0x100);
      const int UserBlock = User.block();
      User.call(UserBlock, "edge_factory", temp(9, TRI.PointerSize), {},
                Factory.F.Entry);
      User.ret(UserBlock, {});

      const std::vector<Finding> Findings = audit({Factory.F, User.F}, &Img);
      const Finding *CallerLeak =
          findInFunction(Findings, VulnClass::HeapLeak, User.F.Entry);
      if (C == Case::UnfreedAliasEdge) {
        ASSERT_NE(CallerLeak, nullptr);
        EXPECT_EQ(CallerLeak->Detail.find("callee may return heap ownership"),
                  std::string::npos)
            << CallerLeak->Detail;
      } else {
        EXPECT_EQ(CallerLeak, nullptr);
      }
    }
  }
}

TEST(AllocLifetime, FreedAliasReturnDowngradesHeapSummaryToMay) {
  enum class FreeKind { Guaranteed, Fallible, MayAlias };
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    for (const FreeKind Kind :
         {FreeKind::Guaranteed, FreeKind::Fallible, FreeKind::MayAlias}) {
      SCOPED_TRACE(static_cast<unsigned>(A));
      SCOPED_TRACE(static_cast<unsigned>(Kind));
      BinaryImage Img;
      Img.Arch = A;
      if (Kind == FreeKind::Fallible)
        Img.Format = BinaryFormat::COFF;
      const TargetRegInfo &TRI = getTargetRegInfo(A);

      FB Factory("partially_dangling_factory", 0x200);
      Factory.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
      const int Entry = Factory.block();
      const int Owned = Factory.block();
      const int Freed = Factory.block();
      Factory.succ(Entry, Owned);
      Factory.succ(Entry, Freed);
      const MedVar Allocation = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
      Factory.call(Entry, "malloc", Allocation,
                   {MedVar::makeConst(16, TRI.PointerSize)});

      const MedVar OwnedResult = mkReg(TRI.IntReturnReg, 2, TRI.PointerSize);
      Factory.op(Owned, NdOp::COPY, OwnedResult, {Allocation});
      Factory.ret(Owned, A == Arch::AArch64
                             ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 1,
                                                         TRI.PointerSize)}
                             : std::vector<MedVar>{OwnedResult});

      MedVar FreedArgument = Allocation;
      if (Kind == FreeKind::MayAlias) {
        FreedArgument = temp(20, TRI.PointerSize);
        Factory.op(Freed, NdOp::SELECT, FreedArgument,
                   {temp(21, 1), Allocation, temp(22, TRI.PointerSize)});
      }
      Factory.call(Freed, Kind == FreeKind::Fallible ? "LocalFree" : "free",
                   MedVar{}, {FreedArgument});
      const MedVar FreedResult = mkReg(TRI.IntReturnReg, 3, TRI.PointerSize);
      Factory.op(Freed, NdOp::COPY, FreedResult, {Allocation});
      Factory.ret(Freed, A == Arch::AArch64
                             ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 2,
                                                         TRI.PointerSize)}
                             : std::vector<MedVar>{FreedResult});

      FB Caller("caller", 0x100);
      Caller.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsNoValue;
      const int CallerBlock = Caller.block();
      Caller.call(CallerBlock, Factory.F.Name, temp(30, TRI.PointerSize), {},
                  Factory.F.Entry);
      Caller.ret(CallerBlock, {});

      const std::vector<Finding> Findings = audit({Factory.F, Caller.F}, &Img);
      const Finding *CallerLeak =
          findInFunction(Findings, VulnClass::HeapLeak, Caller.F.Entry);
      ASSERT_NE(CallerLeak, nullptr);
      EXPECT_EQ(CallerLeak->TheVerdict, Verdict::Unknown) << CallerLeak->Detail;
      EXPECT_NE(CallerLeak->Detail.find("callee may return heap ownership"),
                std::string::npos)
          << CallerLeak->Detail;
    }
  }
}

TEST(AllocLifetime, PotentialFreeOnZeroSiblingKeepsOwnedReturnStrong) {
  enum class FreeKind { Fallible, MayAlias };
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    for (const FreeKind Kind : {FreeKind::Fallible, FreeKind::MayAlias}) {
      SCOPED_TRACE(static_cast<unsigned>(A));
      SCOPED_TRACE(static_cast<unsigned>(Kind));
      BinaryImage Img;
      Img.Arch = A;
      if (Kind == FreeKind::Fallible)
        Img.Format = BinaryFormat::COFF;
      const TargetRegInfo &TRI = getTargetRegInfo(A);

      FB Factory("zero_sibling_free_factory", 0x200);
      Factory.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
      const int Entry = Factory.block();
      const int Owned = Factory.block();
      const int Zero = Factory.block();
      Factory.succ(Entry, Owned);
      Factory.succ(Entry, Zero);
      const MedVar Allocation = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
      Factory.call(Entry, "malloc", Allocation,
                   {MedVar::makeConst(16, TRI.PointerSize)});
      const MedVar OwnedResult = mkReg(TRI.IntReturnReg, 2, TRI.PointerSize);
      Factory.op(Owned, NdOp::COPY, OwnedResult, {Allocation});
      Factory.ret(Owned, A == Arch::AArch64
                             ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 1,
                                                         TRI.PointerSize)}
                             : std::vector<MedVar>{OwnedResult});

      MedVar FreeArgument = Allocation;
      if (Kind == FreeKind::MayAlias) {
        FreeArgument = temp(20, TRI.PointerSize);
        Factory.op(Zero, NdOp::SELECT, FreeArgument,
                   {temp(21, 1), Allocation, temp(22, TRI.PointerSize)});
      }
      Factory.call(Zero, Kind == FreeKind::Fallible ? "LocalFree" : "free",
                   MedVar{}, {FreeArgument});
      const MedVar ZeroResult = mkReg(TRI.IntReturnReg, 3, TRI.PointerSize);
      Factory.op(Zero, NdOp::COPY, ZeroResult,
                 {MedVar::makeConst(0, TRI.PointerSize)});
      Factory.ret(Zero, A == Arch::AArch64
                            ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 2,
                                                        TRI.PointerSize)}
                            : std::vector<MedVar>{ZeroResult});

      FB Caller("caller", 0x100);
      Caller.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsNoValue;
      const int CallerBlock = Caller.block();
      Caller.call(CallerBlock, Factory.F.Name, temp(30, TRI.PointerSize), {},
                  Factory.F.Entry);
      Caller.ret(CallerBlock, {});

      const std::vector<Finding> Findings = audit({Factory.F, Caller.F}, &Img);
      const Finding *CallerLeak =
          findInFunction(Findings, VulnClass::HeapLeak, Caller.F.Entry);
      ASSERT_NE(CallerLeak, nullptr);
      EXPECT_EQ(CallerLeak->Detail.find("callee may return heap ownership"),
                std::string::npos)
          << CallerLeak->Detail;
    }
  }
}

TEST(AllocLifetime, NullablePhiKeepsOwnedHeapReturnStrong) {
  enum class ZeroEdgeRelease { None, Guaranteed };
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    for (const ZeroEdgeRelease Release :
         {ZeroEdgeRelease::None, ZeroEdgeRelease::Guaranteed}) {
      SCOPED_TRACE(static_cast<unsigned>(A));
      SCOPED_TRACE(static_cast<unsigned>(Release));
      BinaryImage Img;
      Img.Arch = A;
      const TargetRegInfo &TRI = getTargetRegInfo(A);

      FB Factory("nullable_phi_factory", 0x200);
      Factory.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
      const int Entry = Factory.block();
      const int Owned = Factory.block();
      const int Zero = Factory.block();
      const int Join = Factory.block();
      Factory.succ(Entry, Owned);
      Factory.succ(Entry, Zero);
      Factory.succ(Owned, Join);
      Factory.succ(Zero, Join);

      const MedVar Allocation = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
      Factory.call(Entry, "malloc", Allocation,
                   {MedVar::makeConst(16, TRI.PointerSize)});
      const MedVar OwnedValue = temp(20, TRI.PointerSize);
      Factory.op(Owned, NdOp::COPY, OwnedValue, {Allocation});
      if (Release == ZeroEdgeRelease::Guaranteed)
        Factory.call(Zero, "free", MedVar{}, {Allocation});
      const MedVar ZeroValue = temp(21, TRI.PointerSize);
      Factory.op(Zero, NdOp::COPY, ZeroValue,
                 {MedVar::makeConst(0, TRI.PointerSize)});

      PhiNode Returned;
      Returned.Output = mkReg(TRI.IntReturnReg, 2, TRI.PointerSize);
      Returned.Args = {{Owned, OwnedValue}, {Zero, ZeroValue}};
      Factory.F.Blocks[Join].Phis.push_back(std::move(Returned));
      Factory.ret(Join, A == Arch::AArch64
                            ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 1,
                                                        TRI.PointerSize)}
                            : std::vector<MedVar>{
                                  Factory.F.Blocks[Join].Phis.front().Output});

      FB Caller("caller", 0x100);
      Caller.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsNoValue;
      const int CallerBlock = Caller.block();
      Caller.call(CallerBlock, Factory.F.Name, temp(30, TRI.PointerSize), {},
                  Factory.F.Entry);
      Caller.ret(CallerBlock, {});

      const std::vector<Finding> Findings = audit({Factory.F, Caller.F}, &Img);
      const Finding *CallerLeak =
          findInFunction(Findings, VulnClass::HeapLeak, Caller.F.Entry);
      ASSERT_NE(CallerLeak, nullptr);
      EXPECT_EQ(CallerLeak->Detail.find("callee may return heap ownership"),
                std::string::npos)
          << CallerLeak->Detail;
    }
  }
}

TEST(AllocLifetime, DuplicateSSAHeapReturnFailsClosedToMay) {
  enum class DuplicateDefinition { Operation, Phi };
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    for (const DuplicateDefinition Duplicate :
         {DuplicateDefinition::Operation, DuplicateDefinition::Phi}) {
      SCOPED_TRACE(static_cast<unsigned>(A));
      SCOPED_TRACE(static_cast<unsigned>(Duplicate));
      BinaryImage Img;
      Img.Arch = A;
      const TargetRegInfo &TRI = getTargetRegInfo(A);

      FB Factory("duplicate_ssa_factory", 0x200);
      Factory.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
      const int Entry = Factory.block();
      const MedVar Allocation = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
      const MedVar Returned = mkReg(TRI.IntReturnReg, 2, TRI.PointerSize);
      Factory.call(Entry, "malloc", Allocation,
                   {MedVar::makeConst(16, TRI.PointerSize)});
      Factory.op(Entry, NdOp::COPY, Returned, {Allocation});

      int ReturnBlock = Entry;
      if (Duplicate == DuplicateDefinition::Operation) {
        Factory.op(Entry, NdOp::COPY, Returned,
                   {MedVar::makeConst(0, TRI.PointerSize)});
      } else {
        const int Left = Factory.block();
        const int Right = Factory.block();
        const int Join = Factory.block();
        Factory.succ(Entry, Left);
        Factory.succ(Entry, Right);
        Factory.succ(Left, Join);
        Factory.succ(Right, Join);
        const MedVar LeftZero = temp(20, TRI.PointerSize);
        const MedVar RightZero = temp(21, TRI.PointerSize);
        Factory.op(Left, NdOp::COPY, LeftZero,
                   {MedVar::makeConst(0, TRI.PointerSize)});
        Factory.op(Right, NdOp::COPY, RightZero,
                   {MedVar::makeConst(0, TRI.PointerSize)});
        PhiNode DuplicatePhi;
        DuplicatePhi.Output = Returned;
        DuplicatePhi.Args = {{Left, LeftZero}, {Right, RightZero}};
        Factory.F.Blocks[Join].Phis.push_back(std::move(DuplicatePhi));
        ReturnBlock = Join;
      }
      Factory.ret(ReturnBlock, A == Arch::AArch64
                                   ? std::vector<MedVar>{mkReg(
                                         TRI.LinkRegister, 1, TRI.PointerSize)}
                                   : std::vector<MedVar>{Returned});

      FB Caller("caller", 0x100);
      Caller.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsNoValue;
      const int CallerBlock = Caller.block();
      Caller.call(CallerBlock, Factory.F.Name, temp(30, TRI.PointerSize), {},
                  Factory.F.Entry);
      Caller.ret(CallerBlock, {});

      const std::vector<Finding> Findings = audit({Factory.F, Caller.F}, &Img);
      const Finding *CallerLeak =
          findInFunction(Findings, VulnClass::HeapLeak, Caller.F.Entry);
      ASSERT_NE(CallerLeak, nullptr);
      EXPECT_EQ(CallerLeak->TheVerdict, Verdict::Unknown) << CallerLeak->Detail;
      EXPECT_NE(CallerLeak->Detail.find("callee may return heap ownership"),
                std::string::npos)
          << CallerLeak->Detail;
    }
  }
}

TEST(AllocLifetime, IncompletePhiCannotProveHeapReturnMustAlias) {
  enum class MalformedPhi {
    MissingArgument,
    DuplicateArgumentPredecessor,
    UnknownArgumentPredecessor,
    DuplicateBlockPredecessor,
    UnknownBlockPredecessor,
    ExistingNonPredecessor,
    DuplicatePredecessorSuccessor,
  };
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    for (const MalformedPhi Kind :
         {MalformedPhi::MissingArgument,
          MalformedPhi::DuplicateArgumentPredecessor,
          MalformedPhi::UnknownArgumentPredecessor,
          MalformedPhi::DuplicateBlockPredecessor,
          MalformedPhi::UnknownBlockPredecessor,
          MalformedPhi::ExistingNonPredecessor,
          MalformedPhi::DuplicatePredecessorSuccessor}) {
      SCOPED_TRACE(static_cast<unsigned>(A));
      SCOPED_TRACE(static_cast<unsigned>(Kind));
      BinaryImage Img;
      Img.Arch = A;
      const TargetRegInfo &TRI = getTargetRegInfo(A);

      FB Factory("malformed_phi_factory", 0x200);
      Factory.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
      const int Entry = Factory.block();
      const int Left = Factory.block();
      const int Right = Factory.block();
      const int Extra = Factory.block();
      const int Join = Factory.block();
      Factory.succ(Entry, Left);
      Factory.succ(Entry, Right);
      Factory.succ(Left, Join);
      Factory.succ(Right, Join);
      const MedVar Allocation = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
      Factory.call(Entry, "malloc", Allocation,
                   {MedVar::makeConst(16, TRI.PointerSize)});

      PhiNode Returned;
      Returned.Output = mkReg(TRI.IntReturnReg, 2, TRI.PointerSize);
      Returned.Args = {{Left, Allocation}, {Right, Allocation}};
      switch (Kind) {
      case MalformedPhi::MissingArgument:
        Returned.Args.pop_back();
        break;
      case MalformedPhi::DuplicateArgumentPredecessor:
        Returned.Args[1].first = Left;
        break;
      case MalformedPhi::UnknownArgumentPredecessor:
        Returned.Args[1].first = 999;
        break;
      case MalformedPhi::DuplicateBlockPredecessor:
        Factory.F.Blocks[Join].Preds.push_back(Left);
        break;
      case MalformedPhi::UnknownBlockPredecessor:
        Factory.F.Blocks[Join].Preds[1] = 999;
        Returned.Args[1].first = 999;
        break;
      case MalformedPhi::ExistingNonPredecessor:
        Factory.F.Blocks[Join].Preds[1] = Extra;
        Returned.Args[1].first = Extra;
        break;
      case MalformedPhi::DuplicatePredecessorSuccessor:
        Factory.F.Blocks[Left].Succs.push_back(Join);
        break;
      }
      Factory.F.Blocks[Join].Phis.push_back(std::move(Returned));
      Factory.ret(Join, A == Arch::AArch64
                            ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 1,
                                                        TRI.PointerSize)}
                            : std::vector<MedVar>{
                                  Factory.F.Blocks[Join].Phis.front().Output});

      FB Caller("caller", 0x100);
      Caller.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsNoValue;
      const int CallerBlock = Caller.block();
      Caller.call(CallerBlock, Factory.F.Name, temp(30, TRI.PointerSize), {},
                  Factory.F.Entry);
      Caller.ret(CallerBlock, {});

      const std::vector<Finding> Findings = audit({Factory.F, Caller.F}, &Img);
      const Finding *CallerLeak =
          findInFunction(Findings, VulnClass::HeapLeak, Caller.F.Entry);
      ASSERT_NE(CallerLeak, nullptr);
      EXPECT_EQ(CallerLeak->TheVerdict, Verdict::Unknown) << CallerLeak->Detail;
      EXPECT_NE(CallerLeak->Detail.find("callee may return heap ownership"),
                std::string::npos)
          << CallerLeak->Detail;
    }
  }
}

TEST(AllocLifetime, IncompletePhiCannotProveZeroSiblingReturn) {
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<unsigned>(A));
    BinaryImage Img;
    Img.Arch = A;
    const TargetRegInfo &TRI = getTargetRegInfo(A);

    FB Factory("malformed_zero_factory", 0x200);
    Factory.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
    const int Entry = Factory.block();
    const int Owned = Factory.block();
    const int ZeroLeft = Factory.block();
    const int ZeroRight = Factory.block();
    const int ZeroJoin = Factory.block();
    Factory.succ(Entry, Owned);
    Factory.succ(Entry, ZeroLeft);
    Factory.succ(Entry, ZeroRight);
    Factory.succ(ZeroLeft, ZeroJoin);
    Factory.succ(ZeroRight, ZeroJoin);
    const MedVar Allocation = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
    Factory.call(Entry, "malloc", Allocation,
                 {MedVar::makeConst(16, TRI.PointerSize)});
    const MedVar OwnedResult = mkReg(TRI.IntReturnReg, 2, TRI.PointerSize);
    Factory.op(Owned, NdOp::COPY, OwnedResult, {Allocation});
    Factory.ret(Owned, A == Arch::AArch64
                           ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 1,
                                                       TRI.PointerSize)}
                           : std::vector<MedVar>{OwnedResult});

    PhiNode ZeroReturned;
    ZeroReturned.Output = mkReg(TRI.IntReturnReg, 3, TRI.PointerSize);
    ZeroReturned.Args = {{ZeroLeft, MedVar::makeConst(0, TRI.PointerSize)}};
    Factory.F.Blocks[ZeroJoin].Phis.push_back(std::move(ZeroReturned));
    Factory.ret(
        ZeroJoin,
        A == Arch::AArch64
            ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 2, TRI.PointerSize)}
            : std::vector<MedVar>{
                  Factory.F.Blocks[ZeroJoin].Phis.front().Output});

    FB Caller("caller", 0x100);
    Caller.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsNoValue;
    const int CallerBlock = Caller.block();
    Caller.call(CallerBlock, Factory.F.Name, temp(30, TRI.PointerSize), {},
                Factory.F.Entry);
    Caller.ret(CallerBlock, {});

    const std::vector<Finding> Findings = audit({Factory.F, Caller.F}, &Img);
    const Finding *CallerLeak =
        findInFunction(Findings, VulnClass::HeapLeak, Caller.F.Entry);
    ASSERT_NE(CallerLeak, nullptr);
    EXPECT_EQ(CallerLeak->TheVerdict, Verdict::Unknown) << CallerLeak->Detail;
    EXPECT_NE(CallerLeak->Detail.find("callee may return heap ownership"),
              std::string::npos)
        << CallerLeak->Detail;
  }
}

TEST(AllocLifetime, IncompleteSharedReturnPredecessorsCannotProveHeapSummary) {
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<unsigned>(A));
    BinaryImage Img;
    Img.Arch = A;
    const TargetRegInfo &TRI = getTargetRegInfo(A);

    FB Factory("incomplete_return_predecessors", 0x200);
    Factory.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
    const int Entry = Factory.block();
    const int Owned = Factory.block();
    const int Borrowed = Factory.block();
    const int Join = Factory.block();
    Factory.succ(Entry, Owned);
    Factory.succ(Entry, Borrowed);
    Factory.succ(Owned, Join);
    Factory.succ(Borrowed, Join);
    const MedVar Allocation = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
    Factory.call(Entry, "malloc", Allocation,
                 {MedVar::makeConst(16, TRI.PointerSize)});
    const MedVar OwnedResult = mkReg(TRI.IntReturnReg, 2, TRI.PointerSize);
    Factory.op(Owned, NdOp::COPY, OwnedResult, {Allocation});
    const MedVar BorrowedResult = mkReg(TRI.IntReturnReg, 3, TRI.PointerSize);
    Factory.op(Borrowed, NdOp::COPY, BorrowedResult,
               {MedVar::makeConst(0x8000, TRI.PointerSize)});
    ASSERT_EQ(Factory.F.Blocks[Join].Preds.size(), 2u);
    Factory.F.Blocks[Join].Preds.pop_back();
    Factory.ret(Join, A == Arch::AArch64
                          ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 1,
                                                      TRI.PointerSize)}
                          : std::vector<MedVar>{OwnedResult});

    FB Caller("caller", 0x100);
    Caller.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsNoValue;
    const int CallerBlock = Caller.block();
    Caller.call(CallerBlock, Factory.F.Name, temp(30, TRI.PointerSize), {},
                Factory.F.Entry);
    Caller.ret(CallerBlock, {});

    const std::vector<Finding> Findings = audit({Factory.F, Caller.F}, &Img);
    const Finding *CallerLeak =
        findInFunction(Findings, VulnClass::HeapLeak, Caller.F.Entry);
    ASSERT_NE(CallerLeak, nullptr);
    EXPECT_EQ(CallerLeak->TheVerdict, Verdict::Unknown) << CallerLeak->Detail;
    EXPECT_NE(CallerLeak->Detail.find("callee may return heap ownership"),
              std::string::npos)
        << CallerLeak->Detail;
  }
}

TEST(AllocLifetime, MultipleHeapReturnCandidatesKeepUncertaintyAbsorbing) {
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<unsigned>(A));
    BinaryImage Img;
    Img.Arch = A;
    Img.Format = BinaryFormat::COFF;
    const TargetRegInfo &TRI = getTargetRegInfo(A);

    FB Factory("ambiguous_multi_allocator", 0x200);
    Factory.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
    const int Block = Factory.block();
    const MedVar SharedCarrier = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
    Factory.call(Block, "malloc", SharedCarrier,
                 {MedVar::makeConst(16, TRI.PointerSize)});
    Factory.call(Block, "LocalFree", MedVar{}, {SharedCarrier});
    // Reused SSA identity is malformed input, but the safety summary must
    // still fail closed: the first allocation candidate is May while this
    // later allocation candidate is Yes.
    Factory.call(Block, "malloc", SharedCarrier,
                 {MedVar::makeConst(32, TRI.PointerSize)});
    Factory.ret(Block, A == Arch::AArch64
                           ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 1,
                                                       TRI.PointerSize)}
                           : std::vector<MedVar>{SharedCarrier});

    FB Caller("caller", 0x100);
    Caller.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsNoValue;
    const int CallerBlock = Caller.block();
    Caller.call(CallerBlock, Factory.F.Name, temp(30, TRI.PointerSize), {},
                Factory.F.Entry);
    Caller.ret(CallerBlock, {});

    const std::vector<Finding> Findings = audit({Factory.F, Caller.F}, &Img);
    const Finding *CallerLeak =
        findInFunction(Findings, VulnClass::HeapLeak, Caller.F.Entry);
    ASSERT_NE(CallerLeak, nullptr);
    EXPECT_EQ(CallerLeak->TheVerdict, Verdict::Unknown) << CallerLeak->Detail;
    EXPECT_NE(CallerLeak->Detail.find("callee may return heap ownership"),
              std::string::npos)
        << CallerLeak->Detail;
  }
}

TEST(AllocLifetime, BorrowedNonNullSiblingReturnDowngradesHeapSummaryToMay) {
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(A));
    BinaryImage Img;
    Img.Arch = A;
    const TargetRegInfo &TRI = getTargetRegInfo(A);

    FB Factory("mixed_owner_factory", 0x200);
    Factory.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
    const int Entry = Factory.block();
    const int Owned = Factory.block();
    const int Borrowed = Factory.block();
    Factory.succ(Entry, Owned);
    Factory.succ(Entry, Borrowed);
    const MedVar Allocation = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
    Factory.call(Entry, "malloc", Allocation,
                 {MedVar::makeConst(16, TRI.PointerSize)});
    const MedVar OwnedResult = mkReg(TRI.IntReturnReg, 2, TRI.PointerSize);
    Factory.op(Owned, NdOp::COPY, OwnedResult, {Allocation});
    Factory.ret(Owned, A == Arch::AArch64
                           ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 1,
                                                       TRI.PointerSize)}
                           : std::vector<MedVar>{OwnedResult});
    const MedVar BorrowedResult = mkReg(TRI.IntReturnReg, 3, TRI.PointerSize);
    Factory.op(Borrowed, NdOp::COPY, BorrowedResult,
               {MedVar::makeConst(0x8000, TRI.PointerSize)});
    Factory.ret(Borrowed, A == Arch::AArch64
                              ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 2,
                                                          TRI.PointerSize)}
                              : std::vector<MedVar>{BorrowedResult});

    FB User("user", 0x100);
    const int UserBlock = User.block();
    User.call(UserBlock, "mixed_owner_factory", temp(9, TRI.PointerSize), {},
              Factory.F.Entry);
    User.ret(UserBlock, {});

    const std::vector<Finding> Findings = audit({Factory.F, User.F}, &Img);
    const Finding *CallerLeak =
        findInFunction(Findings, VulnClass::HeapLeak, User.F.Entry);
    ASSERT_NE(CallerLeak, nullptr);
    EXPECT_EQ(CallerLeak->TheVerdict, Verdict::Unknown);
    EXPECT_NE(CallerLeak->Detail.find("callee may return heap ownership"),
              std::string::npos)
        << CallerLeak->Detail;
  }
}

TEST(AllocLifetime, NestedWrapperAllocationLeak) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  FB Inner("xmalloc", 0x200);
  Inner.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
  int i0 = Inner.block();
  const MedVar InnerResult = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
  Inner.call(i0, "malloc", InnerResult,
             {MedVar::makeConst(16, TRI.PointerSize)});
  Inner.ret(i0, {InnerResult});

  FB Outer("ymalloc", 0x300);
  Outer.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
  int o0 = Outer.block();
  const MedVar OuterResult = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
  Outer.call(o0, "xmalloc", OuterResult,
             {MedVar::makeConst(16, TRI.PointerSize)}, 0x200);
  Outer.ret(o0, {OuterResult});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "ymalloc", temp(9), {MedVar::makeConst(16, 8)}, 0x300);
  User.ret(u0, {});

  auto Fs = audit({User.F, Outer.F, Inner.F}, &Img);
  bool UserLeak = false;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == 0x100)
      UserLeak = true;
  EXPECT_TRUE(UserLeak);
}

TEST(AllocLifetime, DeepWrapperAllocationLeakReachesTopLevelCaller) {
  constexpr int kWrapperDepth = 40;
  constexpr va_t kUserEntry = 0x100;
  constexpr va_t kFirstWrapperEntry = 0x200;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);

  std::vector<MedFunc> Funcs;
  FB User("user", kUserEntry);
  int UserBlock = User.block();
  User.call(UserBlock, "wrapper_0", temp(9), {MedVar::makeConst(16, 8)},
            kFirstWrapperEntry);
  User.ret(UserBlock, {});
  Funcs.push_back(std::move(User.F));

  for (int I = 0; I < kWrapperDepth; ++I) {
    const va_t Entry = kFirstWrapperEntry + static_cast<va_t>(I) * 0x100;
    FB Wrapper("wrapper_" + std::to_string(I), Entry);
    Wrapper.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
    int Block = Wrapper.block();
    const MedVar Result = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
    if (I + 1 == kWrapperDepth) {
      Wrapper.call(Block, "malloc", Result,
                   {MedVar::makeConst(16, TRI.PointerSize)});
    } else {
      const va_t CalleeEntry = Entry + 0x100;
      Wrapper.call(Block, "wrapper_" + std::to_string(I + 1), Result,
                   {MedVar::makeConst(16, TRI.PointerSize)}, CalleeEntry);
    }
    Wrapper.ret(Block, {Result});
    Funcs.push_back(std::move(Wrapper.F));
  }

  auto Fs = audit(std::move(Funcs), &Img);
  bool UserLeak = false;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::HeapLeak && F.FuncEntry == kUserEntry)
      UserLeak = true;
  EXPECT_TRUE(UserLeak);
}

TEST(AllocLifetime, DeepFreeWrapperUseAfterFreeReachesTopLevelCaller) {
  constexpr int kWrapperDepth = 40;
  constexpr va_t kUserEntry = 0x100;
  constexpr va_t kFirstWrapperEntry = 0x200;

  std::vector<MedFunc> Funcs;
  FB User("user", kUserEntry);
  int UserBlock = User.block();
  User.call(UserBlock, "malloc", temp(9), {MedVar::makeConst(16, 8)});
  User.call(UserBlock, "release_0", MedVar{}, {temp(9)}, kFirstWrapperEntry);
  User.op(UserBlock, NdOp::LOAD, temp(10), {temp(9)}, 0x480);
  User.ret(UserBlock, {});
  Funcs.push_back(std::move(User.F));

  for (int I = 0; I < kWrapperDepth; ++I) {
    const va_t Entry = kFirstWrapperEntry + static_cast<va_t>(I) * 0x100;
    FB Wrapper("release_" + std::to_string(I), Entry);
    Wrapper.F.Params.push_back(temp(0));
    int Block = Wrapper.block();
    if (I + 1 == kWrapperDepth) {
      Wrapper.call(Block, "free", MedVar{}, {temp(0)});
    } else {
      Wrapper.call(Block, "release_" + std::to_string(I + 1), MedVar{},
                   {temp(0)}, Entry + 0x100);
    }
    Wrapper.ret(Block, {});
    Funcs.push_back(std::move(Wrapper.F));
  }

  auto Fs = audit(std::move(Funcs));
  bool UserUseAfterFree = false;
  for (const Finding &F : Fs)
    if (F.Class == VulnClass::UseAfterFree && F.FuncEntry == kUserEntry)
      UserUseAfterFree = true;
  EXPECT_TRUE(UserUseAfterFree);
}

TEST(AllocLifetime, DeepEscapeWrapperSuppressesTopLevelLeak) {
  constexpr int kWrapperDepth = 40;
  constexpr va_t kUserEntry = 0x100;
  constexpr va_t kFirstWrapperEntry = 0x200;

  std::vector<MedFunc> Funcs;
  FB User("user", kUserEntry);
  int UserBlock = User.block();
  User.call(UserBlock, "malloc", temp(9), {MedVar::makeConst(16, 8)});
  User.call(UserBlock, "escape_0", MedVar{}, {temp(9)}, kFirstWrapperEntry);
  User.ret(UserBlock, {});
  Funcs.push_back(std::move(User.F));

  for (int I = 0; I < kWrapperDepth; ++I) {
    const va_t Entry = kFirstWrapperEntry + static_cast<va_t>(I) * 0x100;
    FB Wrapper("escape_" + std::to_string(I), Entry);
    Wrapper.F.Params.push_back(temp(0));
    int Block = Wrapper.block();
    if (I + 1 == kWrapperDepth) {
      Wrapper.op(Block, NdOp::STORE, MedVar{},
                 {MedVar::makeConst(0x8000, 8), temp(0)});
    } else {
      Wrapper.call(Block, "escape_" + std::to_string(I + 1), MedVar{},
                   {temp(0)}, Entry + 0x100);
    }
    Wrapper.ret(Block, {});
    Funcs.push_back(std::move(Wrapper.F));
  }

  auto Fs = audit(std::move(Funcs));
  for (const Finding &F : Fs)
    EXPECT_FALSE(F.Class == VulnClass::HeapLeak && F.FuncEntry == kUserEntry)
        << F.Detail;
}

TEST(AllocLifetime, OutParameterAllocatorStatusIsNotAHeapReturn) {
  FB Wrap("aligned_status", 0x200);
  int w0 = Wrap.block();
  Wrap.call(w0, "posix_memalign", temp(1),
            {temp(0), MedVar::makeConst(16, 8), MedVar::makeConst(64, 8)});
  Wrap.ret(w0, {temp(1)});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "aligned_status", temp(9), {}, 0x200);
  User.ret(u0, {});

  auto Fs = audit({Wrap.F, User.F});
  ASSERT_EQ(Fs.size(), 1u);
  EXPECT_EQ(Fs[0].FuncEntry, 0x200u);
  EXPECT_EQ(Fs[0].Class, VulnClass::HeapLeak);
  EXPECT_EQ(Fs[0].TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Fs[0].Detail, "allocation output handle was not recovered");
}

TEST(AllocLifetime, VoidFunctionDoesNotReturnStaleAllocationRegister) {
  constexpr va_t MallocVA = 0x210;
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(A));
    BinaryImage Img;
    Img.Arch = A;
    const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
    FB Helper("work", 0x200);
    Helper.F.ReturnType = NdType::makeVoid();
    Helper.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsNoValue;
    int h0 = Helper.block();
    const MedVar StaleResult = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
    Helper.call(h0, "malloc", StaleResult,
                {MedVar::makeConst(16, TRI.PointerSize)}, 0x9000, MallocVA);
    Helper.ret(h0, A == Arch::AArch64
                       ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 1,
                                                   TRI.PointerSize)}
                       : std::vector<MedVar>{StaleResult});

    FB User("user", 0x100);
    int u0 = User.block();
    User.call(u0, "work", temp(9, TRI.PointerSize), {}, 0x200);
    User.ret(u0, {});

    auto Fs = auditWithLowIR({Helper.F, User.F},
                             {straightLineCallReturnPath(
                                 Helper.F.Entry, MallocVA, 0x9000, Img.Arch)},
                             &Img);
    const Finding *HelperLeak = nullptr;
    const Finding *UserLeak = nullptr;
    for (const Finding &F : Fs) {
      if (F.Class != VulnClass::HeapLeak)
        continue;
      if (F.FuncEntry == Helper.F.Entry)
        HelperLeak = &F;
      if (F.FuncEntry == User.F.Entry)
        UserLeak = &F;
    }
    ASSERT_NE(HelperLeak, nullptr);
    EXPECT_EQ(HelperLeak->TheVerdict, Verdict::Unsafe);
    EXPECT_EQ(UserLeak, nullptr);
  }
}

TEST(AllocLifetime, DebugReturnTypeRequiresAuthenticatedSignatureCapability) {
  constexpr va_t MallocVA = 0x210;
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(A));
    BinaryImage Img;
    Img.Arch = A;
    const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
    FB Helper("work", 0x200);
    // This is the backend's heuristic type and is deliberately not evidence.
    Helper.F.ReturnType = NdType::makeVoid();
    int h0 = Helper.block();
    const MedVar StaleResult = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
    Helper.call(h0, "malloc", StaleResult,
                {MedVar::makeConst(16, TRI.PointerSize)}, 0x9000, MallocVA);
    Helper.ret(h0, A == Arch::AArch64
                       ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 1,
                                                   TRI.PointerSize)}
                       : std::vector<MedVar>{StaleResult});

    TypedFunctionDebug Unauthenticated(Helper.F.Entry, Helper.F.Name, {},
                                       NdType::makeVoid(), false);
    const auto UnauthenticatedFindings =
        auditWithLowIR({Helper.F},
                       {straightLineCallReturnPath(Helper.F.Entry, MallocVA,
                                                   0x9000, Img.Arch)},
                       &Img, &Unauthenticated);
    const Finding *UntrustedLeak =
        find(UnauthenticatedFindings, VulnClass::HeapLeak);
    ASSERT_NE(UntrustedLeak, nullptr);
    EXPECT_EQ(UntrustedLeak->TheVerdict, Verdict::Unknown);

    TypedFunctionDebug Authenticated(Helper.F.Entry, Helper.F.Name, {},
                                     NdType::makeVoid(), true);
    const auto AuthenticatedFindings =
        auditWithLowIR({Helper.F},
                       {straightLineCallReturnPath(Helper.F.Entry, MallocVA,
                                                   0x9000, Img.Arch)},
                       &Img, &Authenticated);
    const Finding *TrustedLeak =
        find(AuthenticatedFindings, VulnClass::HeapLeak);
    ASSERT_NE(TrustedLeak, nullptr);
    EXPECT_EQ(TrustedLeak->TheVerdict, Verdict::Unsafe);
  }
}

TEST(AllocLifetime, UnauthenticatedResolverCannotPublishPointerContract) {
  class InconsistentDebug final : public NullDebugContext {
  public:
    bool hasAuthenticatedFunctionSignatures() const override { return false; }
    AuthenticatedReturnValueState
    resolveAuthenticatedReturnValueState(va_t) const override {
      return {AuthenticatedReturnKind::Pointer, 8};
    }
  } Debug;

  BinaryImage Img;
  Img.Arch = Arch::X64;
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  FB Wrapper("untrusted_debug_factory", 0x200);
  const int WrapperBlock = Wrapper.block();
  const MedVar Result = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
  Wrapper.call(WrapperBlock, "malloc", Result,
               {MedVar::makeConst(16, TRI.PointerSize)});
  Wrapper.ret(WrapperBlock, {Result});

  FB User("user", 0x100);
  const int UserBlock = User.block();
  User.call(UserBlock, "untrusted_debug_factory", temp(9), {}, Wrapper.F.Entry);
  User.ret(UserBlock, {});

  const std::vector<Finding> Findings =
      audit({Wrapper.F, User.F}, &Img, false, false, &Debug);
  const Finding *CallerLeak =
      findInFunction(Findings, VulnClass::HeapLeak, User.F.Entry);
  ASSERT_NE(CallerLeak, nullptr);
  EXPECT_EQ(CallerLeak->TheVerdict, Verdict::Unknown);
  EXPECT_NE(CallerLeak->Detail.find("callee may return heap ownership"),
            std::string::npos)
      << CallerLeak->Detail;
}

TEST(AllocLifetime,
     AuthenticatedFloatingReturnDoesNotBorrowStaleIntegerCarrier) {
  constexpr va_t MallocVA = 0x210;
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(A));
    BinaryImage Img;
    Img.Arch = A;
    const TargetRegInfo &TRI = getTargetRegInfo(A);
    ASSERT_NE(TRI.fpReturnModelReg(), 0u);

    FB Helper("floating_work", 0x200);
    const int Block = Helper.block();
    const MedVar Allocation = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
    Helper.call(Block, "malloc", Allocation,
                {MedVar::makeConst(16, TRI.PointerSize)}, 0x9000, MallocVA);
    const MedVar FloatingResult = mkReg(TRI.fpReturnModelReg(), 2, 8);
    Helper.op(Block, NdOp::COPY, FloatingResult, {MedVar::makeConst(0, 8)});
    Helper.ret(Block, A == Arch::AArch64
                          ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 1,
                                                      TRI.PointerSize)}
                          : std::vector<MedVar>{FloatingResult});

    FB User("user", 0x100);
    const int UserBlock = User.block();
    User.call(UserBlock, "floating_work", temp(9, 8), {}, Helper.F.Entry);
    User.ret(UserBlock, {});

    TypedFunctionDebug Debug(Helper.F.Entry, Helper.F.Name, {},
                             NdType::makeFloat(8));
    const std::vector<Finding> Findings = auditWithLowIR(
        {Helper.F, User.F},
        {straightLineCallReturnPath(Helper.F.Entry, MallocVA, 0x9000, A)}, &Img,
        &Debug);
    const Finding *HelperLeak =
        findInFunction(Findings, VulnClass::HeapLeak, Helper.F.Entry);
    ASSERT_NE(HelperLeak, nullptr);
    EXPECT_EQ(HelperLeak->TheVerdict, Verdict::Unsafe) << HelperLeak->Detail;
    EXPECT_EQ(findInFunction(Findings, VulnClass::HeapLeak, User.F.Entry),
              nullptr);
  }
}

TEST(AllocLifetime,
     NonPointerOrWidthMismatchedReturnCannotPublishStrongHeapSummary) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);

  auto Aggregate = std::make_shared<NdType>();
  Aggregate->Kind = NdTypeKind::Struct;
  Aggregate->Size = 16;
  TypeRef NarrowPointer = NdType::makePtr();
  NarrowPointer->Size = 4;
  const std::array<TypeRef, 3> ReturnTypes = {NdType::makeInt(TRI.PointerSize),
                                              Aggregate, NarrowPointer};

  for (const TypeRef &ReturnType : ReturnTypes) {
    SCOPED_TRACE(static_cast<int>(ReturnType->Kind));
    SCOPED_TRACE(ReturnType->Size);
    FB Helper("not_pointer_factory", 0x200);
    const int Block = Helper.block();
    const MedVar Allocation = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
    Helper.call(Block, "malloc", Allocation,
                {MedVar::makeConst(16, TRI.PointerSize)});
    Helper.ret(Block, {Allocation});

    FB User("user", 0x100);
    const int UserBlock = User.block();
    User.call(UserBlock, "not_pointer_factory", temp(9, TRI.PointerSize), {},
              Helper.F.Entry);
    User.ret(UserBlock, {});

    TypedFunctionDebug Debug(Helper.F.Entry, Helper.F.Name, {}, ReturnType);
    const std::vector<Finding> Findings =
        audit({Helper.F, User.F}, &Img, false, false, &Debug);
    const Finding *CallerLeak =
        findInFunction(Findings, VulnClass::HeapLeak, User.F.Entry);
    ASSERT_NE(CallerLeak, nullptr);
    EXPECT_EQ(CallerLeak->TheVerdict, Verdict::Unknown);
    EXPECT_NE(CallerLeak->Detail.find("callee may return heap ownership"),
              std::string::npos)
        << CallerLeak->Detail;
  }
}

TEST(AllocLifetime, ConflictingVoidAndGenericValueEvidenceFailsClosed) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  FB Helper("conflicting_contract", 0x200);
  Helper.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsValue;
  const int Block = Helper.block();
  const MedVar Allocation = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
  Helper.call(Block, "malloc", Allocation,
              {MedVar::makeConst(16, TRI.PointerSize)});
  Helper.ret(Block, {Allocation});

  TypedFunctionDebug Debug(Helper.F.Entry, Helper.F.Name, {},
                           NdType::makeVoid());
  const std::vector<Finding> Findings =
      audit({Helper.F}, &Img, false, false, &Debug);
  const Finding *Leak = find(Findings, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
  EXPECT_NE(Leak->Detail.find("heap handle may escape"), std::string::npos)
      << Leak->Detail;
}

TEST(AllocLifetime, UnauthenticatedHeapReturnPropagatesAsMayAcrossCallers) {
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(A));
    BinaryImage Img;
    Img.Arch = A;
    const TargetRegInfo &TRI = getTargetRegInfo(A);

    FB Source("maybe_factory", 0x300);
    const int SourceBlock = Source.block();
    const MedVar SourceResult = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
    Source.call(SourceBlock, "malloc", SourceResult,
                {MedVar::makeConst(16, TRI.PointerSize)});
    Source.ret(SourceBlock, A == Arch::AArch64
                                ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 1,
                                                            TRI.PointerSize)}
                                : std::vector<MedVar>{SourceResult});

    FB Wrapper("wrapper", 0x200);
    Wrapper.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
    const int WrapperBlock = Wrapper.block();
    const MedVar WrapperResult = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
    Wrapper.call(WrapperBlock, "maybe_factory", WrapperResult, {},
                 Source.F.Entry);
    Wrapper.ret(WrapperBlock, A == Arch::AArch64
                                  ? std::vector<MedVar>{mkReg(
                                        TRI.LinkRegister, 1, TRI.PointerSize)}
                                  : std::vector<MedVar>{WrapperResult});

    FB LeakingCaller("leaking_caller", 0x100);
    const int LeakingBlock = LeakingCaller.block();
    LeakingCaller.call(LeakingBlock, "wrapper", temp(9, TRI.PointerSize), {},
                       Wrapper.F.Entry);
    LeakingCaller.ret(LeakingBlock, {});

    const std::vector<Finding> LeakingFindings =
        audit({Source.F, Wrapper.F, LeakingCaller.F}, &Img);
    const Finding *CallerLeak = findInFunction(
        LeakingFindings, VulnClass::HeapLeak, LeakingCaller.F.Entry);
    ASSERT_NE(CallerLeak, nullptr);
    EXPECT_EQ(CallerLeak->TheVerdict, Verdict::Unknown);
    EXPECT_NE(CallerLeak->Detail.find("callee may return heap ownership"),
              std::string::npos)
        << CallerLeak->Detail;

    FB FreeingCaller("freeing_caller", 0x100);
    const int FreeingBlock = FreeingCaller.block();
    const MedVar CallResult = temp(9, TRI.PointerSize);
    FreeingCaller.call(FreeingBlock, "wrapper", CallResult, {},
                       Wrapper.F.Entry);
    FreeingCaller.call(FreeingBlock, "free", MedVar{}, {CallResult});
    FreeingCaller.ret(FreeingBlock, {});

    const std::vector<Finding> FreeingFindings =
        audit({Source.F, Wrapper.F, FreeingCaller.F}, &Img);
    EXPECT_EQ(findInFunction(FreeingFindings, VulnClass::HeapLeak,
                             FreeingCaller.F.Entry),
              nullptr);
    EXPECT_EQ(findInFunction(FreeingFindings, VulnClass::DoubleFree,
                             FreeingCaller.F.Entry),
              nullptr);
    EXPECT_EQ(findInFunction(FreeingFindings, VulnClass::UseAfterFree,
                             FreeingCaller.F.Entry),
              nullptr);

    FB MisusingCaller("misusing_caller", 0x100);
    const int MisusingBlock = MisusingCaller.block();
    const MedVar MisusedResult = temp(9, TRI.PointerSize);
    MisusingCaller.call(MisusingBlock, "wrapper", MisusedResult, {},
                        Wrapper.F.Entry);
    MisusingCaller.call(MisusingBlock, "free", MedVar{}, {MisusedResult});
    MisusingCaller.call(MisusingBlock, "free", MedVar{}, {MisusedResult});
    MisusingCaller.op(MisusingBlock, NdOp::LOAD, temp(10), {MisusedResult});
    MisusingCaller.ret(MisusingBlock, {});

    const std::vector<Finding> MisusingFindings =
        audit({Source.F, Wrapper.F, MisusingCaller.F}, &Img);
    const Finding *Double = findInFunction(
        MisusingFindings, VulnClass::DoubleFree, MisusingCaller.F.Entry);
    const Finding *Use = findInFunction(
        MisusingFindings, VulnClass::UseAfterFree, MisusingCaller.F.Entry);
    ASSERT_NE(Double, nullptr);
    ASSERT_NE(Use, nullptr);
    EXPECT_EQ(Double->TheVerdict, Verdict::Unknown) << Double->Detail;
    EXPECT_EQ(Use->TheVerdict, Verdict::Unknown) << Use->Detail;
  }
}

TEST(AllocLifetime, NonDefaultAddressSpaceStoreIsNotALocalHeapSpill) {
  constexpr va_t MallocVA = 0x210;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  FB B("segmented_publish", 0x100);
  const int Block = B.block();
  const MedVar StackAddress = mkReg(kSP, 1);
  B.op(Block, NdOp::INT_SUB, StackAddress,
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  B.call(Block, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000,
         MallocVA);
  B.op(Block, NdOp::STORE, MedVar{}, {StackAddress, temp(1)});
  B.F.Blocks[Block].Ops.back().MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
  B.ret(Block, {});

  const std::vector<Finding> Findings = auditWithLowIR(
      {B.F},
      {straightLineCallReturnPath(B.F.Entry, MallocVA, 0x9000, Img.Arch)}, &Img,
      nullptr, /*StackRegs=*/true);
  const Finding *Leak = find(Findings, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown) << Leak->Detail;
}

TEST(AllocLifetime, NonDefaultAddressSpaceHeapUseIsPotential) {
  struct Case {
    NdOp Opcode;
    NdMemoryAddressSpace AddressSpace;
  } Cases[] = {
      {NdOp::LOAD, NdMemoryAddressSpace::X86FS},
      {NdOp::STORE, NdMemoryAddressSpace::X86GS},
      {NdOp::ATOMIC_XCHG, NdMemoryAddressSpace::X86FS},
      {NdOp::ATOMIC_ADD, NdMemoryAddressSpace::X86GS},
      {NdOp::ATOMIC_CMPXCHG, NdMemoryAddressSpace::X86GS},
  };

  BinaryImage Img;
  Img.Arch = Arch::X64;
  for (const Case &C : Cases) {
    SCOPED_TRACE(static_cast<unsigned>(C.Opcode));
    FB B("segmented_use", 0x100);
    const int Block = B.block();
    B.call(Block, "malloc", temp(1), {MedVar::makeConst(16, 8)});
    B.call(Block, "free", MedVar{}, {temp(1)});
    switch (C.Opcode) {
    case NdOp::LOAD:
      B.op(Block, C.Opcode, temp(2), {temp(1)});
      break;
    case NdOp::STORE:
    case NdOp::ATOMIC_XCHG:
    case NdOp::ATOMIC_ADD:
      B.op(Block, C.Opcode, C.Opcode == NdOp::STORE ? MedVar{} : temp(2),
           {temp(1), MedVar::makeConst(0, 8)});
      break;
    case NdOp::ATOMIC_CMPXCHG:
      B.op(Block, C.Opcode, temp(2),
           {temp(1), MedVar::makeConst(0, 8), MedVar::makeConst(0, 8)});
      break;
    default:
      FAIL() << "unexpected memory opcode";
    }
    B.F.Blocks[Block].Ops.back().MemoryAddressSpace = C.AddressSpace;
    B.ret(Block, {});

    const std::vector<Finding> Findings = audit({B.F}, &Img);
    const Finding *Use = find(Findings, VulnClass::UseAfterFree);
    ASSERT_NE(Use, nullptr);
    EXPECT_EQ(Use->TheVerdict, Verdict::Unknown) << Use->Detail;
    EXPECT_EQ(Use->TheConfidence, Confidence::Low) << Use->Detail;
  }
}

TEST(AllocLifetime, NonDefaultAddressSpaceDoesNotCrossDefaultStackDomain) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  for (const NdOp Opcode : {NdOp::LOAD, NdOp::STORE, NdOp::ATOMIC_XCHG,
                            NdOp::ATOMIC_ADD, NdOp::ATOMIC_CMPXCHG}) {
    SCOPED_TRACE(static_cast<unsigned>(Opcode));
    FB B("segmented_stack_domain", 0x100);
    const int Block = B.block();
    const MedVar StackAddress = mkReg(kSP, 1);
    B.op(Block, NdOp::INT_SUB, StackAddress,
         {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
    B.call(Block, "malloc", temp(1), {MedVar::makeConst(16, 8)});

    if (Opcode == NdOp::STORE) {
      B.op(Block, Opcode, MedVar{}, {StackAddress, temp(1)});
      B.F.Blocks[Block].Ops.back().MemoryAddressSpace =
          NdMemoryAddressSpace::X86GS;
      B.op(Block, NdOp::LOAD, temp(2), {StackAddress});
    } else {
      B.op(Block, NdOp::STORE, MedVar{}, {StackAddress, temp(1)});
      if (Opcode == NdOp::LOAD) {
        B.op(Block, Opcode, temp(2), {StackAddress});
      } else if (Opcode == NdOp::ATOMIC_CMPXCHG) {
        B.op(Block, Opcode, temp(2),
             {StackAddress, MedVar::makeConst(0, 8), MedVar::makeConst(0, 8)});
      } else {
        B.op(Block, Opcode, temp(2), {StackAddress, MedVar::makeConst(0, 8)});
      }
      B.F.Blocks[Block].Ops.back().MemoryAddressSpace =
          NdMemoryAddressSpace::X86FS;
    }

    B.call(Block, "free", MedVar{}, {temp(2)});
    B.op(Block, NdOp::LOAD, temp(3), {temp(1)});
    B.ret(Block, {});

    EXPECT_FALSE(
        has(audit({B.F}, &Img, /*StackRegs=*/true), VulnClass::UseAfterFree));
  }
}

TEST(AllocLifetime, NonDefaultAddressSpaceAtomicPublicationIsPotential) {
  constexpr va_t MallocVA = 0x210;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  for (const NdOp Opcode : {NdOp::ATOMIC_XCHG, NdOp::ATOMIC_CMPXCHG}) {
    SCOPED_TRACE(static_cast<unsigned>(Opcode));
    FB B("segmented_atomic_publish", 0x100);
    const int Block = B.block();
    B.call(Block, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000,
           MallocVA);
    if (Opcode == NdOp::ATOMIC_XCHG) {
      B.op(Block, Opcode, temp(2), {MedVar::makeConst(0x8000, 8), temp(1)});
    } else {
      B.op(Block, Opcode, temp(2),
           {MedVar::makeConst(0x8000, 8), MedVar::makeConst(0, 8), temp(1)});
    }
    B.F.Blocks[Block].Ops.back().MemoryAddressSpace =
        NdMemoryAddressSpace::X86FS;
    B.ret(Block, {});

    const std::vector<Finding> Findings = auditWithLowIR(
        {B.F},
        {straightLineCallReturnPath(B.F.Entry, MallocVA, 0x9000, Img.Arch)},
        &Img);
    const Finding *Leak = find(Findings, VulnClass::HeapLeak);
    ASSERT_NE(Leak, nullptr);
    EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown) << Leak->Detail;
  }
}

TEST(AllocLifetime, NonDefaultAddressSpacePublicationSummaryIsPotential) {
  BinaryImage Img;
  Img.Arch = Arch::X64;

  FB Publish("segmented_publish", 0x200);
  Publish.F.Params = {temp(0)};
  Publish.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsNoValue;
  const int PublishBlock = Publish.block();
  Publish.op(PublishBlock, NdOp::STORE, MedVar{},
             {MedVar::makeConst(0x8000, 8), Publish.F.Params.front()});
  Publish.F.Blocks[PublishBlock].Ops.back().MemoryAddressSpace =
      NdMemoryAddressSpace::X86GS;
  Publish.ret(PublishBlock, {});

  FB User("user", 0x100);
  const int UserBlock = User.block();
  User.call(UserBlock, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(UserBlock, Publish.F.Name, MedVar{}, {temp(1)}, Publish.F.Entry);
  User.ret(UserBlock, {});

  const std::vector<Finding> Findings = audit({Publish.F, User.F}, &Img);
  const Finding *CallerLeak =
      findInFunction(Findings, VulnClass::HeapLeak, User.F.Entry);
  ASSERT_NE(CallerLeak, nullptr);
  EXPECT_EQ(CallerLeak->TheVerdict, Verdict::Unknown) << CallerLeak->Detail;
}

TEST(AllocLifetime, AtomicOwnershipPublicationDistinguishesCertaintyAndPaths) {
  enum class Case {
    XchgAllExits,
    CmpXchgAllExits,
    XchgSibling,
    XchgUnknownDst
  };
  for (const Case C : {Case::XchgAllExits, Case::CmpXchgAllExits,
                       Case::XchgSibling, Case::XchgUnknownDst}) {
    SCOPED_TRACE(static_cast<int>(C));
    FB Helper("atomic_publish", 0x200);
    Helper.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsNoValue;
    const int Entry = Helper.block();
    const MedVar Allocation = temp(1);
    Helper.call(Entry, "malloc", Allocation, {MedVar::makeConst(16, 8)});

    int Publish = Entry;
    if (C == Case::XchgSibling) {
      Publish = Helper.block();
      const int Leak = Helper.block();
      Helper.succ(Entry, Publish);
      Helper.succ(Entry, Leak);
      Helper.ret(Leak, {});
    }
    const MedVar Address =
        C == Case::XchgUnknownDst ? temp(20) : MedVar::makeConst(0x8000, 8);
    if (C == Case::CmpXchgAllExits) {
      Helper.op(Publish, NdOp::ATOMIC_CMPXCHG, temp(3),
                {Address, MedVar::makeConst(0, 8), Allocation});
    } else {
      Helper.op(Publish, NdOp::ATOMIC_XCHG, temp(3), {Address, Allocation});
    }
    Helper.ret(Publish, {});

    const std::vector<Finding> Findings = audit({Helper.F});
    const Finding *Leak = find(Findings, VulnClass::HeapLeak);
    if (C == Case::XchgAllExits) {
      EXPECT_EQ(Leak, nullptr);
    } else {
      ASSERT_NE(Leak, nullptr);
      EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown) << Leak->Detail;
      EXPECT_NE(Leak->Detail.find("heap handle may escape"), std::string::npos)
          << Leak->Detail;
    }
  }
}

TEST(AllocLifetime, AtomicCmpXchgExpectedValueIsNotPublished) {
  constexpr va_t MallocVA = 0x210;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  FB Helper("cmpxchg_expected", 0x200);
  Helper.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsNoValue;
  const int Block = Helper.block();
  const MedVar Allocation = temp(1);
  Helper.call(Block, "malloc", Allocation, {MedVar::makeConst(16, 8)}, 0x9000,
              MallocVA);
  Helper.op(
      Block, NdOp::ATOMIC_CMPXCHG, temp(3),
      {MedVar::makeConst(0x8000, 8), Allocation, MedVar::makeConst(0, 8)});
  Helper.ret(Block, {});

  const std::vector<Finding> Findings = auditWithLowIR(
      {Helper.F},
      {straightLineCallReturnPath(Helper.F.Entry, MallocVA, 0x9000, Img.Arch)},
      &Img);
  const Finding *Leak = find(Findings, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unsafe) << Leak->Detail;
}

TEST(AllocLifetime, NarrowAtomicXchgValueIsOnlyPotentialPublication) {
  FB Helper("narrow_xchg", 0x200);
  Helper.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsNoValue;
  const int Block = Helper.block();
  const MedVar Allocation = temp(1);
  Helper.call(Block, "malloc", Allocation, {MedVar::makeConst(16, 8)});
  const MedVar Narrow = temp(2, 4);
  Helper.op(Block, NdOp::COPY, Narrow, {Allocation});
  Helper.op(Block, NdOp::ATOMIC_XCHG, temp(3),
            {MedVar::makeConst(0x8000, 8), Narrow});
  Helper.ret(Block, {});

  const std::vector<Finding> Findings = audit({Helper.F});
  const Finding *Leak = find(Findings, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown) << Leak->Detail;
  EXPECT_NE(Leak->Detail.find("heap handle may escape"), std::string::npos)
      << Leak->Detail;
}

TEST(AllocLifetime,
     PublicationRequiresPointerAddressAndCompleteAtomicAccessWidth) {
  enum class Case { StoreNarrowAddress, XchgNarrowAddress, XchgNarrowOutput };
  for (const Case C : {Case::StoreNarrowAddress, Case::XchgNarrowAddress,
                       Case::XchgNarrowOutput}) {
    SCOPED_TRACE(static_cast<int>(C));
    BinaryImage Img;
    Img.Arch = Arch::X64;
    FB Helper("width_checked_publish", 0x200);
    Helper.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsNoValue;
    const int Block = Helper.block();
    const MedVar Allocation = temp(1, 8);
    Helper.call(Block, "malloc", Allocation, {MedVar::makeConst(16, 8)});
    const MedVar Address = MedVar::makeConst(
        0x8000,
        C == Case::XchgNarrowAddress || C == Case::StoreNarrowAddress ? 4 : 8);
    if (C == Case::StoreNarrowAddress) {
      Helper.op(Block, NdOp::STORE, MedVar{}, {Address, Allocation});
    } else {
      Helper.op(Block, NdOp::ATOMIC_XCHG,
                temp(3, C == Case::XchgNarrowOutput ? 4 : 8),
                {Address, Allocation});
    }
    Helper.ret(Block, {});

    const std::vector<Finding> Findings = audit({Helper.F}, &Img);
    const Finding *Leak = find(Findings, VulnClass::HeapLeak);
    ASSERT_NE(Leak, nullptr);
    EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown) << Leak->Detail;
    EXPECT_NE(Leak->Detail.find("heap handle may escape"), std::string::npos)
        << Leak->Detail;
  }
}

TEST(AllocLifetime, AtomicParameterPublicationUsesAllExitSummary) {
  enum class Case { XchgAllExits, CmpXchgAllExits, XchgSibling };
  for (const Case C :
       {Case::XchgAllExits, Case::CmpXchgAllExits, Case::XchgSibling}) {
    SCOPED_TRACE(static_cast<int>(C));
    FB Publish("atomic_publish", 0x200);
    Publish.F.Params = {temp(0)};
    Publish.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsNoValue;
    const int Entry = Publish.block();
    int Transfer = Entry;
    if (C == Case::XchgSibling) {
      Transfer = Publish.block();
      const int Other = Publish.block();
      Publish.succ(Entry, Transfer);
      Publish.succ(Entry, Other);
      Publish.ret(Other, {});
    }
    if (C == Case::CmpXchgAllExits) {
      Publish.op(Transfer, NdOp::ATOMIC_CMPXCHG, temp(3),
                 {MedVar::makeConst(0x8000, 8), MedVar::makeConst(0, 8),
                  Publish.F.Params.front()});
    } else {
      Publish.op(Transfer, NdOp::ATOMIC_XCHG, temp(3),
                 {MedVar::makeConst(0x8000, 8), Publish.F.Params.front()});
    }
    Publish.ret(Transfer, {});

    FB User("user", 0x100);
    const int UserBlock = User.block();
    User.call(UserBlock, "malloc", temp(1), {MedVar::makeConst(16, 8)});
    User.call(UserBlock, "atomic_publish", MedVar{}, {temp(1)},
              Publish.F.Entry);
    User.ret(UserBlock, {});

    const std::vector<Finding> Findings = audit({Publish.F, User.F});
    const Finding *CallerLeak =
        findInFunction(Findings, VulnClass::HeapLeak, User.F.Entry);
    if (C == Case::XchgAllExits) {
      EXPECT_EQ(CallerLeak, nullptr);
    } else {
      ASSERT_NE(CallerLeak, nullptr);
      EXPECT_EQ(CallerLeak->TheVerdict, Verdict::Unknown) << CallerLeak->Detail;
      EXPECT_NE(CallerLeak->Detail.find("heap handle may escape"),
                std::string::npos)
          << CallerLeak->Detail;
    }
  }
}

TEST(AllocLifetime, MissingSuccessorCannotCompleteAllExitPublicationProof) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  FB Helper("malformed_cfg_publish", 0x200);
  Helper.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsNoValue;
  const int Entry = Helper.block();
  const int Publish = Helper.block();
  Helper.succ(Entry, Publish);
  Helper.F.Blocks[Entry].Succs.push_back(999);
  const MedVar Allocation = temp(1, 8);
  Helper.call(Entry, "malloc", Allocation, {MedVar::makeConst(16, 8)});
  Helper.op(Publish, NdOp::STORE, MedVar{},
            {MedVar::makeConst(0x8000, 8), Allocation});
  Helper.ret(Publish, {});

  const std::vector<Finding> Findings = audit({Helper.F}, &Img);
  const Finding *Leak = find(Findings, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown) << Leak->Detail;
  EXPECT_NE(Leak->Detail.find("heap handle may escape"), std::string::npos)
      << Leak->Detail;
}

TEST(AllocLifetime, UnresolvedCFGEdgeDowngradesHeapReturnSummaryToMay) {
  enum class EdgeKind {
    OrdinaryMissingBlock,
    ExternalExceptionalTarget,
    TerminalWithoutReturn,
  };
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    for (const EdgeKind Kind :
         {EdgeKind::OrdinaryMissingBlock, EdgeKind::ExternalExceptionalTarget,
          EdgeKind::TerminalWithoutReturn}) {
      SCOPED_TRACE(static_cast<unsigned>(A));
      SCOPED_TRACE(static_cast<unsigned>(Kind));
      BinaryImage Img;
      Img.Arch = A;
      const TargetRegInfo &TRI = getTargetRegInfo(A);

      FB Factory("malformed_factory", 0x200);
      Factory.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsPointer;
      const int Entry = Factory.block();
      const int Returned = Factory.block();
      Factory.succ(Entry, Returned);
      if (Kind == EdgeKind::OrdinaryMissingBlock) {
        Factory.F.Blocks[Entry].Succs.push_back(999);
      } else if (Kind == EdgeKind::ExternalExceptionalTarget) {
        ExceptionalEdge Edge;
        Edge.BlockId = -1;
        Edge.TargetVA = 0xdead;
        Factory.F.Blocks[Entry].ExceptionalSuccs.push_back(Edge);
      } else {
        const int Unterminated = Factory.block();
        Factory.succ(Entry, Unterminated);
      }
      const MedVar Result = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
      Factory.call(Entry, "malloc", Result,
                   {MedVar::makeConst(16, TRI.PointerSize)});
      Factory.ret(Returned, A == Arch::AArch64
                                ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 1,
                                                            TRI.PointerSize)}
                                : std::vector<MedVar>{Result});

      FB Caller("caller", 0x100);
      Caller.F.ReturnValueEvidence = MedReturnValueEvidence::ReturnsNoValue;
      const int CallerBlock = Caller.block();
      Caller.call(CallerBlock, Factory.F.Name, temp(9, TRI.PointerSize), {},
                  Factory.F.Entry);
      Caller.ret(CallerBlock, {});

      const std::vector<Finding> Findings = audit({Factory.F, Caller.F}, &Img);
      const Finding *CallerLeak =
          findInFunction(Findings, VulnClass::HeapLeak, Caller.F.Entry);
      ASSERT_NE(CallerLeak, nullptr);
      EXPECT_EQ(CallerLeak->TheVerdict, Verdict::Unknown) << CallerLeak->Detail;
      EXPECT_NE(CallerLeak->Detail.find("callee may return heap ownership"),
                std::string::npos)
          << CallerLeak->Detail;
    }
  }
}

TEST(AllocLifetime, OwnershipTransferOnOneBranchDoesNotHideSiblingLeak) {
  enum class TransferKind { Return, Store, CalleeCapture };
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    for (const TransferKind Kind : {TransferKind::Return, TransferKind::Store,
                                    TransferKind::CalleeCapture}) {
      SCOPED_TRACE(static_cast<int>(A));
      SCOPED_TRACE(static_cast<int>(Kind));
      BinaryImage Img;
      Img.Arch = A;
      const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);

      FB Helper("partial_transfer", 0x200);
      Helper.F.ReturnValueEvidence =
          Kind == TransferKind::Return ? MedReturnValueEvidence::ReturnsPointer
                                       : MedReturnValueEvidence::ReturnsNoValue;
      const int Entry = Helper.block();
      const int Transfer = Helper.block();
      const int Leak = Helper.block();
      Helper.succ(Entry, Transfer);
      Helper.succ(Entry, Leak);
      const MedVar Result = mkReg(TRI.IntReturnReg, 1, TRI.PointerSize);
      Helper.call(Entry, "malloc", Result,
                  {MedVar::makeConst(16, TRI.PointerSize)});

      std::vector<MedFunc> Functions;
      switch (Kind) {
      case TransferKind::Return:
        Helper.ret(Transfer, A == Arch::AArch64
                                 ? std::vector<MedVar>{mkReg(
                                       TRI.LinkRegister, 1, TRI.PointerSize)}
                                 : std::vector<MedVar>{Result});
        break;
      case TransferKind::Store:
        Helper.op(Transfer, NdOp::STORE, MedVar{},
                  {MedVar::makeConst(0x8000, TRI.PointerSize), Result});
        Helper.ret(Transfer, {});
        break;
      case TransferKind::CalleeCapture: {
        FB Capture("capture", 0x300);
        Capture.F.Params = {temp(0, TRI.PointerSize)};
        const int CaptureBlock = Capture.block();
        Capture.op(CaptureBlock, NdOp::STORE, MedVar{},
                   {MedVar::makeConst(0x8000, TRI.PointerSize),
                    Capture.F.Params.front()});
        Capture.ret(CaptureBlock, {});
        Helper.call(Transfer, "capture", MedVar{}, {Result}, Capture.F.Entry);
        Helper.ret(Transfer, {});
        Functions.push_back(std::move(Capture.F));
        break;
      }
      }

      if (Kind == TransferKind::Return) {
        const MedVar Other = mkReg(TRI.IntReturnReg, 2, TRI.PointerSize);
        Helper.op(Leak, NdOp::COPY, Other,
                  {MedVar::makeConst(0, TRI.PointerSize)});
        Helper.ret(Leak, A == Arch::AArch64
                             ? std::vector<MedVar>{mkReg(TRI.LinkRegister, 2,
                                                         TRI.PointerSize)}
                             : std::vector<MedVar>{Other});
      } else {
        Helper.ret(Leak, {});
      }
      Functions.insert(Functions.begin(), std::move(Helper.F));

      const auto Findings = audit(std::move(Functions), &Img);
      const Finding *HelperLeak = nullptr;
      for (const Finding &F : Findings)
        if (F.Class == VulnClass::HeapLeak && F.FuncEntry == 0x200)
          HelperLeak = &F;
      ASSERT_NE(HelperLeak, nullptr);
      EXPECT_EQ(HelperLeak->TheVerdict, Verdict::Unknown) << HelperLeak->Detail;
    }
  }
}

TEST(AllocLifetime, LeakOnOneExitPath) {
  FB B("f", 0x100);
  int b0 = B.block();
  int b1 = B.block();
  int b2 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.succ(b0, b1);
  B.succ(b0, b2);
  B.ret(b1, {});
  B.call(b2, "free", MedVar{}, {temp(1)});
  B.ret(b2, {});
  auto Fs = audit({B.F});
  EXPECT_TRUE(has(Fs, VulnClass::HeapLeak));
}

TEST(AllocLifetime, GuardedFreeWithoutLowIRFailsClosed) {
  FB B("f", 0x100);
  int b0 = B.block();
  int b1 = B.block();
  int b2 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(b0, NdOp::INT_NOTEQUAL, temp(3), {temp(1), MedVar::makeConst(0, 8)});
  B.succ(b0, b1);
  B.succ(b0, b2);
  B.call(b1, "free", MedVar{}, {temp(1)});
  B.ret(b1, {});
  B.ret(b2, {});
  auto Fs = audit({B.F});
  const Finding *Leak = find(Fs, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
}

TEST(AllocLifetime, ReallocResultLeak) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "realloc", temp(2), {temp(1), MedVar::makeConst(32, 8)});
  B.ret(b0, {});
  auto Fs = audit({B.F});
  EXPECT_TRUE(has(Fs, VulnClass::HeapLeak));
}

TEST(AllocLifetime, ReallocIsNotAnUnconditionalRelease) {
  FB B("f", 0x100);
  int b0 = B.block();
  int failure = B.block();
  int success = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "realloc", temp(2), {temp(1), MedVar::makeConst(32, 8)});
  B.op(b0, NdOp::INT_EQUAL, temp(3), {temp(2), MedVar::makeConst(0, 8)});
  B.succ(b0, failure);
  B.succ(b0, success);
  B.op(failure, NdOp::LOAD, temp(4), {temp(1)});
  B.call(failure, "free", MedVar{}, {temp(1)});
  B.ret(failure, {});
  B.call(success, "free", MedVar{}, {temp(2)});
  B.ret(success, {});

  auto Fs = audit({B.F});
  EXPECT_FALSE(has(Fs, VulnClass::UseAfterFree));
  EXPECT_FALSE(has(Fs, VulnClass::DoubleFree));
}

TEST(AllocLifetime, ReallocfAlwaysReleasesOriginalAllocation) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(b0, "reallocf", temp(2), {temp(1), MedVar::makeConst(32, 8)});
  B.op(b0, NdOp::LOAD, temp(3), {temp(1)}, 0x408);
  B.call(b0, "free", MedVar{}, {temp(2)});
  B.ret(b0, {});

  auto Fs = audit({B.F});
  EXPECT_TRUE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, FreeWrapperUseAfterFree) {
  FB Wrap("xfree", 0x200);
  Wrap.F.Params.push_back(temp(0));
  int w0 = Wrap.block();
  Wrap.call(w0, "free", MedVar{}, {temp(0)});
  Wrap.ret(w0, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(u0, "xfree", MedVar{}, {temp(1)}, 0x200, 0x400);
  User.op(u0, NdOp::LOAD, temp(2), {temp(1)}, 0x408);
  User.ret(u0, {});

  auto Fs = audit({Wrap.F, User.F});
  EXPECT_TRUE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, FreeWrapperSummaryFollowsForwardedParameter) {
  FB Wrap("xfree", 0x200);
  Wrap.F.Params.push_back(temp(0));
  int w0 = Wrap.block();
  Wrap.op(w0, NdOp::COPY, temp(4), {temp(0)});
  Wrap.call(w0, "free", MedVar{}, {temp(4)});
  Wrap.ret(w0, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  User.call(u0, "xfree", MedVar{}, {temp(1)}, 0x200, 0x400);
  User.op(u0, NdOp::LOAD, temp(2), {temp(1)}, 0x408);
  User.ret(u0, {});

  auto Fs = audit({Wrap.F, User.F});
  EXPECT_TRUE(has(Fs, VulnClass::UseAfterFree));
}

TEST(AllocLifetime, FreeWrapperSummaryPreservesEveryReleasedParameter) {
  FB Wrap("release_pair", 0x200);
  Wrap.F.Params.push_back(temp(0));
  Wrap.F.Params.push_back(temp(1));
  int w0 = Wrap.block();
  Wrap.call(w0, "free", MedVar{}, {temp(0)});
  Wrap.call(w0, "free", MedVar{}, {temp(1)});
  Wrap.ret(w0, {});

  FB User("user", 0x100);
  int u0 = User.block();
  User.call(u0, "malloc", temp(10), {MedVar::makeConst(16, 8)});
  User.call(u0, "malloc", temp(11), {MedVar::makeConst(16, 8)});
  User.call(u0, "release_pair", MedVar{}, {temp(10), temp(11)}, 0x200, 0x400);
  User.op(u0, NdOp::LOAD, temp(12), {temp(10)}, 0x408);
  User.op(u0, NdOp::LOAD, temp(13), {temp(11)}, 0x410);
  User.ret(u0, {});

  auto Fs = audit({Wrap.F, User.F});
  EXPECT_EQ(count(Fs, VulnClass::UseAfterFree), 2u);
}

TEST(AllocLifetime, UnknownCalleeReceivingHandleFailsClosed) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, 0x400);
  B.call(b0, "opaque_runtime", MedVar{}, {temp(1)}, 0x9200, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F});
  const Finding *Leak = find(Fs, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Leak->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, UnknownCallBeforeAllocationOnLoopBackedgeFailsClosed) {
  FB B("f", 0x100);
  int Loop = B.block();
  int Exit = B.block();
  B.call(Loop, "opaque_runtime", MedVar{}, {}, 0x9200, 0x400);
  B.call(Loop, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, 0x408);
  B.succ(Loop, Loop);
  B.succ(Loop, Exit);
  B.ret(Exit, {});

  auto Fs = audit({B.F});
  const Finding *Leak = find(Fs, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Leak->TheConfidence, Confidence::Low);
  EXPECT_EQ(Leak->Detail,
            "heap lifetime depends on a call with unrecovered arguments");
}

TEST(AllocLifetime, MissingFreeArgumentFailsClosed) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, 0x400);
  B.call(b0, "free", MedVar{}, {}, 0x9100, 0x408);
  B.ret(b0, {});

  auto Fs = audit({B.F});
  const Finding *Leak = find(Fs, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Leak->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, MissingAllocationResultFailsClosed) {
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", MedVar{}, {MedVar::makeConst(16, 8)}, 0x9000, 0x400);
  B.ret(b0, {});

  auto Fs = audit({B.F});
  const Finding *Leak = find(Fs, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Leak->TheConfidence, Confidence::Low);
}

TEST(AllocLifetime, NarrowAllocationResultFailsClosed) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1, 1), {MedVar::makeConst(16, 8)}, 0x9000, 0x400);
  B.ret(b0, {});

  const std::vector<Finding> Fs = audit({B.F}, &Img);
  const Finding *Leak = find(Fs, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown) << Leak->Detail;
  EXPECT_EQ(Leak->TheConfidence, Confidence::Low) << Leak->Detail;
  EXPECT_EQ(Leak->Detail,
            "allocation result has incompatible target pointer width");
}

TEST(AllocLifetime, NarrowFreeArgumentFailsClosed) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, 0x400);
  B.call(b0, "free", MedVar{}, {temp(1, 1)}, 0x9100, 0x408);
  B.ret(b0, {});

  const std::vector<Finding> Fs = audit({B.F}, &Img);
  const Finding *Leak = find(Fs, VulnClass::HeapLeak);
  ASSERT_NE(Leak, nullptr);
  EXPECT_EQ(Leak->TheVerdict, Verdict::Unknown) << Leak->Detail;
  EXPECT_EQ(Leak->TheConfidence, Confidence::Low) << Leak->Detail;
  EXPECT_EQ(Leak->Detail,
            "heap lifetime depends on a call with unrecovered arguments");
}

TEST(AllocLifetime, NarrowMemoryAddressDoesNotProveUseAfterFree) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  FB B("f", 0x100);
  int b0 = B.block();
  B.call(b0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, 0x9000, 0x400);
  B.call(b0, "free", MedVar{}, {temp(1)}, 0x9100, 0x408);
  B.op(b0, NdOp::LOAD, temp(2), {temp(1, 1)}, 0x410);
  B.ret(b0, {});

  const std::vector<Finding> Fs = audit({B.F}, &Img);
  const Finding *Use = find(Fs, VulnClass::UseAfterFree);
  ASSERT_NE(Use, nullptr);
  EXPECT_EQ(Use->TheVerdict, Verdict::Unknown) << Use->Detail;
  EXPECT_EQ(Use->TheConfidence, Confidence::Low) << Use->Detail;
  EXPECT_NE(Use->Detail.find("derived address may access"), std::string::npos)
      << Use->Detail;
}
