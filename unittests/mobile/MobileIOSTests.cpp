#include "MobileIOSInternal.h"
#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <chrono>

using namespace neverd::mobile;
using namespace neverd::mobile::ios;
namespace {
struct TemporaryDirectory {
  fs::path path;
  TemporaryDirectory() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned i = 0; i < 100; ++i) {
      auto candidate = fs::temp_directory_path() /
                       ("neverd-native-ios-test-" + std::to_string(stamp) +
                        "-" + std::to_string(i));
      if (fs::create_directory(candidate)) {
        path = candidate;
        return;
      }
    }
    throw Error("cannot create test directory");
  }
  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path, error);
  }
};
void integer(std::string &d, size_t p, uint64_t value, unsigned width = 4,
             bool little = true) {
  for (unsigned i = 0; i < width; ++i)
    d[p + i] = char(value >> (8 * (little ? i : width - i - 1)));
}
std::string thin(uint32_t cpu = 0x100000c, bool encrypted = false) {
  std::string d(4096, '\0');
  bool wide = cpu & 0x1000000;
  unsigned header = wide ? 32 : 28;
  integer(d, 0, wide ? 0xfeedfacf : 0xfeedface);
  integer(d, 4, cpu);
  integer(d, 12, 2);
  if (encrypted) {
    integer(d, 16, 1);
    integer(d, 20, 24);
    integer(d, header, 0x2c);
    integer(d, header + 4, 24);
    integer(d, header + 8, 1024);
    integer(d, header + 12, 16);
    integer(d, header + 16, 1);
  }
  return d;
}
std::string fat(bool wide = false) {
  std::string d(12288, '\0');
  integer(d, 0, wide ? 0xcafebabf : 0xcafebabe, 4, false);
  integer(d, 4, 2, 4, false);
  for (unsigned i = 0; i < 2; ++i) {
    size_t p = 8 + i * (wide ? 32 : 20);
    auto cpu = i ? 0x100000c : 12;
    integer(d, p, cpu, 4, false);
    integer(d, p + 8, 4096 * (i + 1), wide ? 8 : 4, false);
    integer(d, p + (wide ? 16 : 12), 4096, wide ? 8 : 4, false);
    integer(d, p + (wide ? 24 : 16), 12, 4, false);
    d.replace(4096 * (i + 1), 4096, thin(cpu));
  }
  return d;
}
std::string loadableThin() {
  auto data = thin();
  integer(data, 16, 1);
  integer(data, 20, 72);
  integer(data, 32, 0x19);
  integer(data, 36, 72);
  data.replace(40, 6, "__TEXT");
  integer(data, 56, 0x100000000, 8);
  integer(data, 64, data.size(), 8);
  integer(data, 80, data.size(), 8);
  integer(data, 88, 7);
  integer(data, 92, 5);
  return data;
}
std::pair<Object, Object> objcFixture(std::string body = "return arg0 + arg1;",
                                      bool class_method = false) {
  Object method{{"selector", "add:to:"},
                {"class_method", class_method},
                {"implementation", "0x1000"},
                {"type_encoding", "q32@0:8q16q24"}};
  Object cls{{"name", "Calculator"},
             {"root_class", true},
             {"superclass", nullptr},
             {"address", "0x2000"},
             {"methods", Array{Object(method)}}};
  Object metadata{{"status", "recovered"},
                  {"classes", Array{std::move(cls)}},
                  {"limitations", Array{}}};
  Object native = method;
  native["class_name"] = "Calculator";
  native["status"] = "recovered";
  native["return_type"] = "int64_t";
  native["function_name"] = "neverd_objc_imp_1000";
  native["parameters"] = Array{Object{{"name", "objc_self"}, {"type", "void*"}},
                               Object{{"name", "objc_cmd"}, {"type", "void*"}},
                               Object{{"name", "arg0"}, {"type", "int64_t"}},
                               Object{{"name", "arg1"}, {"type", "int64_t"}}};
  native["source"] =
      "#include <stdint.h>\nint64_t neverd_objc_imp_1000(void* objc_self, "
      "void* objc_cmd, int64_t arg0, int64_t arg1) {\n" +
      body + "\n}\n";
  return {Object{{"schema_version", 1},
                 {"pointer_size", 8},
                 {"methods", Array{std::move(native)}}},
          std::move(metadata)};
}
Object emptyLocalClass(const std::string &name) {
  return Object{{"name", name},
                {"address", "0x3000"},
                {"root_class", false},
                {"superclass", "NSObject"},
                {"instance_start", 8},
                {"instance_size", 8},
                {"ivar_status", "recovered"},
                {"ivars", Array{}},
                {"methods", Array{}}};
}
std::pair<Object, Object> swiftFixture(bool alias = false) {
  Array supplied, rows, units;
  std::string combined;
  for (unsigned i = 0; i < (alias ? 2 : 1); ++i) {
    auto symbol = "$s4Demo" + std::to_string(i),
         name = "answer" + std::to_string(i),
         source = "public func " + name + "() -> Int32 { return 42 }\n";
    Object id{{"entry", "0x1000"}, {"mangled_symbol", symbol}};
    Object signature = id;
    signature["status"] = "supported";
    signature["classification"] = "callable";
    supplied.push_back(std::move(signature));
    Object row = id;
    row["status"] = "recovered";
    row["source"] = source;
    rows.push_back(std::move(row));
    units.push_back(Object{{"kind", "function"},
                           {"module", "Demo"},
                           {"name", name},
                           {"source", source},
                           {"method_entries", Array{"0x1000"}},
                           {"method_identities", Array{std::move(id)}}});
    combined += source + "\n";
  }
  int count = alias ? 2 : 1;
  return {Object{{"schema_version", 1},
                 {"methods", std::move(supplied)},
                 {"symbols", Array{}},
                 {"symbol_count", count},
                 {"method_count", count},
                 {"supported_signature_count", count},
                 {"unsupported_signature_count", 0},
                 {"unclassified_symbol_count", 0},
                 {"limitations", Array{}}},
          Object{{"schema_version", 1},
                 {"status", "success"},
                 {"methods", std::move(rows)},
                 {"source", combined},
                 {"method_count", count},
                 {"recovered_method_count", count},
                 {"unrecovered_method_count", 0},
                 {"coverage_status", "recovered"},
                 {"source_units", std::move(units)},
                 {"types", Array{}},
                 {"limitations", Array{}}}};
}

} // namespace
TEST(MobileIOSNative, SelectsBothFatWidthsAndExplicitArchitecture) {
  for (bool wide : {false, true}) {
    Budget b;
    auto s = selectSlice(fat(wide), "auto", b);
    EXPECT_EQ(s.architecture, "arm64");
    EXPECT_EQ(s.available, (std::vector<std::string>{"arm", "arm64"}));
    EXPECT_EQ(selectSlice(fat(wide), "arm", b).pointer_size, 4u);
    EXPECT_THROW(selectSlice(fat(wide), "x86_64", b), Error);
  }
}
TEST(MobileIOSNative, RejectsFatOverlapAndHeaderDisagreement) {
  Budget b;
  auto d = fat();
  integer(d, 8 + 20 + 8, 4096, 4, false);
  EXPECT_THROW(selectSlice(d, "auto", b), Error);
  d = fat();
  integer(d, 4096 + 4, 7);
  EXPECT_THROW(selectSlice(d, "auto", b), Error);
}
TEST(MobileIOSNative, ValidatesUnselectedFatSlice) {
  Budget b;
  auto d = fat();
  integer(d, 4096 + 16, 0xffffffff);
  EXPECT_THROW(selectSlice(d, "arm64", b), Error);
}
TEST(MobileIOSNative, DetectsEncryptionAndRejectsTruncatedCommands) {
  Budget b;
  EXPECT_TRUE(selectSlice(thin(0x100000c, true), "auto", b).encrypted);
  auto d = thin();
  integer(d, 16, 1);
  integer(d, 20, 8);
  integer(d, 36, 4);
  EXPECT_THROW(selectSlice(d, "auto", b), Error);
}
TEST(MobileIOSNative, RejectsDuplicateChainedFixupCommands) {
  Budget b;
  auto d = thin();
  integer(d, 16, 2);
  integer(d, 20, 32);
  for (unsigned p : {32, 48}) {
    integer(d, p, 0x80000034);
    integer(d, p + 4, 16);
  }
  EXPECT_THROW(selectSlice(d, "auto", b), Error);
}

TEST(MobileIOSNative, RejectsOutOfBoundsAndUnterminatedSymbolNames) {
  Budget budget;
  auto data = thin();
  integer(data, 16, 1);
  integer(data, 20, 24);
  integer(data, 32, 2);
  integer(data, 36, 24);
  integer(data, 40, 512);
  integer(data, 44, 1);
  integer(data, 48, 600);
  integer(data, 52, 10);
  integer(data, 512, 11);
  integer(data, 516, 0x0f, 1);
  EXPECT_THROW(selectSlice(data, "auto", budget), Error);
  integer(data, 512, 1);
  data.replace(601, 9, "abcdefghi");
  EXPECT_THROW(selectSlice(data, "auto", budget), Error);
}
TEST(MobileIOSNative, ParsesXMLInfoWithEscapedUnicode) {
  Budget b;
  auto p = parsePlist(
      "<?xml version=\"1.0\"?><!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST "
      "1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\"><plist "
      "version=\"1.0\"><dict><key>CFBundleExecutable</"
      "key><string>Demo&amp;&#x4e2d;</string><key>Enabled</key><true/"
      "><key>List</key><array><integer>7</integer><string/></array></dict></"
      "plist>",
      b);
  EXPECT_EQ(str(p, "CFBundleExecutable"), "Demo&中");
  EXPECT_TRUE(flag(p, "Enabled"));
  EXPECT_EQ(array(p, "List").size(), 2u);
}
TEST(MobileIOSNative, RejectsXMLDuplicateKeysAndEntityExpansion) {
  Budget b;
  EXPECT_THROW(parsePlist("<plist><dict><key>x</key><string>a</string><key>x</"
                          "key><string>b</string></dict></plist>",
                          b),
               Error);
  EXPECT_THROW(
      parsePlist("<!DOCTYPE plist [<!ENTITY e 'data'>]><plist><dict/></plist>",
                 b),
      Error);
}
TEST(MobileIOSNative, ParsesBinaryPlistDictionary) {
  Budget b;
  std::string d = "bplist00";
  d += char(0xd1);
  d += char(1);
  d += char(2);
  d += char(0x53);
  d += "Key";
  d += char(0x55);
  d += "Value";
  auto table = d.size();
  d += char(8);
  d += char(11);
  d += char(15);
  auto trailer = d.size();
  d.resize(trailer + 32);
  integer(d, trailer + 6, 1, 1, false);
  integer(d, trailer + 7, 1, 1, false);
  integer(d, trailer + 8, 3, 8, false);
  integer(d, trailer + 16, 0, 8, false);
  integer(d, trailer + 24, table, 8, false);
  EXPECT_EQ(str(parsePlist(d, b), "Key"), "Value");
  d[10] = 0;
  EXPECT_THROW(parsePlist(d, b), Error);
}
TEST(MobileIOSNative, RejectsExcessivePlistNesting) {
  Budget b;
  std::string d = "<plist>";
  for (unsigned i = 0; i < 130; ++i)
    d += "<array>";
  d += "<dict/>";
  for (unsigned i = 0; i < 130; ++i)
    d += "</array>";
  d += "</plist>";
  EXPECT_THROW(parsePlist(d, b), Error);
}
TEST(MobileIOSNative, PlistExpandedValuesRespectTheDecodedBudget) {
  Limits limits;
  limits.max_bytes = 512;
  Budget budget(limits);
  EXPECT_THROW(
      parsePlist(
          "<plist><dict><key>items</key><array><string>a</string><string>b</"
          "string><string>c</string><string>d</string></array></dict></plist>",
          budget),
      Error);
}
TEST(MobileIOSNative, KeepsActualObjCBodyAndImplicitBinding) {
  Budget b;
  auto [batch, metadata] =
      objcFixture("int64_t value=0; for(int64_t i=0;i<arg0;++i) {value+=arg1;} "
                  "return value;");
  auto r = objcSources(batch, metadata, 8, b);
  EXPECT_EQ(number(r.coverage, "recovered_method_count"), 1);
  EXPECT_NE(r.source.find("value+=arg1;"), r.source.npos);
  EXPECT_NE(r.source.find("void* objc_self = (void*)self;"), r.source.npos);
  EXPECT_NE(r.source.find("@implementation Calculator"), r.source.npos);
}
TEST(MobileIOSNative, DirectParameterReturnIsAUseNotADeclaration) {
  Budget b;
  auto [batch, metadata] = objcFixture("return arg0 + arg1;");
  EXPECT_EQ(number(objcSources(batch, metadata, 8, b).coverage,
                   "recovered_method_count"),
            1);
}
TEST(MobileIOSNative, RejectsObjCPrototypeAndWrongIdentity) {
  Budget b;
  auto [batch, metadata] = objcFixture();
  auto &m = *batch.getArray("methods")->front().getAsObject();
  m["function_name"] = "other";
  auto r = objcSources(batch, metadata, 8, b);
  EXPECT_EQ(number(r.coverage, "recovered_method_count"), 0);
  m["function_name"] = "neverd_objc_imp_1000";
  m["source"] = "int64_t neverd_objc_imp_1000(void* objc_self, void* objc_cmd, "
                "int64_t arg0, int64_t arg1);";
  EXPECT_EQ(number(objcSources(batch, metadata, 8, b).coverage,
                   "recovered_method_count"),
            0);
}
TEST(MobileIOSNative, RejectsObjCParameterRebindingAndWrongFloatABI) {
  Budget b;
  auto [batch, metadata] = objcFixture("int64_t arg0=0; return arg0;");
  EXPECT_EQ(number(objcSources(batch, metadata, 8, b).coverage,
                   "recovered_method_count"),
            0);
  auto &m = *batch.getArray("methods")->front().getAsObject();
  m["return_type"] = "double";
  EXPECT_EQ(number(objcSources(batch, metadata, 8, b).coverage,
                   "recovered_method_count"),
            0);
}
TEST(MobileIOSNative, CCommentsLiteralsCannotCreateFunctionBodies) {
  EXPECT_EQ(
      nativeFunctionCount("#include <stdint.h>\n// int fake(){return 1;}\n"),
      0u);
  EXPECT_EQ(
      nativeFunctionCount("static inline int helper(){return 1;}\nint "
                          "real(){const char *s=\"/* } {\";return helper();}"),
      1u);
}
TEST(MobileIOSNative, RuntimeBlockTypeDoesNotInventInvocationSignature) {
  EXPECT_EQ(
      objcTypes("q32@0:8@?16q24"),
      (std::vector<std::string>{"long long", "id", "SEL", "id", "long long"}));
  EXPECT_TRUE(objcTypes("q32@0:8@??16q24").empty());
}
TEST(MobileIOSNative, IvarOffsetsAndSuperclassRemainInHeader) {
  auto [batch, metadata] = objcFixture();
  auto &c = *metadata.getArray("classes")->front().getAsObject();
  c["root_class"] = false;
  c["superclass"] = "NSObject";
  c["instance_start"] = 8;
  c["instance_size"] = 24;
  c["ivar_status"] = "recovered";
  c["ivars"] = Array{Object{{"name", "value"},
                            {"type_encoding", "q"},
                            {"offset", 16},
                            {"size", 8},
                            {"alignment", 8}}};
  auto h = objcHeader(metadata);
  EXPECT_NE(h.find("@interface Calculator : NSObject"), h.npos);
  EXPECT_NE(h.find("neverd_objc_padding_8[8]"), h.npos);
  EXPECT_NE(h.find("long long value;"), h.npos);
}
TEST(MobileIOSNative, UnavailableSuperclassDeclarationCannotRecoverScalarBody) {
  // A class method needs its owner's declaration even without instance data.
  auto [batch, metadata] = objcFixture("return arg0 + arg1;", true);
  auto &cls = *metadata.getArray("classes")->front().getAsObject();
  cls["root_class"] = false;
  cls["superclass"] = "ExternalBase";
  (*batch.getArray("methods")
        ->front()
        .getAsObject())["instance_layout_classes"] = Array{};
  Budget budget;
  auto result = objcSources(batch, metadata, 8, budget);
  EXPECT_EQ(number(result.coverage, "method_count"), 1);
  EXPECT_EQ(number(result.coverage, "recovered_method_count"), 0);
  const auto &row = object(array(result.coverage, "methods")[0], "method");
  EXPECT_NE(str(row, "reason").find("declaration"), std::string::npos);
  EXPECT_EQ(result.source.find("@interface Calculator"), std::string::npos);
  EXPECT_EQ(result.source.find("@implementation Calculator"),
            std::string::npos);
  EXPECT_EQ(objcHeader(metadata).find("@interface Calculator"),
            std::string::npos);
}

TEST(MobileIOSNative,
     UnavailableAncestorDeclarationDoesNotPoisonIndependentClass) {
  auto [batch, metadata] = objcFixture();
  auto &cls = *metadata.getArray("classes")->front().getAsObject();
  Object independent(cls);
  independent["name"] = "Independent";
  cls["root_class"] = false;
  cls["superclass"] = "Intermediate";
  metadata.getArray("classes")->push_back(Object{{"name", "Intermediate"},
                                                 {"root_class", false},
                                                 {"superclass", "ExternalBase"},
                                                 {"methods", Array{}}});
  metadata.getArray("classes")->push_back(std::move(independent));
  auto &native = *batch.getArray("methods")->front().getAsObject();
  native["instance_layout_classes"] = Array{};
  Object independent_method(native);
  independent_method["class_name"] = "Independent";
  batch.getArray("methods")->push_back(std::move(independent_method));
  Budget budget;
  auto result = objcSources(batch, metadata, 8, budget);
  EXPECT_EQ(number(result.coverage, "method_count"), 2);
  EXPECT_EQ(number(result.coverage, "recovered_method_count"), 1);
  EXPECT_EQ(str(result.coverage, "status"), "partial");
  EXPECT_EQ(result.source.find("@interface Calculator"), std::string::npos);
  EXPECT_EQ(result.source.find("@interface Intermediate"), std::string::npos);
  EXPECT_EQ(result.source.find("@implementation Calculator"),
            std::string::npos);
  EXPECT_NE(result.source.find("@implementation Independent"),
            std::string::npos);
}

TEST(MobileIOSNative, InvalidInheritanceDeclarationCannotInventRootClass) {
  for (const std::string parent : {"", "Calculator", "Cycle", "int"}) {
    SCOPED_TRACE(parent);
    auto [batch, metadata] = objcFixture();
    auto &cls = *metadata.getArray("classes")->front().getAsObject();
    cls["root_class"] = false;
    cls["superclass"] = parent;
    if (parent == "Cycle")
      metadata.getArray("classes")->push_back(
          Object{{"name", "Cycle"},
                 {"root_class", false},
                 {"superclass", "Calculator"},
                 {"methods", Array{}}});
    (*batch.getArray("methods")
          ->front()
          .getAsObject())["instance_layout_classes"] = Array{};
    Budget budget;
    auto result = objcSources(batch, metadata, 8, budget);
    EXPECT_EQ(number(result.coverage, "recovered_method_count"), 0);
    EXPECT_EQ(result.source.find("@interface Calculator"), std::string::npos);
    EXPECT_EQ(result.source.find("@interface Cycle"), std::string::npos);
  }
}

TEST(MobileIOSNative, AvailableSuperclassDeclarationKeepsScalarRecovery) {
  for (const std::string parent :
       {"NSObject", "NSProxy", "NSCache", "NSDateFormatter", "NSURLSession",
        "LocalBase"}) {
    SCOPED_TRACE(parent);
    auto [batch, metadata] = objcFixture("return arg0 + arg1;", true);
    auto &cls = *metadata.getArray("classes")->front().getAsObject();
    cls["root_class"] = false;
    cls["superclass"] = parent;
    if (parent == "LocalBase")
      metadata.getArray("classes")->push_back(emptyLocalClass("LocalBase"));
    (*batch.getArray("methods")
          ->front()
          .getAsObject())["instance_layout_classes"] = Array{};
    Budget budget;
    auto result = objcSources(batch, metadata, 8, budget);
    EXPECT_EQ(number(result.coverage, "recovered_method_count"), 1);
    EXPECT_NE(result.source.find("@interface Calculator : " + parent),
              std::string::npos);
    EXPECT_NE(result.source.find("@implementation Calculator"),
              std::string::npos);
    if (parent == "LocalBase") {
      EXPECT_LT(result.source.find("@interface LocalBase"),
                result.source.find("@interface Calculator"));
      EXPECT_NE(result.source.find("@implementation LocalBase\n@end"),
                std::string::npos);
    }
  }
}

TEST(MobileIOSNative, EmptyLocalSuperclassNeedsCompleteInventoryAndLayout) {
  for (const std::string failure :
       {"metadata", "layout", "unrecovered-method", "ancestor-layout"}) {
    SCOPED_TRACE(failure);
    auto [batch, metadata] = objcFixture("return arg0 + arg1;", true);
    auto &child = *metadata.getArray("classes")->front().getAsObject();
    child["root_class"] = false;
    child["superclass"] = "LocalBase";
    auto parent = emptyLocalClass("LocalBase");
    if (failure == "metadata")
      metadata["status"] = "partial";
    else if (failure == "layout")
      parent["ivar_status"] = "unresolved";
    else if (failure == "unrecovered-method")
      parent["methods"] = Array{array(child, "methods")[0]};
    else {
      parent["superclass"] = "GrandBase";
      auto grandparent = emptyLocalClass("GrandBase");
      grandparent["ivar_status"] = "unresolved";
      metadata.getArray("classes")->push_back(std::move(grandparent));
    }
    metadata.getArray("classes")->push_back(std::move(parent));
    (*batch.getArray("methods")
          ->front()
          .getAsObject())["instance_layout_classes"] = Array{};
    Budget budget;
    auto result = objcSources(batch, metadata, 8, budget);
    EXPECT_EQ(number(result.coverage, "method_count"),
              failure == "unrecovered-method" ? 2 : 1);
    EXPECT_EQ(number(result.coverage, "recovered_method_count"), 0);
    const auto &row = object(array(result.coverage, "methods")[0], "method");
    EXPECT_NE(str(row, "reason").find("class definition"), std::string::npos);
    EXPECT_EQ(result.source.find("@implementation Calculator"),
              std::string::npos);
    EXPECT_EQ(result.source.find("@implementation LocalBase"),
              std::string::npos);
  }
}

TEST(MobileIOSNative, CategoryOnlyLocalOwnerNeedsItsOwnClassDefinition) {
  for (bool complete : {true, false}) {
    SCOPED_TRACE(complete);
    auto [batch, metadata] = objcFixture("return arg0 + arg1;", true);
    auto &native = *batch.getArray("methods")->front().getAsObject();
    native["category_name"] = "Arithmetic";
    native["category_address"] = "0x4000";
    native["instance_layout_classes"] = Array{};
    auto cls = emptyLocalClass("Calculator");
    auto method = object(
        array(object(array(metadata, "classes")[0], "class"), "methods")[0],
        "method");
    method["category_name"] = "Arithmetic";
    method["category_address"] = "0x4000";
    cls["methods"] = Array{std::move(method)};
    metadata["classes"] = Array{std::move(cls)};
    metadata["status"] = complete ? "recovered" : "partial";
    Budget budget;
    auto result = objcSources(batch, metadata, 8, budget);
    EXPECT_EQ(number(result.coverage, "method_count"), 1);
    EXPECT_EQ(number(result.coverage, "recovered_method_count"),
              complete ? 1 : 0);
    if (complete) {
      EXPECT_NE(result.source.find("@implementation Calculator\n@end"),
                std::string::npos);
      EXPECT_NE(result.source.find("@implementation Calculator (Arithmetic)"),
                std::string::npos);
    } else {
      EXPECT_EQ(result.source.find("@implementation Calculator"),
                std::string::npos);
    }
  }
}

TEST(MobileIOSNative, RecoveredSuperclassBodyIsReusedWithoutInventingAShell) {
  auto [batch, metadata] = objcFixture("return arg0 + arg1;", true);
  auto &child = *metadata.getArray("classes")->front().getAsObject();
  child["root_class"] = false;
  child["superclass"] = "LocalBase";
  auto parent = emptyLocalClass("LocalBase");
  parent["methods"] = Array{array(child, "methods")[0]};
  metadata.getArray("classes")->push_back(std::move(parent));
  metadata["status"] = "partial";
  auto &native = *batch.getArray("methods")->front().getAsObject();
  native["instance_layout_classes"] = Array{};
  Object parent_native(native);
  parent_native["class_name"] = "LocalBase";
  batch.getArray("methods")->push_back(std::move(parent_native));
  Budget budget;
  auto result = objcSources(batch, metadata, 8, budget);
  EXPECT_EQ(number(result.coverage, "method_count"), 2);
  EXPECT_EQ(number(result.coverage, "recovered_method_count"), 2);
  auto definition = result.source.find("@implementation LocalBase\n");
  ASSERT_NE(definition, std::string::npos);
  EXPECT_EQ(result.source.find("@implementation LocalBase\n", definition + 1),
            std::string::npos);
  EXPECT_EQ(result.source.find("@implementation LocalBase\n@end"),
            std::string::npos);
}

TEST(MobileIOSNative, ConflictingSuperclassBodiesInvalidateTheirDescendants) {
  auto [batch, metadata] = objcFixture("return arg0 + arg1;", true);
  auto &child = *metadata.getArray("classes")->front().getAsObject();
  child["root_class"] = false;
  child["superclass"] = "LocalBase";
  auto parent = emptyLocalClass("LocalBase");
  parent["methods"] = Array{array(child, "methods")[0]};
  auto independent = emptyLocalClass("Independent");
  independent["address"] = "0x4000";
  independent["methods"] = Array{array(child, "methods")[0]};
  metadata.getArray("classes")->push_back(std::move(parent));
  metadata.getArray("classes")->push_back(std::move(independent));
  auto &native = *batch.getArray("methods")->front().getAsObject();
  native["instance_layout_classes"] = Array{};
  Object parent_native(native), independent_native(native);
  parent_native["class_name"] = "LocalBase";
  independent_native["class_name"] = "Independent";
  parent_native["source"] =
      "extern int shared_dependency(void);\n" + str(parent_native, "source");
  independent_native["source"] = "extern double shared_dependency(void);\n" +
                                 str(independent_native, "source");
  batch.getArray("methods")->push_back(std::move(parent_native));
  batch.getArray("methods")->push_back(std::move(independent_native));
  Budget budget;
  auto result = objcSources(batch, metadata, 8, budget);
  EXPECT_EQ(number(result.coverage, "method_count"), 3);
  EXPECT_EQ(number(result.coverage, "recovered_method_count"), 0);
  const auto &row = object(array(result.coverage, "methods")[0], "method");
  EXPECT_NE(str(row, "reason").find("class definition"), std::string::npos);
  EXPECT_EQ(result.source.find("@implementation"), std::string::npos);
}

TEST(MobileIOSNative, ImportedDeclarationConflictDoesNotPoisonOtherClasses) {
  // NSTask is platform-specific; it must not evade the imported-name check
  // merely because it is absent from the portable superclass directory.
  for (const std::string name :
       {"NSObject", "NSTask", "NSRange", "NSUInteger", "int64_t", "size_t"}) {
    SCOPED_TRACE(name);
    auto [batch, metadata] = objcFixture("return arg0 + arg1;", true);
    auto &cls = *metadata.getArray("classes")->front().getAsObject();
    Object independent(cls);
    cls["name"] = name;
    metadata.getArray("classes")->push_back(std::move(independent));
    auto &native = *batch.getArray("methods")->front().getAsObject();
    native["instance_layout_classes"] = Array{};
    Object independent_method(native);
    native["class_name"] = name;
    batch.getArray("methods")->push_back(std::move(independent_method));
    Budget budget;
    auto result = objcSources(batch, metadata, 8, budget);
    EXPECT_EQ(number(result.coverage, "method_count"), 2);
    EXPECT_EQ(number(result.coverage, "recovered_method_count"), 1);
    EXPECT_EQ(str(result.coverage, "status"), "partial");
    EXPECT_EQ(result.source.find("@interface " + name), std::string::npos);
    EXPECT_EQ(result.source.find("@class " + name + ";"), std::string::npos);
    EXPECT_EQ(result.source.find("@implementation " + name), std::string::npos);
    EXPECT_NE(result.source.find("@implementation Calculator"),
              std::string::npos);
  }
}
TEST(MobileIOSNative, ParsesStructuredSwiftFunction) {
  auto s = swiftSignature("0x1000", "$s4Demo6answers5Int32VyF");
  EXPECT_EQ(str(s, "status"), "supported");
  EXPECT_EQ(str(s, "classification"), "callable");
  EXPECT_EQ(str(s, "name"), "answer");
  ASSERT_TRUE(s.getObject("return_type"));
  EXPECT_EQ(number(*s.getObject("return_type"), "bits"), 32);
}
TEST(MobileIOSNative, RetainsUnsupportedSwiftCallableAndMalformedSymbol) {
  auto s = swiftSignature("0x1000", "$s4Demo3BoxC5values5Int64Vvr");
  EXPECT_EQ(str(s, "classification"), "callable");
  EXPECT_EQ(str(s, "status"), "unsupported");
  s = swiftSignature("0x1000", "$s4Demo6answers5Int32Vy");
  EXPECT_EQ(str(s, "classification"), "unknown");
  EXPECT_FALSE(str(s, "reason").empty());
}
TEST(MobileIOSNative, SwiftCompilerEntryRequiresSeparateNativeProof) {
  auto s = swiftSignature("0x1000", "$s4Demo3BoxCMa");
  EXPECT_EQ(str(s, "status"), "unsupported");
  EXPECT_TRUE(flag(s, "requires_runtime_source_proof"));
  EXPECT_EQ(str(s, "runtime_source_kind"), "type_metadata_accessor");
  EXPECT_EQ(str(s, "context_name"), "Box");
}
TEST(MobileIOSNative, EmptyAndUnsupportedSwiftSlicesRetainBuiltinIdentity) {
  for (unsigned pointer_size : {4u, 8u}) {
    TemporaryDirectory directory;
    Options options;
    options.executable = "deliberately-unavailable-native-tool";
    Array symbols;
    if (pointer_size == 4)
      symbols.push_back(
          Object{{"name", "$s4Demo6answers5Int32VyF"}, {"address", "0x1000"}});
    Budget budget;
    auto result =
        swiftSources(options, directory.path, directory.path / "unused-binary",
                     symbols, pointer_size, budget);
    auto stored = parseJSON(
        readFile(directory.path / "metadata/swift-signatures.json", 65536),
        "Swift signatures");
    const auto &inventory = object(stored, "Swift signatures");
    auto *demangler = inventory.getObject("demangler");
    ASSERT_NE(demangler, nullptr);
    EXPECT_EQ(str(*demangler, "execution"), "builtin");
    EXPECT_EQ(str(*demangler, "name"), "llvm-swift-demangle");
    EXPECT_EQ(number(inventory, "symbol_count"), pointer_size == 4 ? 1 : 0);
    EXPECT_FALSE(inventory.get("logs"));
    EXPECT_FALSE(fs::exists(directory.path / "logs"));
    EXPECT_EQ(str(result.coverage, "status"),
              pointer_size == 4 ? "unsupported-architecture" : "no-symbols");
  }
}
TEST(MobileIOSNative, SameEntryDistinctSwiftSymbolsHaveSeparateCoverage) {
  auto [inventory, batch] = swiftFixture(true);
  auto r = swiftCoverage(inventory, &batch);
  EXPECT_EQ(number(r, "recovered_method_count"), 2);
  EXPECT_EQ(array(r, "source_units").size(), 2u);
  for (const auto &u : array(r, "source_units"))
    EXPECT_FALSE(object(u, "unit").get("source"));
}
TEST(MobileIOSNative, RejectsUnknownDuplicateAndMismatchedSwiftUnitIdentity) {
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto [inventory, batch] = swiftFixture(true);
    auto &u = *(*batch.getArray("source_units"))[1].getAsObject();
    auto &id = *u.getArray("method_identities")->front().getAsObject();
    if (mode == 0)
      id["mangled_symbol"] = "$sUnknown";
    if (mode == 1)
      id["mangled_symbol"] = "$s4Demo0";
    if (mode == 2)
      (*u.getArray("method_entries"))[0] = "0x9999";
    EXPECT_THROW(swiftCoverage(inventory, &batch), Error);
  }
}
TEST(MobileIOSNative, RejectsSwiftSourceAndMethodCountFalseSuccess) {
  auto [inventory, batch] = swiftFixture();
  batch["source"] = "";
  EXPECT_THROW(swiftCoverage(inventory, &batch), Error);
  auto second = swiftFixture();
  second.second["method_count"] = 2;
  EXPECT_THROW(swiftCoverage(second.first, &second.second), Error);
}
TEST(MobileIOSNative, CompilerCoverageRequiresEvidenceAndActualTypeSourceUnit) {
  auto [inventory, batch] = swiftFixture();
  auto &s = *inventory.getArray("methods")->front().getAsObject();
  s["requires_runtime_source_proof"] = true;
  s["declaration_kind"] = "runtime";
  s["runtime_source_kind"] = "type_metadata_accessor";
  s["module"] = "Demo";
  s["context_name"] = "Box";
  auto &r = *batch.getArray("methods")->front().getAsObject();
  r["source_representation"] = "compiler-generated-from-type";
  r["compiler_projection_kind"] = "type_metadata_accessor";
  batch["source_body_method_count"] = 0;
  batch["compiler_projection_method_count"] = 1;
  EXPECT_THROW(swiftCoverage(inventory, &batch), Error);
  r["compiler_projection_evidence"] =
      Array{"Exact native metadata and return state."};
  EXPECT_THROW(swiftCoverage(inventory, &batch), Error);
  auto &u = *batch.getArray("source_units")->front().getAsObject();
  u["kind"] = "type";
  u["name"] = "Box";
  auto c = swiftCoverage(inventory, &batch);
  EXPECT_EQ(number(c, "source_body_method_count"), 0);
  EXPECT_EQ(number(c, "compiler_projection_method_count"), 1);
}

TEST(MobileIOSNative, SwiftNominalIndirectMetadataUsesResolvedNativeStorage) {
  neverd::BinaryImage image;
  image.Bits = neverd::Bitness::Bits64;
  auto &segment = image.addSegment("__DATA", 0x1000, 0x100);
  segment.FileSz = 0x100;
  segment.Data.resize(0x100);
  auto put = [&](unsigned offset, uint64_t value, unsigned width = 4) {
    for (unsigned i = 0; i < width; ++i)
      segment.Data[offset + i] = uint8_t(value >> (8 * i));
  };
  auto &section = image.addSection("__swift5_types", 0x1000, 4);
  section.FileSz = 4;
  put(0, 0x21); // Relative indirect slot at 0x1020.
  put(0x20, 0x1040, 8);
  put(0x40, 17); // struct descriptor
  put(0x48, 0x18);
  std::copy_n("Record", 7, segment.Data.begin() + 0x60);
  Budget budget;
  auto result = swiftMetadata(image, budget);
  ASSERT_EQ(array(result, "types").size(), 1u);
  EXPECT_EQ(str(object(array(result, "types")[0], "type"), "name"), "Record");
  image.MachOHasChainedFixups = true;
  EXPECT_EQ(str(swiftMetadata(image, budget), "status"), "partial");
  image.MachOResolvedChainedPointerSlots.insert(0x1020);
  EXPECT_EQ(str(swiftMetadata(image, budget), "status"), "recovered");
  // A mapped zero-fill region cannot supply the nominal type's name.
  segment.FileSz = 0x60;
  EXPECT_EQ(str(swiftMetadata(image, budget), "status"), "partial");
}

TEST(MobileIOSNative,
     SwiftInventoryRetainsUndefinedZeroAddressMetadataSymbols) {
  auto data = thin();
  integer(data, 16, 1);
  integer(data, 20, 24);
  integer(data, 32, 2);
  integer(data, 36, 24);
  integer(data, 40, 512);
  integer(data, 44, 2);
  integer(data, 48, 600);
  integer(data, 52, 100);
  integer(data, 512, 1);
  integer(data, 516, 0x01, 1); // N_UNDF | N_EXT, value zero.
  data.replace(601, 10, "_$sBi64_WV");
  integer(data, 528, 20);
  integer(data, 532, 0x0f, 1); // Defined function at 0x1000.
  integer(data, 536, 0x1000, 8);
  data.replace(620, 11, "_$s4DemoRun");
  neverd::BinaryImage image;
  image.Raw.assign(data.begin(), data.end());
  Budget budget;
  auto result = swiftMetadata(image, budget);
  const auto &symbols = array(result, "symbols");
  ASSERT_EQ(symbols.size(), 2u);
  const auto &undefined = object(symbols[0], "symbol");
  EXPECT_EQ(str(undefined, "name"), "_$sBi64_WV");
  EXPECT_EQ(str(undefined, "address"), "0x0");
  EXPECT_FALSE(flag(undefined, "defined"));
  EXPECT_EQ(str(object(symbols[1], "symbol"), "address"), "0x1000");
  EXPECT_TRUE(flag(object(symbols[1], "symbol"), "defined"));
}

TEST(MobileIOSNative, DuplicateRuntimeClassRecordsRemainInMethodDenominator) {
  Budget budget;
  auto [batch, metadata] = objcFixture();
  auto duplicate = object(array(metadata, "classes")[0], "class");
  duplicate["address"] = "0x3000";
  metadata.getArray("classes")->push_back(std::move(duplicate));
  auto result = objcSources(batch, metadata, 8, budget);
  EXPECT_EQ(number(result.coverage, "method_count"), 2);
  EXPECT_EQ(number(result.coverage, "recovered_method_count"), 0);
}

TEST(MobileIOSNative,
     MalformedObjCReportsCannotLoseBlockIdentityOrPointerWidth) {
  Budget budget;
  auto [batch, metadata] = objcFixture();
  auto &native = *batch.getArray("methods")->front().getAsObject();
  native["shared_block_functions"] = "invalid array";
  EXPECT_EQ(number(objcSources(batch, metadata, 8, budget).coverage,
                   "recovered_method_count"),
            0);
  native.erase("shared_block_functions");
  batch["pointer_size"] = 4;
  EXPECT_THROW(objcSources(batch, metadata, 8, budget), Error);
}

TEST(MobileIOSNative, MetadataOnlyAppUsesAppRelativeArtifactWithoutTools) {
  TemporaryDirectory directory;
  auto bundle = directory.path / pathFromUTF8("测试.app");
  fs::create_directories(bundle / "Frameworks");
  writeFile(
      bundle / "Info.plist",
      "<plist><dict><key>CFBundleExecutable</key><string>Main</"
      "string><key>CFBundleName</key><string>测试</string></dict></plist>");
  writeFile(bundle / "Main", thin(7));
  writeFile(bundle / "Frameworks/Inside", loadableThin());
  Options options;
  options.input = bundle;
  options.metadata_only = true;
  options.artifact = "Frameworks/Inside";
  options.executable = "deliberately-unavailable-native-tool";
  Budget budget;
  auto staging = directory.path / "output";
  fs::create_directory(staging);
  auto report = recoverIOS(options, staging, budget);
  EXPECT_EQ(str(report, "architecture"), "arm64");
  EXPECT_EQ(str(report, "selected_artifact"), "Frameworks/Inside");
  EXPECT_EQ(str(*report.getObject("bundle"), "CFBundleName"), "测试");
  EXPECT_FALSE(fs::exists(staging / "input"));
  EXPECT_TRUE(fs::is_regular_file(staging / "metadata/objc.h"));
  EXPECT_FALSE(fs::exists(staging / "sources"));
}

TEST(MobileIOSNative,
     MetadataOnlyOutputCreationDoesNotOverwriteExistingArtifact) {
  TemporaryDirectory directory;
  auto source = directory.path / "input.macho";
  writeFile(source, thin());
  auto staging = directory.path / "output";
  fs::create_directories(staging / "artifacts");
  writeFile(staging / "artifacts/selected.macho", "keep");
  Options options;
  options.input = source;
  options.metadata_only = true;
  Budget budget;
  EXPECT_THROW(recoverIOS(options, staging, budget), Error);
  EXPECT_EQ(readFile(staging / "artifacts/selected.macho", 16), "keep");
}
