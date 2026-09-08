#include "MobileIOSInternal.h"

#include "neverd/loader/MachO/MachOLoader.h"

#include "llvm/Support/Error.h"

#include <algorithm>

namespace neverd::mobile {
namespace {
fs::path artifactPath(const fs::path &bundle, const std::string &artifact) {
  auto relative = relativeMember(artifact);
  auto candidate = bundle / relative;
  if (fs::is_symlink(candidate) || !fs::is_regular_file(candidate))
    throw Error("iOS artifact is not a regular file: " + artifact);
  auto rel = fs::canonical(candidate).lexically_relative(fs::canonical(bundle));
  if (rel.empty() || rel.is_absolute() || *rel.begin() == "..")
    throw Error("iOS artifact escapes input package");
  return candidate;
}
ios::Object bundleInfo(const fs::path &bundle, Budget &budget) {
  auto p = bundle / "Info.plist";
  if (fs::is_symlink(p) || !fs::is_regular_file(p))
    throw Error("iOS bundle has no readable bounded Info.plist");
  return ios::parsePlist(readFile(p, 16 * 1024 * 1024), budget);
}
fs::path executablePath(const fs::path &bundle, const ios::Object &info,
                        const Options &options) {
  if (options.artifact)
    return artifactPath(bundle, *options.artifact);
  auto name = ios::requiredString(info, "CFBundleExecutable");
  if (name.empty() || name == "." || name == ".." ||
      name.find_first_of(std::string("/\\:\0", 4)) != name.npos)
    throw Error("Info.plist has invalid CFBundleExecutable");
  return artifactPath(bundle, name);
}
void publish(const fs::path &path, std::string_view text, Budget &budget) {
  budget.output(text.size());
  writeFile(path, text);
}
void publishJSON(const fs::path &path, const ios::Object &value,
                 Budget &budget) {
  publish(path, jsonText(ios::Value(ios::Object(value))), budget);
}
} // namespace
llvm::json::Object recoverIOS(const Options &options, const fs::path &staging,
                              Budget &budget) {
  using namespace ios;
  auto input = staging / "input";
  fs::create_directories(input);
  Object info;
  std::string kind;
  fs::path executable;
  if (fs::is_directory(options.input)) {
    if (lowerASCII(pathText(options.input.extension())) != ".app")
      throw Error("iOS directory input must be an .app bundle");
    copyTree(options.input, input, budget.limits);
    kind = "app";
    info = bundleInfo(input, budget);
    executable = executablePath(input, info, options);
  } else if (lowerASCII(pathText(options.input.extension())) == ".ipa") {
    extractZip(options.input, input, budget.limits);
    kind = "ipa";
    std::vector<fs::path> apps;
    auto payload = input / "Payload";
    if (fs::is_directory(payload))
      for (const auto &entry : fs::directory_iterator(payload))
        if (entry.is_directory() &&
            lowerASCII(pathText(entry.path().extension())) == ".app")
          apps.push_back(entry.path());
    if (apps.size() != 1)
      throw Error("IPA must contain exactly one Payload/*.app");
    info = bundleInfo(apps[0], budget);
    executable = executablePath(apps[0], info, options);
  } else {
    if (options.artifact)
      throw Error("--artifact applies only to IPA or .app input");
    if (fs::is_symlink(options.input) || !fs::is_regular_file(options.input))
      throw Error("iOS binary input must be a regular file");
    kind = "macho";
    executable = options.input;
  }
  auto selection = selectSlice(readFile(executable, budget.limits.max_bytes),
                               options.architecture, budget);
  if (selection.encrypted)
    throw Error("selected Mach-O slice is encrypted (cryptid != 0); supply a "
                "decrypted executable");
  auto selected = kind == "macho"
                      ? pathText(options.input.filename())
                      : pathText(executable.lexically_relative(input));
  fs::create_directories(staging / "metadata");
  fs::create_directories(staging / "artifacts");
  auto thin = staging / "artifacts/selected.macho";
  publish(thin, selection.bytes, budget);
  budget.check();
  MachOLoader loader;
  auto loaded = loader.load(thin);
  if (!loaded)
    throw Error("invalid selected Mach-O: " +
                llvm::toString(loaded.takeError()));
  auto objc = objcMetadata(*loaded), swift = swiftMetadata(*loaded, budget);
  publishJSON(staging / "metadata/swift.json", swift, budget);
  Object outputs{{"selected_binary", "artifacts/selected.macho"},
                 {"objc_metadata", "metadata/objc.json"},
                 {"objc_declarations", "metadata/objc.h"},
                 {"swift_metadata", "metadata/swift.json"}};
  Value native_count(nullptr), objc_recovery(nullptr), swift_recovery(nullptr);
  Array native_limitations;
  if (!options.metadata_only) {
    fs::create_directories(staging / "sources");
    fs::create_directories(staging / "logs");
    auto batchpath = staging / "artifacts/native-recovery.json";
    std::vector<std::string> argv{
        options.executable,      "export", pathText(thin),
        "--format=objc-methods", "-o",     pathText(batchpath)};
    if (options.max_functions)
      argv.push_back("--max-func=" + std::to_string(options.max_functions));
    budget.check();
    runTool(argv, staging / "logs/native.log", toolTimeout(budget), staging,
            budget.limits);
    auto value = parseJSON(readFile(batchpath, budget.limits.max_bytes),
                           "native source report");
    const auto &batch = object(value, "native source report");
    if (number(batch, "schema_version") != 1 ||
        str(batch, "status") != "success")
      throw Error("native source report has unsupported schema");
    auto native = requiredString(batch, "native_source");
    native_limitations = array(batch, "limitations");
    for (const auto &v : native_limitations)
      if (!v.getAsString())
        throw Error("invalid native limitation");
    array(batch, "methods");
    auto count = nativeFunctionCount(native);
    if (!count)
      throw Error("native decompiler recovered no function bodies; use "
                  "--metadata-only for metadata inspection");
    auto reported = batch.getInteger("native_function_count");
    if (!reported || *reported < 0 || uint64_t(*reported) != count)
      throw Error("native source report disagrees with emitted function count");
    if (!batch.getObject("objc_metadata"))
      throw Error("native source report has invalid Objective-C metadata");
    objc = *batch.getObject("objc_metadata");
    auto recovered = objcSources(batch, objc, selection.pointer_size, budget);
    publish(staging / "sources/native.c", native, budget);
    native_count = count;
    if (number(recovered.coverage, "recovered_method_count")) {
      publish(staging / "sources/objc.m", recovered.source, budget);
      outputs["objc_source"] = "sources/objc.m";
    }
    publishJSON(staging / "metadata/objc-methods.json", recovered.coverage,
                budget);
    objc_recovery = std::move(recovered.coverage);
    outputs["objc_method_coverage"] = "metadata/objc-methods.json";
    outputs["native_source"] = "sources/native.c";
    outputs["native_log"] = "logs/native.log";
    fs::remove(batchpath);
    auto swift_result =
        swiftSources(options, staging, thin, array(swift, "symbols"),
                     selection.pointer_size, budget);
    swift_recovery = std::move(swift_result.coverage);
    for (auto &[key, v] : swift_result.outputs)
      outputs[key] = std::move(v);
  }
  publishJSON(staging / "metadata/objc.json", objc, budget);
  publish(staging / "metadata/objc.h", objcHeader(objc, selection.pointer_size),
          budget);
  Object bundle;
  for (auto key :
       {"CFBundleIdentifier", "CFBundleName", "CFBundleExecutable",
        "CFBundleVersion", "CFBundleShortVersionString", "MinimumOSVersion"})
    if (auto *v = info.get(key);
        v && (v->getAsString() || v->getAsInteger() || v->getAsBoolean()))
      bundle[key] = *v;
  Array available;
  for (const auto &arch : selection.available)
    available.push_back(arch);
  Array limitations{
      "Original comments, formatting, removed names and source constructs lost "
      "during compilation cannot be restored.",
      "Objective-C method coverage lists reconstructed bodies and omissions; "
      "it is not a proof of semantic equivalence.",
      "Swift method coverage separates recovered native bodies, unsupported "
      "callable signatures and non-callable metadata; stripped or unclassified "
      "symbols can leave coverage unknown.",
      "Native C types and calling conventions remain approximations outside "
      "supported source signatures.",
      "Only the selected executable is analyzed; use --artifact for embedded "
      "frameworks or extensions."};
  appendUnique(limitations, native_limitations);
  if (auto *o = objc_recovery.getAsObject())
    appendUnique(limitations, array(*o, "limitations"));
  if (auto *s = swift_recovery.getAsObject())
    appendUnique(limitations, array(*s, "limitations"));
  appendUnique(limitations, array(objc, "limitations"));
  appendUnique(limitations, array(swift, "limitations"));
  fs::remove_all(input);
  budget.check();
  return Object{{"platform", "ios"},
                {"input_kind", kind},
                {"selected_artifact", selected},
                {"architecture", selection.architecture},
                {"cpu_subtype", selection.cpu_subtype},
                {"available_architectures", std::move(available)},
                {"encrypted", false},
                {"bundle", std::move(bundle)},
                {"metadata_only", options.metadata_only},
                {"outputs", std::move(outputs)},
                {"native_function_count", std::move(native_count)},
                {"objc_method_recovery", std::move(objc_recovery)},
                {"swift_method_recovery", std::move(swift_recovery)},
                {"objc_class_count", array(objc, "classes").size()},
                {"swift_type_count", array(swift, "types").size()},
                {"swift_symbol_count", array(swift, "symbols").size()},
                {"limitations", std::move(limitations)}};
}
} // namespace neverd::mobile
