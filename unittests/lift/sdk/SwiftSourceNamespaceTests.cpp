#include "../../../lib/sdk/capi/SwiftSourceNamespace.h"
#include "gtest/gtest.h"

using namespace neverd;
using namespace neverd::sdk;
namespace {
SwiftSourceSignature declaration(std::string Module, std::string Kind,
                                 std::string Context, std::string Name) {
  SwiftSourceSignature S;
  S.Module = std::move(Module);
  S.ContextKind = std::move(Kind);
  S.ContextName = std::move(Context);
  S.Name = std::move(Name);
  S.ReturnType = {SwiftSourceType::Kind::Integer, "Int64", 64, true, nullptr};
  return S;
}
} // namespace

TEST(SwiftSourceNamespace, DistinctModulesCannotPublishConflictingTypes) {
  auto A = declaration("A", "class", "Counter", "add");
  auto B = declaration("B", "struct", "Counter", "adjust");
  auto C = declaration("A", "global", "", "unrelated");
  EXPECT_EQ(swift_source::namespaceConflicts({A, B, C}),
            (std::set<size_t>{0, 1}));
}

TEST(SwiftSourceNamespace, GlobalNameCollisionsAndSwiftShadowAreRejected) {
  auto A = declaration("A", "global", "", "calculate");
  auto B = declaration("B", "global", "", "calculate");
  auto C = declaration("A", "struct", "Swift", "identity");
  EXPECT_EQ(swift_source::namespaceConflicts({A, B, C}),
            (std::set<size_t>{0, 1, 2}));
}

TEST(SwiftSourceNamespace, OverloadsAndDistinctMembersKeepTheirIdentities) {
  auto A = declaration("A", "class", "Counter", "add");
  auto B = A;
  B.Parameters.push_back({"arg0", A.ReturnType});
  B.Labels.push_back("_");
  auto C = A;
  C.Name = "subtract";
  EXPECT_TRUE(swift_source::namespaceConflicts({A, B, C}).empty());
  // Different native entries cannot turn identical source declarations into
  // legal overloads; source compilation would fail even if both bodies lifted.
  C = A;
  C.Entry = 0x4000;
  EXPECT_EQ(swift_source::namespaceConflicts({A, B, C}),
            (std::set<size_t>{0, 2}));
}

TEST(SwiftSourceNamespace, StorageTypeSpellingQualifiesNestedStandardTypes) {
  SwiftSourceType T{
      SwiftSourceType::Kind::Pointer, "UnsafePointer", 0, false,
      std::make_shared<SwiftSourceType>(SwiftSourceType{
          SwiftSourceType::Kind::Integer, "UInt64", 64, false, nullptr})};
  EXPECT_EQ(swift_source::typeSpelling(T), "Swift.UnsafePointer<Swift.UInt64>");
  EXPECT_TRUE(swift_source::namespaceConflicts(
                  {declaration("A", "struct", "UInt64", "identity")})
                  .empty());
}
