//===- TypeConversionCAPITests.cpp - Independent type providers ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise independent declaration imports through the public C interface.
///
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/types/TypeParse.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <gtest/gtest.h>

using namespace neverd::types;

namespace {

llvm::json::Object convert(llvm::json::Array Declarations,
                           bool KeepGoing = false) {
  llvm::json::Object Request{
      {"dataModel", "ILP32"},
      {"recordABI", "microsoft"},
      {"declarations", std::move(Declarations)},
      {"keepGoing", KeepGoing},
      {"render", llvm::json::Object{{"language", "c"}, {"prototypes", true}}}};
  const auto Text =
      llvm::formatv("{0}", llvm::json::Value(std::move(Request))).str();
  const char *Raw = neverd_types_convert(Text.c_str());
  auto Parsed = llvm::json::parse(Raw ? Raw : "");
  neverd_free_string(Raw);
  if (!Parsed) {
    ADD_FAILURE() << llvm::toString(Parsed.takeError());
    return {};
  }
  if (!Parsed->getAsObject()) {
    ADD_FAILURE() << "type conversion did not return an object";
    return {};
  }
  return std::move(*Parsed->getAsObject());
}

llvm::json::Object declaration(llvm::StringRef Source) {
  return llvm::json::Object{{"language", "c"}, {"source", Source.str()}};
}

} // namespace

TEST(SessionCAPITypeConversion, ImportsPreserveBothProvidersAndCallbackABI) {
  auto Primary = declaration(R"(
    typedef char UNIT;
    typedef struct URL { UNIT *Text; } URL;
    void Existing(URL *Value);
  )");
  auto Secondary = declaration(R"(
    typedef unsigned short UNIT;
    typedef struct URL { UNIT *Text; } URL;
    typedef void (__stdcall *CALL)(URL *Value);
    void __stdcall Added(URL *Value, CALL Callback);
    struct Unused { int Value; };
    int Existing(void);
  )");
  Secondary["importPrefix"] = "Wide";
  const auto Reply =
      convert(llvm::json::Array{std::move(Primary), std::move(Secondary)});
  ASSERT_FALSE(Reply.get("error"));
  ASSERT_NE(Reply.getArray("problems"), nullptr);
  EXPECT_TRUE(Reply.getArray("problems")->empty());
  const auto *Imports = Reply.getArray("imports");
  ASSERT_NE(Imports, nullptr);
  ASSERT_EQ(Imports->size(), 1u);
  const auto *Import = (*Imports)[0].getAsObject();
  ASSERT_NE(Import, nullptr);
  EXPECT_EQ(Import->getInteger("declaration"), 1);
  EXPECT_EQ(Import->getString("prefix"), "Wide");
  const auto *Renamed = Import->getArray("renamed");
  ASSERT_NE(Renamed, nullptr);
  ASSERT_EQ(Renamed->size(), 2u);
  EXPECT_EQ((*Renamed)[0].getAsObject()->getString("original"), "UNIT");
  EXPECT_EQ((*Renamed)[0].getAsObject()->getString("name"), "Wide_UNIT");
  const auto Source = Reply.getString("source");
  ASSERT_TRUE(Source.has_value());
  TypeLibrary Library(DataModel::ILP32);
  auto Parsed = parseCTypes(*Source, Library);
  ASSERT_TRUE(Parsed.Problems.empty());
  EXPECT_EQ(Library.find("Unused"), nullptr);
  ASSERT_NE(Library.find("URL"), nullptr);
  ASSERT_NE(Library.find("Wide_URL"), nullptr);
  EXPECT_EQ(Library.find("URL")->Record->Members[0].Type->Element->Name,
            "UNIT");
  EXPECT_EQ(Library.find("Wide_URL")->Record->Members[0].Type->Element->Name,
            "Wide_UNIT");
  ASSERT_NE(Library.findPrototype("Existing"), nullptr);
  EXPECT_EQ(Library.findPrototype("Existing")->Params.size(), 1u);
  const auto *Added = Library.findPrototype("Added");
  ASSERT_NE(Added, nullptr);
  EXPECT_EQ(Added->Convention, CallingConvention::Stdcall);
  EXPECT_EQ(Added->Params[0].Type->Element->Name, "Wide_URL");
  ASSERT_NE(Library.find("CALL"), nullptr);
  const auto &Callback = *Library.find("CALL")->Target->Element->Function;
  EXPECT_EQ(Callback.Convention, CallingConvention::Stdcall);
  EXPECT_EQ(Callback.Params[0].Type->Element->Name, "Wide_URL");
}

TEST(SessionCAPITypeConversion, PartialProvidersAreNotImportedWithKeepGoing) {
  auto Provider = declaration("int Good(void); not_a_type Bad(void);");
  Provider["importPrefix"] = "Partial";
  const auto Reply = convert(
      llvm::json::Array{declaration("int Existing(void);"), std::move(Provider),
                        declaration("int Later(void);")},
      true);
  ASSERT_FALSE(Reply.get("error"));
  ASSERT_NE(Reply.getArray("problems"), nullptr);
  EXPECT_FALSE(Reply.getArray("problems")->empty());
  EXPECT_EQ(Reply.get("imports"), nullptr);
  const auto Source = Reply.getString("source");
  ASSERT_TRUE(Source.has_value());
  EXPECT_NE(Source->find("Existing("), llvm::StringRef::npos);
  EXPECT_NE(Source->find("Later("), llvm::StringRef::npos);
  EXPECT_EQ(Source->find("Good("), llvm::StringRef::npos);
}

TEST(SessionCAPITypeConversion, InvalidProviderPrefixesAreRefused) {
  for (auto Prefix : {llvm::json::Value(3), llvm::json::Value(""),
                      llvm::json::Value("invalid-prefix")}) {
    auto Provider = declaration("int Added(void);");
    Provider["importPrefix"] = std::move(Prefix);
    const auto Reply = convert(llvm::json::Array{
        declaration("int Existing(void);"), std::move(Provider)});
    ASSERT_TRUE(Reply.getString("error").has_value());
    EXPECT_EQ(Reply.get("source"), nullptr);
  }
}

TEST(SessionCAPITypeConversion, SelectedProfilesKeepTheirOriginalPointees) {
  auto Wide =
      declaration("typedef unsigned short UNIT; struct Record { UNIT *Text; };"
                  "void QueryW(struct Record **Value); void QueryA(struct "
                  "Record **Value);");
  Wide["importPrefix"] = "Unicode";
  Wide["importPrototypes"] = llvm::json::Array{"QueryW"};
  auto Narrow = declaration("typedef char UNIT; struct Record { UNIT *Text; };"
                            "void QueryW(struct Record **Value); void "
                            "QueryA(struct Record **Value);");
  Narrow["importPrefix"] = "ANSI";
  const auto Reply =
      convert(llvm::json::Array{std::move(Wide), std::move(Narrow)});
  ASSERT_FALSE(Reply.get("error"));
  ASSERT_TRUE(Reply.getString("source"));
  TypeLibrary Library(DataModel::ILP32);
  ASSERT_TRUE(
      parseCTypes(*Reply.getString("source"), Library).Problems.empty());
  const auto *W = Library.findPrototype("QueryW");
  const auto *A = Library.findPrototype("QueryA");
  ASSERT_NE(W, nullptr);
  ASSERT_NE(A, nullptr);
  EXPECT_EQ(W->Params[0].Type->Element->Element->Name, "Record");
  EXPECT_EQ(A->Params[0].Type->Element->Element->Name, "ANSI_Record");
  ASSERT_NE(Library.find("Record"), nullptr);
  ASSERT_NE(Library.find("ANSI_Record"), nullptr);
  EXPECT_EQ(
      Library.sizeOf(*Library.find("Record")->Record->Members[0].Type->Element),
      2u);
  EXPECT_EQ(Library.sizeOf(
                *Library.find("ANSI_Record")->Record->Members[0].Type->Element),
            1u);
}

TEST(SessionCAPITypeConversion,
     ProviderSelectionRejectsMalformedAndMissingNames) {
  for (auto Selection :
       {llvm::json::Value("Added"), llvm::json::Value(llvm::json::Array{3}),
        llvm::json::Value(llvm::json::Array{""}),
        llvm::json::Value(llvm::json::Array{"Missing"})}) {
    auto Provider = declaration("int Added(void);");
    Provider["importPrefix"] = "Provider";
    Provider["importPrototypes"] = std::move(Selection);
    const auto Reply = convert(llvm::json::Array{std::move(Provider)});
    EXPECT_TRUE(Reply.getString("error"));
    EXPECT_EQ(Reply.get("source"), nullptr);
  }
  auto Unbound = declaration("int Added(void);");
  Unbound["importPrototypes"] = llvm::json::Array{"Added"};
  EXPECT_TRUE(
      convert(llvm::json::Array{std::move(Unbound)}).getString("error"));
  auto Empty = declaration("int Added(void);");
  Empty["importPrefix"] = "Provider";
  Empty["importPrototypes"] = llvm::json::Array{};
  const auto Reply = convert(llvm::json::Array{std::move(Empty)});
  ASSERT_FALSE(Reply.get("error"));
  ASSERT_NE(Reply.getArray("prototypes"), nullptr);
  EXPECT_TRUE(Reply.getArray("prototypes")->empty());
}
