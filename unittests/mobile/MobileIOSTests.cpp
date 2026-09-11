#include "MobileIOSInternal.h"
#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <chrono>
#include <limits>

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
void expectFailurePhases(const std::string &message, std::string_view prefix,
                         llvm::StringRef active,
                         const std::vector<std::string> &completed) {
  constexpr std::string_view marker = "\n[neverd-ios-phases] ";
  EXPECT_TRUE(message.starts_with(prefix));
  auto position = message.find(marker);
  ASSERT_NE(position, std::string::npos);
  EXPECT_EQ(message.find(marker, position + marker.size()), std::string::npos);
  auto text = message.substr(position + marker.size());
  EXPECT_EQ(text.find('\n'), std::string::npos);
  EXPECT_LT(text.size(), 4096u);
  auto value = parseJSON(text, "iOS phase diagnostic");
  const auto *diagnostic = value.getAsObject();
  ASSERT_NE(diagnostic, nullptr);
  EXPECT_EQ(diagnostic->size(), 4u);
  EXPECT_EQ(str(*diagnostic, "active_phase"), active);
  for (auto key : {"active_elapsed_ms", "elapsed_ms"}) {
    auto elapsed = diagnostic->getInteger(key);
    ASSERT_TRUE(elapsed.has_value());
    EXPECT_GE(*elapsed, 0);
  }
  const auto *phases = diagnostic->getArray("completed_phases");
  ASSERT_NE(phases, nullptr);
  ASSERT_EQ(phases->size(), completed.size());
  for (size_t i = 0; i < phases->size(); ++i) {
    const auto *phase = (*phases)[i].getAsObject();
    ASSERT_NE(phase, nullptr);
    EXPECT_EQ(phase->size(), 2u);
    EXPECT_EQ(str(*phase, "phase"), completed[i]);
    auto elapsed = phase->getInteger("elapsed_ms");
    ASSERT_TRUE(elapsed.has_value());
    EXPECT_GE(*elapsed, 0);
  }
}
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
void makeDylib(std::string &data) {
  constexpr size_t header = 32;
  constexpr char name[] = "@rpath/libMobileFixture.dylib";
  // sizeof(name) includes its NUL; Mach-O 64-bit commands align to 8 bytes.
  constexpr uint32_t length = (24 + sizeof(name) + 7) & ~size_t(7);
  if (data.size() < header)
    throw Error("dylib fixture lacks a Mach-O header");
  auto read = [&](size_t p) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
      value |= uint32_t(static_cast<unsigned char>(data[p + i])) << (i * 8);
    return value;
  };
  const auto count = read(16), bytes = read(20);
  if (read(0) != 0xfeedfacf || count == std::numeric_limits<uint32_t>::max() ||
      bytes > std::numeric_limits<uint32_t>::max() - length ||
      bytes > data.size() - header || length > data.size() - header - bytes)
    throw Error("dylib fixture lacks bounded load-command space");
  // Use existing header padding without changing the __TEXT file/VM extent.
  const size_t p = header + bytes;
  std::fill_n(data.begin() + p, length, '\0');
  integer(data, p, 0x0d); // LC_ID_DYLIB
  integer(data, p + 4, length);
  integer(data, p + 8, 24);          // dylib.name offset within this command
  integer(data, p + 16, 0x00010000); // current_version 1.0.0
  integer(data, p + 20, 0x00010000); // compatibility_version 1.0.0
  data.replace(p + 24, sizeof(name), name, sizeof(name));
  integer(data, 12, 6); // MH_DYLIB
  integer(data, 16, count + 1);
  integer(data, 20, bytes + length);
}
void appendTarget(std::string &data, uint32_t platform,
                  uint32_t minos = 0x00120000, uint32_t sdk = 0x001a0500,
                  const std::vector<std::pair<uint32_t, uint32_t>> &tools = {},
                  uint32_t command = 0x32) {
  const bool wide = (static_cast<unsigned char>(data[7]) & 1) != 0;
  const size_t header = wide ? 32 : 28;
  auto read = [&](size_t p) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
      value |= uint32_t(static_cast<unsigned char>(data[p + i])) << (i * 8);
    return value;
  };
  const auto count = read(16), bytes = read(20);
  const uint32_t length = command == 0x32 ? 24 + tools.size() * 8 : 16;
  const size_t p = header + bytes;
  if (p + length > data.size())
    data.resize(p + length, '\0');
  integer(data, 16, count + 1);
  integer(data, 20, bytes + length);
  integer(data, p, command);
  integer(data, p + 4, length);
  if (command == 0x32) {
    integer(data, p + 8, platform);
    integer(data, p + 12, minos);
    integer(data, p + 16, sdk);
    integer(data, p + 20, tools.size());
    for (size_t i = 0; i < tools.size(); ++i) {
      integer(data, p + 24 + i * 8, tools[i].first);
      integer(data, p + 28 + i * 8, tools[i].second);
    }
  } else {
    integer(data, p + 8, minos);
    integer(data, p + 12, sdk);
  }
}
std::string symbolTable(uint32_t count, bool wide = true,
                        const std::string &name = "_$s4Demo3fooyyF") {
  auto data = thin(wide ? 0x100000c : 12);
  const unsigned header = wide ? 32 : 28;
  const unsigned entry_size = wide ? 16 : 12;
  const uint32_t symbols = 4096, strings = symbols + count * entry_size;
  data.resize(strings + name.size() + 2, '\0');
  integer(data, 16, 1);
  integer(data, 20, 24);
  integer(data, header, 2);
  integer(data, header + 4, 24);
  integer(data, header + 8, symbols);
  integer(data, header + 12, count);
  integer(data, header + 16, strings);
  integer(data, header + 20, name.size() + 2);
  data.replace(strings + 1, name.size(), name);
  for (uint32_t i = 0; i < count; ++i) {
    const auto entry = symbols + i * entry_size;
    integer(data, entry, 1);
    integer(data, entry + 4, i + 1 == count ? 1 : 0x0f, 1);
    integer(data, entry + 8, i + 1 == count ? 0 : 0x1000 + i * 4, wide ? 8 : 4);
  }
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
std::pair<Object, Object> objcDiagnosticFixture() {
  auto result = objcFixture();
  auto &native = *result.first.getArray("methods")->front().getAsObject();
  auto &cls = *result.second.getArray("classes")->front().getAsObject();
  auto &runtime = *cls.getArray("methods")->front().getAsObject();
  for (auto *method : {&native, &runtime}) {
    (*method)["category_name"] = "";
    (*method)["category_address"] = "0x0";
  }
  native["diagnostics"] = Array{};
  return result;
}
void unavailableObjCDeclaration(Object &metadata) {
  auto &cls = *metadata.getArray("classes")->front().getAsObject();
  cls["root_class"] = false;
  cls["superclass"] = "ExternalBase";
}
constexpr char kUnavailableObjCDeclaration[] =
    "required class declaration is unavailable: Calculator: "
    "superclass declaration is unavailable: ExternalBase";
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

std::pair<Object, Object> emptyInitializerCoverageFixture() {
  const std::string source = "struct `Empty` {\n}\n";
  auto init = swiftSignature("0x1000", "$s4Demo5EmptyVACycfC");
  auto accessor = swiftSignature("0x2000", "$s4Demo5EmptyVMa");
  Array rows, identities;
  for (const auto *s : {&init, &accessor}) {
    Object id{{"entry", str(*s, "entry")},
              {"mangled_symbol", str(*s, "mangled_symbol")}};
    Object row = id;
    row["status"] = "recovered";
    row["declaration_kind"] = s == &init ? "initializer" : "runtime";
    row["source"] = source;
    row["source_representation"] = "compiler-generated-from-type";
    row["compiler_projection_kind"] =
        s == &init ? "empty_value_initializer" : "type_metadata_accessor";
    // Explicit serializer inputs; actual native proofs have separate CAPI
    // tests.
    row["compiler_projection_evidence"] = Array{"owned native proof contract"};
    rows.push_back(std::move(row));
    identities.push_back(std::move(id));
  }
  auto pair = swiftFixture(true);
  auto &inventory = pair.first;
  inventory["methods"] = Array{std::move(init), std::move(accessor)};
  inventory["supported_signature_count"] = 0;
  inventory["unsupported_signature_count"] = 2;
  auto &batch = pair.second;
  batch["methods"] = std::move(rows);
  batch["source"] = source + "\n";
  batch["source_body_method_count"] = 0;
  batch["compiler_projection_method_count"] = 2;
  batch["types"] = Array{Object{{"module", "Demo"},
                                {"name", "Empty"},
                                {"kind", "struct"},
                                {"status", "recovered"},
                                {"reason", ""},
                                {"size", 0},
                                {"alignment", 1},
                                {"fields", Array{}}}};
  batch["source_units"] =
      Array{Object{{"kind", "type"},
                   {"module", "Demo"},
                   {"name", "Empty"},
                   {"source", source},
                   {"method_entries", Array{"0x1000", "0x2000"}},
                   {"method_identities", std::move(identities)}}};
  return pair;
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
TEST(MobileIOSNative, BuildTargetKeepsExplicitPlatformAndToolVersions) {
  // arm64 is shared by device, simulator and macOS; it is not a platform tag.
  for (auto [platform, name] :
       {std::pair{1u, "macos"}, {2u, "ios"}, {7u, "ios-simulator"}}) {
    Budget budget;
    auto data = thin();
    appendTarget(
        data, platform, 0x00120003, 0x001a0500,
        {{1, 0x00110002}, {2, 0x00060302}, {3, 0x03f50101}, {999, 0xfffffeff}});
    auto selected = selectSlice(data, "arm64", budget);
    ASSERT_EQ(selected.build_targets.size(), 1u);
    const auto &target = selected.build_targets.front();
    EXPECT_EQ(target.command, 0x32u);
    EXPECT_EQ(target.platform, platform);
    EXPECT_EQ(target.minos, 0x00120003u);
    EXPECT_EQ(target.sdk, 0x001a0500u);
    ASSERT_EQ(target.tools.size(), 4u);
    EXPECT_EQ(target.tools[3].tool, 999u);
    EXPECT_EQ(target.tools[3].version, 0xfffffeffu);
    auto report = buildTargetMetadata(selected, budget);
    EXPECT_EQ(str(report, "status"), "known");
    EXPECT_EQ(str(report, "platform"), name);
    EXPECT_EQ(str(report, "minos"), "18.0.3");
    EXPECT_EQ(str(report, "sdk"), "26.5.0");
    const auto &record = object(array(report, "commands").front(), "target");
    const auto &unknown_tool = object(array(record, "tools")[3], "tool");
    EXPECT_EQ(str(unknown_tool, "tool"), "unknown");
    EXPECT_EQ(number(unknown_tool, "tool_id"), 999);
    EXPECT_EQ(str(unknown_tool, "version"), "65535.254.255");
  }
}
TEST(MobileIOSNative, FatBuildTargetComesFromTheSelectedSlice) {
  for (bool wide : {false, true}) {
    Budget budget;
    auto data = fat(wide);
    auto device = thin(12), simulator = thin();
    appendTarget(device, 2, 0x000c0000, 0x00120000, {{3, 0x01020304}});
    appendTarget(simulator, 7, 0x00120000, 0x001a0500, {{3, 0x05060708}});
    data.replace(4096, 4096, device);
    data.replace(8192, 4096, simulator);
    auto automatic = selectSlice(data, "auto", budget);
    auto explicit_arm = selectSlice(data, "arm", budget);
    ASSERT_EQ(automatic.build_targets.size(), 1u);
    ASSERT_EQ(explicit_arm.build_targets.size(), 1u);
    EXPECT_EQ(automatic.build_targets[0].platform, 7u);
    EXPECT_EQ(explicit_arm.build_targets[0].platform, 2u);
    EXPECT_EQ(automatic.build_targets[0].tools[0].version, 0x05060708u);
    EXPECT_EQ(explicit_arm.build_targets[0].tools[0].version, 0x01020304u);
  }
}
TEST(MobileIOSNative, BuildTargetPreservesMissingUnknownAndLegacyFacts) {
  Budget budget;
  auto missing =
      buildTargetMetadata(selectSlice(thin(), "auto", budget), budget);
  EXPECT_EQ(str(missing, "status"), "unknown");
  EXPECT_EQ(str(missing, "platform"), "unknown");
  EXPECT_TRUE(array(missing, "commands").empty());
  ASSERT_NE(missing.get("platform_id"), nullptr);
  EXPECT_EQ(*missing.get("platform_id"), Value(nullptr));
  auto data = thin();
  appendTarget(data, 999);
  auto unknown = buildTargetMetadata(selectSlice(data, "auto", budget), budget);
  EXPECT_EQ(str(unknown, "status"), "unknown");
  EXPECT_EQ(str(unknown, "platform"), "unknown");
  EXPECT_EQ(number(unknown, "platform_id"), 999);
  EXPECT_FALSE(str(unknown, "reason").empty());
  for (uint32_t cpu : {0x100000cu, 0x1000007u})
    for (uint32_t command : {0x24u, 0x25u, 0x2fu, 0x30u}) {
      data = thin(cpu);
      appendTarget(data, 0, 0x000c0304, 0x00120506, {}, command);
      auto selected = selectSlice(data, "auto", budget);
      ASSERT_EQ(selected.build_targets.size(), 1u);
      EXPECT_EQ(selected.build_targets[0].command, command);
      auto legacy = buildTargetMetadata(selected, budget);
      EXPECT_EQ(str(legacy, "status"), "unknown");
      EXPECT_EQ(str(legacy, "platform"), "unknown");
      EXPECT_EQ(str(legacy, "minos"), "12.3.4");
      EXPECT_EQ(str(legacy, "sdk"), "18.5.6");
      EXPECT_EQ(*legacy.get("platform_id"), Value(nullptr));
    }
}
TEST(MobileIOSNative, MultipleBuildTargetsRemainAmbiguousWithoutLastWins) {
  // A zippered dylib legitimately records macOS and Mac Catalyst. Preserve
  // repeated and conflicting records too, without selecting an SDK profile.
  for (auto [platform, minos, command] : {std::tuple{6u, 0x00120000u, 0x32u},
                                          {1u, 0x00120000u, 0x32u},
                                          {1u, 0x00130000u, 0x32u},
                                          {0u, 0x00120000u, 0x24u}}) {
    Budget budget;
    auto data = thin();
    makeDylib(data);
    appendTarget(data, 1);
    appendTarget(data, platform, minos, 0x001a0500, {}, command);
    auto selected = selectSlice(data, "auto", budget);
    ASSERT_EQ(selected.build_targets.size(), 2u);
    EXPECT_EQ(selected.build_targets[0].platform, 1u);
    EXPECT_EQ(selected.build_targets[1].platform, platform);
    EXPECT_EQ(selected.build_targets[1].minos, minos);
    auto report = buildTargetMetadata(selected, budget);
    EXPECT_EQ(str(report, "status"), "unknown");
    EXPECT_EQ(str(report, "platform"), "unknown");
    EXPECT_EQ(*report.get("platform_id"), Value(nullptr));
    EXPECT_EQ(array(report, "commands").size(), 2u);
    EXPECT_NE(str(report, "reason").find("multiple"), std::string::npos);
  }
}
TEST(MobileIOSNative, RejectsBuildTargetCommandAndToolTableTruncation) {
  Budget budget;
  for (uint32_t length : {8u, 16u, 20u}) {
    auto data = thin();
    appendTarget(data, 2);
    integer(data, 20, length);
    integer(data, 36, length);
    EXPECT_THROW(selectSlice(data, "auto", budget), Error);
  }
  for (uint32_t count : {0u, 2u, 0xffffffffu}) {
    auto data = thin();
    appendTarget(data, 2, 0x00120000, 0x001a0500, {{3, 1}});
    integer(data, 52, count); // One complete entry must mean exactly ntools=1.
    EXPECT_THROW(selectSlice(data, "auto", budget), Error);
  }
  auto data = thin();
  appendTarget(data, 2, 0x00120000, 0x001a0500, {{3, 1}});
  data.resize(32 + 24 + 4);
  EXPECT_THROW(selectSlice(data, "auto", budget), Error);
  data = thin();
  appendTarget(data, 0, 0x00120000, 0x001a0500, {}, 0x25);
  integer(data, 20, 8);
  integer(data, 36, 8);
  EXPECT_THROW(selectSlice(data, "auto", budget), Error);
  data = fat();
  auto malformed = thin(12);
  appendTarget(malformed, 2);
  integer(malformed, 28 + 20, 1);
  data.replace(4096, 4096, malformed);
  EXPECT_THROW(selectSlice(data, "arm64", budget), Error);
}
TEST(MobileIOSNative, BuildTargetToolsRespectWorkAndConstructionBudgets) {
  auto data = thin();
  appendTarget(data, 2, 0x00120000, 0x001a0500,
               {{1, 1}, {2, 2}, {3, 3}, {999, 4}});
  Budget exhausted;
  exhausted.remaining = 3;
  EXPECT_THROW(selectSlice(data, "auto", exhausted), Error);
  Budget budget;
  auto selected = selectSlice(data, "auto", budget);
  auto report = buildTargetMetadata(selected, budget);
  EXPECT_EQ(
      array(object(array(report, "commands")[0], "target"), "tools").size(),
      4u);
  EXPECT_EQ(budget.output_bytes, 0u);
  Limits limits;
  limits.max_bytes = 1024;
  Budget small(limits);
  EXPECT_THROW(buildTargetMetadata(selected, small), Error);
  EXPECT_EQ(small.output_bytes, 0u);
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

TEST(MobileIOSNative, NativeBackendReasonSurvivesDeclarationAndLayoutFailure) {
  for (bool layout_failure : {false, true})
    for (bool native_recovered : {false, true}) {
      SCOPED_TRACE(layout_failure);
      SCOPED_TRACE(native_recovered);
      auto [batch, metadata] = objcDiagnosticFixture();
      auto &cls = *metadata.getArray("classes")->front().getAsObject();
      auto &native = *batch.getArray("methods")->front().getAsObject();
      if (layout_failure) {
        cls["ivar_status"] = "unrecovered";
        native["instance_layout_classes"] = Array{"Calculator"};
      } else
        unavailableObjCDeclaration(metadata);
      constexpr char reason[] =
          "method has no complete typed source body (possibly limited by "
          "max-func)";
      if (!native_recovered) {
        native["status"] = "unrecovered";
        native["reason"] = reason;
      }
      native["diagnostics"] = Array{"retained native diagnostic"};
      Budget budget;
      auto result = objcSources(batch, metadata, 8, budget);
      EXPECT_EQ(number(result.coverage, "method_count"), 1);
      EXPECT_EQ(number(result.coverage, "recovered_method_count"), 0);
      EXPECT_EQ(number(result.coverage, "unrecovered_method_count"), 1);
      const auto &row = object(array(result.coverage, "methods")[0], "method");
      EXPECT_EQ(str(row, "status"), "unrecovered");
      EXPECT_EQ(str(row, "reason"),
                layout_failure
                    ? "required instance-variable layout is unavailable: "
                      "Calculator"
                    : kUnavailableObjCDeclaration);
      EXPECT_EQ(array(row, "diagnostics").size(), layout_failure ? 1U : 0U);
      const auto *backend = row.getObject("native_backend");
      ASSERT_NE(backend, nullptr);
      EXPECT_EQ(backend->size(), 3U);
      EXPECT_EQ(str(*backend, "status"),
                native_recovered ? "recovered" : "unrecovered");
      if (native_recovered) {
        ASSERT_NE(backend->get("reason"), nullptr);
        EXPECT_TRUE(backend->get("reason")->getAsNull().has_value());
      } else
        EXPECT_EQ(str(*backend, "reason"), reason);
      EXPECT_EQ(array(*backend, "diagnostics"),
                (Array{"retained native diagnostic"}));
      EXPECT_FALSE(backend->get("source"));
      EXPECT_FALSE(backend->get("parameters"));
      EXPECT_EQ(result.source.find("@implementation Calculator"),
                std::string::npos);
    }
}

TEST(MobileIOSNative, NativeBackendSummaryRequiresExactRuntimeIdentity) {
  for (const auto *field :
       {"class_name", "selector", "class_method", "category_name",
        "category_address", "implementation", "type_encoding"}) {
    SCOPED_TRACE(field);
    auto [batch, metadata] = objcDiagnosticFixture();
    unavailableObjCDeclaration(metadata);
    auto &native = *batch.getArray("methods")->front().getAsObject();
    if (std::string_view(field) == "class_method")
      native[field] = true;
    else if (std::string_view(field) == "implementation" ||
             std::string_view(field) == "category_address")
      native[field] = "0x9999";
    else
      native[field] = "Other";
    // An unmatched input must not inject its own purported matched summary.
    native["native_backend"] = Object{{"status", "recovered"}};
    Budget budget;
    auto result = objcSources(batch, metadata, 8, budget);
    const auto &rows = array(result.coverage, "methods");
    const bool same_key = std::string_view(field) == "implementation" ||
                          std::string_view(field) == "type_encoding";
    EXPECT_EQ(rows.size(), same_key ? 1U : 2U);
    EXPECT_EQ(number(result.coverage, "recovered_method_count"), 0);
    EXPECT_EQ(str(object(rows[0], "method"), "reason"),
              kUnavailableObjCDeclaration);
    for (const auto &value : rows)
      EXPECT_FALSE(object(value, "method").get("native_backend"));
  }
  for (const auto *field : {"class_method", "category_name", "category_address",
                            "implementation", "type_encoding"})
    for (bool wrong_type : {false, true}) {
      SCOPED_TRACE(field);
      SCOPED_TRACE(wrong_type);
      auto [batch, metadata] = objcDiagnosticFixture();
      unavailableObjCDeclaration(metadata);
      auto &native = *batch.getArray("methods")->front().getAsObject();
      if (wrong_type)
        native[field] = 0;
      else
        native.erase(field);
      Budget budget;
      const auto result = objcSources(batch, metadata, 8, budget);
      EXPECT_EQ(number(result.coverage, "method_count"), 1);
      const auto &row = object(array(result.coverage, "methods")[0], "method");
      EXPECT_EQ(str(row, "reason"), kUnavailableObjCDeclaration);
      EXPECT_FALSE(row.get("native_backend"));
    }
}

TEST(MobileIOSNative, NativeBackendSummaryRemainsSecondaryAfterClassClosure) {
  auto [batch, metadata] = objcDiagnosticFixture();
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
  Budget complete_budget;
  const auto complete = objcSources(batch, metadata, 8, complete_budget);
  ASSERT_EQ(number(complete.coverage, "recovered_method_count"), 1);
  EXPECT_FALSE(object(array(complete.coverage, "methods")[0], "method")
                   .get("native_backend"));
  metadata["status"] = "partial";
  Budget partial_budget;
  const auto partial = objcSources(batch, metadata, 8, partial_budget);
  EXPECT_EQ(number(partial.coverage, "method_count"), 1);
  EXPECT_EQ(number(partial.coverage, "recovered_method_count"), 0);
  const auto &row = object(array(partial.coverage, "methods")[0], "method");
  EXPECT_EQ(str(row, "reason"),
            "required local class definition is unavailable: Calculator: "
            "local class method inventory is incomplete");
  const auto *backend = row.getObject("native_backend");
  ASSERT_NE(backend, nullptr);
  EXPECT_EQ(str(*backend, "status"), "recovered");
  EXPECT_EQ(str(row, "status"), "unrecovered");
  EXPECT_EQ(partial.source.find("@implementation Calculator"),
            std::string::npos);
}

TEST(MobileIOSNative, AmbiguousNativeOrRuntimeRowsCannotPublishBackendSummary) {
  for (unsigned duplicate : {0U, 1U, 2U, 3U}) {
    SCOPED_TRACE(duplicate);
    auto [batch, metadata] = objcDiagnosticFixture();
    unavailableObjCDeclaration(metadata);
    if (duplicate < 2) {
      auto copy = object(array(batch, "methods")[0], "native");
      if (duplicate == 1)
        copy["diagnostics"] = Array{"different native result"};
      batch.getArray("methods")->push_back(std::move(copy));
    } else if (duplicate == 2) {
      auto *methods =
          metadata.getArray("classes")->front().getAsObject()->getArray(
              "methods");
      auto copy = methods->front();
      methods->push_back(std::move(copy));
    } else {
      auto copy = object(array(metadata, "classes")[0], "class");
      copy["address"] = "0x3000";
      metadata.getArray("classes")->push_back(std::move(copy));
    }
    Budget budget;
    const auto result = objcSources(batch, metadata, 8, budget);
    EXPECT_EQ(number(result.coverage, "method_count"), duplicate < 2 ? 1 : 2);
    EXPECT_EQ(number(result.coverage, "recovered_method_count"), 0);
    const auto &rows = array(result.coverage, "methods");
    for (const auto &value : rows)
      EXPECT_FALSE(object(value, "method").get("native_backend"));
    EXPECT_EQ(str(object(rows[0], "method"), "reason"),
              duplicate == 3 ? "duplicate inconsistent runtime class records"
                             : kUnavailableObjCDeclaration);
    if (duplicate >= 2)
      EXPECT_EQ(str(object(rows[1], "method"), "reason"),
                "duplicate runtime method identity");
  }
}

TEST(MobileIOSNative, InvalidOrExcessiveNativeDiagnosticPayloadIsUnavailable) {
  for (unsigned mutation = 0; mutation < 16; ++mutation) {
    SCOPED_TRACE(mutation);
    auto [batch, metadata] = objcDiagnosticFixture();
    unavailableObjCDeclaration(metadata);
    auto &native = *batch.getArray("methods")->front().getAsObject();
    native["status"] = "unrecovered";
    native["reason"] = "native proof failed";
    switch (mutation) {
    case 0:
      native.erase("status");
      break;
    case 1:
      native["status"] = "success";
      break;
    case 2:
      native["status"] = false;
      break;
    case 3:
      native.erase("reason");
      break;
    case 4:
      native["reason"] = "";
      break;
    case 5:
      native["reason"] = 1;
      break;
    case 6:
      native["reason"] = std::string(2049, 'x');
      break;
    case 7:
      native["reason"] = std::string("bad\0reason", 10);
      break;
    case 8:
      native.erase("diagnostics");
      break;
    case 9:
      native["diagnostics"] = "invalid array";
      break;
    case 10:
      native["diagnostics"] = Array{false};
      break;
    case 11:
      native["diagnostics"] = Array{""};
      break;
    case 12:
      native["diagnostics"] = Array{std::string(513, 'x')};
      break;
    case 13:
      native["diagnostics"] =
          Array{"1", "2", "3", "4", "5", "6", "7", "8", "9"};
      break;
    case 14:
      native["diagnostics"] = Array{std::string("bad\x7ftext", 8)};
      break;
    case 15:
      native["status"] = "recovered"; // A conflicting failure reason remains.
      break;
    }
    Budget budget;
    const auto result = objcSources(batch, metadata, 8, budget);
    EXPECT_EQ(number(result.coverage, "method_count"), 1);
    EXPECT_EQ(number(result.coverage, "recovered_method_count"), 0);
    const auto &row = object(array(result.coverage, "methods")[0], "method");
    EXPECT_EQ(str(row, "reason"), kUnavailableObjCDeclaration);
    EXPECT_FALSE(row.get("native_backend"));
  }
}

TEST(MobileIOSNative, NativeBackendSummaryHasAnAggregateBoundAndIsReadOnly) {
  auto [prototype_batch, prototype_metadata] = objcDiagnosticFixture();
  unavailableObjCDeclaration(prototype_metadata);
  Object batch{
      {"schema_version", 1}, {"pointer_size", 8}, {"methods", Array{}}};
  Object metadata{{"status", "recovered"}, {"classes", Array{}}};
  for (unsigned index = 0; index < 64; ++index) {
    const auto name = "Calculator" + std::to_string(index);
    auto cls = object(array(prototype_metadata, "classes")[0], "class");
    cls["name"] = name;
    auto native = object(array(prototype_batch, "methods")[0], "native");
    native["class_name"] = name;
    native["status"] = "unrecovered";
    native["reason"] = std::string(2048, 'r');
    Array diagnostics;
    for (unsigned message = 0; message < 8; ++message)
      diagnostics.push_back(std::string(512, 'd'));
    native["diagnostics"] = std::move(diagnostics);
    batch.getArray("methods")->push_back(std::move(native));
    metadata.getArray("classes")->push_back(std::move(cls));
  }
  const auto original_batch = jsonText(Value(Object(batch)));
  const auto original_metadata = jsonText(Value(Object(metadata)));
  Budget budget;
  const auto result = objcSources(batch, metadata, 8, budget);
  EXPECT_EQ(number(result.coverage, "method_count"), 64);
  EXPECT_EQ(number(result.coverage, "recovered_method_count"), 0);
  EXPECT_EQ(number(result.coverage, "unrecovered_method_count"), 64);
  size_t summaries = 0, bytes = 0;
  for (const auto &value : array(result.coverage, "methods")) {
    const auto &row = object(value, "method");
    EXPECT_EQ(str(row, "status"), "unrecovered");
    EXPECT_NE(str(row, "reason").find("required class declaration"),
              std::string::npos);
    if (auto backend = row.getObject("native_backend")) {
      ++summaries;
      bytes += jsonText(Value(Object(*backend))).size();
      EXPECT_EQ(str(*backend, "reason"), std::string(2048, 'r'));
      EXPECT_EQ(array(*backend, "diagnostics").size(), 8U);
    }
  }
  EXPECT_GT(summaries, 0U);
  EXPECT_LT(summaries, 64U);
  EXPECT_LE(bytes, 256U * 1024U);
  EXPECT_EQ(jsonText(Value(Object(batch))), original_batch);
  EXPECT_EQ(jsonText(Value(Object(metadata))), original_metadata);
}

TEST(MobileIOSNative,
     NativeBackendSummaryLeavesSuccessAndBudgetsAuthoritative) {
  auto [baseline_batch, baseline_metadata] = objcDiagnosticFixture();
  Budget baseline_budget;
  auto baseline =
      objcSources(baseline_batch, baseline_metadata, 8, baseline_budget);
  auto [batch, metadata] = objcDiagnosticFixture();
  // Keep runtime identity identical: it determines generated source names.
  // Only the native diagnostic differs between these successful recoveries.
  (*batch.getArray("methods")->front().getAsObject())["diagnostics"] =
      Array{"ignored diagnostic on recovered method"};
  Budget successful_budget;
  const auto successful = objcSources(batch, metadata, 8, successful_budget);
  EXPECT_EQ(successful.source, baseline.source);
  EXPECT_EQ(Value(Object(successful.coverage)),
            Value(Object(baseline.coverage)));
  EXPECT_EQ(number(successful.coverage, "recovered_method_count"), 1);
  EXPECT_FALSE(object(array(successful.coverage, "methods")[0], "method")
                   .get("native_backend"));
  unavailableObjCDeclaration(metadata);
  Budget control;
  const auto before = control.remaining;
  const auto failed = objcSources(batch, metadata, 8, control);
  const auto work = before - control.remaining;
  ASSERT_GT(work, 0U);
  ASSERT_NE(object(array(failed.coverage, "methods")[0], "method")
                .getObject("native_backend"),
            nullptr);
  Budget insufficient;
  insufficient.remaining = work - 1;
  EXPECT_THROW(objcSources(batch, metadata, 8, insufficient), Error);
  Budget expired;
  expired.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  EXPECT_THROW(objcSources(batch, metadata, 8, expired), Error);
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

TEST(MobileIOSNative,
     EmptyInitializerCoverageKeepsDeclarationAndTwoIdentities) {
  auto [inventory, batch] = emptyInitializerCoverageFixture();
  const auto coverage = swiftCoverage(inventory, &batch);
  EXPECT_EQ(number(coverage, "method_count"), 2);
  EXPECT_EQ(number(coverage, "recovered_method_count"), 2);
  EXPECT_EQ(number(coverage, "source_body_method_count"), 0);
  EXPECT_EQ(number(coverage, "compiler_projection_method_count"), 2);
  const auto &methods = array(coverage, "methods");
  ASSERT_EQ(methods.size(), 2U);
  const auto &init = object(methods[0], "initializer");
  EXPECT_EQ(str(init, "declaration_kind"), "initializer");
  EXPECT_EQ(str(init, "node_kind"), "Allocator");
  EXPECT_EQ(str(init, "compiler_projection_kind"), "empty_value_initializer");
  EXPECT_EQ(str(init, "signature_status"), "unsupported");
  const auto &units = array(coverage, "source_units");
  ASSERT_EQ(units.size(), 1U);
  EXPECT_EQ(array(object(units[0], "unit"), "method_identities").size(), 2U);
}

TEST(MobileIOSNative, EmptyInitializerCoverageRejectsFalseProjectionContracts) {
  for (unsigned mutation = 0; mutation < 12; ++mutation) {
    SCOPED_TRACE(mutation);
    auto [inventory, batch] = emptyInitializerCoverageFixture();
    auto &init = *inventory.getArray("methods")->front().getAsObject();
    auto &row = *batch.getArray("methods")->front().getAsObject();
    auto &type = *batch.getArray("types")->front().getAsObject();
    auto &unit = *batch.getArray("source_units")->front().getAsObject();
    switch (mutation) {
    case 0:
      unit["method_entries"] = Array{"0x1000"};
      unit.getArray("method_identities")->pop_back();
      break;
    case 1: {
      auto &accessor = *(*batch.getArray("methods"))[1].getAsObject();
      accessor["compiler_projection_kind"] = "trivial_destructor";
      break;
    }
    case 2:
      type["size"] = 8;
      break;
    case 3:
      type["fields"] = Array{Object{{"name", "value"}}};
      break;
    case 4:
      init["node_kind"] = "Constructor";
      break;
    case 5:
      init["parameters"] = Array{Object{{"name", "arg0"}}};
      break;
    case 6:
      (*init.getObject("return_type"))["name"] = "Other";
      break;
    case 7:
      init["requires_runtime_source_proof"] = true;
      break;
    case 8:
      row["declaration_kind"] = "runtime";
      break;
    case 9: {
      const std::string changed = "struct `Empty` { init() {} }\n";
      batch["source"] = changed + "\n";
      unit["source"] = changed;
      for (auto &value : *batch.getArray("methods"))
        (*value.getAsObject())["source"] = changed;
      break;
    }
    case 10:
      row["compiler_projection_evidence"] = Array{};
      break;
    case 11:
      type["status"] = "unrecovered";
      break;
    }
    EXPECT_THROW(swiftCoverage(inventory, &batch), Error);
  }
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

TEST(MobileIOSNative, SymbolInventoryAboveFormerLimitPreservesAllRecords) {
  constexpr uint32_t count = 100001;
  for (bool wide : {false, true}) {
    SCOPED_TRACE(wide);
    auto data = symbolTable(count, wide);
    Budget budget;
    budget.output_bytes = 17;
    auto selected = selectSlice(data, "auto", budget);
    EXPECT_EQ(selected.bytes, data);
    EXPECT_EQ(selected.pointer_size, wide ? 8u : 4u);
    neverd::BinaryImage image;
    image.Raw.assign(data.begin(), data.end());
    auto result = swiftMetadata(image, budget);
    const auto &symbols = array(result, "symbols");
    ASSERT_EQ(symbols.size(), count);
    const auto &first = object(symbols.front(), "symbol");
    const auto &penultimate = object(symbols[count - 2], "symbol");
    const auto &last = object(symbols.back(), "symbol");
    EXPECT_EQ(str(first, "name"), "_$s4Demo3fooyyF");
    EXPECT_EQ(str(first, "address"), "0x1000");
    EXPECT_TRUE(flag(first, "defined"));
    EXPECT_EQ(str(penultimate, "address"), hex(0x1000 + (count - 2) * 4));
    EXPECT_EQ(str(last, "name"), str(first, "name"));
    EXPECT_EQ(str(last, "address"), "0x0");
    EXPECT_FALSE(flag(last, "defined"));
    EXPECT_EQ(budget.output_bytes, 17u);
  }
}

TEST(MobileIOSNative, SymbolInventoryStillRejectsMalformedRangesAndNames) {
  for (unsigned mutation = 0; mutation < 4; ++mutation) {
    SCOPED_TRACE(mutation);
    auto data = symbolTable(1);
    if (mutation == 0)
      integer(data, 44, 0xffffffffu); // Declared nlist storage is absent.
    else if (mutation == 1)
      integer(data, 4096, 0xffffffffu); // Invalid string-table index.
    else if (mutation == 2)
      data.back() = 'x'; // No NUL terminator within the string table.
    else
      data = symbolTable(1, true, "_$s" + std::string(16384, 'x'));
    Budget selection_budget, metadata_budget;
    EXPECT_THROW(selectSlice(data, "auto", selection_budget), Error);
    neverd::BinaryImage image;
    image.Raw.assign(data.begin(), data.end());
    EXPECT_THROW(swiftMetadata(image, metadata_budget), Error);
  }
}

TEST(MobileIOSNative, SymbolInventoryHonorsInputWorkAndDeadlineBudgets) {
  auto data = symbolTable(8);
  neverd::BinaryImage image;
  image.Raw.assign(data.begin(), data.end());
  for (unsigned limit = 0; limit < 3; ++limit) {
    SCOPED_TRACE(limit);
    Limits limits;
    if (limit == 0)
      limits.max_bytes = data.size() - 1;
    Budget selection_budget(limits), metadata_budget(limits);
    if (limit == 1) {
      selection_budget.remaining = metadata_budget.remaining = 3;
    } else if (limit == 2) {
      selection_budget.deadline = metadata_budget.deadline =
          std::chrono::steady_clock::now() - std::chrono::seconds(1);
    }
    EXPECT_THROW(selectSlice(data, "auto", selection_budget), Error);
    EXPECT_THROW(swiftMetadata(image, metadata_budget), Error);
  }
}

TEST(MobileIOSNative, SymbolMetadataDoesNotConsumePublicationBudget) {
  auto data = symbolTable(8);
  Limits limits;
  limits.max_bytes = data.size();
  Budget budget(limits);
  budget.output_bytes = limits.max_bytes - 1;
  neverd::BinaryImage image;
  image.Raw.assign(data.begin(), data.end());
  auto result = swiftMetadata(image, budget);
  EXPECT_EQ(array(result, "symbols").size(), 8u);
  EXPECT_EQ(budget.output_bytes, limits.max_bytes - 1);
}

TEST(MobileIOSNative,
     RepeatedSymbolNamesCannotAmplifyBeyondConstructionBudget) {
  auto data = symbolTable(4000, true, "_$s" + std::string(900, 'x'));
  Limits limits;
  limits.max_bytes = data.size();
  Budget budget(limits);
  budget.output_bytes = 17;
  neverd::BinaryImage image;
  image.Raw.assign(data.begin(), data.end());
  try {
    swiftMetadata(image, budget);
    FAIL() << "repeated names exceeded the metadata construction byte limit";
  } catch (const Error &error) {
    EXPECT_STREQ(error.what(), "metadata construction exceeds byte budget");
  }
  EXPECT_EQ(budget.output_bytes, 17u);
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
  auto inside = loadableThin();
  appendTarget(inside, 7, 0x00120000, 0x001a0500, {{3, 0x03f50101}});
  writeFile(bundle / "Frameworks/Inside", inside);
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
  const auto *target = report.getObject("build_target");
  ASSERT_NE(target, nullptr);
  EXPECT_EQ(str(*target, "status"), "known");
  EXPECT_EQ(str(*target, "platform"), "ios-simulator");
  EXPECT_EQ(number(*target, "platform_id"), 7);
  EXPECT_EQ(str(*target, "minos"), "18.0.0");
  EXPECT_EQ(str(*target, "sdk"), "26.5.0");
  const auto &command = object(array(*target, "commands").front(), "target");
  EXPECT_EQ(number(command, "load_command_index"), 1);
  EXPECT_EQ(number(object(array(command, "tools")[0], "tool"), "tool_id"), 3);
  EXPECT_FALSE(fs::exists(staging / "input"));
  EXPECT_TRUE(fs::is_regular_file(staging / "metadata/objc.h"));
  EXPECT_FALSE(fs::exists(staging / "sources"));
}

TEST(MobileIOSNative, RecoveryFailureReportsSlicePhaseAndPreservesCleanup) {
  TemporaryDirectory directory;
  Options options;
  options.input = directory.path / "invalid-input.macho";
  options.output = directory.path / "output";
  options.platform = "ios";
  writeFile(options.input, std::string(32, '\0'));
  try {
    recover(options);
    FAIL() << "invalid image unexpectedly recovered";
  } catch (const Error &error) {
    expectFailurePhases(error.what(),
                        "expected a little-endian Mach-O executable",
                        "slice_selection", {"input_staging"});
  }
  EXPECT_FALSE(fs::exists(options.output));
  for (const auto &entry : fs::directory_iterator(directory.path))
    EXPECT_EQ(entry.path(), options.input);
}

TEST(MobileIOSNative, RecoveryFailureReportsWorkerLaunchBeforeMetadata) {
  TemporaryDirectory directory;
  Options options;
  options.input = directory.path / "input.macho";
  options.output = directory.path / "output";
  options.platform = "ios";
  options.architecture = "arm64";
  options.executable = pathText(directory.path / "missing-native-backend");
  writeFile(options.input, loadableThin());
  ASSERT_FALSE(fs::exists(pathFromUTF8(options.executable)));
  try {
    recover(options);
    FAIL() << "missing native backend unexpectedly recovered";
  } catch (const Error &error) {
    expectFailurePhases(error.what(),
                        "cannot execute backend " + options.executable,
                        "native_export",
                        {"input_staging", "slice_selection", "build_target",
                         "worker_preparation"});
  }
  EXPECT_FALSE(fs::exists(options.output));
  for (const auto &entry : fs::directory_iterator(directory.path))
    EXPECT_EQ(entry.path(), options.input);
}

TEST(MobileIOSNative, MetadataOnlyRecoveryDoesNotPublishFailureDiagnostics) {
  TemporaryDirectory directory;
  Options options;
  options.input = directory.path / "input.macho";
  options.output = directory.path / "output";
  options.platform = "ios";
  options.architecture = "arm64";
  options.metadata_only = true;
  options.executable = pathText(directory.path / "missing-native-backend");
  writeFile(options.input, loadableThin());
  auto report = recover(options);
  EXPECT_EQ(str(report, "status"), "success");
  EXPECT_TRUE(flag(report, "metadata_only"));
  EXPECT_TRUE(fs::is_regular_file(options.output / "artifacts/selected.macho"));
  EXPECT_TRUE(fs::is_regular_file(options.output / "metadata/objc.json"));
  EXPECT_TRUE(fs::is_regular_file(options.output / "metadata/objc.h"));
  EXPECT_TRUE(fs::is_regular_file(options.output / "metadata/swift.json"));
  EXPECT_FALSE(fs::exists(options.output / "sources"));
  EXPECT_FALSE(fs::exists(options.output / "input"));
  for (auto key :
       {"active_phase", "active_elapsed_ms", "elapsed_ms", "completed_phases"})
    EXPECT_EQ(report.get(key), nullptr);
  auto text =
      readFile(options.output / "report.json", options.limits.max_bytes);
  EXPECT_EQ(text.find("[neverd-ios-phases]"), std::string::npos);
  EXPECT_EQ(parseJSON(text, "successful mobile report"), Value(Object(report)));
  for (const auto &entry : fs::directory_iterator(directory.path))
    EXPECT_FALSE(
        pathText(entry.path().filename()).starts_with(".neverd-mobile-"));
}

TEST(MobileIOSNative, MetadataOnlyPreservesMissingAndZipperedBuildTargets) {
  for (bool zippered : {false, true}) {
    TemporaryDirectory directory;
    auto data = loadableThin();
    if (zippered) {
      makeDylib(data);
      appendTarget(data, 1, 0x000e0000, 0x001a0500);
      appendTarget(data, 6, 0x00120000, 0x001a0500);
    }
    Options options;
    options.input = directory.path / "input.macho";
    options.metadata_only = true;
    options.executable = "deliberately-unavailable-native-tool";
    writeFile(options.input, data);
    auto staging = directory.path / "output";
    fs::create_directory(staging);
    Budget budget;
    auto report = recoverIOS(options, staging, budget);
    const auto *target = report.getObject("build_target");
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(str(*target, "status"), "unknown");
    EXPECT_EQ(str(*target, "platform"), "unknown");
    EXPECT_EQ(*target->get("platform_id"), Value(nullptr));
    EXPECT_EQ(array(*target, "commands").size(), zippered ? 2u : 0u);
    EXPECT_TRUE(fs::exists(staging / "metadata/objc.h"));
    EXPECT_FALSE(fs::exists(staging / "sources"));
  }
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

namespace {
WorkerRequest workerRequestFixture(const Budget &parent) {
  WorkerRequest request;
  request.limits = parent.limits;
  request.architecture = "arm64";
  request.pointer_size = 8;
  request.selected_sha256 = std::string(64, 'a');
  request.max_functions = 37;
  request.remaining = parent.remaining;
  request.output_bytes = parent.output_bytes;
  request.time_remaining_ms = 1234;
  return request;
}
} // namespace

TEST(MobileIOSNative, WorkerRequestPreservesOriginalLimitsAndSpentCounters) {
  Budget parent({17, 456, 9000000});
  parent.tick(123);
  parent.output(789);
  for (auto architecture : {"arm64", "x86_64", "arm", "i386"}) {
    auto request = workerRequestFixture(parent);
    request.architecture = architecture;
    request.pointer_size = request.architecture == "arm" ||
                                   request.architecture == "i386"
                               ? 4
                               : 8;
    request.max_functions = std::numeric_limits<size_t>::max();
    const auto encoded = workerRequestJSON(request);
    const auto text = jsonText(Value(Object(encoded)));
    EXPECT_LT(text.size(), WorkerRequestByteLimit);
    const auto parsed = parseJSON(text, "owned worker request");
    auto decoded = parseWorkerRequest(object(parsed, "owned worker request"));
    EXPECT_EQ(decoded.architecture, request.architecture);
    EXPECT_EQ(decoded.pointer_size, request.pointer_size);
    EXPECT_EQ(decoded.selected_sha256, std::string(64, 'a'));
    EXPECT_EQ(decoded.limits.timeout, 17u);
    EXPECT_EQ(decoded.limits.max_files, 456u);
    EXPECT_EQ(decoded.limits.max_bytes, 9000000u);
    EXPECT_EQ(decoded.remaining, 20000000u - 123);
    EXPECT_EQ(decoded.output_bytes, 789u);
    EXPECT_EQ(decoded.time_remaining_ms, 1234u);
    EXPECT_EQ(decoded.max_functions, std::numeric_limits<size_t>::max());
    if constexpr (sizeof(size_t) > sizeof(unsigned)) {
      request.max_functions = uint64_t(std::numeric_limits<unsigned>::max()) + 1;
      EXPECT_EQ(parseWorkerRequest(workerRequestJSON(request)).max_functions,
                uint64_t(std::numeric_limits<unsigned>::max()) + 1);
    }
  }
}

TEST(MobileIOSNative, WorkerRequestRejectsNoncanonicalAndRenewedBudgets) {
  Budget parent({17, 456, 9000000});
  const auto valid = workerRequestJSON(workerRequestFixture(parent));
  for (auto field : {"max_functions", "remaining", "output_bytes",
                     "time_remaining_ms"}) {
    for (auto spelling : {"", "00", "+1", "-1", "1x", "1.0", " 1",
                           "18446744073709551616"}) {
      SCOPED_TRACE(std::string(field) + ":" + spelling);
      Object malformed(valid);
      malformed[field] = spelling;
      EXPECT_THROW(parseWorkerRequest(malformed), Error);
    }
    for (int kind = 0; kind != 4; ++kind) {
      SCOPED_TRACE(std::string(field) + ":type:" + std::to_string(kind));
      Object malformed(valid);
      if (kind == 0)
        malformed.erase(field);
      else if (kind == 1)
        malformed[field] = 1;
      else if (kind == 2)
        malformed[field] = true;
      else
        malformed[field] = 1.0;
      EXPECT_THROW(parseWorkerRequest(malformed), Error);
    }
  }
  for (unsigned mutation = 0; mutation != 13; ++mutation) {
    SCOPED_TRACE(mutation);
    Object malformed(valid);
    switch (mutation) {
    case 0: malformed["remaining"] = "20000001"; break;
    case 1: malformed["output_bytes"] = "9000001"; break;
    case 2: malformed["time_remaining_ms"] = "0"; break;
    case 3: malformed["time_remaining_ms"] = "17001"; break;
    case 4: malformed["architecture"] = "auto"; break;
    case 5: malformed["pointer_size"] = 4; break;
    case 6: malformed["selected_sha256"] = std::string(64, 'A'); break;
    case 7: malformed["selected_sha256"] = std::string(63, 'a'); break;
    case 8: malformed["schema_version"] = 2; break;
    case 9: malformed["extra"] = "unknown"; break;
    case 10: (*malformed.getObject("limits"))["timeout"] = "0"; break;
    case 11: (*malformed.getObject("limits"))["max_bytes"] = "0"; break;
    case 12: (*malformed.getObject("limits"))["extra"] = "0"; break;
    }
    EXPECT_THROW(parseWorkerRequest(malformed), Error);
  }
}

TEST(MobileIOSNative, WorkerEnvelopeMergesConsumptionWithoutRenewingDeadline) {
  Budget parent({17, 456, 9000000});
  parent.tick(123);
  parent.output(789);
  const auto request = workerRequestFixture(parent);
  Budget child(parent);
  child.tick(321);
  child.output(456);
  auto result = workerResultJSON(request, child, Object{});
  const auto deadline = parent.deadline;
  mergeWorkerBudget(result, request, parent);
  EXPECT_EQ(parent.remaining, 20000000u - 123 - 321);
  EXPECT_EQ(parent.output_bytes, 789u + 456);
  EXPECT_EQ(parent.deadline, deadline);
  EXPECT_EQ(parent.limits.timeout, 17u);
  EXPECT_EQ(parent.limits.max_files, 456u);
  EXPECT_EQ(parent.limits.max_bytes, 9000000u);
  EXPECT_THROW(mergeWorkerBudget(result, request, parent), Error);
  EXPECT_EQ(parent.remaining, 20000000u - 123 - 321);
  EXPECT_EQ(parent.output_bytes, 789u + 456);
}

TEST(MobileIOSNative, WorkerEnvelopeRejectsWrongIdentityAndCounterReversal) {
  Budget initial({17, 456, 9000000});
  initial.tick(123);
  initial.output(789);
  const auto request = workerRequestFixture(initial);
  Budget child(initial);
  child.tick(321);
  child.output(456);
  const auto valid = workerResultJSON(request, child, Object{});
  for (unsigned mutation = 0; mutation != 12; ++mutation) {
    SCOPED_TRACE(mutation);
    Budget parent(initial);
    Object result(valid);
    switch (mutation) {
    case 0: result["architecture"] = "x86_64"; break;
    case 1: result["pointer_size"] = 4; break;
    case 2: result["selected_sha256"] = std::string(64, 'b'); break;
    case 3: result["remaining"] = std::to_string(request.remaining + 1); break;
    case 4: result["output_bytes"] = "788"; break;
    case 5: result["output_bytes"] = "9000001"; break;
    case 6: result["remaining"] = "01"; break;
    case 7: result["status"] = "failed"; break;
    case 8: result.erase("report"); break;
    case 9: result["report"] = Array{}; break;
    case 10: result["schema_version"] = 2; break;
    case 11: result["extra"] = "unknown"; break;
    }
    EXPECT_THROW(mergeWorkerBudget(result, request, parent), Error);
    EXPECT_EQ(parent.remaining, initial.remaining);
    EXPECT_EQ(parent.output_bytes, initial.output_bytes);
    EXPECT_EQ(parent.deadline, initial.deadline);
  }
  for (unsigned mutation = 0; mutation != 6; ++mutation) {
    SCOPED_TRACE(mutation);
    Budget parent(initial);
    switch (mutation) {
    case 0: --parent.remaining; break;
    case 1: ++parent.output_bytes; break;
    case 2: ++parent.limits.timeout; break;
    case 3: ++parent.limits.max_files; break;
    case 4: ++parent.limits.max_bytes; break;
    case 5:
      parent.deadline = std::chrono::steady_clock::now() -
                        std::chrono::seconds(1);
      break;
    }
    const auto remaining = parent.remaining;
    const auto output = parent.output_bytes;
    EXPECT_THROW(mergeWorkerBudget(valid, request, parent), Error);
    EXPECT_EQ(parent.remaining, remaining);
    EXPECT_EQ(parent.output_bytes, output);
  }
}
