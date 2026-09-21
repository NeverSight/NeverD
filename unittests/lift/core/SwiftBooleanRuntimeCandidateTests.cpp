#include "../../../lib/loader/Swift/SwiftBooleanRuntimeCandidate.h"
#include "gtest/gtest.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"

#include "llvm/Support/Endian.h"

#include <type_traits>

using namespace neverd;
namespace {
constexpr va_t Slot = 0x2080;
BinaryImage
candidateImage(llvm::StringRef Name = SwiftBooleanComparisonImport,
               llvm::StringRef Provider = SwiftBooleanComparisonProvider) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Image.DynInfo.NeededLibs = {Provider.str()};
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = Data.FileSz = 0x100;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(0x100);
  Image.Segments.push_back(std::move(Data));
  Image.ImportPtrSlots[Slot] = Name.str();
  EXPECT_TRUE(Image.recordDyldBindSlot(Slot, Name, 0, Provider, false));
  return Image;
}

void addVeneer(BinaryImage &Image, bool Selector = false) {
  Segment Text;
  Text.VA = 0x1000;
  Text.FileOff = 0x100;
  Text.Size = Text.FileSz = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100);
  const std::vector<uint32_t> Stub =
      Selector ? std::vector<uint32_t>{0xb0000001, 0xf9402021, 0xb0000010,
                                       0xf9404210, 0xd61f0200}
               : std::vector<uint32_t>{0xb0000010, 0xf9404210, 0xd61f0200};
  for (size_t I = 0; I != Stub.size(); ++I)
    llvm::support::endian::write32le(Text.Data.data() + I * 4, Stub[I]);
  Image.Segments.push_back(std::move(Text));
  Section Code;
  Code.Name = "__objc_stubs";
  Code.VA = 0x1000;
  Code.FileOff = 0x100;
  Code.Size = Code.FileSz = 0x100;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
  Image.Sections.push_back(Code);
  if (Selector) {
    ObjCSourceReference Ref;
    Ref.Address = 0x2040;
    Ref.Name = "test";
    Image.ObjCSourceReferences[0x2040] = Ref;
  }
}

LowFunc veneerCaller() {
  LowFunc Function;
  Function.Entry = 0x3000;
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = 0x3000;
  Block.EndAddr = 0x3004;
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = 0x3000;
  Call.Seq = 0;
  Call.Output = NdVar::reg(0, 8);
  Call.addInput(NdVar::cst(0x1000, 8));
  Block.Ops.push_back(Call);
  Function.Blocks.push_back(Block);
  return Function;
}
} // namespace

TEST(SwiftBooleanRuntimeCandidate, VeneerRequiresCurrentImmutableMachineBytes) {
  auto Image = candidateImage();
  addVeneer(Image);
  ASSERT_EQ(darwinImportVeneerSlot(Image, 0x1000), Slot);
  ASSERT_TRUE(swiftBooleanRuntimeVeneerCandidate(Image, 0x1000));
  EXPECT_TRUE(buildObjCSourceCallHints(Image, veneerCaller()).empty());
  for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Image;
    va_t Query = 0x1000;
    switch (Mutation) {
    case 0:
      Changed.Segments[1].Data[8] ^= 1; // Wrong BR register.
      break;
    case 1:
      Changed.Segments[1].Data[0] ^= 1; // Wrong ADRP register.
      break;
    case 2:
      Changed.Segments[1].Data[4] ^= 1; // Wrong LDR register.
      break;
    case 3:
      Changed.Segments[1].Flags = SegmentFlags::Readable |
                                  SegmentFlags::Writable |
                                  SegmentFlags::Executable;
      break;
    case 4:
      Changed.Sections[0].Flags = SegmentFlags::Readable;
      break;
    case 5:
      Changed.Segments[1].Data.resize(11);
      break;
    case 6:
      Changed.Sections[0].FileSz = 11;
      break;
    case 7:
      Changed.Sections[0].FileOff += 1;
      break;
    case 8:
      Changed.Sections.push_back(Changed.Sections[0]);
      break;
    case 9:
      Changed.Segments.push_back(Changed.Segments[1]);
      break;
    case 10:
      Changed.CodePtrRelocSlots.insert(0x1008);
      break;
    case 11:
      Query += 1;
      break;
    case 12:
      Changed.MachOChainedFixupsAmbiguous = true;
      break;
    case 13:
      Changed.DyldBindSlots[Slot].Module = "/tmp/libswiftCore.dylib";
      break;
    }
    EXPECT_FALSE(swiftBooleanRuntimeVeneerCandidate(Changed, Query));
  }
}

TEST(SwiftBooleanRuntimeCandidate,
     ExistingCatalogAndSelectorBindingStaySeparate) {
  auto Ordinary = candidateImage("_objc_retain", "/usr/lib/libobjc.A.dylib");
  addVeneer(Ordinary);
  EXPECT_EQ(darwinImportVeneerSlot(Ordinary, 0x1000), Slot);
  const auto Hints = buildObjCSourceCallHints(Ordinary, veneerCaller());
  ASSERT_EQ(Hints.size(), 1U);
  EXPECT_EQ(Hints.begin()->second.CallKind,
            SourceCallTypeHint::Kind::ObjCRuntimeCall);
  EXPECT_EQ(Hints.begin()->second.TargetName, "objc_retain");
  EXPECT_FALSE(swiftBooleanRuntimeVeneerCandidate(Ordinary, 0x1000));

  auto Selector = candidateImage("_objc_msgSend", "/usr/lib/libobjc.A.dylib");
  addVeneer(Selector, true);
  EXPECT_TRUE(objcSelectorStubOverwritesCommand(Selector, 0x1000));
  EXPECT_FALSE(darwinImportVeneerSlot(Selector, 0x1000));
  EXPECT_FALSE(swiftBooleanRuntimeVeneerCandidate(Selector, 0x1000));

  auto WrongSelector = candidateImage();
  addVeneer(WrongSelector, true);
  EXPECT_FALSE(objcSelectorStubOverwritesCommand(WrongSelector, 0x1000));
  EXPECT_FALSE(darwinImportVeneerSlot(WrongSelector, 0x1000));
  EXPECT_TRUE(buildObjCSourceCallHints(WrongSelector, veneerCaller()).empty());
}

TEST(SwiftBooleanRuntimeCandidate, RawI1IsNotAByteReturnDeclaration) {
  static_assert(
      !std::is_convertible_v<SwiftBooleanRuntimeCandidate, SourceCallTypeHint>);
  auto Image = candidateImage();
  const auto Candidate = swiftBooleanRuntimeCandidate(Image, Slot);
  ASSERT_TRUE(Candidate);
  EXPECT_EQ(Candidate->SourceImage, &Image);
  EXPECT_EQ(Candidate->ImportSlot, Slot);
  const auto &Raw = Candidate->RawContract;
  EXPECT_EQ(Raw.Architecture, Arch::AArch64);
  EXPECT_EQ(Raw.Convention, SourceFunctionTypeHint::ConventionKind::Swift);
  EXPECT_EQ(Raw.ResultCarrierBytes, 8U);
  EXPECT_EQ(Raw.DefinedResultBits, 1U);
  const auto Inputs = sourceBooleanInputParameters(Raw);
  ASSERT_TRUE(Inputs);
  ASSERT_EQ(Inputs->size(), 5U);
  for (size_t I = 0; I != Inputs->size(); ++I) {
    EXPECT_EQ((*Inputs)[I].Location.Kind,
              SourceABICarrierKind::IntegerRegister);
    EXPECT_EQ((*Inputs)[I].Location.RegisterOffset, I * 8);
    EXPECT_EQ((*Inputs)[I].Type->Size, I == 4 ? 1U : 8U);
    EXPECT_EQ((*Inputs)[I].Type->Kind,
              I == 1 || I == 3 ? NdTypeKind::Ptr : NdTypeKind::Int);
  }
  EXPECT_TRUE(Inputs->back().Location.ExtendTo32Bits);
  // Lookup alone must not publish the unresolved result as a complete byte
  // ABI. The future per-occurrence projection owns the use proof separately.
  EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, Slot));
}

TEST(SwiftBooleanRuntimeCandidate, CurrentProviderAndStorageMustAgree) {
  for (unsigned Mutation = 0; Mutation != 22; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = candidateImage();
    va_t Query = Slot;
    switch (Mutation) {
    case 0:
      Image.Format = BinaryFormat::ELF;
      break;
    case 1:
      Image.Arch = Arch::X64;
      break;
    case 2:
      Image.Bits = Bitness::Bits32;
      break;
    case 3:
      Image.IsRelocatable = true;
      break;
    case 4:
      Image.MachOChainedFixupsAmbiguous = true;
      break;
    case 5:
      Image.DyldBindSlots.clear();
      break;
    case 6:
      Image.DyldBindSlots[Slot].Module = "/tmp/libswiftCore.dylib";
      break;
    case 7:
      Image.DyldBindSlots[Slot].Module =
          "/usr/lib/swift/libswiftFoundation.dylib";
      break;
    case 8:
      Image.DyldBindSlots[Slot].WeakImport = true;
      break;
    case 9:
      Image.DyldBindSlots[Slot].Addend = 1;
      break;
    case 10:
      Image.DyldBindSlots[Slot].Name = "_other";
      break;
    case 11:
      Image.ImportPtrSlots[Slot] = "_other";
      break;
    case 12:
      Image.ImportPtrSlots.clear();
      break;
    case 13:
      Image.ImportStorageSlots[Slot].Name = "_other";
      break;
    case 14:
      Image.ImportStorageSlots[Slot].Addend = 8;
      break;
    case 15:
      Image.ConflictingImportStorageSlots.insert(Slot);
      break;
    case 16:
      Image.DynInfo.NeededLibs.clear();
      break;
    case 17:
      Image.DynInfo.NeededLibs = {"/tmp/libswiftCore.dylib"};
      break;
    case 18:
      Image.DynInfo.NeededLibs.push_back(SwiftBooleanComparisonProvider.str());
      break;
    case 19:
      Image.Segments[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Executable;
      break;
    case 20:
      Image.Segments[0].Data.resize(Slot - Image.Segments[0].VA + 7);
      break;
    case 21:
      Query += 1;
      break;
    }
    EXPECT_FALSE(swiftBooleanRuntimeCandidate(Image, Query));
  }
}

TEST(SwiftBooleanRuntimeCandidate, RawContractRejectsByteAndMalformedInputs) {
  auto Image = candidateImage();
  const auto Candidate = swiftBooleanRuntimeCandidate(Image, Slot);
  ASSERT_TRUE(Candidate);
  for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Raw = Candidate->RawContract;
    switch (Mutation) {
    case 0:
      Raw.DefinedResultBits = 0;
      break;
    case 1:
      Raw.DefinedResultBits = 8;
      break;
    case 2:
      Raw.DefinedResultBits = 64;
      break;
    case 3:
      Raw.ResultCarrierBytes = 4;
      break;
    case 4:
      Raw.ResultRegister = 8;
      break;
    case 5:
      Raw.Architecture = Arch::X64;
      break;
    case 6:
      Raw.Convention = SourceFunctionTypeHint::ConventionKind::C;
      break;
    case 7:
      Raw.Parameters[0].Location.ValueBytes = 4;
      break;
    case 8:
      Raw.Parameters[1].Location = Raw.Parameters[0].Location;
      break;
    case 9:
      Raw.Parameters[4].Location.ExtendTo32Bits = false;
      break;
    case 10:
      Raw.Parameters[0].Components.push_back(Raw.Parameters[0].Location);
      break;
    case 11:
      Raw.Parameters[0].Type = NdType::makeFloat(8);
      break;
    }
    EXPECT_FALSE(sourceBooleanInputParameters(Raw));
  }
}
