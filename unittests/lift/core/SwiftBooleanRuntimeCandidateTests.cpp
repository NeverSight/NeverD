#include "../../../lib/loader/ObjC/ObjCClassAccessorMachine.h"
#include "../../../lib/loader/Swift/SwiftBooleanProjection.h"
#include "gtest/gtest.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"
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

TEST(SwiftBooleanRuntimeCandidate, PrefixHasFourInputsAndNoByteReturnBinding) {
  auto Image = candidateImage(SwiftBooleanPrefixImport);
  addVeneer(Image);
  const auto Candidate = swiftBooleanRuntimeVeneerCandidate(Image, 0x1000);
  ASSERT_TRUE(Candidate);
  EXPECT_EQ(Candidate->ImportName, SwiftBooleanPrefixImport);
  EXPECT_EQ(Candidate->RawContract.Parameters.size(), 4U);
  EXPECT_EQ(Candidate->RawContract.DefinedResultBits, 1U);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, veneerCaller()).empty());
  EXPECT_FALSE(swiftBooleanRuntimeInputs("_$sSS9hasSuffixySbSSF_suffix"));
  Image.DyldBindSlots[Slot].WeakImport = true;
  EXPECT_FALSE(swiftBooleanRuntimeVeneerCandidate(Image, 0x1000));
  Image.DyldBindSlots[Slot].WeakImport = false;
  Image.DyldBindSlots[Slot].Module = "/tmp/libswiftCore.dylib";
  EXPECT_FALSE(swiftBooleanRuntimeVeneerCandidate(Image, 0x1000));
}

TEST(SwiftBooleanRuntimeCandidate, SuffixHasFourInputsAndNoByteReturnBinding) {
  auto Image = candidateImage(SwiftBooleanSuffixImport);
  addVeneer(Image);
  const auto Candidate = swiftBooleanRuntimeVeneerCandidate(Image, 0x1000);
  ASSERT_TRUE(Candidate);
  EXPECT_EQ(Candidate->ImportName, SwiftBooleanSuffixImport);
  EXPECT_EQ(Candidate->RawContract.Parameters.size(), 4U);
  EXPECT_EQ(Candidate->RawContract.DefinedResultBits, 1U);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, veneerCaller()).empty());
  Image.DyldBindSlots[Slot].Module = "/tmp/libswiftCore.dylib";
  EXPECT_FALSE(swiftBooleanRuntimeVeneerCandidate(Image, 0x1000));
}

TEST(SwiftBooleanRuntimeCandidate,
     ObjectEqualityUsesSwiftContextAndExactProvider) {
  auto Image = candidateImage(SwiftBooleanObjectEqualityImport,
                              SwiftBooleanObjectEqualityProvider);
  addVeneer(Image);
  const auto Candidate = swiftBooleanRuntimeVeneerCandidate(Image, 0x1000);
  ASSERT_TRUE(Candidate);
  const auto Inputs = sourceBooleanInputParameters(Candidate->RawContract);
  ASSERT_TRUE(Inputs);
  ASSERT_EQ(Inputs->size(), 3U);
  for (unsigned I = 0; I != 3; ++I) {
    EXPECT_EQ((*Inputs)[I].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ((*Inputs)[I].Type->Size, 8U);
    EXPECT_EQ((*Inputs)[I].Location.RegisterOffset, (I == 2 ? 20 : I) * 8U);
    EXPECT_EQ(Candidate->RawContract.Parameters[I].TheRole,
              I == 2 ? SourceParameterTypeHint::Role::SwiftContext
                     : SourceParameterTypeHint::Role::Ordinary);
  }
  EXPECT_EQ(Candidate->RawContract.DefinedResultBits, 1U);
  EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, Slot));
  EXPECT_TRUE(buildObjCSourceCallHints(Image, veneerCaller()).empty());
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    auto Changed = Image;
    if (Mutation == 0)
      Changed.DyldBindSlots[Slot].Module = SwiftBooleanComparisonProvider.str();
    else if (Mutation == 1)
      Changed.DyldBindSlots[Slot].WeakImport = true;
    else if (Mutation == 2)
      Changed.DynInfo.NeededLibs.clear();
    else
      Changed.DynInfo.NeededLibs.push_back(
          SwiftBooleanObjectEqualityProvider.str());
    EXPECT_FALSE(swiftBooleanRuntimeVeneerCandidate(Changed, 0x1000));
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

namespace {
struct ProjectionFixture {
  BinaryImage Image = candidateImage();
  LowFunc Low;
  SourceFunctionTypeHint Entry;

  void word(va_t Address, uint32_t Value) {
    uint8_t Bytes[4];
    llvm::support::endian::write32le(Bytes, Value);
    ASSERT_TRUE(Image.writeVA(Address, Bytes, 4));
  }
  ProjectionFixture() {
    addVeneer(Image);
    auto Text = Image.Segments.back();
    Text.VA = 0x3000;
    Text.FileOff = 0x200;
    Image.Segments.push_back(Text);
    auto Code = Image.Sections.front();
    Code.Name = "__text";
    Code.VA = Text.VA;
    Code.FileOff = Text.FileOff;
    Image.Sections.push_back(Code);
    word(0x3000, 0x97fff800); // BL 0x1000.
    word(0x3004, 0x92400000); // AND X0, X0, #1.
    word(0x3008, 0xd65f03c0); // RET.
    Low = veneerCaller();
    auto &Block = Low.Blocks.front();
    Block.EndAddr = 0x300c;
    LowOp Mask;
    Mask.Addr = 0x3004;
    Mask.Seq = 0;
    Mask.Opcode = NdOp::INT_AND;
    Mask.Output = NdVar::reg(0, 8);
    Mask.addInput(NdVar::reg(0, 8));
    Mask.addInput(NdVar::cst(1, 8));
    Block.Ops.push_back(Mask);
    LowOp Return;
    Return.Addr = 0x3008;
    Return.Seq = 0;
    Return.Opcode = NdOp::RETURN;
    Return.addInput(
        NdVar::reg(getTargetRegInfo(Arch::AArch64).LinkRegister, 8));
    Block.Ops.push_back(Return);
    ObjCMethod Method;
    Method.Implementation = Low.Entry;
    Method.Status = "supported";
    Method.TypeHint = parseObjCMethodEncoding("value", "B16@0:8");
    Image.ObjCMethods.push_back(Method);
    const auto ABI = objcMethodSourceTypeHint(Image, Low.Entry);
    if (!ABI)
      throw std::runtime_error("missing Boolean fixture entry ABI");
    Entry = *ABI;
  }
  auto qualify() const {
    return qualifySwiftBooleanProjection(Image, Low, Entry);
  }
};

SourceCallTypeHint addOpaqueObjectMessage(
    ProjectionFixture &F,
    llvm::StringRef Selector = "interactivePopGestureRecognizer") {
  constexpr va_t Getter = 0x1040, SelectorSlot = 0x2040;
  constexpr va_t MessageSlot = Slot + 8;
  Section Data;
  Data.Name = "__got";
  Data.VA = 0x2000;
  Data.Size = Data.FileSz = 0x100;
  Data.Flags = SegmentFlags::Readable;
  F.Image.Sections.push_back(Data);
  F.Image.DynInfo.NeededLibs.push_back("/usr/lib/libobjc.A.dylib");
  F.Image.DynInfo.NeededLibs.push_back(
      "/System/Library/Frameworks/UIKit.framework/UIKit");
  F.Image.DynInfo.NeededLibs.push_back(
      "/System/Library/Frameworks/Foundation.framework/Foundation");
  F.Image.ImportPtrSlots[MessageSlot] = "_objc_msgSend";
  EXPECT_TRUE(F.Image.recordDyldBindSlot(MessageSlot, "_objc_msgSend", 0,
                                         "/usr/lib/libobjc.A.dylib", false));
  F.Image.ObjCSourceReferences[SelectorSlot] = {
      ObjCSourceReference::Kind::Selector, SelectorSlot, 8,
      Selector.str()};
  const uint32_t Stub[] = {0xb0000001, 0xf9402021, 0xb0000010, 0xf9404610,
                           0xd61f0200};
  for (unsigned I = 0; I != std::size(Stub); ++I)
    F.word(Getter + I * 4, Stub[I]);
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  auto Signature =
      objcSelectorSourceTypeHint(F.Image, Selector);
  EXPECT_TRUE(Signature);
  if (Signature)
    Hint.Signature = *Signature;
  Hint.TargetAddress = Getter;
  Hint.TargetName = "objc_msgSend";
  Hint.Selector = Selector.str();
  Hint.SelectorReferenceAddress = SelectorSlot;
  return Hint;
}
} // namespace

TEST(SwiftBooleanProjection, CombinesCurrentIdentityAndConsumerProof) {
  ProjectionFixture F;
  const auto Result = F.qualify();
  ASSERT_TRUE(Result);
  EXPECT_EQ(Result->Normalization.Function, &F.Low);
  EXPECT_EQ(Result->Normalization.Site.Instruction, 0x3000U);
  EXPECT_EQ(Result->Normalization.Site.Sequence, 0);
  EXPECT_EQ(Result->Normalization.Site.StaticTarget, 0x1000U);
  EXPECT_EQ(Result->Runtime.SourceImage, &F.Image);
  EXPECT_EQ(Result->Runtime.ImportSlot, Slot);
  EXPECT_EQ(Result->Runtime.RawContract.DefinedResultBits, 1U);
  EXPECT_TRUE(buildObjCSourceCallHints(F.Image, F.Low).empty());
}

TEST(SwiftBooleanProjection, NativeEntryUsesConservativeWordUntilBound) {
  ProjectionFixture F;
  F.Image.ObjCMethods.clear();
  auto Native = Symbol::makeFunc(F.Low.Entry, 12);
  Native.Name = "_$s4Test10lookupHashSiyF";
  F.Image.Symbols.push_back(Native);
  const auto Provisional =
      provisionalNativeSwiftBooleanEntry(F.Image, F.Low.Entry);
  ASSERT_TRUE(Provisional);
  EXPECT_EQ(Provisional->Origin,
            SourceFunctionTypeHint::OriginKind::NativeAnalysis);
  EXPECT_EQ(Provisional->ReturnType->Size, 8U);
  EXPECT_TRUE(qualifySwiftBooleanProjection(F.Image, F.Low, *Provisional));
  EXPECT_FALSE(qualifySwiftBooleanProjection(F.Image, F.Low, F.Entry));

  auto Duplicate = F.Image;
  Duplicate.Symbols.push_back(Native);
  EXPECT_TRUE(provisionalNativeSwiftBooleanEntry(Duplicate, F.Low.Entry));
  auto Conflicting = F.Image;
  auto Data = Native;
  Data.IsFunc = false;
  Conflicting.Symbols.push_back(Data);
  EXPECT_FALSE(provisionalNativeSwiftBooleanEntry(Conflicting, F.Low.Entry));
  auto Missing = F.Image;
  Missing.Symbols.clear();
  EXPECT_FALSE(provisionalNativeSwiftBooleanEntry(Missing, F.Low.Entry));
  auto Method = F.Image;
  ObjCMethod ObjC;
  ObjC.Implementation = F.Low.Entry;
  Method.ObjCMethods.push_back(ObjC);
  EXPECT_FALSE(provisionalNativeSwiftBooleanEntry(Method, F.Low.Entry));
}

TEST(SwiftBooleanProjection,
     OpaqueGetterRequiresCurrentStubImportSelectorAndFixedABI) {
  ProjectionFixture F;
  const auto Hint = addOpaqueObjectMessage(F);
  SourceCallOccurrenceKey Site{0x3000, 0, NdOp::CALL, 0x1040};
  EXPECT_TRUE(readImmutableCodeBytes(F.Image, 0x1040, 20));
  EXPECT_TRUE(objcSelectorStubOverwritesCommand(F.Image, 0x1040));
  EXPECT_TRUE(objcSelectorStubMatches(F.Image, 0x1040, 0x2040,
                                      "interactivePopGestureRecognizer"));
  EXPECT_EQ(darwinImportVeneerSlot(F.Image, 0x1048), Slot + 8);
  EXPECT_TRUE(isImmutableImageImportSlot(F.Image, Slot + 8));
  EXPECT_EQ(Hint.Signature.Origin, SourceFunctionTypeHint::OriginKind::ObjCSDK);
  EXPECT_TRUE(Hint.Signature.HasExplicitABI);
  ASSERT_TRUE(
      swift_boolean_projection_detail::opaqueObjectMessage(F.Image, Site,
                                                            Hint));
  for (unsigned Mutation = 0; Mutation != 21; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = F.Image;
    auto Changed = Hint;
    switch (Mutation) {
    case 0:
      Changed.TargetName = "objc_msgSendSuper2";
      break;
    case 1:
      Changed.Selector += ":";
      break;
    case 2:
      Changed.SelectorReferenceAddress += 8;
      break;
    case 3:
      Image.ObjCSourceReferences[0x2040].Name = "parentViewController";
      break;
    case 4:
      Image.CodePtrRelocSlots.insert(0x1040);
      break;
    case 5:
      Image.Sections.push_back(Image.Sections.front());
      break;
    case 6:
      Image.Segments[1].Flags = SegmentFlags::Readable |
                                SegmentFlags::Writable |
                                SegmentFlags::Executable;
      break;
    case 7:
      Image.DyldBindSlots[Slot + 8].Module = "/tmp/libobjc.A.dylib";
      break;
    case 8:
      Image.DyldBindSlots[Slot + 8].WeakImport = true;
      break;
    case 9:
      Image.DyldBindSlots[Slot + 8].Addend = 1;
      break;
    case 10:
      Image.DynInfo.NeededLibs.clear();
      break;
    case 11:
      Image.DynInfo.NeededLibs.push_back("/usr/lib/libobjc.A.dylib");
      break;
    case 12:
      Changed.Signature.Origin =
          SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      break;
    case 13:
      Changed.Signature.Parameters[1].Location.RegisterOffset = 16;
      break;
    case 14:
      Changed.Signature.Parameters[1].Components = {
          Changed.Signature.Parameters[1].Location};
      Changed.Signature.Parameters[1].Location = {};
      break;
    case 15:
      Changed.Signature.ReturnLocation.RegisterOffset = 8;
      break;
    case 16:
      Changed.Format = SourceCallTypeHint::FormatArguments{};
      break;
    case 17:
      Image.MachOChainedFixupsAmbiguous = true;
      break;
    case 18:
      Site.StaticTarget = 0x1044;
      break;
    case 19:
      Image.Sections.back().FileSz = 0x80;
      break;
    case 20:
      Image.Segments.front().Flags =
          SegmentFlags::Readable | SegmentFlags::Executable;
      break;
    }
    EXPECT_FALSE(swift_boolean_projection_detail::opaqueObjectMessage(
        Image, Site, Changed));
  }
}

TEST(SwiftBooleanProjection,
     OpaqueObjectMessageAcceptsExactPointerArgumentsWithoutSupplyingABI) {
  ProjectionFixture F;
  constexpr llvm::StringLiteral Selector =
      "localizedStringForKey:value:table:";
  auto Hint = addOpaqueObjectMessage(F, Selector);
  const SourceCallOccurrenceKey Site{0x3000, 0, NdOp::CALL, 0x1040};
  ASSERT_EQ(Hint.Signature.Parameters.size(), 5U);
  EXPECT_TRUE(swift_boolean_projection_detail::opaqueObjectMessage(F.Image,
                                                                    Site,
                                                                    Hint));
  Hint.Signature.Parameters[4].Type = NdType::makeInt(8, false);
  EXPECT_FALSE(swift_boolean_projection_detail::opaqueObjectMessage(F.Image,
                                                                     Site,
                                                                     Hint));
}

TEST(SwiftBooleanProjection,
     OpaqueGetterBeforeSelectedCallKeepsFailedStubsClosed) {
  ProjectionFixture F;
  addOpaqueObjectMessage(F);
  auto &Block = F.Low.Blocks.front();
  auto Getter = Block.Ops.front();
  Getter.Addr = 0x3000;
  Getter.Inputs[0].Offset = 0x1040;
  for (auto &Op : Block.Ops)
    Op.Addr += 4;
  Block.Ops.insert(Block.Ops.begin(), Getter);
  Block.EndAddr += 4;
  F.word(0x3000, 0x97fff810); // BL selector stub.
  F.word(0x3004, 0x97fff7ff); // BL comparison veneer.
  F.word(0x3008, 0x92400000);
  F.word(0x300c, 0xd65f03c0);
  const auto Hints = buildObjCSourceCallHints(F.Image, F.Low);
  ASSERT_EQ(Hints.count(0x3000), 1U);
  EXPECT_EQ(Hints.at(0x3000).CallKind, SourceCallTypeHint::Kind::ObjCMessage);
  EXPECT_TRUE(F.qualify());

  // If the selector prefix no longer has its shared machine proof, this
  // __objc_stubs target cannot fall back to an unknown native opaque call.
  F.word(0x1040, 0xb0000000);
  EXPECT_EQ(buildObjCSourceCallHints(F.Image, F.Low).count(0x3000), 0U);
  EXPECT_FALSE(F.qualify());
}

TEST(SwiftBooleanProjection,
     OpaquePointerMessageBeforeBooleanCallRetainsCompleteProof) {
  ProjectionFixture F;
  addOpaqueObjectMessage(F, "localizedStringForKey:value:table:");
  auto &Block = F.Low.Blocks.front();
  auto Message = Block.Ops.front();
  Message.Addr = 0x3000;
  Message.Inputs[0].Offset = 0x1040;
  for (auto &Op : Block.Ops)
    Op.Addr += 4;
  Block.Ops.insert(Block.Ops.begin(), Message);
  Block.EndAddr += 4;
  F.word(0x3000, 0x97fff810); // BL exact selector stub.
  F.word(0x3004, 0x97fff7ff); // BL Boolean veneer.
  F.word(0x3008, 0x92400000); // AND X0, X0, #1.
  F.word(0x300c, 0xd65f03c0); // RET.
  const auto Hints = buildObjCSourceCallHints(F.Image, F.Low);
  ASSERT_EQ(Hints.count(0x3000), 1U);
  EXPECT_TRUE(F.qualify());
  F.Image.DyldBindSlots[Slot + 8].Module = "/tmp/libobjc.A.dylib";
  EXPECT_FALSE(F.qualify());
}

TEST(SwiftBooleanProjection, UnknownPrefixRequiresIdenticalPhysicalState) {
  for (bool Prefix : {false, true}) {
    ProjectionFixture F;
    auto &B = F.Low.Blocks.front();
    auto Opaque = B.Ops.front();
    Opaque.Inputs[0] = NdVar::cst(0x4000, 8);
    B.Ops.insert(B.Ops.begin() + (Prefix ? 0 : 1), Opaque);
    B.EndAddr += 4;
    for (unsigned I = 0; I < B.Ops.size(); ++I) {
      auto &Op = B.Ops[I];
      Op.Addr = B.StartAddr + I * 4;
      if (Op.Opcode == NdOp::CALL) {
        const int64_t Delta = int64_t(Op.Inputs[0].Offset) - int64_t(Op.Addr);
        F.word(Op.Addr, 0x94000000 | (uint32_t(Delta / 4) & 0x03ffffff));
      } else
        F.word(Op.Addr, Op.Opcode == NdOp::RETURN ? 0xd65f03c0 : 0x92400000);
    }
    EXPECT_EQ(bool(F.qualify()), Prefix);
    EXPECT_TRUE(buildObjCSourceCallHints(F.Image, F.Low).empty());
    F.word(Prefix ? 0x3000 : 0x3004,
           0xd63f0200); // BLR is not this direct call.
    EXPECT_FALSE(F.qualify());
  }
}

TEST(SwiftBooleanProjection,
     ProvesEveryOccurrenceWithoutInventingOtherResults) {
  ProjectionFixture F;
  auto &B = F.Low.Blocks.front();
  auto Call = B.Ops.front();
  auto Mask = B.Ops[1];
  Call.Addr = 0x3008;
  Mask.Addr = 0x300c;
  B.Ops.back().Addr = 0x3010;
  B.Ops.insert(B.Ops.end() - 1, Call);
  B.Ops.insert(B.Ops.end() - 1, Mask);
  B.EndAddr = 0x3014;
  F.word(0x3008, 0x97fff7fe);
  F.word(0x300c, 0x92400000);
  F.word(0x3010, 0xd65f03c0);
  const auto Prove = [&] {
    return qualifySwiftBooleanProjections(F.Image, F.Low, F.Entry);
  };
  ASSERT_EQ(Prove().size(), 2U);
  B.Ops[1].Inputs[1].Offset = 3;
  EXPECT_TRUE(Prove().empty()); // Raw padding reaches the next call's input.
  B.Ops[1].Inputs[1].Offset = 1;
  B.Ops[3].Inputs[1].Offset = 3;
  EXPECT_TRUE(Prove().empty()); // The second normalization also needs proof.
  B.Ops[3].Inputs[1].Offset = 1;
  ASSERT_EQ(Prove().size(), 2U);
  F.word(0x3008, 0x97fff7ff);
  EXPECT_TRUE(Prove().empty()); // The second machine target must still agree.
}

TEST(SwiftBooleanProjection, RejectsStaleIdentityABIAndObservableUpperBits) {
  for (unsigned Mutation = 0; Mutation != 20; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ProjectionFixture F;
    switch (Mutation) {
    case 0:
      F.Image.ObjCMethods.clear();
      break;
    case 1:
      F.Image.ObjCMethods[0].Status = "ambiguous_dispatch";
      break;
    case 2:
      F.Image.ObjCMethods[0].TypeHint.reset();
      break;
    case 3:
      F.Entry.ReturnType = NdType::makeInt(8, false);
      break;
    case 4:
      F.Low.Entry += 4;
      break;
    case 5:
      F.Low.Blocks[0].Ops[1].Inputs[1].Offset = 3;
      break;
    case 6:
      F.word(0x3000, 0x17fff800);
      break; // Tail branch, not BL.
    case 7:
      F.word(0x3000, 0x97fff801);
      break; // Different direct target.
    case 8:
      F.Low.Blocks[0].Ops[0].Inputs[0].Offset += 4;
      break;
    case 9:
      F.Image.Segments.back().Flags = SegmentFlags::Readable |
                                      SegmentFlags::Writable |
                                      SegmentFlags::Executable;
      break;
    case 10:
      F.Image.Sections.back().FileSz = 4;
      break;
    case 11:
      F.Image.Sections.push_back(F.Image.Sections.back());
      break;
    case 12:
      F.Image.CodePtrRelocSlots.insert(0x3004);
      break;
    case 13:
      F.Image.DyldBindSlots[Slot].Module = "/tmp/libswiftCore.dylib";
      break;
    case 14:
      F.Image.IsRelocatable = true;
      break;
    case 15:
      F.Image.Arch = Arch::X64;
      break;
    case 16:
      F.Low.Blocks[0].Ops[0].Opcode = NdOp::INDIR_CALL;
      break;
    case 17:
      F.Low.Blocks[0].Ops.insert(F.Low.Blocks[0].Ops.begin(),
                                 F.Low.Blocks[0].Ops.front());
      break;
    case 18:
      F.Low.Blocks[0].ExceptionalSuccs.push_back({});
      break;
    case 19:
      F.Image.ObjCMethods[0].Implementation += 4;
      break;
    }
    EXPECT_FALSE(F.qualify());
  }
}

TEST(SwiftBooleanProjection,
     RebuildsOtherCallContractsAndRejectsNativeGuesses) {
  ProjectionFixture F;
  const va_t ReleaseSlot = Slot + 8;
  F.Image.ImportPtrSlots[ReleaseSlot] = "_swift_bridgeObjectRelease";
  ASSERT_TRUE(
      F.Image.recordDyldBindSlot(ReleaseSlot, "_swift_bridgeObjectRelease", 0,
                                 SwiftBooleanComparisonProvider, false));
  F.word(0x1020, 0xb0000010);
  F.word(0x1024, 0xf9404610);
  F.word(0x1028, 0xd61f0200);
  auto &Block = F.Low.Blocks.front();
  auto Release = Block.Ops.front();
  Release.Inputs[0].Offset = 0x1020;
  for (auto &Op : Block.Ops)
    Op.Addr += 4;
  Block.Ops.insert(Block.Ops.begin(), Release);
  Block.EndAddr += 4;
  F.word(0x3000, 0x97fff808); // BL release veneer.
  F.word(0x3004, 0x97fff7ff); // BL comparison veneer.
  F.word(0x3008, 0x92400000);
  F.word(0x300c, 0xd65f03c0);
  ASSERT_TRUE(F.qualify());
  F.Image.ImportPtrSlots.erase(ReleaseSlot);
  F.Image.DyldBindSlots.erase(ReleaseSlot);
  EXPECT_FALSE(F.qualify());
}

namespace {
void addClassAccessor(ProjectionFixture &F) {
  F.Image.DynInfo.NeededLibs.push_back("/usr/lib/libobjc.A.dylib");
  F.Image.ImportPtrSlots[Slot + 8] = "_objc_opt_self";
  ASSERT_TRUE(F.Image.recordDyldBindSlot(Slot + 8, "_objc_opt_self", 0,
                                         "/usr/lib/libobjc.A.dylib", false));
  F.word(0x1020, 0xb0000010);
  F.word(0x1024, 0xf9404610);
  F.word(0x1028, 0xd61f0200);
  const uint32_t Body[] = {0xa9bf7bfd, 0x910003fd, 0xf0ffffe0, 0x91140000,
                           0x97fff7f4, 0xd2800001, 0xa8c17bfd, 0xd65f03c0};
  for (unsigned I = 0; I != std::size(Body); ++I)
    F.word(0x3040 + 4 * I, Body[I]);
}
} // namespace

TEST(ObjCClassAccessorMachine, ProvesUnusedInputsAndFullPointerResultOnly) {
  ProjectionFixture F;
  addClassAccessor(F);
  auto M = objcClassAccessorMachine(F.Image, 0x3040);
  ASSERT_TRUE(M);
  EXPECT_EQ(M->ClassAddress, 0x2500U);
  EXPECT_EQ(M->SelfTarget, 0x1020U);
  EXPECT_EQ(M->SelfCall.TargetAddress, Slot + 8);
  EXPECT_TRUE(M->Signature.Parameters.empty());
  EXPECT_EQ(M->Signature.ReturnType->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(M->Signature.ReturnLocation.ValueBytes, 8U);
  EXPECT_EQ(M->Signature.ReturnLocation.RegisterOffset,
            getTargetRegInfo(Arch::AArch64).IntReturnReg);
  // This fixture has no class identity at 0x2500. Machine ABI facts cannot
  // authenticate that address as a source-rebuildable runtime object.
  EXPECT_EQ(F.Image.ObjCClasses.size(), 0U);
}

TEST(ObjCClassAccessorMachine, RejectsChangedInstructionsProviderAndStorage) {
  for (unsigned Mutation = 0; Mutation != 17; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ProjectionFixture F;
    addClassAccessor(F);
    va_t Entry = 0x3040;
    switch (Mutation) {
    case 0:
      F.word(Entry, 0xa9bf7bfc);
      break;
    case 1:
      F.word(Entry + 4, 0x910003fc);
      break;
    case 2:
      F.word(Entry + 8, 0xf0ffffe1);
      break;
    case 3:
      F.word(Entry + 12, 0x91540000);
      break;
    case 4:
      F.word(Entry + 16, 0x17fff7f4);
      break;
    case 5:
      F.word(Entry + 20, 0xd2800000);
      break;
    case 6:
      F.word(Entry + 24, 0xa8c17bfc);
      break;
    case 7:
      F.word(Entry + 28, 0xd65f0000);
      break;
    case 8:
      F.Image.DyldBindSlots[Slot + 8].Module = "/tmp/libobjc.A.dylib";
      break;
    case 9:
      F.Image.ImportPtrSlots[Slot + 8] = "_objc_release";
      break;
    case 10:
      F.word(0x1028, 0xd61f0220);
      break;
    case 11:
      F.Image.CodePtrRelocSlots.insert(Entry + 8);
      break;
    case 12:
      F.Image.Sections.push_back(F.Image.Sections.back());
      break;
    case 13:
      F.Image.Segments.back().Flags = SegmentFlags::Readable |
                                      SegmentFlags::Writable |
                                      SegmentFlags::Executable;
      break;
    case 14:
      Entry += 1;
      break;
    case 15:
      F.Image.IsRelocatable = true;
      break;
    case 16:
      F.Image.MachOChainedFixupsAmbiguous = true;
      break;
    }
    EXPECT_FALSE(objcClassAccessorMachine(F.Image, Entry));
  }
}

TEST(ObjCClassAccessorMachine, UsesSharedStructuralUnwindBoundary) {
  ProjectionFixture F;
  addClassAccessor(F);
  ExceptionFunction Metadata;
  Metadata.CodeRange = {0x3040, 0x3060};
  Metadata.Encoding = ExceptionEncoding::CompactUnwind;
  Metadata.Compact.emplace();
  Metadata.Compact->SemanticStatus = CompactUnwindSemanticStatus::Complete;
  F.Image.ExceptionMetadata.Functions = {Metadata};
  ASSERT_TRUE(objcClassAccessorMachine(F.Image, 0x3040));
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    auto Bad = F.Image;
    auto &M = Bad.ExceptionMetadata.Functions.front();
    switch (Mutation) {
    case 0:
      M.ParseStatus = ExceptionParseStatus::Partial;
      break;
    case 1:
      M.PersonalityVA = 0x4000;
      break;
    case 2:
      M.Compact->HasLSDA = true;
      break;
    case 3:
      M.Compact->SemanticStatus = CompactUnwindSemanticStatus::Partial;
      break;
    }
    EXPECT_FALSE(objcClassAccessorMachine(Bad, 0x3040));
  }
}

TEST(SwiftBooleanProjection, OtherNativeCallsSeparateABIFromIdenticalState) {
  ProjectionFixture F;
  addClassAccessor(F);
  auto &B = F.Low.Blocks.front();
  auto Call = B.Ops.front();
  Call.Addr = 0x3008;
  Call.Inputs[0].Offset = 0x3040;
  B.Ops.back().Addr = 0x300c;
  B.Ops.insert(B.Ops.end() - 1, Call);
  B.EndAddr = 0x3010;
  F.word(0x3008, 0x9400000e);
  F.word(0x300c, 0xd65f03c0);
  ASSERT_TRUE(F.qualify());
  // This native call still has no ordinary loader source binding. Its current
  // complete body, rather than a native candidate signature, supplies the ABI.
  EXPECT_EQ(buildObjCSourceCallHints(F.Image, F.Low).count(0x3008), 0U);
  F.word(0x3048, 0xf0ffffe1); // x0 is now an incoming argument.
  EXPECT_FALSE(objcClassAccessorMachine(F.Image, 0x3040));
  EXPECT_TRUE(F.qualify()); // The mask already made every physical byte equal.
  B.Ops[1].Inputs[1].Offset = 3;
  EXPECT_FALSE(F.qualify()); // Opaque calls cannot accept even one changed bit.
  B.Ops[1].Inputs[1].Offset = 1;
  F.word(0x3048, 0xf0ffffe0);
  ASSERT_TRUE(F.qualify());
  F.Image.DyldBindSlots[Slot + 8].Module = "/tmp/libobjc.A.dylib";
  EXPECT_FALSE(objcClassAccessorMachine(F.Image, 0x3040));
  EXPECT_TRUE(F.qualify()); // No callee facts are obtained from the bad body.
}

TEST(SwiftBooleanProjection,
     SuperInitRequiresCurrentSelectorProviderAndInputs) {
  ProjectionFixture F;
  const va_t SuperSlot = Slot + 16, SelectorSlot = 0x20c0;
  F.Image.ImportPtrSlots[SuperSlot] = "_objc_msgSendSuper2";
  ASSERT_TRUE(F.Image.recordDyldBindSlot(SuperSlot, "_objc_msgSendSuper2", 0,
                                         "/usr/lib/libobjc.A.dylib", false));
  F.Image.DynInfo.NeededLibs.push_back("/usr/lib/libobjc.A.dylib");
  F.Image.ObjCSourceReferences[SelectorSlot] = {
      ObjCSourceReference::Kind::Selector, SelectorSlot, 8, "init"};
  ObjCMethod Init;
  Init.ClassName = "Base";
  Init.Selector = "init";
  Init.TypeEncoding = "@16@0:8";
  Init.TypeHint = parseObjCMethodEncoding(Init.Selector, Init.TypeEncoding);
  Init.Status = "supported";
  F.Image.ObjCMethods.push_back(Init);
  F.word(0x1020, 0xb0000010);
  F.word(0x1024, 0xf9404a10);
  F.word(0x1028, 0xd61f0200);
  auto &B = F.Low.Blocks.front();
  auto Call = B.Ops.front();
  Call.Addr = 0x300c;
  Call.Inputs[0].Offset = 0x1020;
  auto Load = B.Ops.front();
  Load.Opcode = NdOp::LOAD;
  Load.Addr = 0x3008;
  Load.Output = NdVar::reg(8, 8);
  Load.Inputs[0] = NdVar::cst(SelectorSlot, 8);
  B.Ops.back().Addr = 0x3010;
  B.Ops.insert(B.Ops.end() - 1, Load);
  B.Ops.insert(B.Ops.end() - 1, Call);
  B.EndAddr = 0x3014;
  F.word(0x3008, 0x58ff85c1);
  F.word(0x300c, 0x97fff805);
  F.word(0x3010, 0xd65f03c0);
  const auto Hints = buildObjCSourceCallHints(F.Image, F.Low);
  ASSERT_EQ(Hints.count(0x300c), 1U);
  ASSERT_EQ(Hints.at(0x300c).CallKind, SourceCallTypeHint::Kind::ObjCSuper2);
  ASSERT_TRUE(F.qualify());
  F.Low.Blocks[0].Ops[1].Inputs[1].Offset = 3;
  EXPECT_FALSE(F.qualify()); // The changed result reaches the super pointer.
  F.Low.Blocks[0].Ops[1].Inputs[1].Offset = 1;
  F.Image.ObjCSourceReferences[SelectorSlot].Name = "description";
  EXPECT_FALSE(F.qualify());
  F.Image.ObjCSourceReferences[SelectorSlot].Name = "init";
  F.Image.DyldBindSlots[SuperSlot].Module = "/tmp/libobjc.A.dylib";
  EXPECT_FALSE(F.qualify());
}
