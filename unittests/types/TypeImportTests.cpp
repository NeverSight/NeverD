//===- TypeImportTests.cpp - Independent prototype providers --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preserve recursive provider dependencies and reject incomplete imports.
///
//===----------------------------------------------------------------------===//

#include "neverd/types/Limits.h"
#include "neverd/types/TypeImport.h"
#include "neverd/types/TypeJSON.h"
#include "neverd/types/TypeParse.h"
#include "neverd/types/TypeRender.h"

#include <gtest/gtest.h>

using namespace neverd::types;

namespace {

void parse(TypeLibrary &Library, llvm::StringRef Source) {
  const auto Result = parseCTypes(Source, Library);
  for (const auto &Problem : Result.Problems)
    ADD_FAILURE() << Problem.Message;
}

} // namespace

TEST(TypeImport, EqualSizeDoesNotRebindPointeesAndRecursiveDependencies) {
  for (auto Model : {DataModel::LLP64, DataModel::ILP32}) {
    SCOPED_TRACE(dataModelName(Model).str());
    TypeLibrary Library(Model), Provider(Model);
    const char *Body = R"(
      typedef struct URL { UNIT *Text; unsigned int Length; } URL;
      typedef int (*VISITOR)(URL *Value);
      typedef struct OWNER {
        URL Value;
        union { UNIT *Text; VISITOR Call; };
        struct OWNER *Next;
      } OWNER;
      typedef OWNER ALIAS;
    )";
    parse(Library, std::string("typedef char UNIT;\n") + Body + R"(
      typedef int Wide_UNIT;
      void Wide_URL(void);
      void Existing(ALIAS *Value);
      int Shared(int Count);
      enum Mode { ModeValue = 1 };
    )");
    parse(Provider, std::string("typedef unsigned short UNIT;\n") + Body + R"(
      struct Batch { ALIAS Values[2]; };
      struct Unused { int Value; };
      enum Mode { ModeValue = 3 };
      VISITOR Factory(ALIAS *Value);
      void Added(struct Batch *Value, enum Mode Mode);
      void Shared(void);
    )");
    const auto *OriginalURL = Library.find("URL");
    const auto *OriginalShared = Library.findPrototype("Shared");
    auto Imported = importPrototypes(Library, Provider, "Wide");
    ASSERT_TRUE(bool(Imported)) << llvm::toString(Imported.takeError());
    EXPECT_EQ(Imported->Prototypes,
              (std::vector<std::string>{"Added", "Factory"}));
    EXPECT_EQ(Library.find("URL"), OriginalURL);
    EXPECT_EQ(Library.findPrototype("Shared"), OriginalShared);
    EXPECT_EQ(Library.find("Unused"), nullptr);
    // Names already occupied by either a type or a routine remain intact.
    ASSERT_NE(Library.find("Wide_UNIT_1"), nullptr);
    ASSERT_NE(Library.find("Wide_URL_1"), nullptr);
    EXPECT_EQ(Library.sizeOf(*makeNamed("UNIT", TypeKind::Typedef)), 1u);
    EXPECT_EQ(Library.sizeOf(*makeNamed("Wide_UNIT_1", TypeKind::Typedef)), 2u);
    EXPECT_EQ(Library.find("URL")->Record->Size,
              Library.find("Wide_URL_1")->Record->Size);
    EXPECT_EQ(
        Library.find("Wide_URL_1")->Record->Members[0].Type->Element->Name,
        "Wide_UNIT_1");
    ASSERT_NE(Library.find("Wide_OWNER"), nullptr);
    const auto &Owner = *Library.find("Wide_OWNER")->Record;
    ASSERT_EQ(Owner.Members.size(), 3u);
    EXPECT_EQ(Owner.Members[0].Type->Name, "Wide_URL_1");
    EXPECT_EQ(Owner.Members.back().Type->Element->Name, "Wide_OWNER");
    ASSERT_EQ(Owner.Members[1].Type->Kind, TypeExprKind::Record);
    EXPECT_EQ(Owner.Members[1].Type->Record->Members[0].Type->Element->Name,
              "Wide_UNIT_1");
    ASSERT_NE(Library.find("Batch"), nullptr);
    EXPECT_EQ(Library.find("Batch")->Record->Members[0].Type->Element->Name,
              "Wide_ALIAS");
    EXPECT_EQ(Library.findPrototype("Factory")->Return->Name, "Wide_VISITOR");
    EXPECT_EQ(Library.findPrototype("Factory")->Params[0].Type->Element->Name,
              "Wide_ALIAS");
    ASSERT_NE(Library.find("Mode"), nullptr);
    ASSERT_NE(Library.find("Wide_Mode"), nullptr);
    EXPECT_EQ(Library.find("Mode")->Enum->Values[0].Value, 1u);
    EXPECT_EQ(Library.find("Wide_Mode")->Enum->Values[0].Value, 3u);
    EXPECT_EQ(Provider.find("URL")->Record->Members[0].Type->Element->Name,
              "UNIT");
    // Both providers render together and read back with their own identities.
    RenderOptions Options;
    Options.Prototypes = true;
    Options.Offsets = false;
    TypeLibrary Restored(Model);
    parse(Restored, renderTypes(Library, {}, Options));
    ASSERT_NE(Restored.find("Wide_OWNER"), nullptr);
    EXPECT_EQ(Restored.find("Wide_OWNER")->Record->Size, Owner.Size);
    EXPECT_EQ(
        Restored.find("Wide_URL_1")->Record->Members[0].Type->Element->Name,
        "Wide_UNIT_1");
    ASSERT_NE(Restored.findPrototype("Factory"), nullptr);
    EXPECT_EQ(Restored.findPrototype("Factory")->Return->Name, "Wide_VISITOR");
  }
}

TEST(TypeImport,
     IdenticalDependenciesAreSharedAndOnlyNewPrototypesAreImported) {
  TypeLibrary Library, Provider;
  const char *Prelude = "typedef unsigned int COUNT;\n"
                        "struct Packet { COUNT Length; };\n"
                        "void Existing(struct Packet *Value);\n";
  parse(Library, Prelude);
  parse(Provider, std::string(Prelude) +
                      "void Added(struct Packet *Value, int (*Visit)(COUNT));\n"
                      "struct Unused { char Bytes[1024]; };\n");
  const auto *Packet = Library.find("Packet");
  auto Imported = importPrototypes(Library, Provider, "Provider");
  ASSERT_TRUE(bool(Imported)) << llvm::toString(Imported.takeError());
  EXPECT_TRUE(Imported->Types.empty());
  EXPECT_TRUE(Imported->Renamed.empty());
  EXPECT_EQ(Imported->Prototypes, (std::vector<std::string>{"Added"}));
  EXPECT_EQ(Library.find("Packet"), Packet);
  EXPECT_EQ(Library.find("Unused"), nullptr);
  auto Again = importPrototypes(Library, Provider, "Provider");
  ASSERT_TRUE(bool(Again)) << llvm::toString(Again.takeError());
  EXPECT_TRUE(Again->Types.empty());
  EXPECT_TRUE(Again->Prototypes.empty());
}

TEST(TypeImport, CallbackConventionAndQualifiersArePartOfItsIdentity) {
  TypeLibrary Library(DataModel::ILP32), Provider(DataModel::ILP32);
  parse(Library, "typedef int (__cdecl *CALL)(const int *Value);\n"
                 "void Existing(CALL Visit);\n");
  parse(Provider, "typedef int (__stdcall *CALL)(volatile int *Value);\n"
                  "CALL __fastcall Added(CALL Visit);\n");
  auto Imported = importPrototypes(Library, Provider, "Other");
  ASSERT_TRUE(bool(Imported)) << llvm::toString(Imported.takeError());
  const auto *Added = Library.findPrototype("Added");
  ASSERT_NE(Added, nullptr);
  EXPECT_EQ(Added->Convention, CallingConvention::Fastcall);
  EXPECT_EQ(Added->Return->Name, "Other_CALL");
  EXPECT_EQ(Added->Params[0].Type->Name, "Other_CALL");
  const auto &Callback = *Library.find("Other_CALL")->Target->Element->Function;
  EXPECT_EQ(Callback.Convention, CallingConvention::Stdcall);
  EXPECT_TRUE(Callback.Params[0].Type->Element->Volatile);
  EXPECT_FALSE(Callback.Params[0].Type->Element->Const);
  EXPECT_EQ(Library.find("CALL")->Target->Element->Function->Convention,
            CallingConvention::Cdecl);
}

TEST(TypeImport, ExactSelectionRetainsOnlyItsDependencyClosure) {
  TypeLibrary Library, Provider;
  parse(Library, "int Existing(int Value);");
  parse(Provider, "struct Wide { unsigned short *Text; };\n"
                  "struct Narrow { char *Text; };\n"
                  "void QueryW(struct Wide **Result);\n"
                  "void QueryA(struct Narrow **Result);\n"
                  "void Existing(void);\n");
  const llvm::StringRef Names[] = {"QueryW", "Existing", "QueryW"};
  auto Imported = importPrototypes(Library, Provider, "Wide", Names);
  ASSERT_TRUE(bool(Imported)) << llvm::toString(Imported.takeError());
  EXPECT_EQ(Imported->Prototypes, (std::vector<std::string>{"QueryW"}));
  EXPECT_EQ(Imported->Dependencies, (std::vector<std::string>{"Wide"}));
  EXPECT_NE(Library.find("Wide"), nullptr);
  EXPECT_EQ(Library.find("Narrow"), nullptr);
  EXPECT_EQ(Library.findPrototype("QueryA"), nullptr);
  EXPECT_EQ(Library.findPrototype("Existing")->Params.size(), 1u);

  const llvm::StringRef Missing[] = {"QueryA", "Absent"};
  auto Refused = importPrototypes(Library, Provider, "Wide", Missing);
  ASSERT_FALSE(bool(Refused));
  llvm::consumeError(Refused.takeError());
  EXPECT_EQ(Library.find("Narrow"), nullptr);
  EXPECT_EQ(Library.findPrototype("QueryA"), nullptr);
  auto Empty = importPrototypes(Library, Provider, "Wide",
                                llvm::ArrayRef<llvm::StringRef>{});
  ASSERT_TRUE(bool(Empty)) << llvm::toString(Empty.takeError());
  EXPECT_TRUE(Empty->Prototypes.empty());
  EXPECT_TRUE(Empty->Types.empty());
}

TEST(TypeImport, RefusalsDoNotPartiallyModifyTheDestination) {
  TypeLibrary Library;
  parse(Library, "struct Existing { int Value; }; int Present(void);");
  const auto *Existing = Library.find("Existing");
  const auto *Present = Library.findPrototype("Present");
  const auto refuses = [&](const TypeLibrary &Provider,
                           llvm::StringRef Prefix) {
    auto Result = importPrototypes(Library, Provider, Prefix);
    ASSERT_FALSE(bool(Result));
    llvm::consumeError(Result.takeError());
    EXPECT_EQ(Library.size(), 1u);
    EXPECT_EQ(Library.prototypes().size(), 1u);
    EXPECT_EQ(Library.find("Existing"), Existing);
    EXPECT_EQ(Library.findPrototype("Present"), Present);
  };
  TypeLibrary Provider;
  parse(Provider, "int Good(int Value);");
  refuses(Provider, "invalid-prefix");
  TypeLibrary WrongModel(DataModel::LP64);
  refuses(WrongModel, "Other");
  TypeLibrary WrongABI;
  WrongABI.setRecordABI(RecordABI::Itanium);
  refuses(WrongABI, "Other");
  auto Bad = std::make_shared<FunctionDef>();
  Bad->Return = makeBuiltin(Builtin::Int);
  Bad->Params.push_back({"Value", makeNamed("Missing", TypeKind::Struct)});
  Provider.definePrototype("Bad", Bad);
  refuses(Provider, "Other");
  Bad->Params[0].Type = nullptr;
  refuses(Provider, "Other");
  LocalType MissingDefinition;
  MissingDefinition.Name = "EmptyAlias";
  MissingDefinition.Kind = TypeKind::Typedef;
  Provider.define(std::move(MissingDefinition));
  Bad->Params[0].Type = makeNamed("EmptyAlias", TypeKind::Typedef);
  refuses(Provider, "Other");
  auto Deep = makeBuiltin(Builtin::Int);
  for (unsigned I = 0; I <= neverd::limits::types::kPrototypeTypeImportMaxDepth;
       ++I)
    Deep = makePointer(Deep, 8);
  Bad->Params[0].Type = Deep;
  refuses(Provider, "Other");
}
