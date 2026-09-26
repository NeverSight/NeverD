#include "../../../lib/sdk/capi/ObjCImmutableStringCallbackSources.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

using namespace neverd;
using namespace neverd::sdk;
namespace {
struct ImmutableCallbackFixture {
  static constexpr va_t Root = 0x1000, Shared = 0x1100, Provider = 0x1200;
  static constexpr va_t Retain = 0x1300, Slot = 0x3080, Pair = 0x3100;
  static constexpr va_t Destination = 0x2000, Pool = 0x4000;
  BinaryImage Image;
  llvm::LLVMContext Context;
  PipelineResult Result;
  SwiftOnceSourcePlan Once;

  void word(va_t Address, uint32_t Value) {
    uint8_t Bytes[4];
    llvm::support::endian::write32le(Bytes, Value);
    ASSERT_TRUE(Image.writeVA(Address, Bytes, 4));
  }
  void pointer(va_t Address, uint64_t Value) {
    uint8_t Bytes[8];
    llvm::support::endian::write64le(Bytes, Value);
    ASSERT_TRUE(Image.writeVA(Address, Bytes, 8));
  }
  static uint32_t branch(va_t Site, va_t Target) {
    return 0x14000000 |
           (uint32_t((int64_t(Target) - int64_t(Site)) / 4) & 0x03ffffff);
  }
  ImmutableCallbackFixture() {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Entry = Root;
    Image.MachOHasChainedFixups = true;
    Image.DynInfo.NeededLibs = {"/usr/lib/swift/libswiftCore.dylib"};
    for (unsigned I = 0; I < 4; ++I) {
      Segment S;
      S.Name = I == 0 ? "__TEXT" : I == 1 ? "__DATA" : "__DATA_CONST";
      S.VA = (I + 1) * 0x1000;
      S.FileOff = I * 0x1000;
      S.Size = S.FileSz = 0x1000;
      S.Flags = SegmentFlags::Readable;
      if (I == 0)
        S.Flags = S.Flags | SegmentFlags::Executable;
      if (I == 1)
        S.Flags = S.Flags | SegmentFlags::Writable;
      S.Data.resize(S.Size);
      Image.Segments.push_back(S);
      Section Sec;
      Sec.Name = I == 0 ? "__text" : I == 3 ? "__cstring" : "__data";
      Sec.SegmentName = S.Name;
      Sec.VA = S.VA;
      Sec.Size = Sec.FileSz = S.Size;
      Sec.FileOff = S.FileOff;
      Sec.Flags = S.Flags;
      Sec.Type = I == 0   ? uint32_t(llvm::MachO::S_ATTR_PURE_INSTRUCTIONS)
                 : I == 3 ? uint32_t(llvm::MachO::S_CSTRING_LITERALS)
                          : 0;
      Image.Sections.push_back(Sec);
    }
    const uint32_t Caller[] = {0xb0000002, 0x91000042,
                               0x90000001, 0x91080021,
                               0x91002043, branch(Root + 20, Shared)};
    const uint32_t Body[] = {
        0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xaa0303f3,
        0xaa0203f4, 0xd63f0020, 0xa9400008, 0xf9000288,
        0xf9000260, 0xa9417bfd, 0xa8c24ff4, branch(Shared + 44, Retain)};
    for (unsigned I = 0; I < std::size(Caller); ++I)
      word(Root + I * 4, Caller[I]);
    for (unsigned I = 0; I < std::size(Body); ++I)
      word(Shared + I * 4, Body[I]);
    word(Provider, 0xd0000000);
    word(Provider + 4, 0x91040000);
    word(Provider + 8, 0xd65f03c0);
    word(Retain, 0xd0000010);
    word(Retain + 4, 0xf9404210);
    word(Retain + 8, 0xd61f0200);
    pointer(Pair, UINT64_C(0xd000000000000024));
    pointer(Pair + 8, (Pool + 0x40 - SwiftLiteralString::StorageBias) |
                          SwiftLiteralString::ImmortalTag);
    const std::string Text = "https://example.invalid/privacy.html";
    EXPECT_EQ(Text.size(), 36U);
    EXPECT_TRUE(Image.writeVA(Pool + 0x40,
                              reinterpret_cast<const uint8_t *>(Text.c_str()),
                              Text.size() + 1));
    Image.MachOResolvedChainedPointerSlots.insert(Pair + 8);
    Image.ImportPtrSlots[Slot] = "_swift_bridgeObjectRetain";
    EXPECT_TRUE(Image.recordDyldBindSlot(Slot, "_swift_bridgeObjectRetain", 0,
                                         Image.DynInfo.NeededLibs[0], false));
    Image.Symbols.push_back({"_callback_storage", Destination, 16, false});
    for (const auto Entry : {Root, Shared, Provider, Retain}) {
      const auto Name = "callback_" + std::to_string(Entry);
      Image.Symbols.push_back({Name, Entry, 0, true});
      Image.Exports.push_back({Name, 0, Entry});
    }
    Once.CallbackHints.emplace(
        Root, swift_once_source_detail::callbackHint(Image.Arch));
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Root, Shared, Provider};
    Options.SourceTypeHints = Once.CallbackHints;
    Result = Pipeline().run(Image, Context, Options);
    EXPECT_TRUE(Result.Success) << Result.Error;
  }
  const HighFunc &high() const {
    const auto *F =
        objc_super_getter_detail::uniqueEntry(Result.HighFuncs, Root);
    if (!F)
      throw std::runtime_error("callback fixture missing");
    return *F;
  }
  auto proof() const {
    return objc_immutable_string_callback_detail::prove(Image, Result, Once,
                                                        Root);
  }
};

struct AddressorCallbackFixture : ImmutableCallbackFixture {
  static constexpr va_t Initializer = 0x1400, Predicate = 0x2010;
  static constexpr va_t Storage = 0x2020, OnceSlot = 0x2080;
  static constexpr const char *AccessorName = "_$s4Test5valueSSvau";
  static constexpr const char *InitializerName = "_$s4Test5value_WZ";
  static constexpr const char *PredicateName = "_$s4Test5value_Wz";
  static constexpr const char *StorageName = "_$s4Test5valueSSvpZ";

  AddressorCallbackFixture() {
    word(Provider + 8, 0xd503201f);
    Image.Symbols.push_back({AccessorName, Provider, 0, true});
    Image.Symbols.push_back({InitializerName, Initializer, 0, true});
    Image.Symbols.push_back({PredicateName, Predicate, 8, false});
    Image.Symbols.push_back({StorageName, Storage, 16, false});
    Image.ImportPtrSlots[OnceSlot] = "_swift_once";
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    const auto Integer = NdType::makeInt(8);
    auto Param = [&](unsigned Id) {
      MedVar V;
      V.Kind = MedVar::Param;
      V.Id = Id;
      V.Size = 8;
      return HighExpr::makeVar(V, Pointer);
    };
    HighFunc Accessor;
    Accessor.Entry = Provider;
    Accessor.Name = AccessorName;
    Accessor.ReturnType = Pointer;
    for (unsigned I = 0; I < 3; ++I)
      Accessor.Params.push_back({"arg" + std::to_string(I), Pointer});
    MedVar LoadedVar;
    LoadedVar.Kind = MedVar::Temp;
    LoadedVar.Id = 1;
    LoadedVar.Size = 8;
    HighStmt Load;
    Load.Kind = StmtKind::Assign;
    Load.Dst = HighExpr::makeVar(LoadedVar, Integer);
    Load.Val = HighExpr::makeLoad(
        HighExpr::makeConst(Predicate, 8,
                            ConstantAddressProvenance::DataAddress),
        Integer);
    const auto Runtime = swiftRuntimeSourceCallHint(Image, OnceSlot);
    EXPECT_TRUE(Runtime);
    if (!Runtime)
      return;
    HighStmt Invoke;
    Invoke.Kind = StmtKind::Call;
    Invoke.CallExpr = HighExpr::makeCall(
        "swift_once", OnceSlot,
        {HighExpr::makeConst(Predicate, 8,
                             ConstantAddressProvenance::DataAddress),
         HighExpr::makeConst(Initializer, 8,
                             ConstantAddressProvenance::CodeAddress),
         Param(2)});
    Invoke.CallExpr->Type = NdType::makeVoid();
    Invoke.CallExpr->SourceCallHint =
        std::make_shared<SourceCallTypeHint>(*Runtime);
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = HighExpr::makeConst(
        Storage, 8, ConstantAddressProvenance::DataAddress);
    HighStmt Initialize;
    Initialize.Kind = StmtKind::If;
    Initialize.Cond = HighExpr::makeBinop(
        NdOp::INT_NOTEQUAL,
        HighExpr::makeBinop(NdOp::INT_ADD,
                            HighExpr::makeVar(LoadedVar, Integer),
                            HighExpr::makeConst(1, 8)),
        HighExpr::makeConst(0, 8));
    Initialize.Body = {Invoke, Return};
    Accessor.Body = {Load, Initialize, Return};
    HighFunc Callback;
    Callback.Entry = Initializer;
    Callback.Name = InitializerName;
    Callback.ReturnType = NdType::makeVoid();
    Callback.SourceTypeHint =
        swift_once_source_detail::callbackHint(Image.Arch);
    Callback.Params = {{"once_context", Pointer}};
    HighStmt CallbackReturn;
    CallbackReturn.Kind = StmtKind::Return;
    Callback.Body = {CallbackReturn};
    for (auto &F : Result.HighFuncs)
      if (F.Entry == Provider)
        F = Accessor;
    Result.HighFuncs.push_back(Callback);
    Once.CallbackHints.emplace(Initializer,
                               swift_once_source_detail::callbackHint(
                                   Image.Arch));
    const auto Contract =
        swift_once_source_detail::addressorContract(Accessor, Image);
    EXPECT_TRUE(Contract);
    if (Contract)
      Once.Addressors.emplace(Provider, *Contract);
  }

  auto addressorProof() const {
    return objc_immutable_string_callback_detail::proveAddressor(
        Image, Result, Once, Root);
  }
};

TEST(ObjCImmutableStringCallbackSources,
     AddressorCopiesPairOnceAndRetainsSecondWord) {
  AddressorCallbackFixture F;
  ASSERT_TRUE(F.addressorProof());
  auto P =
      projectObjCImmutableStringCallback(F.high(), F.Image, F.Result, F.Once);
  ASSERT_TRUE(P);
  EXPECT_EQ(P->Dependencies, std::set<va_t>{F.Initializer});
  EXPECT_EQ(P->SwiftOnceAccessors, std::set<va_t>{F.Provider});
  EXPECT_EQ(P->LocalStorageExtents,
            (std::map<va_t, uint64_t>{{F.Destination, 16}, {F.Predicate, 8},
                                      {F.Storage, 16}}));
  ASSERT_EQ(P->Function.Body.size(), 5U);
  EXPECT_EQ(P->Function.Body[0].Kind, StmtKind::Assign);
  EXPECT_EQ(P->Function.Body[0].Val->CallAddr, F.Provider);
  EXPECT_EQ(P->Function.Body[0].Val->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeSwiftOnceAccessor);
  EXPECT_EQ(P->Function.Body[1].Kind, StmtKind::Store);
  EXPECT_EQ(P->Function.Body[2].Kind, StmtKind::Store);
  EXPECT_EQ(P->Function.Body[3].Kind, StmtKind::Call);
  EXPECT_TRUE(
      objCImmutableStringCallbackValid(P->Function, F.Image, F.Result, F.Once));
  auto Bound = bindObjCSourceReferences(P->Function, F.Image);
  EXPECT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  EXPECT_TRUE(objCImmutableStringCallbackValid(Bound.Function, F.Image,
                                               F.Result, F.Once));
  std::map<va_t, const HighFunc *> Functions;
  for (const auto &H : F.Result.HighFuncs)
    Functions.emplace(H.Entry, &H);
  const auto *Audit =
      objc_super_getter_detail::uniqueEntry(F.Result.FunctionAudits, F.Root);
  const auto Limitation = sourceBodyLimitation(
      Bound.Function, *Bound.Function.SourceTypeHint, Audit,
      [&](const HighExpr &E) {
        return objcSourceCallBound(E, F.Image, Functions) ||
               swiftOnceAddressorBound(E, F.Image, F.Once, Functions);
      });
  EXPECT_TRUE(Limitation.empty()) << Limitation;
}

TEST(ObjCImmutableStringCallbackSources,
     AddressorRejectsChangedContractAndProjectedBody) {
  for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    AddressorCallbackFixture F;
    auto P = projectObjCImmutableStringCallback(F.high(), F.Image, F.Result,
                                                F.Once);
    ASSERT_TRUE(P);
    switch (Mutation) {
    case 0:
      F.Once.Addressors.at(F.Provider).Initializer = F.Root;
      break;
    case 1:
      F.Once.CallbackHints.erase(F.Initializer);
      break;
    case 2:
      F.Image.Symbols.back().Size = 8;
      break;
    case 3:
      F.word(F.Shared + 20, 0xd63f0040);
      break;
    case 4:
      P->Function.Body[0].Val->CallAddr = F.Root;
      break;
    case 5:
      P->Function.Body[1].StoreVal->Operands[0]->Var.Id++;
      break;
    case 6:
      P->Function.Body[3]
          .CallExpr->Operands[0]->Operands[0]->Operands[0]->Operands[1]
          ->ConstVal = 16;
      break;
    case 7: {
      MedVar Context;
      Context.Kind = MedVar::Param;
      Context.Size = 8;
      F.Result.HighFuncs.back().Body[0].RetVal =
          HighExpr::makeVar(Context, NdType::makePtr(NdType::makeVoid()));
      break;
    }
    }
    EXPECT_FALSE(objCImmutableStringCallbackValid(P->Function, F.Image,
                                                  F.Result, F.Once));
  }
}

TEST(ObjCImmutableStringCallbackSources,
     PreservesStoresRetainAndSharedIdentity) {
  ImmutableCallbackFixture F;
  ASSERT_NE(objc_super_getter_detail::completeLow(F.Result, F.Shared, 12),
            nullptr);
  EXPECT_EQ(F.Result.LowFuncs.size(), 3u);
  ASSERT_TRUE(F.proof());
  auto P =
      projectObjCImmutableStringCallback(F.high(), F.Image, F.Result, F.Once);
  ASSERT_TRUE(P);
  EXPECT_TRUE(P->Dependencies.empty());
  EXPECT_EQ(P->LocalStorageExtents,
            (std::map<va_t, uint64_t>{{F.Destination, 16}}));
  EXPECT_EQ(P->CStringSections, std::set<va_t>{F.Pool});
  EXPECT_EQ(P->Function.Body[0].Kind, StmtKind::Store);
  EXPECT_EQ(P->Function.Body[1].Kind, StmtKind::Store);
  EXPECT_EQ(P->Function.Body[2].Kind, StmtKind::Call);
  EXPECT_EQ(P->Function.Body[2].CallExpr->Operands[0]->CallAddr, F.Retain);
  EXPECT_TRUE(
      objCImmutableStringCallbackValid(P->Function, F.Image, F.Result, F.Once));
  auto Bound = bindObjCSourceReferences(P->Function, F.Image);
  EXPECT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  EXPECT_TRUE(objCImmutableStringCallbackValid(Bound.Function, F.Image,
                                               F.Result, F.Once));
  EXPECT_TRUE(swift_once_source_detail::ignoresContext(Bound.Function));
  const auto *Audit =
      objc_super_getter_detail::uniqueEntry(F.Result.FunctionAudits, F.Root);
  EXPECT_TRUE(sourceBodyLimitation(P->Function, *P->Function.SourceTypeHint,
                                   Audit,
                                   [&](const HighExpr &E) {
                                     return objcSourceCallBound(E, F.Image, {});
                                   })
                  .empty());
}

TEST(ObjCImmutableStringCallbackSources, PlainUnwindIsNotLanguageDispatch) {
  ImmutableCallbackFixture F;
  for (auto &H : F.Result.HighFuncs) {
    ExceptionFunction Plain;
    Plain.Encoding = ExceptionEncoding::DwarfFDE;
    Plain.Dwarf.emplace();
    H.ExceptionMetadata = Plain;
  }
  ASSERT_TRUE(F.proof());
  auto P =
      projectObjCImmutableStringCallback(F.high(), F.Image, F.Result, F.Once);
  ASSERT_TRUE(P);
  EXPECT_TRUE(
      objCImmutableStringCallbackValid(P->Function, F.Image, F.Result, F.Once));
  for (auto &H : F.Result.HighFuncs) {
    H.ExceptionMetadata->PersonalityVA = 1;
    EXPECT_FALSE(F.proof());
    EXPECT_FALSE(objCImmutableStringCallbackValid(P->Function, F.Image,
                                                  F.Result, F.Once));
    H.ExceptionMetadata->PersonalityVA = 0;
  }
}

TEST(ObjCImmutableStringCallbackSources,
     RejectsChangedMachinePathStorageAndProvider) {
  for (unsigned Mutation = 0; Mutation < 19; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ImmutableCallbackFixture F;
    ASSERT_TRUE(F.proof());
    switch (Mutation) {
    case 0:
      F.word(F.Root + 16, 0x91004043);
      break;
    case 1:
      F.word(F.Shared + 20, 0xd63f0040);
      break;
    case 2:
      F.word(F.Shared + 28, 0xf9000268);
      break;
    case 3:
      F.word(F.Shared + 32, 0xf9000280);
      break;
    case 4:
      F.word(F.Provider + 8, 0xd65f0000);
      break;
    case 5:
      F.word(F.Provider, 0xd0000001);
      break;
    case 6:
      F.Image.MachOResolvedChainedPointerSlots.clear();
      break;
    case 7:
      F.Image.MachOChainedFixupsAmbiguous = true;
      break;
    case 8:
      F.pointer(F.Pair + 8, F.Pool + 0x20);
      break;
    case 9:
      F.pointer(F.Pair, UINT64_C(0xd000000000000025));
      break;
    case 10:
      F.Image.DyldBindSlots[F.Slot].WeakImport = true;
      break;
    case 11:
      F.Image.DyldBindSlots[F.Slot].Module = "foreign";
      break;
    case 12:
      F.Image.DynInfo.NeededLibs.clear();
      break;
    case 13:
      F.Image.Segments[2].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      break;
    case 14:
      F.Once.CallbackHints.clear();
      break;
    case 15:
      F.Result.FunctionAudits.front().MedIRVerified = false;
      break;
    case 16:
      F.Result.SourceImage = nullptr;
      break;
    case 17:
      F.Image.Symbols.front().Size = 8;
      break;
    case 18:
      F.word(F.Retain + 8, 0xd61f0220);
      break;
    }
    EXPECT_FALSE(F.proof());
  }
}

TEST(ObjCImmutableStringCallbackSources, RevalidatesFinalBodyAndCurrentImage) {
  for (unsigned Mutation = 0; Mutation < 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ImmutableCallbackFixture F;
    auto P =
        projectObjCImmutableStringCallback(F.high(), F.Image, F.Result, F.Once);
    ASSERT_TRUE(P);
    auto &B = P->Function.Body;
    switch (Mutation) {
    case 0:
      std::swap(B[0], B[1]);
      break;
    case 1:
      B.erase(B.begin() + 2);
      break;
    case 2:
      B[0].StoreVal->ConstVal ^= 1;
      break;
    case 3:
      B[1].StoreAddr->Operands[1]->ConstVal = 16;
      break;
    case 4:
      B[2].CallExpr->Operands[0] =
          objc_immutable_string_callback_detail::scalar(0);
      break;
    case 5:
      B[1].MemoryOrdering = NdMemoryOrdering::Release;
      break;
    case 6:
      B[0].Body.push_back(B[2]);
      break;
    case 7:
      F.word(F.Provider + 8, 0xd503201f);
      break;
    case 8:
      F.Image.DyldBindSlots[F.Slot].WeakImport = true;
      break;
    case 9:
      F.pointer(F.Pair, UINT64_C(0xd000000000000025));
      break;
    case 10:
      P->Function.ReturnType = NdType::makeInt(8);
      break;
    }
    EXPECT_FALSE(objCImmutableStringCallbackValid(P->Function, F.Image,
                                                  F.Result, F.Once));
  }
}
void execute(const std::string &Source) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Found = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(Found);
  const std::string Compiler = *Found;
#endif
  llvm::SmallString<128> C, Exe, Err;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-immutable-callback", "c", C));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-immutable-callback",
                                                  "exe", Exe));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-immutable-callback",
                                                  "err", Err));
  llvm::FileRemover RemoveC(C), RemoveExe(Exe), RemoveErr(Err);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(C, EC);
    ASSERT_FALSE(EC);
    OS << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt,
                                                      std::nullopt, Err.str()};
  for (const llvm::StringRef Opt : {"-O0", "-O2"}) {
    const llvm::SmallVector<llvm::StringRef> Args = {
        Compiler, "-x", "c", "-std=gnu11", "-Werror", Opt, C, "-o", Exe};
    std::string Error;
    const int Status = llvm::sys::ExecuteAndWait(Compiler, Args, std::nullopt,
                                                 Redirects, 30, 0, &Error);
    const auto Errors = llvm::MemoryBuffer::getFile(Err);
    ASSERT_EQ(Status, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "")
                         << Source;
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Exe, {Exe}, std::nullopt, Redirects, 30,
                                        0, &Error),
              0)
        << Error << Source;
  }
}

TEST(ObjCImmutableStringCallbackSources,
     ExecutedSourceKeepsBothStoresBeforeRetain) {
  ImmutableCallbackFixture F;
  auto P =
      projectObjCImmutableStringCallback(F.high(), F.Image, F.Result, F.Once);
  ASSERT_TRUE(P);
  P->Function.Name = "initialize_string";
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::MachO;
  Options.EmitComments = false;
  std::string Source = "#include <string.h>\n";
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({P->Function}, OS, Options));
  std::set<std::string> Helpers;
  Source +=
      renderObjCLocalStorageHelpers(F.Image, P->LocalStorageExtents, Helpers);
  Source += renderCStringStorageHelpers(F.Image, P->CStringSections, Helpers);
  Source += R"C(
static unsigned calls;
static int mismatch;
void *swift_bridgeObjectRetain(void *value) {
  uint64_t words[2];
  memcpy(words, (void *)neverd_local_storage_2000_address(), sizeof(words));
  uintptr_t expected = ((neverd_cstring_storage_4000_address() + 64) - 32) | UINT64_C(0x8000000000000000);
  mismatch |= words[0] != UINT64_C(0xd000000000000024) || words[1] != expected || (uintptr_t)value != expected;
  mismatch |= strcmp((char *)((words[1] & ~UINT64_C(0x8000000000000000)) + 32), "https://example.invalid/privacy.html") != 0;
  ++calls;
  return value;
}
int main(void) {
  initialize_string((void *)0x1234);
  initialize_string((void *)0x5678);
  return mismatch || calls != 2;
}
)C";
  execute(Source);
}

TEST(ObjCImmutableStringCallbackSources,
     ExecutedAddressorCallbackInitializesOnceAndRetainsCopiedWord) {
  AddressorCallbackFixture F;
  auto P =
      projectObjCImmutableStringCallback(F.high(), F.Image, F.Result, F.Once);
  ASSERT_TRUE(P);
  P->Function.Name = "initialize_string";
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::MachO;
  Options.EmitComments = false;
  std::string Source = "#include <string.h>\n";
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({P->Function}, OS, Options));
  std::set<std::string> Helpers;
  Source +=
      renderObjCLocalStorageHelpers(F.Image, P->LocalStorageExtents, Helpers);
  std::map<va_t, const HighFunc *> Functions;
  for (const auto &H : F.Result.HighFuncs)
    Functions.emplace(H.Entry, &H);
  Source += renderSwiftOnceAddressorHelpers(F.Image, P->SwiftOnceAccessors,
                                            F.Once, Functions, Helpers);
  Source += R"C(
static unsigned initializations;
static unsigned retentions;
static int mismatch;
void neverd_swift_once(void *predicate, void (*initializer)(void *), void *context) {
  ++initializations;
  initializer(context);
  *(intptr_t *)predicate = -1;
}
void neverd_swift_once_initializer_1400(void *context) {
  uint64_t *words = (uint64_t *)(uintptr_t)neverd_local_storage_2020_address();
  mismatch |= context != (void *)(uintptr_t)neverd_local_storage_2010_address();
  words[0] = UINT64_C(0x1122334455667788);
  words[1] = UINT64_C(0x8877665544332211);
}
void *swift_bridgeObjectRetain(void *value) {
  uint64_t words[2];
  memcpy(words, (void *)neverd_local_storage_2000_address(), sizeof(words));
  mismatch |= words[0] != UINT64_C(0x1122334455667788);
  mismatch |= words[1] != UINT64_C(0x8877665544332211);
  mismatch |= (uintptr_t)value != words[1];
  ++retentions;
  return value;
}
int main(void) {
  initialize_string((void *)0x1234);
  initialize_string((void *)0x5678);
  return mismatch || initializations != 1 || retentions != 2;
}
)C";
  execute(Source);
}
} // namespace
