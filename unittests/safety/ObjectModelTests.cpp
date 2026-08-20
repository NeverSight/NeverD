//===- ObjectModelTests.cpp - Destination capacity recovery --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/ObjectModel.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"

#include "gtest/gtest.h"

#include <algorithm>

using namespace neverd;
using namespace neverd::safety;

namespace {

// An arbitrary register offset standing in for the architecture stack pointer.
constexpr uint64_t kSP = 0x1000;

MedVar mkReg(uint64_t Off, int Ver, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Reg;
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
                std::vector<MedVar> Args) {
  int OpIdx = static_cast<int>(F.Blocks[0].Ops.size());
  MedOp O;
  O.Opcode = NdOp::CALL;
  O.Output = Ret;
  O.addInput(MedVar::makeConst(0x9000, 8));
  F.Blocks[0].Ops.push_back(O);
  MedCallInfo CI;
  CI.BlockId = 0;
  CI.OpIdx = OpIdx;
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
  size_t Sink = pushCall(F, "memcpy",
                         temp(0), {temp(1), temp(2), temp(3)});

  DestObject D = resolveDestination(In, Cat, F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Heap);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 16u);
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
  push(F, NdOp::INT_SUB, mkReg(kSP, 1), {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  push(F, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});

  DestObject D = resolveDestination(In, Cat, F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Stack);
  EXPECT_EQ(D.StackOffset, -0x28);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 0x28u); // sound upper bound to the incoming SP.
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
  push(F, NdOp::INT_SUB, mkReg(kSP, 1), {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  push(F, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});

  DestObject D = resolveDestination(In, Cat, F, Sink, 0);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 16u);
  EXPECT_EQ(D.Detail, "declared array");
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
  push(F, NdOp::INT_SUB, mkReg(kSP, 1), {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  push(F, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});

  DestObject D = resolveDestination(In, Cat, F, Sink, 0);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 8u);
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
