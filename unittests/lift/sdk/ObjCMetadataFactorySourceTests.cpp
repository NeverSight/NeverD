#include "../../../lib/sdk/capi/ObjCMetadataFactorySources.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

using namespace neverd;
using namespace neverd::sdk;

namespace {
struct FactoryFixture {
  static constexpr va_t Root = 0x1000, OtherRoot = 0x1020;
  static constexpr va_t Shared = 0x1100, Metadata = 0x1200,
                        OtherMetadata = 0x1300;
  static constexpr va_t Profile = 0x2400;
  BinaryImage Image;
  llvm::LLVMContext Context;
  PipelineResult Result;

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
  static uint32_t branch(va_t Site, va_t Target, bool Link = true) {
    return (Link ? 0x94000000u : 0x14000000u) |
           (uint32_t((int64_t(Target) - int64_t(Site)) / 4) & 0x03ffffff);
  }
  HighFunc &high(va_t Entry) {
    const auto It =
        std::find_if(Result.HighFuncs.begin(), Result.HighFuncs.end(),
                     [&](const auto &F) { return F.Entry == Entry; });
    if (It == Result.HighFuncs.end())
      throw std::runtime_error("fixture function missing");
    return *It;
  }
  FactoryFixture() {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Entry = Root;
    for (unsigned I = 0; I < 2; ++I) {
      Segment S;
      S.Name = I ? "__DATA" : "__TEXT";
      S.VA = 0x1000 + I * 0x1000;
      S.FileOff = I * 0x1000;
      S.Size = S.FileSz = 0x1000;
      S.Flags = SegmentFlags::Readable |
                (I ? SegmentFlags::Writable : SegmentFlags::Executable);
      S.Data.resize(S.Size);
      Image.Segments.push_back(std::move(S));
    }
    for (const auto [Address, Size, Name] :
         {std::tuple{0x1000, 0x1000, "__text"},
          std::tuple{0x2000, 0x400, "__data"},
          std::tuple{0x2400, 0x100, "__llvm_prf_cnts"},
          std::tuple{0x2500, 0xb00, "__data"}}) {
      Section S;
      S.Name = Name;
      S.SegmentName = Address < 0x2000 ? "__TEXT" : "__DATA";
      S.VA = Address;
      S.FileOff = Address - 0x1000;
      S.Size = S.FileSz = Size;
      S.Flags = Image.Segments[Address < 0x2000 ? 0 : 1].Flags;
      S.Type = Address < 0x2000
                   ? uint32_t(llvm::MachO::S_ATTR_PURE_INSTRUCTIONS)
                   : uint32_t(llvm::MachO::S_REGULAR);
      Image.Sections.push_back(S);
    }
    for (const auto [Entry, Counter, Accessor] :
         {std::tuple{Root, Profile, Metadata},
          std::tuple{OtherRoot, Profile + 8, OtherMetadata}}) {
      word(Entry, 0xb0000002);
      word(Entry + 4, 0x91000042 | ((Counter & 4095) << 10));
      word(Entry + 8, 0x90000003);
      word(Entry + 12, 0x91000063 | ((Accessor & 4095) << 10));
      word(Entry + 16, branch(Entry + 16, Shared, false));
    }
    const uint32_t Body[] = {
        0xa9bf7bfd, 0x910003fd, 0xf9400048,
        0x91000508, 0xf9000048, 0xd2800000,
        0xd63f0060, 0xa8c17bfd, branch(Shared + 32, 0x1260, false)};
    for (unsigned I = 0; I < std::size(Body); ++I)
      word(Shared + I * 4, Body[I]);
    for (const auto Entry : {Metadata, OtherMetadata}) {
      const uint32_t Accessor[] = {0xa9bf7bfd,
                                   0x910003fd,
                                   0xb0000000,
                                   0x91008000,
                                   branch(Entry + 16, 0x1240),
                                   0xd2800001,
                                   0xa8c17bfd,
                                   0xd65f03c0};
      for (unsigned I = 0; I < std::size(Accessor); ++I)
        word(Entry + I * 4, Accessor[I]);
    }
    for (const auto [Entry, Slot, Name, Module] :
         {std::tuple{0x1240, 0x2f00, "_objc_opt_self",
                     "/usr/lib/libobjc.A.dylib"},
          std::tuple{0x1260, 0x2f08, "_swift_getObjCClassFromMetadata",
                     "/usr/lib/swift/libswiftCore.dylib"}}) {
      word(Entry, 0xb0000010);
      word(Entry + 4, 0xf9400210 | (((Slot & 4095) / 8) << 10));
      word(Entry + 8, 0xd61f0200);
      Image.Symbols.push_back({Name, va_t(Entry), 12, true});
      Image.Exports.push_back({Name, 0, va_t(Entry)});
      Image.ImportPtrSlots[Slot] = Name;
      EXPECT_TRUE(Image.recordDyldBindSlot(Slot, Name, 0, Module, false));
    }
    pointer(0x2020, 0x2080);
    pointer(0x2040, 0x2200);
    pointer(0x20a0, 0x2280);
    pointer(0x2280, 1);
    pointer(0x2218, 0x2380);
    pointer(0x2298, 0x2380);
    const char Name[] = "Receiver";
    EXPECT_TRUE(Image.writeVA(0x2380, reinterpret_cast<const uint8_t *>(Name),
                              sizeof(Name)));
    ObjCClass Class;
    Class.Address = 0x2020;
    Class.Name = Name;
    Class.SuperclassName = "UICollectionViewLayout";
    Class.InheritanceStatus = "resolved";
    Image.ObjCClasses.push_back(Class);
    for (const auto [Entry, Selector] :
         {std::pair{Root, "layoutAttributesClass"},
          std::pair{OtherRoot, "invalidationContextClass"}}) {
      ObjCMethod M;
      M.Status = "supported";
      M.Implementation = Entry;
      M.IsClassMethod = true;
      M.ClassName = Class.Name;
      M.ClassAddress = Class.Address;
      M.Selector = Selector;
      M.TypeEncoding = "#16@0:8";
      M.TypeHint = parseObjCMethodEncoding(M.Selector, M.TypeEncoding);
      std::string Error;
      EXPECT_TRUE(assignDarwinObjCSourceABI(*M.TypeHint, Image.Arch, Error));
      Image.ObjCMethods.push_back(M);
    }
    for (const auto Entry :
         {Root, OtherRoot, Shared, Metadata, OtherMetadata}) {
      const auto Name = "factory_" + std::to_string(Entry);
      Image.Symbols.push_back({Name, Entry, 0, true});
      Image.Exports.push_back({Name, 0, Entry});
    }
    SourceFunctionTypeHint MetadataHint;
    MetadataHint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    MetadataHint.ReturnType = NdType::makeInt(8);
    std::string Error;
    EXPECT_TRUE(assignDarwinScalarSourceABI(MetadataHint, Image.Arch, Error));
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Root, OtherRoot, Shared, Metadata,
                                   OtherMetadata};
    Options.SourceTypeHints.emplace(Metadata, MetadataHint);
    Options.SourceTypeHints.emplace(OtherMetadata, MetadataHint);
    Result = Pipeline().run(Image, Context, Options);
    EXPECT_TRUE(Result.Success) << Result.Error;
  }
};

TEST(ObjCMetadataFactorySources, KeepsPerCallerTargetsAndSharedCounterStorage) {
  FactoryFixture F;
  const ObjCProfileStorage Storage(F.Image);
  const auto Plan =
      discoverObjCMetadataFactorySources(F.Image, F.Result, Storage);
  ASSERT_EQ(Plan.Callers.size(), 2U);
  auto A = projectObjCMetadataFactory(F.high(F.Root), F.Image, Plan, Storage);
  auto B =
      projectObjCMetadataFactory(F.high(F.OtherRoot), F.Image, Plan, Storage);
  ASSERT_TRUE(A.Projected);
  ASSERT_TRUE(B.Projected);
  EXPECT_EQ(A.Dependencies, std::set<va_t>{F.Metadata});
  EXPECT_EQ(B.Dependencies, std::set<va_t>{F.OtherMetadata});
  EXPECT_EQ(A.ProfileSections, std::set<va_t>{F.Profile});
  EXPECT_EQ(B.ProfileSections, A.ProfileSections);
  std::map<va_t, const HighFunc *> Functions;
  for (const auto &Function : F.Result.HighFuncs)
    Functions.emplace(Function.Entry, &Function);
  for (auto *P : {&A, &B}) {
    const auto Bound = [&](const HighExpr &E) {
      return objCMetadataFactorySourceCallBound(E, F.Image, Plan, Storage,
                                                P->Function, Functions);
    };
    const auto &Call = *P->Function.Body.front().RetVal;
    EXPECT_TRUE(Bound(Call));
    EXPECT_TRUE(Bound(*Call.Operands[0]));
    EXPECT_TRUE(Bound(*Call.Operands[1]));
    const auto *Audit = objc_super_getter_detail::uniqueEntry(
        F.Result.FunctionAudits, P->Function.Entry);
    EXPECT_TRUE(sourceBodyLimitation(P->Function, *P->Function.SourceTypeHint,
                                     Audit, Bound)
                    .empty());
  }
  // An unrecognized caller and the original shared helper retain their own
  // ABI and unresolved indirect call. No all-callers assumption is made.
  auto Unknown = F.high(F.Root);
  Unknown.Entry = 0x1400;
  EXPECT_FALSE(
      projectObjCMetadataFactory(Unknown, F.Image, Plan, Storage).Projected);
  EXPECT_FALSE(
      projectObjCMetadataFactory(F.high(F.Shared), F.Image, Plan, Storage)
          .Projected);
}

TEST(ObjCMetadataFactorySources, RejectsChangedMachineStorageAndProviderProof) {
  for (unsigned Mutation = 0; Mutation != 30; ++Mutation) {
    SCOPED_TRACE(Mutation);
    FactoryFixture F;
    const ObjCProfileStorage Before(F.Image);
    const auto Plan =
        discoverObjCMetadataFactorySources(F.Image, F.Result, Before);
    ASSERT_EQ(Plan.Callers.size(), 2U);
    switch (Mutation) {
    case 0:
      F.word(F.Root + 16, F.branch(F.Root + 16, F.Shared));
      break;
    case 1:
      F.word(F.Root + 12, 0x91100063);
      break; // missing accessor
    case 2:
      F.word(F.Shared + 8, 0xb9400048);
      break; // 32-bit counter
    case 3:
      F.word(F.Shared + 16, 0xf9000068);
      break; // store through x3
    case 4:
      F.word(F.Shared + 20, 0xaa0203e3);
      break; // overwrite call target
    case 5:
      F.word(F.Shared + 24, 0xd63f0080);
      break; // call x4
    case 6:
      F.word(F.Shared + 28, 0xa8c17bfc);
      break; // wrong frame restore
    case 7:
      F.word(F.Metadata + 8, 0xaa1403e0);
      break; // hidden context
    case 8:
      F.word(F.Metadata + 16, F.branch(F.Metadata + 16, 0x1260));
      break;
    case 9:
      F.Image.DyldBindSlots.at(0x2f08).Module = "/tmp/libswiftCore.dylib";
      break;
    case 10:
      F.Image.DyldBindSlots.at(0x2f08).WeakImport = true;
      break;
    case 11:
      F.Image.CodePtrRelocSlots.insert(F.Root + 8);
      break;
    case 12:
      F.Image.Sections[2].Name = "__data";
      break;
    case 13:
      F.Image.DataPtrRelocSlots.insert(F.Profile);
      break;
    case 14:
      F.Image.Sections[2].FileSz = 4;
      break;
    case 15:
      F.Image.ObjCMethods[0].IsClassMethod = false;
      break;
    case 16:
      F.Image.ObjCMethods.push_back(F.Image.ObjCMethods.front());
      break;
    case 17:
      F.Result.SourceImage = nullptr;
      break;
    case 18:
      F.Result.HighFuncs.push_back(F.high(F.Metadata));
      break;
    case 19:
      F.high(F.Metadata).SourceTypeHint->ReturnLocation.RegisterOffset = 8;
      break;
    case 20:
      F.high(F.Metadata).DoesNotReturn = true;
      break;
    case 21:
      F.high(F.Metadata)
          .Params.push_back({"context", NdType::makePtr(NdType::makeVoid())});
      break;
    case 22:
      for (auto &A : F.Result.FunctionAudits)
        if (A.Entry == F.Shared)
          A.MedIRVerified = false;
      break;
    case 23:
      for (auto &L : F.Result.LowFuncs)
        if (L.Entry == F.Shared)
          for (auto &O : L.Blocks.front().Ops)
            if (O.Opcode == NdOp::INDIR_CALL)
              O.Inputs[0] = NdVar::reg(32, 8);
      break;
    case 24:
      F.Image.DyldBindSlots.at(0x2f08).Addend = 8;
      break;
    case 25:
      F.Image.ConflictingImportStorageSlots.insert(0x2f08);
      break;
    case 26:
      F.Image.DyldBindSlots.erase(0x2f08);
      break;
    case 27:
      F.Image.DyldBindSlots.at(0x2f08).Name = "_other";
      break;
    case 28:
      F.Image.Sections.push_back(F.Image.Sections.front());
      break;
    case 29:
      for (const auto &L : F.Result.LowFuncs)
        if (L.Entry == F.Shared) {
          F.Result.LowFuncs.push_back(L);
          break;
        }
      break;
    }
    const ObjCProfileStorage Storage(F.Image);
    EXPECT_FALSE(validatedObjCMetadataFactory(F.Image, Plan, Storage, F.Root));
    std::set<std::string> Helpers;
    EXPECT_THROW(renderObjCMetadataFactoryHelpers(F.Image, Plan, Storage,
                                                  {F.Root}, Helpers),
                 std::runtime_error);
  }
}

TEST(ObjCMetadataFactorySources,
     PublicationChecksCurrentOperandsAndDependencies) {
  FactoryFixture F;
  const ObjCProfileStorage Storage(F.Image);
  const auto Plan =
      discoverObjCMetadataFactorySources(F.Image, F.Result, Storage);
  std::map<va_t, const HighFunc *> Functions;
  for (const auto &Function : F.Result.HighFuncs)
    Functions.emplace(Function.Entry, &Function);
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto P = projectObjCMetadataFactory(F.high(F.Root), F.Image, Plan, Storage);
    ASSERT_TRUE(P.Projected);
    auto &Call = *P.Function.Body.front().RetVal;
    auto Hint = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
    auto StorageHint =
        std::make_shared<SourceCallTypeHint>(*Call.Operands[0]->SourceCallHint);
    auto MetadataHint =
        std::make_shared<SourceCallTypeHint>(*Call.Operands[1]->SourceCallHint);
    Call.SourceCallHint = Hint;
    Call.Operands[0]->SourceCallHint = StorageHint;
    Call.Operands[1]->SourceCallHint = MetadataHint;
    switch (Mutation) {
    case 0:
      Hint->DoesNotReturn = true;
      break;
    case 1:
      Call.CallAddr = F.Shared;
      break;
    case 2:
      Hint->TargetName += "_forged";
      break;
    case 3:
      Hint->Signature.Parameters[0].Location.RegisterOffset = 16;
      break;
    case 4:
      std::swap(Call.Operands[0], Call.Operands[1]);
      break;
    case 5:
      MetadataHint->TargetAddress = F.OtherMetadata;
      break;
    case 6:
      P.Function.Body.push_back(P.Function.Body.front());
      break;
    case 7:
      Functions.erase(F.Metadata);
      break;
    case 8:
      StorageHint->TargetAddress += 8;
      break;
    }
    EXPECT_FALSE(objCMetadataFactorySourceCallBound(
        Call, F.Image, Plan, Storage, P.Function, Functions));
    Functions[F.Metadata] = &F.high(F.Metadata);
  }
  // The C backend independently rejects a forged abstraction ABI or a
  // marker from an unrelated runtime contract, even without SDK publication.
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto P = projectObjCMetadataFactory(F.high(F.Root), F.Image, Plan, Storage);
    auto &Call = *P.Function.Body.front().RetVal;
    auto Hint = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
    Call.SourceCallHint = Hint;
    if (Mutation == 1)
      Hint->Signature.HasExplicitABI = false;
    else if (Mutation == 2)
      Hint->Signature.Parameters[0].Location.RegisterOffset = 16;
    else if (Mutation == 3)
      Hint->Format.emplace(SourceCallTypeHint::FormatArguments{});
    else if (Mutation == 4)
      Hint->WeakImport = true;
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    const bool Emitted =
        HighCEmitter().emit({P.Function, F.high(F.Metadata)}, OS, Options);
    if (!Mutation) {
      EXPECT_TRUE(Emitted) << Source;
      EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
    } else
      EXPECT_NE(Source.find("bad source call"), std::string::npos) << Source;
  }
  // A matching declaration alone never removes the provider dependency.
  auto P = projectObjCMetadataFactory(F.high(F.Root), F.Image, Plan, Storage);
  F.high(F.Metadata).Body.clear();
  EXPECT_EQ(P.Dependencies, std::set<va_t>{F.Metadata});
  EXPECT_FALSE(sourceBodyLimitation(F.high(F.Metadata),
                                    *F.high(F.Metadata).SourceTypeHint, nullptr,
                                    [](const HighExpr &) { return false; })
                   .empty());
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
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-factory", "c", C));
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-factory", "exe", Exe));
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-factory", "err", Err));
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

TEST(ObjCMetadataFactorySources,
     RuntimePreservesCounterAliasesWrapAndCallOrder) {
  FactoryFixture F;
  F.pointer(F.Profile, UINT64_MAX);
  F.pointer(F.Profile + 8, 7);
  const ObjCProfileStorage Storage(F.Image);
  const auto Plan =
      discoverObjCMetadataFactorySources(F.Image, F.Result, Storage);
  ASSERT_EQ(Plan.Callers.size(), 2U);
  std::set<std::string> Helpers, StorageHelpers;
  std::string Source = "#include <stdint.h>\n#include <string.h>\n";
  Source += Storage.render({F.Profile}, StorageHelpers);
  Source += renderObjCMetadataFactoryHelpers(F.Image, Plan, Storage,
                                             {F.Root, F.OtherRoot}, Helpers);
  const auto StorageName = ObjCProfileStorage::helperName(F.Profile);
  Source +=
      "\nstatic unsigned trace; static int mismatch;\n"
      "static uint64_t counter(unsigned index) { uint64_t v; memcpy(&v, "
      "(void *)(" +
      StorageName +
      "() + index*8), 8); return v; }\n"
      "static int64_t first(void) { mismatch |= trace != 0 || counter(0) != 0 "
      "|| "
      "counter(1) != 7; trace = 1; return 0x1111; }\n"
      "static int64_t second(void) { mismatch |= trace != 2 || counter(0) != 0 "
      "|| "
      "counter(1) != 8; trace = 3; return 0x2222; }\n"
      "void *swift_getObjCClassFromMetadata(void *value) { "
      "mismatch |= trace != 1 && trace != 3; ++trace; return value; }\n"
      "int main(void) { void *storage = (void *)" +
      StorageName +
      "();\n"
      " void *a = neverd_objc_metadata_factory_1000(storage, (void *)&first);\n"
      " void *b = neverd_objc_metadata_factory_1020(storage, (void "
      "*)&second);\n"
      " return mismatch || trace != 4 || a != (void *)0x1111 || b != (void "
      "*)0x2222; }\n";
  execute(Source);
}
} // namespace
