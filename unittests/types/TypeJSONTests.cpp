//===- TypeJSONTests.cpp - Local types as JSON for frontends --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/types/PDBTypeImport.h"
#include "neverd/types/TypeJSON.h"
#include "neverd/types/TypeRender.h"

#include <algorithm>
#include <gtest/gtest.h>

using namespace neverd::types;

namespace {

class TypeJSONFixture : public ::testing::Test {
protected:
  void SetUp() override {
    auto Loaded = importPDBTypes(NEVERD_TYPES_FIXTURES "/local_types.pdb");
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    Library = std::move(*Loaded);
  }

  llvm::json::Object json(llvm::StringRef Name) const {
    const LocalType *Type = Library->find(Name);
    EXPECT_NE(Type, nullptr) << Name.str();
    return Type ? typeJSON(*Library, *Type) : llvm::json::Object();
  }

  std::unique_ptr<TypeLibrary> Library;
};

/// One member as `kind name@offset`, its members in braces.
std::string shape(const llvm::json::Array &Members) {
  std::string Out;
  for (const llvm::json::Value &Value : Members) {
    const llvm::json::Object &Entry = *Value.getAsObject();
    if (!Out.empty())
      Out += ' ';
    Out += Entry.getString("kind")->str();
    if (!Entry.getString("name")->empty())
      Out += " " + Entry.getString("name")->str();
    Out += "@" + std::to_string(*Entry.getInteger("offset"));
    if (auto Width = Entry.getInteger("bitWidth"))
      Out += "." + std::to_string(*Entry.getInteger("bitOffset") % 8) + ":" +
             std::to_string(*Width);
    if (Entry.getString("kind") == "padding")
      Out += "+" + std::to_string(*Entry.getInteger("size"));
    if (const llvm::json::Array *Inner = Entry.getArray("members"))
      Out += "{" + shape(*Inner) + "}";
  }
  return Out;
}

} // namespace

TEST_F(TypeJSONFixture, SharedSpellerMatchesOwnedLayouts) {
  for (BuiltinSpelling Spelling :
       {BuiltinSpelling::Portable, BuiltinSpelling::Windows}) {
    SCOPED_TRACE(static_cast<unsigned>(Spelling));
    RenderOptions Options;
    Options.Spelling = Spelling;
    TypeSpeller Shared(*Library, Options);
    auto Types = Library->types();
    ASSERT_FALSE(Types.empty());
    for (bool Reversed : {false, true}) {
      SCOPED_TRACE(Reversed);
      if (Reversed)
        std::reverse(Types.begin(), Types.end());
      for (const LocalType *Type : Types) {
        SCOPED_TRACE(Type->Name);
        EXPECT_EQ(
            llvm::json::Value(typeJSON(*Library, *Type, Spelling, &Shared)),
            llvm::json::Value(typeJSON(*Library, *Type, Spelling)));
      }
    }
  }
}

TEST_F(TypeJSONFixture, LaysARecordOutWithPaddingAndBits) {
  const llvm::json::Object Thread = json("_THREAD_LIKE");
  EXPECT_EQ(Thread.getString("kind"), "struct");
  EXPECT_EQ(Thread.getInteger("size"), 24);
  EXPECT_EQ(Thread.getInteger("alignment"), 8);
  EXPECT_EQ(shape(*Thread.getArray("members")),
            "field Header@0 union@8{struct@8{bitfield AutoBoostActive@8.0:1 "
            "bitfield ReadyTransition@8.1:1 bitfield MiscSpare@8.2:30} field "
            "MiscFlags@8} union@12{struct@12{bitfield ThreadFlagsSpare@12.0:2 "
            "bitfield AutoAlignment@12.2:1 bitfield DisableBoost@12.3:1 "
            "bitfield ThreadReserved@12.4:28} field ThreadFlags@12} field "
            "Tail@16 padding@19+5");
  const llvm::json::Object &Header =
      *(*Thread.getArray("members"))[0].getAsObject();
  EXPECT_EQ(Header.getString("type"), "ULONGLONG");
}

TEST_F(TypeJSONFixture, NamesTheTypesMembersReferTo) {
  const llvm::json::Object Device = json("_DEVICE");
  const llvm::json::Array &Members = *Device.getArray("members");
  // Type and Size, padding, then Links.
  const llvm::json::Object &Links = *Members[3].getAsObject();
  EXPECT_EQ(Links.getString("name"), "Links");
  EXPECT_EQ(Links.getString("refers"), "_LIST_ENTRY");
  EXPECT_EQ(Links.getString("type"), "struct _LIST_ENTRY");
  const llvm::json::Object &Next = *Members[5].getAsObject();
  EXPECT_EQ(Next.getString("name"), "Next");
  EXPECT_EQ(Next.getString("refers"), "_DEVICE");
}

TEST_F(TypeJSONFixture, EnumerationsKeepTheirSign) {
  const llvm::json::Object Kind = json("_SIGNED_KIND");
  EXPECT_EQ(Kind.getString("underlying"), "INT");
  const llvm::json::Array &Values = *Kind.getArray("values");
  EXPECT_EQ((*Values[0].getAsObject()).getInteger("value"), -1);
  EXPECT_EQ((*Values[0].getAsObject()).getString("name"), "Negative");
}

TEST_F(TypeJSONFixture, ListsEveryType) {
  const llvm::json::Array List = typeListJSON(*Library);
  EXPECT_EQ(List.size(), Library->size());
  bool SawForward = false;
  for (const llvm::json::Value &Value : List) {
    const llvm::json::Object &Entry = *Value.getAsObject();
    if (Entry.getString("name") == "_FORWARD_ONLY") {
      SawForward = true;
      // A declaration has no size.
      EXPECT_FALSE(Entry.getInteger("size"));
    }
    if (Entry.getString("name") == "_THREAD_LIKE")
      EXPECT_EQ(Entry.getInteger("members"), 11);
  }
  EXPECT_TRUE(SawForward);
}
