//===- JVMRecoveryTests.cpp - JVM producer and boundary checks -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Owned compiler artifacts and independently recorded javap observations.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/JVMRecovery.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Base64.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>
#include <fstream>

using namespace llvm;
namespace {
json::Array loadFixtures(StringRef FileName) {
  auto Path = std::filesystem::path(JVM_ARTIFACT_MANIFEST).parent_path() /
              FileName.str();
  auto File = MemoryBuffer::getFile(Path.string());
  if (!File)
    return {};
  auto P = json::parse((*File)->getBuffer());
  if (!P) {
    consumeError(P.takeError());
    return {};
  }
  return std::move(*P->getAsObject()->getArray("fixtures"));
}
const json::Array &fixtures() {
  static const auto Rows = loadFixtures("classes.json");
  return Rows;
}
std::string bytes(const json::Object &Row) {
  std::vector<char> Out;
  if (auto E = decodeBase64(*Row.getString("bytes"), Out)) {
    consumeError(std::move(E));
    return {};
  }
  return {Out.begin(), Out.end()};
}
json::Object read(StringRef B, const json::Object &Options = {}) {
  return neverd::jvm::recover(B, "Sample.class", Options, [] { return false; });
}
const json::Object &first(const json::Object &R, StringRef Field) {
  return *R.getArray(Field)->front().getAsObject();
}
TEST(JVMRecovery, CompilerVersionsInstructionsAndExceptionRanges) {
  ASSERT_GE(fixtures().size(), 32u);
  for (auto &Entry : fixtures()) {
    auto &Row = *Entry.getAsObject();
    SCOPED_TRACE(Row.getString("id")->str());
    auto R = read(bytes(Row));
    ASSERT_EQ(R.getString("input_sha256"), Row.getString("sha256"));
    auto *C = first(R, "components").getObject("inventory");
    ASSERT_TRUE(C);
    EXPECT_EQ(C->getInteger("major"), Row.getInteger("major"));
    EXPECT_EQ(C->getInteger("minor"), Row.getInteger("minor"));
    auto &Methods = *R.getArray("functions");
    auto &Expected = *Row.getArray("methods");
    ASSERT_EQ(Methods.size(), Expected.size());
    for (unsigned I = 0; I < Methods.size(); ++I) {
      const auto &M = *Methods[I].getAsObject(),
                 &E = *Expected[I].getAsObject();
      SCOPED_TRACE(M.getString("name")->str());
      EXPECT_EQ(M.getString("name"), E.getString("name"));
      EXPECT_EQ(M.getString("descriptor"), E.getString("descriptor"));
      auto &Ops = *M.getArray("instructions"),
           &Oracle = *E.getArray("instructions");
      ASSERT_EQ(Ops.size(), Oracle.size());
      for (unsigned P = 0; P < Ops.size(); ++P) {
        auto &O = *Ops[P].getAsObject();
        auto &J = *Oracle[P].getAsArray();
        EXPECT_EQ(O.getInteger("pc"), J[0].getAsInteger());
        EXPECT_EQ(O.getString("opcode"), J[1].getAsString());
        if (J.size() > 2)
          for (auto &[Key, Expected] : *J[2].getAsObject()) {
            if (StringRef(Key) == "branch_target") {
              auto *Succ = O.getArray("successors");
              ASSERT_TRUE(Succ && !Succ->empty());
              EXPECT_EQ(Succ->front(), Expected);
            } else {
              auto *Actual = O.get(Key);
              ASSERT_TRUE(Actual) << StringRef(Key).str();
              EXPECT_EQ(*Actual, Expected);
            }
          }
      }
      auto &Handlers = *E.getArray("handlers");
      if (Ops.empty())
        continue;
      auto *Table = M.getArray("exception_table");
      ASSERT_NE(Table, nullptr);
      ASSERT_EQ(Table->size(), Handlers.size());
      for (unsigned H = 0; H < Handlers.size(); ++H) {
        auto &T = *(*Table)[H].getAsObject();
        auto &J = *Handlers[H].getAsArray();
        EXPECT_EQ(T.getInteger("start"), J[0].getAsInteger());
        EXPECT_EQ(T.getInteger("end"), J[1].getAsInteger());
        EXPECT_EQ(T.getInteger("handler"), J[2].getAsInteger());
        if (J[3].getAsString() != "any")
          EXPECT_EQ(T.getString("type"), J[3].getAsString());
      }
      if (auto *IR = M.getObject("ir")) {
        for (auto &V : *IR->getArray("blocks")) {
          auto &B = *V.getAsObject();
          auto PC = *B.getInteger("pc");
          auto *Edges = B.getArray("exception_successors");
          ASSERT_TRUE(Edges);
          ASSERT_TRUE(B.getArray("normal_successors"));
          unsigned Count = 0;
          for (auto &HV : Handlers) {
            const auto &H = *HV.getAsArray();
            if (PC >= *H[0].getAsInteger() && PC < *H[1].getAsInteger()) {
              ASSERT_LT(Count, Edges->size());
              EXPECT_EQ((*Edges)[Count++].getAsObject()->getInteger("target"),
                        H[2].getAsInteger());
            }
          }
          EXPECT_EQ(Count, Edges->size());
        }
      }
      if (Row.getString("id")->starts_with("java-") ||
          Row.getString("id") == "stripped" || Row.getString("id") == "ecj") {
        EXPECT_EQ(M.getString("status"), "reconstructed_source")
            << formatv("{0}",
                       json::Value(json::Array(*M.getArray("diagnostics"))))
                   .str();
        EXPECT_NE(M.getObject("ir"), nullptr);
        EXPECT_NE(M.getArray("source_map"), nullptr);
      }
    }
  }
}
TEST(JVMRecovery, SelectionPreviewAndJobLimits) {
  ASSERT_FALSE(fixtures().empty());
  auto D = bytes(*fixtures()[20].getAsObject());
  auto R = read(D, json::Object{{"method", "arithmetic(II)I"}});
  ASSERT_EQ(R.getArray("functions")->size(), 1u);
  EXPECT_TRUE(first(R, "functions").getString("source"));
  R = read(D, json::Object{{"inventory", true}});
  EXPECT_TRUE(R.getArray("functions")->empty());
  EXPECT_EQ(R.getString("status"), "metadata_only");
  R = read(D, json::Object{{"method", "absent()V"}});
  EXPECT_EQ(R.getString("status"), "invalid_request");
  R = read(D, json::Object{{"max_member_bytes", 32}});
  EXPECT_EQ(R.getString("status"), "budget_exceeded");
  R = read(D, json::Object{{"max_job_items", 3}});
  EXPECT_EQ(R.getString("status"), "budget_exceeded");
  R = read(D, json::Object{{"max_method_work", 1}, {"method", "choose(I)I"}});
  ASSERT_EQ(R.getArray("functions")->size(), 1u);
  EXPECT_EQ(first(R, "functions").getString("status"), "budget_exceeded");
  R = neverd::jvm::recover(D, "a.class", {}, [] { return true; });
  EXPECT_EQ(R.getString("status"), "cancelled");
  D[4] = char(255);
  D[5] = char(255);
  EXPECT_TRUE(read(D).getArray("functions")->empty());
  EXPECT_TRUE(read(D, json::Object{{"release", 22}, {"preview", true}})
                  .getArray("functions")
                  ->empty());
  R = read(D, json::Object{{"release", 21},
                           {"preview", true},
                           {"method", "choose(I)I"}});
  ASSERT_EQ(R.getArray("functions")->size(), 1u);
  EXPECT_TRUE(first(R, "functions").getString("source"));
}
TEST(JVMRecovery, MethodlessClassAndSourceEncoding) {
  for (auto &V : fixtures()) {
    const auto &F = *V.getAsObject();
    if (F.getString("id") != "module")
      continue;
    auto R = read(bytes(F),
                  json::Object{{"class", "module-info"}, {"inventory", true}});
    EXPECT_EQ(R.getString("status"), "metadata_only");
    EXPECT_TRUE(
        first(R, "components").getObject("inventory")->getObject("module"));
  }
  auto Bad = neverd::jvm::recover(StringRef("a\0b", 3), "Info.java", {},
                                  [] { return false; });
  EXPECT_EQ(Bad.getString("status"), "unsupported");
  EXPECT_EQ(first(Bad, "diagnostics").getString("code"), "source_encoding");
}
TEST(JVMRecovery, MalformedPoolAndTruncatedClass) {
  ASSERT_FALSE(fixtures().empty());
  auto D = bytes(*fixtures()[0].getAsObject());
  for (size_t I = 0; I < D.size(); ++I)
    EXPECT_EQ(read(StringRef(D).take_front(I)).getString("status"), "malformed")
        << I;
  D[10] = char(255);
  EXPECT_EQ(read(D).getString("status"), "malformed");
}
TEST(JVMRecovery, SyntheticCodeTypesSwitchesAndOpaqueRecords) {
  auto Rows = loadFixtures("synthetic.json");
  ASSERT_GE(Rows.size(), 21u);
  for (auto &V : Rows) {
    auto &F = *V.getAsObject();
    auto Id = *F.getString("id");
    SCOPED_TRACE(Id.str());
    auto R = read(bytes(F));
    EXPECT_EQ(R.getString("input_sha256"), F.getString("sha256"));
    ASSERT_EQ(R.getArray("functions")->size(), 1u);
    const auto &M = first(R, "functions");
    EXPECT_EQ(M.getString("status"), F.getString("status"));
    if (Id.starts_with("unknown-")) {
      ASSERT_TRUE(M.getObject("ir"));
      ASSERT_FALSE(R.getArray("relations")->empty());
      EXPECT_FALSE(M.get("source"));
    }
    if (Id == "opaque") {
      auto &Attrs = *M.getArray("code_attributes");
      ASSERT_EQ(Attrs.size(), 1u);
      EXPECT_EQ(Attrs.front().getAsObject()->getString("status"), "opaque");
    }
  }
}
TEST(JVMRecovery, PhysicalJarMembersAndExplicitRuntimeSelection) {
  auto Jars = loadFixtures("jars.json");
  ASSERT_EQ(Jars.size(), 8u);
  for (auto &V : Jars) {
    auto &Entry = *V.getAsObject();
    auto Id = *Entry.getString("id");
    auto B = bytes(Entry);
    SCOPED_TRACE(Id.str());
    if (Id == "multi") {
      auto Limited = neverd::jvm::recover(
          B, "fixture.jar", json::Object{{"max_expanded_bytes", 32}},
          [] { return false; });
      EXPECT_EQ(Limited.getString("status"), "budget_exceeded");
      EXPECT_EQ(Limited.getString("input_sha256"), Entry.getString("sha256"));
    }
    for (int Release : {0, 8, 11, 17, 21, 27}) {
      auto R = neverd::jvm::recover(
          B, "fixture.jar",
          json::Object{{"release", Release}, {"inventory", true}},
          [] { return false; });
      EXPECT_EQ(R.getString("input_sha256"), Entry.getString("sha256"));
      if (Id == "traversal" || Id == "duplicate") {
        EXPECT_EQ(R.getString("status"), "malformed");
        EXPECT_TRUE(R.getArray("components")->empty());
        continue;
      }
      if (Id == "classpath-alias") {
        bool Found = false;
        for (auto &V : *R.getArray("components")) {
          auto &C = *V.getAsObject();
          if (C.getString("kind") != "class")
            continue;
          EXPECT_FALSE(C.getBoolean("classpath_name_matches").value_or(true));
          Found = true;
        }
        EXPECT_TRUE(Found);
        break;
      }
      if (Id == "mixed") {
        EXPECT_EQ(R.getString("status"), "partial_reconstruction");
        auto Detailed = neverd::jvm::recover(
            B, "mixed.jar", json::Object{{"method", "arithmetic(II)I"}},
            [] { return false; });
        ASSERT_EQ(Detailed.getArray("functions")->size(), 1u);
        EXPECT_TRUE(first(Detailed, "functions").getString("source"));
        break;
      }
      EXPECT_EQ(R.getString("status"), "metadata_only");
      int Count = 0, Active = -1, Selections = 0;
      for (auto &M : *R.getArray("components")) {
        auto &C = *M.getAsObject();
        if (C.getString("kind") != "class")
          continue;
        ++Count;
        if (*C.getBoolean("selected")) {
          Active = *C.getInteger("release");
          ++Selections;
        }
      }
      EXPECT_EQ(Count, 3);
      EXPECT_EQ(Selections, 1);
      bool Multi = Id == "multi" || Id == "manifest-fold";
      EXPECT_EQ(Active, !Multi || Release < 11 ? 0 : Release < 21 ? 11 : 21);
    }
  }
}
TEST(JVMRecovery, IndependentMethodFailureKeepsSiblingSource) {
  auto D = bytes(*fixtures().front().getAsObject());
  auto Original = read(D);
  for (auto &V : *Original.getArray("functions")) {
    auto &M = *V.getAsObject();
    if (M.getString("name") == "arithmetic")
      D[*M.getInteger("code_offset")] = char(202);
  }
  auto R = read(D);
  EXPECT_EQ(R.getString("status"), "partial_reconstruction");
  for (auto &V : *R.getArray("functions")) {
    auto &M = *V.getAsObject();
    if (M.getString("name") == "arithmetic")
      EXPECT_EQ(M.getString("status"), "malformed");
    if (M.getString("name") == "choose")
      EXPECT_TRUE(M.getString("source"));
  }
}
TEST(JVMRecovery, BootstrapMetadataAndNativeRelations) {
  bool Lambda = false, Native = false, Kotlin = false;
  for (auto &V : fixtures()) {
    auto &F = *V.getAsObject();
    auto Id = *F.getString("id");
    if (Id != "lambda-generics" && Id != "java-17" &&
        !Id.starts_with("kotlin-"))
      continue;
    auto R = read(bytes(F));
    if (Id == "lambda-generics") {
      unsigned Supported = 0;
      for (auto &V : *R.getArray("functions")) {
        auto &M = *V.getAsObject();
        if (M.getString("name") == "add" || M.getString("name") == "describe") {
          EXPECT_TRUE(M.getString("source"));
          ++Supported;
        }
        if (M.getString("name") == "cleanup")
          EXPECT_EQ(M.getString("status"), "disassembly_only");
      }
      EXPECT_EQ(Supported, 2u);
      Lambda = true;
    } else if (Id == "java-17") {
      for (auto &V : *R.getArray("relations")) {
        auto &E = *V.getAsObject();
        if (E.getString("kind") != "jni_native")
          continue;
        EXPECT_EQ(E.getString("long_name"), "Java_Sample_sampleNative__I");
        Native = true;
      }
    } else {
      auto &C = first(R, "components");
      auto *K = C.getObject("inventory")->getObject("kotlin_metadata");
      ASSERT_TRUE(K);
      auto *MV = K->getArray("mv");
      ASSERT_TRUE(MV);
      ASSERT_GE(MV->size(), 2u);
      EXPECT_EQ((*MV)[0].getAsInteger(), Id.starts_with("kotlin-1.") ? 1 : 2);
      EXPECT_EQ((*MV)[1].getAsInteger(), Id.starts_with("kotlin-1.") ? 9 : 4);
      EXPECT_EQ(K->getInteger("k"), 1);
      Kotlin = true;
    }
  }
  EXPECT_TRUE(Lambda);
  EXPECT_TRUE(Native);
  EXPECT_TRUE(Kotlin);
}
TEST(JVMRecovery, OrderedLocalHierarchyAndContentCache) {
  SmallString<128> Tmp;
  ASSERT_FALSE(sys::fs::createUniqueDirectory("neverd-jvm", Tmp));
  auto Cleanup = scope_exit([&] { sys::fs::remove_directories(Tmp); });
  auto Dir = std::filesystem::path(Tmp.c_str());
  auto file = [&](StringRef Name) { return (Dir / Name.str()).string(); };
  auto write = [&](StringRef Name, StringRef Data) {
    std::ofstream F(file(Name), std::ios::binary);
    F.write(Data.data(), Data.size());
    return F.good();
  };
  std::string Leaf, Middle, Root;
  for (auto &V : fixtures()) {
    auto &F = *V.getAsObject();
    auto Id = *F.getString("id");
    if (Id == "hierarchy-leaf")
      Leaf = bytes(F);
    if (Id == "hierarchy-middle")
      Middle = bytes(F);
    if (Id == "hierarchy-root")
      Root = bytes(F);
  }
  ASSERT_FALSE(Leaf.empty());
  ASSERT_FALSE(Middle.empty());
  ASSERT_FALSE(Root.empty());
  ASSERT_TRUE(write("Leaf.class", Leaf));
  ASSERT_TRUE(write("A.class", Middle));
  ASSERT_TRUE(write("B.class", Middle));
  auto fetch = [&](std::initializer_list<StringRef> Deps, int Release = 0,
                   bool Preview = false) {
    json::Array A;
    for (auto N : Deps)
      A.emplace_back(file(N));
    json::Object Q{{"release", Release},
                   {"preview", Preview},
                   {"method", "parent(LLeaf;)LRoot;"},
                   {"dependencies", std::move(A)}};
    std::string S;
    raw_string_ostream(S) << json::Value(std::move(Q));
    return neverd::jvm::recoverFileJSON(file("Leaf.class"), S,
                                        [] { return false; });
  };
  auto parse = [](StringRef S) {
    auto R = json::parse(S);
    if (!R) {
      consumeError(R.takeError());
      return json::Object{};
    }
    return std::move(*R->getAsObject());
  };
  auto Missing = parse(fetch({}));
  ASSERT_EQ(first(Missing, "functions").getString("status"),
            "disassembly_only");
  EXPECT_EQ(first(first(Missing, "functions"), "diagnostics").getString("code"),
            "unresolved_hierarchy");
  auto With = fetch({"A.class"});
  EXPECT_EQ(fetch({"A.class"}), With);
  EXPECT_EQ(first(parse(With), "functions").getString("status"),
            "reconstructed_source");
  for (bool Reverse : {false, true}) {
    auto R = parse(Reverse ? fetch({"B.class", "A.class"})
                           : fetch({"A.class", "B.class"}));
    ASSERT_EQ(first(R, "functions").getString("status"), "disassembly_only");
    bool Found = false;
    for (auto &V : *R.getArray("relations")) {
      auto &Edge = *V.getAsObject();
      if (Edge.getInteger("from") != 0 || Edge.getString("target") != "Middle")
        continue;
      ASSERT_EQ(Edge.getString("resolution"), "ambiguous");
      auto Id = Edge.getInteger("first_definition");
      ASSERT_TRUE(Id);
      EXPECT_EQ(
          (*R.getArray("components"))[*Id].getAsObject()->getString("name"),
          Reverse ? "B.class" : "A.class");
      Found = true;
    }
    EXPECT_TRUE(Found);
  }
  auto Newer = Middle;
  Newer[7] = 71;
  ASSERT_TRUE(write("A.class", Newer));
  EXPECT_EQ(
      first(parse(fetch({"A.class"}, 17)), "functions").getString("status"),
      "disassembly_only");
  EXPECT_EQ(
      first(parse(fetch({"A.class"}, 27)), "functions").getString("status"),
      "reconstructed_source");
  auto Preview = Middle;
  Preview[4] = char(255);
  Preview[5] = char(255);
  ASSERT_TRUE(write("A.class", Preview));
  EXPECT_EQ(first(parse(fetch({"A.class"}, 17, true)), "functions")
                .getString("status"),
            "reconstructed_source");
  EXPECT_EQ(
      first(parse(fetch({"A.class"}, 17)), "functions").getString("status"),
      "disassembly_only");
  ASSERT_TRUE(write("A.class", Root));
  EXPECT_EQ(first(parse(fetch({"A.class"})), "functions").getString("status"),
            "disassembly_only");
  ASSERT_TRUE(write("A.class", Middle));
  EXPECT_EQ(fetch({"A.class"}), With);
}
} // namespace
