#include "../../lib/loader/Swift/SwiftBooleanSourceBinding.h"
#include "../../lib/sdk/capi/ObjCSourceProjection.h"
#include "../../lib/sdk/capi/SourceSwiftOpaqueValueProjection.h"
#include "../../lib/sdk/capi/SourceSwiftValueConstructorProjection.h"
#include "../lift/core/ImmutableNativeCallFixture.h"
#include "gtest/gtest.h"

#include "neverd/ir/low/SourceFrameAnalysis.h"
#include "neverd/loader/Swift/SwiftOpaqueValueEffects.h"
#include "neverd/pipeline/NativeSourceHints.h"

#include <functional>

using namespace neverd;
namespace {
constexpr va_t Entry = 0x1000, Copy = 0x1200, Destroy = 0x1240;
constexpr va_t Metadata = 0x2800, Equality = 0x2808;
constexpr const char *Provider = "/usr/lib/swift/libswiftCore.dylib";
struct Fixture : immutable_native_call_test::Fixture {
  SourceCallTypeHint CopyBinding, DestroyBinding, EqualBinding;
  const SourceCallOccurrenceKey CopySite{0x1018, 1, NdOp::CALL, Copy};
  const SourceCallOccurrenceKey EqualSite{0x1024, 1, NdOp::CALL, 0x1100};
  const SourceCallOccurrenceKey DestroySite{0x1030, 1, NdOp::CALL, Destroy};
  Fixture() {
    Image.DynInfo.NeededLibs = {Provider};
    Image.ImportPtrSlots.clear();
    Image.DyldBindSlots.clear();
    Image.Segments[1].VA = Metadata & ~va_t(0xfff);
    Image.Sections[0].VA = Metadata & ~va_t(0xfff);
    Image.ImportPtrSlots[Metadata] = "_$ss11AnyHashableVN";
    Image.DyldBindSlots[Metadata] = {Image.ImportPtrSlots[Metadata], 0,
                                     Provider, false};
    Image.ImportPtrSlots[Equality] =
        SwiftBooleanAnyHashableEqualityImport.str();
    Image.DyldBindSlots[Equality] = {Image.ImportPtrSlots[Equality], 0,
                                     Provider, false};
    Image.Segments[1].Size = Image.Segments[1].FileSz = 4096;
    Image.Segments[1].Data.resize(4096);
    Image.Sections[0].Size = Image.Sections[0].FileSz = 4096;
    Image.Symbols = {{"_caller", Entry, 72, true},
                     {"_equal_stub", 0x1100, 12, true},
                     {"_copy", Copy, 60, true},
                     {"_destroy", Destroy, 52, true}};
    const uint32_t Caller[] = {
        0xd10143ff, 0xa9034ff4, 0xa9047bfd, 0x910103fd, 0xaa0003f3, 0x910023e1,
        0x9400007a, 0x910023e0, 0xaa1303e1, 0x94000037, 0x12000014, 0x910023e0,
        0x94000084, 0xaa1403e0, 0xa9447bfd, 0xa9434ff4, 0x910143ff, 0xd65f03c0};
    for (size_t I = 0; I < std::size(Caller); ++I)
      word(I, Caller[I]);
    word(0x100 / 4 + 1, 0xf9440610);
    const uint32_t CopyWords[] = {
        0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xaa0103f3, 0xaa0003e1,
        0xb0000002, 0xf9440042, 0xf85f8048, 0xf9400908, 0xaa1303e0,
        0xd63f0100, 0xaa1303e0, 0xa9417bfd, 0xa8c24ff4, 0xd65f03c0};
    const uint32_t DestroyWords[] = {
        0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xaa0003f3, 0xb0000001,
        0xf9440021, 0xf85f8028, 0xf9400508, 0xd63f0100, 0xaa1303e0,
        0xa9417bfd, 0xa8c24ff4, 0xd65f03c0};
    for (size_t I = 0; I < std::size(CopyWords); ++I)
      word((Copy - Entry) / 4 + I, CopyWords[I]);
    for (size_t I = 0; I < std::size(DestroyWords); ++I)
      word((Destroy - Entry) / 4 + I, DestroyWords[I]);
    CopyBinding.TargetAddress = Copy;
    CopyBinding.Signature.Origin =
        SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    CopyBinding.Signature.ReturnType = NdType::makeInt(8);
    CopyBinding.Signature.Parameters = {
        {"source", NdType::makePtr(NdType::makeVoid())},
        {"destination", NdType::makePtr(NdType::makeVoid())}};
    DestroyBinding = CopyBinding;
    DestroyBinding.TargetAddress = Destroy;
    DestroyBinding.Signature.Parameters.resize(1);
    std::string Error;
    EXPECT_TRUE(assignDarwinScalarSourceABI(CopyBinding.Signature,
                                            Arch::AArch64, Error));
    EXPECT_TRUE(assignDarwinScalarSourceABI(DestroyBinding.Signature,
                                            Arch::AArch64, Error));
    EqualBinding.CallKind = SourceCallTypeHint::Kind::SwiftBooleanProjection;
    EqualBinding.TargetName =
        SwiftBooleanAnyHashableEqualityImport.drop_front().str();
    EqualBinding.TargetAddress = Equality;
    EqualBinding.Signature =
        *swiftBooleanNormalizedSignature(SwiftBooleanAnyHashableEqualityImport);
    EqualBinding.BooleanResult =
        SourceCallTypeHint::BooleanResultProjection{Entry, EqualSite};
    rerun();
  }
  void rerun() {
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Entry, Copy, Destroy, 0x1100};
    if (EntrySignature.ReturnType)
      Options.SourceTypeHints.emplace(Entry, EntrySignature);
    Options.SourceTypeHints.emplace(Copy, CopyBinding.Signature);
    Options.SourceTypeHints.emplace(Destroy, DestroyBinding.Signature);
    Result = Pipeline().run(Image, Context, Options);
  }
  const LowFunc *function(va_t Address) const {
    for (const auto &F : Result.LowFuncs)
      if (F.Entry == Address)
        return &F;
    return nullptr;
  }
  std::optional<SourceFrameEffects> effects(bool Copying) {
    return swiftOpaqueValueCallEffects(Image, *function(Entry),
                                       Copying ? CopySite : DestroySite,
                                       Copying ? CopyBinding : DestroyBinding,
                                       function(Copying ? Copy : Destroy));
  }
  std::optional<SourceFrameEffects> equality() {
    return swiftOpaqueValueCallEffects(Image, *function(Entry), EqualSite,
                                       EqualBinding);
  }
};
} // namespace

TEST(SwiftOpaqueValueEffects,
     AuthenticatedTypedLifetimeDoesNotInitializePadding) {
  Fixture F;
  ASSERT_TRUE(F.Result.Success) << F.Result.Error;
  auto C = F.effects(true), D = F.effects(false), E = F.equality();
  ASSERT_TRUE(C);
  ASSERT_TRUE(D);
  ASSERT_TRUE(E);
  using Action = SourceFrameValueEffect::Action;
  EXPECT_EQ(C->OpaqueValueParameters.at(0).TheAction, Action::Read);
  EXPECT_EQ(C->OpaqueValueParameters.at(1).TheAction, Action::Initialize);
  EXPECT_EQ(D->OpaqueValueParameters.at(0).TheAction, Action::Destroy);
  EXPECT_EQ(E->OpaqueValueParameters.at(0).TheAction, Action::Read);
  EXPECT_EQ(E->OpaqueValueParameters.at(1), C->OpaqueValueParameters.at(0));
  EXPECT_EQ(C->OpaqueValueParameters.at(1).Bytes, 40U);
  EXPECT_TRUE(C->InitializedFrameParameters.empty());
  EXPECT_FALSE(C->InitializesIndirectResult);
  EXPECT_EQ(C->ReturnFrameOrExternal, (SourceFrameReturnAlias{1, 40}));
  EXPECT_EQ(D->ReturnFrameOrExternal, (SourceFrameReturnAlias{0, 40}));
  NativeSourceCalls Calls;
  for (const auto &[Site, Binding, Effects] :
       {std::tuple{F.CopySite, &F.CopyBinding, *C},
        std::tuple{F.EqualSite, &F.EqualBinding, *E},
        std::tuple{F.DestroySite, &F.DestroyBinding, *D}}) {
    auto &Contract = Calls[Site];
    Contract.Signature = &Binding->Signature;
    static_cast<SourceFrameEffects &>(Contract) = Effects;
  }
  std::set<uint64_t> Observed;
  EXPECT_TRUE(restoresNativeSourceState(*F.function(Entry), Arch::AArch64,
                                        Calls, &Observed));
  EXPECT_TRUE(Observed.count(0));
  // The shared IR owner still owns all lifetime/path/padding obligations.
  for (const auto &Site : {F.CopySite, F.EqualSite, F.DestroySite}) {
    auto Bad = Calls;
    Bad[Site].OpaqueValueParameters.clear();
    EXPECT_FALSE(
        restoresNativeSourceState(*F.function(Entry), Arch::AArch64, Bad));
  }
}

TEST(SwiftOpaqueValueEffects,
     EveryMachineWordAndCurrentLowDefinitionMustMatch) {
  Fixture F;
  for (const auto &[Target, Count] :
       {std::pair{Copy, 15U}, std::pair{Destroy, 13U}}) {
    ASSERT_TRUE(F.effects(Target == Copy));
    for (unsigned I = 0; I < Count; ++I) {
      auto &Byte = F.Image.Segments[0].Data[Target - Entry + I * 4];
      Byte ^= 1;
      EXPECT_FALSE(F.effects(Target == Copy))
          << std::hex << Target << " word " << I;
      Byte ^= 1;
    }
    const auto *Good = F.function(Target);
    auto Bad = *Good;
    Bad.Blocks.front().Ops.front().Inputs[0] = NdVar::cst(0, 8);
    EXPECT_FALSE(swiftOpaqueValueCallEffects(
        F.Image, *F.function(Entry),
        Target == Copy ? F.CopySite : F.DestroySite,
        Target == Copy ? F.CopyBinding : F.DestroyBinding, &Bad));
    Bad = *Good;
    Bad.Blocks.front().Succs.push_back(Bad.Blocks.front().Id);
    EXPECT_FALSE(swiftOpaqueValueCallEffects(
        F.Image, *F.function(Entry),
        Target == Copy ? F.CopySite : F.DestroySite,
        Target == Copy ? F.CopyBinding : F.DestroyBinding, &Bad));
    Bad = *Good;
    --Bad.DecodedInstructionCount;
    EXPECT_FALSE(swiftOpaqueValueCallEffects(
        F.Image, *F.function(Entry),
        Target == Copy ? F.CopySite : F.DestroySite,
        Target == Copy ? F.CopyBinding : F.DestroyBinding, &Bad));
  }
  auto Caller = *F.function(Entry);
  Caller.Blocks.front().Ops.front().Inputs[0] = NdVar::cst(0, 8);
  EXPECT_FALSE(swiftOpaqueValueCallEffects(F.Image, Caller, F.CopySite,
                                           F.CopyBinding, F.function(Copy)));
  EXPECT_FALSE(swiftOpaqueValueCallEffects(F.Image, Caller, F.EqualSite,
                                           F.EqualBinding));
  auto Site = F.CopySite;
  ++Site.Sequence;
  EXPECT_FALSE(swiftOpaqueValueCallEffects(F.Image, *F.function(Entry), Site,
                                           F.CopyBinding, F.function(Copy)));
  EXPECT_FALSE(swiftOpaqueValueCallEffects(F.Image, *F.function(Entry),
                                           F.CopySite, F.CopyBinding));
  EXPECT_FALSE(swiftOpaqueValueCallEffects(F.Image, *F.function(Entry),
                                           F.CopySite, F.CopyBinding,
                                           F.function(Destroy)));
}

TEST(SwiftOpaqueValueEffects,
     StrongIdentityStorageAndCompleteABICannotBeSubstituted) {
  Fixture F;
  const std::vector<std::function<void(BinaryImage &, va_t)>> Mutations = {
      [](auto &I, auto S) { I.DyldBindSlots[S].WeakImport = true; },
      [](auto &I, auto S) { I.DyldBindSlots[S].Addend = 8; },
      [](auto &I, auto S) { I.DyldBindSlots[S].Module = "other"; },
      [](auto &I, auto S) { I.DyldBindSlots[S].Name = "_$sOtherN"; },
      [](auto &I, auto S) { I.ImportPtrSlots[S] = "_$sOtherN"; },
      [](auto &I, auto S) { I.DyldBindSlots.erase(S); },
      [](auto &I, auto S) { I.ConflictingImportStorageSlots.insert(S); },
      [](auto &I, auto) { I.DynInfo.NeededLibs.clear(); },
      [](auto &I, auto) { I.DynInfo.NeededLibs.push_back(Provider); },
      [](auto &I, auto) {
        I.Segments[1].Flags = I.Segments[1].Flags | SegmentFlags::Writable;
      },
      [](auto &I, auto) {
        I.Sections[0].Type = llvm::MachO::S_THREAD_LOCAL_VARIABLES;
      },
      [](auto &I, auto) { I.IsRelocatable = true; },
      [](auto &I, auto) { I.Arch = Arch::X64; },
      [](auto &I, auto) { I.Bits = Bitness::Bits32; },
      [](auto &I, auto) { I.Format = BinaryFormat::ELF; },
      [](auto &I, auto) { I.MachOChainedFixupsAmbiguous = true; },
  };
  for (va_t Slot : {Metadata, Equality})
    for (size_t Index = 0; Index < Mutations.size(); ++Index) {
      auto Image = F.Image;
      Mutations[Index](Image, Slot);
      EXPECT_FALSE(swiftOpaqueValueCallEffects(
          Image, *F.function(Entry),
          Slot == Metadata ? F.CopySite : F.EqualSite,
          Slot == Metadata ? F.CopyBinding : F.EqualBinding,
          Slot == Metadata ? F.function(Copy) : nullptr))
          << Index << ":" << Slot;
    }
  const std::vector<std::function<void(SourceCallTypeHint &)>> ABIs = {
      [](auto &B) { B.WeakImport = true; },
      [](auto &B) { B.DoesNotReturn = true; },
      [](auto &B) { B.Signature.HasExplicitABI = false; },
      [](auto &B) { B.Signature.Architecture = Arch::X64; },
      [](auto &B) { B.Signature.Parameters[0].Location.ValueBytes = 4; },
      [](auto &B) { B.Signature.Parameters[0].Type = NdType::makeInt(4); },
      [](auto &B) { B.Signature.Parameters[0].Location.RegisterOffset = 16; },
      [](auto &B) {
        B.Signature.Parameters[0].TheRole =
            SourceParameterTypeHint::Role::SwiftContext;
      },
      [](auto &B) {
        B.Signature.Parameters.push_back(B.Signature.Parameters[0]);
      },
      [](auto &B) { B.Signature.ReturnType = NdType::makeFloat(8); },
      [](auto &B) { B.Signature.ReturnLocation.ValueBytes = 4; },
      [](auto &B) { B.TargetAddress += 4; },
  };
  for (size_t Index = 0; Index < ABIs.size(); ++Index)
    for (bool Native : {true, false}) {
      auto Binding = Native ? F.CopyBinding : F.EqualBinding;
      ABIs[Index](Binding);
      EXPECT_FALSE(swiftOpaqueValueCallEffects(
          F.Image, *F.function(Entry), Native ? F.CopySite : F.EqualSite,
          Binding, Native ? F.function(Copy) : nullptr))
          << Index;
    }
  auto Binding = F.EqualBinding;
  Binding.BooleanResult.reset();
  EXPECT_FALSE(swiftOpaqueValueCallEffects(F.Image, *F.function(Entry),
                                           F.EqualSite, Binding));
  Binding = F.EqualBinding;
  Binding.BooleanResult->FunctionEntry += 4;
  EXPECT_FALSE(swiftOpaqueValueCallEffects(F.Image, *F.function(Entry),
                                           F.EqualSite, Binding));
}

TEST(SwiftOpaqueValueEffects,
     FreshReliftingAndNamesCannotReplaceBodyOrEntryProof) {
  Fixture F;
  ASSERT_TRUE(F.effects(true));
  ASSERT_TRUE(F.effects(false));
  // Neither a mangled helper name nor a particular parameter spelling supplies
  // its type, physical carrier, or value witness operation.
  for (auto &Symbol : F.Image.Symbols)
    Symbol.Name = "private_renamed_" + std::to_string(Symbol.Addr);
  F.CopyBinding.Signature.Parameters[0].Name = "renamed_input";
  F.CopyBinding.Signature.Parameters[1].Name = "renamed_destination";
  EXPECT_TRUE(F.effects(true));
  EXPECT_TRUE(F.effects(false));
  for (const auto &[Address, Word] :
       {std::pair{Copy + 44, 0xaa0103e0U}, std::pair{Copy + 32, 0xf9400d08U},
        std::pair{Destroy + 28, 0xf9400908U},
        std::pair{Destroy + 36, 0xaa0103e0U}}) {
    const auto Original = llvm::support::endian::read32le(
        F.Image.Segments[0].Data.data() + Address - Entry);
    F.word((Address - Entry) / 4, Word);
    F.rerun();
    EXPECT_FALSE(F.effects(Address < Destroy));
    F.word((Address - Entry) / 4, Original);
    F.rerun();
    ASSERT_TRUE(F.effects(Address < Destroy));
  }
  for (bool Interior : {false, true}) {
    auto Image = F.Image;
    if (Interior)
      Image.Symbols.push_back({"interior_entry", Copy + 4, 56, true});
    else
      std::erase_if(Image.Symbols,
                    [](const auto &S) { return S.Addr == Copy; });
    EXPECT_FALSE(swiftOpaqueValueCallEffects(Image, *F.function(Entry),
                                             F.CopySite, F.CopyBinding,
                                             F.function(Copy)));
  }
  // A decoder constant rewritten into a saved data-address annotation is not
  // enough for the strict machine replay, even if its numeric payload agrees.
  auto Callee = *F.function(Copy);
  for (auto &Block : Callee.Blocks)
    for (auto &Op : Block.Ops)
      if (Op.Addr == Copy + 20)
        Op.Inputs[0] = NdVar::dataAddress(Op.Inputs[0].Offset, 8, Metadata);
  EXPECT_FALSE(swiftOpaqueValueCallEffects(F.Image, *F.function(Entry),
                                           F.CopySite, F.CopyBinding, &Callee));
}

TEST(SwiftOpaqueValueConsumer, FreshCalleeProofAndTypedFrameInferEntry) {
  Fixture F;
  auto C = nativeSourceCalleeContracts(F.Image, F.Result);
  ASSERT_TRUE(validateSwiftOpaqueValueBindings(F.Image, F.low(), *F.med(), &C));
  unsigned Receipts = 0;
  for (const auto &B : F.med()->Blocks)
    for (const auto &O : B.Ops)
      if (O.SourceCallHint && O.SourceCallHint->SwiftOpaqueValue) {
        ++Receipts;
        EXPECT_TRUE(O.SourceCallHint->requiresUniqueSourceOccurrence());
      }
  ASSERT_EQ(Receipts, 3U);
  const PipelineFunctionAudit *A = nullptr;
  for (const auto &Audit : F.Result.FunctionAudits)
    if (Audit.Entry == Entry)
      A = &Audit;
  ASSERT_NE(A, nullptr);
  std::string Error;
  auto H = inferNativeSourceTypeHint(F.Image, *F.med(), *F.high(), *A, Error,
                                     F.low(), false, &C);
  ASSERT_TRUE(H) << Error;
  ASSERT_EQ(H->Parameters.size(), 1U);
  EXPECT_EQ(H->Parameters.front().Location.RegisterOffset, 0U);
  F.EntrySignature = *H;
  F.rerun();
  auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  EXPECT_TRUE(sdk::SourceSwiftOpaqueValueProjectionValidator(F.Image, F.Result)
                  .valid(Bound.Function));
}

TEST(SwiftOpaqueValueConsumer, MissingReceiptsCalleesAndAuditsCannotAuthorize) {
  for (unsigned Case = 0; Case < 9; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F;
    auto C = nativeSourceCalleeContracts(F.Image, F.Result);
    if (Case == 0)
      C.CurrentCallees.at(Copy).Audit = nullptr;
    if (Case == 1)
      C.CurrentCallees.at(Copy).Low = nullptr;
    if (Case == 2)
      C.CurrentCallees.at(Destroy).Signature = nullptr;
    if (Case == 3)
      C.SourceImage = nullptr;
    if (Case >= 4)
      for (auto &B : F.med()->Blocks)
        for (auto &O : B.Ops)
          if (O.SourceCallHint && O.SourceCallHint->SwiftOpaqueValue) {
            auto H = std::make_shared<SourceCallTypeHint>(*O.SourceCallHint);
            if (Case == 4)
              H->SwiftOpaqueValue.reset();
            if (Case == 5)
              ++H->SwiftOpaqueValue->Site.Sequence;
            if (Case == 6)
              H->Signature.Parameters.front().Location.ValueBytes = 4;
            if (Case == 7)
              O.Inputs[1].Size = 4;
            if (Case == 8)
              O.PreservesCallerSaved = true;
            O.SourceCallHint = std::move(H);
          }
    EXPECT_FALSE(
        validateSwiftOpaqueValueBindings(F.Image, F.low(), *F.med(), &C));
  }
}

namespace {
std::optional<SourceFunctionTypeHint>
inferFixture(Fixture &F, std::string &Error, bool Pair = false) {
  auto Contracts = nativeSourceCalleeContracts(F.Image, F.Result);
  for (const auto &A : F.Result.FunctionAudits)
    if (A.Entry == Entry)
      return inferNativeSourceTypeHint(F.Image, *F.med(), *F.high(), A, Error,
                                       F.low(), Pair, &Contracts);
  return std::nullopt;
}
void dictionaryLoop(Fixture &F) {
  // Complete original collision search; only three BL displacements differ.
  constexpr uint32_t Words[] = {
      0xd10203ff, 0xa90367fa, 0xa9045ff8, 0xa90557f6, 0xa9064ff4, 0xa9077bfd,
      0x9101c3fd, 0x91010297, 0x39408288, 0x92800009, 0x9ac82128, 0x8a280033,
      0xd346fe69, 0xf8697ae9, 0x9ad32529, 0x360002c9, 0xaa0003f5, 0xaa2803f8,
      0x52800519, 0xf9401a88, 0x9b192260, 0x910023e1, 0x9400006a, 0x910023e0,
      0xaa1503e1, 0x94000027, 0xaa0003f6, 0x910023e0, 0x94000074, 0x37000136,
      0x91000668, 0x8a180113, 0xd346fe68, 0xf8687ae8, 0x9ad32508, 0x3707fe08,
      0x14000002, 0x52800016, 0x120002c1, 0xaa1303e0, 0xa9477bfd, 0xa9464ff4,
      0xa94557f6, 0xa9445ff8, 0xa94367fa, 0x910203ff, 0xd65f03c0};
  for (size_t I = 0; I < std::size(Words); ++I)
    F.word(I, Words[I]);
  F.Image.Symbols[0].Size = sizeof(Words);
  F.rerun();
}
} // namespace

TEST(SwiftOpaqueValueConsumer, CollisionBackedgeAndEmptyExitReplayCompletely) {
  Fixture F;
  dictionaryLoop(F);
  const auto Contracts = nativeSourceCalleeContracts(F.Image, F.Result);
  EXPECT_TRUE(validateSwiftValueConstructorBindings(F.Image, F.low(), *F.med(),
                                                    &Contracts));
  EXPECT_TRUE(
      validateSwiftOpaqueValueBindings(F.Image, F.low(), *F.med(), &Contracts));
  EXPECT_TRUE(validateSwiftConsumedInputBindings(F.Image, F.low(), *F.med()));
  EXPECT_TRUE(validateSwiftWitnessFrameBindings(F.Image, F.low(), *F.med()));
  std::string Error;
  auto EntryHint = inferFixture(F, Error, true);
  ASSERT_TRUE(EntryHint) << Error;
  ASSERT_EQ(EntryHint->Parameters.size(), 3U);
  EXPECT_EQ(EntryHint->Parameters[2].Location.RegisterOffset, 0xa0U);
  ASSERT_EQ(EntryHint->ReturnComponents.size(), 2U);
  F.EntrySignature = *EntryHint;
  F.rerun();
  auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  ASSERT_TRUE(sdk::SourceSwiftOpaqueValueProjectionValidator(F.Image, F.Result)
                  .valid(Bound.Function));
  unsigned Branches = 0, Loops = 0;
  walkStmts(Bound.Function.Body, [&](const HighStmt &S) {
    Branches += S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse;
    Loops += S.Kind == StmtKind::While || S.Kind == StmtKind::DoWhile;
  });
  EXPECT_GT(Branches, 0U);
  EXPECT_GT(Loops, 0U);
}

TEST(SwiftOpaqueValueConsumer, ScalarResultCannotHideInvalidTypedLifetime) {
  for (unsigned Case = 0; Case < 5; ++Case) {
    Fixture F;
    if (Case == 1)
      F.word(12, 0xd28000e0); // Missing destroy, defined scalar.
    if (Case == 2)
      F.word(7, 0xf9400be0); // Raw padding read in a live value.
    if (Case == 3)
      F.word(13, 0xf94007e0); // Raw read after destruction.
    if (Case == 4)
      F.word(5, 0x9100a3e1); // Value overlaps saved state.
    F.rerun();
    std::string Error;
    EXPECT_EQ(bool(inferFixture(F, Error)), Case == 0) << Case << ": " << Error;
  }
}

TEST(SwiftOpaqueValueConsumer, StructuredPublicationRejectsEditedEvidence) {
  for (unsigned Case = 0; Case < 20; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F;
    dictionaryLoop(F);
    std::string Error;
    auto EntryHint = inferFixture(F, Error, true);
    ASSERT_TRUE(EntryHint) << Error;
    F.EntrySignature = *EntryHint;
    F.rerun();
    auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image);
    ASSERT_TRUE(Bound.Limitation.empty());
    std::vector<ExprPtr> Calls;
    walkStmts(Bound.Function.Body, [&](HighStmt &S) {
      forEachExpr(S, [&](ExprPtr &E) {
        if (E && E->SourceCallHint && E->SourceCallHint->SwiftOpaqueValue)
          Calls.push_back(E);
      });
    });
    ASSERT_EQ(Calls.size(), 3U);
    if (Case == 0)
      Calls[0]->Operands[0] = HighExpr::makeConst(0, 8);
    if (Case == 1)
      std::swap(Calls[0]->Operands[0], Calls[0]->Operands[1]);
    if (Case == 2)
      Calls[1]->Operands[1] = HighExpr::makeConst(0, 8);
    if (Case == 3)
      Calls[2]->Operands[0] = HighExpr::makeConst(0, 8);
    if (Case == 4) {
      HighStmt S;
      S.Kind = StmtKind::Call;
      S.CallExpr = Calls[0];
      Bound.Function.Body.push_back(S);
    }
    if (Case == 5)
      --Bound.Function.FrameSize;
    if (Case == 6)
      ++Bound.Function.FrameHeadroom;
    if (Case == 7)
      walkStmts(Bound.Function.Body, [&](HighStmt &S) {
        if (S.Cond)
          S.Cond = HighExpr::makeConst(1, 1);
      });
    if (Case == 8)
      walkStmts(Bound.Function.Body, [&](HighStmt &S) {
        if (S.LoopHeaderAddr)
          S.LoopHeaderAddr += 4;
      });
    if (Case == 9) {
      auto H = std::make_shared<SourceCallTypeHint>(*Calls[0]->SourceCallHint);
      H->SwiftOpaqueValue.reset();
      Calls[0]->SourceCallHint = H;
    }
    if (Case == 10) {
      for (auto &B : F.med()->Blocks)
        for (auto &O : B.Ops)
          if (O.SourceCallHint && O.SourceCallHint->SwiftOpaqueValue) {
            auto H = std::make_shared<SourceCallTypeHint>(*O.SourceCallHint);
            H->SwiftOpaqueValue.reset();
            O.SourceCallHint = H;
          }
      for (auto &E : Calls) {
        auto H = std::make_shared<SourceCallTypeHint>(*E->SourceCallHint);
        H->SwiftOpaqueValue.reset();
        E->SourceCallHint = H;
      }
    }
    if (Case == 11) {
      auto H = std::make_shared<SourceCallTypeHint>(*Calls[0]->SourceCallHint);
      H->ByteCount = 40;
      Calls[0]->SourceCallHint = H;
    }
    if (Case == 12)
      F.word((Copy - Entry) / 4 + 8, 0xf9400508); // Wrong dynamic witness.
    if (Case == 13)
      F.Image.DyldBindSlots[Metadata].WeakImport = true;
    if (Case == 14)
      F.Image.Sections[0].Type = llvm::MachO::S_THREAD_LOCAL_VARIABLES;
    if (Case == 15)
      for (auto &A : F.Result.FunctionAudits)
        if (A.Entry == Copy)
          A.MedIRVerified = false;
    if (Case == 16)
      for (auto &B : F.med()->Blocks)
        for (auto &O : B.Ops)
          if (O.SourceCallHint && O.SourceCallHint->SwiftOpaqueValue)
            O.Inputs[1] = MedVar::makeConst(0, 8);
    if (Case == 17)
      Calls[0]->CallAddr += 4;
    if (Case == 18) {
      ASSERT_FALSE(Bound.Function.Params.empty());
      Bound.Function.Params[0].Type = NdType::makeInt(4);
    }
    if (Case == 19) {
      auto H = std::make_shared<SourceCallTypeHint>(*Calls[2]->SourceCallHint);
      H->ReturnedArgument = 0;
      Calls[2]->SourceCallHint = H;
    }
    EXPECT_FALSE(
        sdk::SourceSwiftOpaqueValueProjectionValidator(F.Image, F.Result)
            .valid(Bound.Function));
  }
}

namespace {
HighStmt opaqueScanLeaf(ExprPtr Value, bool Call = false) {
  HighStmt Statement;
  Statement.Kind = Call ? StmtKind::Call : StmtKind::Return;
  if (Call)
    Statement.CallExpr = std::move(Value);
  else
    Statement.RetVal = std::move(Value);
  return Statement;
}
} // namespace

TEST(SwiftOpaqueValueConsumer, ExceptionOnlyBodiesHaveNoOpaqueProofObligation) {
  BinaryImage Image;
  PipelineResult Result;
  sdk::SourceSwiftOpaqueValueProjectionValidator Validator(Image, Result);
  sdk::SourceSwiftValueConstructorProjectionValidator Constructors(Image,
                                                                   Result);
  for (const auto Kind :
       {StmtKind::SEHTry, StmtKind::CxxTry, StmtKind::ItaniumTry}) {
    for (unsigned Shape = 0; Shape < 3; ++Shape) {
      SCOPED_TRACE(static_cast<unsigned>(Kind));
      SCOPED_TRACE(Shape);
      HighFunc Function;
      Function.Entry = Entry;
      HighStmt Region;
      Region.Kind = Kind;
      Region.Body.push_back(opaqueScanLeaf(HighExpr::makeConst(1, 4)));
      if (Shape != 1)
        Region.EHClauses.emplace_back();
      if (Shape != 0)
        Region.EHClauseBodies.push_back(
            {opaqueScanLeaf(HighExpr::makeConst(2, 4))});
      Function.Body.push_back(std::move(Region));
      EXPECT_TRUE(Validator.valid(Function));
      EXPECT_TRUE(Constructors.valid(Function));
      SourceFunctionTypeHint Signature;
      Signature.ReturnType = NdType::makeInt(4);
      std::string Error;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Signature, Arch::AArch64, Error));
      const auto Diagnostics =
          sdk::sourceBodyDiagnostics(Function, Signature, nullptr);
      EXPECT_TRUE(std::any_of(Diagnostics.Items.begin(),
                              Diagnostics.Items.end(), [](const auto &Item) {
                                return Item.Issue ==
                                       sdk::SourceProjectionIssue::Exception;
                              }));
    }
  }
}

TEST(SwiftOpaqueValueConsumer, OpaqueMarkersInExceptionArmsRemainUnsupported) {
  BinaryImage Image;
  PipelineResult Result;
  sdk::SourceSwiftOpaqueValueProjectionValidator Validator(Image, Result);
  for (unsigned Shape = 0; Shape < 8; ++Shape) {
    SCOPED_TRACE(Shape);
    HighFunc Function;
    Function.Entry = Entry;
    auto Call = HighExpr::makeCall("opaque", Copy, {});
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->SwiftOpaqueValue = {Entry, {0x1010, 1, NdOp::CALL, Copy}};
    Call->SourceCallHint = Hint;
    HighStmt Nested;
    Nested.Kind = StmtKind::Block;
    switch (Shape) {
    case 0:
      Nested.Body.push_back(opaqueScanLeaf(Call, true));
      break;
    case 1:
      Nested.ElseBody.push_back(opaqueScanLeaf(Call, true));
      break;
    case 2:
      Nested.DefaultBody.push_back(opaqueScanLeaf(Call, true));
      break;
    case 3:
      Nested.Cases.emplace_back();
      Nested.Cases.back().Body.push_back(opaqueScanLeaf(Call, true));
      break;
    case 4:
      Nested.EHClauseBodies.push_back({opaqueScanLeaf(Call, true)});
      break;
    case 5:
      Nested.Cond = Call;
      break;
    case 6:
      Nested.CallExpr = HighExpr::makeCall("indirect", 0, {});
      Nested.CallExpr->IsIndirectCall = true;
      Nested.CallExpr->IndirectTarget = Call;
      break;
    case 7:
      Call->Kind = ExprKind::Undef;
      Nested.Val = Call;
      break;
    }
    HighStmt Region;
    Region.Kind = StmtKind::ItaniumTry;
    Region.EHClauseBodies.push_back({std::move(Nested)});
    Function.Body.push_back(std::move(Region));
    EXPECT_FALSE(Validator.valid(Function));
    Hint->SwiftOpaqueValue.reset();
    Hint->SwiftValueConstructor = {Entry, {0x1010, 1, NdOp::CALL, Copy}};
    EXPECT_FALSE(
        sdk::SourceSwiftValueConstructorProjectionValidator(Image, Result)
            .valid(Function));
  }
}

TEST(SwiftOpaqueValueConsumer, UnrelatedExceptionArmsDoNotExtendOpaqueReplay) {
  Fixture F;
  std::string Error;
  auto EntryHint = inferFixture(F, Error);
  ASSERT_TRUE(EntryHint) << Error;
  F.EntrySignature = *EntryHint;
  F.rerun();
  auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image);
  ASSERT_TRUE(Bound.Limitation.empty());
  sdk::SourceSwiftOpaqueValueProjectionValidator Validator(F.Image, F.Result);
  ASSERT_TRUE(Validator.valid(Bound.Function));
  HighStmt Region;
  Region.Kind = StmtKind::ItaniumTry;
  Region.EHClauseBodies.push_back({});
  Bound.Function.Body.push_back(std::move(Region));
  EXPECT_FALSE(Validator.valid(Bound.Function));
}

TEST(SwiftOpaqueValueConsumer, EmptyOccurrenceScansStillFailOnExhaustedBounds) {
  BinaryImage Image;
  PipelineResult Result;
  sdk::SourceSwiftOpaqueValueProjectionValidator Validator(Image, Result);
  HighFunc Function;
  Function.Entry = Entry;
  HighStmt Region;
  Region.Kind = StmtKind::ItaniumTry;
  for (unsigned I = 0; I < 70; ++I) {
    HighStmt Parent;
    Parent.Kind = StmtKind::ItaniumTry;
    Parent.EHClauseBodies.push_back({std::move(Region)});
    Region = std::move(Parent);
  }
  Function.Body.push_back(std::move(Region));
  EXPECT_FALSE(Validator.valid(Function));
  EXPECT_FALSE(
      sdk::SourceSwiftValueConstructorProjectionValidator(Image, Result)
          .valid(Function));
  Function.Body.clear();
  auto Cycle = HighExpr::makeConst(0, 8);
  Cycle->Operands.push_back(Cycle);
  Region = HighStmt{};
  Region.EHClauseBodies.push_back({opaqueScanLeaf(Cycle)});
  Function.Body.push_back(std::move(Region));
  EXPECT_FALSE(Validator.valid(Function));
  EXPECT_FALSE(
      sdk::SourceSwiftValueConstructorProjectionValidator(Image, Result)
          .valid(Function));
  Cycle->Operands.clear();
}
