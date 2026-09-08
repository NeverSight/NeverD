#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCBlockCallHints.h"
#include "neverd/loader/ObjC/ObjCBlocks.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"
#include "neverd/loader/ObjC/ObjCMethods.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"

using namespace neverd;

namespace {
struct BlockFixture {
  BinaryImage Image;
  static constexpr va_t Literal = 0x2100;
  static constexpr va_t Descriptor = 0x2200;
  static constexpr va_t Signature = 0x2300;
  static constexpr va_t Layout = 0x2400;
  static constexpr va_t Invoke = 0x1100;

  BlockFixture() {
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::MachO;
    Segment Segment;
    Segment.VA = 0x1000;
    Segment.Size = Segment.FileSz = 0x1000;
    Segment.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Segment.Data.resize(0x1000);
    Image.Segments.push_back(std::move(Segment));
    neverd::Segment DataSegment;
    DataSegment.VA = 0x2000;
    DataSegment.Size = DataSegment.FileSz = 0x3000;
    DataSegment.FileOff = 0x1000;
    DataSegment.Flags = SegmentFlags::Readable;
    DataSegment.Data.resize(0x3000);
    Image.Segments.push_back(std::move(DataSegment));
    Section Text;
    Text.VA = 0x1000;
    Text.Size = Text.FileSz = 0x1000;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    Image.Sections.push_back(Text);
    Section Data;
    Data.VA = 0x2000;
    Data.Size = Data.FileSz = 0x3000;
    Data.FileOff = 0x1000;
    Data.Flags = SegmentFlags::Readable;
    Image.Sections.push_back(Data);
    Image.ImportPtrSlots[Literal] = "__NSConcreteGlobalBlock";
    put32(Literal + 8, 0x50000000);
    put64(Literal + 16, Invoke);
    put64(Literal + 24, Descriptor);
    put64(Descriptor, 0);
    put64(Descriptor + 8, 32);
    put64(Descriptor + 16, Signature);
    string(Signature, "i12@?0i8");
  }

  void put32(va_t Address, uint32_t Value) {
    llvm::support::endian::write32le(
        Image.Segments[1].Data.data() + Address - 0x2000, Value);
  }
  void put64(va_t Address, uint64_t Value) {
    llvm::support::endian::write64le(
        Image.Segments[1].Data.data() + Address - 0x2000, Value);
  }
  void string(va_t Address, llvm::StringRef Value) {
    auto *Start = Image.Segments[1].Data.data() + Address - 0x2000;
    std::copy(Value.bytes_begin(), Value.bytes_end(), Start);
    Start[Value.size()] = 0;
  }
  std::optional<ObjCBlockLiteral> read(std::string &Error) {
    return readObjCBlockLiteral(Image, Literal, Error);
  }
};

TEST(ObjCBlocks, GlobalLiteralIdentityIsNotItsLoadedIsaValue) {
  BlockFixture Fixture;
  std::string Error;
  auto Block = Fixture.read(Error);
  ASSERT_TRUE(Block) << Error;
  EXPECT_EQ(Block->Address, BlockFixture::Literal);
  EXPECT_EQ(Block->InvokeEntry, BlockFixture::Invoke);
  EXPECT_EQ(Block->Descriptor.LiteralSize, 32U);
  ASSERT_TRUE(Block->Descriptor.InvokeTypeHint);
  EXPECT_EQ(Block->Descriptor.Signature, "i12@?0i8");
  EXPECT_EQ(Block->Descriptor.InvokeTypeHint->Origin,
            SourceFunctionTypeHint::OriginKind::BlockRuntime);
  ASSERT_EQ(Block->Descriptor.InvokeTypeHint->Parameters.size(), 2U);
  EXPECT_EQ(Block->Descriptor.InvokeTypeHint->Parameters[0].Type->Kind,
            NdTypeKind::Ptr);
  EXPECT_EQ(
      Block->Descriptor.InvokeTypeHint->Parameters[1].Location.RegisterOffset,
      getTargetRegInfo(Arch::AArch64).IntParamRegs[1]);
  EXPECT_TRUE(Block->Descriptor.Captures.empty());
  EXPECT_TRUE(Block->Descriptor.Limitations.empty());
  EXPECT_EQ(findObjCGlobalBlocks(Fixture.Image).size(), 1U);
  // No block identity may be inherited by another equal numeric field.
  EXPECT_FALSE(
      readObjCBlockLiteral(Fixture.Image, BlockFixture::Descriptor, Error));
}

TEST(ObjCBlocks, RequiresImportSlotIdentityRatherThanPlausibleHeaderOrName) {
  for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
    BlockFixture Fixture;
    if (Mutation == 0)
      Fixture.Image.ImportPtrSlots.clear();
    if (Mutation == 1)
      Fixture.Image.ImportPtrSlots[BlockFixture::Literal] =
          "__NSConcreteGlobalBlockSuffix";
    if (Mutation == 2)
      Fixture.Image.ConflictingImportStorageSlots.insert(BlockFixture::Literal);
    std::string Error;
    EXPECT_FALSE(Fixture.read(Error)) << Mutation;
  }
}

TEST(ObjCBlocks,
     LiteralDescriptorSignatureTakesPriorityOverObservedReturnWidth) {
  BlockFixture Fixture;
  const auto &TRI = getTargetRegInfo(Fixture.Image.Arch);
  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Blocks.emplace_back();
  auto &B = Low.Blocks[0];
  B.Id = 0;
  auto Add = [&](NdOp Opcode, NdVar Output,
                 std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Opcode = Opcode;
    Op.Output = Output;
    Op.Addr = 0x1000 + B.Ops.size() * 4;
    for (const auto &Input : Inputs)
      Op.addInput(Input);
    B.Ops.push_back(Op);
  };
  Add(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[0], 8),
      {NdVar::cst(BlockFixture::Literal, 8)});
  Add(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[1], 8), {NdVar::cst(42, 8)});
  Add(NdOp::LOAD, NdVar::reg(64, 8),
      {NdVar::cst(BlockFixture::Literal + 16, 8)});
  Add(NdOp::INDIR_CALL, NdVar::reg(TRI.IntReturnReg, 8), {NdVar::reg(64, 8)});
  Add(NdOp::RETURN, {}, {NdVar::reg(TRI.IntReturnReg, 8)});
  auto Hints = buildObjCBlockCallHints(Fixture.Image, Low);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Signature = Hints.begin()->second.Signature;
  EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::BlockRuntime);
  EXPECT_EQ(Signature.ReturnType->Size, 4U);
  EXPECT_TRUE(Signature.ReturnType->IsSigned);
  EXPECT_EQ(Signature.Parameters[1].Type->Size, 4U);
}

TEST(ObjCBlocks, ChainedPointerSlotsRequireIndividualResolution) {
  for (va_t Missing : {BlockFixture::Literal + 16, BlockFixture::Literal + 24,
                       BlockFixture::Descriptor + 16}) {
    BlockFixture Fixture;
    Fixture.Image.MachOHasChainedFixups = true;
    Fixture.Image.MachOResolvedChainedPointerSlots = {
        BlockFixture::Literal + 16, BlockFixture::Literal + 24,
        BlockFixture::Descriptor + 16};
    std::string Error;
    ASSERT_TRUE(Fixture.read(Error)) << Error;
    Fixture.Image.MachOResolvedChainedPointerSlots.erase(Missing);
    EXPECT_FALSE(Fixture.read(Error)) << Missing;
  }
}

TEST(ObjCBlocks, RejectsTruncatedZerofillBadMappingAndInvalidCodePointers) {
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    BlockFixture Fixture;
    if (Mutation == 0)
      Fixture.Image.Sections[1].FileSz = 0x108;
    if (Mutation == 1)
      Fixture.Image.Sections[1].Type = llvm::MachO::S_ZEROFILL;
    if (Mutation == 2)
      ++Fixture.Image.Sections[1].FileOff;
    if (Mutation == 3)
      Fixture.put64(BlockFixture::Literal + 16, BlockFixture::Signature);
    if (Mutation == 4)
      Fixture.put64(BlockFixture::Descriptor + 8, 0x4000);
    std::string Error;
    EXPECT_FALSE(Fixture.read(Error)) << Mutation;
  }
}

TEST(ObjCBlocks, ClassicDescriptorDoesNotReadAbsentSignatureFields) {
  BlockFixture Fixture;
  Fixture.put32(BlockFixture::Literal + 8,
                0x30000000);                // Legacy bit29 is ignored.
  Fixture.Image.Sections[1].FileSz = 0x210; // Exactly the mandatory descriptor.
  std::string Error;
  auto Block = Fixture.read(Error);
  ASSERT_TRUE(Block) << Error;
  EXPECT_FALSE(Block->Descriptor.InvokeTypeHint);
  EXPECT_FALSE(Block->Descriptor.Limitations.empty());
}

TEST(ObjCBlocks, ScalarStackCaptureMetadataDoesNotInventFieldTypeOrValue) {
  BlockFixture Fixture;
  Fixture.put64(BlockFixture::Descriptor + 8, 36);
  Fixture.put64(BlockFixture::Descriptor + 24, BlockFixture::Layout);
  Fixture.string(BlockFixture::Layout, "");
  std::string Error;
  auto Descriptor = readObjCBlockDescriptor(
      Fixture.Image, BlockFixture::Descriptor, 0xc0000000, Error);
  ASSERT_TRUE(Descriptor) << Error;
  ASSERT_TRUE(Descriptor->InvokeTypeHint);
  ASSERT_EQ(Descriptor->Captures.size(), 1U);
  EXPECT_EQ(Descriptor->Captures[0].Offset, 32U);
  EXPECT_EQ(Descriptor->Captures[0].Size, 4U);
  EXPECT_EQ(Descriptor->Captures[0].StorageKind,
            ObjCBlockCaptureRange::Kind::NonObjectBytes);
  EXPECT_EQ(Descriptor->LayoutBytes, std::vector<uint8_t>{0});
}

TEST(ObjCBlocks, CopyDisposeFieldsChangeSignaturePositionAndKeepDependencies) {
  BlockFixture Fixture;
  Fixture.put32(BlockFixture::Literal + 8, 0xd2000000);
  Fixture.put64(BlockFixture::Descriptor + 8, 64);
  Fixture.put64(BlockFixture::Descriptor + 16, 0x1200);
  Fixture.put64(BlockFixture::Descriptor + 24, 0x1210);
  Fixture.put64(BlockFixture::Descriptor + 32, BlockFixture::Signature);
  Fixture.put64(BlockFixture::Descriptor + 40, 0x211);
  std::string Error;
  auto Block = Fixture.read(Error);
  ASSERT_TRUE(Block) << Error;
  EXPECT_EQ(Block->Descriptor.CopyHelper, 0x1200U);
  EXPECT_EQ(Block->Descriptor.DisposeHelper, 0x1210U);
  ASSERT_TRUE(Block->Descriptor.InvokeTypeHint);
  ASSERT_EQ(Block->Descriptor.Captures.size(), 3U);
  EXPECT_EQ(Block->Descriptor.Captures[0].Size, 16U);
  EXPECT_EQ(Block->Descriptor.Captures[1].StorageKind,
            ObjCBlockCaptureRange::Kind::Byref);
  EXPECT_EQ(Block->Descriptor.Captures[2].StorageKind,
            ObjCBlockCaptureRange::Kind::Weak);
  EXPECT_FALSE(Block->Descriptor.Limitations.empty());
}

TEST(ObjCBlocks, ExtendedOwnershipBytecodeIsBoundedAndReportsUnknownOpcodes) {
  BlockFixture Fixture;
  Fixture.put32(BlockFixture::Literal + 8, 0xd0000000);
  Fixture.put64(BlockFixture::Descriptor + 8, 48);
  Fixture.put64(BlockFixture::Descriptor + 24, BlockFixture::Layout);
  Fixture.string(BlockFixture::Layout, llvm::StringRef("\x17\x30", 2));
  std::string Error;
  auto Block = Fixture.read(Error);
  ASSERT_TRUE(Block) << Error;
  ASSERT_EQ(Block->Descriptor.Captures.size(), 2U);
  EXPECT_EQ(Block->Descriptor.Captures[1].Offset, 40U);
  EXPECT_EQ(Block->Descriptor.Captures[1].StorageKind,
            ObjCBlockCaptureRange::Kind::Strong);
  Fixture.string(BlockFixture::Layout, llvm::StringRef("\xf0", 1));
  Block = Fixture.read(Error);
  ASSERT_TRUE(Block);
  EXPECT_FALSE(Block->Descriptor.Limitations.empty());
  EXPECT_EQ(Block->Descriptor.Captures[0].StorageKind,
            ObjCBlockCaptureRange::Kind::Unknown);
}

TEST(ObjCBlocks, InlineLayoutCountsAreScalarsEvenInChainedImages) {
  BlockFixture Fixture;
  Fixture.Image.MachOHasChainedFixups = true;
  Fixture.Image.MachOResolvedChainedPointerSlots = {
      BlockFixture::Literal + 16, BlockFixture::Literal + 24,
      BlockFixture::Descriptor + 16};
  Fixture.put32(BlockFixture::Literal + 8, 0xd0000000);
  Fixture.put64(BlockFixture::Descriptor + 8, 40);
  Fixture.put64(BlockFixture::Descriptor + 24, 0x100);
  std::string Error;
  auto Block = Fixture.read(Error);
  ASSERT_TRUE(Block) << Error;
  EXPECT_TRUE(Block->Descriptor.Limitations.empty());
  ASSERT_EQ(Block->Descriptor.Captures.size(), 1U);
  EXPECT_EQ(Block->Descriptor.Captures[0].StorageKind,
            ObjCBlockCaptureRange::Kind::Strong);
  Fixture.put64(BlockFixture::Descriptor + 24, 0x200);
  Block = Fixture.read(Error);
  ASSERT_TRUE(Block);
  EXPECT_FALSE(Block->Descriptor.Limitations.empty());
}

TEST(ObjCBlocks, MissingStretAndAggregateSignaturesNeverInventInvokePrototype) {
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    BlockFixture Fixture;
    if (Mutation == 0)
      Fixture.put64(BlockFixture::Descriptor + 16, 0);
    if (Mutation == 1)
      Fixture.put32(BlockFixture::Literal + 8, 0x70000000);
    if (Mutation == 2)
      Fixture.string(BlockFixture::Signature, "{Pair=ii}12@?0i8");
    if (Mutation == 3)
      Fixture.string(BlockFixture::Signature, "i16@0:8");
    std::string Error;
    auto Block = Fixture.read(Error);
    ASSERT_TRUE(Block) << Mutation << Error;
    EXPECT_FALSE(Block->Descriptor.InvokeTypeHint) << Mutation;
    EXPECT_FALSE(Block->Descriptor.Limitations.empty());
  }
}

TEST(ObjCBlocks, BlockSignaturesUseOneHiddenArgumentAndIndependentFloatBank) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    std::string Error;
    auto Hint = parseObjCBlockSignature("d24@?0d8q16", Architecture, Error);
    ASSERT_TRUE(Hint) << Error;
    ASSERT_EQ(Hint->Parameters.size(), 3U);
    const auto &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, TRI.IntParamRegs[0]);
    EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, TRI.FPParamRegs[0]);
    EXPECT_EQ(Hint->Parameters[2].Location.RegisterOffset, TRI.IntParamRegs[1]);
    EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, TRI.FPReturnReg);
    EXPECT_FALSE(parseObjCBlockSignature("@?", Architecture, Error));
    EXPECT_FALSE(
        parseObjCBlockSignature("i12@?\"Class\"0i8", Architecture, Error));
  }
}

TEST(ObjCBlocks, PublicScalarParserBoundsOffsetAndConsumesOnlyBlockPointer) {
  size_t Offset = 10;
  EXPECT_FALSE(parseObjCScalarType("i", Offset));
  Offset = 0;
  auto Type = parseObjCScalarType("@?\"Class\"", Offset);
  ASSERT_TRUE(Type);
  EXPECT_EQ(Type->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Offset, 2U);
  Offset = 0;
  EXPECT_FALSE(parseObjCScalarType(std::string(20, '^') + "i", Offset));
}

TEST(ObjCBlocks, MethodBlockParameterDoesNotBecomeAnInvokeSignatureOrReceiver) {
  BlockFixture Fixture;
  Fixture.Image.Sections[1].Size = Fixture.Image.Sections[1].FileSz = 0x600;
  Section Classes;
  Classes.Name = "__objc_classlist";
  Classes.VA = 0x2600;
  Classes.Size = Classes.FileSz = 8;
  Classes.FileOff = 0x1600;
  Classes.Flags = SegmentFlags::Readable;
  Fixture.Image.Sections.push_back(Classes);
  Section Records;
  Records.VA = 0x2608;
  Records.Size = Records.FileSz = 0x29f8;
  Records.FileOff = 0x1608;
  Records.Flags = SegmentFlags::Readable;
  Fixture.Image.Sections.push_back(Records);
  Fixture.put64(0x2600, 0x2700);
  Fixture.put64(0x2720, 0x2800);
  Fixture.put64(0x2818, 0x2900);
  Fixture.put64(0x2820, 0x2a00);
  Fixture.string(0x2900, "AcceptsBlock");
  Fixture.string(0x2920, "accept:");
  Fixture.string(0x2940, "i24@0:8@?16");
  Fixture.put32(0x2a00, 24);
  Fixture.put32(0x2a04, 1);
  Fixture.put64(0x2a08, 0x2920);
  Fixture.put64(0x2a10, 0x2940);
  Fixture.put64(0x2a18, BlockFixture::Invoke);
  parseObjCMethods(Fixture.Image);
  ASSERT_EQ(Fixture.Image.ObjCMethods.size(), 1U);
  const auto &Method = Fixture.Image.ObjCMethods[0];
  ASSERT_TRUE(Method.TypeHint);
  EXPECT_EQ(Method.TypeEncoding, "i24@0:8@?16");
  EXPECT_EQ(Method.TypeHint->Parameters[2].Type->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Method.TypeHint->Origin,
            SourceFunctionTypeHint::OriginKind::ObjCRuntime);
  // A block parameter proves its pointer ABI, not any callable block metadata.
  EXPECT_EQ(findObjCGlobalBlocks(Fixture.Image).size(), 1U);
  Fixture.string(0x2920, "answer");
  Fixture.string(0x2940, "i16@?0:8");
  parseObjCMethods(Fixture.Image);
  ASSERT_EQ(Fixture.Image.ObjCMethods.size(), 1U);
  EXPECT_FALSE(Fixture.Image.ObjCMethods[0].TypeHint);
  EXPECT_EQ(Fixture.Image.ObjCMethods[0].Status, "unsupported_encoding");
}
} // namespace
