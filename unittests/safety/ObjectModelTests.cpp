//===- ObjectModelTests.cpp - Destination capacity recovery --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/safety/ObjectModel.h"

#include <algorithm>
#include <limits>

using namespace neverd;
using namespace neverd::safety;

namespace {

// An arbitrary register offset standing in for the architecture stack pointer.
constexpr uint64_t kSP = 0x1000;
constexpr uint64_t kFP = 0x2000;

MedVar mkReg(uint64_t Off, int Ver, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = static_cast<int>(Off);
  V.RegOff = Off;
  V.SSAVer = Ver;
  V.Size = Size;
  return V;
}

MedVar temp(int Id, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = Size;
  return V;
}

MedFunc newFunc(Arch A) {
  MedFunc F;
  F.Entry = 0x100;
  F.Name = "f";
  MedBlock B;
  B.Id = 0;
  F.Blocks.push_back(std::move(B));
  return F;
}

void push(MedFunc &F, NdOp Op, MedVar Out, std::vector<MedVar> Ins) {
  MedOp O;
  O.Opcode = Op;
  O.Output = Out;
  for (auto &I : Ins)
    O.addInput(I);
  F.Blocks[0].Ops.push_back(O);
}

size_t pushCall(MedFunc &F, const std::string &Name, MedVar Ret,
                std::vector<MedVar> Args, va_t Target = 0x9000) {
  int OpIdx = static_cast<int>(F.Blocks[0].Ops.size());
  MedOp O;
  O.Opcode = NdOp::CALL;
  O.Output = Ret;
  O.addInput(MedVar::makeConst(Target, 8));
  F.Blocks[0].Ops.push_back(O);
  MedCallInfo CI;
  CI.BlockId = 0;
  CI.OpIdx = OpIdx;
  CI.TargetAddr = Target;
  CI.TargetName = Name;
  CI.Args = std::move(Args);
  F.CallInfos.push_back(CI);
  return F.CallInfos.size() - 1;
}

} // namespace

TEST(ObjectModel, HeapAllocationExactSize) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc(Arch::X64);
  pushCall(F, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  size_t Sink = pushCall(F, "memcpy", temp(0), {temp(1), temp(2), temp(3)});

  DestObject D = resolveDestination(In, Cat, F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Heap);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 16u);
  EXPECT_TRUE(D.CapacityExact);
}

TEST(ObjectModel, HeapAllocationSurvivesStackSpillReload) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  push(F, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  push(F, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  pushCall(F, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  push(F, NdOp::STORE, MedVar{}, {temp(10), temp(1)});
  push(F, NdOp::LOAD, temp(2), {temp(10)});
  size_t Sink = pushCall(F, "memcpy", temp(0),
                         {temp(2), temp(3), MedVar::makeConst(8, 8)});

  DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 16u);
  EXPECT_TRUE(D.CapacityExact);
}

TEST(ObjectModel, StoreAfterLoadDoesNotSupplyHeapCapacity) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  push(F, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  push(F, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  push(F, NdOp::LOAD, temp(2), {temp(10)});
  pushCall(F, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  push(F, NdOp::STORE, MedVar{}, {temp(10), temp(1)});
  size_t Sink = pushCall(F, "memcpy", temp(0),
                         {temp(2), temp(3), MedVar::makeConst(8, 8)});

  DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, HeapAllocationUsesResolvedImportIdentity) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Imports.push_back({"runtime", "malloc", 0, 0x9100});
  AnalysisInput In;
  In.Img = &Img;

  MedFunc F = newFunc(Arch::X64);
  pushCall(F, "sub_9100", temp(1), {MedVar::makeConst(16, 8)}, 0x9100);
  size_t Sink = pushCall(F, "memcpy", temp(0),
                         {temp(1), temp(2), MedVar::makeConst(8, 8)});

  DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 16u);
  EXPECT_TRUE(D.CapacityExact);
}

TEST(ObjectModel, HeapAllocationSizeFollowsConstantSSA) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;

  MedFunc F = newFunc(Arch::X64);
  push(F, NdOp::INT_ZEXT, temp(10), {MedVar::makeConst(16, 4)});
  pushCall(F, "malloc", temp(1), {temp(10)});
  size_t Sink = pushCall(F, "memcpy", temp(0),
                         {temp(1), temp(2), MedVar::makeConst(8, 8)});

  DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 16u);
  EXPECT_TRUE(D.CapacityExact);
}

TEST(ObjectModel, OutParameterAllocatorStatusIsNotAHeapObject) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;

  MedFunc F = newFunc(Arch::X64);
  pushCall(F, "posix_memalign", temp(1),
           {temp(10), MedVar::makeConst(16, 8), MedVar::makeConst(64, 8)});
  size_t Sink = pushCall(F, "memcpy", temp(0),
                         {temp(1), temp(2), MedVar::makeConst(8, 8)});

  DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, CallocCapacityIsProduct) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc(Arch::X64);
  pushCall(F, "calloc", temp(1),
           {MedVar::makeConst(4, 8), MedVar::makeConst(8, 8)});
  size_t Sink = pushCall(F, "memcpy", temp(0), {temp(1), temp(2), temp(3)});

  DestObject D = resolveDestination(In, Cat, F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Heap);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 32u);
  EXPECT_TRUE(D.CapacityExact);
}

TEST(ObjectModel, OverflowingCallocSizeDoesNotBecomeSmallCapacity) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc(Arch::X64);
  const uint64_t Count = std::numeric_limits<uint64_t>::max() / 2 + 1;
  pushCall(F, "calloc", temp(1),
           {MedVar::makeConst(Count, 8), MedVar::makeConst(2, 8)});
  size_t Sink = pushCall(F, "memcpy", temp(0), {temp(1), temp(2), temp(3)});

  DestObject D = resolveDestination(In, Cat, F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, CallocProductMustFitTheTargetPointerWidth) {
  BinaryImage Img;
  Img.Arch = Arch::X86;
  AnalysisInput In;
  In.Img = &Img;

  MedFunc F = newFunc(Arch::X86);
  pushCall(F, "calloc", temp(1, 4),
           {MedVar::makeConst(0x10000, 4), MedVar::makeConst(0x10000, 4)});
  size_t Sink =
      pushCall(F, "memcpy", temp(0, 4), {temp(1, 4), temp(2, 4), temp(3, 4)});

  DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, StackDestinationUpperBound) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;
  SinkCatalog Cat = SinkCatalog::defaults();

  // sp1 = sp - 0x30; dst = sp1 + 8 (i.e. incoming_sp - 0x28)
  MedFunc F = newFunc(Arch::X64);
  push(F, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  push(F, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});

  DestObject D = resolveDestination(In, Cat, F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Stack);
  EXPECT_EQ(D.StackOffset, -0x28);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 0x28u); // sound upper bound to the incoming SP.
  EXPECT_FALSE(D.CapacityExact);
}

TEST(ObjectModel, OverflowingStackAddressArithmeticIsUnknown) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  push(F, NdOp::INT_ADD, temp(10),
       {mkReg(kSP, 0),
        MedVar::makeConst(
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max()), 8)});
  push(F, NdOp::INT_ADD, temp(11), {temp(10), MedVar::makeConst(1, 8)});
  size_t Sink = pushCall(F, "strcpy", temp(0), {temp(11), temp(2)});

  DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

class TypeDebug : public NullDebugContext {
public:
  TypeDebug(va_t Func, int64_t Off, TypeRef Type)
      : Func(Func), Off(Off), Type(std::move(Type)) {}

  std::optional<VariableSym> resolveVariable(va_t F, int64_t O) const override {
    if (F != Func || O != Off)
      return std::nullopt;
    VariableSym V;
    V.Name = "object";
    V.Type = Type;
    V.StackOffset = Off;
    return V;
  }
  bool hasInfo() const override { return true; }

private:
  va_t Func;
  int64_t Off;
  TypeRef Type;
};

TEST(ObjectModel, OverflowingDebugArraySizeIsNotAnExactCapacity) {
  auto Inner = std::make_shared<NdType>();
  Inner->Kind = NdTypeKind::Array;
  Inner->ArrayCount = std::numeric_limits<uint32_t>::max();
  Inner->ElemType = NdType::makeInt(2, false);
  auto Outer = std::make_shared<NdType>();
  Outer->Kind = NdTypeKind::Array;
  Outer->ArrayCount = std::numeric_limits<uint32_t>::max();
  Outer->ElemType = Inner;

  BinaryImage Img;
  Img.Arch = Arch::X64;
  TypeDebug Dbg(0x100, 8, Outer);
  AnalysisInput In;
  In.Img = &Img;
  In.Dbg = &Dbg;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  push(F, NdOp::INT_ADD, temp(10), {mkReg(kSP, 0), MedVar::makeConst(8, 8)});
  size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});

  DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Stack);
  EXPECT_FALSE(D.Capacity.has_value());
  EXPECT_FALSE(D.CapacityExact);
}

TEST(ObjectModel, CyclicDebugArrayTypeIsNotAnExactCapacity) {
  auto Cycle = std::make_shared<NdType>();
  Cycle->Kind = NdTypeKind::Array;
  Cycle->ArrayCount = 2;
  Cycle->ElemType = Cycle;

  BinaryImage Img;
  Img.Arch = Arch::X64;
  TypeDebug Dbg(0x100, 8, Cycle);
  AnalysisInput In;
  In.Img = &Img;
  In.Dbg = &Dbg;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  push(F, NdOp::INT_ADD, temp(10), {mkReg(kSP, 0), MedVar::makeConst(8, 8)});
  size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});

  DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_FALSE(D.Capacity.has_value());
  EXPECT_FALSE(D.CapacityExact);

  Cycle->ElemType.reset();
}

class ArrayDebug : public NullDebugContext {
public:
  ArrayDebug(va_t Func, int64_t Off, uint32_t Count) : Func(Func), Off(Off) {
    auto Elem = NdType::makeInt(1, false);
    Type = std::make_shared<NdType>();
    Type->Kind = NdTypeKind::Array;
    Type->ArrayCount = Count;
    Type->ElemType = Elem;
    Type->Size = static_cast<uint16_t>(Count);
  }
  std::optional<VariableSym> resolveVariable(va_t F, int64_t O) const override {
    if (F != Func || O != Off)
      return std::nullopt;
    VariableSym V;
    V.Name = "buf";
    V.Type = Type;
    V.StackOffset = Off;
    return V;
  }
  bool hasInfo() const override { return true; }

private:
  va_t Func;
  int64_t Off;
  TypeRef Type;
};

class StackPointerArrayDebug : public NullDebugContext {
public:
  StackPointerArrayDebug(va_t Func, int64_t Off, uint32_t Count)
      : Func(Func), Off(Off) {
    auto Elem = NdType::makeInt(1, false);
    Type = std::make_shared<NdType>();
    Type->Kind = NdTypeKind::Array;
    Type->ArrayCount = Count;
    Type->ElemType = Elem;
    Type->Size = static_cast<uint16_t>(Count);
  }

  std::optional<VariableSym>
  resolveStackPointerVariable(va_t F, int64_t O) const override {
    if (F != Func || O != Off)
      return std::nullopt;
    VariableSym V;
    V.Name = "buf";
    V.Type = Type;
    V.StackOffset = Off;
    return V;
  }
  bool hasInfo() const override { return true; }

private:
  va_t Func;
  int64_t Off;
  TypeRef Type;
};

TEST(ObjectModel, DebugArrayPreferredOverFrameBound) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  ArrayDebug Dbg(0x100, -0x28, 16);
  AnalysisInput In;
  In.Img = &Img;
  In.Dbg = &Dbg;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc(Arch::X64);
  push(F, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  push(F, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});

  DestObject D = resolveDestination(In, Cat, F, Sink, 0);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 16u);
  EXPECT_EQ(D.Detail, "declared array");
  EXPECT_TRUE(D.CapacityExact);
}

TEST(ObjectModel, StackPointerRelativeDebugArrayUsesAdjustedFrameOffset) {
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  StackPointerArrayDebug Dbg(0x100, 0x10, 64);
  AnalysisInput In;
  In.Img = &Img;
  In.Dbg = &Dbg;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::AArch64);
  F.FrameSize = 0x60;
  push(F, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x60, 8)});
  push(F, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x10, 8)});
  size_t Sink = pushCall(F, "strncpy", temp(0),
                         {temp(10), temp(2), MedVar::makeConst(63, 8)});

  DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 64u);
  EXPECT_TRUE(D.CapacityExact);
}

TEST(ObjectModel, FrameBaseRelativeDebugArrayUsesRecoveredFramePointer) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  ArrayDebug Dbg(0x100, -0x40, 64);
  AnalysisInput In;
  In.Img = &Img;
  In.Dbg = &Dbg;
  In.StackPointerReg = kSP;
  In.FramePointerReg = kFP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  F.FrameSize = 0x58;
  push(F, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x8, 8)});
  push(F, NdOp::COPY, mkReg(kFP, 1), {mkReg(kSP, 1)});
  push(F, NdOp::INT_SUB, temp(10), {mkReg(kFP, 1), MedVar::makeConst(0x40, 8)});
  size_t Sink = pushCall(F, "strncpy", temp(0),
                         {temp(10), temp(2), MedVar::makeConst(63, 8)});

  DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 64u);
  EXPECT_TRUE(D.CapacityExact);
}

TEST(ObjectModel, TypedLocalCapacity) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc(Arch::X64);
  auto Elem = NdType::makeInt(1, false);
  MedTypedLocal L;
  L.Name = "buf";
  L.StackOff = -0x28;
  L.Type = std::make_shared<NdType>();
  L.Type->Kind = NdTypeKind::Array;
  L.Type->ArrayCount = 8;
  L.Type->ElemType = Elem;
  L.Type->Size = 8;
  F.TypedLocals.push_back(L);
  push(F, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  push(F, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});

  DestObject D = resolveDestination(In, Cat, F, Sink, 0);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 8u);
  EXPECT_TRUE(D.CapacityExact);
}

TEST(ObjectModel, InferredScalarLocalIsNotAnExactObjectCapacity) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  MedTypedLocal L;
  L.Name = "stack_28";
  L.StackOff = -0x28;
  L.Type = NdType::makeInt(8, false);
  F.TypedLocals.push_back(L);
  push(F, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  push(F, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});

  DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 0x28u);
  EXPECT_FALSE(D.CapacityExact);
}

TEST(ObjectModel, UnknownDestinationHasNoCapacity) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();

  MedFunc F = newFunc(Arch::X64);
  // dst is a bare load from unknown memory.
  push(F, NdOp::LOAD, temp(10), {MedVar::makeConst(0x4000, 8)});
  size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});

  DestObject D = resolveDestination(In, Cat, F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}
