//===- LocalTypesDebugContextTests.cpp - Local types context --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LocalTypesDebugContext.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/SourceCallABI.h"
#include "neverd/libc/LibCFloatingArguments.h"
#include "neverd/types/TypeParse.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
#define LOCAL_SOURCE_CALL_TEXT(Name, Text) constexpr char Name[] = Text;
#define LOCAL_SOURCE_CALL_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "LocalTypesSourceCallFixtures.def"

/// Debug information that names functions and types none, as a PDB of
/// public symbols does.
class NamesOnlyDebugContext final : public neverd::DebugContext {
public:
  std::map<neverd::va_t, neverd::FunctionSym> Functions;
  std::optional<neverd::FunctionSym>
  resolveFunction(neverd::va_t Addr) const override {
    for (const auto &[Entry, Function] : Functions)
      if (Function.contains(Addr) || Entry == Addr)
        return Function;
    return std::nullopt;
  }
  std::optional<neverd::VariableSym> resolveVariable(neverd::va_t,
                                                     int64_t) const override {
    return std::nullopt;
  }
  std::optional<neverd::TypeSym> resolveType(uint64_t) const override {
    return std::nullopt;
  }
  std::optional<neverd::SourceLoc> sourceLocation(neverd::va_t) const override {
    return std::nullopt;
  }
  std::vector<neverd::FunctionSym> allFunctions() const override { return {}; }
  bool hasInfo() const override { return true; }
};

} // namespace

TEST(DebugContext, NoReturnQueryUsesTheProvidersValidatedPrototype) {
  struct Prototypes final : neverd::NullDebugContext {
    std::optional<neverd::FunctionSym>
    resolvePrototype(llvm::StringRef Name) const override {
      if (Name == "missing")
        return std::nullopt;
      neverd::FunctionSym Function;
      Function.NoReturn = Name == "stop";
      return Function;
    }
  } Context;
  EXPECT_TRUE(Context.prototypeDoesNotReturn("stop"));
  EXPECT_FALSE(Context.prototypeDoesNotReturn("returning"));
  EXPECT_FALSE(Context.prototypeDoesNotReturn("missing"));
  ASSERT_TRUE(Context.resolvePrototypeABI("stop"));
  EXPECT_TRUE(Context.resolvePrototypeABI("stop")->NoReturn);
  EXPECT_FALSE(Context.resolvePrototypeABI("missing"));
}

TEST(LocalTypesDebugContext, TakesDeclarationsAndNewTypesInPlace) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>(types::DataModel::LLP64);
  ASSERT_TRUE(
      types::parseCTypes("struct Known { int Value; };", *Library).ok());
  NamesOnlyDebugContext Names;
  const va_t Entry = 0x1000, Other = 0x2000;
  sdk::LocalTypesDebugContext Context(
      Names, Library, std::map<va_t, std::string>{{Entry, "int Entry(void)"}});
  UserFactReads Reads;
  Context.noteDeclarationReads(&Reads);
  const auto ReturnBytes = [&](va_t Address) -> unsigned {
    const auto Function = Context.resolveFunction(Address);
    return Function && Function->ReturnType ? Function->ReturnType->Size : 0;
  };
  EXPECT_EQ(ReturnBytes(Entry), 4u);
  // What it asks of a function without a declaration counts too.
  EXPECT_EQ(ReturnBytes(Other), 0u);
  EXPECT_EQ(Reads.addresses(), (std::vector<va_t>{Entry, Other}));
  {
    // Work for every result reads apart from the result so far.
    const UserFactReads::Held Held(Reads);
    EXPECT_EQ(Reads.addresses(), std::vector<va_t>{});
    EXPECT_EQ(ReturnBytes(0x3000), 0u);
  }
  EXPECT_EQ(Reads.addresses(), (std::vector<va_t>{Entry, Other}));

  // No name binds a prototype here: nothing analysis read changes.
  EXPECT_FALSE(Context.takeDeclarations(
      {{Entry, "long long Entry(void)"}, {Other, "char Other(void)"}}));
  EXPECT_EQ(ReturnBytes(Entry), 8u);
  EXPECT_EQ(ReturnBytes(Other), 1u);
  EXPECT_FALSE(Context.takeDeclarations({{Other, "short Other(void)"}}));
  EXPECT_EQ(ReturnBytes(Entry), 0u);
  EXPECT_EQ(ReturnBytes(Other), 2u);

  // A declaration that names a type the context has yet to take does not read;
  // one read after it takes the type does, as in a context made of them all.
  EXPECT_FALSE(Context.takeDeclarations({{Entry, "struct Later Entry(void)"}}));
  EXPECT_EQ(ReturnBytes(Entry), 0u);
  types::TypeLibrary Grown(*Library);
  ASSERT_TRUE(types::parseCTypes("struct Later { long long First; long long "
                                 "Second; };",
                                 Grown)
                  .ok());
  Context.takeNewTypes(Grown, {"Later"});
  EXPECT_EQ(Context.library().size(), Grown.size());
  EXPECT_FALSE(
      Context.takeDeclarations({{Entry, "struct Later *Entry(void)"}}));
  EXPECT_EQ(ReturnBytes(Entry), 8u);
  // A second take of what it holds adds nothing.
  Context.takeNewTypes(Grown, {"Later"});
  EXPECT_EQ(Context.library().size(), Grown.size());
}

TEST(LocalTypesDebugContext, TakesADeclarationThatHidesAPrototypeInPlace) {
  using namespace neverd;
  const types::TypeLibrary *Kernel =
      types::headerLibrary(types::HeaderLibrary::WindowsKernelX64);
  ASSERT_NE(Kernel, nullptr);
  auto Library = std::make_shared<types::TypeLibrary>(Kernel->dataModel());
  types::mergeHeaderTypes(*Library, *Kernel);
  BinaryImage Image;
  const va_t Owner = 0x1000, Plain = 0x2000;
  Image.Exports = {{"IoThreadToProcess", 1, Owner}, {"Unprototyped", 2, Plain}};
  NamesOnlyDebugContext Names;
  const std::vector<types::HeaderLibrary> Headers{
      types::HeaderLibrary::WindowsKernelX64};
  sdk::LocalTypesDebugContext Context(Names, Library, {}, &Image, Headers);
  // What a context made of the same declarations reads of each function.
  const auto reads = [&](const sdk::LocalTypesDebugContext &From, va_t At) {
    const std::optional<FunctionSym> Function = From.resolveFunction(At);
    if (!Function)
      return std::string("none");
    std::string Text = Function->PrototypeLibrary + "|" +
                       Function->PrototypeDeclaration + "|" +
                       std::to_string(Function->Params.size());
    for (const auto &[Name, Type] : Function->Params)
      Text += "|" + Name + ":" + std::to_string(Type ? Type->Size : 0);
    return Text + "|" +
           std::to_string(Function->ReturnType ? Function->ReturnType->Size
                                               : 0);
  };
  const auto agrees = [&](const std::map<va_t, std::string> &Declarations) {
    sdk::LocalTypesDebugContext Made(Names, Library, Declarations, &Image,
                                     Headers);
    for (const va_t At : {Owner, Plain})
      EXPECT_EQ(reads(Context, At), reads(Made, At)) << std::hex << At;
  };
  EXPECT_TRUE(Context.hidesPrototype(Owner));
  EXPECT_FALSE(Context.hidesPrototype(Plain));
  EXPECT_EQ(Context.resolveFunction(Owner)->PrototypeLibrary,
            "windows-kernel-x64");

  // A declaration of a function whose name binds no prototype: nothing
  // analysis read changes.
  const std::map<va_t, std::string> PlainOnly{
      {Plain, "int Unprototyped(int Value, int Other)"}};
  EXPECT_FALSE(Context.takeDeclarations(PlainOnly));
  agrees(PlainOnly);
  // One that hides the header's prototype: the context takes it as well,
  // and says that analysis read what it changes.
  std::map<va_t, std::string> Hiding = PlainOnly;
  Hiding[Owner] = "void *IoThreadToProcess(void *Thread, int Extra)";
  EXPECT_TRUE(Context.takeDeclarations(Hiding));
  ASSERT_TRUE(Context.resolveFunction(Owner).has_value());
  EXPECT_EQ(Context.resolveFunction(Owner)->Params.size(), 2u);
  EXPECT_TRUE(Context.resolveFunction(Owner)->PrototypeLibrary.empty());
  agrees(Hiding);
  // Whether a declaration hides one reads the function's name alone.
  EXPECT_TRUE(Context.hidesPrototype(Owner));
  // Another declaration of it hides the prototype still.
  Hiding[Owner] = "void *IoThreadToProcess(void *Thread)";
  EXPECT_TRUE(Context.takeDeclarations(Hiding));
  EXPECT_EQ(Context.resolveFunction(Owner)->Params.size(), 1u);
  agrees(Hiding);
  // Given back, the function reads the header's prototype again.
  EXPECT_TRUE(Context.takeDeclarations(PlainOnly));
  EXPECT_EQ(Context.resolveFunction(Owner)->PrototypeDeclaration,
            "PEPROCESS IoThreadToProcess(PETHREAD Thread)");
  agrees(PlainOnly);
  // The same declarations again change nothing.
  EXPECT_FALSE(Context.takeDeclarations(PlainOnly));
}

TEST(LocalTypesDebugContext, NewTypesReadAsInAContextMadeOfThem) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>(types::DataModel::LLP64);
  // C spells this name as the new type's, which steps aside for it.
  types::LocalType Scoped;
  Scoped.Name = "Scope::Item";
  Scoped.Kind = types::TypeKind::Struct;
  Scoped.Record = std::make_shared<types::RecordDef>();
  Library->define(std::move(Scoped));
  ASSERT_TRUE(
      types::parseCTypes("struct Known { int Value; };", *Library).ok());
  NamesOnlyDebugContext Names;
  sdk::LocalTypesDebugContext Context(Names, Library);
  const auto Known = NdType::makeNamedRecord("Known");
  const auto Item = NdType::makeNamedRecord("Scope::Item");
  const auto Later = NdType::makeNamedRecord("Scope__Item");
  EXPECT_FALSE(Context.definitions({Known, Item}).Text.empty());

  auto Grown = std::make_shared<types::TypeLibrary>(*Library);
  ASSERT_TRUE(
      types::parseCTypes("struct Scope__Item { long long Value; };", *Grown)
          .ok());
  Context.takeNewTypes(*Grown, {"Scope__Item"});
  NamesOnlyDebugContext FreshNames;
  sdk::LocalTypesDebugContext Fresh(FreshNames, Grown);
  for (const std::vector<TypeRef> &Unit :
       {std::vector<TypeRef>{Known, Item}, std::vector<TypeRef>{Later},
        std::vector<TypeRef>{Later, Item, Known}}) {
    const CRecordDefinitions Taken = Context.definitions(Unit);
    const CRecordDefinitions Made = Fresh.definitions(Unit);
    EXPECT_EQ(Taken.Text, Made.Text);
    EXPECT_EQ(Taken.Records, Made.Records);
    EXPECT_EQ(Taken.Tags, Made.Tags);
    EXPECT_EQ(Taken.Identifiers, Made.Identifiers);
  }
}

TEST(LocalTypesDebugContext, NewTypesOfTheirOwnNamesKeepWhatSpellsTheLibrary) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>(types::DataModel::LLP64);
  // A name C spells otherwise, an enumeration and prototypes, as a program's
  // debug information gives them.
  types::LocalType Scoped;
  Scoped.Name = "Scope::Item";
  Scoped.Kind = types::TypeKind::Struct;
  Scoped.Record = std::make_shared<types::RecordDef>();
  Library->define(std::move(Scoped));
  ASSERT_TRUE(
      types::parseCTypes("struct Known { int Value; };\n"
                         "enum Mode { ModeA, ModeB = 4 };\n"
                         "int UseKnown(struct Known *Item);\n"
                         "int UseMode(enum Mode How, struct Known *Item);\n",
                         *Library)
          .ok());
  NamesOnlyDebugContext Names;
  sdk::LocalTypesDebugContext Context(Names, Library);
  const auto Known = NdType::makeNamedRecord("Known");
  const auto Item = NdType::makeNamedRecord("Scope::Item");
  const auto Mode = NdType::makeNamedRecord("Mode", 0, /*Enum=*/true);
  // What spells and renders the library's names is made before they come.
  EXPECT_FALSE(Context.definitions({Known, Item, Mode}).Text.empty());
  ASSERT_TRUE(Context.resolvePrototype("UseKnown").has_value());

  // New types spelled by their own names keep it, and read as in a context
  // made of them.
  auto Grown = std::make_shared<types::TypeLibrary>(*Library);
  ASSERT_TRUE(types::parseCTypes(
                  "struct Later { long long Value; struct Known *Next; };\n"
                  "enum Phase { PhaseOne, PhaseTwo = 7 };\n",
                  *Grown)
                  .ok());
  Context.takeNewTypes(*Grown, {"Later", "Phase"}, /*NamesHold=*/true);
  EXPECT_EQ(Context.library().size(), Grown->size());
  NamesOnlyDebugContext FreshNames;
  sdk::LocalTypesDebugContext Fresh(FreshNames, Grown);
  const auto Later = NdType::makeNamedRecord("Later");
  const auto Phase = NdType::makeNamedRecord("Phase", 0, /*Enum=*/true);
  for (const std::vector<TypeRef> &Unit :
       {std::vector<TypeRef>{Known, Item, Mode}, std::vector<TypeRef>{Later},
        std::vector<TypeRef>{Phase, Later, Item}}) {
    const CRecordDefinitions Taken = Context.definitions(Unit);
    const CRecordDefinitions Made = Fresh.definitions(Unit);
    EXPECT_EQ(Taken.Text, Made.Text);
    EXPECT_EQ(Taken.Records, Made.Records);
    EXPECT_EQ(Taken.Tags, Made.Tags);
    EXPECT_EQ(Taken.Identifiers, Made.Identifiers);
    EXPECT_EQ(Taken.Enumerations, Made.Enumerations);
  }
  // A prototype read after them spells its types as such a context does.
  const auto Taken = Context.resolvePrototype("UseMode");
  const auto Made = Fresh.resolvePrototype("UseMode");
  ASSERT_TRUE(Taken && Made);
  EXPECT_EQ(Taken->PrototypeDeclaration, Made->PrototypeDeclaration);
  EXPECT_EQ(Taken->PortablePrototypeDeclaration,
            Made->PortablePrototypeDeclaration);
  EXPECT_EQ(Taken->PortableParameterTypes, Made->PortableParameterTypes);
}

TEST(LocalTypesDebugContext, NoReturnQueryMatchesValidatedPrototype) {
  using namespace neverd;
  for (auto Model : {types::DataModel::ILP32, types::DataModel::LLP64,
                     types::DataModel::LP64}) {
    auto Library = std::make_shared<types::TypeLibrary>(Model);
    ASSERT_TRUE(types::parseCTypes(R"(
      typedef struct Node { struct Node *Next; unsigned long Value; } Node;
      Node *ReadNode(Node *Input);
      _Noreturn void Stop(Node *Input);
      _Noreturn void StopWithMessage(const char *Message, ...);
    )",
                                   *Library)
                    .ok());
    auto BadReturn = std::make_shared<types::FunctionDef>();
    BadReturn->NoReturn = true;
    BadReturn->Return =
        types::makeNamed("MissingType", types::TypeKind::Typedef);
    Library->definePrototype("BadReturn", BadReturn);
    auto BadParameter = std::make_shared<types::FunctionDef>();
    BadParameter->NoReturn = true;
    BadParameter->Return = types::makeBuiltin(types::Builtin::Void);
    BadParameter->Params.push_back(
        {"Input", types::makeBuiltin(types::Builtin::Void)});
    Library->definePrototype("BadParameter", BadParameter);
    NamesOnlyDebugContext Names;
    sdk::LocalTypesDebugContext Context(Names, Library);
    for (const char *Name : {"ReadNode", "Stop", "StopWithMessage", "BadReturn",
                             "BadParameter", "Absent"}) {
      SCOPED_TRACE(Name);
      const bool Before = Context.prototypeDoesNotReturn(Name);
      const auto Signature = Context.resolvePrototype(Name);
      EXPECT_EQ(Before, Signature && Signature->NoReturn);
      EXPECT_EQ(Context.prototypeDoesNotReturn(Name), Before);
    }
    EXPECT_TRUE(Context.prototypeDoesNotReturn("Stop"));
    EXPECT_FALSE(Context.prototypeDoesNotReturn("BadReturn"));
    EXPECT_FALSE(Context.prototypeDoesNotReturn("BadParameter"));
  }
}

TEST(LocalTypesDebugContext, NoReturnValidationRetainsTypeBoundaries) {
  using namespace neverd;
  for (auto Model : {types::DataModel::ILP32, types::DataModel::LLP64,
                     types::DataModel::LP64}) {
    auto Library = std::make_shared<types::TypeLibrary>(Model);
    ASSERT_TRUE(types::parseCTypes(R"(
      typedef long Count;
      typedef struct Node { struct Node *Next; Count Value; } Node;
      enum Mode { StopNow, StopLater };
      typedef void (*Callback)(Node *Input, Count Value);
    )",
                                   *Library)
                    .ok());
    auto Callback = std::make_shared<types::FunctionDef>();
    Callback->Return = types::makeBuiltin(types::Builtin::Void);
    const auto Missing = types::makeNamed("Missing", types::TypeKind::Typedef);
    // The display type stores object sizes in uint16_t. A value beyond that
    // boundary is unavailable, but a pointer to it still has a valid ABI.
    const auto Oversized =
        types::makeArray(types::makeBuiltin(types::Builtin::UChar), 0x10000);
    struct Case {
      const char *Name;
      types::TypeExprRef Type;
      bool Valid;
    };
    const std::vector<Case> Cases = {
        {"Count", types::makeNamed("Count", types::TypeKind::Typedef), true},
        {"Node", types::makeNamed("Node", types::TypeKind::Typedef), true},
        {"Mode", types::makeNamed("Mode", types::TypeKind::Enum), true},
        {"Callback", types::makeNamed("Callback", types::TypeKind::Typedef),
         true},
        {"Missing", Missing, false},
        {"Oversized", Oversized, false},
        {"OpaquePointer",
         types::makePointer(Missing, types::pointerBytes(Model)), true},
        {"LargePointer",
         types::makePointer(Oversized, types::pointerBytes(Model)), true},
        {"Function", types::makeFunction(Callback), false},
        {"Void", types::makeBuiltin(types::Builtin::Void), false},
        {"Null", nullptr, false}};
    for (const auto &Item : Cases) {
      auto Function = std::make_shared<types::FunctionDef>();
      Function->NoReturn = true;
      Function->Return = types::makeBuiltin(types::Builtin::Void);
      Function->Params.push_back({"Input", Item.Type});
      Library->definePrototype(std::string("Stop") + Item.Name,
                               std::move(Function));
    }
    NamesOnlyDebugContext Names;
    sdk::LocalTypesDebugContext Queried(Names, Library);
    sdk::LocalTypesDebugContext Resolved(Names, Library);
    for (const auto &Item : Cases) {
      const std::string Name = std::string("Stop") + Item.Name;
      SCOPED_TRACE(Name);
      EXPECT_EQ(Queried.prototypeDoesNotReturn(Name), Item.Valid);
      EXPECT_EQ(Queried.prototypeDoesNotReturn(Name), Item.Valid);
      const auto ABI = Queried.resolvePrototypeABI(Name);
      ASSERT_EQ(ABI.has_value(), Item.Valid);
      const auto Expected = Resolved.resolvePrototype(Name);
      const auto Actual = Queried.resolvePrototype(Name);
      ASSERT_EQ(Expected.has_value(), Item.Valid);
      ASSERT_EQ(Actual.has_value(), Item.Valid);
      EXPECT_EQ(Queried.prototypeDoesNotReturn(Name), Item.Valid);
      EXPECT_EQ(Resolved.prototypeDoesNotReturn(Name), Item.Valid);
      if (Expected && Actual) {
        EXPECT_EQ(Actual->PrototypeDeclaration, Expected->PrototypeDeclaration);
        EXPECT_EQ(Actual->PortablePrototypeDeclaration,
                  Expected->PortablePrototypeDeclaration);
        EXPECT_EQ(Actual->PortableParameterTypes,
                  Expected->PortableParameterTypes);
        EXPECT_TRUE(sdk::passesAlike(*Actual, *Expected));
        EXPECT_TRUE(sdk::passesAlike(*ABI, *Expected));
        EXPECT_EQ(ABI->PrototypeLibrary, Expected->PrototypeLibrary);
        EXPECT_EQ(ABI->NoReturn, Expected->NoReturn);
      }
    }
  }
}

TEST(LocalTypesDebugContext, ABIQueriesPreserveFloatingCallContracts) {
  using namespace neverd;
  for (auto Model : {types::DataModel::ILP32, types::DataModel::LLP64,
                     types::DataModel::LP64}) {
    auto Library = std::make_shared<types::TypeLibrary>(Model);
    ASSERT_TRUE(types::parseCTypes(R"(
      float Single(float Value, int Count);
      double Double(long Count, double Value);
      long double Extended(void);
      void Nothing(void);
      int Integer(void);
      float *Pointer(double Value);
      double Variadic(const char *Format, ...);
      double __fastcall Registers(double Value);
      struct Pair { double X, Y; };
      struct Pair Record(struct Pair Value);
    )",
                                   *Library)
                    .ok());
    NamesOnlyDebugContext Names;
    sdk::LocalTypesDebugContext Context(Names, Library);
    for (const char *Name :
         {"Single", "Double", "Extended", "Nothing", "Integer", "Pointer",
          "Variadic", "Registers", "Record"}) {
      SCOPED_TRACE(Name);
      const auto ABI = Context.resolvePrototypeABI(Name);
      const auto Full = Context.resolvePrototype(Name);
      ASSERT_TRUE(ABI && Full);
      EXPECT_EQ(ABI->PrototypeLibrary, Full->PrototypeLibrary);
      EXPECT_EQ(ABI->PrototypeFromHeader, Full->PrototypeFromHeader);
      EXPECT_EQ(ABI->PrototypeFromCLibrary, Full->PrototypeFromCLibrary);
      EXPECT_EQ(ABI->CallConv, Full->CallConv);
      EXPECT_EQ(ABI->IsVariadic, Full->IsVariadic);
      EXPECT_TRUE(sdk::passesAlike(*ABI, *Full));
      for (auto Arch : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
        for (auto Format :
             {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
          const auto Expected =
              libc::floatingCallArguments(*Full, Arch, Format);
          EXPECT_EQ(libc::floatingCallArguments(*ABI, Arch, Format), Expected);
          EXPECT_EQ(libc::floatingCallArguments(
                        *Context.resolvePrototypeABI(Name), Arch, Format),
                    Expected);
        }
    }
  }
}

TEST(LocalTypesDebugContext, ABIQueriesShareConversionWithSourceDeclarations) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>(types::DataModel::LLP64);
  ASSERT_TRUE(types::parseCTypes(R"(
    struct Payload { unsigned long Count; double Value; };
    long Read(struct Payload *Input, unsigned long Count);
  )",
                                 *Library)
                  .ok());
  NamesOnlyDebugContext Names;
  sdk::LocalTypesDebugContext Context(Names, Library);
  sdk::LocalTypesDebugContext Fresh(Names, Library);
  const auto ABI = Context.resolvePrototypeABI("Read");
  const auto Again = Context.resolvePrototypeABI("Read");
  ASSERT_TRUE(ABI && Again);
  EXPECT_TRUE(ABI->PrototypeDeclaration.empty());
  EXPECT_TRUE(ABI->PortablePrototypeDeclaration.empty());
  EXPECT_EQ(ABI->ReturnType, Again->ReturnType);
  EXPECT_EQ(ABI->Params.front().second, Again->Params.front().second);
  const auto Full = Context.resolvePrototype("Read");
  const auto Expected = Fresh.resolvePrototype("Read");
  ASSERT_TRUE(Full && Expected);
  EXPECT_EQ(Full->ReturnType, ABI->ReturnType);
  EXPECT_EQ(Full->Params.front().second, ABI->Params.front().second);
  EXPECT_EQ(Full->ReturnType->SourceName, Expected->ReturnType->SourceName);
  EXPECT_EQ(Full->PrototypeDeclaration, Expected->PrototypeDeclaration);
  EXPECT_EQ(Full->PortablePrototypeDeclaration,
            Expected->PortablePrototypeDeclaration);
  EXPECT_EQ(Full->PortableParameterTypes, Expected->PortableParameterTypes);
  EXPECT_TRUE(sdk::passesAlike(*Full, *Expected));
  // Completing the cache entry must not mutate an earlier returned value.
  EXPECT_TRUE(ABI->PrototypeDeclaration.empty());
  EXPECT_EQ(Context.resolvePrototypeABI("Read")->ReturnType, Full->ReturnType);
}

TEST(LocalTypesDebugContext, NewTypesInvalidateUnrenderedABISignatures) {
  using namespace neverd;
  for (bool NamesHold : {false, true}) {
    SCOPED_TRACE(NamesHold);
    auto Library =
        std::make_shared<types::TypeLibrary>(types::DataModel::LLP64);
    ASSERT_TRUE(types::parseCTypes(
                    "struct Payload; struct Payload *Read(void);", *Library)
                    .ok());
    NamesOnlyDebugContext Names;
    sdk::LocalTypesDebugContext Context(Names, Library);
    const auto Before = Context.resolvePrototypeABI("Read");
    ASSERT_TRUE(Before && Before->ReturnType);
    const auto BeforePointee = Before->ReturnType->Pointee;
    EXPECT_FALSE(BeforePointee && BeforePointee->Size);
    auto Grown = std::make_shared<types::TypeLibrary>(*Library);
    ASSERT_TRUE(
        types::parseCTypes(
            "struct Payload { long long First; long long Second; };", *Grown)
            .ok());
    Context.takeNewTypes(*Grown, {"Payload"}, NamesHold);
    sdk::LocalTypesDebugContext Fresh(Names, Grown);
    const auto Actual = Context.resolvePrototype("Read");
    const auto Expected = Fresh.resolvePrototype("Read");
    ASSERT_TRUE(Actual && Expected && Actual->ReturnType);
    ASSERT_TRUE(Actual->ReturnType->Pointee);
    EXPECT_EQ(Actual->ReturnType->Pointee->Size, 16);
    EXPECT_EQ(Actual->PrototypeDeclaration, Expected->PrototypeDeclaration);
    EXPECT_EQ(Actual->PortablePrototypeDeclaration,
              Expected->PortablePrototypeDeclaration);
    EXPECT_FALSE(BeforePointee && BeforePointee->Size);
  }
}

TEST(LocalTypesDebugContext, NewTypesNoPrototypeReachesKeepABISignatures) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>(types::DataModel::LLP64);
  ASSERT_TRUE(types::parseCTypes(R"(
    struct Payload { unsigned long Count; double Value; };
    long Read(struct Payload *Input, unsigned long Count);
  )",
                                 *Library)
                  .ok());
  NamesOnlyDebugContext Names;
  sdk::LocalTypesDebugContext Context(Names, Library);
  const auto Before = Context.resolvePrototypeABI("Read");
  ASSERT_TRUE(Before && Before->ReturnType);
  auto Grown = std::make_shared<types::TypeLibrary>(*Library);
  ASSERT_TRUE(
      types::parseCTypes("struct Unrelated { long long First; };", *Grown)
          .ok());
  // No prototype reaches the new name: the converted signature stays, the
  // very nodes it was, and reads as a fresh context's.
  Context.takeNewTypes(*Grown, {"Unrelated"}, /*NamesHold=*/true,
                       /*Prototyped=*/false);
  const auto Kept = Context.resolvePrototypeABI("Read");
  ASSERT_TRUE(Kept && Kept->ReturnType);
  EXPECT_EQ(Kept->ReturnType, Before->ReturnType);
  EXPECT_EQ(Kept->Params.front().second, Before->Params.front().second);
  sdk::LocalTypesDebugContext Fresh(Names, Grown);
  const auto Actual = Context.resolvePrototype("Read");
  const auto Expected = Fresh.resolvePrototype("Read");
  ASSERT_TRUE(Actual && Expected);
  EXPECT_EQ(Actual->PrototypeDeclaration, Expected->PrototypeDeclaration);
  EXPECT_EQ(Actual->PortablePrototypeDeclaration,
            Expected->PortablePrototypeDeclaration);
  EXPECT_TRUE(sdk::passesAlike(*Actual, *Expected));
  ASSERT_TRUE(Context.library().find("Unrelated"));
}

TEST(LocalTypesDebugContext, SourceCallDeclarationsRetainCompleteValueLayouts) {
  using namespace neverd;
  types::TypeLibrary Library(types::DataModel::LP64);
  ASSERT_TRUE(types::parseCTypes(kSourceValueDeclarations, Library).ok());
  const auto Parsed =
      types::parseFunctionDeclaration(kSourceValueCall, Library);
  ASSERT_TRUE(Parsed.ok());
  ASSERT_TRUE(Parsed.Function);
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::Analyst;
  Hint.Signature.ReturnType =
      sdk::sourceCallTypeOf(Library, *Parsed.Function->Return);
  ASSERT_TRUE(Hint.Signature.ReturnType);
  EXPECT_EQ(Hint.Signature.ReturnType->Fields.size(), kSourceValueRecordFields);
  EXPECT_TRUE(
      Hint.Signature.ReturnType->Fields.front()->Pointee->Fields.empty());
  const auto Pointer = Hint.Signature.ReturnType->Fields.front();
  auto OtherPointee = std::make_shared<NdType>(*Pointer->Pointee);
  EXPECT_TRUE(equalSourceTypes(Pointer, NdType::makePtr(OtherPointee)));
  OtherPointee->SourceName = kDifferentOpaqueRecordName;
  EXPECT_FALSE(equalSourceTypes(Pointer, NdType::makePtr(OtherPointee)));
  EXPECT_FALSE(equalSourceTypes(OtherPointee, OtherPointee));
  for (const auto &Parameter : Parsed.Function->Params) {
    const auto Value = sdk::sourceCallTypeOf(Library, *Parameter.Type);
    ASSERT_TRUE(Value);
    EXPECT_EQ(Value->Size, kSourceValueWideBytes);
    if (Value->Kind == NdTypeKind::Struct) {
      EXPECT_FALSE(Value->Fields.empty());
      EXPECT_TRUE(
          sdk::decompilerTypeOf(Library, *Parameter.Type)->Fields.empty());
    }
    Hint.Signature.Parameters.push_back({Parameter.Name, Value});
  }
  std::string Diagnostic;
  ASSERT_TRUE(assignAnalystCallABI(Hint, Arch::AArch64, BinaryFormat::MachO,
                                   Diagnostic))
      << Diagnostic;
  EXPECT_EQ(sourceABIParameters(Hint.Signature).size(),
            kSourceValuePhysicalArguments);
  const auto Unsupported =
      types::parseFunctionDeclaration(kUnsupportedSourceValueCall, Library);
  ASSERT_TRUE(Unsupported.ok());
  for (const auto &Parameter : Unsupported.Function->Params)
    EXPECT_FALSE(sdk::sourceCallTypeOf(Library, *Parameter.Type));
}

TEST(LocalTypesDebugContext,
     CallbackConventionsSurviveParameterAndReturnTypes) {
  using namespace neverd;
  for (auto Model : {types::DataModel::ILP32, types::DataModel::LLP64}) {
    auto Library = std::make_shared<types::TypeLibrary>(Model);
    ASSERT_TRUE(types::parseCTypes(R"(
      typedef int (__stdcall *EventCallback)(void *Context, unsigned int Code);
      EventCallback __stdcall SetCallback(EventCallback Handler, void *Context);
    )",
                                   *Library)
                    .ok());
    NamesOnlyDebugContext Names;
    sdk::LocalTypesDebugContext Context(Names, Library);
    const auto Signature = Context.resolvePrototype("SetCallback");
    ASSERT_TRUE(Signature);
    ASSERT_EQ(Signature->Params.size(), 2u);
    for (const auto &Type :
         {Signature->ReturnType, Signature->Params[0].second}) {
      ASSERT_NE(Type, nullptr);
      ASSERT_EQ(Type->Kind, NdTypeKind::Ptr);
      ASSERT_NE(Type->Pointee, nullptr);
      ASSERT_EQ(Type->Pointee->Kind, NdTypeKind::Func);
      const auto Expected = Model == types::DataModel::ILP32
                                ? types::CallingConvention::Stdcall
                                : types::CallingConvention::Unspecified;
      EXPECT_EQ(Type->Pointee->FunctionConvention, Expected);
    }
  }
}

TEST(LocalTypesDebugContext,
     PortableDeclarationsRetainQualifiersAndConvention) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>(types::DataModel::ILP32);
  ASSERT_TRUE(types::parseCTypes(R"(
    const unsigned short *__stdcall ReadText(const unsigned short *const *Input,
                                             volatile unsigned int *Output);
    _Noreturn void __cdecl Stop(void);
  )",
                                 *Library)
                  .ok());
  NamesOnlyDebugContext Names;
  sdk::LocalTypesDebugContext Context(Names, Library);
  const auto Signature = Context.resolvePrototype("ReadText");
  ASSERT_TRUE(Signature);
  EXPECT_EQ(Signature->CallConv, DebugCallConv::Stdcall);
  EXPECT_EQ(Signature->PortablePrototypeDeclaration,
            "const uint16_t *ReadText(const uint16_t *const *Input, volatile "
            "uint32_t *Output)");
  EXPECT_EQ(Signature->PortableParameterTypes,
            (std::vector<std::string>{"const uint16_t *const *",
                                      "volatile uint32_t *"}));
  const auto Stop = Context.resolvePrototype("Stop");
  ASSERT_TRUE(Stop);
  EXPECT_TRUE(Stop->NoReturn);
  EXPECT_EQ(Stop->PortablePrototypeDeclaration, "_Noreturn void Stop(void)");
}

TEST(LocalTypesDebugContext,
     CallbackBorrowsKeepIdentityWithoutRecursiveLayouts) {
  using namespace neverd;
  for (const auto Model : {types::DataModel::ILP32, types::DataModel::LLP64}) {
    auto Library = std::make_shared<types::TypeLibrary>(Model);
    ASSERT_TRUE(types::parseCTypes(R"c(
      struct Entry;
      typedef void (*Observer)(const struct Entry *, volatile unsigned int *);
      struct Entry { Observer Notify; struct Entry *Next; };
      void Watch(Observer Notify, const struct Entry *Input);
    )c",
                                   *Library)
                    .ok());
    NamesOnlyDebugContext Names;
    sdk::LocalTypesDebugContext Context(Names, Library);
    const auto Function = Context.resolvePrototype("Watch");
    ASSERT_TRUE(Function);
    ASSERT_EQ(Function->Params.size(), 2u);
    const auto Callback = Function->Params[0].second->Pointee;
    ASSERT_EQ(Callback->Kind, NdTypeKind::Func);
    ASSERT_EQ(Callback->ParamTypes.size(), 2u);
    const auto Borrow = Callback->ParamTypes[0]->Pointee;
    EXPECT_EQ(Borrow->SourceName, "Entry");
    EXPECT_EQ(Borrow->Size, 0);
    EXPECT_TRUE(Borrow->Fields.empty());
    EXPECT_TRUE(Borrow->FieldDisplayTypes.empty());
    ASSERT_EQ(Function->PortableParameterTypes.size(), 2u);
    EXPECT_EQ(Function->PortableParameterTypes[0],
              "void (*)(const struct Entry *, volatile uint32_t *)");
    EXPECT_EQ(Function->PortableParameterTypes[1], "const struct Entry *");
  }
}

TEST(LocalTypesDebugContext, RecursiveRecordBorrowsReleaseAndComplete) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>(types::DataModel::LLP64);
  ASSERT_TRUE(types::parseCTypes(R"c(
    struct Left;
    struct Right { struct Left *Back; unsigned Tag; };
    struct Left { struct Right *Next; struct Left *Self; unsigned Value; };
    struct Left Read(void);
  )c",
                                 *Library)
                  .ok());
  const auto *Prototype = Library->findPrototype("Read");
  ASSERT_TRUE(Prototype);
  std::weak_ptr<NdType> RootLifetime, BorrowLifetime;
  {
    const auto Root = sdk::decompilerTypeOf(*Library, *Prototype->Return);
    ASSERT_TRUE(Root);
    ASSERT_EQ(Root->FieldDisplayTypes.size(), 3u);
    const auto Right = Root->FieldDisplayTypes[0]->Pointee;
    ASSERT_TRUE(Right);
    ASSERT_EQ(Right->FieldDisplayTypes.size(), 2u);
    const auto Back = Right->FieldDisplayTypes[0]->Pointee;
    ASSERT_TRUE(Back);
    EXPECT_EQ(Back->SourceName, "Left");
    EXPECT_EQ(Back->Size, 0u);
    EXPECT_TRUE(Back->FieldDisplayTypes.empty());
    EXPECT_NE(Back.get(), Root.get());
    EXPECT_NE(Root->FieldDisplayTypes[1]->Pointee.get(), Root.get());
    RootLifetime = Root;
    BorrowLifetime = Back;
    NamesOnlyDebugContext Names;
    sdk::LocalTypesDebugContext Context(Names, Library);
    Context.completeType(Back);
    EXPECT_EQ(Back->FieldDisplayNames,
              (std::vector<std::string>{"Next", "Self", "Value"}));
  }
  EXPECT_TRUE(RootLifetime.expired());
  EXPECT_TRUE(BorrowLifetime.expired());
}

namespace {

/// A library whose records reach each other through pointers, as a kernel's
/// do, and hold a record in place.
std::shared_ptr<neverd::types::TypeLibrary> recursiveRecordLibrary() {
  auto Library = std::make_shared<neverd::types::TypeLibrary>(
      neverd::types::DataModel::LLP64);
  EXPECT_TRUE(neverd::types::parseCTypes(R"c(
    struct Left;
    struct Inner { unsigned short Kind; unsigned short Size; };
    struct Right { struct Left *Back; struct Inner Header; unsigned Tag; };
    struct Left { struct Right *Next; struct Left *Self; unsigned Value; };
  )c",
                                         *Library)
                  .ok());
  return Library;
}

/// The display members of \p Type, and of the records it reaches, to
/// \p Depth records: names, offsets and the named records they point to.
std::string displayLayout(const neverd::TypeRef &Type, unsigned Depth) {
  if (!Type)
    return "null";
  std::string Out = Type->SourceName + "{";
  for (size_t I = 0; I < Type->FieldDisplayNames.size(); ++I) {
    Out += Type->FieldDisplayNames[I] + "@" +
           std::to_string(Type->FieldDisplayOffsets[I]) + ":";
    neverd::TypeRef Member = Type->FieldDisplayTypes[I];
    while (Member && Member->Kind == neverd::NdTypeKind::Ptr) {
      Out += "*";
      Member = Member->Pointee;
    }
    if (Member && Member->Kind == neverd::NdTypeKind::Struct && Depth)
      Out += displayLayout(Member, Depth - 1);
    else if (Member)
      Out += Member->SourceName.empty() ? std::to_string(Member->Size)
                                        : Member->SourceName;
    Out += ";";
  }
  return Out + "}";
}

} // namespace

TEST(LocalTypesDebugContext, RecordsOfOneNameShareTheirLibraryMembers) {
  // Each record the debug information gives of a library name takes the
  // library's members, converted once: two of them read the same members,
  // and completing those changes nothing.
  using namespace neverd;
  const auto Library = recursiveRecordLibrary();
  NamesOnlyDebugContext Names;
  sdk::LocalTypesDebugContext Context(Names, Library);
  const TypeRef First = NdType::makeNamedRecord("Left", 0);
  const TypeRef Second = NdType::makeNamedRecord("Left", 0);
  Context.completeType(First);
  Context.completeType(Second);
  EXPECT_EQ(First->FieldDisplayNames,
            (std::vector<std::string>{"Next", "Self", "Value"}));
  EXPECT_EQ(First->FieldDisplayTypes, Second->FieldDisplayTypes);
  const TypeRef Right = First->FieldDisplayTypes[0]->Pointee;
  ASSERT_TRUE(Right);
  EXPECT_EQ(Right->FieldDisplayNames,
            (std::vector<std::string>{"Back", "Header", "Tag"}));
  // A record a member reaches is complete as converted, and stays so.
  const std::string Before = displayLayout(First, 4);
  Context.completeType(Right);
  Context.completeType(Right->FieldDisplayTypes[0]);
  Context.completeType(Right->FieldDisplayTypes[1]);
  EXPECT_EQ(displayLayout(First, 4), Before);
  EXPECT_EQ(Right->FieldDisplayTypes[0]->Pointee->FieldDisplayNames,
            (std::vector<std::string>{"Next", "Self", "Value"}));
}

TEST(LocalTypesDebugContext, CompletedRecordsReadTheSameInEitherOrder) {
  // What a completed record reads, to any depth, is a function of the
  // library and its name: never of what the context completed before.
  using namespace neverd;
  const auto Library = recursiveRecordLibrary();
  std::vector<std::string> Layouts[2];
  for (unsigned Order = 0; Order < 2; ++Order) {
    NamesOnlyDebugContext Names;
    sdk::LocalTypesDebugContext Context(Names, Library);
    std::vector<TypeRef> Records{NdType::makeNamedRecord("Left", 0),
                                 NdType::makeNamedRecord("Right", 0),
                                 NdType::makeNamedRecord("Inner", 0)};
    if (Order)
      std::reverse(Records.begin(), Records.end());
    for (const TypeRef &Record : Records)
      Context.completeType(Record);
    // Members completed through their parents as the writer does.
    for (const TypeRef &Record : Records)
      for (const TypeRef &Member : Record->FieldDisplayTypes)
        Context.completeType(Member);
    if (Order)
      std::reverse(Records.begin(), Records.end());
    for (const TypeRef &Record : Records)
      Layouts[Order].push_back(displayLayout(Record, 5));
  }
  EXPECT_EQ(Layouts[0], Layouts[1]);
  EXPECT_NE(Layouts[0][0].find("Header@8:Inner{Kind@0:"), std::string::npos)
      << Layouts[0][0];
}

TEST(LocalTypesDebugContext, CompletedTypesLiveAsLongAsTheContextNamesThem) {
  // A type the writer copied for one unit and completed is held while the
  // context remembers it as completed: another type allocated where it was
  // must not read as completed.  The shared records go with the context.
  using namespace neverd;
  const auto Library = recursiveRecordLibrary();
  std::weak_ptr<NdType> Copy, Shared;
  {
    NamesOnlyDebugContext Names;
    sdk::LocalTypesDebugContext Context(Names, Library);
    const TypeRef Record = NdType::makeNamedRecord("Left", 0);
    Context.completeType(Record);
    {
      // The first copy is also the variant of its name the debug
      // information gave; the second reads as it does, and nothing else
      // holds it.
      auto First = std::make_shared<NdType>(*Record);
      auto Second = std::make_shared<NdType>(*Record);
      Context.completeType(First);
      Context.completeType(Second);
      Copy = Second;
      Shared = Record->FieldDisplayTypes[0]->Pointee;
    }
    EXPECT_FALSE(Copy.expired());
    EXPECT_FALSE(Shared.expired());
  }
  EXPECT_TRUE(Copy.expired());
  EXPECT_TRUE(Shared.expired());
}

TEST(LocalTypesDebugContext, PortablePrototypesRetainIntegerKeywordIdentity) {
  using namespace neverd;
  for (auto Model : {types::DataModel::ILP32, types::DataModel::LLP64,
                     types::DataModel::LP64}) {
    auto Library = std::make_shared<types::TypeLibrary>(Model);
    ASSERT_TRUE(types::parseCTypes(R"(
      typedef unsigned long DWORD;
      typedef enum { OpFirst = 1, OpSecond = 2 } Operation;
      double Scalar(DWORD Flags, Operation Op, double Value, long *Output,
                    const long long *Wide);
    )",
                                   *Library)
                    .ok());
    NamesOnlyDebugContext Names;
    sdk::LocalTypesDebugContext Context(Names, Library);
    const auto Signature = Context.resolvePrototype("Scalar");
    ASSERT_TRUE(Signature);
    const std::string Bytes = Model == types::DataModel::LP64 ? "8" : "4";
    const std::string Bits = Model == types::DataModel::LP64 ? "64" : "32";
    EXPECT_NE(Signature->PortablePrototypeDeclaration.find(
                  "__typeof__(__builtin_choose_expr(sizeof(unsigned long) == " +
                  Bytes + ", (unsigned long)0, (uint" + Bits + "_t)0)) Flags"),
              std::string::npos)
        << Signature->PortablePrototypeDeclaration;
    EXPECT_NE(Signature->PortablePrototypeDeclaration.find("Operation Op"),
              std::string::npos);
    EXPECT_EQ(Signature->Params[0].second->Size,
              Model == types::DataModel::LP64 ? 8u : 4u);
    // Call conversions must use the declaration's C identity too. Equal
    // widths do not make long *, long long * and int64_t * compatible.
    EXPECT_NE(Signature->Params[3].second->Pointee->SourceName.find(
                  "__typeof__(__builtin_choose_expr(sizeof(long) == " + Bytes),
              std::string::npos);
    EXPECT_NE(Signature->Params[4].second->Pointee->SourceName.find(
                  "__typeof__(__builtin_choose_expr(sizeof(long long) == 8"),
              std::string::npos);
    EXPECT_EQ(Context.resolvePrototype("Scalar")->PortablePrototypeDeclaration,
              Signature->PortablePrototypeDeclaration);
  }
}

TEST(LocalTypesDebugContext, RenderedDefinitionsReadTheSameInEveryOrder) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>(types::DataModel::LLP64);
  ASSERT_TRUE(types::parseCTypes(R"(
    struct Inner { int Value; };
    struct Pair { struct Inner Left; long long Right; };
    struct Other { short Count; };
  )",
                                 *Library)
                  .ok());
  const auto Pair = NdType::makeNamedRecord("Pair");
  const auto Inner = NdType::makeNamedRecord("Inner");
  const auto Other = NdType::makeNamedRecord("Other");
  // Two units that name the same first record and differ after it, then the
  // first again: each reads as a context that defined it alone does.
  const std::vector<std::vector<TypeRef>> Units = {
      {Pair, Inner}, {Pair, Other}, {Pair, Inner}, {Other}};
  NamesOnlyDebugContext Names;
  sdk::LocalTypesDebugContext Context(Names, Library);
  for (const auto &Unit : Units) {
    const CRecordDefinitions Defined = Context.definitions(Unit);
    NamesOnlyDebugContext FreshNames;
    sdk::LocalTypesDebugContext Fresh(FreshNames, Library);
    const CRecordDefinitions Alone = Fresh.definitions(Unit);
    EXPECT_FALSE(Defined.Text.empty());
    EXPECT_EQ(Defined.Text, Alone.Text);
    EXPECT_EQ(Defined.Records, Alone.Records);
    EXPECT_EQ(Defined.Tags, Alone.Tags);
    EXPECT_EQ(Defined.Types, Alone.Types);
    EXPECT_EQ(Defined.Identifiers, Alone.Identifiers);
    EXPECT_EQ(Defined.DeclaredTypes, Alone.DeclaredTypes);
    EXPECT_EQ(Defined.Enumerations, Alone.Enumerations);
    for (const auto &Name : Fresh.readTypes())
      EXPECT_TRUE(Context.readTypes().contains(Name.getKey()))
          << Name.getKey().str();
  }
  EXPECT_NE(Context.definitions({Pair, Other}).Text.find("Count"),
            std::string::npos);
}

TEST(LocalTypesDebugContext, PointerDeclarationsDoNotRequireRecordLayouts) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>(types::DataModel::LLP64);
  ASSERT_TRUE(types::parseCTypes(R"(
    struct Item { int Value; };
    union Choice { int Number; void *Pointer; };
  )",
                                 *Library)
                  .ok());
  NamesOnlyDebugContext Names;
  sdk::LocalTypesDebugContext Context(Names, Library);
  const auto Item = NdType::makeNamedRecord("Item");
  const auto Choice = NdType::makeNamedRecord("Choice");
  const auto ItemPointer = NdType::makePtr(Item);
  const auto ChoicePointer = NdType::makePtr(Choice);
  const auto Declarations = Context.definitions({ItemPointer, ChoicePointer});
  EXPECT_TRUE(Declarations.Text.empty());
  EXPECT_TRUE(Declarations.Records.empty());
  ASSERT_EQ(Declarations.Tags.size(), 2u);
  EXPECT_FALSE(Declarations.Tags.at("Item"));
  EXPECT_TRUE(Declarations.Tags.at("Choice"));
  EXPECT_TRUE(Context.readTypes().contains("Item"));
  EXPECT_TRUE(Context.readTypes().contains("Choice"));
  // Requiring a value or a member layout wins regardless of query order.
  for (const auto &Requests :
       {std::vector<TypeRef>{ItemPointer, ChoicePointer, Item, Choice},
        std::vector<TypeRef>{Item, Choice, ItemPointer, ChoicePointer}}) {
    const auto Definitions = Context.definitions(Requests);
    EXPECT_NE(Definitions.Text.find("int32_t Value"), std::string::npos);
    EXPECT_EQ(Definitions.Records.size(), 2u);
    EXPECT_FALSE(Definitions.Records.at("Item"));
    EXPECT_TRUE(Definitions.Records.at("Choice"));
  }
}

TEST(LocalTypesDebugContext, TypesTheFunctionsDebugInformationLeavesUntyped) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>();
  ASSERT_TRUE(types::parseCTypes(R"(
    typedef LONG NTSTATUS;
    typedef struct _PAIR { ULONG Low; union { LONG High; ULONG Top; }; } PAIR,
        *PPAIR;
    typedef VOID (NTAPI *PCALLBACK)(PPAIR Pair, ULONG Flags);
    NTSTATUS NTAPI Pair(PPAIR Out, ULONG Flags, PCALLBACK Done);
    PPAIR NTAPI NextPair(PPAIR Current);
    ULONG __cdecl Print(const char *Format, ...);
  )",
                                 *Library)
                  .ok());
  NamesOnlyDebugContext Names;
  const auto name = [&](va_t Addr, const char *Name) {
    FunctionSym Function;
    Function.Name = Name;
    Function.Addr = Addr;
    Function.Size = 0x10;
    Names.Functions[Addr] = Function;
  };
  name(0x1000, "Pair");
  name(0x2000, "NextPair");
  name(0x3000, "Print");
  name(0x4000, "Unknown");
  FunctionSym Typed;
  Typed.Name = "Pair";
  Typed.Addr = 0x5000;
  Typed.Size = 0x10;
  Typed.ReturnType = NdType::makeInt(8, false);
  Names.Functions[0x5000] = Typed;
  sdk::LocalTypesDebugContext Context(Names, Library);

  std::optional<FunctionSym> Pair = Context.resolveFunction(0x1000);
  ASSERT_TRUE(Pair.has_value());
  ASSERT_NE(Pair->ReturnType, nullptr);
  EXPECT_EQ(Pair->ReturnType->Kind, NdTypeKind::Int);
  EXPECT_EQ(Pair->ReturnType->Size, 4u);
  EXPECT_TRUE(Pair->ReturnType->IsSigned);
  ASSERT_EQ(Pair->Params.size(), 3u);
  EXPECT_EQ(Pair->Params[0].first, "Out");
  // The record pointed to has its name and its members' names, those of
  // the union in place among them; it is C's.
  const TypeRef &Record = Pair->Params[0].second->Pointee;
  EXPECT_EQ(Record->SourceName, "_PAIR");
  EXPECT_EQ(Record->Size, 8u);
  EXPECT_EQ(Record->Passing, NdRecordPassing::ByValue);
  EXPECT_EQ(Record->FieldDisplayNames,
            (std::vector<std::string>{"Low", "High", "Top"}));
  EXPECT_EQ(Record->FieldDisplayOffsets, (std::vector<uint16_t>{0, 4, 4}));
  // Callback pointer parameters retain their named borrowed records.
  const TypeRef &Done = Pair->Params[2].second;
  ASSERT_EQ(Done->Kind, NdTypeKind::Ptr);
  ASSERT_EQ(Done->Pointee->Kind, NdTypeKind::Func);
  ASSERT_EQ(Done->Pointee->ParamTypes.size(), 2u);
  EXPECT_EQ(Done->Pointee->ParamTypes[0]->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Done->Pointee->ParamTypes[0]->Pointee->Kind, NdTypeKind::Struct);
  EXPECT_EQ(Done->Pointee->ParamTypes[0]->Pointee->SourceName, "_PAIR");
  EXPECT_EQ(Pair->PortableParameterTypes,
            Context.resolvePrototype("Pair")->PortableParameterTypes);

  // A record returned by a C declaration's pointer is no hidden result.
  std::optional<FunctionSym> Next = Context.resolveFunction(0x2000);
  ASSERT_TRUE(Next.has_value());
  EXPECT_FALSE(
      isMsvcIndirectReturn(Next->ReturnType, Arch::X64, BinaryFormat::COFF));
  // A variadic prototype retains its fixed prefix and its open tail.
  ASSERT_TRUE(Context.resolvePrototype("Print"));
  EXPECT_TRUE(Context.resolvePrototype("Print")->IsVariadic);
  EXPECT_EQ(Context.resolvePrototype("Print")->PortablePrototypeDeclaration,
            "__typeof__(__builtin_choose_expr(sizeof(unsigned long) == 4, "
            "(unsigned long)0, (uint32_t)0)) Print(const char *Format, ...)");
  EXPECT_EQ(Context.resolvePrototype("Print")->Params.size(), 1u);
  EXPECT_TRUE(Context.resolveFunction(0x3000)->IsVariadic);
  EXPECT_EQ(Context.resolveFunction(0x3000)->Params.size(), 1u);
  // A name without a prototype, an interior address and a typed declaration
  // stay.
  EXPECT_TRUE(Context.resolveFunction(0x4000)->Params.empty());
  EXPECT_TRUE(Context.resolveFunction(0x1008)->Params.empty());
  EXPECT_TRUE(Context.resolveFunction(0x5000)->Params.empty());
  EXPECT_EQ(Context.resolveFunction(0x5000)->ReturnType->Size, 8u);
}

TEST(LocalTypesDebugContext, TypesTheFunctionsByTheNamesTheyAreShownUnder) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>();
  ASSERT_TRUE(types::parseCTypes(R"(
    typedef struct _THREAD *PTHREAD;
    typedef struct _PROCESS *PPROCESS;
    PPROCESS NTAPI ThreadToProcess(PTHREAD Thread);
    ULONG NTAPI Exported(ULONG Value);
    ULONG NTAPI Stated(ULONG Value, ULONG Shift);
    ULONG NTAPI Analyzed(ULONG Value);
    ULONG NTAPI Folded(ULONG Value, void *Context);
    ULONG NTAPI Debugged(ULONG Value);
    ULONG NTAPI Fewer(ULONG Value);
    ULONG NTAPI More(ULONG Value, ULONG Extra);
    LONG NTAPI Signed(PTHREAD Thread);
    ULONG NTAPI Unsigned(PPROCESS Process);
    ULONG NTAPI Recognized(ULONG Value, ULONG Other);
  )",
                                 *Library)
                  .ok());
  BinaryImage Image;
  // A linker that folds identical functions exports one entry under each
  // of their names: the first names the function shown there.
  Image.Exports = {{"ThreadToProcess", 1, 0x1000}, {"Unprototyped", 2, 0x2000},
                   {"Folded", 3, 0x2000},          {"Exported", 4, 0x6000},
                   {"Fewer", 5, 0x7000},           {"More", 6, 0x7000},
                   {"Signed", 7, 0x8000},          {"Unsigned", 8, 0x8000}};
  const auto symbol = [&](const char *Name, va_t Addr, NameOrigin Origin) {
    Symbol Sym;
    Sym.Name = Name;
    Sym.Addr = Addr;
    Sym.IsFunc = true;
    Sym.Origin = Origin;
    Image.Symbols.push_back(Sym);
  };
  symbol("Stated", 0x3000, NameOrigin::Stated);
  symbol("Analyzed", 0x4000, NameOrigin::Analysis);
  NamesOnlyDebugContext Names;
  const auto name = [&](va_t Addr, const char *Name) {
    FunctionSym Function;
    Function.Name = Name;
    Function.Addr = Addr;
    Function.Size = 0x10;
    Names.Functions[Addr] = Function;
  };
  name(0x5000, "Debugged");
  name(0x6000, "Debugged");
  // Signatures recognize what nothing else names; a stated name comes first.
  sdk::LocalTypesDebugContext Context(
      Names, Library, {}, &Image, {},
      {{0x9000, "Recognized"}, {0x1000, "Recognized"}});

  // An export the debug information does not name takes its prototype,
  // with what declares it.
  std::optional<FunctionSym> Owner = Context.resolveFunction(0x1000);
  ASSERT_TRUE(Owner.has_value());
  EXPECT_EQ(Owner->Addr, 0x1000u);
  ASSERT_EQ(Owner->Params.size(), 1u);
  EXPECT_EQ(Owner->Params[0].first, "Thread");
  ASSERT_NE(Owner->ReturnType, nullptr);
  EXPECT_EQ(Owner->ReturnType->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Owner->PrototypeLibrary, "local-types");
  EXPECT_EQ(Owner->PrototypeDeclaration,
            "PPROCESS ThreadToProcess(PTHREAD Thread)");
  // The function folded with another is shown under its first name, which
  // has no prototype: the other name's would describe another function.
  EXPECT_FALSE(Context.resolveFunction(0x2000).has_value());
  // A symbol the program states names a function as an export does; one
  // that analysis made up does not.
  ASSERT_TRUE(Context.resolveFunction(0x3000).has_value());
  EXPECT_EQ(Context.resolveFunction(0x3000)->Params.size(), 2u);
  EXPECT_FALSE(Context.resolveFunction(0x4000).has_value());
  // Where the image states no name, the debug information's name does; the
  // image's comes first, as the function is shown under it.
  std::optional<FunctionSym> Debugged = Context.resolveFunction(0x5000);
  ASSERT_TRUE(Debugged.has_value());
  EXPECT_EQ(Debugged->Name, "Debugged");
  EXPECT_EQ(Debugged->Size, 0x10u);
  EXPECT_EQ(Debugged->Params.size(), 1u);
  std::optional<FunctionSym> Exported = Context.resolveFunction(0x6000);
  ASSERT_TRUE(Exported.has_value());
  // A builtin, where no header names it, in exact widths.
  EXPECT_EQ(Exported->PrototypeDeclaration,
            "uint32_t Exported(uint32_t Value)");
  // An address inside a function names no function of its own.
  EXPECT_TRUE(Context.resolveFunction(0x5008)->Params.empty());
  // Folded functions whose prototypes take different parameters: which the
  // code reads is unknown, and so it takes neither.
  EXPECT_FALSE(Context.resolveFunction(0x7000).has_value());
  // Ones that pass and return alike, whatever they point at or sign: the
  // shown name's.
  std::optional<FunctionSym> Signed = Context.resolveFunction(0x8000);
  ASSERT_TRUE(Signed.has_value());
  ASSERT_EQ(Signed->Params.size(), 1u);
  EXPECT_EQ(Signed->Params[0].first, "Thread");
  EXPECT_TRUE(Signed->ReturnType->IsSigned);
  std::optional<FunctionSym> Recognized = Context.resolveFunction(0x9000);
  ASSERT_TRUE(Recognized.has_value());
  EXPECT_EQ(Recognized->Params.size(), 2u);
}

TEST(LocalTypesDebugContext, RecordParametersRetainCompleteABILayout) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>(types::DataModel::LP64);
  ASSERT_TRUE(types::parseCTypes(R"(
    typedef struct { char *key; void *data; } ENTRY;
    typedef struct { unsigned int bits : 3; int value; } BITS;
    void consume(ENTRY item, BITS flags);
  )",
                                 *Library)
                  .ok());
  NamesOnlyDebugContext Names;
  sdk::LocalTypesDebugContext Context(Names, Library);
  const auto Function = Context.resolvePrototype("consume");
  ASSERT_TRUE(Function);
  ASSERT_EQ(Function->Params.size(), 2u);
  const auto &Entry = Function->Params[0].second;
  ASSERT_TRUE(Entry);
  EXPECT_EQ(Entry->Alignment, 8u);
  ASSERT_EQ(Entry->ScalarLeaves.size(), 2u);
  for (unsigned I = 0; I < 2; ++I) {
    EXPECT_EQ(Entry->ScalarLeaves[I].Offset, I * 8u);
    EXPECT_EQ(Entry->ScalarLeaves[I].Size, 8u);
    EXPECT_FALSE(Entry->ScalarLeaves[I].Floating);
  }
  EXPECT_TRUE(Function->Params[1].second->ScalarLeaves.empty());
}

TEST(LocalTypesDebugContext, NamesTheHeaderLibraryAPrototypeComesFrom) {
  using namespace neverd;
  const types::TypeLibrary *Kernel =
      types::headerLibrary(types::HeaderLibrary::WindowsKernelX64);
  ASSERT_NE(Kernel, nullptr);
  auto Library = std::make_shared<types::TypeLibrary>(Kernel->dataModel());
  types::mergeHeaderTypes(*Library, *Kernel);
  BinaryImage Image;
  Image.Exports = {{"IoThreadToProcess", 1, 0x1000}};
  NamesOnlyDebugContext Names;
  sdk::LocalTypesDebugContext Context(Names, Library, {}, &Image,
                                      {types::HeaderLibrary::WindowsKernelX64});
  const auto ABI = Context.resolvePrototypeABI("IoThreadToProcess");
  ASSERT_TRUE(ABI);
  std::optional<FunctionSym> Owner = Context.resolveFunction(0x1000);
  ASSERT_TRUE(Owner.has_value());
  EXPECT_EQ(Owner->PrototypeLibrary, "windows-kernel-x64");
  EXPECT_EQ(ABI->PrototypeLibrary, Owner->PrototypeLibrary);
  EXPECT_EQ(ABI->PrototypeFromHeader, Owner->PrototypeFromHeader);
  EXPECT_EQ(ABI->PrototypeFromCLibrary, Owner->PrototypeFromCLibrary);
  EXPECT_TRUE(sdk::passesAlike(*ABI, *Owner));
  EXPECT_TRUE(Owner->PrototypeFromHeader);
  EXPECT_FALSE(Owner->PrototypeFromCLibrary);
  EXPECT_EQ(Owner->PrototypeDeclaration,
            "PEPROCESS IoThreadToProcess(PETHREAD Thread)");
  ASSERT_EQ(Owner->Params.size(), 1u);
  EXPECT_EQ(Owner->Params[0].first, "Thread");
}

TEST(LocalTypesDebugContext, GivesTheDebugInformationsRecordsTheirMembers) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>();
  ASSERT_TRUE(types::parseCTypes(R"(
    typedef union _LARGE {
      struct { ULONG Low; LONG High; };
      struct { ULONG Low; LONG High; } u;
      LONGLONG Quad;
    } LARGE;
    typedef struct _HEADER {
      union {
        struct { UCHAR Type; UCHAR Absolute; };
        struct { UCHAR TimerType; UCHAR TimerFlags; };
        LONG Lock;
      };
      ULONG Signal;
    } HEADER;
    typedef struct _HOLDER { HEADER Header; LARGE Time; void *Next; } HOLDER;
  )",
                                 *Library)
                  .ok());
  NamesOnlyDebugContext Names;
  auto Context = std::make_unique<sdk::LocalTypesDebugContext>(Names, Library);
  // A record the debug information names but gives no members takes the
  // library's.
  const TypeRef Holder = NdType::makeNamedRecord("_HOLDER", 24);
  Context->completeType(NdType::makePtr(Holder));
  ASSERT_FALSE(Holder->FieldDisplayNames.empty());
  // An eight-byte read of the union in place is its scalar, not the record
  // of the same size; a byte of the header names no one member of the
  // union's alternatives, and is no header.
  EXPECT_EQ(Holder->displayFieldPathAt(8, 8), "Time.Quad");
  EXPECT_EQ(Holder->displayFieldPathAt(0, 1), std::nullopt);
  EXPECT_EQ(Holder->displayFieldPathAt(4, 4), "Header.Signal");
  EXPECT_EQ(Holder->displayFieldPathAt(16, 8), "Next");
  EXPECT_TRUE(Context->readTypes().contains("_HOLDER"));

  // A record of another size stays as it is.
  const TypeRef Smaller = NdType::makeNamedRecord("_HEADER", 4);
  Context->completeType(Smaller);
  EXPECT_TRUE(Smaller->FieldDisplayNames.empty());
  // Members the debug information gave where the library's lie take the
  // library's names, as those the user renamed there do; members that lie
  // where the library's do not, as a C++ base's flattened into its class,
  // stay.
  const TypeRef Own = NdType::makeNamedRecord("_LARGE", 8);
  Own->FieldDisplayNames = {"Theirs"};
  Own->FieldDisplayOffsets = {0};
  Own->FieldDisplayTypes = {NdType::makeInt(8, true)};
  Context->completeType(Own);
  EXPECT_EQ(Own->displayFieldPathAt(0, 8), "Quad");
  EXPECT_EQ(Own->displayFieldPathAt(4, 4), "High");
  const TypeRef Elsewhere = NdType::makeNamedRecord("_LARGE", 8);
  Elsewhere->FieldDisplayNames = {"Theirs"};
  Elsewhere->FieldDisplayOffsets = {2};
  Elsewhere->FieldDisplayTypes = {NdType::makeInt(2, true)};
  Context->completeType(Elsewhere);
  EXPECT_EQ(Elsewhere->FieldDisplayNames, std::vector<std::string>{"Theirs"});

  // A unit holds the records whose members it names as the library does,
  // and what they hold by value; one named otherwise stays declared.
  const CRecordDefinitions Defined = Context->definitions({Holder, Own});
  EXPECT_NE(Defined.Text.find("struct _HOLDER\n{"), std::string::npos)
      << Defined.Text;
  EXPECT_NE(Defined.Text.find("#ifndef NEVERD_TYPE__HOLDER"),
            std::string::npos);
  EXPECT_EQ(Defined.Records.count("_HOLDER"), 1u);
  EXPECT_TRUE(Defined.Records.at("_LARGE")) << "held by value, a union";
  const CRecordDefinitions Otherwise = Context->definitions({Elsewhere});
  EXPECT_TRUE(Otherwise.Text.empty());

  // The context takes back what it gave, for the next to give anew.
  Context.reset();
  EXPECT_TRUE(Holder->FieldDisplayNames.empty());
  EXPECT_EQ(Own->FieldDisplayNames, std::vector<std::string>{"Theirs"});
  EXPECT_EQ(Own->FieldDisplayOffsets, std::vector<uint16_t>{0});
}

TEST(LocalTypesDebugContext, MatchingNamesStillCompleteTheMemberTypes) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>();
  ASSERT_TRUE(types::parseCTypes(R"(
    struct STATUS_LIKE { LONG Status; ULONGLONG Information; };
    union ASSOCIATED_LIKE { void *Buffer; ULONGLONG Bits; };
    struct IRP_LIKE {
      union ASSOCIATED_LIKE Associated;
      struct STATUS_LIKE Status;
      struct STATUS_LIKE *User;
    };
  )",
                                 *Library)
                  .ok());
  NamesOnlyDebugContext Names;
  auto Context = std::make_unique<sdk::LocalTypesDebugContext>(Names, Library);
  const TypeRef Record = NdType::makeNamedRecord("IRP_LIKE", 32);
  Record->FieldDisplayNames = {"Associated", "Status", "User"};
  Record->FieldDisplayOffsets = {0, 8, 24};
  // A bounded traversal may already have reached these names while leaving
  // the member types absent or shallow. Equal names are not complete types.
  const std::vector<TypeRef> Original = {
      nullptr, NdType::makeNamedRecord("STATUS_LIKE", 16),
      NdType::makePtr(NdType::makeVoid())};
  Record->FieldDisplayTypes = Original;
  Context->completeType(Record);
  EXPECT_EQ(
      Record->displayFieldPathAt(0, 8, true, true, NdAccessClass::Integer),
      "Associated.Bits");
  EXPECT_EQ(Record->displayFieldPathAt(8, 4), "Status.Status");
  const auto Member = Record->displayFieldTypeAt(24, 8);
  ASSERT_TRUE(Member);
  ASSERT_EQ(Member->Kind, NdTypeKind::Ptr);
  ASSERT_TRUE(Member->Pointee);
  EXPECT_EQ(Member->Pointee->SourceName, "STATUS_LIKE");
  const auto Completed = Record->FieldDisplayTypes;
  Context->completeType(Record);
  EXPECT_EQ(Record->FieldDisplayTypes, Completed);
  // Completion belongs to this context; a later context gets the original
  // debug graph, just as it does when the library changes member names.
  Context.reset();
  EXPECT_EQ(Record->FieldDisplayTypes, Original);
}

TEST(LocalTypesDebugContext, FlexibleArrayMembersHoldTheirElements) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>();
  ASSERT_TRUE(types::parseCTypes(R"(
    struct fde_vector {
      const void *orig_data;
      unsigned long long count;
      const void *array[];
    };
  )",
                                 *Library)
                  .ok());
  NamesOnlyDebugContext Names;
  auto Context = std::make_unique<sdk::LocalTypesDebugContext>(Names, Library);
  // libgcc stores into the first element of the array of no stated length
  // that ends the record: a store names the element, as no array is
  // assignable.
  const TypeRef Vector = NdType::makeNamedRecord("fde_vector", 16);
  Context->completeType(NdType::makePtr(Vector));
  ASSERT_EQ(Vector->FieldDisplayNames.size(), 3u);
  EXPECT_EQ(Vector->displayFieldPathAt(16, 8), "array[0]");
  EXPECT_EQ(Vector->displayFieldPathAt(24, 8), "array[1]");
  EXPECT_EQ(Vector->displayFieldPathAt(8, 8), "count");
}

TEST(LocalTypesDebugContext, AccessesTakeTheUnionAlternativeOfTheirClass) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>();
  ASSERT_TRUE(types::parseCTypes(R"(
    typedef union _VALUE { LONGLONG Integer; double Real; void *Pointer; } VALUE;
    typedef struct _CELL { ULONG Tag; VALUE Value; } CELL;
    typedef union _FAST_REF { void *Object; ULONGLONG Value; } FAST_REF;
    typedef union _TWINS { LONGLONG A; ULONGLONG B; double D; } TWINS;
  )",
                                 *Library)
                  .ok());
  NamesOnlyDebugContext Names;
  auto Context = std::make_unique<sdk::LocalTypesDebugContext>(Names, Library);
  const auto record = [&](const char *Name, uint16_t Size) {
    const TypeRef Record = NdType::makeNamedRecord(Name, Size);
    Context->completeType(NdType::makePtr(Record));
    return Record;
  };
  const auto path = [](const TypeRef &Record, uint64_t Offset,
                       NdAccessClass Class) {
    return Record->displayFieldPathAt(Offset, 8, /*EnterNestedAtZero=*/true,
                                      /*ArrayElements=*/true, Class);
  };
  // Where nothing says which alternative the code holds, an access of the
  // union's width reads the union whole; an integer, a floating value or a
  // pointer the code loads or stores picks the alternative of its class.
  const TypeRef Cell = record("_CELL", 16);
  EXPECT_EQ(Cell->displayFieldPathAt(8, 8), "Value");
  EXPECT_EQ(path(Cell, 8, NdAccessClass::Integer), "Value.Integer");
  EXPECT_EQ(path(Cell, 8, NdAccessClass::Floating), "Value.Real");
  EXPECT_EQ(path(Cell, 8, NdAccessClass::Pointer), "Value.Pointer");
  const TypeRef Real = Cell->displayFieldTypeAt(8, 8, /*ArrayElements=*/true,
                                                NdAccessClass::Floating);
  ASSERT_TRUE(Real);
  EXPECT_EQ(Real->Kind, NdTypeKind::Float);
  // A pointer is an integer of its width to the machine, as an integer is a
  // pointer where the union holds no integer.
  const TypeRef FastRef = record("_FAST_REF", 8);
  EXPECT_EQ(path(FastRef, 0, NdAccessClass::Pointer), "Object");
  EXPECT_EQ(path(FastRef, 0, NdAccessClass::Integer), "Value");
  // Two alternatives of the class stay raw.
  const TypeRef Twins = record("_TWINS", 8);
  EXPECT_EQ(path(Twins, 0, NdAccessClass::Integer), std::nullopt);
  EXPECT_EQ(path(Twins, 0, NdAccessClass::Pointer), std::nullopt);
  EXPECT_EQ(path(Twins, 0, NdAccessClass::Floating), "D");
}

TEST(LocalTypesDebugContext, NamesValuesByTheLibrarysEnumerators) {
  using namespace neverd;
  auto Library = std::make_shared<types::TypeLibrary>();
  ASSERT_TRUE(types::parseCTypes(R"(
    typedef enum _COLOR { Red, Green = 5, Huge = 0x10000, Below = -1 } COLOR;
    typedef struct _PAINT { COLOR Color; ULONG Count; } PAINT;
  )",
                                 *Library)
                  .ok());
  NamesOnlyDebugContext Names;
  auto Context = std::make_unique<sdk::LocalTypesDebugContext>(Names, Library);
  // A record's member of an enumeration is the enumeration, its values
  // named by its enumerators: those a display holds, none negative and
  // none wider than 16 bits.
  const TypeRef Paint = NdType::makeNamedRecord("_PAINT", 8);
  Context->completeType(Paint);
  ASSERT_EQ(Paint->FieldDisplayTypes.size(), 2u);
  const TypeRef Color = Paint->FieldDisplayTypes[0];
  ASSERT_TRUE(Color && Color->IsEnum);
  EXPECT_EQ(Color->SourceName, "_COLOR");
  EXPECT_EQ(Color->Size, 4);
  EXPECT_TRUE(Color->IsSigned);
  EXPECT_EQ(Color->displayFieldNameAt(5), "Green");
  EXPECT_EQ(Color->FieldDisplayNames,
            (std::vector<std::string>{"Red", "Green"}));

  // An enumeration the debug information gave enumerators takes the
  // library's, which the user may have renamed, and gets its own back.
  const TypeRef Given = NdType::makeNamedRecord("_COLOR", 0, /*Enum=*/true);
  Given->FieldDisplayNames = {"Rouge", "Vert"};
  Given->FieldDisplayOffsets = {0, 5};
  Context->completeType(Given);
  EXPECT_EQ(Given->displayFieldNameAt(5), "Green");
  EXPECT_TRUE(Context->readTypes().contains("_COLOR"));
  // A unit holds the enumeration, its enumerators agreeing.
  const CRecordDefinitions Defined = Context->definitions({Given});
  EXPECT_NE(Defined.Text.find("enum _COLOR\n{"), std::string::npos)
      << Defined.Text;
  EXPECT_EQ(Defined.Enumerations, std::vector<std::string>{"_COLOR"});
  Context.reset();
  EXPECT_EQ(Given->FieldDisplayNames,
            (std::vector<std::string>{"Rouge", "Vert"}));
}
