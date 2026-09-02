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
#include <array>
#include <limits>
#include <utility>

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

void push(MedBlock &B, NdOp Op, MedVar Out, std::vector<MedVar> Ins) {
  MedOp O;
  O.Opcode = Op;
  O.Output = Out;
  for (auto &I : Ins)
    O.addInput(I);
  B.Ops.push_back(O);
}

size_t pushCall(MedFunc &F, MedBlock &B, const std::string &Name, MedVar Ret,
                std::vector<MedVar> Args, va_t Target = 0x9000) {
  const int OpIdx = static_cast<int>(B.Ops.size());
  MedOp O;
  O.Opcode = NdOp::CALL;
  O.Output = Ret;
  O.addInput(MedVar::makeConst(Target, 8));
  B.Ops.push_back(O);
  MedCallInfo CI;
  CI.BlockId = B.Id;
  CI.OpIdx = OpIdx;
  CI.TargetAddr = Target;
  CI.TargetName = Name;
  CI.Args = std::move(Args);
  F.CallInfos.push_back(CI);
  return F.CallInfos.size() - 1;
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

TypeRef byteArray(uint32_t Count) {
  auto Type = std::make_shared<NdType>();
  Type->Kind = NdTypeKind::Array;
  Type->ArrayCount = Count;
  Type->ElemType = NdType::makeInt(1, false);
  Type->Size = static_cast<uint16_t>(Count);
  return Type;
}

void addTypedLocal(MedFunc &F, const char *Name, int64_t Base, TypeRef Type) {
  MedTypedLocal Local;
  Local.Name = Name;
  Local.StackOff = Base;
  Local.Type = std::move(Type);
  F.TypedLocals.push_back(std::move(Local));
}

} // namespace

TEST(ObjectModel, HeapAllocationIsAnInexactStorageBound) {
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
  EXPECT_EQ(D.Precision, CapacityPrecision::StorageExact);
  EXPECT_FALSE(D.CapacityExact);
}

TEST(ObjectModel, DeepForwardingKeepsHeapAllocationCapacity) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;

  MedFunc F = newFunc(Arch::X64);
  pushCall(F, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  MedVar Forwarded = temp(1);
  for (int I = 0; I < 40; ++I) {
    const MedVar Next = temp(10 + I);
    push(F, NdOp::COPY, Next, {Forwarded});
    Forwarded = Next;
  }
  size_t Sink = pushCall(F, "memcpy", temp(0),
                         {Forwarded, temp(2), MedVar::makeConst(32, 8)});

  DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Heap);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 16u);
  EXPECT_EQ(D.Precision, CapacityPrecision::StorageExact);
  EXPECT_FALSE(D.CapacityExact);
}

TEST(ObjectModel, InteriorHeapPointerUsesRemainingCapacity) {
  for (const auto &[Offset, Remaining] :
       {std::pair<uint64_t, uint64_t>{8, 8}, {16, 0}}) {
    SCOPED_TRACE(Offset);
    BinaryImage Img;
    Img.Arch = Arch::X64;
    AnalysisInput In;
    In.Img = &Img;

    MedFunc F = newFunc(Arch::X64);
    pushCall(F, "malloc", temp(1), {MedVar::makeConst(16, 8)});
    push(F, NdOp::INT_ADD, temp(2), {temp(1), MedVar::makeConst(Offset, 8)});
    size_t Sink = pushCall(F, "memcpy", temp(0),
                           {temp(2), temp(3), MedVar::makeConst(9, 8)});

    DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
    EXPECT_EQ(D.Region, ObjectRegion::Heap);
    ASSERT_TRUE(D.Capacity.has_value());
    EXPECT_EQ(*D.Capacity, Remaining);
    EXPECT_EQ(D.Precision, CapacityPrecision::StorageExact);
    EXPECT_FALSE(D.CapacityExact);
  }
}

TEST(ObjectModel, NarrowEncodedHeapOffsetPreservesRemainingCapacity) {
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  AnalysisInput In;
  In.Img = &Img;

  MedFunc F = newFunc(Arch::AArch64);
  pushCall(F, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  push(F, NdOp::INT_ADD, temp(2),
       {temp(1), MedVar::makeConst(8, 4, ConstantAddressProvenance::Scalar)});
  const size_t Sink = pushCall(F, "memcpy", temp(0),
                               {temp(2), temp(3), MedVar::makeConst(9, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Heap);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 8u);
  EXPECT_EQ(D.Precision, CapacityPrecision::StorageExact);
  EXPECT_FALSE(D.CapacityExact);
}

TEST(ObjectModel, RelocationAddressCannotBecomeAHeapOffset) {
  for (const Arch A : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM}) {
    SCOPED_TRACE(static_cast<int>(A));
    const uint16_t PointerSize = (A == Arch::X86 || A == Arch::ARM) ? 4 : 8;
    BinaryImage Img;
    Img.Arch = A;
    AnalysisInput In;
    In.Img = &Img;

    MedFunc F = newFunc(A);
    pushCall(F, "malloc", temp(1, PointerSize),
             {MedVar::makeConst(16, PointerSize)});
    push(F, NdOp::INT_ADD, temp(2, PointerSize),
         {temp(1, PointerSize),
          MedVar::makeConst(4, PointerSize,
                            ConstantAddressProvenance::DataAddress, 0x4000)});
    const size_t Sink = pushCall(F, "memcpy", temp(0, PointerSize),
                                 {temp(2, PointerSize), temp(3, PointerSize),
                                  MedVar::makeConst(8, PointerSize)});

    const DestObject D =
        resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
    EXPECT_EQ(D.Region, ObjectRegion::Unknown);
    EXPECT_FALSE(D.Capacity.has_value());
  }
}

TEST(ObjectModel, SharedConstantDiamondKeepsAllocationProofLinear) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;

  MedFunc F = newFunc(Arch::X64);
  MedVar Shared = MedVar::makeConst(0, 8);
  for (int I = 0; I < 28; ++I) {
    const MedVar Next = temp(100 + I);
    push(F, NdOp::INT_ADD, Next, {Shared, Shared});
    Shared = Next;
  }
  pushCall(F, "malloc", temp(1), {MedVar::makeConst(64, 8)});
  push(F, NdOp::INT_ADD, temp(2), {temp(1), Shared});
  const size_t Sink = pushCall(F, "memcpy", temp(0),
                               {temp(2), temp(3), MedVar::makeConst(1, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Heap);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 64u);
  EXPECT_EQ(D.Precision, CapacityPrecision::StorageExact);
  EXPECT_FALSE(D.CapacityExact);
}

TEST(ObjectModel, SharedScalarAddressDiamondFailsClosedInBoundedWork) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;

  MedFunc F = newFunc(Arch::X64);
  MedVar Shared = MedVar::makeConst(1, 8, ConstantAddressProvenance::Scalar);
  for (int I = 0; I < 28; ++I) {
    const MedVar Next = temp(300 + I);
    push(F, NdOp::INT_ADD, Next, {Shared, Shared});
    Shared = Next;
  }
  const size_t Sink = pushCall(F, "memcpy", temp(0),
                               {Shared, temp(2), MedVar::makeConst(1, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, CompletedConstantProofSurvivesADeepSharedAlias) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;

  MedFunc F = newFunc(Arch::X64);
  push(F, NdOp::INT_XOR, temp(100),
       {MedVar::makeConst(0, 8), MedVar::makeConst(0, 8)});
  MedVar DeepAlias = temp(100);
  for (int I = 0; I < 33; ++I) {
    const MedVar Next = temp(101 + I);
    push(F, NdOp::COPY, Next, {DeepAlias});
    DeepAlias = Next;
  }
  push(F, NdOp::INT_ADD, temp(200), {temp(100), DeepAlias});
  pushCall(F, "malloc", temp(1), {MedVar::makeConst(64, 8)});
  push(F, NdOp::INT_ADD, temp(2), {temp(1), temp(200)});
  const size_t Sink = pushCall(F, "memcpy", temp(0),
                               {temp(2), temp(3), MedVar::makeConst(1, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Heap);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 64u);
  EXPECT_EQ(D.Precision, CapacityPrecision::StorageExact);
  EXPECT_FALSE(D.CapacityExact);
}

TEST(ObjectModel, UnprovenHeapPointerOffsetsDoNotInventCapacity) {
  for (const MedVar &Offset : {MedVar::makeConst(17, 8), temp(9)}) {
    BinaryImage Img;
    Img.Arch = Arch::X64;
    AnalysisInput In;
    In.Img = &Img;

    MedFunc F = newFunc(Arch::X64);
    pushCall(F, "malloc", temp(1), {MedVar::makeConst(16, 8)});
    push(F, NdOp::INT_ADD, temp(2), {temp(1), Offset});
    size_t Sink = pushCall(F, "memcpy", temp(0),
                           {temp(2), temp(3), MedVar::makeConst(1, 8)});

    DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
    EXPECT_EQ(D.Region, ObjectRegion::Unknown);
    EXPECT_FALSE(D.Capacity.has_value());
  }
}

TEST(ObjectModel, TruncatingPointerArithmeticDoesNotPreserveCapacity) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;

  MedFunc F = newFunc(Arch::X64);
  pushCall(F, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  push(F, NdOp::INT_ADD, temp(2, 4), {temp(1), MedVar::makeConst(1, 8)});
  size_t Sink = pushCall(F, "memcpy", temp(0),
                         {temp(2, 4), temp(3), MedVar::makeConst(1, 8)});

  DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, WidthChangingForwardDoesNotPreserveAllocationCapacity) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;

  MedFunc F = newFunc(Arch::X64);
  pushCall(F, "malloc", temp(1, 4), {MedVar::makeConst(16, 8)});
  push(F, NdOp::INT_ZEXT, temp(2, 8), {temp(1, 4)});
  size_t Sink = pushCall(F, "memcpy", temp(0),
                         {temp(2, 8), temp(3), MedVar::makeConst(1, 8)});

  DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, ReallocationResultHasAStorageBound) {
  for (const char *Name : {"realloc", "reallocf"}) {
    SCOPED_TRACE(Name);
    BinaryImage Img;
    Img.Arch = Arch::X64;
    AnalysisInput In;
    In.Img = &Img;

    MedFunc F = newFunc(Arch::X64);
    pushCall(F, Name, temp(1), {temp(9), MedVar::makeConst(24, 8)});
    size_t Sink = pushCall(F, "memcpy", temp(0),
                           {temp(1), temp(2), MedVar::makeConst(32, 8)});

    DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
    EXPECT_EQ(D.Region, ObjectRegion::Heap);
    ASSERT_TRUE(D.Capacity.has_value());
    EXPECT_EQ(*D.Capacity, 24u);
    EXPECT_EQ(D.Precision, CapacityPrecision::StorageExact);
    EXPECT_FALSE(D.CapacityExact);
  }
}

TEST(ObjectModel, StackAllocationKeepsItsObjectRegion) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;

  MedFunc F = newFunc(Arch::X64);
  pushCall(F, "alloca", temp(1), {MedVar::makeConst(24, 8)});
  size_t Sink = pushCall(F, "memcpy", temp(0),
                         {temp(1), temp(2), MedVar::makeConst(32, 8)});

  DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Stack);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 24u);
  EXPECT_EQ(D.Precision, CapacityPrecision::StorageExact);
  EXPECT_FALSE(D.CapacityExact);
}

TEST(ObjectModel, AuthenticatedGlobalExtentHasRemainingCapacityAcrossFormats) {
  constexpr std::array<std::pair<BinaryFormat, Arch>, 6> Targets = {{
      {BinaryFormat::ELF, Arch::X64},
      {BinaryFormat::ELF, Arch::AArch64},
      {BinaryFormat::COFF, Arch::X64},
      {BinaryFormat::COFF, Arch::AArch64},
      {BinaryFormat::MachO, Arch::X64},
      {BinaryFormat::MachO, Arch::AArch64},
  }};
  for (const auto &[Format, A] : Targets) {
    SCOPED_TRACE(static_cast<int>(Format));
    SCOPED_TRACE(static_cast<int>(A));
    BinaryImage Img;
    Img.Arch = A;
    Img.Format = Format;
    Segment Data;
    Data.Name = "data";
    Data.VA = 0x1000;
    Data.Size = 0x40;
    Data.FileSz = 0x40;
    Data.Data.resize(0x40);
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Img.Segments.push_back(std::move(Data));
    Img.Symbols.push_back(Symbol{"buffer", 0x1010, 16, false});
    Img.ExactDataObjects.push_back(ExactDataObjectExtent{
        0x1010, 16, ExactDataObjectEvidence::AuthenticatedDebug,
        ExactDataObjectPrecision::TypedBuffer});
    Img.ExactDataObjects.push_back(ExactDataObjectExtent{
        0x1010, 16, ExactDataObjectEvidence::ELFObjectSymbol,
        ExactDataObjectPrecision::Storage});

    AnalysisInput In;
    In.Img = &Img;
    MedFunc F = newFunc(A);
    push(F, NdOp::COPY, temp(1),
         {MedVar::makeConst(0x1014, 8, ConstantAddressProvenance::DataAddress,
                            0x1000)});
    size_t Sink = pushCall(F, "memcpy", temp(0),
                           {temp(1), temp(2), MedVar::makeConst(8, 8)});

    DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
    EXPECT_EQ(D.Region, ObjectRegion::Global);
    ASSERT_TRUE(D.Capacity.has_value());
    EXPECT_EQ(*D.Capacity, 12u);
    EXPECT_EQ(D.Precision, CapacityPrecision::TypedBufferExact);
    EXPECT_TRUE(D.CapacityExact);
  }
}

TEST(ObjectModel, GenericAddressBindsUniqueAuthenticatedObjectExtent) {
  constexpr std::array<std::pair<BinaryFormat, Arch>, 6> Targets = {{
      {BinaryFormat::ELF, Arch::X64},
      {BinaryFormat::ELF, Arch::AArch64},
      {BinaryFormat::COFF, Arch::X64},
      {BinaryFormat::COFF, Arch::AArch64},
      {BinaryFormat::MachO, Arch::X64},
      {BinaryFormat::MachO, Arch::AArch64},
  }};
  for (const auto &[Format, A] : Targets) {
    SCOPED_TRACE(static_cast<int>(Format));
    SCOPED_TRACE(static_cast<int>(A));
    BinaryImage Img;
    Img.Arch = A;
    Img.Format = Format;
    Segment Data;
    Data.Name = "data";
    Data.VA = 0x1000;
    Data.Size = 0x40;
    Data.FileSz = 0x40;
    Data.Data.resize(0x40);
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Img.Segments.push_back(std::move(Data));
    Img.ExactDataObjects.push_back(ExactDataObjectExtent{
        0x1010, 8, ExactDataObjectEvidence::AuthenticatedDebug,
        ExactDataObjectPrecision::TypedBuffer});

    AnalysisInput In;
    In.Img = &Img;
    MedFunc F = newFunc(A);
    const size_t Sink = pushCall(
        F, "memcpy", temp(0),
        {MedVar::makeConst(0x1013, 8, ConstantAddressProvenance::Address),
         temp(2), MedVar::makeConst(6, 8)});

    const DestObject D =
        resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
    EXPECT_EQ(D.Region, ObjectRegion::Global);
    ASSERT_TRUE(D.Capacity.has_value());
    EXPECT_EQ(*D.Capacity, 5u);
    EXPECT_EQ(D.Precision, CapacityPrecision::TypedBufferExact);
    EXPECT_TRUE(D.CapacityExact);
    EXPECT_EQ(D.Detail, "authenticated typed buffer");
  }
}

TEST(ObjectModel, ELFStorageExtentIsOnlyAnUpperBoundForCountedWrites) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Format = BinaryFormat::ELF;
  Segment Data;
  Data.VA = 0x1000;
  Data.Size = 0x40;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Data));
  Img.ExactDataObjects.push_back(ExactDataObjectExtent{
      0x1010, 16, ExactDataObjectEvidence::ELFObjectSymbol,
      ExactDataObjectPrecision::Storage});

  AnalysisInput In;
  In.Img = &Img;
  MedFunc F = newFunc(Arch::X64);
  const size_t Sink = pushCall(
      F, "memcpy", temp(0),
      {MedVar::makeConst(0x1013, 8, ConstantAddressProvenance::Address),
       temp(2), MedVar::makeConst(6, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Global);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 13u);
  EXPECT_EQ(D.Precision, CapacityPrecision::StorageExact);
  EXPECT_FALSE(D.CapacityExact);
  EXPECT_EQ(D.Detail, "storage object upper bound");
}

TEST(ObjectModel, TypedMemberCannotNarrowOverlappingELFContainerByValueAlone) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Format = BinaryFormat::ELF;
  Segment Data;
  Data.VA = 0x1000;
  Data.Size = 0x40;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Data));
  Img.ExactDataObjects.push_back(ExactDataObjectExtent{
      0x1010, 16, ExactDataObjectEvidence::ELFObjectSymbol,
      ExactDataObjectPrecision::Storage});
  Img.ExactDataObjects.push_back(ExactDataObjectExtent{
      0x1010, 8, ExactDataObjectEvidence::AuthenticatedDebug,
      ExactDataObjectPrecision::TypedBuffer});

  AnalysisInput In;
  In.Img = &Img;
  MedFunc F = newFunc(Arch::X64);
  const size_t Sink = pushCall(
      F, "memcpy", temp(0),
      {MedVar::makeConst(0x1013, 8, ConstantAddressProvenance::Address),
       temp(2), MedVar::makeConst(9, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Global);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 13u);
  EXPECT_EQ(D.Precision, CapacityPrecision::ContainerUpperBound);
  EXPECT_FALSE(D.CapacityExact);
  EXPECT_EQ(D.Detail, "ambiguous object upper bound");
}

TEST(ObjectModel, NumericConstantsCannotBorrowAuthenticatedObjectExtent) {
  for (const ConstantAddressProvenance Provenance :
       {ConstantAddressProvenance::Unknown, ConstantAddressProvenance::Scalar,
        ConstantAddressProvenance::AddressFragment,
        ConstantAddressProvenance::CodeAddress}) {
    SCOPED_TRACE(static_cast<int>(Provenance));
    BinaryImage Img;
    Img.Arch = Arch::X64;
    Segment Data;
    Data.VA = 0x1000;
    Data.Size = 0x40;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Img.Segments.push_back(std::move(Data));
    Img.ExactDataObjects.push_back(ExactDataObjectExtent{
        0x1010, 8, ExactDataObjectEvidence::ELFObjectSymbol});

    AnalysisInput In;
    In.Img = &Img;
    MedFunc F = newFunc(Arch::X64);
    const size_t Sink = pushCall(F, "memcpy", temp(0),
                                 {MedVar::makeConst(0x1013, 8, Provenance),
                                  temp(2), MedVar::makeConst(1, 8)});

    const DestObject D =
        resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
    EXPECT_EQ(D.Region, ObjectRegion::Unknown);
    EXPECT_FALSE(D.Capacity.has_value());
  }
}

TEST(ObjectModel, ConflictingAuthenticatedObjectExtentsFailClosed) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Segment Data;
  Data.VA = 0x1000;
  Data.Size = 0x40;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Data));
  Img.ExactDataObjects.push_back(ExactDataObjectExtent{
      0x1010, 8, ExactDataObjectEvidence::ELFObjectSymbol});
  Img.ExactDataObjects.push_back(ExactDataObjectExtent{
      0x1012, 8, ExactDataObjectEvidence::AuthenticatedDebug,
      ExactDataObjectPrecision::TypedBuffer});

  AnalysisInput In;
  In.Img = &Img;
  MedFunc F = newFunc(Arch::X64);
  const size_t Sink = pushCall(
      F, "memcpy", temp(0),
      {MedVar::makeConst(0x1011, 8, ConstantAddressProvenance::Address),
       temp(2), MedVar::makeConst(1, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Global);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 7u);
  EXPECT_EQ(D.Precision, CapacityPrecision::ContainerUpperBound);
  EXPECT_FALSE(D.CapacityExact);
}

TEST(ObjectModel, UniqueOnePastObjectExtentIsZeroButAdjacentBaseIsAmbiguous) {
  for (const bool HasAdjacentObject : {false, true}) {
    for (const va_t Address : {va_t{0x1013}, va_t{0x1018}}) {
      SCOPED_TRACE(HasAdjacentObject);
      SCOPED_TRACE(Address);
      BinaryImage Img;
      Img.Arch = Arch::AArch64;
      Segment Data;
      Data.VA = 0x1000;
      Data.Size = 0x40;
      Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Img.Segments.push_back(std::move(Data));
      Img.ExactDataObjects.push_back(ExactDataObjectExtent{
          0x1010, 8, ExactDataObjectEvidence::AuthenticatedDebug,
          ExactDataObjectPrecision::TypedBuffer});
      if (HasAdjacentObject)
        Img.ExactDataObjects.push_back(ExactDataObjectExtent{
            0x1018, 8, ExactDataObjectEvidence::AuthenticatedDebug,
            ExactDataObjectPrecision::TypedBuffer});

      AnalysisInput In;
      In.Img = &Img;
      MedFunc F = newFunc(Arch::AArch64);
      const size_t Sink = pushCall(
          F, "memcpy", temp(0),
          {MedVar::makeConst(Address, 8, ConstantAddressProvenance::Address),
           temp(2), MedVar::makeConst(1, 8)});
      const DestObject D =
          resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
      EXPECT_EQ(D.Region, ObjectRegion::Global);
      ASSERT_TRUE(D.Capacity.has_value());
      if (HasAdjacentObject) {
        EXPECT_EQ(*D.Capacity, Address == 0x1018 ? 8u : 5u);
        EXPECT_EQ(D.Precision, CapacityPrecision::ContainerUpperBound);
        EXPECT_FALSE(D.CapacityExact);
      } else {
        EXPECT_EQ(*D.Capacity, Address == 0x1018 ? 0u : 5u);
        EXPECT_EQ(D.Precision, CapacityPrecision::TypedBufferExact);
        EXPECT_TRUE(D.CapacityExact);
      }
    }
  }
}

TEST(ObjectModel, NarrowDestinationPointerFailsClosed) {
  constexpr va_t BufferVA = 0x50;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Segment Data;
  Data.Name = "data";
  Data.VA = BufferVA;
  Data.Size = 0x20;
  Data.FileSz = 0x20;
  Data.Data.resize(0x20);
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Data));
  Img.Symbols.push_back(Symbol{"buffer", BufferVA, 8, false});

  AnalysisInput In;
  In.Img = &Img;
  MedFunc F = newFunc(Arch::X64);
  size_t Sink = pushCall(
      F, "memcpy", temp(0),
      {MedVar::makeConst(BufferVA, 1, ConstantAddressProvenance::DataAddress,
                         BufferVA),
       temp(2), MedVar::makeConst(9, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, WritableGlobalRunIsOnlyACapacityUpperBound) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Segment Data;
  Data.Name = "data";
  Data.VA = 0x2000;
  Data.Size = 0x40;
  Data.FileSz = 0x40;
  Data.Data.resize(0x40);
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Data));

  AnalysisInput In;
  In.Img = &Img;
  MedFunc F = newFunc(Arch::X64);
  size_t Sink =
      pushCall(F, "memcpy", temp(0),
               {MedVar::makeConst(
                    0x2010, 8, ConstantAddressProvenance::DataAddress, 0x2000),
                temp(2), MedVar::makeConst(8, 8)});

  DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Global);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 0x30u);
  EXPECT_FALSE(D.CapacityExact);
}

TEST(ObjectModel, OnlyExactDataOccurrenceCanNameZeroVAGlobal) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Segment Data;
  Data.Name = "data";
  Data.VA = 0;
  Data.Size = 0x20;
  Data.FileSz = 0x20;
  Data.Data.resize(0x20);
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Data));
  Segment Foreign;
  Foreign.Name = "foreign";
  Foreign.VA = 0x1000;
  Foreign.Size = 0x20;
  Foreign.FileSz = 0x20;
  Foreign.Data.resize(0x20);
  Foreign.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Foreign));
  Img.Symbols.push_back(Symbol{"zero_buffer", 0, 16, false});
  Img.ExactDataObjects.push_back(
      ExactDataObjectExtent{0, 16, ExactDataObjectEvidence::AuthenticatedDebug,
                            ExactDataObjectPrecision::TypedBuffer});

  AnalysisInput In;
  In.Img = &Img;
  MedFunc F = newFunc(Arch::X64);
  size_t NullSink =
      pushCall(F, "memcpy", temp(0),
               {MedVar::makeConst(0, 8), temp(2), MedVar::makeConst(8, 8)});
  size_t GlobalSink = pushCall(
      F, "memcpy", temp(0),
      {MedVar::makeConst(0, 8, ConstantAddressProvenance::DataAddress, 0),
       temp(2), MedVar::makeConst(8, 8)});
  size_t WrongOwnerSink = pushCall(
      F, "memcpy", temp(0),
      {MedVar::makeConst(0, 8, ConstantAddressProvenance::DataAddress, 0x1000),
       temp(2), MedVar::makeConst(8, 8)});

  DestObject Null =
      resolveDestination(In, SinkCatalog::defaults(), F, NullSink, 0);
  EXPECT_FALSE(Null.Capacity.has_value());
  DestObject Global =
      resolveDestination(In, SinkCatalog::defaults(), F, GlobalSink, 0);
  EXPECT_EQ(Global.Region, ObjectRegion::Global);
  ASSERT_TRUE(Global.Capacity.has_value());
  EXPECT_EQ(*Global.Capacity, 16u);
  EXPECT_TRUE(Global.CapacityExact);
  EXPECT_FALSE(
      resolveDestination(In, SinkCatalog::defaults(), F, WrongOwnerSink, 0)
          .Capacity.has_value());
}

TEST(ObjectModel, GlobalCapacityUsesWritableSectionOwnership) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Segment Mapping;
  Mapping.Name = "rw";
  Mapping.VA = 0x3000;
  Mapping.Size = 0x80;
  Mapping.FileSz = 0x80;
  Mapping.Data.resize(0x80);
  Mapping.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Mapping));

  Section ReadOnly;
  ReadOnly.Name = "const";
  ReadOnly.VA = 0x3000;
  ReadOnly.Size = 0x20;
  ReadOnly.Flags = SegmentFlags::Readable;
  Img.Sections.push_back(std::move(ReadOnly));
  Section Writable;
  Writable.Name = "data";
  Writable.VA = 0x3020;
  Writable.Size = 0x10;
  Writable.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Sections.push_back(std::move(Writable));
  Img.Symbols.push_back(Symbol{"const_object", 0x3000, 16, false});
  Img.Symbols.push_back(Symbol{"oversized_object", 0x3028, 16, false});

  AnalysisInput In;
  In.Img = &Img;
  MedFunc F = newFunc(Arch::X64);
  size_t ReadOnlySink =
      pushCall(F, "memcpy", temp(0),
               {MedVar::makeConst(
                    0x3000, 8, ConstantAddressProvenance::DataAddress, 0x3000),
                temp(2), MedVar::makeConst(4, 8)});
  size_t WritableSink =
      pushCall(F, "memcpy", temp(0),
               {MedVar::makeConst(
                    0x3028, 8, ConstantAddressProvenance::DataAddress, 0x3020),
                temp(2), MedVar::makeConst(4, 8)});
  size_t GapSink =
      pushCall(F, "memcpy", temp(0),
               {MedVar::makeConst(
                    0x3040, 8, ConstantAddressProvenance::DataAddress, 0x3000),
                temp(2), MedVar::makeConst(4, 8)});

  EXPECT_FALSE(
      resolveDestination(In, SinkCatalog::defaults(), F, ReadOnlySink, 0)
          .Capacity.has_value());
  DestObject InWritable =
      resolveDestination(In, SinkCatalog::defaults(), F, WritableSink, 0);
  ASSERT_TRUE(InWritable.Capacity.has_value());
  EXPECT_EQ(*InWritable.Capacity, 8u);
  EXPECT_FALSE(InWritable.CapacityExact);
  EXPECT_FALSE(resolveDestination(In, SinkCatalog::defaults(), F, GapSink, 0)
                   .Capacity.has_value());
}

TEST(ObjectModel, OnePastDestinationKeepsItsContainerOwnerAcrossFormats) {
  constexpr std::array<std::pair<BinaryFormat, Arch>, 6> Targets = {{
      {BinaryFormat::ELF, Arch::X64},
      {BinaryFormat::ELF, Arch::AArch64},
      {BinaryFormat::COFF, Arch::X64},
      {BinaryFormat::COFF, Arch::AArch64},
      {BinaryFormat::MachO, Arch::X64},
      {BinaryFormat::MachO, Arch::AArch64},
  }};
  for (const auto &[Format, A] : Targets) {
    SCOPED_TRACE(static_cast<int>(Format));
    SCOPED_TRACE(static_cast<int>(A));

    BinaryImage Img;
    Img.Arch = A;
    Img.Format = Format;
    Segment Mapping;
    Mapping.Name = "rw";
    Mapping.VA = 0x4000;
    Mapping.Size = 0x40;
    Mapping.FileSz = 0x40;
    Mapping.Data.resize(0x40);
    Mapping.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Img.Segments.push_back(std::move(Mapping));

    Section Owner;
    Owner.Name = "owner";
    Owner.VA = 0x4000;
    Owner.Size = 0x10;
    Owner.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Img.Sections.push_back(std::move(Owner));
    Section Neighbour;
    Neighbour.Name = "neighbour";
    Neighbour.VA = 0x4010;
    Neighbour.Size = 0x20;
    Neighbour.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Img.Sections.push_back(std::move(Neighbour));

    AnalysisInput In;
    In.Img = &Img;
    MedFunc F = newFunc(A);
    push(F, NdOp::COPY, temp(1),
         {MedVar::makeConst(0x4010, 8, ConstantAddressProvenance::DataAddress,
                            0x4000)});
    const size_t Sink = pushCall(F, "memcpy", temp(0),
                                 {temp(1), temp(2), MedVar::makeConst(1, 8)});

    DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
    EXPECT_EQ(D.Region, ObjectRegion::Global);
    ASSERT_TRUE(D.Capacity.has_value());
    EXPECT_EQ(*D.Capacity, 0u);
    EXPECT_EQ(D.Precision, CapacityPrecision::ContainerUpperBound);
    EXPECT_FALSE(D.CapacityExact);
  }
}

TEST(ObjectModel, AdjacentSameSectionSymbolsDoNotTurnOnePastIntoExactCapacity) {
  constexpr std::array<std::pair<BinaryFormat, Arch>, 12> Targets = {{
      {BinaryFormat::ELF, Arch::X64},
      {BinaryFormat::ELF, Arch::AArch64},
      {BinaryFormat::ELF, Arch::X86},
      {BinaryFormat::ELF, Arch::ARM},
      {BinaryFormat::COFF, Arch::X64},
      {BinaryFormat::COFF, Arch::AArch64},
      {BinaryFormat::COFF, Arch::X86},
      {BinaryFormat::COFF, Arch::ARM},
      {BinaryFormat::MachO, Arch::X64},
      {BinaryFormat::MachO, Arch::AArch64},
      {BinaryFormat::MachO, Arch::X86},
      {BinaryFormat::MachO, Arch::ARM},
  }};
  for (const auto &[Format, A] : Targets) {
    SCOPED_TRACE(static_cast<int>(Format));
    SCOPED_TRACE(static_cast<int>(A));
    const uint16_t PointerSize = (A == Arch::X86 || A == Arch::ARM) ? 4 : 8;

    BinaryImage Img;
    Img.Arch = A;
    Img.Format = Format;
    Segment Mapping;
    Mapping.Name = "rw";
    Mapping.VA = 0x4000;
    Mapping.Size = 0x20;
    Mapping.FileSz = 0x20;
    Mapping.Data.resize(0x20);
    Mapping.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Img.Segments.push_back(std::move(Mapping));
    Section Data;
    Data.Name = "data";
    Data.VA = 0x4000;
    Data.Size = 0x20;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Img.Sections.push_back(std::move(Data));
    Img.Symbols.push_back(Symbol{"first", 0x4000, 0x10, false});
    Img.Symbols.push_back(Symbol{"second", 0x4010, 0x10, false});

    AnalysisInput In;
    In.Img = &Img;
    MedFunc F = newFunc(A);
    const size_t Sink = pushCall(
        F, "memcpy", temp(0, PointerSize),
        {MedVar::makeConst(0x4010, PointerSize,
                           ConstantAddressProvenance::DataAddress, 0x4000),
         temp(2, PointerSize), MedVar::makeConst(1, PointerSize)});

    const DestObject D =
        resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
    EXPECT_EQ(D.Region, ObjectRegion::Global);
    ASSERT_TRUE(D.Capacity.has_value());
    EXPECT_EQ(*D.Capacity, 0x10u);
    EXPECT_FALSE(D.CapacityExact);
  }
}

TEST(ObjectModel,
     ComputedOnePastDestinationKeepsItsContainerOwnerAcrossFormats) {
  constexpr std::array<std::pair<BinaryFormat, Arch>, 12> Targets = {{
      {BinaryFormat::ELF, Arch::X64},
      {BinaryFormat::ELF, Arch::AArch64},
      {BinaryFormat::ELF, Arch::X86},
      {BinaryFormat::ELF, Arch::ARM},
      {BinaryFormat::COFF, Arch::X64},
      {BinaryFormat::COFF, Arch::AArch64},
      {BinaryFormat::COFF, Arch::X86},
      {BinaryFormat::COFF, Arch::ARM},
      {BinaryFormat::MachO, Arch::X64},
      {BinaryFormat::MachO, Arch::AArch64},
      {BinaryFormat::MachO, Arch::X86},
      {BinaryFormat::MachO, Arch::ARM},
  }};
  for (const auto &[Format, A] : Targets) {
    for (unsigned OffsetShape = 0; OffsetShape != 4; ++OffsetShape) {
      SCOPED_TRACE(static_cast<int>(Format));
      SCOPED_TRACE(static_cast<int>(A));
      SCOPED_TRACE(OffsetShape);
      const uint16_t PointerSize = (A == Arch::X86 || A == Arch::ARM) ? 4 : 8;

      BinaryImage Img;
      Img.Arch = A;
      Img.Format = Format;
      Segment Mapping;
      Mapping.Name = "rw";
      Mapping.VA = 0x4000;
      Mapping.Size = 0x40;
      Mapping.FileSz = 0x40;
      Mapping.Data.resize(0x40);
      Mapping.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Img.Segments.push_back(std::move(Mapping));

      Section Owner;
      Owner.Name = "owner";
      Owner.VA = 0x4000;
      Owner.Size = 0x10;
      Owner.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Img.Sections.push_back(std::move(Owner));
      Section Neighbour;
      Neighbour.Name = "neighbour";
      Neighbour.VA = 0x4010;
      Neighbour.Size = 0x20;
      Neighbour.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Img.Sections.push_back(std::move(Neighbour));

      AnalysisInput In;
      In.Img = &Img;
      MedFunc F = newFunc(A);
      MedVar Offset = MedVar::makeConst(0x10, PointerSize,
                                        ConstantAddressProvenance::Scalar);
      if (OffsetShape == 1) {
        push(F, NdOp::COPY, temp(4, PointerSize), {Offset});
        Offset = temp(4, PointerSize);
      } else if (OffsetShape == 2) {
        Offset = MedVar::makeConst(0, PointerSize,
                                   ConstantAddressProvenance::Scalar);
        for (int I = 0; I != 28; ++I) {
          const MedVar Next = temp(100 + I, PointerSize);
          push(F, NdOp::INT_ADD, Next, {Offset, Offset});
          Offset = Next;
        }
        push(F, NdOp::INT_ADD, temp(140, PointerSize),
             {Offset, MedVar::makeConst(0x10, PointerSize,
                                        ConstantAddressProvenance::Scalar)});
        Offset = temp(140, PointerSize);
      } else if (OffsetShape == 3) {
        Offset = MedVar::makeConst(0x10, PointerSize);
        push(F, NdOp::COPY, temp(4, PointerSize), {Offset});
        Offset = temp(4, PointerSize);
      }
      push(F, NdOp::INT_ADD, temp(1, PointerSize),
           {MedVar::makeConst(0x4000, PointerSize,
                              ConstantAddressProvenance::DataAddress, 0x4000),
            Offset});
      push(F, NdOp::COPY, temp(2, PointerSize), {temp(1, PointerSize)});
      const size_t Sink = pushCall(F, "memcpy", temp(0, PointerSize),
                                   {temp(2, PointerSize), temp(3, PointerSize),
                                    MedVar::makeConst(1, PointerSize)});

      const DestObject D =
          resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
      if (OffsetShape == 3) {
        EXPECT_FALSE(D.CapacityExact);
        continue;
      }
      EXPECT_EQ(D.Region, ObjectRegion::Global);
      ASSERT_TRUE(D.Capacity.has_value());
      EXPECT_EQ(*D.Capacity, 0u);
      EXPECT_EQ(D.Precision, CapacityPrecision::ContainerUpperBound);
      EXPECT_FALSE(D.CapacityExact);
    }
  }
}

TEST(ObjectModel, NarrowEncodedGlobalOffsetKeepsItsOwnerAcross64BitFormats) {
  for (const BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
    for (const Arch A : {Arch::X64, Arch::AArch64}) {
      SCOPED_TRACE(static_cast<int>(Format));
      SCOPED_TRACE(static_cast<int>(A));

      BinaryImage Img;
      Img.Arch = A;
      Img.Format = Format;
      Segment Mapping;
      Mapping.Name = "rw";
      Mapping.VA = 0x4000;
      Mapping.Size = 0x40;
      Mapping.FileSz = 0x40;
      Mapping.Data.resize(0x40);
      Mapping.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Img.Segments.push_back(std::move(Mapping));

      Section Owner;
      Owner.Name = "owner";
      Owner.VA = 0x4000;
      Owner.Size = 0x10;
      Owner.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Img.Sections.push_back(std::move(Owner));
      Section Neighbour;
      Neighbour.Name = "neighbour";
      Neighbour.VA = 0x4010;
      Neighbour.Size = 0x20;
      Neighbour.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Img.Sections.push_back(std::move(Neighbour));

      AnalysisInput In;
      In.Img = &Img;
      MedFunc F = newFunc(A);
      push(F, NdOp::INT_ADD, temp(1),
           {MedVar::makeConst(0x4000, 8, ConstantAddressProvenance::DataAddress,
                              0x4000),
            MedVar::makeConst(0x10, 4, ConstantAddressProvenance::Scalar)});
      const size_t Sink = pushCall(F, "memcpy", temp(0),
                                   {temp(1), temp(2), MedVar::makeConst(1, 8)});

      const DestObject D =
          resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
      EXPECT_EQ(D.Region, ObjectRegion::Global);
      ASSERT_TRUE(D.Capacity.has_value());
      EXPECT_EQ(*D.Capacity, 0u);
      EXPECT_EQ(D.Precision, CapacityPrecision::ContainerUpperBound);
      EXPECT_FALSE(D.CapacityExact);
    }
  }
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
  pushCall(F, "opaque_external", MedVar{}, {MedVar::makeConst(0x7000, 8)});
  push(F, NdOp::LOAD, temp(2), {temp(10)});
  size_t Sink = pushCall(F, "memcpy", temp(0),
                         {temp(2), temp(3), MedVar::makeConst(8, 8)});

  DestObject D = resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 16u);
  EXPECT_EQ(D.Precision, CapacityPrecision::StorageExact);
  EXPECT_FALSE(D.CapacityExact);
}

TEST(ObjectModel, NonDefaultLoadCannotRecoverAllocationSpill) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  push(F, NdOp::INT_SUB, temp(10), {mkReg(kSP, 0), MedVar::makeConst(0x10, 8)});
  pushCall(F, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  push(F, NdOp::STORE, MedVar{}, {temp(10), temp(1)});
  push(F, NdOp::LOAD, temp(2), {temp(10)});
  F.Blocks[0].Ops.back().MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  const size_t Sink = pushCall(F, "memcpy", temp(0),
                               {temp(2), temp(3), MedVar::makeConst(8, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, NonDefaultStoreCannotCreateAllocationSpill) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  push(F, NdOp::INT_SUB, temp(10), {mkReg(kSP, 0), MedVar::makeConst(0x10, 8)});
  pushCall(F, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  push(F, NdOp::STORE, MedVar{}, {temp(10), temp(1)});
  F.Blocks[0].Ops.back().MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
  push(F, NdOp::LOAD, temp(2), {temp(10)});
  const size_t Sink = pushCall(F, "memcpy", temp(0),
                               {temp(2), temp(3), MedVar::makeConst(8, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, CallAliasesInvalidateAllocationSpills) {
  constexpr std::array<const char *, 7> Scenarios = {
      "interior-call-pointer",       "unresolved-call-alias",
      "unresolved-stored-escape",    "missing-call-inventory",
      "frame-base-call-alias",       "opaque-destination-stored-escape",
      "copied-frame-base-call-alias"};
  for (size_t Scenario = 0; Scenario < Scenarios.size(); ++Scenario) {
    SCOPED_TRACE(Scenarios[Scenario]);
    BinaryImage Img;
    Img.Arch = Arch::X64;
    AnalysisInput In;
    In.Img = &Img;
    In.StackPointerReg = kSP;
    In.StackRegsKnown = true;

    MedFunc F = newFunc(Arch::X64);
    push(F, NdOp::INT_SUB, mkReg(kSP, 1),
         {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
    push(F, NdOp::INT_ADD, temp(10),
         {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
    pushCall(F, "malloc", temp(1), {MedVar::makeConst(16, 8)});
    push(F, NdOp::STORE, MedVar{}, {temp(10), temp(1)});

    if (Scenario == 3) {
      push(F, NdOp::CALL, temp(30), {MedVar::makeConst(0xa000, 8)});
    } else if (Scenario == 4) {
      pushCall(F, "opaque_external", MedVar{}, {mkReg(kSP, 1)});
    } else if (Scenario == 6) {
      push(F, NdOp::COPY, temp(11), {mkReg(kSP, 1)});
    } else if (Scenario != 0) {
      push(F, NdOp::SELECT, temp(11),
           {temp(20, 1), temp(10), MedVar::makeConst(0x7000, 8)});
    } else {
      push(F, NdOp::INT_ADD, temp(11), {temp(10), MedVar::makeConst(1, 8)});
    }
    if (Scenario < 2 || Scenario == 6) {
      pushCall(F, "opaque_external", MedVar{}, {temp(11)});
    } else if (Scenario == 2) {
      push(F, NdOp::INT_ADD, temp(12),
           {mkReg(kSP, 1), MedVar::makeConst(0x18, 8)});
      push(F, NdOp::STORE, MedVar{}, {temp(12), temp(11)});
    } else if (Scenario == 5) {
      push(F, NdOp::STORE, MedVar{}, {temp(30), temp(11)});
    }

    push(F, NdOp::LOAD, temp(2), {temp(10)});
    const size_t Sink = pushCall(F, "memcpy", temp(0),
                                 {temp(2), temp(3), MedVar::makeConst(8, 8)});

    const DestObject D =
        resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
    EXPECT_EQ(D.Region, ObjectRegion::Unknown);
    EXPECT_FALSE(D.Capacity.has_value());
  }
}

TEST(ObjectModel, IncompletePhiAddressInvalidatesAllocationSpill) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;
  MedFunc F = newFunc(Arch::X64);
  push(F, NdOp::INT_SUB, temp(10), {mkReg(kSP, 0), MedVar::makeConst(0x10, 8)});
  pushCall(F, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  push(F, NdOp::STORE, MedVar{}, {temp(10), temp(1)});
  PhiNode Phi;
  Phi.Output = temp(20);
  Phi.Args = {{9, MedVar::makeConst(0x5000, 8)}};
  F.Blocks[0].Phis.push_back(Phi);
  push(F, NdOp::STORE, MedVar{}, {Phi.Output, temp(9)});
  push(F, NdOp::LOAD, temp(2), {temp(10)});
  const size_t Sink = pushCall(F, "memcpy", temp(0),
                               {temp(2), temp(3), MedVar::makeConst(8, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, DeepUnresolvedFrameStoreInvalidatesAllocationSpill) {
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

  MedVar DeepAddress = temp(10);
  for (int I = 0; I < 70; ++I) {
    const MedVar Next = temp(100 + I);
    push(F, NdOp::COPY, Next, {DeepAddress});
    DeepAddress = Next;
  }
  push(F, NdOp::STORE, MedVar{}, {DeepAddress, temp(9)});
  push(F, NdOp::LOAD, temp(2), {temp(10)});
  const size_t Sink = pushCall(F, "memcpy", temp(0),
                               {temp(2), temp(3), MedVar::makeConst(8, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, RecurrentFrameAliasCannotReuseAContextualNegative) {
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

  PhiNode A;
  A.Output = temp(100);
  A.Args = {{0, temp(101)}, {0, temp(10)}};
  F.Blocks[0].Phis.push_back(std::move(A));
  PhiNode B;
  B.Output = temp(101);
  B.Args = {{0, temp(100)}};
  F.Blocks[0].Phis.push_back(std::move(B));

  // The first unknown write visits A, reaches the stack initializer, and used
  // to cache B=false only because A was active on that DFS path.  The exact
  // spill then resets the slot state before B is queried again.
  push(F, NdOp::STORE, MedVar{}, {temp(100), temp(9)});
  pushCall(F, "malloc", temp(1), {MedVar::makeConst(16, 8)});
  push(F, NdOp::STORE, MedVar{}, {temp(10), temp(1)});
  push(F, NdOp::STORE, MedVar{}, {temp(101), temp(8)});
  push(F, NdOp::LOAD, temp(2), {temp(10)});
  const size_t Sink = pushCall(F, "memcpy", temp(0),
                               {temp(2), temp(3), MedVar::makeConst(8, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, SharedNonFrameDiamondPreservesAllocationSpill) {
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

  MedVar SharedAddress = MedVar::makeConst(0x5000, 8);
  for (int I = 0; I < 28; ++I) {
    const MedVar Next = temp(200 + I);
    push(F, NdOp::SELECT, Next,
         {MedVar::makeConst(1, 1), SharedAddress, SharedAddress});
    SharedAddress = Next;
  }
  push(F, NdOp::STORE, MedVar{}, {SharedAddress, temp(9)});
  push(F, NdOp::LOAD, temp(2), {temp(10)});
  const size_t Sink = pushCall(F, "memcpy", temp(0),
                               {temp(2), temp(3), MedVar::makeConst(8, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Heap);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 16u);
  EXPECT_EQ(D.Precision, CapacityPrecision::StorageExact);
  EXPECT_FALSE(D.CapacityExact);
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
  EXPECT_EQ(D.Precision, CapacityPrecision::StorageExact);
  EXPECT_FALSE(D.CapacityExact);
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
  EXPECT_EQ(D.Precision, CapacityPrecision::StorageExact);
  EXPECT_FALSE(D.CapacityExact);
}

TEST(ObjectModel, NarrowAllocationSizeIsNotAnExactCapacity) {
  for (int Mode = 0; Mode < 3; ++Mode) {
    SCOPED_TRACE(Mode);
    BinaryImage Img;
    Img.Arch = Mode == 0 ? Arch::X64 : Arch::Unknown;
    AnalysisInput In;
    In.Img = Mode == 2 ? nullptr : &Img;

    MedFunc F = newFunc(Img.Arch);
    pushCall(F, "malloc", temp(1), {MedVar::makeConst(16, 1)});
    const size_t Sink = pushCall(F, "memcpy", temp(0),
                                 {temp(1), temp(2), MedVar::makeConst(8, 8)});

    const DestObject D =
        resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
    EXPECT_EQ(D.Region, ObjectRegion::Unknown);
    EXPECT_FALSE(D.Capacity.has_value());
  }
}

TEST(ObjectModel, NarrowAllocationResultIsNotAPointerWithoutTargetInfo) {
  AnalysisInput In;

  MedFunc F = newFunc(Arch::Unknown);
  pushCall(F, "malloc", temp(1, 1), {MedVar::makeConst(16, 8)});
  const size_t Sink = pushCall(F, "memcpy", temp(0),
                               {temp(1, 1), temp(2), MedVar::makeConst(8, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, NarrowCallocOperandIsNotAnExactCapacity) {
  for (const int NarrowArg : {0, 1}) {
    SCOPED_TRACE(NarrowArg);
    BinaryImage Img;
    Img.Arch = Arch::X64;
    AnalysisInput In;
    In.Img = &Img;

    MedFunc F = newFunc(Arch::X64);
    std::vector<MedVar> Args = {MedVar::makeConst(4, 8),
                                MedVar::makeConst(8, 8)};
    Args[NarrowArg] = MedVar::makeConst(Args[NarrowArg].ConstVal, 1);
    pushCall(F, "calloc", temp(1), std::move(Args));
    const size_t Sink = pushCall(F, "memcpy", temp(0),
                                 {temp(1), temp(2), MedVar::makeConst(8, 8)});

    const DestObject D =
        resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
    EXPECT_EQ(D.Region, ObjectRegion::Unknown);
    EXPECT_FALSE(D.Capacity.has_value());
  }
}

TEST(ObjectModel, MalformedConstantSSAIsNotAnAllocationSize) {
  for (int Shape = 0; Shape < 6; ++Shape) {
    SCOPED_TRACE(Shape);
    BinaryImage Img;
    Img.Arch = Arch::X64;
    AnalysisInput In;
    In.Img = &Img;

    MedFunc F = newFunc(Arch::X64);
    MedVar Size;
    switch (Shape) {
    case 0:
      Size = temp(10, 9);
      push(F, NdOp::COPY, Size, {MedVar::makeConst(16, 8)});
      break;
    case 1:
      Size = temp(10, 4);
      push(F, NdOp::COPY, Size, {MedVar::makeConst(16, 8)});
      break;
    case 2:
      Size = temp(10, 4);
      push(F, NdOp::INT_ZEXT, Size, {MedVar::makeConst(16, 8)});
      break;
    case 3:
      Size = temp(10, 4);
      push(F, NdOp::INT_SEXT, Size, {MedVar::makeConst(16, 8)});
      break;
    case 4:
      Size = temp(10, 8);
      push(F, NdOp::SUBBYTES, Size,
           {MedVar::makeConst(16, 4), MedVar::makeConst(0, 8)});
      break;
    case 5:
      Size = temp(10, 8);
      push(F, NdOp::INT_ADD, Size,
           {MedVar::makeConst(8, 4), MedVar::makeConst(8, 8)});
      break;
    default:
      FAIL() << "unhandled malformed constant shape";
    }

    pushCall(F, "malloc", temp(1), {Size});
    const size_t Sink = pushCall(F, "memcpy", temp(0),
                                 {temp(1), temp(2), MedVar::makeConst(1, 8)});

    const DestObject D =
        resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
    EXPECT_EQ(D.Region, ObjectRegion::Unknown);
    EXPECT_FALSE(D.Capacity.has_value());
  }
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
  EXPECT_EQ(D.Precision, CapacityPrecision::StorageExact);
  EXPECT_FALSE(D.CapacityExact);
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

TEST(ObjectModel, EntrySelfCopyAuthenticatesIncomingStackPointer) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  F.Blocks[0].StartAddr = F.Entry;
  push(F, NdOp::COPY, mkReg(kSP, 0), {mkReg(kSP, 0)});
  push(F, NdOp::INT_SUB, temp(10), {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  const size_t Sink = pushCall(F, "memcpy", temp(0),
                               {temp(10), temp(2), MedVar::makeConst(8, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Stack);
  EXPECT_EQ(D.StackOffset, -0x20);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 0x20u);
}

TEST(ObjectModel, EntrySelfCopyAuthenticationRejectsStructuralAmbiguity) {
  enum class Case { AfterNonCopy, NonEntryBlock, DuplicateEntryAddress };
  for (const Case Kind :
       {Case::AfterNonCopy, Case::NonEntryBlock, Case::DuplicateEntryAddress}) {
    SCOPED_TRACE(static_cast<unsigned>(Kind));
    BinaryImage Img;
    Img.Arch = Arch::X64;
    AnalysisInput In;
    In.Img = &Img;
    In.StackPointerReg = kSP;
    In.StackRegsKnown = true;

    MedFunc F = newFunc(Arch::X64);
    F.Blocks[0].StartAddr = F.Entry;
    size_t BodyIndex = 0;
    if (Kind != Case::AfterNonCopy) {
      MedBlock Other;
      Other.Id = 1;
      Other.StartAddr =
          Kind == Case::DuplicateEntryAddress ? F.Entry : F.Entry + 0x10;
      F.Blocks.push_back(std::move(Other));
      if (Kind == Case::NonEntryBlock)
        BodyIndex = 1;
    }

    MedBlock &Body = F.Blocks[BodyIndex];
    if (Kind == Case::AfterNonCopy) {
      push(Body, NdOp::INT_XOR, temp(99),
           {MedVar::makeConst(0, 8), MedVar::makeConst(0, 8)});
    }

    push(Body, NdOp::COPY, mkReg(kSP, 0), {mkReg(kSP, 0)});
    push(Body, NdOp::INT_SUB, temp(10),
         {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
    const size_t Sink = pushCall(F, Body, "memcpy", temp(0),
                                 {temp(10), temp(2), MedVar::makeConst(8, 8)});

    const DestObject D =
        resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
    EXPECT_EQ(D.Region, ObjectRegion::Unknown);
    EXPECT_FALSE(D.Capacity.has_value());
  }
}

TEST(ObjectModel, StackPointerPhiWithDifferentOffsetsCannotPublishCapacity) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  F.Blocks.resize(4);
  for (int I = 0; I < 4; ++I)
    F.Blocks[I].Id = I;
  F.Blocks[0].Succs = {1, 2};
  F.Blocks[1].Preds = {0};
  F.Blocks[1].Succs = {3};
  F.Blocks[2].Preds = {0};
  F.Blocks[2].Succs = {3};
  F.Blocks[3].Preds = {1, 2};

  push(F.Blocks[1], NdOp::INT_SUB, temp(10),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  push(F.Blocks[2], NdOp::INT_SUB, temp(11),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  PhiNode Phi;
  Phi.Output = mkReg(kSP, 2);
  Phi.Args = {{1, temp(10)}, {2, temp(11)}};
  F.Blocks[3].Phis.push_back(Phi);
  push(F.Blocks[3], NdOp::INT_SUB, temp(12),
       {Phi.Output, MedVar::makeConst(0x10, 8)});
  const size_t Sink = pushCall(F, F.Blocks[3], "memcpy", temp(0),
                               {temp(12), temp(2), MedVar::makeConst(8, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, CompleteStackPointerPhiWithEqualOffsetsPublishesCapacity) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  F.Blocks.resize(4);
  for (int I = 0; I < 4; ++I)
    F.Blocks[I].Id = I;
  F.Blocks[0].Succs = {1, 2};
  F.Blocks[1].Preds = {0};
  F.Blocks[1].Succs = {3};
  F.Blocks[2].Preds = {0};
  F.Blocks[2].Succs = {3};
  F.Blocks[3].Preds = {1, 2};
  push(F.Blocks[1], NdOp::INT_SUB, temp(10),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  push(F.Blocks[2], NdOp::INT_SUB, temp(11),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  PhiNode Phi;
  Phi.Output = mkReg(kSP, 2);
  Phi.Args = {{1, temp(10)}, {2, temp(11)}};
  F.Blocks[3].Phis.push_back(Phi);
  push(F.Blocks[3], NdOp::INT_SUB, temp(12),
       {Phi.Output, MedVar::makeConst(0x10, 8)});
  const size_t Sink = pushCall(F, F.Blocks[3], "memcpy", temp(0),
                               {temp(12), temp(2), MedVar::makeConst(8, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Stack);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 0x30u);
  EXPECT_EQ(D.Precision, CapacityPrecision::ContainerUpperBound);
}

TEST(ObjectModel, StackPointerPhiRequiresCompleteNormalPredecessorGraph) {
  constexpr std::array<const char *, 9> Scenarios = {
      "missing-pred",   "forged-pred",     "duplicate-pred",
      "missing-arg",    "forged-arg",      "duplicate-arg",
      "duplicate-edge", "duplicate-block", "non-stack-arm"};
  for (size_t Scenario = 0; Scenario < Scenarios.size(); ++Scenario) {
    SCOPED_TRACE(Scenarios[Scenario]);
    BinaryImage Img;
    Img.Arch = Arch::X64;
    Img.Bits = Bitness::Bits64;
    AnalysisInput In;
    In.Img = &Img;
    In.StackPointerReg = kSP;
    In.StackRegsKnown = true;

    MedFunc F = newFunc(Arch::X64);
    F.Blocks.resize(Scenario == 7 ? 5 : 4);
    for (int I = 0; I < static_cast<int>(F.Blocks.size()); ++I)
      F.Blocks[I].Id = I;
    if (Scenario == 7)
      F.Blocks[4].Id = 2;
    F.Blocks[0].Succs = {1, 2};
    F.Blocks[1].Preds = {0};
    F.Blocks[1].Succs =
        Scenario == 6 ? std::vector<int>{3, 3} : std::vector<int>{3};
    F.Blocks[2].Preds = {0};
    F.Blocks[2].Succs = {3};
    F.Blocks[3].Preds = {1, 2};
    if (Scenario == 0)
      F.Blocks[3].Preds = {1};
    else if (Scenario == 1)
      F.Blocks[3].Preds = {1, 9};
    else if (Scenario == 2)
      F.Blocks[3].Preds = {1, 2, 2};

    push(F.Blocks[1], NdOp::INT_SUB, temp(10),
         {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
    push(F.Blocks[2], NdOp::INT_SUB, temp(11),
         {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
    PhiNode Phi;
    Phi.Output = mkReg(kSP, 2);
    Phi.Args = {{1, temp(10)}, {2, temp(11)}};
    if (Scenario == 3)
      Phi.Args.pop_back();
    else if (Scenario == 4)
      Phi.Args[1].first = 9;
    else if (Scenario == 5)
      Phi.Args[1].first = 1;
    else if (Scenario == 8)
      Phi.Args[1].second = temp(99);
    F.Blocks[3].Phis.push_back(Phi);
    push(F.Blocks[3], NdOp::INT_SUB, temp(12),
         {Phi.Output, MedVar::makeConst(0x10, 8)});
    const size_t Sink = pushCall(F, F.Blocks[3], "memcpy", temp(0),
                                 {temp(12), temp(2), MedVar::makeConst(8, 8)});

    const DestObject D =
        resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
    EXPECT_EQ(D.Region, ObjectRegion::Unknown);
    EXPECT_FALSE(D.Capacity.has_value());
  }
}

TEST(ObjectModel, UndefinedNonLiveInStackPointerVersionHasNoCapacity) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;
  MedFunc F = newFunc(Arch::X64);
  push(F, NdOp::INT_SUB, temp(10), {mkReg(kSP, 9), MedVar::makeConst(0x20, 8)});
  const size_t Sink = pushCall(F, "memcpy", temp(0),
                               {temp(10), temp(2), MedVar::makeConst(8, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, ZeroWidthStackPointerPhiCannotAuthenticateALiveIn) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;
  MedFunc F = newFunc(Arch::X64);
  PhiNode Phi;
  Phi.Output = mkReg(kSP, 0, 0);
  Phi.Args = {{9, MedVar::makeConst(0, 0)}};
  F.Blocks[0].Phis.push_back(Phi);
  push(F, NdOp::INT_SUB, temp(10), {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  const size_t Sink = pushCall(F, "memcpy", temp(0),
                               {temp(10), temp(2), MedVar::makeConst(8, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, ZeroWidthStackPointerOpCannotAuthenticateALiveIn) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;
  MedFunc F = newFunc(Arch::X64);
  push(F, NdOp::INT_ADD, mkReg(kSP, 0, 0),
       {MedVar::makeConst(0, 0), MedVar::makeConst(0, 0)});
  push(F, NdOp::INT_SUB, temp(10), {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  const size_t Sink = pushCall(F, "memcpy", temp(0),
                               {temp(10), temp(2), MedVar::makeConst(8, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, CallClobberedStackPointerCannotAuthenticateALiveIn) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;
  MedFunc F = newFunc(Arch::X64);
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.CallSiteId = 1;
  Call.addInput(MedVar::makeConst(0x9000, 8));
  F.Blocks[0].Ops.push_back(Call);
  MedCallInfo CallInfo;
  CallInfo.BlockId = 0;
  CallInfo.OpIdx = 0;
  CallInfo.TargetName = "opaque_external";
  F.CallInfos.push_back(CallInfo);
  MedCallClobber Clobber;
  Clobber.Value = mkReg(kSP, 0);
  Clobber.CallSiteId = 1;
  F.CallClobbers.push_back(Clobber);
  push(F, NdOp::INT_SUB, temp(10), {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  const size_t Sink = pushCall(F, "memcpy", temp(0),
                               {temp(10), temp(2), MedVar::makeConst(8, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, DuplicateStackAddressDefinitionsHaveNoCapacity) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;
  MedFunc F = newFunc(Arch::X64);
  push(F, NdOp::INT_SUB, temp(10), {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  push(F, NdOp::INT_SUB, temp(10), {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  const size_t Sink = pushCall(F, "memcpy", temp(0),
                               {temp(10), temp(2), MedVar::makeConst(8, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, PhiAndOpCannotBothDefineAStackCapacity) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;
  MedFunc F = newFunc(Arch::X64);
  F.Blocks.resize(4);
  for (int I = 0; I < 4; ++I)
    F.Blocks[I].Id = I;
  F.Blocks[0].Succs = {1, 2};
  F.Blocks[1].Preds = {0};
  F.Blocks[1].Succs = {3};
  F.Blocks[2].Preds = {0};
  F.Blocks[2].Succs = {3};
  F.Blocks[3].Preds = {1, 2};
  push(F.Blocks[1], NdOp::INT_SUB, temp(10),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  push(F.Blocks[2], NdOp::INT_SUB, temp(11),
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  PhiNode Phi;
  Phi.Output = temp(12);
  Phi.Args = {{1, temp(10)}, {2, temp(11)}};
  F.Blocks[3].Phis.push_back(Phi);
  push(F.Blocks[3], NdOp::INT_SUB, Phi.Output,
       {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  const size_t Sink = pushCall(F, F.Blocks[3], "memcpy", temp(0),
                               {Phi.Output, temp(2), MedVar::makeConst(8, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Unknown);
  EXPECT_FALSE(D.Capacity.has_value());
}

TEST(ObjectModel, RelocationAddressCannotBecomeAStackOffset) {
  for (const Arch A : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM}) {
    SCOPED_TRACE(static_cast<int>(A));
    const uint16_t PointerSize = (A == Arch::X86 || A == Arch::ARM) ? 4 : 8;
    BinaryImage Img;
    Img.Arch = A;
    AnalysisInput In;
    In.Img = &Img;
    In.StackPointerReg = kSP;
    In.StackRegsKnown = true;

    MedFunc F = newFunc(A);
    push(F, NdOp::INT_SUB, mkReg(kSP, 1, PointerSize),
         {mkReg(kSP, 0, PointerSize), MedVar::makeConst(0x30, PointerSize)});
    push(F, NdOp::INT_ADD, temp(10, PointerSize),
         {mkReg(kSP, 1, PointerSize),
          MedVar::makeConst(8, PointerSize,
                            ConstantAddressProvenance::DataAddress, 0x4000)});
    const size_t Sink = pushCall(F, "strcpy", temp(0, PointerSize),
                                 {temp(10, PointerSize), temp(2, PointerSize)});

    const DestObject D =
        resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
    EXPECT_EQ(D.Region, ObjectRegion::Unknown);
    EXPECT_FALSE(D.Capacity.has_value());
  }
}

TEST(ObjectModel, WidthChangingStackArithmeticDoesNotPreserveCapacity) {
  for (const NdOp Opcode :
       {NdOp::COPY, NdOp::CAST, NdOp::INT_ADD, NdOp::INT_SUB}) {
    SCOPED_TRACE(static_cast<int>(Opcode));
    BinaryImage Img;
    Img.Arch = Arch::X64;
    AnalysisInput In;
    In.Img = &Img;
    In.StackPointerReg = kSP;
    In.StackRegsKnown = true;

    MedFunc F = newFunc(Arch::X64);
    push(F, NdOp::INT_SUB, mkReg(kSP, 1),
         {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
    if (Opcode == NdOp::COPY || Opcode == NdOp::CAST)
      push(F, Opcode, temp(10, 4), {mkReg(kSP, 1)});
    else
      push(F, Opcode, temp(10, 4), {mkReg(kSP, 1), MedVar::makeConst(0, 8)});
    const size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10, 4), temp(2)});

    const DestObject D =
        resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
    EXPECT_EQ(D.Region, ObjectRegion::Unknown);
    EXPECT_FALSE(D.Capacity.has_value());
  }
}

TEST(ObjectModel, NarrowEncodedStackImmediatePreservesPointerWidth) {
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::AArch64);
  push(F, NdOp::INT_SUB, mkReg(kSP, 1, 8),
       {mkReg(kSP, 0, 8), MedVar::makeConst(0x30, 4)});
  push(F, NdOp::INT_ADD, temp(10, 8),
       {mkReg(kSP, 1, 8), MedVar::makeConst(8, 4)});
  const size_t Sink =
      pushCall(F, "strcpy", temp(0, 8), {temp(10, 8), temp(2, 8)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Stack);
  EXPECT_EQ(D.StackOffset, -0x28);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 0x28u);
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
  bool hasAuthenticatedObjectExtents() const override { return true; }
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
  TypeDebug Dbg(0x100, 0, Outer);
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
  TypeDebug Dbg(0x100, 0, Cycle);
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
  bool hasAuthenticatedObjectExtents() const override { return true; }
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
  bool hasAuthenticatedObjectExtents() const override { return true; }
  bool hasInfo() const override { return true; }

private:
  va_t Func;
  int64_t Off;
  TypeRef Type;
};

class OccurrenceArrayDebug : public NullDebugContext {
public:
  OccurrenceArrayDebug(va_t Func, va_t UsePC, int64_t Base, uint32_t Count)
      : Func(Func), UsePC(UsePC), Base(Base) {
    Type = std::make_shared<NdType>();
    Type->Kind = NdTypeKind::Array;
    Type->ArrayCount = Count;
    Type->ElemType = NdType::makeInt(1, false);
    Type->Size = static_cast<uint16_t>(Count);
  }

  VariableExtentLookup resolveVariableAt(va_t F, va_t PC,
                                         int64_t Offset) const override {
    if (F != Func || PC != UsePC || Offset < Base ||
        static_cast<uint64_t>(Offset) - static_cast<uint64_t>(Base) >
            Type->ArrayCount)
      return VariableExtentLookup::notFound();
    VariableSym V;
    V.Name = "buf";
    V.Type = Type;
    V.StackOffset = Base;
    return VariableExtentLookup::unique(std::move(V));
  }
  bool hasAuthenticatedObjectExtents() const override { return true; }
  bool hasInfo() const override { return true; }

private:
  va_t Func;
  va_t UsePC;
  int64_t Base;
  TypeRef Type;
};

VariableExtentLookup uniqueLocal(const char *Name, int64_t Base, TypeRef Type) {
  VariableSym Variable;
  Variable.Name = Name;
  Variable.StackOffset = Base;
  Variable.Type = std::move(Type);
  return VariableExtentLookup::unique(std::move(Variable));
}

class CoordinateExtentDebug : public NullDebugContext {
public:
  CoordinateExtentDebug(va_t Func, va_t UsePC, VariableExtentLookup FrameLookup,
                        VariableExtentLookup StackPointerLookup,
                        bool Authenticated = true,
                        VariableExtentLookup FramePointerLookup =
                            VariableExtentLookup::notFound())
      : Func(Func), UsePC(UsePC), FrameLookup(std::move(FrameLookup)),
        StackPointerLookup(std::move(StackPointerLookup)),
        FramePointerLookup(std::move(FramePointerLookup)),
        Authenticated(Authenticated) {}

  VariableExtentLookup resolveVariableAt(va_t F, va_t PC,
                                         int64_t) const override {
    if (F != Func || PC != UsePC)
      return VariableExtentLookup::notFound();
    return FrameLookup;
  }

  VariableExtentLookup resolveStackPointerVariableAt(va_t F, va_t PC,
                                                     int64_t) const override {
    if (F != Func || PC != UsePC)
      return VariableExtentLookup::notFound();
    return StackPointerLookup;
  }

  VariableExtentLookup resolveFramePointerVariableAt(va_t F, va_t PC,
                                                     int64_t) const override {
    if (F != Func || PC != UsePC)
      return VariableExtentLookup::notFound();
    return FramePointerLookup;
  }

  bool hasAuthenticatedObjectExtents() const override { return Authenticated; }
  bool hasInfo() const override { return true; }

private:
  va_t Func;
  va_t UsePC;
  VariableExtentLookup FrameLookup;
  VariableExtentLookup StackPointerLookup;
  VariableExtentLookup FramePointerLookup;
  bool Authenticated;
};

TEST(ObjectModel, DebugArrayPreferredOverFrameBound) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  ArrayDebug Dbg(0x100, -0x30, 16);
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

TEST(ObjectModel, DebugArrayInteriorUsesOccurrenceAndRemainingExtent) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  OccurrenceArrayDebug Dbg(0x100, 0x155, -0x30, 8);
  AnalysisInput In;
  In.Img = &Img;
  In.Dbg = &Dbg;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  push(F, NdOp::INT_SUB, temp(10), {mkReg(kSP, 0), MedVar::makeConst(0x25, 8)});
  const size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});
  F.Blocks[0].Ops.back().Addr = 0x155;

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 5u);
  EXPECT_TRUE(D.CapacityExact);
  EXPECT_EQ(D.Detail, "declared array");
}

TEST(ObjectModel, UnauthenticatedDebugCannotPublishExactObjectExtent) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  CoordinateExtentDebug Dbg(0x100, 0x155,
                            uniqueLocal("buf", -0x28, byteArray(8)),
                            VariableExtentLookup::notFound(), false);
  AnalysisInput In;
  In.Img = &Img;
  In.Dbg = &Dbg;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  push(F, NdOp::INT_SUB, temp(10), {mkReg(kSP, 0), MedVar::makeConst(0x25, 8)});
  const size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});
  F.Blocks[0].Ops.back().Addr = 0x155;

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 0x25u);
  EXPECT_FALSE(D.CapacityExact);
  EXPECT_EQ(D.Detail, "stack frame bound");
}

TEST(ObjectModel, AmbiguousDebugExtentCannotFallThroughToTypedLocal) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  CoordinateExtentDebug Dbg(0x100, 0x155, VariableExtentLookup::ambiguous(),
                            VariableExtentLookup::notFound());
  AnalysisInput In;
  In.Img = &Img;
  In.Dbg = &Dbg;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  addTypedLocal(F, "buf", -0x28, byteArray(8));
  push(F, NdOp::INT_SUB, temp(10), {mkReg(kSP, 0), MedVar::makeConst(0x25, 8)});
  const size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});
  F.Blocks[0].Ops.back().Addr = 0x155;

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 0x25u);
  EXPECT_FALSE(D.CapacityExact);
  EXPECT_EQ(D.Detail, "stack frame bound");
}

TEST(ObjectModel, AuthenticatedNonBufferIsAuthoritativeAgainstTypedArray) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  CoordinateExtentDebug Dbg(
      0x100, 0x155, uniqueLocal("scalar", -0x30, NdType::makeInt(8, false)),
      VariableExtentLookup::notFound());
  AnalysisInput In;
  In.Img = &Img;
  In.Dbg = &Dbg;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  addTypedLocal(F, "buf", -0x28, byteArray(8));
  push(F, NdOp::INT_SUB, temp(10), {mkReg(kSP, 0), MedVar::makeConst(0x25, 8)});
  const size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});
  F.Blocks[0].Ops.back().Addr = 0x155;

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 0x25u);
  EXPECT_FALSE(D.CapacityExact);
}

TEST(ObjectModel, DebugCoordinateSystemsMustAgreeOnCanonicalExtent) {
  for (const bool Agree : {true, false}) {
    SCOPED_TRACE(Agree);
    BinaryImage Img;
    Img.Arch = Arch::X64;
    CoordinateExtentDebug Dbg(
        0x100, 0x155, uniqueLocal("cfa_buf", -0x30, byteArray(16)),
        uniqueLocal("sp_buf", Agree ? 0x8 : 0x5, byteArray(16)));
    AnalysisInput In;
    In.Img = &Img;
    In.Dbg = &Dbg;
    In.StackPointerReg = kSP;
    In.StackRegsKnown = true;

    MedFunc F = newFunc(Arch::X64);
    F.FrameSize = 0x30;
    push(F, NdOp::INT_SUB, temp(10),
         {mkReg(kSP, 0), MedVar::makeConst(0x25, 8)});
    const size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});
    F.Blocks[0].Ops.back().Addr = 0x155;

    const DestObject D =
        resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
    ASSERT_TRUE(D.Capacity.has_value());
    if (Agree) {
      EXPECT_EQ(*D.Capacity, 13u);
      EXPECT_TRUE(D.CapacityExact);
      EXPECT_EQ(D.Detail, "declared array");
    } else {
      EXPECT_EQ(*D.Capacity, 0x25u);
      EXPECT_FALSE(D.CapacityExact);
      EXPECT_EQ(D.Detail, "stack frame bound");
    }
  }
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
  CoordinateExtentDebug Dbg(0x100, 0, VariableExtentLookup::notFound(),
                            VariableExtentLookup::notFound(), true,
                            uniqueLocal("fp_buf", -0x40, byteArray(64)));
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

TEST(ObjectModel, ConflictingFrameBasesDoNotSelectDebugCapacityByOrder) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  ArrayDebug Dbg(0x100, -0x20, 64);
  AnalysisInput In;
  In.Img = &Img;
  In.Dbg = &Dbg;
  In.StackPointerReg = kSP;
  In.FramePointerReg = kFP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  push(F, NdOp::INT_SUB, temp(20), {mkReg(kSP, 0), MedVar::makeConst(0x8, 8)});
  push(F, NdOp::COPY, mkReg(kFP, 1), {temp(20)});
  push(F, NdOp::INT_SUB, temp(21), {mkReg(kSP, 0), MedVar::makeConst(0x18, 8)});
  push(F, NdOp::COPY, mkReg(kFP, 2), {temp(21)});
  push(F, NdOp::INT_SUB, temp(10), {mkReg(kSP, 0), MedVar::makeConst(0x28, 8)});
  const size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 0x28u);
  EXPECT_FALSE(D.CapacityExact);
  EXPECT_EQ(D.Detail, "stack frame bound");
}

TEST(ObjectModel, TypedLocalArrayWithoutEvidenceIsOnlyAFrameBound) {
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
  EXPECT_EQ(*D.Capacity, 0x28u);
  EXPECT_EQ(D.Precision, CapacityPrecision::ContainerUpperBound);
  EXPECT_FALSE(D.CapacityExact);
}

TEST(ObjectModel, TypedLocalArrayInteriorWithoutEvidenceIsOnlyAFrameBound) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  MedTypedLocal L;
  L.Name = "buf";
  L.StackOff = -0x28;
  L.Type = std::make_shared<NdType>();
  L.Type->Kind = NdTypeKind::Array;
  L.Type->ArrayCount = 8;
  L.Type->ElemType = NdType::makeInt(1, false);
  L.Type->Size = 8;
  F.TypedLocals.push_back(std::move(L));
  push(F, NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  push(F, NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0xb, 8)});
  const size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Stack);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 0x25u);
  EXPECT_EQ(D.Precision, CapacityPrecision::ContainerUpperBound);
  EXPECT_FALSE(D.CapacityExact);
  EXPECT_EQ(D.Detail, "stack frame bound");
}

TEST(ObjectModel, AdjacentArrayAndScalarMakeSharedBoundaryAmbiguous) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  addTypedLocal(F, "array", -0x28, byteArray(8));
  addTypedLocal(F, "scalar", -0x20, NdType::makeInt(8, false));
  push(F, NdOp::INT_SUB, temp(10), {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  const size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Stack);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 0x20u);
  EXPECT_EQ(D.Precision, CapacityPrecision::ContainerUpperBound);
  EXPECT_FALSE(D.CapacityExact);
  EXPECT_EQ(D.Detail, "stack frame bound");
}

TEST(ObjectModel, OverlappingDifferentTypedLocalKindsFailClosed) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  addTypedLocal(F, "array", -0x28, byteArray(16));
  auto Record = std::make_shared<NdType>();
  Record->Kind = NdTypeKind::Struct;
  Record->Size = 8;
  addTypedLocal(F, "record", -0x24, std::move(Record));
  push(F, NdOp::INT_SUB, temp(10), {mkReg(kSP, 0), MedVar::makeConst(0x22, 8)});
  const size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Stack);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 0x22u);
  EXPECT_EQ(D.Precision, CapacityPrecision::ContainerUpperBound);
  EXPECT_FALSE(D.CapacityExact);
  EXPECT_EQ(D.Detail, "stack frame bound");
}

TEST(ObjectModel, IdenticalTypedArrayAliasesDoNotCreateAuthenticatedEvidence) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  addTypedLocal(F, "first", -0x28, byteArray(8));
  addTypedLocal(F, "alias", -0x28, byteArray(8));
  push(F, NdOp::INT_SUB, temp(10), {mkReg(kSP, 0), MedVar::makeConst(0x25, 8)});
  const size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Stack);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 0x25u);
  EXPECT_EQ(D.Precision, CapacityPrecision::ContainerUpperBound);
  EXPECT_FALSE(D.CapacityExact);
  EXPECT_EQ(D.Detail, "stack frame bound");
}

TEST(ObjectModel, UniqueTypedArrayOnePastStillNeedsAuthenticatedEvidence) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  In.StackPointerReg = kSP;
  In.StackRegsKnown = true;

  MedFunc F = newFunc(Arch::X64);
  addTypedLocal(F, "array", -0x28, byteArray(8));
  push(F, NdOp::INT_SUB, temp(10), {mkReg(kSP, 0), MedVar::makeConst(0x20, 8)});
  const size_t Sink = pushCall(F, "strcpy", temp(0), {temp(10), temp(2)});

  const DestObject D =
      resolveDestination(In, SinkCatalog::defaults(), F, Sink, 0);
  EXPECT_EQ(D.Region, ObjectRegion::Stack);
  ASSERT_TRUE(D.Capacity.has_value());
  EXPECT_EQ(*D.Capacity, 0x20u);
  EXPECT_EQ(D.Precision, CapacityPrecision::ContainerUpperBound);
  EXPECT_FALSE(D.CapacityExact);
  EXPECT_EQ(D.Detail, "stack frame bound");
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
