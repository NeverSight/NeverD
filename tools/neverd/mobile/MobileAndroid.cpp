//===- MobileAndroid.cpp - Native Android staging and source recovery
//------===//
#include "MobileCommon.h"
#include "MobileDalvik.h"

#include "llvm/Support/Regex.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <tuple>

namespace neverd::mobile {
namespace {
using llvm::json::Array;
using llvm::json::Object;
using llvm::json::Value;
struct WorkDirectory {
  fs::path path;
  bool active = true;
  explicit WorkDirectory(const fs::path &staging)
      : path(staging / ".android-work") {
    if (!fs::create_directory(path))
      throw Error("Android workspace must be a new exclusive directory");
  }
  void clear() {
    fs::remove_all(path);
    active = false;
  }
  ~WorkDirectory() {
    if (active) {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  }
};
Array strings(const std::vector<std::string> &values) {
  Array result;
  for (auto &value : values)
    result.push_back(value);
  return result;
}
std::string trim(std::string_view value) {
  auto white = [](unsigned char c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
  };
  while (!value.empty() && white(uint8_t(value.front())))
    value.remove_prefix(1);
  while (!value.empty() && white(uint8_t(value.back())))
    value.remove_suffix(1);
  return std::string(value);
}
std::vector<std::string> lines(std::string_view text) {
  std::vector<std::string> result;
  size_t start = 0;
  for (size_t i = 0; i <= text.size(); ++i)
    if (i == text.size() || text[i] == '\n' || text[i] == '\r') {
      result.emplace_back(text.substr(start, i - start));
      start = i + 1;
    }
  return result;
}
void copyInput(const fs::path &source, const fs::path &destination,
               const Limits &limits) {
  // readFile rejects links and nonregular input; writeFile creates exclusively.
  auto bytes = readFile(source, limits.max_bytes);
  writeFile(destination, bytes);
}
void checkDex(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  std::array<char, 8> magic{};
  stream.read(magic.data(), magic.size());
  if (stream.gcount() != 8 || std::string_view(magic.data(), 4) != "dex\n" ||
      magic[7] || magic[4] < '0' || magic[4] > '9' || magic[5] < '0' ||
      magic[5] > '9' || magic[6] < '0' || magic[6] > '9')
    throw Error("Invalid DEX header: " + pathText(path.filename()));
}
bool dexName(std::string_view name) {
  if (name == "classes.dex")
    return true;
  if (!name.starts_with("classes") || !name.ends_with(".dex"))
    return false;
  auto number = name.substr(7, name.size() - 11);
  if (number.empty() || number.front() == '0')
    return false;
  if (number.size() == 1 && number.front() < '2')
    return false;
  return std::all_of(number.begin(), number.end(),
                     [](char c) { return c >= '0' && c <= '9'; });
}
struct Inputs {
  std::string kind;
  fs::path code;
  std::vector<std::string> names;
};
Inputs stageInputs(const fs::path &source, const fs::path &work,
                   const Limits &limits) {
  Inputs result;
  result.code = work / "code";
  if (!fs::create_directory(result.code))
    throw Error("Android code staging directory already exists");
  if (fs::is_directory(source)) {
    result.kind = "smali-directory";
    auto tree = work / "tree";
    copyTree(source, tree, limits);
    std::vector<fs::path> paths;
    for (const auto &entry : fs::recursive_directory_iterator(tree))
      if (entry.is_regular_file() &&
          lowerASCII(pathText(entry.path().extension())) == ".smali")
        paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end(),
              [](const fs::path &a, const fs::path &b) {
                return pathText(a) < pathText(b);
              });
    for (auto &path : paths) {
      result.names.push_back(pathText(path.lexically_relative(tree)));
      auto ordinal = std::to_string(result.names.size());
      if (ordinal.size() < 6)
        ordinal.insert(0, 6 - ordinal.size(), '0');
      fs::rename(path, result.code / (ordinal + ".smali"));
    }
    if (result.names.empty())
      throw Error("Smali directory contains no .smali files");
  } else {
    result.kind = lowerASCII(pathText(source.extension()));
    if (!result.kind.empty() && result.kind.front() == '.')
      result.kind.erase(0, 1);
    if (result.kind != "apk" && result.kind != "dex" && result.kind != "smali")
      throw Error(
          "Android input must be an APK, DEX, smali file, or smali directory");
    if (result.kind == "apk") {
      auto archive = work / "input.apk", tree = work / "archive";
      copyInput(source, archive, limits);
      auto extracted = extractZip(archive, tree, limits, [](const fs::path &Path) {
        return Path.parent_path().empty() && dexName(pathText(Path.filename()));
      });
      std::vector<fs::path> paths;
      for (auto &path : extracted)
        if (path.parent_path() == tree && dexName(pathText(path.filename())))
          paths.push_back(path);
      std::sort(paths.begin(), paths.end(),
                [](const fs::path &a, const fs::path &b) {
                  return pathText(a) < pathText(b);
                });
      if (paths.empty())
        throw Error(
            "APK contains no root classes.dex or classesN.dex bytecode");
      for (auto &path : paths) {
        checkDex(path);
        result.names.push_back(pathText(path.filename()));
        fs::rename(path, result.code / path.filename());
      }
    } else {
      auto path = result.code / ("input." + result.kind);
      copyInput(source, path, limits);
      if (result.kind == "dex")
        checkDex(path);
      result.names.push_back(pathText(source.filename()));
    }
  }
  return result;
}
std::vector<std::string> launcher(const std::string &selected) {
  auto executable = pathFromUTF8(selected);
  if (executable.parent_path().empty() || executable.parent_path() == ".")
    if (auto found = findProgram(selected))
      executable = pathFromUTF8(*found);
  auto suffix = lowerASCII(pathText(executable.extension()));
  if (suffix != ".bat" && suffix != ".cmd" && suffix != ".jar")
    return {pathText(executable)};
  std::vector<fs::path> jars;
  if (suffix == ".jar")
    jars.push_back(executable);
  else {
    auto library = executable.parent_path().parent_path() / "lib";
    if (fs::is_directory(library))
      for (const auto &entry : fs::directory_iterator(library)) {
        auto name = pathText(entry.path().filename());
        if (name.starts_with("jadx-") && name.ends_with("-all.jar"))
          jars.push_back(entry.path());
      }
  }
  if (jars.size() != 1 || !fs::is_regular_file(jars.front()))
    throw Error("JADX batch launcher requires one lib/jadx-*-all.jar in its "
                "distribution");
  std::optional<std::string> java;
  if (auto home = std::getenv("JAVA_HOME")) {
#ifdef _WIN32
    auto path = pathFromUTF8(home) / "bin" / "java.exe";
#else
    auto path = pathFromUTF8(home) / "bin" / "java";
#endif
    if (fs::is_regular_file(path))
      java = pathText(path);
  }
  if (!java)
    java = findProgram("java");
  if (!java)
    throw Error(
        "JADX requires Java 11 or newer; set JAVA_HOME or add java to PATH");
  return {*java, "-cp", pathText(fs::canonical(jars.front())),
          "jadx.cli.JadxCLI"};
}
std::string backendVersion(std::string_view text) {
  llvm::Regex expression(
      "^([0-9]+)\\.([0-9]+)\\.([0-9]+)([-+][^[:space:]]+)?[[:space:]]*$");
  for (auto &line : lines(text)) {
    // Preserve the original exact-line contract: leading text is not a version.
    llvm::SmallVector<llvm::StringRef, 5> matches;
    if (!expression.match(line, &matches))
      continue;
    std::array<uint64_t, 3> version{};
    bool valid = true;
    for (unsigned i = 0; i < 3; ++i) {
      auto value = matches[i + 1];
      auto parsed = std::from_chars(value.begin(), value.end(), version[i]);
      valid &= parsed.ec == std::errc{} && parsed.ptr == value.end();
    }
    if (valid && version >= std::array<uint64_t, 3>{1, 5, 6})
      return trim(line);
  }
  throw Error("Android support requires JADX 1.5.6 or newer with dex-input and "
              "smali-input plugins");
}
bool backendErrors(std::string_view text) {
  if (text.find("Failed to load code for plugin:") != std::string_view::npos ||
      text.find("Found duplicated class:") != std::string_view::npos)
    return true;
  llvm::Regex error("^[[:space:]]*(\x1b\\[[0-9;]*m)*ERROR[[:space:]]*[-:]",
                    llvm::Regex::IgnoreCase);
  for (auto &line : lines(text))
    if (error.match(line))
      return true;
  return false;
}
std::vector<fs::path> checkOutput(const fs::path &sources,
                                  const Limits &limits) {
  if (!fs::is_directory(fs::symlink_status(sources)))
    throw Error("JADX produced no safe Java source directory");
  validateTree(sources, limits);
  std::vector<fs::path> java;
  for (const auto &entry : fs::recursive_directory_iterator(sources)) {
    if (!entry.is_regular_file() ||
        pathText(entry.path().extension()) != ".java")
      continue;
    if (!entry.file_size())
      throw Error("JADX produced an empty Java source file");
    auto content = readFile(entry.path(), limits.max_bytes);
    for (auto &line : lines(content)) {
      auto value = trim(line);
      if (value.starts_with("/*"))
        value.erase(0, 2);
      else if (value.starts_with('*'))
        value.erase(0, 1);
      else
        continue;
      value = lowerASCII(trim(value));
      if (value.starts_with("jadx error:") ||
          value.starts_with("code decompiled incorrectly"))
        throw Error("JADX produced incomplete Java code; see logs/jadx.log");
    }
    java.push_back(entry.path());
  }
  if (java.empty())
    throw Error(
        "JADX produced no Java sources; verify the input and backend plugins");
  std::sort(java.begin(), java.end(), [](const fs::path &a, const fs::path &b) {
    return pathText(a) < pathText(b);
  });
  return java;
}
Object external(const Options &options, const fs::path &staging) {
  const auto &limits = options.limits;
  auto logs = staging / "logs";
  fs::create_directory(logs);
  auto command = launcher(*options.jadx);
  WorkDirectory work(staging);
  std::map<std::string, std::optional<std::string>> environment;
  for (auto flag : {"JADX_DISABLE_XML_SECURITY", "JADX_DISABLE_ZIP_SECURITY",
                    "JADX_DISABLE_ALL_SECURITY_FLAGS"})
    environment[flag] = std::nullopt;
  for (auto suffix : {"CONFIG", "CACHE", "TMP"}) {
    auto directory = work.path / "runtime" / lowerASCII(suffix);
    fs::create_directories(directory);
    environment[std::string("JADX_") + suffix + "_DIR"] = pathText(directory);
  }
  auto version_command = command;
  version_command.push_back("--version");
  auto version_log = logs / "jadx-version.log";
  runTool(version_command, version_log, limits.timeout, staging, limits,
          environment);
  auto version = backendVersion(trim(readFile(version_log, limits.max_bytes)));
  auto inputs = stageInputs(fs::absolute(options.input), work.path, limits);
  auto log = logs / "jadx.log";
  command.insert(command.end(),
                 {"--config", "none", "--no-res", "--output-format", "java",
                  "--decompilation-mode", "restructure", "--comments-level",
                  "warn", "--log-level", "warn", "--deobf-cfg-file-mode",
                  "ignore", "--output-dir", pathText(staging),
                  pathText(inputs.code)});
  runTool(command, log, limits.timeout, staging, limits, environment);
  if (backendErrors(readFile(log, limits.max_bytes)))
    throw Error(
        "JADX reported input or decompilation errors; see logs/jadx.log");
  auto paths = checkOutput(staging / "sources", limits);
  work.clear();
  std::vector<std::string> sources;
  for (auto &path : paths)
    sources.push_back(pathText(path.lexically_relative(staging)));
  Array limitations{
      "Java is reconstructed from bytecode; original comments, formatting, and "
      "stripped names cannot be restored.",
      "A successful backend run does not prove semantic equivalence or that "
      "every method can be recompiled.",
      "Android resources, manifests, native libraries, and dynamically "
      "downloaded or encrypted code are not decompiled."};
  if (inputs.kind == "smali")
    limitations.push_back(
        "Single smali input has only one class; supply its directory to "
        "resolve sibling and nested classes together.");
  bool dex = inputs.kind == "apk" || inputs.kind == "dex";
  return Object{{"status", "success"},
                {"platform", "android"},
                {"input_kind", inputs.kind},
                {"backend", Object{{"name", "jadx"}, {"version", version}}},
                {"input_code_files", strings(inputs.names)},
                {"dex_count", dex ? uint64_t(inputs.names.size()) : 0},
                {"smali_count", dex ? 0 : uint64_t(inputs.names.size())},
                {"java_source_count", uint64_t(sources.size())},
                {"java_sources", strings(sources)},
                {"logs", Array{"logs/jadx-version.log", "logs/jadx.log"}},
                {"limitations", std::move(limitations)}};
}
Object builtin(const Options &options, const fs::path &staging,
               Budget &budget) {
  const auto &limits = options.limits;
  WorkDirectory work(staging);
  auto inputs = stageInputs(fs::absolute(options.input), work.path, limits);
  std::vector<fs::path> paths;
  for (const auto &entry : fs::directory_iterator(inputs.code))
    if (entry.is_regular_file())
      paths.push_back(entry.path());
  std::sort(paths.begin(), paths.end(),
            [](const fs::path &a, const fs::path &b) {
              return pathText(a) < pathText(b);
            });
  if (paths.size() != inputs.names.size())
    throw Error("Android staged input inventory is inconsistent");
  bool dex = inputs.kind == "apk" || inputs.kind == "dex";
  std::vector<dalvik::Class> classes;
  for (size_t i = 0; i < paths.size(); ++i) {
    auto content = readFile(paths[i], limits.max_bytes);
    budget.tick(content.size());
    if (dex) {
      auto parsed = dalvik::parseDex(content, inputs.names[i], budget);
      classes.insert(classes.end(), std::make_move_iterator(parsed.begin()),
                     std::make_move_iterator(parsed.end()));
    } else {
      if (!llvm::json::isUTF8(content))
        throw Error("smali input is not valid UTF-8");
      classes.push_back(dalvik::parseSmali(content, inputs.names[i], budget));
    }
  }
  Value coverage(dalvik::recoverJava(
      dalvik::linkClasses(std::move(classes), budget), budget));
  auto *object = coverage.getAsObject();
  auto *source_units = object ? object->getArray("source_units") : nullptr;
  if (!source_units)
    throw Error("Android native emitter produced no source inventory");
  Array units = std::move(*source_units);
  object->erase("source_units");
  work.clear();
  auto metadata_path = pathFromUTF8("metadata/android-methods.json");
  auto metadata = jsonText(coverage);
  budget.output(metadata.size());
  uint64_t total_bytes = metadata.size();
  std::map<std::string, std::pair<std::string, bool>> entries;
  auto reserve = [&](const fs::path &path, bool directory) {
    auto name = pathText(path), key = portableCaseKey(name);
    auto [found, inserted] = entries.emplace(key, std::pair{name, directory});
    if (!inserted &&
        (found->second != std::pair{name, directory} || !directory))
      throw Error(
          "Android Java output has conflicting class or directory paths");
    if (entries.size() > limits.max_files)
      throw Error(
          "Android output exceeds the file-count limit including directories");
  };
  reserve(metadata_path.parent_path(), true);
  reserve(metadata_path, false);
  std::vector<std::pair<fs::path, std::string>> pending;
  for (auto &unit_value : units) {
    auto *unit = unit_value.getAsObject();
    if (!unit)
      throw Error("Android native emitter has an invalid source unit");
    auto name = unit->getString("path"), source = unit->getString("source");
    if (!name || !source || source->empty() || !llvm::json::isUTF8(*source))
      throw Error("Android native emitter has an invalid Java source");
    auto relative =
        fs::path("sources") /
        relativeMember(std::string_view(name->data(), name->size()));
    for (auto parent = relative.parent_path(); !parent.empty() && parent != ".";
         parent = parent.parent_path())
      reserve(parent, true);
    reserve(relative, false);
    budget.tick(source->size());
    if (source->size() >
        limits.max_bytes - std::min(total_bytes, limits.max_bytes))
      throw Error("Android source and metadata output exceed the byte limit");
    total_bytes += source->size();
    pending.emplace_back(relative, source->str());
  }
  if (total_bytes > limits.max_bytes)
    throw Error("Android metadata output exceeds the byte limit");
  // The complete artifact set is checked before publishing the first Java file.
  std::vector<std::string> sources;
  for (auto &[relative, source] : pending) {
    writeFile(staging / relative, source);
    sources.push_back(pathText(relative));
  }
  writeFile(staging / metadata_path, metadata);
  validateTree(staging, limits);
  return Object{
      {"status", "success"},
      {"platform", "android"},
      {"input_kind", inputs.kind},
      {"backend",
       Object{{"name", "neverd"}, {"version", "1"}, {"execution", "builtin"}}},
      {"input_code_files", strings(inputs.names)},
      {"dex_count", dex ? uint64_t(inputs.names.size()) : 0},
      {"smali_count", dex ? 0 : uint64_t(inputs.names.size())},
      {"java_source_count", uint64_t(sources.size())},
      {"java_sources", strings(sources)},
      {"logs", Array{}},
      {"android_method_recovery", std::move(coverage)},
      {"limitations",
       Array{"Java is reconstructed from bytecode; compilation-lost source "
             "text and identifiers cannot be restored.",
             "Unsupported instructions, declarations and unproven register "
             "flows reject the input instead of publishing missing bodies.",
             "Generated method control flow can use a Java dispatch loop; it "
             "does not execute the input DEX or call an external decompiler.",
             "Native and abstract declarations remain declaration-only and are "
             "counted separately from recovered bodies.",
             "Android resources, manifests and native libraries are outside "
             "this Java recovery workflow.",
             "Successful recovery does not certify behavior for arbitrary "
             "applications."}}};
}
} // namespace
Object recoverAndroid(const Options &options, const fs::path &staging,
                      Budget &budget) {
  if (!fs::is_directory(fs::symlink_status(staging)))
    throw Error("Android output staging must be a directory");
  return options.jadx ? external(options, staging)
                      : builtin(options, staging, budget);
}
} // namespace neverd::mobile
