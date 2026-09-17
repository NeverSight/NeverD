#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "../../../lib/sdk/capi/ObjCSourceInputs.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <array>
#include <filesystem>
#include <fstream>

using namespace neverd;
using namespace neverd::sdk;
TEST(ObjCSourceBindings, NativeTerminationRequiresExactCalleeAndCompleteFlow) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
      SCOPED_TRACE(Mutation);
      BinaryImage Image;
      Image.Arch = Architecture;
      Image.Format = BinaryFormat::MachO;
      Image.Bits = Bitness::Bits64;
      HighFunc Callee;
      Callee.Entry = 0x2000;
      Callee.DoesNotReturn = true;
      Callee.ReturnType = NdType::makeVoid();
      SourceFunctionTypeHint Signature;
      Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      Signature.ReturnType = Callee.ReturnType;
      std::string Error;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Signature, Architecture, Error));
      Callee.SourceTypeHint = Signature;
      HighStmt Trap;
      Trap.Kind = StmtKind::Call;
      Trap.CallExpr = HighExpr::makeCall("trap", 0, {});
      Trap.CallExpr->IntrinsicId =
          Architecture == Arch::AArch64 ? Intrinsic::Brk : Intrinsic::Ud2;
      Callee.Body = {Trap};
      auto Call = HighExpr::makeCall("native_terminator", Callee.Entry, {});
      Call->Type = NdType::makeVoid();
      auto Binding = std::make_shared<SourceCallTypeHint>();
      Binding->TargetAddress = Callee.Entry;
      Binding->Signature = Signature;
      Binding->DoesNotReturn = true;
      Call->SourceCallHint = Binding;
      if (Mutation == 1)
        Callee.DoesNotReturn = false;
      if (Mutation == 2)
        Callee.Body.clear();
      if (Mutation == 3) {
        Callee.Body[0] = HighStmt{};
        Callee.Body[0].Kind = StmtKind::Return;
      }
      if (Mutation == 4)
        Call->CallAddr += 4;
      if (Mutation == 5)
        Call->IsIndirectCall = true;
      if (Mutation == 6)
        Binding->Signature.ReturnType = NdType::makeInt(8);
      EXPECT_EQ(objcSourceCallBound(*Call, Image, {{Callee.Entry, &Callee}}),
                Mutation == 0);
    }
}

namespace {
struct EntryInputFixture {
  BinaryImage Image;
  HighFunc Caller, Callee;
  std::shared_ptr<SourceCallTypeHint> Hint;
  ExprPtr Pointer, Call, Load;
  EntryInputFixture(Arch Architecture = Arch::AArch64, unsigned Width = 4) {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Architecture;
    Image.Bits = Bitness::Bits64;
    Image.ObjCSourceReferences.emplace(
        0x1010, ObjCSourceReference{ObjCSourceReference::Kind::IvarOffset,
                                    0x1010, 8, "field", "Owner"});
    auto Type = NdType::makeInt(Width, false);
    Callee.Entry = 0x2000;
    Callee.Name = "readField";
    Callee.ReturnType = Type;
    Callee.Params = {{"offset", NdType::makePtr(Type)}};
    SourceFunctionTypeHint Signature;
    Signature.ReturnType = Type;
    Signature.Parameters = {{"offset", NdType::makePtr(Type)}};
    std::string Reason;
    if (!assignDarwinScalarSourceABI(Signature, Architecture, Reason))
      ADD_FAILURE() << Reason;
    Callee.SourceTypeHint = Signature;
    Hint = std::make_shared<SourceCallTypeHint>();
    Hint->CallKind = SourceCallTypeHint::Kind::Native;
    Hint->TargetAddress = Callee.Entry;
    Hint->Signature = Signature;
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.Id = 0;
    Parameter.Size = 8;
    Pointer = HighExpr::makeVar(Parameter, NdType::makeInt(8));
    Load = HighExpr::makeLoad(Pointer, Type);
    MedVar Temporary;
    Temporary.Kind = MedVar::Temp;
    Temporary.Id = 10;
    Temporary.Size = Width;
    auto Local = HighExpr::makeVar(Temporary, Type);
    HighStmt Assignment;
    Assignment.Kind = StmtKind::Assign;
    Assignment.Dst = Local;
    Assignment.Val = Load;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = Local;
    Callee.Body = {Assignment, Return};
    Call = HighExpr::makeCall(
        "readField", Callee.Entry,
        {HighExpr::makeConst(0x1010, 8,
                             ConstantAddressProvenance::DataAddress)});
    Call->SourceCallHint = Hint;
    Call->Type = Type;
    Return.RetVal = Call;
    Caller.ReturnType = Type;
    Caller.Body = {Return};
  }
  HighFunc project() {
    return snapshotObjCEntryInputs(Caller, Image, {{Callee.Entry, &Callee}});
  }
};

TEST(ObjCSourceBindings, SwiftValueWitnessRequiresCanonicalIndirectCall) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  const std::map<va_t, const HighFunc *> Functions;
  for (const auto Operation :
       {SourceCallTypeHint::SwiftValueWitnessKind::Destroy,
        SourceCallTypeHint::SwiftValueWitnessKind::InitializeWithCopy,
        SourceCallTypeHint::SwiftValueWitnessKind::
            InitializeBufferWithCopyOfBuffer,
        SourceCallTypeHint::SwiftValueWitnessKind::AssignWithCopy,
        SourceCallTypeHint::SwiftValueWitnessKind::InitializeWithTake,
        SourceCallTypeHint::SwiftValueWitnessKind::AssignWithTake,
        SourceCallTypeHint::SwiftValueWitnessKind::GetEnumTagSinglePayload,
        SourceCallTypeHint::SwiftValueWitnessKind::StoreEnumTagSinglePayload}) {
    const auto Hint = swiftValueWitnessSourceCallHint(Image.Arch, Operation);
    ASSERT_TRUE(Hint);
    std::vector<ExprPtr> Arguments;
    for (const auto &Parameter : Hint->Signature.Parameters) {
      auto Argument = HighExpr::makeConst(0, 8);
      Argument->Type = Parameter.Type;
      Arguments.push_back(std::move(Argument));
    }
    auto Call = HighExpr::makeCall("indirect_call", 0, std::move(Arguments));
    Call->IsIndirectCall = true;
    Call->SourceCallHint = std::make_shared<const SourceCallTypeHint>(*Hint);
    EXPECT_TRUE(objcSourceCallBound(*Call, Image, Functions));
    Call->IsIndirectCall = false;
    EXPECT_FALSE(objcSourceCallBound(*Call, Image, Functions));
    Call->IsIndirectCall = true;
    auto Forged = *Hint;
    Forged.Signature.Parameters.back().Location.RegisterOffset += 8;
    Call->SourceCallHint =
        std::make_shared<const SourceCallTypeHint>(std::move(Forged));
    EXPECT_FALSE(objcSourceCallBound(*Call, Image, Functions));
    Forged = *Hint;
    Forged.CallKind = SourceCallTypeHint::Kind::Native;
    Call->SourceCallHint =
        std::make_shared<const SourceCallTypeHint>(std::move(Forged));
    EXPECT_FALSE(objcSourceCallBound(*Call, Image, Functions));
  }
}

TEST(ObjCSourceInputs, EntrySnapshotUsesRuntimeOffsetWithoutChangingNativeABI) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Width : {4U, 8U}) {
      EntryInputFixture F(Architecture, Width);
      ASSERT_TRUE(entryScalarLoadInput(F.Callee, 0));
      auto Projected = F.project();
      ASSERT_EQ(Projected.Body.size(), 2U);
      ASSERT_EQ(Projected.Body[0].Val->Kind, ExprKind::Load);
      EXPECT_EQ(Projected.Body[0].Val->Type->Size, Width);
      const auto Address = Projected.Body[1].RetVal->Operands[0];
      ASSERT_EQ(Address->Kind, ExprKind::Addr);
      EXPECT_TRUE(Address->Operands[0]->structuralEq(*Projected.Body[0].Dst));
      EXPECT_EQ(Projected.Body[1].RetVal->SourceCallHint, F.Hint);
      EXPECT_EQ(F.Caller.Body.size(), 1U);
      EXPECT_EQ(F.Call->Operands[0]->Kind, ExprKind::Const);
      EXPECT_EQ(F.Callee.Body[0].Val, F.Load);
      const auto Bound = bindObjCSourceReferences(Projected, F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      ASSERT_TRUE(Bound.Function.Body[0].Val->SourceCallHint);
      EXPECT_EQ(Bound.Function.Body[0].Val->SourceCallHint->CallKind,
                SourceCallTypeHint::Kind::RuntimeIvarOffset);
      EXPECT_EQ(Bound.InstanceLayoutClasses, std::set<std::string>{"Owner"});
    }
  }
}

TEST(ObjCSourceInputs, EntryReadRejectsEffectsEscapesControlAndMalformedUses) {
  for (unsigned Case = 0; Case < 22; ++Case) {
    SCOPED_TRACE(Case);
    EntryInputFixture F;
    switch (Case) {
    case 0:
      F.Callee.Body[1].RetVal = F.Load;
      break;
    case 1:
      F.Callee.Body[1].RetVal = F.Pointer;
      break;
    case 2:
      F.Callee.Body[1].Kind = StmtKind::Store;
      F.Callee.Body[1].StoreAddr = F.Pointer;
      F.Callee.Body[1].RetVal.reset();
      F.Callee.Body[1].StoreVal = HighExpr::makeConst(0, 4);
      break;
    case 3:
      F.Callee.Body.insert(F.Callee.Body.begin(), F.Callee.Body.back());
      break;
    case 4:
      F.Callee.Body[1].Kind = StmtKind::Goto;
      F.Callee.Body[1].GotoTarget = F.Callee.Entry;
      break;
    case 5:
      F.Callee.Body[1].Kind = StmtKind::If;
      break;
    case 6:
      F.Callee.Body[1].Body = {F.Callee.Body.front()};
      break;
    case 7:
      F.Callee.UnstructuredExceptionRegions = 1;
      break;
    case 8:
      F.Load->MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 9:
      F.Pointer->Var.SSAVer = 1;
      break;
    case 10:
      F.Pointer->Var.RenameTag = 3;
      break;
    case 11:
      F.Pointer->Type = NdType::makeInt(4);
      break;
    case 12:
      F.Load->Type = NdType::makeFloat(4);
      break;
    case 13:
      F.Callee.Body[0].Dst = F.Pointer;
      break;
    case 14:
      F.Callee.Body.insert(F.Callee.Body.begin(), F.Callee.Body.front());
      F.Callee.Body[0].Val = HighExpr::makeCall("effect", 0x3000, {});
      break;
    case 15:
      F.Callee.Body[1].RetVal =
          HighExpr::makeCall("escape", 0x3000, {F.Pointer});
      break;
    case 16:
      F.Load->Operands[0] = HighExpr::makeBinop(NdOp::INT_ADD, F.Pointer,
                                                HighExpr::makeConst(4, 8));
      break;
    case 17:
      F.Callee.Body.resize(257);
      break;
    case 18:
      F.Callee.Body.insert(F.Callee.Body.begin(), F.Callee.Body.front());
      F.Callee.Body[0].Kind = StmtKind::Nop;
      F.Callee.Body[0].Dst.reset();
      F.Callee.Body[0].Val = HighExpr::makeCall("effect", 0x3000, {});
      break;
    case 19:
      F.Callee.Body[1].RetVal = HighExpr::makeCall("effects", 0x3000, {});
      F.Callee.Body[1].RetVal->IntrinsicOutputs = {F.Pointer->Var};
      break;
    case 20:
      F.Callee.Body[1].RetVal = HighExpr::makeCall(
          "large", 0x3000,
          std::vector<ExprPtr>(4096, HighExpr::makeConst(0, 8)));
      break;
    case 21:
      F.Callee.Params[0].Type = NdType::makeInt(8);
      break;
    }
    EXPECT_FALSE(entryScalarLoadInput(F.Callee, 0));
    EXPECT_EQ(F.project().Body.size(), 1U);
  }
}

TEST(ObjCSourceInputs, SnapshotRequiresExactTargetABIStorageAndArgumentOrder) {
  for (unsigned Case = 0; Case < 18; ++Case) {
    SCOPED_TRACE(Case);
    EntryInputFixture F;
    auto Argument = F.Call->Operands[0];
    switch (Case) {
    case 0:
      F.Call->CallAddr += 4;
      break;
    case 1:
      F.Hint->Signature.ReturnLocation.RegisterOffset += 8;
      break;
    case 2:
      F.Call->IsIndirectCall = true;
      break;
    case 3:
      F.Hint->CallKind = SourceCallTypeHint::Kind::ObjCMessage;
      break;
    case 4:
      Argument->ConstProvenance = ConstantAddressProvenance::Scalar;
      break;
    case 5:
      Argument->ConstProvenance = ConstantAddressProvenance::Unknown;
      break;
    case 6:
      Argument->ConstProvenance = ConstantAddressProvenance::CodeAddress;
      break;
    case 7:
      Argument->AddressOwnerVA = 0x1000;
      break;
    case 8:
      Argument->Type = NdType::makeInt(4);
      break;
    case 9:
      F.Image.ObjCSourceReferences.at(0x1010).Size = 2;
      break;
    case 10:
      F.Image.ObjCSourceReferences.at(0x1010).TheKind =
          ObjCSourceReference::Kind::Class;
      break;
    case 11:
      F.Image.ObjCSourceReferences.clear();
      break;
    case 12:
      F.Caller.Body[0].MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 13:
      F.Caller.Body[0].Kind = StmtKind::While;
      break;
    case 14:
      F.Call->Operands[0] = HighExpr::makeCall("effect", 0x3000, {});
      break;
    case 15:
      F.Callee.SourceTypeHint.reset();
      break;
    case 16:
      F.Callee.Params[0].Name = "different";
      break;
    case 17:
      F.Callee.Params[0].Type = NdType::makePtr(NdType::makeInt(8));
      break;
    }
    EXPECT_EQ(F.project().Body.size(), 1U);
    EXPECT_EQ(F.Caller.Body[0].RetVal, F.Call);
  }
}

struct Fixture {
  BinaryImage Image;
  HighFunc Function;
  Fixture() {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Segment Mapping;
    Mapping.VA = 0x1000;
    Mapping.Size = Mapping.FileSz = 0x100;
    Mapping.Flags = SegmentFlags::Readable;
    Mapping.Data.resize(0x100);
    Image.Segments.push_back(Mapping);
    Section Data;
    Data.VA = 0x1000;
    Data.Size = Data.FileSz = 0x100;
    Data.Flags = SegmentFlags::Readable;
    Image.Sections.push_back(Data);
    Image.ObjCSourceReferences.emplace(
        0x1010,
        ObjCSourceReference{
            ObjCSourceReference::Kind::Selector, 0x1010, 8, "step:", {}});
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal =
        HighExpr::makeLoad(HighExpr::makeConst(0x1010, 8), NdType::makeInt(8));
    Function.Body.push_back(Return);
  }
};
} // namespace

namespace {
Fixture readonlyTableFixture(uint16_t Width = 8) {
  Fixture F;
  F.Image.ObjCSourceReferences.clear();
  F.Function.ReturnType = NdType::makeInt(Width, false);
  MedVar V;
  V.Kind = MedVar::Param;
  V.Size = 4;
  auto Index = HighExpr::makeVar(V, NdType::makeInt(4, false));
  auto Guard =
      HighExpr::makeBinop(NdOp::INT_LESS, Index, HighExpr::makeConst(3, 4));
  Guard->Type = NdType::makeInt(4, false);
  auto Wide = HighExpr::makeUnary(NdOp::INT_ZEXT, Index);
  Wide->Type = NdType::makeInt(8, false);
  auto Offset =
      HighExpr::makeBinop(NdOp::INT_MULT, Wide, HighExpr::makeConst(Width, 8));
  auto Address = HighExpr::makeBinop(NdOp::INT_ADD,
                                     HighExpr::makeConst(0x1040, 8), Offset);
  HighStmt Load;
  Load.Kind = StmtKind::Return;
  Load.RetVal = HighExpr::makeLoad(Address, NdType::makeInt(Width, false));
  HighStmt Other;
  Other.Kind = StmtKind::Return;
  Other.RetVal = HighExpr::makeConst(0, Width);
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond = Guard;
  Branch.Body = {Load};
  Branch.ElseBody = {Other};
  F.Function.Body = {Branch};
  const uint64_t Values[] = {7, 99, 253};
  for (unsigned I = 0; I < 3; ++I)
    for (unsigned B = 0; B < Width; ++B)
      F.Image.Segments[0].Data[0x40 + I * Width + B] = Values[I] >> (B * 8);
  return F;
}
} // namespace

TEST(ObjCSourceBindings, ReadOnlyTablesBindEveryBoundedScalarLoadOccurrence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (uint16_t Width : {1, 2, 4, 8}) {
      auto F = readonlyTableFixture(Width);
      F.Image.Arch = Architecture;
      const auto Original = F.Function.Body[0].Body[0].RetVal;
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      ASSERT_EQ(Bound.BorrowedBytes.size(), 1U);
      EXPECT_EQ(Bound.BorrowedBytes.begin()->first, 0x1040U);
      EXPECT_EQ(Bound.BorrowedBytes.begin()->second, Width * 3U);
      const auto &Load = Bound.Function.Body[0].Body[0].RetVal;
      EXPECT_EQ(Load->Kind, ExprKind::Load);
      EXPECT_EQ(Load->Type->Size, Width);
      const auto &Helper = Load->Operands[0]->Operands[0];
      ASSERT_TRUE(Helper->SourceCallHint);
      EXPECT_EQ(Helper->SourceCallHint->CallKind,
                SourceCallTypeHint::Kind::RuntimeReadOnlyBytes);
      const auto Allowed = readOnlyScalarSourceHelpers(Bound.Function, F.Image);
      ASSERT_EQ(Allowed.size(), 1U);
      EXPECT_TRUE(objcSourceCallBound(*Helper, F.Image, {}, nullptr, &Allowed));
      EXPECT_FALSE(objcSourceCallBound(*Helper, F.Image, {}));
      EXPECT_EQ(Original->Operands[0]->Operands[0]->Kind, ExprKind::Const);
    }
}

TEST(ObjCSourceBindings, ReadOnlyTablesRejectUnprovenRangesStorageAndUses) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 14; ++Mutation) {
      auto F = readonlyTableFixture();
      F.Image.Arch = Architecture;
      auto &Branch = F.Function.Body[0];
      auto &Load = Branch.Body[0].RetVal;
      auto &Offset = Load->Operands[0]->Operands[1];
      if (Mutation == 0)
        Branch.Cond = HighExpr::makeConst(1, 1);
      if (Mutation == 1)
        Branch.Cond->Op = NdOp::INT_SLESS;
      if (Mutation == 2)
        F.Image.Sections[0].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
      if (Mutation == 3)
        F.Image.Sections.push_back(F.Image.Sections[0]);
      if (Mutation == 4)
        F.Image.Sections[0].FileSz = 0x47;
      if (Mutation == 5)
        F.Image.DataPtrRelocSlots.insert(0x1048);
      if (Mutation == 6)
        Load->MemoryOrdering = NdMemoryOrdering::Acquire;
      if (Mutation == 7)
        Load->MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      if (Mutation == 8)
        Offset->Operands[1] = HighExpr::makeConst(65537, 8);
      if (Mutation == 9)
        llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 0x48,
                                         0x1080);
      if (Mutation == 10)
        F.Function.UnstructuredExceptionRegions = 1;
      if (Mutation == 11)
        F.Function.ReturnType = NdType::makePtr(NdType::makeVoid());
      if (Mutation == 12)
        F.Function.Body.push_back(Branch.Body[0]);
      if (Mutation == 13) {
        HighStmt Overwrite;
        Overwrite.Kind = StmtKind::Assign;
        Overwrite.Dst = Offset->Operands[0]->Operands[0];
        Overwrite.Val = HighExpr::makeConst(99, 4);
        HighStmt Loop;
        Loop.Kind = StmtKind::DoWhile;
        Loop.Cond = Load;
        Loop.Body = {Overwrite};
        Branch.Body = {Loop};
      }
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      EXPECT_FALSE(Bound.Limitation.empty()) << Mutation;
      EXPECT_TRUE(Bound.BorrowedBytes.empty()) << Mutation;
    }
}

TEST(ObjCSourceBindings, ReadOnlyTablePublicationRechecksFlowBytesAndEscapes) {
  for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
    auto F = readonlyTableFixture();
    auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty());
    auto &Branch = Bound.Function.Body[0];
    const auto &Load = Branch.Body[0].RetVal;
    const auto Helper = Load->Operands[0]->Operands[0];
    ASSERT_EQ(readOnlyScalarSourceHelpers(Bound.Function, F.Image).size(), 1U);
    if (Mutation == 0)
      Branch.Cond = HighExpr::makeConst(1, 1);
    if (Mutation == 1) {
      HighStmt Escape;
      Escape.Kind = StmtKind::Return;
      Escape.RetVal = Helper;
      Bound.Function.Body.push_back(Escape);
    }
    if (Mutation == 2)
      F.Image.Segments[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
    if (Mutation == 3)
      F.Image.DataPtrRelocSlots.insert(0x1048);
    if (Mutation == 4 || Mutation == 5) {
      auto Changed =
          std::make_shared<SourceCallTypeHint>(*Helper->SourceCallHint);
      Changed->ByteCount = Mutation == 4 ? 8 : 65537;
      Helper->SourceCallHint = std::move(Changed);
    }
    if (Mutation == 6)
      Load->Operands[0]->Operands[1]->Operands[1] =
          HighExpr::makeConst(4096, 8);
    const auto Allowed = readOnlyScalarSourceHelpers(Bound.Function, F.Image);
    EXPECT_TRUE(Allowed.empty()) << Mutation;
    EXPECT_FALSE(objcSourceCallBound(*Helper, F.Image, {}, nullptr, &Allowed))
        << Mutation;
  }
}

TEST(ObjCSourceBindings,
     ScalarPointerAmbiguityRequiresWidthAndSectionOwnership) {
  for (uint16_t Width : {1, 2, 4, 8}) {
    auto F = readonlyTableFixture(Width);
    EXPECT_EQ(isImagePointerBitPattern(F.Image, 0x1040, Width), Width == 8);
    EXPECT_FALSE(isImagePointerBitPattern(F.Image, 0, Width));
    F.Image.Sections[0].VA += 0x20;
    F.Image.Sections[0].FileOff += 0x20;
    F.Image.Sections[0].Size -= 0x20;
    F.Image.Sections[0].FileSz -= 0x20;
    EXPECT_FALSE(isImagePointerBitPattern(F.Image, 0x1010, Width));
    EXPECT_TRUE(F.Image.getSegmentFor(0x1010));
    EXPECT_FALSE(F.Image.getSectionFor(0x1010));
  }
  for (uint16_t Width : {2, 4}) {
    auto F = readonlyTableFixture(Width);
    llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 0x40,
                                     0x1040);
    F.Function.Body = {F.Function.Body[0].Body[0]};
    F.Function.Body[0].RetVal = HighExpr::makeLoad(
        HighExpr::makeConst(0x1040, 8), NdType::makeInt(Width, false));
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty());
    const auto &Value = Bound.Function.Body[0].RetVal;
    ASSERT_EQ(Value->Kind, ExprKind::BitCast);
    EXPECT_EQ(Value->Operands[0]->ConstVal, 0x1040U);
  }
}

TEST(ObjCSourceBindings,
     CStringPoolsPreserveInteriorOffsetsAndRevalidateStorage) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    Fixture F;
    F.Image.Arch = Architecture;
    F.Image.ObjCSourceReferences.clear();
    F.Image.Sections[0].Type = llvm::MachO::S_CSTRING_LITERALS;
    F.Function.ReturnType = NdType::makePtr(NdType::makeInt(1));
    const std::array<uint8_t, 9> Bytes{'a', 0,   'b',  '?', '?',
                                       '/', '"', '\\', 0xff};
    std::copy(Bytes.begin(), Bytes.end(), F.Image.Segments[0].Data.begin());
    for (va_t Address : {0x1000, 0x1001, 0x1002, 0x10ff}) {
      F.Function.Body[0].RetVal = HighExpr::makeConst(Address, 8);
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      EXPECT_EQ(Bound.CStringSections, std::set<va_t>{0x1000});
      auto Helper = Bound.Function.Body[0].RetVal;
      if (Address != 0x1000) {
        ASSERT_EQ(Helper->Kind, ExprKind::BinOp);
        EXPECT_EQ(Helper->Operands[1]->ConstVal, Address - 0x1000);
        Helper = Helper->Operands[0];
      }
      ASSERT_TRUE(Helper->SourceCallHint);
      EXPECT_EQ(Helper->SourceCallHint->CallKind,
                SourceCallTypeHint::Kind::RuntimeCStringStorage);
      EXPECT_TRUE(objcSourceCallBound(*Helper, F.Image, {}));
      auto Forged =
          std::make_shared<SourceCallTypeHint>(*Helper->SourceCallHint);
      Forged->ByteCount--;
      Helper->SourceCallHint = std::move(Forged);
      EXPECT_FALSE(objcSourceCallBound(*Helper, F.Image, {}));
    }
    std::set<std::string> Helpers;
    const auto Source = renderCStringStorageHelpers(F.Image, {0x1000}, Helpers);
    EXPECT_NE(Source.find("a\\000b\\077\\077/\\042\\134\\377"),
              std::string::npos);
    EXPECT_NE(Source.find("storage[256]"), std::string::npos);
    EXPECT_FALSE(objc_binding_detail::associationKeyHint(F.Image, 0x1002));
    F.Image.DataPtrRelocSlots.insert(0x10f8);
    EXPECT_FALSE(cstringStorageSourceHint(F.Image, 0x1000));
    EXPECT_THROW(renderCStringStorageHelpers(F.Image, {0x1000}, Helpers),
                 std::runtime_error);
  }
}

TEST(ObjCSourceBindings, CStringPoolsRejectPartialAmbiguousAndMutableStorage) {
  for (unsigned Case = 0; Case != 9; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F;
    F.Image.ObjCSourceReferences.clear();
    F.Image.Sections[0].Type = llvm::MachO::S_CSTRING_LITERALS;
    switch (Case) {
    case 0:
      F.Image.Sections[0].Type = llvm::MachO::S_REGULAR;
      break;
    case 1:
      F.Image.Segments[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      break;
    case 2:
      F.Image.Sections[0].FileSz--;
      break;
    case 3:
      F.Image.Sections.push_back(F.Image.Sections[0]);
      break;
    case 4:
      F.Image.ImportPtrSlots[0x10f8] = "_other";
      break;
    case 5:
      F.Image.MachOChainedFixupsAmbiguous = true;
      break;
    case 6:
      F.Image.IsRelocatable = true;
      break;
    case 7:
      F.Image.Sections[0].Size = 1024 * 1024 + 1;
      break;
    case 8:
      F.Image.Segments[0].Data.pop_back();
      break;
    }
    EXPECT_FALSE(cstringStorageSourceHint(F.Image, 0x1000));
  }
}

TEST(ObjCSourceBindings, CStringPoolsExecuteEveryByteWithoutChangingIdentity) {
  Fixture F;
  F.Image.ObjCSourceReferences.clear();
  F.Image.Sections[0].Type = llvm::MachO::S_CSTRING_LITERALS;
  for (unsigned I = 0; I != 256; ++I)
    F.Image.Segments[0].Data[I] = uint8_t(I);
  std::set<std::string> Helpers;
  std::string Source = "#include <stdint.h>\n" +
                       renderCStringStorageHelpers(F.Image, {0x1000}, Helpers);
  Source += R"(
int main(void) {
  const unsigned char *bytes =
      (const unsigned char *)neverd_cstring_storage_1000_address();
  for (unsigned i = 0; i != 256; ++i) {
    if (bytes[i] != i) return 1;
    const unsigned char *alias =
        (const unsigned char *)neverd_cstring_storage_1000_address() + i;
    if (alias != bytes + i || *alias != i) return 2;
  }
  return 0;
}
)";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-cstring-storage",
                                                    Directory));
  const std::filesystem::path Work(Directory.str().str());
  struct Cleanup {
    std::filesystem::path Work;
    ~Cleanup() {
      std::error_code Error;
      std::filesystem::remove_all(Work, Error);
    }
  } Cleanup{Work};
  const auto Path = (Work / "literal.c").string();
  const auto Executable = (Work / "literal").string();
  const auto ErrorPath = (Work / "stderr").string();
  std::ofstream(Path) << Source;
  const std::string Compiler = NEVERD_TEST_CLANG;
  const std::vector<std::string> Arguments{
      Compiler, "-std=c11", "-O3", "-Werror", Path, "-o", Executable};
  std::vector<llvm::StringRef> Refs(Arguments.begin(), Arguments.end());
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt,
                                                      std::nullopt, ErrorPath};
  std::string Error;
  const auto Status = llvm::sys::ExecuteAndWait(Compiler, Refs, std::nullopt,
                                                Redirects, 60, 0, &Error);
  auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
  ASSERT_EQ(Status, 0) << Error << (Errors ? (*Errors)->getBuffer().str() : "");
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}, std::nullopt,
                                      Redirects, 30, 0, &Error),
            0)
      << Error;
}

TEST(ObjCSourceBindings, ImmutableScalarsPreserveWidthSignAndFloatingBits) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const auto &Type :
         {NdType::makeInt(1, false), NdType::makeInt(2, true),
          NdType::makeInt(4, true), NdType::makeInt(8, false),
          NdType::makeFloat(4), NdType::makeFloat(8)}) {
      for (uint64_t Bits : std::array<uint64_t, 8>{
               0, 7, 0x8000000000000000ULL, 0x41323456789abcdeULL,
               0x7ff8000001234567ULL, 0x80000000, 0x7fc12345, UINT64_MAX}) {
        SCOPED_TRACE(Type->str());
        SCOPED_TRACE(Bits);
        Fixture F;
        F.Image.Arch = Architecture;
        F.Image.ObjCSourceReferences.clear();
        llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 0x40,
                                         Bits);
        F.Function.ReturnType = Type;
        auto Load = HighExpr::makeLoad(HighExpr::makeConst(0x1040, 8), Type);
        F.Function.Body[0].RetVal = Load;
        const auto Result = bindObjCSourceReferences(F.Function, F.Image);
        ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
        const auto Value = Result.Function.Body[0].RetVal;
        ASSERT_EQ(Value->Kind, ExprKind::BitCast);
        EXPECT_EQ(Value->Type->str(), Type->str());
        ASSERT_EQ(Value->Operands.size(), 1U);
        EXPECT_EQ(Value->Operands[0]->Kind, ExprKind::Const);
        EXPECT_EQ(Value->Operands[0]->Type->Size, Type->Size);
        EXPECT_EQ(Value->Operands[0]->ConstProvenance,
                  ConstantAddressProvenance::Scalar);
        const uint64_t Mask = Type->Size == 8
                                  ? UINT64_MAX
                                  : (uint64_t(1) << (Type->Size * 8)) - 1;
        EXPECT_EQ(Value->Operands[0]->ConstVal, Bits & Mask);
        EXPECT_EQ(Load->Kind, ExprKind::Load);
        EXPECT_EQ(Load->Operands[0]->ConstVal, 0x1040U);
      }
    }
  }
}

TEST(ObjCSourceBindings,
     ImmutableWideScalarsPreserveBothLanesAndRejectPointers) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Case = 0; Case != 7; ++Case) {
      SCOPED_TRACE(Case);
      Fixture F;
      F.Image.Arch = Architecture;
      F.Image.ObjCSourceReferences.clear();
      uint64_t Low = 0xfedcba9876543210ULL, High = 0x8123456789abcdefULL;
      if (Case == 1)
        Low = 0x1040;
      if (Case == 2)
        High = 0x1040;
      llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 0x40,
                                       Low);
      llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 0x48,
                                       High);
      if (Case == 3)
        F.Image.DataPtrRelocSlots.insert(0x1048);
      if (Case == 4)
        F.Image.Sections[0].FileSz = 0x4f;
      if (Case == 5)
        F.Image.Sections[0].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
      F.Function.ReturnType = NdType::makeInt(16, Case == 6);
      auto Load = HighExpr::makeLoad(HighExpr::makeConst(0x1040, 8),
                                     F.Function.ReturnType);
      F.Function.Body[0].RetVal = Load;
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      if (Case >= 1 && Case <= 5) {
        EXPECT_FALSE(Bound.Limitation.empty());
        continue;
      }
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      const auto Value = Bound.Function.Body[0].RetVal;
      ASSERT_EQ(Value->Kind, ExprKind::BitCast);
      EXPECT_EQ(Value->Type->str(), F.Function.ReturnType->str());
      const auto Joined = Value->Operands[0];
      ASSERT_EQ(Joined->Kind, ExprKind::BinOp);
      ASSERT_EQ(Joined->Op, NdOp::CONCAT);
      EXPECT_EQ(Joined->Type->Size, 16U);
      EXPECT_EQ(Joined->Operands[0]->ConstVal, High);
      EXPECT_EQ(Joined->Operands[1]->ConstVal, Low);
      for (const auto &Lane : Joined->Operands)
        EXPECT_EQ(Lane->ConstProvenance, ConstantAddressProvenance::Scalar);
      EXPECT_EQ(Load->Kind, ExprKind::Load);
    }
  }
}

TEST(ObjCSourceBindings, ImmutableScalarsRejectUnprovedStorageAndAddressUses) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Case = 0; Case < 22; ++Case) {
      SCOPED_TRACE(Case);
      Fixture F;
      F.Image.Arch = Architecture;
      F.Image.ObjCSourceReferences.clear();
      const auto Type = NdType::makeInt(8, false);
      F.Function.ReturnType = Type;
      auto Load = HighExpr::makeLoad(HighExpr::makeConst(0x1040, 8), Type);
      F.Function.Body[0].RetVal = Load;
      llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 0x40,
                                       7);
      switch (Case) {
      case 0:
        F.Image.Sections[0].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        break;
      case 1:
        F.Image.Segments[0].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        break;
      case 2:
        F.Image.Sections[0].FileSz = 0x47;
        break;
      case 3:
        F.Image.Segments[0].Data.resize(0x47);
        break;
      case 4:
        F.Image.Sections.push_back(F.Image.Sections[0]);
        break;
      case 5:
        F.Image.Segments.push_back(F.Image.Segments[0]);
        break;
      case 6:
        F.Image.Sections[0].Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
        F.Image.Sections[0].Flags =
            SegmentFlags::Readable | SegmentFlags::Executable;
        break;
      case 7:
        F.Image.BaseRelocations.push_back({0x1039, 0});
        break;
      case 8:
        F.Image.MachOResolvedChainedPointerSlots.insert(0x1040);
        break;
      case 9:
        F.Image.MachOChainedFixupsAmbiguous = true;
        break;
      case 10:
        Load->Type = NdType::makePtr(NdType::makeVoid());
        break;
      case 11:
        Load->Type = NdType::makeFloat(16);
        break;
      case 12:
        Load->Type = NdType::makeInt(32, false);
        break;
      case 13:
        Load->MemoryOrdering = NdMemoryOrdering::Acquire;
        break;
      case 14:
        Load->MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
        break;
      case 15:
        llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 0x40,
                                         0x1040);
        break;
      case 16:
        F.Function.ReturnType = NdType::makePtr(NdType::makeVoid());
        break;
      case 17:
        // A shared load may fold in a numeric occurrence, but that proof
        // cannot authorize the same node as a memory address.
        F.Function.Body[0].RetVal = HighExpr::makeBinop(
            NdOp::INT_ADD, Load, HighExpr::makeLoad(Load, Type));
        break;
      case 18:
        F.Image.IsRelocatable = true;
        break;
      case 19:
        F.Image.Segments[0].FileSz = 0x47;
        break;
      case 20:
        F.Image.DataPtrRelocSlots.insert(0x1047);
        break;
      case 21:
        Load->Type = NdType::makeInt(3, false);
        break;
      }
      const auto Result = bindObjCSourceReferences(F.Function, F.Image);
      EXPECT_FALSE(Result.Limitation.empty());
      EXPECT_EQ(Load->Kind, ExprKind::Load);
      if (Case == 17)
        EXPECT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind,
                  ExprKind::BitCast);
    }
  }
}

namespace {
struct SwiftLiteralFixture : Fixture {
  static constexpr va_t ImportSlot = 0x10e0;
  static constexpr va_t Contents = 0x1040;
  std::string Text;
  ExprPtr Call;
  SwiftLiteralFixture(Arch Architecture, bool Unicode = false) {
    Image.Arch = Architecture;
    Image.ObjCSourceReferences.clear();
    Text = Unicode ? "literal caf\xc3\xa9 bytes"
                   : "immutable compiler literal bytes";
    std::copy(Text.begin(), Text.end(), Image.Segments[0].Data.begin() + 0x40);
    const std::string Name =
        "_$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF";
    Image.ImportPtrSlots[ImportSlot] = Name;
    Image.recordDyldBindSlot(
        ImportSlot, Name, 0,
        "/System/Library/Frameworks/Foundation.framework/Foundation", false);
    const uint64_t Flags =
        Unicode ? UINT64_C(0x1000000000000000) : UINT64_C(0xd000000000000000);
    const auto Storage = HighExpr::makeBinop(
        NdOp::INT_OR, HighExpr::makeConst(Contents - 32, 8),
        HighExpr::makeConst(UINT64_C(0x8000000000000000), 8));
    Call = HighExpr::makeCall(
        Name, ImportSlot,
        {HighExpr::makeConst(Flags | Text.size(), 8), Storage});
    Call->Type = NdType::makePtr(NdType::makeVoid());
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(
        *swiftStringSourceCallHint(Image, ImportSlot));
    Function.ReturnType = Call->Type;
    Function.Body[0].RetVal = Call;
  }
};
} // namespace

TEST(ObjCSourceBindings, SwiftLiteralStoragePreservesBytesAndConsumerIdentity) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool Unicode : {false, true}) {
      SwiftLiteralFixture F(Architecture, Unicode);
      const auto Result = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
      const BorrowedByteRange Range{F.Contents, F.Text.size() + 1};
      EXPECT_EQ(Result.BorrowedBytes, std::set<BorrowedByteRange>{Range});
      const auto Bound = Result.Function.Body[0].RetVal;
      EXPECT_EQ(Bound->SourceCallHint->CallKind,
                SourceCallTypeHint::Kind::SwiftStringBridge);
      EXPECT_TRUE(objcSourceCallBound(*Bound, F.Image, {}));
      ASSERT_NE(
          objc_binding_detail::swiftLiteralStorageHelper(Bound->Operands[1]),
          nullptr);
      EXPECT_EQ(F.Call->Operands[1]->Operands[0]->Kind, ExprKind::Const);
      std::set<std::string> Shared;
      const auto Helpers =
          renderBorrowedByteHelpers(F.Image, Result.BorrowedBytes, Shared);
      EXPECT_EQ(Shared, std::set<std::string>{borrowedByteHelperName(Range)});
      EXPECT_NE(Helpers.find("static const unsigned char bytes[]"),
                std::string::npos);
      // A raw occurrence outside the established bridge must retain its own
      // address-binding failure, even when it shares the original node.
      F.Function.Body[0].RetVal =
          HighExpr::makeBinop(NdOp::INT_OR, F.Call, F.Call->Operands[1]);
      EXPECT_FALSE(
          bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
    }
  }
}

TEST(ObjCSourceBindings,
     SwiftLiteralStorageRejectsChangedWordsStorageAndImports) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 17; ++Mutation) {
      SCOPED_TRACE(Mutation);
      SwiftLiteralFixture F(Architecture);
      auto &Word = F.Call->Operands[0]->ConstVal;
      switch (Mutation) {
      case 0:
        Word |= UINT64_C(0x2000000000000000);
        break;
      case 1:
        F.Call->Operands[1]->Operands[1]->ConstVal = 0;
        break;
      case 2:
        ++Word;
        break;
      case 3:
        --Word;
        break;
      case 4:
        F.Image.Segments[0].Data[0x44] = 0;
        break;
      case 5:
        F.Image.Segments[0].Data[0x44] = 0xff;
        break;
      case 6:
        F.Image.Sections[0].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        break;
      case 7:
        F.Image.Segments[0].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        break;
      case 8:
        F.Image.DataPtrRelocSlots.insert(F.Contents - 1);
        break;
      case 9:
        F.Image.Sections[0].FileSz = 0x48;
        break;
      case 10:
        F.Image.DyldBindSlots[F.ImportSlot].Module =
            "/tmp/libswiftFoundation.dylib";
        break;
      case 11:
        F.Image.DyldBindSlots[F.ImportSlot].WeakImport = true;
        break;
      case 12:
        F.Image.DyldBindSlots[F.ImportSlot].Addend = 8;
        break;
      case 13:
        F.Image.MachOChainedFixupsAmbiguous = true;
        break;
      case 14:
        F.Call->Operands[0]->Type = NdType::makeFloat(8);
        break;
      case 15:
        F.Call->IsIndirectCall = true;
        break;
      case 16:
        F.Call->MemoryOrdering = NdMemoryOrdering::Acquire;
        break;
      }
      const auto Result = bindObjCSourceReferences(F.Function, F.Image);
      EXPECT_FALSE(Result.Limitation.empty());
      EXPECT_TRUE(Result.BorrowedBytes.empty());
    }
    for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
      SCOPED_TRACE(Mutation);
      SwiftLiteralFixture F(Architecture);
      auto Result = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Result.Limitation.empty());
      auto Bound = Result.Function.Body[0].RetVal;
      auto Helper = Bound->Operands[1]->Operands[0]->Operands[0];
      auto ChangedHint =
          std::make_shared<SourceCallTypeHint>(*Helper->SourceCallHint);
      Helper->SourceCallHint = ChangedHint;
      switch (Mutation) {
      case 0:
        ++Bound->Operands[0]->ConstVal;
        break;
      case 1:
        ++ChangedHint->TargetAddress;
        break;
      case 2:
        --ChangedHint->ByteCount;
        break;
      case 3:
        Bound->Operands[1]->Operands[1]->ConstVal = 0;
        break;
      case 4:
        Bound->Operands[1]->Operands[0]->Operands[1]->ConstVal = 16;
        break;
      case 5:
        F.Image.DyldBindSlots[F.ImportSlot].Module = "/tmp/other.dylib";
        break;
      case 6:
        Helper->IsIndirectCall = true;
        break;
      case 7:
        Bound->IsIndirectCall = true;
        break;
      case 8:
        Bound->Operands[1]->Type = NdType::makeFloat(8);
        break;
      }
      EXPECT_FALSE(objcSourceCallBound(*Bound, F.Image, {}));
    }
  }
}

namespace {
struct ConstantStringFixture {
  BinaryImage Image;
  HighFunc Function;
  ConstantStringFixture() {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Segment Objects;
    Objects.Name = "__DATA_CONST";
    Objects.VA = 0x2000;
    Objects.Size = Objects.FileSz = 64;
    Objects.Flags = SegmentFlags::Readable;
    Objects.Data.resize(64);
    Image.Segments.push_back(Objects);
    Section Records;
    Records.Name = "__cfstring";
    Records.SegmentName = Objects.Name;
    Records.VA = Objects.VA;
    Records.Size = Records.FileSz = Objects.Size;
    Records.Flags = Objects.Flags;
    Image.Sections.push_back(Records);
    Segment Text;
    Text.Name = "__TEXT";
    Text.VA = Text.FileOff = 0x1000;
    Text.Size = Text.FileSz = 0x200;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(0x200);
    Image.Segments.push_back(Text);
    Section ASCII;
    ASCII.Name = "__cstring";
    ASCII.SegmentName = Text.Name;
    ASCII.VA = ASCII.FileOff = Text.VA;
    ASCII.Size = ASCII.FileSz = 0x100;
    ASCII.Flags = Text.Flags;
    ASCII.Type = llvm::MachO::S_CSTRING_LITERALS;
    Image.Sections.push_back(ASCII);
    Section Unicode = ASCII;
    Unicode.Name = "__ustring";
    Unicode.VA = Unicode.FileOff = 0x1100;
    Unicode.Type = llvm::MachO::S_REGULAR;
    Image.Sections.push_back(Unicode);
    for (unsigned I = 0; I < 2; ++I) {
      const va_t Address = 0x2000 + I * 32;
      Image.recordDyldBindSlot(
          Address, "___CFConstantStringClassReference", 0,
          "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
          false);
      auto *Record = Image.Segments[0].Data.data() + I * 32;
      llvm::support::endian::write64le(Record + 8, I ? 0x7d0 : 0x7c8);
      llvm::support::endian::write64le(Record + 16, I ? 0x1100 : 0x1000);
      llvm::support::endian::write64le(Record + 24, I ? 4 : 3);
    }
    Image.Segments[1].Data[0] = 'a';
    Image.Segments[1].Data[1] = '\n';
    Image.Segments[1].Data[2] = '"';
    const uint16_t Units[] = {0x767e, 0, 0xd83d, 0xde00, 0};
    for (unsigned I = 0; I < 5; ++I)
      llvm::support::endian::write16le(
          Image.Segments[1].Data.data() + 0x100 + I * 2, Units[I]);
    Function.ReturnType = NdType::makePtr(NdType::makeVoid());
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = HighExpr::makeConst(0x2000, 8);
    Function.Body.push_back(Return);
  }
};
} // namespace

namespace {
ConstantStringFixture immutablePointerFixture(Arch Architecture, bool Chained) {
  ConstantStringFixture F;
  F.Image.Arch = Architecture;
  F.Image.MachOHasChainedFixups = Chained;
  F.Image.MachOResolvedChainedPointerSlots = {0x2000, 0x2010, 0x2020,
                                              0x2030, 0x3000, 0x3008};
  Segment Slots;
  // Deliberately unrelated names: the loader flag, not spelling, is proof.
  Slots.Name = "pointer_storage";
  Slots.VA = Slots.FileOff = 0x3000;
  Slots.Size = Slots.FileSz = 32;
  Slots.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Slots.ReadOnlyAfterRelocations = true;
  Slots.Data.resize(32);
  llvm::support::endian::write64le(Slots.Data.data(), 0x2020);
  llvm::support::endian::write64le(Slots.Data.data() + 8, 0x2020);
  Slots.Data[16] = 42;
  F.Image.Segments.push_back(Slots);
  Section Section;
  Section.Name = "pointers";
  Section.SegmentName = Slots.Name;
  Section.VA = Section.FileOff = Slots.VA;
  Section.Size = Section.FileSz = Slots.Size;
  Section.Flags = Slots.Flags;
  F.Image.Sections.push_back(Section);
  F.Image.DataPtrRelocSlots = {0x3000, 0x3008};
  F.Image.DataPtrRelocTargetOwners = {{0x3000, 0x2000}, {0x3008, 0x2000}};
  F.Function.Body[0].RetVal = HighExpr::makeLoad(
      HighExpr::makeConst(0x3000, 8), NdType::makePtr(NdType::makeVoid()));
  return F;
}
} // namespace

TEST(ObjCSourceBindings, MutableStringPointersKeepTheirSharedStorage) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (bool Chained : {false, true}) {
      auto F = immutablePointerFixture(Architecture, Chained);
      F.Image.Segments[2].ReadOnlyAfterRelocations = false;
      F.Image.Symbols.push_back({"_mutableString", 0x3000, 8, false});
      F.Function.Body[0].RetVal->Type = NdType::makeInt(8, false);
      EXPECT_FALSE(readImmutableImagePointer(F.Image, 0x3000));
      EXPECT_EQ(readInitialImagePointer(F.Image, 0x3000), 0x2020U);
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      const auto Load = Bound.Function.Body[0].RetVal;
      ASSERT_EQ(Load->Kind, ExprKind::Load);
      const auto Helper = Load->Operands[0];
      ASSERT_TRUE(Helper->SourceCallHint);
      EXPECT_EQ(Helper->SourceCallHint->CallKind,
                SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
      EXPECT_EQ(Helper->SourceCallHint->TargetAddress, 0x3000U);
      EXPECT_TRUE(objcSourceCallBound(*Helper, F.Image, {}));
      EXPECT_EQ(Bound.LocalStorageExtents,
                (std::map<va_t, uint64_t>{{0x3000, 8}}));
      std::set<std::string> Helpers;
      const auto Source = renderObjCLocalStorageHelpers(
          F.Image, Bound.LocalStorageExtents, Helpers);
      EXPECT_NE(
          Source.find("storage = neverd_objc_constant_string_2020_address()"),
          std::string::npos);
      EXPECT_NE(Source.find("return (uintptr_t)&storage"), std::string::npos);
      F.Image.DataPtrRelocTargetOwners.erase(0x3000);
      EXPECT_FALSE(objcSourceCallBound(*Helper, F.Image, {}));
      EXPECT_THROW(renderObjCLocalStorageHelpers(
                       F.Image, Bound.LocalStorageExtents, Helpers),
                   std::runtime_error);
    }
}

TEST(ObjCSourceBindings, MutableStringPointersRejectUnprovedInitializers) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Case = 0; Case != 13; ++Case) {
      SCOPED_TRACE(Case);
      auto F = immutablePointerFixture(Architecture, true);
      F.Image.Segments[2].ReadOnlyAfterRelocations = false;
      F.Image.Symbols.push_back({"_mutableString", 0x3000, 8, false});
      F.Function.Body[0].RetVal->Type = NdType::makeInt(8, false);
      switch (Case) {
      case 0:
        F.Image.Symbols.clear();
        break;
      case 1:
        F.Image.DataPtrRelocSlots.erase(0x3000);
        break;
      case 2:
        F.Image.DataPtrRelocTargetOwners.erase(0x3000);
        break;
      case 3:
        F.Image.DataPtrRelocTargetOwners[0x3000] = 0x1000;
        break;
      case 4:
        F.Image.MachOResolvedChainedPointerSlots.erase(0x3000);
        break;
      case 5:
        F.Image.DyldBindSlots[0x3000] = {};
        break;
      case 6:
        F.Image.DataPtrRelocSlots.insert(0x3004);
        break;
      case 7:
        F.Image.Segments[2].FileSz = 7;
        break;
      case 8:
        F.Image.Sections.push_back(F.Image.Sections[3]);
        break;
      case 9:
        F.Function.Body[0].RetVal->Type = NdType::makeInt(4);
        break;
      case 10:
        F.Function.Body[0].RetVal->Type = NdType::makeInt(16);
        break;
      case 11:
        F.Image.Symbols[0].Size = 4;
        break;
      case 12:
        llvm::support::endian::write64le(F.Image.Segments[2].Data.data(),
                                         0x2040);
        break;
      }
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      EXPECT_FALSE(Bound.Limitation.empty());
      EXPECT_TRUE(Bound.LocalStorageExtents.empty());
    }
}

TEST(ObjCSourceBindings, StrongStoresAuthenticateTheirSharedPointerCell) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    auto F = immutablePointerFixture(Architecture, true);
    F.Image.Segments[2].ReadOnlyAfterRelocations = false;
    F.Image.Symbols.push_back({"_mutableString", 0x3000, 8, false});
    F.Image.recordDyldBindSlot(0x3100, "_objc_storeStrong", 0,
                               "/usr/lib/libobjc.A.dylib", false);
    F.Image.ImportPtrSlots[0x3100] = "_objc_storeStrong";
    const auto Hint = objcRuntimeSourceCallHint(F.Image, 0x3100);
    ASSERT_TRUE(Hint);
    auto Call = HighExpr::makeCall(
        "objc_storeStrong", 0x3100,
        {HighExpr::makeConst(0x3000, 8), HighExpr::makeConst(0, 8)});
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    Call->Type = NdType::makeVoid();
    F.Function.Body[0].RetVal = Call;
    auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    const auto Storage = Bound.Function.Body[0].RetVal->Operands[0];
    ASSERT_TRUE(Storage->SourceCallHint);
    EXPECT_EQ(Storage->SourceCallHint->TargetAddress, 0x3000U);
    EXPECT_EQ(Storage->SourceCallHint->ByteCount, 8U);
    EXPECT_TRUE(objcSourceCallBound(*Storage, F.Image, {}));
    auto Forged = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
    Forged->Signature.Parameters[0].Location.RegisterOffset += 8;
    Call->SourceCallHint = std::move(Forged);
    Bound = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_FALSE(
        objcSourceCallBound(*Bound.Function.Body[0].RetVal, F.Image, {}));
    EXPECT_EQ(Bound.Function.Body[0].RetVal->Operands[0]->Kind,
              ExprKind::Const);
    EXPECT_TRUE(Bound.LocalStorageExtents.empty());
  }
}

TEST(ObjCSourceBindings, ImmutablePointerLoadsShareTheTargetObjectIdentity) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (bool Chained : {false, true}) {
      auto F = immutablePointerFixture(Architecture, Chained);
      EXPECT_EQ(readImmutableImagePointer(F.Image, 0x3000), 0x2020U);
      EXPECT_FALSE(readImmutableImageBytes(F.Image, 0x3000, 8));
      const auto Scalar = readImmutableImageBytes(F.Image, 0x3010, 8);
      ASSERT_TRUE(Scalar);
      EXPECT_EQ((*Scalar)[0], 42U);
      for (bool Reversed : {false, true}) {
        F.Function.Body.resize(1);
        for (auto Address : {0x3000U, 0x3008U, 0x2020U}) {
          HighStmt Return;
          Return.Kind = StmtKind::Return;
          Return.RetVal = HighExpr::makeConst(Address, 8);
          if (Address != 0x2020)
            Return.RetVal =
                HighExpr::makeLoad(Return.RetVal, NdType::makeInt(8));
          F.Function.Body.push_back(Return);
        }
        if (Reversed)
          std::reverse(F.Function.Body.begin(), F.Function.Body.end());
        const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
        ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
        EXPECT_EQ(Bound.ConstantStrings, std::set<va_t>{0x2020});
        for (const auto &Statement : Bound.Function.Body) {
          ASSERT_TRUE(Statement.RetVal->SourceCallHint);
          EXPECT_EQ(Statement.RetVal->SourceCallHint->TargetAddress, 0x2020U);
          EXPECT_TRUE(objcSourceCallBound(*Statement.RetVal, F.Image, {}));
        }
        EXPECT_EQ(F.Function.Body[1].RetVal->Kind, ExprKind::Load);
      }
    }
}

TEST(ObjCSourceBindings, ImmutablePointerLoadsRejectIncompleteAndStaleProofs) {
  using Mutation = std::function<void(BinaryImage &)>;
  const std::vector<Mutation> Mutations = {
      [](auto &I) { I.Segments[2].ReadOnlyAfterRelocations = false; },
      [](auto &I) {
        I.Segments[2].ReadOnlyAfterRelocations = false;
        I.Segments[2].Name = I.Sections[3].SegmentName = "__DATA_CONST";
      },
      [](auto &I) { I.Format = BinaryFormat::ELF; },
      [](auto &I) { I.IsRelocatable = true; },
      [](auto &I) { I.Bits = Bitness::Bits32; },
      [](auto &I) { I.MachOChainedFixupsAmbiguous = true; },
      [](auto &I) { I.MachOResolvedChainedPointerSlots.erase(0x3000); },
      [](auto &I) { I.DataPtrRelocSlots.erase(0x3000); },
      [](auto &I) { I.DataPtrRelocTargetOwners.erase(0x3000); },
      [](auto &I) { I.DataPtrRelocTargetOwners[0x3000] = 0x2020; },
      [](auto &I) { I.DataPtrRelocTargetOwners[0x3000] = 0x1000; },
      [](auto &I) { I.DataPtrRelocSlots.insert(0x2ff9); },
      [](auto &I) { I.DataPtrRelocSlots.insert(0x3007); },
      [](auto &I) { I.MachOResolvedChainedPointerSlots.insert(0x3001); },
      [](auto &I) { I.CodePtrRelocSlots.insert(0x3000); },
      [](auto &I) { I.RelDataPtrRelocSlots.insert(0x3000); },
      [](auto &I) { I.RelCodeRelocSlots.insert(0x3000); },
      [](auto &I) { I.ConflictingImportStorageSlots.insert(0x3000); },
      [](auto &I) { I.ImportPtrSlots[0x3000] = "_other"; },
      [](auto &I) { I.ImportStorageSlots[0x3000] = {}; },
      [](auto &I) { I.DyldBindSlots[0x3000] = {}; },
      [](auto &I) { I.ObjCSourceReferences[0x3000] = {}; },
      [](auto &I) { I.DataAddressRelocOperands[0x3000] = {}; },
      [](auto &I) { I.CodeAddressRelocOperands[0x3000] = {}; },
      [](auto &I) { I.Relocations.push_back({0x3000}); },
      [](auto &I) { I.BaseRelocations.push_back({0x3000}); },
      [](auto &I) { I.Sections[3].Flags = SegmentFlags::None; },
      [](auto &I) { I.Segments[2].Flags = SegmentFlags::None; },
      [](auto &I) { I.Sections[3].FileSz = 7; },
      [](auto &I) { I.Segments[2].FileSz = 7; },
      [](auto &I) { I.Segments[2].Data.resize(7); },
      [](auto &I) { I.Sections[3].FileOff++; },
      [](auto &I) { I.Sections[3].Type = llvm::MachO::S_ZEROFILL; },
      [](auto &I) {
        I.Sections[3].Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
      },
      [](auto &I) { I.Sections.push_back(I.Sections[3]); },
      [](auto &I) { I.Segments.push_back(I.Segments[2]); },
      [](auto &I) { I.Sections.push_back(I.Sections[0]); },
      [](auto &I) { I.Segments.push_back(I.Segments[0]); },
      [](auto &I) { I.Segments[2].Data[0] = 0x40; },
      [](auto &I) { I.Segments[2].Data[0] = 0x21; },
      [](auto &I) { I.Segments[0].Data[40] = 0; },
      [](auto &I) {
        I.Raw.resize(32);
        llvm::support::endian::write32le(I.Raw.data(),
                                         llvm::MachO::MH_MAGIC_64);
        llvm::support::endian::write32le(I.Raw.data() + 8,
                                         llvm::MachO::CPU_SUBTYPE_ARM64E);
      },
  };
  for (size_t Index = 0; Index < Mutations.size(); ++Index) {
    SCOPED_TRACE(Index);
    auto F = immutablePointerFixture(Arch::AArch64, true);
    const auto Before = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Before.Limitation.empty());
    Mutations[Index](F.Image);
    // Projection may encounter a different runtime-reference binding or
    // defer an unmapped address to source validation. Neither can authorize
    // the constant object supplied by this immutable-pointer proof.
    EXPECT_TRUE(
        bindObjCSourceReferences(F.Function, F.Image).ConstantStrings.empty());
    EXPECT_FALSE(
        objcSourceCallBound(*Before.Function.Body[0].RetVal, F.Image, {}));
  }
  auto F = immutablePointerFixture(Arch::X64, false);
  const auto Before = bindObjCSourceReferences(F.Function, F.Image);
  // A new valid target in the same owner range must invalidate the old hint.
  llvm::support::endian::write64le(F.Image.Segments[2].Data.data(), 0x2000);
  EXPECT_FALSE(
      objcSourceCallBound(*Before.Function.Body[0].RetVal, F.Image, {}));
  const auto After = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_TRUE(After.Limitation.empty());
  EXPECT_EQ(After.ConstantStrings, std::set<va_t>{0x2000});
}

TEST(ObjCSourceBindings, ImmutablePointerProofAppliesOnlyToOrdinaryFullLoads) {
  for (unsigned Variant = 0; Variant < 6; ++Variant) {
    SCOPED_TRACE(Variant);
    auto F = immutablePointerFixture(Arch::AArch64, true);
    auto &Load = F.Function.Body[0].RetVal;
    if (Variant == 0)
      Load = Load->Operands[0];
    if (Variant == 1)
      Load->Type = NdType::makeInt(4);
    if (Variant == 2)
      Load->Type = NdType::makeFloat(8);
    if (Variant == 3)
      Load->MemoryOrdering = NdMemoryOrdering::Acquire;
    if (Variant == 4)
      Load->MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
    if (Variant == 5)
      Load = HighExpr::makeLoad(Load, NdType::makeInt(8));
    EXPECT_FALSE(
        bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
  }
}

namespace {
struct ConstantObjectFixture : ConstantStringFixture {
  void storage(va_t Address, size_t Size, const char *Name) {
    Segment S;
    S.Name = "__DATA_CONST";
    S.VA = S.FileOff = Address;
    S.Size = S.FileSz = Size;
    S.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    S.ReadOnlyAfterRelocations = true;
    S.Data.resize(Size);
    Image.Segments.push_back(S);
    Section R;
    R.Name = Name;
    R.SegmentName = S.Name;
    R.VA = R.FileOff = Address;
    R.Size = R.FileSz = Size;
    R.Flags = S.Flags;
    Image.Sections.push_back(R);
  }
  void word(va_t Address, uint64_t Value) {
    for (auto &S : Image.Segments)
      if (Address >= S.VA && Address - S.VA < S.Size) {
        llvm::support::endian::write64le(S.Data.data() + Address - S.VA, Value);
        return;
      }
    FAIL() << "unmapped fixture address";
  }
  void pointer(va_t Slot, va_t Target) {
    word(Slot, Target);
    Image.DataPtrRelocSlots.insert(Slot);
    Image.DataPtrRelocTargetOwners[Slot] = Image.getSectionFor(Target)->VA;
    Image.MachOResolvedChainedPointerSlots.insert(Slot);
  }
  ConstantObjectFixture(Arch Architecture = Arch::AArch64,
                        bool Chained = false) {
    Image.Arch = Architecture;
    Image.MachOHasChainedFixups = Chained;
    Image.MachOResolvedChainedPointerSlots = {0x2000, 0x2010, 0x2020, 0x2030};
    pointer(0x2010, 0x1000);
    pointer(0x2030, 0x1100);
    storage(0x3000, 48, "__objc_arrayobj");
    storage(0x4000, 48, "__objc_intobj");
    storage(0x5000, 40, "__objc_dictobj");
    storage(0x6000, 512, "__const");
    for (auto Address : {0x3000U, 0x3018U, 0x4000U, 0x4018U, 0x5000U}) {
      const char *Name = Address < 0x4000 ? "_OBJC_CLASS_$_NSConstantArray"
                         : Address < 0x5000
                             ? "_OBJC_CLASS_$_NSConstantIntegerNumber"
                             : "_OBJC_CLASS_$_NSConstantDictionary";
      EXPECT_TRUE(Image.recordDyldBindSlot(
          Address, Name, 0,
          "/System/Library/Frameworks/Foundation.framework/Foundation", false));
      Image.MachOResolvedChainedPointerSlots.insert(Address);
    }
    Image.Segments[1].Data[8] = 'q';
    Image.Segments[1].Data[10] = 'Q';
    pointer(0x4008, 0x1008);
    word(0x4010, uint64_t(-12345));
    pointer(0x4020, 0x100a);
    word(0x4028, UINT64_C(0xfedcba9876543210));
    word(0x3008, 3);
    pointer(0x3010, 0x6000);
    pointer(0x6000, 0x2000);
    pointer(0x6008, 0x2020);
    pointer(0x6010, 0x2000);
    word(0x3020, 3);
    pointer(0x3028, 0x6018);
    pointer(0x6018, 0x3000);
    pointer(0x6020, 0x4000);
    pointer(0x6028, 0x5000);
    word(0x5008, 1);
    word(0x5010, 2);
    pointer(0x5018, 0x6030);
    pointer(0x6030, 0x2000);
    pointer(0x6038, 0x2020);
    pointer(0x5020, 0x6040);
    pointer(0x6040, 0x3000);
    pointer(0x6048, 0x4000);
    pointer(0x6050, 0x3018);
    Function.Body[0].RetVal = HighExpr::makeConst(0x3018, 8);
  }
};
} // namespace

TEST(ObjCSourceBindings, ConstantObjectGraphsPreserveNestedAliasesAndStrings) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Chained : {false, true}) {
      ConstantObjectFixture F(Architecture, Chained);
      const auto Graph = readObjCConstantObjectGraph(F.Image, 0x3018);
      ASSERT_TRUE(Graph);
      EXPECT_EQ(Graph->size(), 6U);
      EXPECT_EQ(Graph->at(0x3000).Elements,
                (std::vector<va_t>{0x2000, 0x2020, 0x2000}));
      EXPECT_EQ(Graph->at(0x5000).Keys, (std::vector<va_t>{0x2000, 0x2020}));
      EXPECT_EQ(Graph->at(0x5000).Elements,
                (std::vector<va_t>{0x3000, 0x4000}));
      EXPECT_EQ(Graph->at(0x4000).Encoding, 'q');
      EXPECT_EQ(Graph->at(0x4000).Bits, uint64_t(-12345));
      EXPECT_EQ(Graph->at(0x2020).String.Units,
                (std::vector<uint16_t>{0x767e, 0, 0xd83d, 0xde00}));
      const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      EXPECT_EQ(Bound.ConstantObjects, std::set<va_t>{0x3018});
      EXPECT_TRUE(
          objcSourceCallBound(*Bound.Function.Body[0].RetVal, F.Image, {}));
      std::set<std::string> Shared;
      const auto Source = renderObjCConstantObjectHelpers(
          F.Image, {0x3018, 0x4018}, {0x2000}, Shared);
      EXPECT_EQ(Shared.size(), 8U);
      EXPECT_EQ(Shared.count("neverd_cstring_storage_1000_address"), 1U);
      EXPECT_EQ(Shared.count("neverd_objc_constant_string_2000_address"), 1U);
      EXPECT_NE(Source.find("UINT64_C(0xfedcba9876543210)"), std::string::npos);
      EXPECT_EQ(F.Function.Body[0].RetVal->Kind, ExprKind::Const);
    }
}

TEST(ObjCSourceBindings,
     ConstantObjectGraphsRejectConflictingOrMutableStorage) {
  using Mutation = std::function<void(ConstantObjectFixture &)>;
  const std::vector<Mutation> Mutations = {
      [](auto &F) { F.Image.DyldBindSlots.at(0x3018).WeakImport = true; },
      [](auto &F) { F.Image.DyldBindSlots.at(0x3018).Addend = 8; },
      [](auto &F) {
        F.Image.DyldBindSlots.at(0x3018).Module = "/tmp/Foundation";
      },
      [](auto &F) { F.Image.DyldBindSlots.at(0x3018).Name = "_unknown"; },
      [](auto &F) { F.Image.ImportStorageSlots.at(0x3018).Addend = 8; },
      [](auto &F) { F.Image.ImportPtrSlots[0x3018] = "_unknown"; },
      [](auto &F) { F.Image.CodePtrRelocSlots.insert(0x3014); },
      [](auto &F) { F.Image.RelDataPtrRelocSlots.insert(0x3018); },
      [](auto &F) { F.Image.DataPtrRelocSlots.insert(0x3018); },
      [](auto &F) { F.Image.DataPtrRelocTargetOwners[0x3018] = 0x3000; },
      [](auto &F) {
        F.Image.DyldBindSlots[0x3017] = F.Image.DyldBindSlots.at(0x3018);
      },
      [](auto &F) { F.Image.ConflictingImportStorageSlots.insert(0x301a); },
      [](auto &F) { F.Image.Relocations.push_back({0x3018}); },
      [](auto &F) { F.Image.BaseRelocations.push_back({0x3014}); },
      [](auto &F) { F.Image.RuntimeCallablePointerSlots.push_back({0x3014}); },
      [](auto &F) { F.Image.DyldBindSlots.erase(0x3018); },
      [](auto &F) { F.Image.MachOResolvedChainedPointerSlots.erase(0x6028); },
      [](auto &F) { F.Image.DataPtrRelocTargetOwners.erase(0x6028); },
      [](auto &F) { F.Image.DataPtrRelocTargetOwners[0x6028] = 0x4000; },
      [](auto &F) { F.Image.ImportPtrSlots[0x6028] = "_foreignObject"; },
      [](auto &F) { F.Image.Segments[2].ReadOnlyAfterRelocations = false; },
      [](auto &F) { F.Image.Segments[5].ReadOnlyAfterRelocations = false; },
      [](auto &F) { F.Image.Sections[3].Type = llvm::MachO::S_ZEROFILL; },
      [](auto &F) { F.Image.Sections[3].FileSz = 47; },
      [](auto &F) { F.Image.Sections[3].FileOff++; },
      [](auto &F) { F.Image.Sections.push_back(F.Image.Sections[3]); },
      [](auto &F) { F.Image.Segments.push_back(F.Image.Segments[2]); },
      [](auto &F) { F.word(0x3020, UINT64_MAX); },
      [](auto &F) { F.word(0x3028, 0x6001); },
      [](auto &F) { F.pointer(0x6028, 0x3018); }, // Cycle through the root.
      [](auto &F) { F.word(0x6028, 0x7000); },
      [](auto &F) { F.word(0x5008, 3); },
      [](auto &F) {
        F.pointer(0x6030, 0x2020);
        F.pointer(0x6038, 0x2000);
      },
      [](auto &F) { F.pointer(0x6030, 0x4000); },
      [](auto &F) {
        F.Image.Segments[1].Data[0x107] = 0xd8;
      }, // Lone surrogate.
      [](auto &F) { F.Image.Segments[1].Data[8] = 'f'; },
      [](auto &F) { F.Image.Segments[1].Data[9] = 'q'; },
      [](auto &F) {
        F.Image.Segments[1].Data[8] = 'I';
      }, // Invalid zero extension.
      [](auto &F) { F.Image.MachOChainedFixupsAmbiguous = true; },
      [](auto &F) { F.Image.IsRelocatable = true; },
      [](auto &F) { F.Image.DataPtrRelocTargetOwners.erase(0x2010); },
      [](auto &F) { F.Image.CodePtrRelocSlots.insert(0x1fff); },
      [](auto &F) {
        F.Image.Segments[0].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
      },
  };
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (size_t I = 0; I < Mutations.size(); ++I) {
      SCOPED_TRACE(I);
      ConstantObjectFixture F(Architecture, true);
      Mutations[I](F);
      EXPECT_FALSE(readObjCConstantObjectGraph(F.Image, 0x3018));
      EXPECT_FALSE(
          bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
    }
}

TEST(ObjCSourceBindings,
     ConstantObjectGraphsBoundDepthAndPreserveSharedFanout) {
  // Visit a shared suffix first through a short edge, then through a longer
  // prefix. Reusing its proof must not hide the complete longest path.
  for (unsigned Count : {63U, 64U}) {
    ConstantObjectFixture F;
    F.storage(0x7000, Count * 24, "__objc_arrayobj");
    F.storage(0x9000, Count * 8, "__const");
    F.word(0x3008, 2);
    F.pointer(0x6000, 0x7000);
    F.pointer(0x6008, 0x7000 + 32 * 24);
    for (unsigned I = 0; I < Count; ++I) {
      const va_t Address = 0x7000 + I * 24;
      ASSERT_TRUE(F.Image.recordDyldBindSlot(
          Address, "_OBJC_CLASS_$_NSConstantArray", 0,
          "/System/Library/Frameworks/Foundation.framework/Foundation", false));
      F.word(Address + 8, 1);
      F.pointer(Address + 16, 0x9000 + I * 8);
      F.pointer(0x9000 + I * 8, I == 31          ? 0x2000
                                : I + 1 == Count ? 0x7000
                                                 : Address + 24);
    }
    EXPECT_EQ(bool(readObjCConstantObjectGraph(F.Image, 0x3000)), Count == 63);
    F.pointer(0x6000, 0x7000 + 32 * 24);
    F.pointer(0x6008, 0x7000);
    EXPECT_EQ(bool(readObjCConstantObjectGraph(F.Image, 0x3000)), Count == 63);
  }
  for (unsigned Depth : {64U, 65U}) {
    ConstantObjectFixture F;
    F.storage(0x7000, Depth * 24, "__objc_arrayobj");
    F.storage(0x9000, Depth * 8, "__const");
    for (unsigned I = 0; I < Depth; ++I) {
      const va_t Address = 0x7000 + I * 24;
      ASSERT_TRUE(F.Image.recordDyldBindSlot(
          Address, "_OBJC_CLASS_$_NSConstantArray", 0,
          "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
          false));
      F.word(Address + 8, 1);
      F.pointer(Address + 16, 0x9000 + I * 8);
      F.pointer(0x9000 + I * 8, I + 1 == Depth ? 0x2000 : Address + 24);
    }
    const auto Graph = readObjCConstantObjectGraph(F.Image, 0x7000);
    EXPECT_EQ(bool(Graph), Depth == 64);
    if (Graph)
      EXPECT_EQ(Graph->size(), 65U);
  }
  for (unsigned Count : {16384U, 16385U}) {
    ConstantObjectFixture F;
    F.storage(0xa000, Count * 8, "__const");
    F.word(0x3008, Count);
    F.pointer(0x3010, 0xa000);
    for (unsigned I = 0; I < Count; ++I)
      F.pointer(0xa000 + I * 8, 0x2000);
    const auto Graph = readObjCConstantObjectGraph(F.Image, 0x3000);
    EXPECT_EQ(bool(Graph), Count == 16384);
    if (Graph) {
      EXPECT_EQ(Graph->size(), 2U);
      EXPECT_EQ(Graph->at(0x3000).Elements.size(), Count);
    }
  }
}

TEST(ObjCSourceBindings, ConstantIntegerObjectsRetainSignednessAndExactBits) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (char Encoding : std::string("cCsSiIlLqQ")) {
      const unsigned Width = Encoding == 'c' || Encoding == 'C'   ? 8
                             : Encoding == 's' || Encoding == 'S' ? 16
                             : Encoding == 'i' || Encoding == 'I' ? 32
                                                                  : 64;
      const bool Signed = Encoding >= 'a' && Encoding <= 'z';
      const uint64_t Mask =
          Width == 64 ? UINT64_MAX : (UINT64_C(1) << Width) - 1;
      for (uint64_t Bits :
           {UINT64_C(0), UINT64_C(1), UINT64_C(1) << (Width - 1), Mask}) {
        ConstantObjectFixture F(Architecture);
        F.Image.Segments[1].Data[8] = Encoding;
        const uint64_t Extended =
            Signed && (Bits & (UINT64_C(1) << (Width - 1))) ? Bits | ~Mask
                                                            : Bits;
        F.word(0x4010, Extended);
        const auto Graph = readObjCConstantObjectGraph(F.Image, 0x4000);
        ASSERT_TRUE(Graph);
        EXPECT_EQ(Graph->at(0x4000).Bits, Extended);
        EXPECT_EQ(Graph->at(0x4000).Encoding, Encoding);
        if (Width < 64) {
          F.word(0x4010, Extended ^ (UINT64_C(1) << Width));
          EXPECT_FALSE(readObjCConstantObjectGraph(F.Image, 0x4000));
        }
      }
    }
}

TEST(ObjCSourceBindings,
     ConstantObjectBindingsRevalidateSlotsAndRejectExtraEffects) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    ConstantObjectFixture F(Architecture, true);
    F.Function.Body[0].RetVal = HighExpr::makeLoad(
        HighExpr::makeConst(0x6050, 8), NdType::makePtr(NdType::makeVoid()));
    auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    const auto Call = Bound.Function.Body[0].RetVal;
    ASSERT_TRUE(Call->SourceCallHint);
    EXPECT_EQ(Call->SourceCallHint->ImmutablePointerSlot, 0x6050U);
    EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
    for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto Changed = *Call;
      auto Hint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
      Changed.SourceCallHint = Hint;
      auto &H = *Hint;
      switch (Mutation) {
      case 0:
        H.ByteCount = 8;
        break;
      case 1:
        H.BorrowedByteInputs = {{0, 1}};
        break;
      case 2:
        H.DoesNotReturn = true;
        break;
      case 3:
        H.ReturnedArgument = 0;
        break;
      case 4:
        H.ImmutablePointerSlot = 0x6058;
        break;
      case 5:
        H.TargetAddress = 0x2000;
        break;
      case 6:
        H.TargetName = "forged";
        break;
      case 7:
        H.SelectorReferenceAddress = 0x1000;
        break;
      case 8:
        Changed.IsIndirectCall = true;
        break;
      case 9:
        Changed.MemoryOrdering = NdMemoryOrdering::Acquire;
        break;
      case 10:
        Changed.Operands.push_back(HighExpr::makeConst(0, 8));
        break;
      case 11:
        H.Signature.ReturnType = NdType::makeInt(8);
        break;
      }
      EXPECT_FALSE(objcSourceCallBound(Changed, F.Image, {}));
    }
    F.pointer(0x6050, 0x3000);
    EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
    F.Function.Body[0].RetVal =
        HighExpr::makeLoad(HighExpr::makeConst(0x3018, 8), NdType::makeInt(8));
    EXPECT_FALSE(
        bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
    F.Function.ReturnType = NdType::makeInt(8);
    auto Scalar = HighExpr::makeConst(0x3018, 8);
    Scalar->ConstProvenance = ConstantAddressProvenance::Scalar;
    F.Function.Body[0].RetVal = Scalar;
    Bound = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_TRUE(Bound.ConstantObjects.empty());
    EXPECT_EQ(Bound.Function.Body[0].RetVal->Kind, ExprKind::Const);
  }
}

TEST(ObjCSourceBindings, ConstantStringsKeepBytesUnicodeAndObjectIdentity) {
  ConstantStringFixture F;
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    F.Image.Arch = Architecture;
    for (bool Chained : {false, true}) {
      F.Image.MachOHasChainedFixups = Chained;
      F.Image.MachOResolvedChainedPointerSlots = {0x2000, 0x2010, 0x2020,
                                                  0x2030};
      auto A = readObjCConstantString(F.Image, 0x2000);
      auto U = readObjCConstantString(F.Image, 0x2020);
      ASSERT_TRUE(A && U);
      EXPECT_FALSE(A->UTF16);
      EXPECT_TRUE(U->UTF16);
      EXPECT_EQ(A->ContentsAddress, 0x1000U);
      EXPECT_EQ(U->ContentsAddress, 0x1100U);
      EXPECT_EQ(A->Units, (std::vector<uint16_t>{'a', '\n', '"'}));
      EXPECT_EQ(U->Units, (std::vector<uint16_t>{0x767e, 0, 0xd83d, 0xde00}));
      auto Result = bindObjCSourceReferences(F.Function, F.Image);
      EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
      EXPECT_EQ(Result.ConstantStrings, std::set<va_t>{0x2000});
      const auto Call = Result.Function.Body[0].RetVal;
      ASSERT_TRUE(Call->SourceCallHint);
      EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
      std::set<std::string> Shared;
      const auto Helpers =
          renderObjCConstantStringHelpers(F.Image, {0x2000, 0x2020}, Shared);
      EXPECT_EQ(Shared.size(), 3U);
      EXPECT_NE(Helpers.find("a\\012\\042\\000"), std::string::npos);
      EXPECT_NE(Helpers.find("30334, 0, 55357, 56832, 0"), std::string::npos);
      EXPECT_EQ(F.Function.Body[0].RetVal->Kind, ExprKind::Const);
    }
  }
}

TEST(ObjCSourceBindings, ConstantStringsRejectUnprovedRecordsAndContents) {
  using Mutation = std::function<void(BinaryImage &)>;
  const std::vector<Mutation> Mutations = {
      [](auto &I) { I.IsRelocatable = true; },
      [](auto &I) { I.MachOChainedFixupsAmbiguous = true; },
      [](auto &I) {
        I.Raw.resize(32);
        llvm::support::endian::write32le(I.Raw.data(),
                                         llvm::MachO::MH_MAGIC_64);
        llvm::support::endian::write32le(I.Raw.data() + 8,
                                         llvm::MachO::CPU_SUBTYPE_ARM64E);
      },
      [](auto &I) { I.DyldBindSlots.clear(); },
      [](auto &I) { I.DyldBindSlots.at(0x2000).Name = "_otherClass"; },
      [](auto &I) { I.DyldBindSlots.at(0x2000).Addend = 1; },
      [](auto &I) { I.DyldBindSlots.at(0x2000).WeakImport = true; },
      [](auto &I) { I.ConflictingImportStorageSlots.insert(0x2010); },
      [](auto &I) { I.ImportStorageSlots.at(0x2000).Addend = 8; },
      [](auto &I) { I.ImportPtrSlots[0x2010] = "_otherData"; },
      [](auto &I) { I.MachOHasChainedFixups = true; },
      [](auto &I) { I.DataPtrRelocSlots.insert(0x2008); },
      [](auto &I) { I.CodePtrRelocSlots.insert(0x2010); },
      [](auto &I) { I.DataPtrRelocSlots.insert(0xfff); },
      [](auto &I) { I.DataPtrRelocSlots.insert(0x1002); },
      [](auto &I) { I.Sections[0].Size = 63; },
      [](auto &I) { I.Sections[0].FileSz = 31; },
      [](auto &I) { I.Sections[0].FileOff = 1; },
      [](auto &I) { I.Sections[1].FileSz = 2; },
      [](auto &I) {
        I.Sections[1].Type |= llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
      },
      [](auto &I) {
        I.Segments[1].Flags = I.Segments[1].Flags | SegmentFlags::Writable;
      },
      [](auto &I) {
        I.Sections[1].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      },
      [](auto &I) { I.Sections.push_back(I.Sections[1]); },
      [](auto &I) { I.Segments.push_back(I.Segments[1]); },
      [](auto &I) { I.Segments[0].Data[8] = 0xc9; },
      [](auto &I) { I.Segments[0].Data[12] = 1; },
      [](auto &I) { I.Segments[0].Data[31] = 0xff; },
      [](auto &I) { I.Segments[1].Data[3] = 1; },
      [](auto &I) { I.Segments[1].Data[0] = 0x80; },
  };
  for (size_t Index = 0; Index < Mutations.size(); ++Index) {
    SCOPED_TRACE(Index);
    ConstantStringFixture F;
    Mutations[Index](F.Image);
    EXPECT_FALSE(readObjCConstantString(F.Image, 0x2000));
    EXPECT_FALSE(
        bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
  }
  ConstantStringFixture F;
  EXPECT_FALSE(readObjCConstantString(F.Image, 0x2008));
  EXPECT_FALSE(readObjCConstantString(F.Image, 0x2040));
}

TEST(ObjCSourceBindings, ConstantStringObjectsCannotAuthorizeRawMemoryAccess) {
  ConstantStringFixture F;
  auto Address = F.Function.Body[0].RetVal;
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = Address;
  Store.StoreVal = HighExpr::makeConst(0, 8);
  F.Function.Body.insert(F.Function.Body.begin(), Store);
  for (bool Reversed : {false, true}) {
    if (Reversed)
      std::reverse(F.Function.Body.begin(), F.Function.Body.end());
    EXPECT_FALSE(
        bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
  }
  F.Function.Body.resize(1);
  F.Function.Body[0].Kind = StmtKind::Return;
  F.Function.Body[0].RetVal = HighExpr::makeLoad(Address, NdType::makeInt(8));
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
}

TEST(ObjCSourceBindings, ConstantStringPointersRetainStoredAddressProvenance) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (auto Provenance : {ConstantAddressProvenance::Address,
                            ConstantAddressProvenance::DataAddress}) {
      for (bool ExpressionStore : {false, true}) {
        ConstantStringFixture F;
        F.Image.Arch = Architecture;
        auto Value = HighExpr::makeConst(0x2000, 8, Provenance);
        auto Destination =
            HighExpr::makeVar(MedVar{.Kind = MedVar::Param, .Size = 8});
        HighStmt Store;
        if (ExpressionStore) {
          Store.Kind = StmtKind::ExprStmt;
          Store.Val = std::make_shared<HighExpr>();
          Store.Val->Kind = ExprKind::Store;
          Store.Val->Operands = {Destination, Value};
        } else {
          Store.Kind = StmtKind::Store;
          Store.StoreAddr = Destination;
          Store.StoreVal = Value;
        }
        F.Function.Body.insert(F.Function.Body.begin(), Store);
        auto Result = bindObjCSourceReferences(F.Function, F.Image);
        ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
        EXPECT_EQ(Result.ConstantStrings, std::set<va_t>{0x2000});
        const auto Bound = ExpressionStore
                               ? Result.Function.Body[0].Val->Operands[1]
                               : Result.Function.Body[0].StoreVal;
        ASSERT_TRUE(Bound && Bound->SourceCallHint);
        EXPECT_TRUE(objcSourceCallBound(*Bound, F.Image, {}));
        EXPECT_EQ(Value->Kind, ExprKind::Const);
        EXPECT_EQ(Value->ConstProvenance, Provenance);
        // A shared node used to access the object's private bytes is still
        // unbound, independently of which occurrence is visited first.
        HighStmt Read;
        Read.Kind = StmtKind::Return;
        Read.RetVal = HighExpr::makeLoad(Value, NdType::makeInt(8));
        F.Function.Body.push_back(Read);
        for (bool Reversed : {false, true}) {
          if (Reversed)
            std::reverse(F.Function.Body.begin(), F.Function.Body.end());
          EXPECT_FALSE(
              bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
        }
      }
    }
  }
}

TEST(ObjCSourceBindings, StoredStringsRejectIncompleteAndNumericProvenance) {
  for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ConstantStringFixture F;
    auto Value =
        HighExpr::makeConst(0x2000, 8, ConstantAddressProvenance::DataAddress);
    HighStmt Store;
    Store.Kind = StmtKind::Store;
    Store.StoreAddr =
        HighExpr::makeVar(MedVar{.Kind = MedVar::Param, .Size = 8});
    Store.StoreVal = Value;
    switch (Mutation) {
    case 0:
      Value->ConstProvenance = ConstantAddressProvenance::Unknown;
      break;
    case 1:
      Value->ConstProvenance = ConstantAddressProvenance::Scalar;
      break;
    case 2:
      Value->ConstProvenance = ConstantAddressProvenance::AddressFragment;
      break;
    case 3:
      Value->ConstProvenance = ConstantAddressProvenance::CodeAddress;
      break;
    case 4:
      Value->Type = NdType::makeInt(4);
      break;
    case 5:
      Value->Type = NdType::makeFloat(8);
      break;
    case 6:
      Value->AddressOwnerVA = 0x2020;
      break;
    case 7:
      Store.StoreVal =
          HighExpr::makeBinop(NdOp::INT_ADD, Value, HighExpr::makeConst(1, 8));
      break;
    }
    F.Function.Body = {Store};
    auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_FALSE(Result.Limitation.empty());
    EXPECT_TRUE(Result.ConstantStrings.empty());
  }
}

TEST(ObjCSourceBindings, ReplacesLoadedSelectorAndKeepsOriginalProjection) {
  Fixture F;
  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  auto Call = Result.Function.Body[0].RetVal;
  ASSERT_EQ(Call->Kind, ExprKind::Call);
  ASSERT_TRUE(Call->SourceCallHint);
  EXPECT_EQ(Call->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeSelector);
  EXPECT_EQ(Call->SourceCallHint->TargetName, "step:");
  EXPECT_TRUE(Call->Operands.empty());
  EXPECT_EQ(F.Function.Body[0].RetVal->Kind, ExprKind::Load);
  EXPECT_EQ(F.Function.Body[0].RetVal->Operands[0]->ConstVal, 0x1010u);
  EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
}

TEST(ObjCSourceBindings, SlotAddressIsNotTheRuntimeValue) {
  Fixture F;
  F.Function.Body[0].RetVal = HighExpr::makeConst(0x1010, 8);
  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_FALSE(Result.Limitation.empty());
  EXPECT_EQ(Result.Function.Body[0].RetVal->Kind, ExprKind::Const);
  ASSERT_EQ(Result.Diagnostics.Items.size(), 1U);
  EXPECT_TRUE(Result.Diagnostics.Complete);
  EXPECT_EQ(Result.Diagnostics.Items[0].Issue,
            SourceProjectionIssue::DataBinding);
  EXPECT_EQ(Result.Diagnostics.Items[0].RelatedAddress, 0x1010U);
  EXPECT_EQ(Result.Diagnostics.Items[0].Expression,
            Result.Function.Body[0].RetVal.get());
  F.Function.Body[0].RetVal =
      HighExpr::makeLoad(HighExpr::makeConst(0x1014, 8), NdType::makeInt(4));
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
}

TEST(ObjCSourceBindings, NumericMasksKeepOccurrenceProvenanceThroughHighIR) {
  using P = ConstantAddressProvenance;
  MedFunc Med;
  Med.Entry = 0x2000;
  Med.Name = "constant_origin";
  SourceFunctionTypeHint Hint;
  Hint.ReturnType = NdType::makeInt(8, false);
  Hint.Parameters = {{"objc_self", NdType::makePtr(NdType::makeVoid())},
                     {"objc_cmd", NdType::makePtr(NdType::makeVoid())}};
  Med.SourceTypeHint = Hint;
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Med.Entry;
  MedOp Set;
  Set.Opcode = NdOp::COPY;
  Set.Output.Kind = MedVar::Reg;
  Set.Output.TheArch = Arch::AArch64;
  Set.Output.Id = 1;
  Set.Output.SSAVer = 1;
  Set.Output.Size = 8;
  Set.Output.RegOff = getTargetRegInfo(Arch::AArch64).IntReturnReg;
  Set.addInput(MedVar::makeConst(0x1040, 8, P::DataAddress, 0x1000));
  Block.Ops.push_back(Set);
  MedOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Block.Ops.push_back(Ret);
  Med.Blocks.push_back(Block);
  for (P Provenance : {P::Scalar, P::Unknown, P::AddressFragment, P::Address,
                       P::DataAddress, P::CodeAddress}) {
    const va_t Owner = Provenance == P::Scalar ? InvalidVA : 0x1000;
    Med.Blocks[0].Ops[0].Inputs[0] =
        MedVar::makeConst(0x1040, 8, Provenance, Owner);
    const auto High = MedToHighConverter().convert(Med, Arch::AArch64);
    ASSERT_FALSE(High.Body.empty());
    const auto Value = High.Body.back().RetVal;
    ASSERT_TRUE(Value);
    ASSERT_EQ(Value->Kind, ExprKind::Const);
    EXPECT_EQ(Value->ConstProvenance, Provenance);
    EXPECT_EQ(Value->AddressOwnerVA, Owner);

    Fixture F;
    F.Function.ReturnType = NdType::makeInt(8, false);
    MedVar Input;
    Input.Kind = MedVar::Param;
    Input.Size = 8;
    F.Function.Body[0].RetVal =
        HighExpr::makeBinop(NdOp::INT_OR, HighExpr::makeVar(Input), Value);
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_EQ(Bound.Limitation.empty(), Provenance == P::Scalar);
    EXPECT_EQ(Bound.Function.Body[0].RetVal->Operands[1]->ConstVal, 0x1040U);
  }
}

TEST(ObjCSourceBindings, ScalarMaskIdentityCannotAuthorizeAnAddressConsumer) {
  using P = ConstantAddressProvenance;
  Fixture F;
  F.Function.ReturnType = NdType::makeInt(8, false);
  const auto Scalar = HighExpr::makeConst(0x1040, 8, P::Scalar);
  const auto Unknown = HighExpr::makeConst(0x1040, 8);
  const auto Address = HighExpr::makeConst(0x1040, 8, P::DataAddress, 0x1000);
  const auto OtherOwner =
      HighExpr::makeConst(0x1040, 8, P::DataAddress, 0x1020);
  EXPECT_FALSE(Scalar->structuralEq(*Unknown));
  EXPECT_FALSE(Scalar->structuralEq(*Address));
  EXPECT_FALSE(Address->structuralEq(*OtherOwner));
  MedVar Input;
  Input.Kind = MedVar::Param;
  Input.Size = 8;
  const auto Mask =
      HighExpr::makeBinop(NdOp::INT_OR, HighExpr::makeVar(Input), Scalar);
  F.Function.Body[0].RetVal = Mask;
  ASSERT_TRUE(bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
  // Reuse both nodes in a different context, including a pointer expression
  // whose root is not a constant. Neither the DAG cache nor a scalar leaf can
  // turn a memory address into a numeric-only use.
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = Mask;
  Store.StoreVal = HighExpr::makeConst(7, 8);
  for (bool AddressFirst : {false, true}) {
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = Mask;
    F.Function.Body = AddressFirst ? std::vector<HighStmt>{Store, Return}
                                   : std::vector<HighStmt>{Return, Store};
    EXPECT_FALSE(
        bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
  }
  F.Function.Body.resize(1);
  F.Function.Body[0].Kind = StmtKind::Return;
  F.Function.Body[0].RetVal = HighExpr::makeLoad(Mask, NdType::makeInt(8));
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
  F.Function.Body[0].RetVal = Mask;
  F.Function.ReturnType = NdType::makePtr(NdType::makeVoid());
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
  F.Function.ReturnType = NdType::makeInt(8, false);
  Mask->Operands[1] = HighExpr::makeConst(0x1040, 8, P::Scalar, 0x1000);
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
}

TEST(ObjCSourceBindings, OrderedOrWrongWidthLoadsDoNotBecomeQueries) {
  Fixture F;
  F.Function.Body[0].RetVal->Type = NdType::makeInt(4);
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
  F.Function.Body[0].RetVal->Type = NdType::makeInt(8);
  F.Function.Body[0].RetVal->MemoryOrdering = NdMemoryOrdering::Acquire;
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
}

TEST(ObjCSourceBindings, IvarCarrierPreservesReadWidthAndDeclaringClass) {
  Fixture F;
  F.Image.ObjCSourceReferences[0x1010] = {ObjCSourceReference::Kind::IvarOffset,
                                          0x1010, 8, "_wide", "Base"};
  for (uint16_t Width : {4, 8}) {
    F.Function.Body[0].RetVal->Type = NdType::makeInt(Width);
    auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    ASSERT_TRUE(Result.Function.Body[0].RetVal->SourceCallHint);
    EXPECT_EQ(Result.Function.Body[0]
                  .RetVal->SourceCallHint->Signature.ReturnType->Size,
              Width);
    EXPECT_EQ(Result.InstanceLayoutClasses, std::set<std::string>{"Base"});
    EXPECT_TRUE(
        objcSourceCallBound(*Result.Function.Body[0].RetVal, F.Image, {}));
  }
  F.Image.ObjCSourceReferences[0x1010].Size = 4;
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
}

TEST(ObjCSourceBindings, ForgedRuntimeIdentityCannotReuseAnotherSlot) {
  Fixture F;
  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  auto Expression = Result.Function.Body[0].RetVal;
  auto Hint = std::make_shared<SourceCallTypeHint>(*Expression->SourceCallHint);
  Hint->TargetName = "different:";
  Expression->SourceCallHint = Hint;
  EXPECT_FALSE(objcSourceCallBound(*Expression, F.Image, {}));
}

TEST(ObjCSourceBindings, RuntimeCallsRevalidateImportedSignatureAndRegister) {
  Fixture F;
  F.Image.ImportPtrSlots[0x1020] = "_objc_retain_x19";
  const auto Original = objcRuntimeSourceCallHint(F.Image, 0x1020);
  ASSERT_TRUE(Original);
  auto Call = HighExpr::makeCall("objc_retain", 0x1020,
                                 {HighExpr::makeConst(0x5678, 8)});
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Original);
  EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
  HighFunc Function;
  HighStmt Statement;
  Statement.Kind = StmtKind::Return;
  Statement.RetVal = Call;
  Function.Body.push_back(Statement);
  EXPECT_TRUE(bindObjCSourceReferences(Function, F.Image).Dependencies.empty());
  auto Changed = *Original;
  Changed.Signature.Parameters[0].Location.RegisterOffset = 0;
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Changed);
  EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Original);
  F.Image.ImportPtrSlots[0x1020] = "_objc_release_x19";
  EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
}

TEST(ObjCSourceBindings, SwiftMetadataCallsRevalidateDeclarationAndConvention) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    Fixture F;
    F.Image.Arch = Architecture;
    constexpr va_t Slot = 0x10e0;
    const std::string Symbol = "_$s10Foundation3URLVMa";
    F.Image.ImportPtrSlots[Slot] = Symbol;
    ASSERT_TRUE(F.Image.recordDyldBindSlot(
        Slot, Symbol, 0,
        "/System/Library/Frameworks/Foundation.framework/Foundation", false));
    const auto Hint = swiftRuntimeSourceCallHint(F.Image, Slot);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->Signature.Origin,
              SourceFunctionTypeHint::OriginKind::SwiftSDK);
    EXPECT_EQ(Hint->Signature.Convention,
              SourceFunctionTypeHint::ConventionKind::Swift);
    ASSERT_EQ(Hint->Signature.ReturnComponents.size(), 2U);
    auto Call = HighExpr::makeCall(Symbol, Slot, {HighExpr::makeConst(0, 8)});
    Call->Type = Hint->Signature.ReturnType;
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    ASSERT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
    for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
      auto Bad = std::make_shared<SourceCallTypeHint>(*Hint);
      if (Mutation == 0)
        Bad->Signature.Convention = SourceFunctionTypeHint::ConventionKind::C;
      else if (Mutation == 1)
        Bad->Signature.ReturnComponents.pop_back();
      else if (Mutation == 2)
        Bad->Signature.Origin =
            SourceFunctionTypeHint::OriginKind::SwiftRuntime;
      else
        Bad->Signature.Parameters[0].Location.RegisterOffset += 8;
      Call->SourceCallHint = Bad;
      EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {})) << Mutation;
    }
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    const auto Bind = F.Image.DyldBindSlots.at(Slot);
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      F.Image.DyldBindSlots[Slot] = Bind;
      F.Image.ImportPtrSlots[Slot] = Symbol;
      if (Mutation == 0)
        F.Image.DyldBindSlots[Slot].Module =
            "/tmp/Foundation.framework/Foundation";
      else if (Mutation == 1)
        F.Image.DyldBindSlots[Slot].Addend = 1;
      else if (Mutation == 2)
        F.Image.DyldBindSlots[Slot].WeakImport = true;
      else if (Mutation == 3)
        F.Image.DyldBindSlots.erase(Slot);
      else {
        const std::string Unknown =
            Mutation == 4 ? Symbol + ".fake" : "_$s4Test3BoxVMa";
        F.Image.ImportPtrSlots[Slot] = Unknown;
        F.Image.DyldBindSlots[Slot].Name = Unknown;
      }
      EXPECT_FALSE(swiftRuntimeSourceCallHint(F.Image, Slot)) << Mutation;
      EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {})) << Mutation;
    }
  }
}

TEST(ObjCSourceBindings, FixedCRecordsRevalidateExportsTypesAndCarriers) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool Floating : {false, true}) {
      Fixture F;
      F.Image.Arch = Architecture;
      constexpr va_t Slot = 0x10e0;
      const std::string Name =
          Floating ? "_CGRectStandardize" : "_NSUnionRange";
      const std::string Module =
          Floating
              ? "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics"
              : "/System/Library/Frameworks/Foundation.framework/Foundation";
      F.Image.ImportPtrSlots[Slot] = Name;
      ASSERT_TRUE(F.Image.recordDyldBindSlot(Slot, Name, 0, Module, false));
      const auto Hint = darwinRuntimeSourceCallHint(F.Image, Slot);
      if (Floating && Architecture == Arch::X64) {
        EXPECT_FALSE(Hint);
        continue;
      }
      ASSERT_TRUE(Hint);
      EXPECT_EQ(Hint->Signature.Origin,
                SourceFunctionTypeHint::OriginKind::DarwinSDK);
      ASSERT_EQ(Hint->Signature.Parameters.size(), Floating ? 1U : 2U);
      ASSERT_EQ(Hint->Signature.ReturnComponents.size(), Floating ? 4U : 2U);
      std::vector<ExprPtr> Arguments;
      for (const auto &Parameter : Hint->Signature.Parameters) {
        std::vector<ExprPtr> Leaves;
        for (const auto &Member : sourceAggregateMembers(Parameter.Type))
          Leaves.push_back(
              HighExpr::makeBitCast(HighExpr::makeConst(0, 8), Member.Type));
        Arguments.push_back(HighExpr::makeRecord(Parameter.Type, Leaves));
      }
      auto Call = HighExpr::makeCall(Name, Slot, Arguments);
      Call->Type = Hint->Signature.ReturnType;
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      ASSERT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
      for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
        auto Bad = std::make_shared<SourceCallTypeHint>(*Hint);
        if (Mutation == 0)
          Bad->Signature.ReturnComponents.pop_back();
        else if (Mutation == 1)
          Bad->Signature.Parameters[0].Components[0].RegisterOffset += 8;
        else if (Mutation == 2) {
          auto Record = std::make_shared<NdType>(*Bad->Signature.ReturnType);
          Record->FieldOffsets[1] = 0;
          Bad->Signature.ReturnType = Record;
        } else
          Bad->Signature.Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
        Call->SourceCallHint = Bad;
        EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {})) << Mutation;
      }
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      const auto Bind = F.Image.DyldBindSlots.at(Slot);
      for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
        F.Image.DyldBindSlots[Slot] = Bind;
        F.Image.ImportPtrSlots[Slot] = Name;
        if (Mutation == 0)
          F.Image.DyldBindSlots[Slot].Module = "/tmp/Other.framework/Other";
        else if (Mutation == 1)
          F.Image.DyldBindSlots[Slot].Addend = 8;
        else if (Mutation == 2)
          F.Image.DyldBindSlots[Slot].WeakImport = true;
        else if (Mutation == 3)
          F.Image.DyldBindSlots.erase(Slot);
        else
          F.Image.ImportPtrSlots.erase(Slot);
        EXPECT_FALSE(darwinRuntimeSourceCallHint(F.Image, Slot)) << Mutation;
        EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {})) << Mutation;
      }
      F.Image.DyldBindSlots[Slot] = Bind;
      F.Image.ImportPtrSlots[Slot] = Name;
      EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
    }
  }
}

TEST(ObjCSourceBindings, StaticAssociationKeysKeepExactContextAndIdentity) {
  Fixture F;
  F.Image.Sections[0].Type = llvm::MachO::S_CSTRING_LITERALS;
  F.Image.ImportPtrSlots[0x1020] = "_objc_getAssociatedObject";
  const auto Hint = objcRuntimeSourceCallHint(F.Image, 0x1020);
  ASSERT_TRUE(Hint);
  auto Key = HighExpr::makeConst(0x1031, 8);
  auto Call = HighExpr::makeCall("objc_getAssociatedObject", 0x1020,
                                 {HighExpr::makeConst(0, 8), Key});
  Call->Type = NdType::makePtr(NdType::makeVoid());
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
  F.Function.Body[0].RetVal = Call;
  for (va_t Address : {0x1031, 0x1032, 0x1031}) {
    Key->ConstVal = Address;
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    EXPECT_EQ(Result.AssociationKeys, std::set<va_t>{Address});
    const auto Bound = Result.Function.Body[0].RetVal->Operands[1];
    ASSERT_TRUE(Bound->SourceCallHint);
    EXPECT_EQ(Bound->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeAssociationKey);
    EXPECT_EQ(Bound->SourceCallHint->TargetAddress, Address);
    EXPECT_TRUE(objcSourceCallBound(*Bound, F.Image, {}));
    EXPECT_EQ(Key->Kind, ExprKind::Const);
  }
  // Reusing the same expression as an ordinary address must not reuse the
  // contextual key substitution through the projection's clone cache.
  F.Function.Body[0].RetVal = HighExpr::makeBinop(NdOp::INT_ADD, Call, Key);
  const auto Mixed = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_FALSE(Mixed.Limitation.empty());
  EXPECT_EQ(Mixed.Function.Body[0].RetVal->Operands[1]->Kind, ExprKind::Const);

  F.Image.Arch = Arch::X64;
  const auto X64Hint = objcRuntimeSourceCallHint(F.Image, 0x1020);
  ASSERT_TRUE(X64Hint);
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*X64Hint);
  F.Function.Body[0].RetVal = Call;
  const auto X64 = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_TRUE(X64.Limitation.empty()) << X64.Limitation;
  const auto X64Key = X64.Function.Body[0].RetVal->Operands[1];
  ASSERT_TRUE(X64Key->SourceCallHint);
  EXPECT_EQ(X64Key->SourceCallHint->Signature.Architecture, Arch::X64);
  EXPECT_TRUE(objcSourceCallBound(*X64Key, F.Image, {}));
}

TEST(ObjCSourceBindings,
     StaticAssociationKeysRejectUnprovedConsumersAndStorage) {
  Fixture F;
  F.Image.Sections[0].Type = llvm::MachO::S_CSTRING_LITERALS;
  F.Image.ImportPtrSlots[0x1020] = "_objc_getAssociatedObject";
  const auto Hint = objcRuntimeSourceCallHint(F.Image, 0x1020);
  ASSERT_TRUE(Hint);
  auto Call = HighExpr::makeCall(
      "objc_getAssociatedObject", 0x1020,
      {HighExpr::makeConst(0, 8), HighExpr::makeConst(0x1031, 8)});
  Call->Type = NdType::makePtr(NdType::makeVoid());
  F.Function.Body[0].RetVal = Call;
  for (unsigned Case = 0; Case < 5; ++Case) {
    SCOPED_TRACE(Case);
    F.Image.Sections[0].Type = llvm::MachO::S_CSTRING_LITERALS;
    F.Image.Segments[0].Flags = SegmentFlags::Readable;
    auto Changed = *Hint;
    if (Case == 0)
      Changed.TargetName = "objc_setAssociatedObject";
    else if (Case == 1)
      Changed.Signature.Parameters[1].Location.RegisterOffset += 8;
    else if (Case == 2)
      F.Image.Sections[0].Type = 0;
    else if (Case == 3)
      F.Image.Segments[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
    else
      Call->Operands[1] = HighExpr::makeConst(0x1031, 4);
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Changed);
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_FALSE(Result.Limitation.empty());
    EXPECT_TRUE(Result.AssociationKeys.empty());
  }
}

TEST(ObjCSourceBindings, SelfPointerGlobalsBecomeSharedStaticIdentities) {
  Fixture F;
  constexpr va_t Address = 0x1040;
  F.Image.ObjCSourceReferences.clear();
  F.Image.Segments[0].Flags =
      SegmentFlags::Readable | SegmentFlags::Writable;
  F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
  F.Image.Symbols.push_back({"_ObserverContext", Address, 8, false});
  F.Image.MachOHasChainedFixups = true;
  F.Image.MachOResolvedChainedPointerSlots.insert(Address);
  llvm::support::endian::write64le(
      F.Image.Segments[0].Data.data() + Address - 0x1000, Address);
  F.Function.Body[0].RetVal = HighExpr::makeLoad(
      HighExpr::makeConst(Address, 8,
                         ConstantAddressProvenance::DataAddress),
      NdType::makeInt(8, false));

  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  EXPECT_EQ(Result.StaticIdentities, std::set<va_t>{Address});
  const auto Bound = Result.Function.Body[0].RetVal;
  ASSERT_TRUE(Bound->SourceCallHint);
  EXPECT_EQ(Bound->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeStaticIdentity);
  EXPECT_EQ(Bound->SourceCallHint->TargetName, "_ObserverContext");
  EXPECT_TRUE(objcSourceCallBound(*Bound, F.Image, {}));
  std::set<std::string> Helpers;
  const auto Source =
      renderObjCStaticIdentityHelpers(Result.StaticIdentities, Helpers);
  EXPECT_EQ(Helpers,
            std::set<std::string>{"neverd_static_identity_1040_address"});
  EXPECT_NE(Source.find("static unsigned char identity"), std::string::npos);

  F.Image.MachOResolvedChainedPointerSlots.clear();
  Result = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_FALSE(Result.Limitation.empty());
  EXPECT_TRUE(Result.StaticIdentities.empty());
}

TEST(ObjCSourceBindings, NamedWritableScalarsUseSharedRebuiltStorage) {
  Fixture F;
  constexpr va_t Address = 0x1040;
  F.Image.ObjCSourceReferences.clear();
  F.Image.Segments[0].Flags =
      SegmentFlags::Readable | SegmentFlags::Writable;
  F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
  F.Image.Symbols.push_back({"_captureLevel", Address, 8, false});
  llvm::support::endian::write64le(
      F.Image.Segments[0].Data.data() + Address - 0x1000, 31);
  F.Function.Body[0].RetVal = HighExpr::makeLoad(
      HighExpr::makeConst(Address, 8,
                         ConstantAddressProvenance::DataAddress),
      NdType::makeInt(8, false));

  const auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  EXPECT_EQ(Result.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{Address, 8}}));
  const auto Load = Result.Function.Body[0].RetVal;
  ASSERT_EQ(Load->Kind, ExprKind::Load);
  const auto Bound = Load->Operands[0];
  ASSERT_TRUE(Bound->SourceCallHint);
  EXPECT_EQ(Bound->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
  EXPECT_EQ(Bound->SourceCallHint->ByteCount, 8U);
  EXPECT_TRUE(objcSourceCallBound(*Bound, F.Image, {}));
  std::set<std::string> Helpers;
  const auto Source = renderObjCLocalStorageHelpers(
      F.Image, Result.LocalStorageExtents, Helpers);
  EXPECT_EQ(Helpers,
            std::set<std::string>{"neverd_local_storage_1040_address"});
  EXPECT_NE(Source.find("storage[8]"), std::string::npos);
  EXPECT_NE(Source.find("[0] = 31"), std::string::npos);

  F.Image.DataPtrRelocSlots.insert(Address);
  const auto Rejected = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_FALSE(Rejected.Limitation.empty());
  EXPECT_TRUE(Rejected.LocalStorageExtents.empty());
}

TEST(ObjCSourceBindings,
     NamedWritableAggregateFieldsShareOneRebuiltStorage) {
  Fixture F;
  constexpr va_t Base = 0x1020;
  F.Image.ObjCSourceReferences.clear();
  F.Image.Segments[0].Flags =
      SegmentFlags::Readable | SegmentFlags::Writable;
  F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
  F.Image.Symbols.push_back({"_MergedGlobals", Base, 0, false});
  F.Image.Symbols.push_back({"_nextStorage", Base + 0x20, 0, false});
  llvm::support::endian::write32le(
      F.Image.Segments[0].Data.data() + Base + 4 - 0x1000, 17);
  llvm::support::endian::write32le(
      F.Image.Segments[0].Data.data() + Base + 20 - 0x1000, 29);
  const auto Field = [](va_t Address) {
    return HighExpr::makeLoad(
        HighExpr::makeConst(Address, 8,
                            ConstantAddressProvenance::DataAddress),
        NdType::makeInt(4, false));
  };
  F.Function.ReturnType = NdType::makeInt(4, false);
  F.Function.Body[0].RetVal = HighExpr::makeBinop(
      NdOp::INT_ADD, Field(Base + 4), Field(Base + 20));

  const auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  EXPECT_EQ(Result.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{Base, 24}}));
  EXPECT_TRUE(Result.ProfileCounterSections.empty());
  const auto &LeftAddress =
      Result.Function.Body[0].RetVal->Operands[0]->Operands[0];
  const auto &RightAddress =
      Result.Function.Body[0].RetVal->Operands[1]->Operands[0];
  for (const auto &Address : {LeftAddress, RightAddress}) {
    ASSERT_EQ(Address->Kind, ExprKind::BinOp);
    ASSERT_EQ(Address->Operands.size(), 2U);
    ASSERT_TRUE(Address->Operands[0]->SourceCallHint);
    EXPECT_EQ(Address->Operands[0]->SourceCallHint->TargetAddress, Base);
    EXPECT_TRUE(objcSourceCallBound(*Address->Operands[0], F.Image, {}));
  }
  EXPECT_EQ(LeftAddress->Operands[0]->SourceCallHint->ByteCount, 8U);
  EXPECT_EQ(RightAddress->Operands[0]->SourceCallHint->ByteCount, 24U);
  std::set<std::string> Helpers;
  const auto Source = renderObjCLocalStorageHelpers(
      F.Image, Result.LocalStorageExtents, Helpers);
  EXPECT_EQ(Helpers,
            std::set<std::string>{"neverd_local_storage_1020_address"});
  EXPECT_NE(Source.find("storage[24]"), std::string::npos);
  EXPECT_NE(Source.find("[4] = 17"), std::string::npos);
  EXPECT_NE(Source.find("[20] = 29"), std::string::npos);

  auto Split = F.Image;
  Split.Symbols.push_back({"_intervening", Base + 22, 0, false});
  const auto Rejected = bindObjCSourceReferences(F.Function, Split);
  EXPECT_FALSE(Rejected.Limitation.empty());
  EXPECT_EQ(Rejected.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{Base, 8}}));

  auto Relocated = F.Image;
  Relocated.DataPtrRelocSlots.insert(Base + 16);
  const auto PointerRejected =
      bindObjCSourceReferences(F.Function, Relocated);
  EXPECT_FALSE(PointerRejected.Limitation.empty());
  EXPECT_EQ(PointerRejected.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{Base, 8}}));
}

TEST(ObjCSourceBindings,
     SwiftBeginAccessUsesTypedOrMemoryAccessProvedLocalStorage) {
  Fixture F;
  constexpr va_t Address = 0x1040;
  constexpr va_t Slot = 0x10e0;
  F.Image.ObjCSourceReferences.clear();
  F.Image.Segments[0].Flags =
      SegmentFlags::Readable | SegmentFlags::Writable;
  F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
  F.Image.Symbols.push_back(
      {"_$s4Test3BoxC7enabledSbvpZ", Address, 1, false});
  F.Image.Segments[0].Data[Address - 0x1000] = 1;
  F.Image.ImportPtrSlots[Slot] = "_swift_beginAccess";
  ASSERT_TRUE(F.Image.recordDyldBindSlot(
      Slot, "_swift_beginAccess", 0, "/usr/lib/swift/libswiftCore.dylib",
      false));
  const auto Hint = swiftRuntimeSourceCallHint(F.Image, Slot);
  ASSERT_TRUE(Hint);

  auto Call = HighExpr::makeCall(
      "swift_beginAccess", Slot,
      {HighExpr::makeConst(Address, 8,
                           ConstantAddressProvenance::DataAddress),
       HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8),
       HighExpr::makeConst(0, 8)});
  Call->Type = NdType::makeVoid();
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
  HighStmt Access;
  Access.Kind = StmtKind::ExprStmt;
  Access.CallExpr = Call;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeLoad(
      HighExpr::makeConst(Address, 8,
                         ConstantAddressProvenance::DataAddress),
      NdType::makeInt(1, false));
  F.Function.ReturnType = NdType::makeInt(1, false);
  F.Function.Body = {Access, Return};

  const auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  EXPECT_EQ(Result.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{Address, 1}}));
  const auto Marker = Result.Function.Body[0].CallExpr->Operands[0];
  ASSERT_TRUE(Marker->SourceCallHint);
  EXPECT_EQ(Marker->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
  EXPECT_EQ(Marker->SourceCallHint->TargetAddress, Address);
  EXPECT_EQ(Marker->SourceCallHint->ByteCount, 1U);
  EXPECT_TRUE(objcSourceCallBound(*Marker, F.Image, {}));
  const auto Storage = Result.Function.Body[1].RetVal->Operands[0];
  ASSERT_TRUE(Storage->SourceCallHint);
  EXPECT_EQ(Storage->SourceCallHint->TargetAddress, Address);

  auto Forged = *Hint;
  Forged.Signature.Parameters[0].Location.RegisterOffset += 8;
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Forged);
  const auto Rejected = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_FALSE(Rejected.Limitation.empty());
  EXPECT_EQ(Rejected.Function.Body[0].CallExpr->Operands[0]->Kind,
            ExprKind::Const);

  const auto RebuiltAddress = [] {
    ExprPtr Result = HighExpr::makeConst((Address >> 56) & 0xff, 1);
    for (int Shift = 48, Width = 2; Shift >= 0; Shift -= 8, ++Width) {
      Result = HighExpr::makeBinop(
          NdOp::CONCAT, Result,
          HighExpr::makeConst((Address >> Shift) & 0xff, 1));
      Result->Type = NdType::makeInt(Width, false);
    }
    return Result;
  };
  const auto MarkerFunction = [&](ExprPtr AddressExpression) {
    auto MarkerCall = HighExpr::makeCall(
        "swift_beginAccess", Slot,
        {std::move(AddressExpression), HighExpr::makeConst(0, 8),
         HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8)});
    MarkerCall->Type = NdType::makeVoid();
    MarkerCall->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    HighStmt Marker;
    Marker.Kind = StmtKind::ExprStmt;
    Marker.CallExpr = std::move(MarkerCall);
    HighFunc Function;
    Function.ReturnType = NdType::makeVoid();
    Function.Body = {std::move(Marker)};
    return Function;
  };
  const auto Typed =
      bindObjCSourceReferences(MarkerFunction(RebuiltAddress()), F.Image);
  ASSERT_TRUE(Typed.Limitation.empty()) << Typed.Limitation;
  EXPECT_EQ(Typed.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{Address, 1}}));
  ASSERT_TRUE(Typed.Function.Body[0].CallExpr->Operands[0]->SourceCallHint);
  EXPECT_EQ(Typed.Function.Body[0]
                .CallExpr->Operands[0]
                ->SourceCallHint->ByteCount,
            1U);

  auto InvalidShift = HighExpr::makeBinop(
      NdOp::INT_LEFT,
      HighExpr::makeConst(Address, 8,
                         ConstantAddressProvenance::DataAddress),
      HighExpr::makeConst(64, 8));
  InvalidShift->Type = NdType::makeInt(8, false);
  const auto UnboundedShift = bindObjCSourceReferences(
      MarkerFunction(std::move(InvalidShift)), F.Image);
  EXPECT_FALSE(UnboundedShift.Limitation.empty());
  EXPECT_TRUE(UnboundedShift.LocalStorageExtents.empty());

  for (const char *Name :
       {"_untypedStorage", "_$s4Test3BoxC5valueSSvpZ",
        "_$s4Test3BoxC7enabledSbvgZ"}) {
    SCOPED_TRACE(Name);
    const auto Symbol = llvm::find_if(F.Image.Symbols, [](const auto &S) {
      return S.Addr == Address && !S.IsFunc;
    });
    ASSERT_NE(Symbol, F.Image.Symbols.end());
    Symbol->Name = Name;
    EXPECT_FALSE(swiftStaticScalarStorageWidth(Symbol->Name));
    const auto Function = MarkerFunction(RebuiltAddress());
    EXPECT_TRUE(objc_binding_detail::directLocalStorageAccessExtents(
                    Function, F.Image)
                    .empty());
    const auto Unproved = bindObjCSourceReferences(Function, F.Image);
    EXPECT_FALSE(Unproved.Limitation.empty());
    EXPECT_TRUE(Unproved.LocalStorageExtents.empty());
    EXPECT_EQ(Unproved.Function.Body[0].CallExpr->Operands[0]->Kind,
              ExprKind::BinOp);
  }

  const auto Symbol = llvm::find_if(F.Image.Symbols, [](const auto &S) {
    return S.Addr == Address && !S.IsFunc;
  });
  ASSERT_NE(Symbol, F.Image.Symbols.end());
  Symbol->Name = "_$s4Test3BoxC5countSivpZ";
  F.Image.Symbols.push_back({"_nextStorage", Address + 4, 1, false});
  const auto Overlapping = bindObjCSourceReferences(
      MarkerFunction(RebuiltAddress()), F.Image);
  EXPECT_FALSE(Overlapping.Limitation.empty());
  EXPECT_TRUE(Overlapping.LocalStorageExtents.empty());
}

TEST(ObjCSourceBindings,
     OnceTokensBindOnlyAuthenticatedZeroInitializedStorage) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    Fixture F;
    F.Image.Arch = Architecture;
    constexpr va_t Address = 0x1040;
    constexpr va_t Slot = 0x10e0;
    F.Image.ObjCSourceReferences.clear();
    F.Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
    F.Image.Symbols.push_back({"_onceToken", Address, 8, false});
    F.Image.ImportPtrSlots[Slot] = "_dispatch_once";
    ASSERT_TRUE(F.Image.recordDyldBindSlot(
        Slot, "_dispatch_once", 0, "/usr/lib/system/libdispatch.dylib", false));
    const auto Hint = darwinRuntimeSourceCallHint(F.Image, Slot);
    ASSERT_TRUE(Hint);
    auto Call = HighExpr::makeCall(
        "dispatch_once", Slot,
        {HighExpr::makeConst(Address, 8,
                             ConstantAddressProvenance::DataAddress),
         HighExpr::makeConst(0, 8)});
    Call->Type = NdType::makeVoid();
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    F.Function.Body[0].RetVal = Call;
    const auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_EQ(Bound.LocalStorageExtents,
              (std::map<va_t, uint64_t>{{Address, 8}}));
    EXPECT_TRUE(objcSourceCallBound(*Bound.Function.Body[0].RetVal->Operands[0],
                                    F.Image, {}));
    for (unsigned Case = 0; Case < 4; ++Case) {
      auto Image = F.Image;
      auto Forged = *Hint;
      if (Case == 0)
        Image.Segments[0].Data[Address - Image.Segments[0].VA] = 1;
      if (Case == 1)
        Image.Symbols.clear();
      if (Case == 2)
        Forged.Signature.Parameters[0].Location.RegisterOffset += 8;
      if (Case == 3)
        Image.DyldBindSlots[Slot].Module = "/tmp/libdispatch.dylib";
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Forged);
      EXPECT_TRUE(bindObjCSourceReferences(F.Function, Image)
                      .LocalStorageExtents.empty())
          << Case;
    }
    EXPECT_FALSE(
        objc_binding_detail::oncePredicateStorageHint(F.Image, Address + 1));
  }
}

TEST(ObjCSourceBindings, UnfairLocksBindExactNamedFourByteStorage) {
  Fixture F;
  constexpr va_t Address = 0x1040;
  constexpr va_t Slot = 0x10e0;
  F.Image.ObjCSourceReferences.clear();
  F.Image.Segments[0].Flags =
      SegmentFlags::Readable | SegmentFlags::Writable;
  F.Image.Sections[0].Flags = F.Image.Segments[0].Flags;
  F.Image.Symbols.push_back({"_providerLock", Address, 4, false});
  F.Image.ImportPtrSlots[Slot] = "_os_unfair_lock_lock";
  ASSERT_TRUE(F.Image.recordDyldBindSlot(
      Slot, "_os_unfair_lock_lock", 0,
      "/usr/lib/system/libsystem_platform.dylib", false));
  const auto Hint = darwinRuntimeSourceCallHint(F.Image, Slot);
  ASSERT_TRUE(Hint);
  auto Call = HighExpr::makeCall(
      "os_unfair_lock_lock", Slot,
      {HighExpr::makeConst(Address, 8,
                           ConstantAddressProvenance::DataAddress)});
  Call->Type = NdType::makeVoid();
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
  F.Function.Body[0].RetVal = Call;

  const auto Result = bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  EXPECT_EQ(Result.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{Address, 4}}));
  const auto Bound = Result.Function.Body[0].RetVal->Operands[0];
  ASSERT_TRUE(Bound->SourceCallHint);
  EXPECT_EQ(Bound->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
  EXPECT_TRUE(objcSourceCallBound(*Bound, F.Image, {}));

  auto Forged = *Hint;
  Forged.Signature.Parameters[0].Location.RegisterOffset += 8;
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Forged);
  const auto Rejected = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_TRUE(Rejected.LocalStorageExtents.empty());
  EXPECT_EQ(Rejected.Function.Body[0].RetVal->Operands[0]->Kind,
            ExprKind::Const);
  EXPECT_FALSE(objcSourceCallBound(*Rejected.Function.Body[0].RetVal, F.Image,
                                   {}));
}

namespace {
struct ProfileFixture : Fixture {
  ProfileFixture() {
    Image.ObjCSourceReferences.clear();
    Image.Segments[0].Name = "__DATA";
    Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Image.Sections[0].Name = "__llvm_prf_cnts";
    Image.Sections[0].SegmentName = "__DATA";
    Image.Sections[0].Flags = Image.Segments[0].Flags;
    for (size_t I = 0; I < 0x100; ++I)
      Image.Segments[0].Data[I] = uint8_t(I * 37 + 9);
  }
};
} // namespace

TEST(ObjCSourceBindings, ProfileCountersKeepOverlappingStorageAndAccessWidths) {
  ProfileFixture F;
  const ObjCProfileStorage Storage(F.Image);
  for (uint16_t Width : {1, 2, 4, 8, 16}) {
    auto Address = HighExpr::makeConst(0x1007, 8);
    F.Function.Body[0].RetVal =
        HighExpr::makeLoad(Address, NdType::makeInt(Width, false));
    const auto Result = bindObjCSourceReferences(F.Function, F.Image, &Storage);
    EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    EXPECT_EQ(Result.ProfileCounterSections, std::set<va_t>{0x1000});
    const auto Load = Result.Function.Body[0].RetVal;
    EXPECT_EQ(Load->Kind, ExprKind::Load);
    EXPECT_EQ(Load->Type->Size, Width);
    const auto BoundAddress = Load->Operands[0];
    ASSERT_EQ(BoundAddress->Kind, ExprKind::BinOp);
    EXPECT_EQ(BoundAddress->Operands[1]->ConstVal, 7U);
    EXPECT_TRUE(
        objcSourceCallBound(*BoundAddress->Operands[0], F.Image, {}, &Storage));
    EXPECT_EQ(Address->ConstVal, 0x1007U);
    // A shared node used outside a memory access still has no source binding.
    F.Function.Body[0].RetVal =
        HighExpr::makeBinop(NdOp::INT_ADD, F.Function.Body[0].RetVal, Address);
    EXPECT_FALSE(bindObjCSourceReferences(F.Function, F.Image, &Storage)
                     .Limitation.empty());
  }
  EXPECT_EQ(Storage.sectionFor(0x10f0, 16), 0x1000U);
  EXPECT_FALSE(Storage.sectionFor(0x10f1, 16));
  EXPECT_FALSE(Storage.sectionFor(UINT64_MAX - 7, 16));
  EXPECT_FALSE(Storage.sectionFor(0x1000, 0));
  std::set<std::string> Names;
  const auto Source = Storage.render({0x1000}, Names);
  EXPECT_EQ(Names,
            std::set<std::string>{"neverd_profile_counters_1000_address"});
  EXPECT_NE(Source.find("[0] = 9"), std::string::npos);
  EXPECT_NE(Source.find("counters[256]"), std::string::npos);
}

TEST(ObjCSourceBindings, ProfileCountersRejectUnprovedStorageAndEffects) {
  for (unsigned Case = 0; Case < 18; ++Case) {
    SCOPED_TRACE(Case);
    ProfileFixture F;
    switch (Case) {
    case 0:
      F.Image.Sections[0].Name = "__llvm_prf_data";
      break;
    case 1:
      F.Image.Sections[0].FileSz--;
      break;
    case 2:
      F.Image.Segments[0].Data.resize(8);
      break;
    case 3:
      F.Image.Sections[0].Flags = SegmentFlags::Readable;
      break;
    case 4:
      F.Image.MachOChainedFixupsAmbiguous = true;
      break;
    case 5:
      F.Image.DataPtrRelocSlots.insert(0x1040);
      break;
    case 6:
      F.Image.MachOResolvedChainedPointerSlots.insert(0x1040);
      break;
    case 7:
      F.Image.DyldBindSlots[0x1040] = {};
      break;
    case 8:
      F.Image.BaseRelocations.push_back({0xff9, 0});
      break;
    case 9:
      F.Image.Sections.push_back(F.Image.Sections[0]);
      break;
    case 10:
      F.Image.Segments.push_back(F.Image.Segments[0]);
      break;
    case 11:
      F.Image.IsRelocatable = true;
      break;
    case 12:
      F.Image.Arch = Arch::ARM;
      break;
    case 13:
      F.Function.Body[0].RetVal->MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 14:
      F.Function.Body[0].RetVal->Type = NdType::makePtr(NdType::makeVoid());
      break;
    case 15:
      F.Function.Body[0].RetVal->Operands[0] = HighExpr::makeConst(0x10f9, 8);
      break;
    case 16:
      F.Function.Body[0].RetVal->Type = NdType::makeFloat(16);
      break;
    case 17:
      F.Image.Segments[0].FileSz = 16;
      break;
    }
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_FALSE(Result.Limitation.empty());
    EXPECT_TRUE(Result.ProfileCounterSections.empty());
  }
}

TEST(ObjCSourceBindings, ProfileCountersExecuteAcrossTranslationUnits) {
  ProfileFixture F;
  const ObjCProfileStorage Storage(F.Image);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-profile-storage",
                                                    Directory));
  const std::filesystem::path Work(Directory.str().str());
  struct Cleanup {
    std::filesystem::path Work;
    ~Cleanup() {
      std::error_code Error;
      std::filesystem::remove_all(Work, Error);
    }
  } Cleanup{Work};
  std::vector<std::string> Sources;
  std::set<va_t> Used;
  for (bool Store : {false, true}) {
    for (uint16_t Width : {1, 2, 4, 8, 16}) {
      const auto Type = NdType::makeInt(Width, false);
      HighFunc Function;
      Function.Name =
          std::string(Store ? "put" : "get") + std::to_string(Width);
      Function.Entry = 0x2000 + Sources.size() * 16;
      Function.ReturnType = Store ? NdType::makeVoid() : Type;
      const auto Address = HighExpr::makeConst(0x1007, 8);
      if (Store) {
        Function.Params.push_back({"arg0", Type});
        MedVar Value;
        Value.Kind = MedVar::Param;
        Value.Id = 0;
        Value.Size = Width;
        Value.TheArch = Arch::X64;
        HighStmt Statement;
        Statement.Kind = StmtKind::Store;
        Statement.StoreAddr = Address;
        Statement.StoreVal = HighExpr::makeVar(Value, Type);
        Function.Body.push_back(Statement);
      }
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      if (!Store)
        Return.RetVal = HighExpr::makeLoad(Address, Type);
      Function.Body.push_back(Return);
      auto Bound = bindObjCSourceReferences(Function, F.Image, &Storage);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      Used.insert(Bound.ProfileCounterSections.begin(),
                  Bound.ProfileCounterSections.end());
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      CEmitterOptions Options;
      Options.TheArch = Arch::X64;
      ASSERT_TRUE(HighCEmitter().emit({Bound.Function}, OS, Options));
      const auto Path = (Work / (Function.Name + ".c")).string();
      std::ofstream(Path) << Source;
      Sources.push_back(Path);
    }
  }
  std::set<std::string> Helpers;
  std::string Harness = "#include <stdint.h>\n#include <string.h>\n" +
                        Storage.render(Used, Helpers);
  for (uint16_t Width : {1, 2, 4, 8, 16}) {
    const auto Type = Width == 16 ? "unsigned __int128"
                                  : "uint" + std::to_string(Width * 8) + "_t";
    Harness += "extern " + Type + " get" + std::to_string(Width) + "(void);\n";
    Harness += "extern void put" + std::to_string(Width) + "(" + Type + ");\n";
  }
  Harness += R"(
int main(void) {
  unsigned char expected[256];
  unsigned char *data = (unsigned char *)neverd_profile_counters_1000_address();
  for (unsigned i = 0; i != 256; ++i) expected[i] = (unsigned char)(i * 37 + 9);
  if (memcmp(data, expected, sizeof expected)) return 1;
  for (unsigned round = 0; round != 64; ++round) {
    unsigned __int128 value = ((unsigned __int128)(UINT64_MAX - round) << 64) | round;
    put16(value); memcpy(expected + 7, &value, 16);
    if (get16() != value || memcmp(data, expected, sizeof expected)) return 2;
    uint64_t a = UINT64_MAX - round; put8(a); memcpy(expected + 7, &a, 8);
    if (get8() != a || memcmp(data, expected, sizeof expected)) return 3;
    uint32_t b = UINT32_MAX - round; put4(b); memcpy(expected + 7, &b, 4);
    if (get4() != b || memcmp(data, expected, sizeof expected)) return 4;
    uint16_t c = UINT16_MAX - round; put2(c); memcpy(expected + 7, &c, 2);
    if (get2() != c || memcmp(data, expected, sizeof expected)) return 5;
    uint8_t d = (uint8_t)round; put1(d); memcpy(expected + 7, &d, 1);
    if (get1() != d || memcmp(data, expected, sizeof expected)) return 6;
    memcpy(&value, expected + 7, 16);
    if (get16() != value) return 7;
  }
  return 0;
}
)";
  const auto HarnessPath = (Work / "harness.c").string();
  std::ofstream(HarnessPath) << Harness;
  Sources.push_back(HarnessPath);
  const std::string Compiler = NEVERD_TEST_CLANG;
  const auto Executable = (Work / "test.exe").string();
  const auto ErrorPath = (Work / "stderr").string();
  std::vector<std::string> Arguments{
      Compiler,  "-std=c11", "-O3",     "-fstrict-aliasing",
      "-Werror", "-o",       Executable};
  Arguments.insert(Arguments.end(), Sources.begin(), Sources.end());
  std::vector<llvm::StringRef> Refs(Arguments.begin(), Arguments.end());
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt,
                                                      std::nullopt, ErrorPath};
  std::string Error;
  const auto Status = llvm::sys::ExecuteAndWait(Compiler, Refs, std::nullopt,
                                                Redirects, 60, 0, &Error);
  auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
  ASSERT_EQ(Status, 0) << Error << (Errors ? (*Errors)->getBuffer().str() : "");
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}, std::nullopt,
                                      Redirects, 30, 0, &Error),
            0)
      << Error;
}

TEST(ObjCSourceBindings, ExplicitABIPositionDriftIsDetected) {
  SourceFunctionTypeHint Hint;
  Hint.ReturnType = NdType::makeInt(8);
  Hint.Parameters = {{"objc_self", NdType::makePtr(NdType::makeVoid())},
                     {"objc_cmd", NdType::makePtr(NdType::makeVoid())}};
  std::string Reason;
  ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Arch::AArch64, Reason));
  auto Changed = Hint;
  Changed.Parameters[1].Location.RegisterOffset += 8;
  EXPECT_FALSE(objc_projection_detail::sameHint(Hint, Changed));
  Changed = Hint;
  Changed.ReturnLocation.RegisterOffset += 8;
  EXPECT_FALSE(objc_projection_detail::sameHint(Hint, Changed));
}

namespace {
struct ObjectFixture {
  BinaryImage Image;
  static constexpr va_t ClassAddress = 0x2020;
  static constexpr va_t MetaAddress = 0x2080;
  static constexpr va_t ClassSlot = 0x2010;
  static constexpr va_t RuntimeSlot = 0x2f00;
  ObjectFixture() {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Segment Segment;
    Segment.VA = 0x2000;
    Segment.Size = Segment.FileSz = 0x1000;
    Segment.Flags = SegmentFlags::Readable;
    Segment.Data.resize(0x1000);
    Image.Segments.push_back(Segment);
    Section Section;
    Section.VA = 0x2000;
    Section.Size = Section.FileSz = 0x1000;
    Section.Flags = SegmentFlags::Readable;
    Image.Sections.push_back(Section);
    put(ClassAddress, MetaAddress);
    put(ClassAddress + 32, 0x2200);
    put(MetaAddress + 32, 0x2280);
    put(0x2280, 1); // RO_META
    put(0x2218, 0x2380);
    put(0x2298, 0x2380);
    const char Name[] = "Receiver";
    std::copy(std::begin(Name), std::end(Name),
              Image.Segments[0].Data.begin() + 0x380);
    ObjCClass Class;
    Class.Address = ClassAddress;
    Class.Name = "Receiver";
    Image.ObjCClasses.push_back(Class);
    Image.ObjCSourceReferences[ClassSlot] = {
        ObjCSourceReference::Kind::Class, ClassSlot, 8, "Receiver", {}};
    Image.ImportPtrSlots[RuntimeSlot] = "_objc_opt_self";
    EXPECT_TRUE(Image.recordDyldBindSlot(RuntimeSlot, "_objc_opt_self", 0,
                                         "/usr/lib/libobjc.A.dylib", false));
  }
  void put(va_t Address, uint64_t Value) {
    llvm::support::endian::write64le(
        Image.Segments[0].Data.data() + Address - 0x2000, Value);
  }
  HighFunc message(ExprPtr Receiver) {
    auto Binding = std::make_shared<SourceCallTypeHint>();
    Binding->CallKind = SourceCallTypeHint::Kind::ObjCMessage;
    Binding->TargetName = "objc_msgSend";
    Binding->Selector = "answer";
    Binding->Signature.ReturnType = NdType::makeInt(4);
    Binding->Signature.Parameters = {
        {"objc_self", NdType::makePtr(NdType::makeVoid())},
        {"objc_cmd", NdType::makePtr(NdType::makeVoid())}};
    std::string Error;
    EXPECT_TRUE(
        assignDarwinObjCSourceABI(Binding->Signature, Image.Arch, Error));
    auto Call = HighExpr::makeCall("objc_msgSend", 0,
                                   {Receiver, HighExpr::makeConst(0, 8)});
    Call->SourceCallHint = Binding;
    HighFunc Function;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = Call;
    Function.Body.push_back(Return);
    return Function;
  }
  HighFunc runtime(ExprPtr Object) {
    const auto Binding = objcRuntimeSourceCallHint(Image, RuntimeSlot);
    EXPECT_TRUE(Binding);
    auto Call = HighExpr::makeCall("objc_opt_self", RuntimeSlot, {Object});
    Call->Type = NdType::makePtr(NdType::makeVoid());
    if (Binding)
      Call->SourceCallHint =
          std::make_shared<SourceCallTypeHint>(*Binding);
    HighFunc Function;
    Function.ReturnType = Call->Type;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = Call;
    Function.Body.push_back(Return);
    return Function;
  }
};
} // namespace

TEST(ObjCSourceBindings,
     DirectClassAndMetaclassReceiverUseVerifiedObjectIdentity) {
  ObjectFixture Fixture;
  for (va_t Address :
       {ObjectFixture::ClassAddress, ObjectFixture::MetaAddress}) {
    auto Original = Fixture.message(HighExpr::makeConst(Address, 8));
    auto Result = bindObjCSourceReferences(Original, Fixture.Image);
    EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    auto Receiver = Result.Function.Body[0].RetVal->Operands[0];
    ASSERT_TRUE(Receiver->SourceCallHint);
    EXPECT_EQ(Receiver->SourceCallHint->TargetName, "Receiver");
    EXPECT_EQ(Receiver->SourceCallHint->CallKind,
              Address == ObjectFixture::ClassAddress
                  ? SourceCallTypeHint::Kind::RuntimeClass
                  : SourceCallTypeHint::Kind::RuntimeMetaclass);
    EXPECT_TRUE(objcSourceCallBound(*Receiver, Fixture.Image, {}));
    EXPECT_EQ(Original.Body[0].RetVal->Operands[0]->Kind, ExprKind::Const);
  }
}

TEST(ObjCSourceBindings,
     DirectClassRuntimeArgumentsUseVerifiedObjectIdentity) {
  ObjectFixture Fixture;
  for (va_t Address :
       {ObjectFixture::ClassAddress, ObjectFixture::MetaAddress}) {
    auto Original = Fixture.runtime(HighExpr::makeConst(Address, 8));
    auto Result = bindObjCSourceReferences(Original, Fixture.Image);
    EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    auto Call = Result.Function.Body[0].RetVal;
    ASSERT_TRUE(Call->SourceCallHint);
    ASSERT_EQ(Call->Operands.size(), 1U);
    auto Object = Call->Operands[0];
    ASSERT_TRUE(Object->SourceCallHint);
    EXPECT_EQ(Object->SourceCallHint->TargetName, "Receiver");
    EXPECT_EQ(Object->SourceCallHint->CallKind,
              Address == ObjectFixture::ClassAddress
                  ? SourceCallTypeHint::Kind::RuntimeClass
                  : SourceCallTypeHint::Kind::RuntimeMetaclass);
    EXPECT_TRUE(objcSourceCallBound(*Object, Fixture.Image, {}));
    EXPECT_TRUE(objcSourceCallBound(*Call, Fixture.Image, {}));
    EXPECT_EQ(Original.Body[0].RetVal->Operands[0]->Kind, ExprKind::Const);
  }
}

TEST(ObjCSourceBindings,
     DirectClassRuntimeArgumentsRequireAnExactImportedBinding) {
  for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
    ObjectFixture Fixture;
    auto Function = Fixture.runtime(
        HighExpr::makeConst(ObjectFixture::ClassAddress, 8));
    if (Mutation == 0)
      Fixture.Image.DyldBindSlots[ObjectFixture::RuntimeSlot].Module =
          "/tmp/libobjc.A.dylib";
    else if (Mutation == 1)
      Fixture.Image.DyldBindSlots[ObjectFixture::RuntimeSlot].Addend = 1;
    else
      Fixture.Image.ImportPtrSlots[ObjectFixture::RuntimeSlot] =
          "_objc_opt_class";
    const auto Result = bindObjCSourceReferences(Function, Fixture.Image);
    EXPECT_FALSE(Result.Limitation.empty()) << Mutation;
    EXPECT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind,
              ExprKind::Const)
        << Mutation;
  }
}

TEST(ObjCSourceBindings,
     ObjectReceiverRewriteDoesNotAffectSharedScalarConstant) {
  ObjectFixture Fixture;
  auto Shared = HighExpr::makeConst(ObjectFixture::ClassAddress, 8);
  auto Function = Fixture.message(Shared);
  HighStmt Scalar;
  Scalar.Kind = StmtKind::Return;
  Scalar.RetVal = Shared;
  Function.Body.push_back(Scalar);
  auto Result = bindObjCSourceReferences(Function, Fixture.Image);
  ASSERT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind, ExprKind::Call);
  EXPECT_EQ(Result.Function.Body[1].RetVal->Kind, ExprKind::Const);
  EXPECT_FALSE(Result.Limitation.empty());
}

TEST(ObjCSourceBindings, ClassrefSlotAddressCannotBecomeClassObjectReceiver) {
  ObjectFixture Fixture;
  auto Function =
      Fixture.message(HighExpr::makeConst(ObjectFixture::ClassSlot, 8));
  auto Result = bindObjCSourceReferences(Function, Fixture.Image);
  EXPECT_FALSE(Result.Limitation.empty());
  EXPECT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind, ExprKind::Const);
  Function = Fixture.message(HighExpr::makeLoad(
      HighExpr::makeConst(ObjectFixture::ClassSlot, 8), NdType::makeInt(8)));
  Result = bindObjCSourceReferences(Function, Fixture.Image);
  EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  auto Receiver = Result.Function.Body[0].RetVal->Operands[0];
  ASSERT_TRUE(Receiver->SourceCallHint);
  EXPECT_EQ(Receiver->SourceCallHint->TargetAddress, ObjectFixture::ClassSlot);
}

TEST(ObjCSourceBindings, MetaObjectNeedsResolvedIsaAndMatchingMetadata) {
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    ObjectFixture Fixture;
    if (Mutation == 0)
      Fixture.Image.MachOHasChainedFixups = true;
    if (Mutation == 1)
      Fixture.put(0x2280, 0); // A class is not a metaclass.
    if (Mutation == 2)
      Fixture.Image.ObjCClasses.push_back(Fixture.Image.ObjCClasses[0]);
    if (Mutation == 3)
      ++Fixture.Image.Sections[0].FileOff;
    auto Function =
        Fixture.message(HighExpr::makeConst(ObjectFixture::MetaAddress, 8));
    auto Result = bindObjCSourceReferences(Function, Fixture.Image);
    EXPECT_FALSE(Result.Limitation.empty()) << Mutation;
    EXPECT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind,
              ExprKind::Const);
  }
  ObjectFixture Fixture;
  Fixture.Image.MachOHasChainedFixups = true;
  Fixture.Image.MachOResolvedChainedPointerSlots = {
      ObjectFixture::ClassAddress, ObjectFixture::ClassAddress + 32,
      ObjectFixture::MetaAddress + 32, 0x2218, 0x2298};
  auto Function =
      Fixture.message(HighExpr::makeConst(ObjectFixture::MetaAddress, 8));
  EXPECT_TRUE(
      bindObjCSourceReferences(Function, Fixture.Image).Limitation.empty());
}

TEST(ObjCSourceBindings,
     NativeOrSuperStructurePointersAreNotMessageObjectReceivers) {
  ObjectFixture Fixture;
  for (auto Kind : {SourceCallTypeHint::Kind::Native,
                    SourceCallTypeHint::Kind::ObjCSuper2}) {
    auto Function =
        Fixture.message(HighExpr::makeConst(ObjectFixture::ClassAddress, 8));
    auto Binding = std::make_shared<SourceCallTypeHint>(
        *Function.Body[0].RetVal->SourceCallHint);
    Binding->CallKind = Kind;
    Function.Body[0].RetVal->SourceCallHint = Binding;
    auto Result = bindObjCSourceReferences(Function, Fixture.Image);
    EXPECT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind,
              ExprKind::Const);
    EXPECT_FALSE(Result.Limitation.empty());
  }
}

TEST(ObjCSourceBindings, FormatArgumentsRequireCompleteConsistentSlots) {
  auto Parse = [](llvm::StringRef Text) {
    std::vector<uint16_t> Units(Text.begin(), Text.end());
    return objcFormatArgumentTypes(Units);
  };
  auto Types = Parse("text %% %@ %d %hhu %lld %zu %*.*f %s %S %p");
  ASSERT_TRUE(Types);
  ASSERT_EQ(Types->size(), 11U);
  EXPECT_EQ((*Types)[0]->Kind, NdTypeKind::Ptr);
  EXPECT_EQ((*Types)[1]->Size, 4U);
  EXPECT_TRUE((*Types)[2]->IsSigned);
  EXPECT_EQ((*Types)[3]->Size, 8U);
  EXPECT_FALSE((*Types)[4]->IsSigned);
  EXPECT_EQ((*Types)[5]->Size, 4U);
  EXPECT_EQ((*Types)[6]->Size, 4U);
  EXPECT_EQ((*Types)[7]->Kind, NdTypeKind::Float);
  auto Positional = Parse("%3$*1$.*2$f / %3$f");
  ASSERT_TRUE(Positional);
  ASSERT_EQ(Positional->size(), 3U);
  EXPECT_EQ((*Positional)[2]->Kind, NdTypeKind::Float);
  EXPECT_TRUE(Parse("%1000000d"));
  for (const char *Text :
       {"%", "%q", "%n", "%Lf", "%ls", "%1$@ %1$d", "%2$@", "%0$d", "%65$d",
        "%d %2$d", "%*2$d", "%1$d %d", "%99999999999999999999$d", "%.*"})
    EXPECT_FALSE(Parse(Text)) << Text;
  EXPECT_FALSE(objcFormatArgumentTypes(std::vector<uint16_t>{'%', 0x12d, 'd'}));
  EXPECT_FALSE(
      objcFormatArgumentTypes(std::vector<uint16_t>{'a', 0, '%', 'd'}));
}

namespace {
ConstantStringFixture formatFixture(Arch Architecture,
                                    const std::string &Format = "%@ %d %.2f") {
  ConstantStringFixture F;
  F.Image.Arch = Architecture;
  F.Image.DynInfo.NeededLibs.push_back(
      "/System/Library/Frameworks/Foundation.framework/Foundation");
  std::copy(Format.begin(), Format.end(), F.Image.Segments[1].Data.begin());
  F.Image.Segments[1].Data[Format.size()] = 0;
  llvm::support::endian::write64le(F.Image.Segments[0].Data.data() + 24,
                                   Format.size());
  return F;
}
} // namespace

TEST(ObjCSourceBindings,
     PredicateFormatsRespectQuotedTokensAndScalarPromotions) {
  auto Parse = [](llvm::StringRef Text) {
    return objcFormatArgumentTypes(
        std::vector<uint16_t>(Text.begin(), Text.end()),
        SourceCallTypeHint::FormatSyntax::Predicate);
  };
  const auto Types = Parse("%K == %@ AND n > %d AND t < %ld AND f > %f");
  ASSERT_TRUE(Types);
  ASSERT_EQ(Types->size(), 5U);
  EXPECT_EQ((*Types)[0]->Kind, NdTypeKind::Ptr);
  EXPECT_EQ((*Types)[1]->Kind, NdTypeKind::Ptr);
  EXPECT_EQ((*Types)[2]->Size, 4);
  EXPECT_TRUE((*Types)[2]->IsSigned);
  EXPECT_EQ((*Types)[3]->Size, 8);
  EXPECT_EQ((*Types)[4]->Kind, NdTypeKind::Float);
  const auto Quoted = Parse("\"%@ %d\" == '%K' OR SELF == %@");
  ASSERT_TRUE(Quoted);
  ASSERT_EQ(Quoted->size(), 1U);
  EXPECT_EQ(Quoted->front()->Kind, NdTypeKind::Ptr);
  ASSERT_TRUE(Parse("TRUEPREDICATE"));
  EXPECT_TRUE(Parse("TRUEPREDICATE")->empty());
  for (const char *Bad :
       {"'unclosed %@", "\"unclosed %K", "'escaped\\' %@'", "%2$@", "%*d",
        "%.2f", "%n", "%s", "%p", "%lK", "%Lf", "%", "%h", "%ll", "%%", "%llf"})
    EXPECT_FALSE(Parse(Bad)) << Bad;
  std::string TooMany;
  for (unsigned I = 0; I < 65; ++I)
    TooMany += "%@ ";
  EXPECT_FALSE(Parse(TooMany));
  EXPECT_FALSE(
      objcFormatArgumentTypes(std::vector<uint16_t>{'\'', 0, '\''},
                              SourceCallTypeHint::FormatSyntax::Predicate));
  EXPECT_FALSE(objcFormatArgumentTypes(
      {}, static_cast<SourceCallTypeHint::FormatSyntax>(255)));
}

TEST(ObjCSourceBindings, PredicateCallsRevalidateSyntaxDeclarationAndImage) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (const char *Selector :
         {"predicateWithFormat:", "expressionWithFormat:"}) {
      auto F = formatFixture(A, "'quoted %@' == %@");
      const auto Declaration = objcSelectorFormatDeclaration(F.Image, Selector);
      ASSERT_TRUE(Declaration);
      EXPECT_EQ(Declaration->Syntax,
                SourceCallTypeHint::FormatSyntax::Predicate);
      EXPECT_FALSE(objcSelectorSourceTypeHint(F.Image, Selector));
      const auto Hint = objcFormattedSourceCallHint(F.Image, Selector, 0x2000);
      ASSERT_TRUE(Hint);
      ASSERT_TRUE(Hint->Format);
      ASSERT_EQ(Hint->Signature.Parameters.size(), 4U);
      auto Call = HighExpr::makeCall("objc_msgSend", 0, {});
      Call->Type = Hint->Signature.ReturnType;
      for (unsigned I = 0; I < 4; ++I)
        Call->Operands.push_back(HighExpr::makeConst(I == 2 ? 0x2000 : 0, 8));
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
      auto Bad = std::make_shared<SourceCallTypeHint>(*Hint);
      Bad->Format->Syntax = SourceCallTypeHint::FormatSyntax::NSString;
      Call->SourceCallHint = Bad;
      EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      ObjCMethod Method;
      Method.Selector = Selector;
      Method.TypeHint = Declaration->Signature;
      F.Image.ObjCMethods.push_back(Method);
      EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
      F.Image.ObjCMethods.back().TypeHint->Parameters.back().Type =
          NdType::makeInt(8);
      EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
      F.Image.ObjCMethods.clear();
      F.Image.Segments[1].Data[0] = ' ';
      EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
      F.Image.Segments[1].Data[0] = '\'';
      F.Image.Segments[1].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
      F.Image.Segments[1].Flags = SegmentFlags::Readable;
      F.Image.DynInfo.NeededLibs.clear();
      EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
    }
}

TEST(ObjCSourceBindings, FormattedMessagesRevalidateFormatAndActualArguments) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto F = formatFixture(Architecture);
    EXPECT_FALSE(objcSelectorSourceTypeHint(F.Image, "stringWithFormat:"));
    auto Hint =
        objcFormattedSourceCallHint(F.Image, "stringWithFormat:", 0x2000);
    ASSERT_TRUE(Hint);
    ASSERT_TRUE(Hint->Format);
    ASSERT_EQ(Hint->Signature.Parameters.size(), 6U);
    EXPECT_EQ(Hint->Format->FixedCount, 3U);
    auto Call = HighExpr::makeCall("objc_msgSend", 0, {});
    Call->Type = NdType::makePtr(NdType::makeVoid());
    for (unsigned I = 0; I < 6; ++I)
      Call->Operands.push_back(HighExpr::makeConst(I == 2 ? 0x2000 : 0, 8));
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      auto Bad = *Call;
      auto Binding = std::make_shared<SourceCallTypeHint>(*Hint);
      Bad.SourceCallHint = Binding;
      if (Mutation == 0)
        Binding->Format->FixedCount = 4;
      if (Mutation == 1)
        Binding->Format->FormatParameter = 1;
      if (Mutation == 2)
        Bad.Operands[2] = HighExpr::makeConst(0x2020, 8);
      if (Mutation == 3)
        Bad.Operands.pop_back();
      if (Mutation == 4)
        Binding->Signature.Parameters.back().Type = NdType::makeInt(8);
      if (Mutation == 5)
        Binding->CallKind = SourceCallTypeHint::Kind::Native;
      EXPECT_FALSE(objcSourceCallBound(Bad, F.Image, {})) << Mutation;
    }
    F.Function.Body.front().RetVal = Call;
    // A separately assigned format may be shared elsewhere; source binding
    // must prove this argument occurrence against the original object.
    auto Bound = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    EXPECT_TRUE(
        objcSourceCallBound(*Bound.Function.Body.front().RetVal, F.Image, {}));
    F.Image.Segments[1].Data[1] = 'n';
    EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
  }
}

TEST(ObjCSourceBindings,
     FormattedMessagesRespectEveryDeclarationAndImageProof) {
  auto F = formatFixture(Arch::AArch64);
  auto Declared = objcSelectorFormatDeclaration(F.Image, "stringWithFormat:");
  ASSERT_TRUE(Declared);
  ObjCMethod Method;
  Method.Selector = "stringWithFormat:";
  Method.TypeHint = Declared->Signature;
  F.Image.ObjCMethods.push_back(Method);
  EXPECT_TRUE(objcFormattedSourceCallHint(F.Image, Method.Selector, 0x2000));
  F.Image.ObjCMethods.back().TypeHint->Parameters.back().Type =
      NdType::makeInt(8);
  EXPECT_FALSE(objcFormattedSourceCallHint(F.Image, Method.Selector, 0x2000));
  F.Image.ObjCMethods.clear();
  ObjCProtocol Protocol;
  ObjCProtocolMethod PM;
  PM.Selector = Method.Selector;
  Protocol.Methods.push_back(PM);
  F.Image.ObjCProtocols.push_back(Protocol);
  EXPECT_FALSE(objcFormattedSourceCallHint(F.Image, Method.Selector, 0x2000));
  F.Image.ObjCProtocols.clear();
  F.Image.Segments[1].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  EXPECT_FALSE(objcFormattedSourceCallHint(F.Image, Method.Selector, 0x2000));
  F = formatFixture(Arch::AArch64);
  F.Image.DynInfo.NeededLibs.clear();
  EXPECT_FALSE(objcFormattedSourceCallHint(F.Image, Method.Selector, 0x2000));
}

TEST(ObjCSourceBindings, FormattedLowCallsReadDarwinStackArgumentsBeforeSSA) {
  auto F = formatFixture(Arch::AArch64);
  auto &Image = F.Image;
  const auto &TRI = getTargetRegInfo(Image.Arch);
  Image.ImportPtrSlots[0x3000] = "_objc_msgSend";
  Image.ObjCSourceReferences.emplace(
      0x3008, ObjCSourceReference{ObjCSourceReference::Kind::Selector,
                                  0x3008,
                                  8,
                                  "stringWithFormat:",
                                  {}});
  LowFunc Function;
  Function.Entry = 0x3000;
  Function.Blocks.resize(1);
  auto &Block = Function.Blocks.front();
  Block.StartAddr = 0x3000;
  auto Add = [&](NdOp Code, NdVar Out, std::initializer_list<NdVar> Args) {
    LowOp Op;
    Op.Opcode = Code;
    Op.Output = Out;
    Op.Addr = 0x3000 + Block.Ops.size() * 4;
    for (const auto &Arg : Args)
      Op.addInput(Arg);
    Block.Ops.push_back(Op);
  };
  Add(NdOp::LOAD, NdVar::reg(TRI.IntParamRegs[1], 8), {NdVar::cst(0x3008, 8)});
  Add(NdOp::COPY, NdVar::reg(TRI.IntParamRegs[2], 8), {NdVar::cst(0x2000, 8)});
  Add(NdOp::INDIR_CALL, {}, {NdVar::cst(0x3000, 8)});
  Add(NdOp::RETURN, {}, {});
  const auto Hints = buildObjCSourceCallHints(Image, Function);
  ASSERT_EQ(Hints.size(), 1U);
  ASSERT_TRUE(Hints.begin()->second.Format);
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  Converter.setSourceCallHintsEnabled(true);
  auto Med = Converter.convert(Function, Image.Arch, BinaryFormat::MachO);
  const MedOp *Call = nullptr;
  for (const auto &B : Med.Blocks)
    for (const auto &Op : B.Ops)
      if (Op.SourceCallHint)
        Call = &Op;
  ASSERT_NE(Call, nullptr);
  ASSERT_EQ(Call->NumInputs, 7U);
  for (unsigned I = 3; I < 6; ++I) {
    EXPECT_EQ(Call->SourceCallHint->Signature.Parameters[I].Location.Kind,
              SourceABICarrierKind::Stack);
    EXPECT_EQ(
        Call->SourceCallHint->Signature.Parameters[I].Location.EntryStackOffset,
        (I - 3) * 8);
  }
  // Tail veneers live in another block. A single incoming edge carries the
  // format fact, independent of block storage order; joins cannot borrow it.
  LowFunc Split = Function;
  Split.Blocks[0].Id = 0;
  Split.Blocks[0].Ops.resize(2);
  Split.Blocks[0].Succs = {1};
  LowBlock Tail;
  Tail.Id = 1;
  Tail.StartAddr = 0x3010;
  Tail.Preds = {0};
  Tail.Ops = {Block.Ops[2], Block.Ops[3]};
  Split.Blocks.push_back(Tail);
  EXPECT_EQ(buildObjCSourceCallHints(Image, Split).size(), 1U);
  std::reverse(Split.Blocks.begin(), Split.Blocks.end());
  EXPECT_EQ(buildObjCSourceCallHints(Image, Split).size(), 1U);
  Split.Blocks.front().Preds.push_back(2);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Split).empty());
  // Losing the format register fact must not leave a guessed variadic call.
  Block.Ops.insert(Block.Ops.begin() + 2, Block.Ops[1]);
  Block.Ops[2].Inputs[0] = NdVar::reg(TRI.IntParamRegs[3], 8);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Function).empty());
}

TEST(ObjCSourceBindings, DarwinFormattedCallsRequireExactImportAndFormat) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto F = formatFixture(Architecture);
    Segment Import;
    Import.VA = Import.FileOff = 0x3000;
    Import.Size = Import.FileSz = 8;
    Import.Flags = SegmentFlags::Readable;
    Import.Data.resize(8);
    F.Image.Segments.push_back(Import);
    Section Slot;
    Slot.VA = Slot.FileOff = 0x3000;
    Slot.Size = Slot.FileSz = 8;
    Slot.Flags = SegmentFlags::Readable;
    F.Image.Sections.push_back(Slot);
    ASSERT_TRUE(F.Image.recordDyldBindSlot(
        0x3000, "_NSLog", 0,
        "/System/Library/Frameworks/Foundation.framework/Foundation", false));
    F.Image.ImportPtrSlots[0x3000] = "_NSLog";
    ASSERT_TRUE(darwinRuntimeFormatDeclaration(F.Image, 0x3000));
    ASSERT_TRUE(readObjCConstantString(F.Image, 0x2000));
    auto Hint = darwinFormattedSourceCallHint(F.Image, 0x3000, 0x2000);
    ASSERT_TRUE(Hint);
    ASSERT_TRUE(Hint->Format);
    EXPECT_EQ(Hint->Format->FixedCount, 1U);
    EXPECT_EQ(Hint->Format->FormatParameter, 0U);
    ASSERT_EQ(Hint->Signature.Parameters.size(), 4U);
    auto Call = HighExpr::makeCall(
        "_NSLog", 0x3000,
        {HighExpr::makeConst(0x2000, 8), HighExpr::makeConst(0, 8),
         HighExpr::makeConst(17, 4), HighExpr::makeConst(0, 8)});
    Call->Type = NdType::makeVoid();
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      auto Bad = std::make_shared<SourceCallTypeHint>(*Hint);
      Call->SourceCallHint = Bad;
      if (Mutation == 0)
        Bad->TargetName = "NSLogv";
      if (Mutation == 1)
        Bad->TargetAddress = 0x3008;
      if (Mutation == 2)
        Bad->Format->FixedCount = 2;
      if (Mutation == 3)
        Bad->Format->FormatParameter = 1;
      if (Mutation == 4)
        Bad->Format.reset();
      EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {})) << Mutation;
    }
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    Call->Operands[0] = HighExpr::makeConst(0x2020, 8);
    EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
    F.Image.DyldBindSlots[0x3000].Module = "/tmp/private/Foundation";
    EXPECT_FALSE(darwinFormattedSourceCallHint(F.Image, 0x3000, 0x2000));
    F.Image.DyldBindSlots[0x3000].Module =
        "/System/Library/Frameworks/Foundation.framework/Foundation";
    F.Image.DyldBindSlots[0x3000].WeakImport = true;
    EXPECT_FALSE(darwinFormattedSourceCallHint(F.Image, 0x3000, 0x2000));
  }
}
