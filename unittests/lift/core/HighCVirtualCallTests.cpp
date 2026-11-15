//===- HighCVirtualCallTests.cpp - Virtual slot display ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Source names, navigation and dynamic dispatch from independent vtables.
//===----------------------------------------------------------------------===//

#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/dialect/SourceDialect.h"
#include "neverd/loader/BinaryImage.h"

namespace {
using namespace neverd;

struct VirtualFixture : NullDebugContext {
  BinaryImage Image;
  TypeRef Record = NdType::makeNamedRecord("Device", 8);
  HighFunc Function;
  ExprPtr Call;
  FunctionSym Method;
  CSourceMap Map;
  unsigned Bytes;

  explicit VirtualFixture(Arch Architecture = Arch::X64)
      : Bytes(Architecture == Arch::X86 || Architecture == Arch::ARM ? 4 : 8) {
    Image.Arch = Architecture;
    Image.Format = BinaryFormat::ELF;
    Image.Bits = Bytes == 8 ? Bitness::Bits64 : Bitness::Bits32;
    Segment Data;
    Data.VA = 0x6000;
    Data.Size = Data.FileSz = 6 * Bytes;
    Data.Data.resize(Data.Size);
    Data.Flags = SegmentFlags::Readable;
    for (unsigned I = 2; I < 6; ++I) {
      writePtr(Data.Data.data() + I * Bytes, 0x8000 + (I - 2) * 16, Bytes == 8,
               true);
      Image.CodePtrRelocSlots.insert(Data.VA + I * Bytes);
    }
    Image.Segments.push_back(std::move(Data));
    Segment Text;
    Text.VA = 0x8000;
    Text.Size = Text.FileSz = 64;
    Text.Data.resize(Text.Size);
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Image.Segments.push_back(std::move(Text));
    Image.Symbols = {{"_ZTV6Device", 0x6000, 6 * Bytes, false},
                     {"_ZN6Device5valueEv", 0x8020, 16, true}};
    Method.Name = "Device::value";
    Method.Addr = 0x8020;
    Method.ReturnType = NdType::makeInt(Bytes, false);
    auto Pointer = NdType::makePtr(Record);
    Pointer->Size = Bytes;
    Method.Params = {{"this", Pointer}};
    Method.PlatformConvention = true;
    Function.Entry = 0x9000;
    Function.Name = "dispatch";
    Function.Params = {{"object", Pointer}};
    Function.ReturnType = Method.ReturnType;
    MedVar V;
    V.Kind = MedVar::Param;
    V.Id = 0;
    V.Size = Bytes;
    V.TheArch = Architecture;
    auto Object = HighExpr::makeVar(V, NdType::makeInt(Bytes, false));
    auto Table = HighExpr::makeLoad(Object, NdType::makeInt(Bytes, false));
    auto Address = HighExpr::makeBinop(NdOp::INT_ADD, Table,
                                       HighExpr::makeConst(2 * Bytes, Bytes));
    Call = HighExpr::makeCall("indirect_call", 0, {Object});
    Call->IsIndirectCall = true;
    Call->CallSiteAddr = 0x9004;
    Call->IndirectTarget =
        HighExpr::makeLoad(Address, NdType::makeInt(Bytes, false));
    Call->Type = Method.ReturnType;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = Call;
    Function.Body = {Return};
  }

  std::optional<FunctionSym> resolveFunction(va_t Address) const override {
    return Address == Method.Addr ? std::optional(Method) : std::nullopt;
  }

  std::string emit(bool Prepared = false) {
    CEmitterOptions Options;
    Options.Image = &Image;
    Options.TheArch = Image.Arch;
    Options.Format = Image.Format;
    Options.SourceMap = &Map;
    HighCEmitter Emitter;
    if (Prepared)
      Emitter.prepareImageFunctionNames(Image);
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    EXPECT_TRUE(Emitter.emit({Function}, OS, Options, this));
    return Text;
  }
};

TEST(HighCVirtualCalls, DeclaredSlotRetainsRuntimeOverrideAndNavigation) {
  VirtualFixture F;
  const auto Text = F.emit();
  EXPECT_NE(Text.find("/* virtual Device_value [slot +0x10] */"),
            std::string::npos)
      << Text;
  EXPECT_EQ(Text, F.emit(true));
  const auto Name = llvm::find_if(F.Map.Names, [](const CSourceName &N) {
    return N.Symbol == "_ZN6Device5valueEv";
  });
  ASSERT_NE(Name, F.Map.Names.end());
  EXPECT_EQ(Name->Address, 0x8020u);
  EXPECT_EQ(Name->Identifier, "Device_value");
  ASSERT_EQ(F.Map.VirtualCalls.size(), 1u);
  const auto &Reference = F.Map.VirtualCalls.front();
  EXPECT_EQ(Reference.Function, F.Function.Entry);
  EXPECT_EQ(Reference.CallSite, 0x9004u);
  EXPECT_EQ(Reference.Table, 0x6000u);
  EXPECT_EQ(Reference.Slot, 0x6020u);
  EXPECT_EQ(Reference.SlotOffset, 0x10u);
  EXPECT_EQ(Reference.Target, 0x8020u);
  EXPECT_EQ(Reference.ReceiverType, "Device");
  SourceDialectOptions Dialect;
  Dialect.Dialect = SourceDialect::Cpp;
  Dialect.Names = F.Map.Names;
  const auto Spelled = spellInDialect(Text, Dialect);
  const auto Target = llvm::find_if(Spelled.Names, [](const auto &N) {
    return N.Identifier == "Device_value";
  });
  ASSERT_NE(Target, Spelled.Names.end()) << Spelled.Text;
  EXPECT_EQ(Target->Address, 0x8020u);
  EXPECT_EQ(Target->Symbol, "_ZN6Device5valueEv");
  EXPECT_EQ(Spelled.Text.substr(Target->Begin, Target->End - Target->Begin),
            "Device::value");
  const std::string Literal =
      "const char *note = \"/* virtual Device_value [slot +0x10] */\";\n";
  const auto Quoted = spellInDialect(Literal, Dialect);
  EXPECT_TRUE(Quoted.Names.empty());
  EXPECT_NE(Quoted.Text.find("/* virtual Device_value [slot +0x10] */"),
            std::string::npos);
  for (bool MissingAddress : {false, true}) {
    auto Names = F.Map.Names;
    for (auto &N : Names)
      if (MissingAddress)
        N.Address.reset();
      else
        N.Identifier += "_different";
    Dialect.Names = Names;
    const auto Unbound = spellInDialect(Text, Dialect);
    EXPECT_TRUE(llvm::none_of(Unbound.Names, [](const auto &N) {
      return N.Identifier == "Device_value";
    }));
  }
  source_call_execution_test::compileAndRun(
      Text + R"c(
struct Device { void **vptr; uint64_t value; };
static uint64_t base(Device *p) { return p->value + 1; }
static uint64_t derived(Device *p) { return p->value + 9; }
int main(void) {
  void *a[3] = {0, 0, (void *)base};
  void *b[3] = {0, 0, (void *)derived};
  Device first = {a, 20}, second = {b, 30};
  return dispatch(&first) != 21 || dispatch(&second) != 39;
}
)c",
      {"-Werror=int-conversion", "-Werror=incompatible-pointer-types"});
}

TEST(HighCVirtualCalls, TargetPointerWidthOwnsTheAddressPoint) {
  for (const auto Architecture :
       {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    VirtualFixture F(Architecture);
    EXPECT_NE(F.emit().find("/* virtual Device_value"), std::string::npos);
  }
}

TEST(HighCVirtualCalls, TemplateReceiversKeepTheirOriginalClassIdentity) {
  VirtualFixture F;
  F.Record->SourceName = "sample::Box<sample::Pair<int, unsigned int>, 3>";
  F.Image.Symbols[0].Name = "_ZTVN6sample3BoxINS_4PairIijEELi3EEE";
  F.Image.Symbols[1].Name = "_ZN6sample3BoxINS_4PairIijEELi3EE3getEv";
  F.Method.Name = F.Record->SourceName + "::get";
  F.emit();
  ASSERT_EQ(F.Map.VirtualCalls.size(), 1u);
  EXPECT_EQ(F.Map.VirtualCalls[0].ReceiverType, F.Record->SourceName);
  EXPECT_EQ(F.Method.Params[0].second->Pointee->SourceName,
            F.Record->SourceName);
}

TEST(HighCVirtualCalls, ScopedReceiverDoesNotRetypeOtherCallSites) {
  VirtualFixture F;
  const auto Derived = F.Function.Params.front().Type;
  F.Function.Params.front().Type =
      NdType::makePtr(NdType::makeNamedRecord("Base", 8));
  EXPECT_TRUE(F.Map.VirtualCalls.empty());
  F.emit();
  EXPECT_TRUE(F.Map.VirtualCalls.empty());
  F.Function.DebugCallReceiverTypes.emplace(F.Call->CallSiteAddr, Derived);
  EXPECT_NE(F.emit().find("/* virtual Device_value"), std::string::npos);
  ASSERT_EQ(F.Map.VirtualCalls.size(), 1u);
  EXPECT_EQ(F.Map.VirtualCalls.front().ReceiverType, "Device");
  EXPECT_EQ(F.Function.Params.front().Type->Pointee->SourceName, "Base");
  F.Call->CallSiteAddr += 4;
  F.emit();
  EXPECT_TRUE(F.Map.VirtualCalls.empty());
  F.Call->CallSiteAddr = 0;
  F.emit();
  EXPECT_TRUE(F.Map.VirtualCalls.empty());
}

TEST(HighCVirtualCalls, CopiedReceiversRequireUnchangedValuesAndMemory) {
  for (unsigned Change = 0; Change != 6; ++Change) {
    SCOPED_TRACE(Change);
    VirtualFixture F;
    const auto Parameter = F.Call->Operands[0];
    MedVar Local;
    Local.Kind = MedVar::Temp;
    Local.Id = 4;
    Local.Size = F.Bytes;
    Local.TheArch = F.Image.Arch;
    const auto Copy = HighExpr::makeVar(Local, Parameter->Type);
    HighStmt Assignment;
    Assignment.Kind = StmtKind::Assign;
    Assignment.Dst = Copy;
    Assignment.Val = Parameter;
    const auto Return = F.Function.Body.front();
    F.Function.Body = {Assignment};
    F.Call->Operands[0] = Copy;
    if (Change == 1) { // The saved receiver is no longer the current parameter.
      auto Write = Assignment;
      Write.Dst = Parameter;
      Write.Val = HighExpr::makeConst(0, F.Bytes);
      F.Function.Body.push_back(Write);
    } else if (Change == 2) {
      HighStmt Branch;
      Branch.Kind = StmtKind::IfElse;
      Branch.Cond = Parameter;
      Branch.Body = {Assignment};
      F.Function.Body = {Branch};
    } else if (Change >= 3) {
      // The same loaded pointer may change at a store/call/ordered access.
      const auto Load = HighExpr::makeLoad(Parameter, Parameter->Type);
      F.Function.Body[0].Val = Load;
      F.Call->IndirectTarget->Operands[0]->Operands[0]->Operands[0] = Load;
      HighStmt Effect;
      if (Change == 3) {
        Effect.Kind = StmtKind::Store;
        Effect.StoreAddr = Parameter;
        Effect.StoreVal = HighExpr::makeConst(0, F.Bytes);
      } else if (Change == 4) {
        Effect.Kind = StmtKind::Call;
        Effect.CallExpr = HighExpr::makeCall("touch", 0x8100, {});
      } else {
        Effect = Assignment;
        Effect.Dst = HighExpr::makeVar(Local, Parameter->Type);
        Effect.Dst->Var.Id = 5;
        Effect.Val = std::make_shared<HighExpr>();
        Effect.Val->Kind = ExprKind::Addr;
        Effect.Val->Operands = {Parameter};
        Effect.Val->Type = NdType::makePtr(Parameter->Type);
      }
      F.Function.Body.push_back(Effect);
    }
    F.Function.Body.push_back(Return);
    F.emit();
    EXPECT_EQ(F.Map.VirtualCalls.size(), Change == 0 ? 1u : 0u);
  }
}

TEST(HighCVirtualCalls, SavedTableSurvivesOnlyProvedDisjointStores) {
  for (const Arch Architecture :
       {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (const int64_t Offset : {0, 3, 7, 8, 16, -1, -2}) {
      SCOPED_TRACE(int(Architecture));
      SCOPED_TRACE(Offset);
      VirtualFixture F(Architecture);
      const auto Parameter = F.Call->Operands[0];
      MedVar Local;
      Local.Kind = MedVar::Temp;
      Local.Id = 4;
      Local.Size = F.Bytes;
      Local.TheArch = Architecture;
      const auto Copy = HighExpr::makeVar(Local, Parameter->Type);
      HighStmt Capture;
      Capture.Kind = StmtKind::Assign;
      Capture.Dst = Copy;
      Capture.Val = F.Call->IndirectTarget->Operands[0]->Operands[0];
      F.Call->IndirectTarget->Operands[0]->Operands[0] = Copy;
      HighStmt Store;
      Store.Kind = StmtKind::Store;
      Store.StoreAddr = HighExpr::makeBinop(
          NdOp::INT_ADD, Parameter, HighExpr::makeConst(Offset, F.Bytes));
      Store.StoreVal = HighExpr::makeConst(1, 2);
      F.Function.Body = {Capture, Store, F.Function.Body.front()};
      F.emit();
      const bool Disjoint = Offset >= F.Bytes || Offset <= -2;
      EXPECT_EQ(F.Map.VirtualCalls.size(), Disjoint ? 1u : 0u);
      if (!Disjoint)
        continue;

      F.Function.Body[1].MemoryOrdering =
          NdMemoryOrdering::SequentiallyConsistent;
      F.emit();
      EXPECT_TRUE(F.Map.VirtualCalls.empty());
      F.Function.Body[1].MemoryOrdering = NdMemoryOrdering::None;

      // An unrelated base can alias the table; a narrowed base can wrap onto
      // it.
      auto Other = std::make_shared<HighExpr>(*Parameter);
      Other->Var.Id = 1;
      F.Function.Params.push_back({"other", F.Function.Params[0].Type});
      Store.StoreAddr->Operands[0] = Other;
      F.emit();
      EXPECT_TRUE(F.Map.VirtualCalls.empty());
      auto Narrow = std::make_shared<HighExpr>();
      Narrow->Kind = ExprKind::Cast;
      Narrow->Type = Narrow->CastTo = NdType::makeInt(2, false);
      Narrow->Operands = {Parameter};
      auto Widen = std::make_shared<HighExpr>();
      Widen->Kind = ExprKind::Cast;
      Widen->Type = Widen->CastTo = Parameter->Type;
      Widen->Operands = {Narrow};
      Store.StoreAddr->Operands[0] = Widen;
      F.emit();
      EXPECT_TRUE(F.Map.VirtualCalls.empty());
    }
}

TEST(HighCVirtualCalls, MachODecorationAndRelocatedReadonlyStorageAgree) {
  VirtualFixture F;
  F.Image.Format = BinaryFormat::MachO;
  for (auto &Symbol : F.Image.Symbols)
    Symbol.Name = "_" + Symbol.Name;
  F.Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  F.Image.Segments[0].ReadOnlyAfterRelocations = true;
  EXPECT_NE(F.emit(true).find("/* virtual Device_value"), std::string::npos);
}

TEST(HighCVirtualCalls, RttiMustNameTheSameClass) {
  VirtualFixture F;
  F.Image.Segments[0].Size = F.Image.Segments[0].FileSz = 256;
  F.Image.Segments[0].Data.resize(256);
  writePtr(F.Image.Segments[0].Data.data() + F.Bytes, 0x6080, true, true);
  F.Image.Symbols.push_back({"_ZTI6Device", 0x6080, 16, false});
  EXPECT_NE(F.emit().find("/* virtual Device_value"), std::string::npos);
  F.Image.Symbols.back().Name = "_ZTI5Other";
  EXPECT_EQ(F.emit().find("/* virtual"), std::string::npos);
}

TEST(HighCVirtualCalls, AmbiguousUnboundAndSecondaryTablesStayRaw) {
  for (unsigned Kind = 0; Kind < 8; ++Kind) {
    SCOPED_TRACE(Kind);
    VirtualFixture F;
    if (Kind == 0)
      F.Image.CodePtrRelocSlots.clear();
    if (Kind == 1)
      F.Image.Segments[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
    if (Kind == 2)
      writePtr(F.Image.Segments[0].Data.data(), 8, true, true);
    if (Kind == 3)
      F.Image.Symbols[0].Size = 3 * F.Bytes;
    if (Kind == 4)
      F.Image.Symbols.push_back({"_ZTV6Device", 0x7000, 48, false});
    if (Kind == 5)
      writePtr(F.Image.Segments[0].Data.data() + F.Bytes, 0x7000, true, true);
    if (Kind == 6)
      F.Image.CodePtrRelocSlots.erase(0x6000 + 3 * F.Bytes);
    if (Kind == 7)
      F.Call->Operands[0] = HighExpr::makeConst(1, F.Bytes);
    EXPECT_EQ(F.emit().find("/* virtual"), std::string::npos);
  }
}

TEST(HighCVirtualCalls, FloatingResultCannotRetypeAnIntegerRegister) {
  VirtualFixture F;
  F.Method.ReturnType = NdType::makeFloat(4);
  const auto Text = F.emit();
  EXPECT_NE(Text.find("/* virtual Device_value"), std::string::npos) << Text;
  EXPECT_EQ(Text.find("float (*"), std::string::npos) << Text;
}
TEST(HighCVirtualCalls, NarrowReturnCannotDefineUnobservedUpperBits) {
  VirtualFixture F;
  F.Method.ReturnType = NdType::makeInt(4, false);
  const auto Text = F.emit();
  EXPECT_NE(Text.find("/* virtual Device_value"), std::string::npos) << Text;
  EXPECT_EQ(Text.find("uint32_t (*"), std::string::npos) << Text;
}
} // namespace
