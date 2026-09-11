#include "MobileIOSInternal.h"
#include "capi/NativeMobileSession.h"

#include "neverd/loader/MachO/MachOLoader.h"
#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <memory>

namespace neverd::mobile::ios {
namespace {
uint64_t workerInteger(const Object &object, llvm::StringRef key) {
  auto text = object.getString(key);
  if (!text || text->empty() || text->size() > 20 ||
      (text->size() > 1 && text->front() == '0'))
    throw Error("invalid iOS worker integer: " + key.str());
  uint64_t value = 0;
  auto parsed = std::from_chars(text->data(), text->data() + text->size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size())
    throw Error("invalid iOS worker integer: " + key.str());
  return value;
}
void checkWorkerIdentity(const Object &object, const WorkerRequest &request) {
  if (object.getString("architecture") != request.architecture ||
      object.getInteger("pointer_size") != request.pointer_size ||
      object.getString("selected_sha256") != request.selected_sha256)
    throw Error("iOS worker result does not match its selected image");
}
} // namespace

Object workerRequestJSON(const WorkerRequest &request) {
  return Object{
      {"schema_version", 1},
      {"architecture", request.architecture},
      {"pointer_size", request.pointer_size},
      {"selected_sha256", request.selected_sha256},
      {"limits", Object{{"timeout", std::to_string(request.limits.timeout)},
                        {"max_files", std::to_string(request.limits.max_files)},
                        {"max_bytes", std::to_string(request.limits.max_bytes)}}},
      {"max_functions", std::to_string(request.max_functions)},
      {"remaining", std::to_string(request.remaining)},
      {"output_bytes", std::to_string(request.output_bytes)},
      {"time_remaining_ms", std::to_string(request.time_remaining_ms)}};
}

WorkerRequest parseWorkerRequest(const Object &request) {
  if (request.size() != 9 || request.getInteger("schema_version") != 1)
    throw Error("unsupported iOS worker request schema");
  WorkerRequest result;
  auto *limits = request.getObject("limits");
  if (!limits || limits->size() != 3)
    throw Error("invalid iOS worker limits");
  result.limits = {workerInteger(*limits, "timeout"),
                   workerInteger(*limits, "max_files"),
                   workerInteger(*limits, "max_bytes")};
  Budget initial(result.limits);
  result.architecture = requiredString(request, "architecture");
  auto pointer_size = request.getInteger("pointer_size");
  const bool wide = result.architecture == "arm64" ||
                    result.architecture == "x86_64";
  if ((!wide && result.architecture != "arm" &&
       result.architecture != "i386") ||
      !pointer_size || *pointer_size != (wide ? 8 : 4))
    throw Error("invalid iOS worker architecture");
  result.pointer_size = static_cast<unsigned>(*pointer_size);
  result.selected_sha256 = requiredString(request, "selected_sha256");
  if (result.selected_sha256.size() != 64 ||
      !std::all_of(result.selected_sha256.begin(), result.selected_sha256.end(),
                   [](unsigned char c) {
                     return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                   }))
    throw Error("invalid iOS worker selected-image digest");
  result.max_functions = workerInteger(request, "max_functions");
  result.remaining = workerInteger(request, "remaining");
  result.output_bytes = workerInteger(request, "output_bytes");
  result.time_remaining_ms = workerInteger(request, "time_remaining_ms");
  if (result.max_functions > std::numeric_limits<size_t>::max() ||
      result.remaining > initial.remaining ||
      result.output_bytes > result.limits.max_bytes ||
      !result.time_remaining_ms ||
      result.time_remaining_ms > result.limits.timeout * 1000)
    throw Error("invalid iOS worker budget snapshot");
  return result;
}

Object workerResultJSON(const WorkerRequest &request, const Budget &budget,
                        Object report) {
  budget.check();
  if (budget.remaining > request.remaining ||
      budget.output_bytes < request.output_bytes ||
      budget.output_bytes > request.limits.max_bytes)
    throw Error("invalid iOS worker budget consumption");
  return Object{{"schema_version", 1},
                {"status", "success"},
                {"architecture", request.architecture},
                {"pointer_size", request.pointer_size},
                {"selected_sha256", request.selected_sha256},
                {"remaining", std::to_string(budget.remaining)},
                {"output_bytes", std::to_string(budget.output_bytes)},
                {"report", std::move(report)}};
}

void mergeWorkerBudget(const Object &result, const WorkerRequest &request,
                        Budget &budget) {
  budget.check();
  if (result.size() != 8 || result.getInteger("schema_version") != 1 ||
      result.getString("status") != "success" || !result.getObject("report"))
    throw Error("unsupported iOS worker result schema");
  checkWorkerIdentity(result, request);
  const auto remaining = workerInteger(result, "remaining");
  const auto output_bytes = workerInteger(result, "output_bytes");
  if (budget.remaining != request.remaining ||
      budget.output_bytes != request.output_bytes ||
      budget.limits.timeout != request.limits.timeout ||
      budget.limits.max_files != request.limits.max_files ||
      budget.limits.max_bytes != request.limits.max_bytes ||
      remaining > request.remaining || output_bytes < request.output_bytes ||
      output_bytes > request.limits.max_bytes)
    throw Error("iOS worker budget counters are inconsistent");
  budget.tick(request.remaining - remaining);
  budget.output(output_bytes - request.output_bytes);
}
} // namespace neverd::mobile::ios

namespace neverd::mobile {
namespace {
class PhaseTimer {
  using Clock = std::chrono::steady_clock;
  struct CompletedPhase {
    const char *name = nullptr;
    int64_t elapsed = 0;
  };
  Clock::time_point started = Clock::now(), active_started = started;
  const char *active = "input_staging";
  std::array<CompletedPhase, 9> completed{};
  size_t count = 0;

  static int64_t milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - begin)
        .count();
  }

public:
  explicit PhaseTimer(const char *first = "input_staging") : active(first) {}
  void enter(const char *phase) {
    auto now = Clock::now();
    if (count < completed.size())
      completed[count++] = {active, milliseconds(active_started, now)};
    active = phase;
    active_started = now;
  }
  std::string diagnostic() const {
    auto now = Clock::now();
    llvm::json::Array phases;
    for (size_t i = 0; i < count; ++i)
      phases.push_back(llvm::json::Object{
          {"phase", completed[i].name}, {"elapsed_ms", completed[i].elapsed}});
    llvm::json::Object report{
        {"active_phase", active},
        {"active_elapsed_ms", milliseconds(active_started, now)},
        {"elapsed_ms", milliseconds(started, now)},
        {"completed_phases", std::move(phases)}};
    return llvm::formatv("{0}", llvm::json::Value(std::move(report))).str();
  }
};
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
std::string imageDigest(llvm::ArrayRef<uint8_t> bytes, Budget &budget) {
  llvm::SHA256 digest;
  constexpr size_t chunk = 64 * 1024;
  while (!bytes.empty()) {
    budget.check();
    const auto count = std::min(chunk, bytes.size());
    digest.update(bytes.take_front(count));
    bytes = bytes.drop_front(count);
  }
  budget.check();
  return llvm::toHex(digest.final(), true);
}
std::string imageDigest(std::string_view bytes, Budget &budget) {
  return imageDigest(llvm::ArrayRef<uint8_t>(
                         reinterpret_cast<const uint8_t *>(bytes.data()),
                         bytes.size()),
                     budget);
}
std::string nativeText(const char *raw, Budget &budget,
                       std::string_view what) {
  std::unique_ptr<const char, decltype(&neverd_free_string)> text(
      raw, neverd_free_string);
  if (!text)
    throw Error(std::string(what) + " returned no text");
  const auto limit = std::min<uint64_t>(budget.limits.max_bytes,
                                       std::string().max_size());
  for (size_t length = 0;; ++length) {
    if (!(length % (64 * 1024)))
      budget.check();
    if (!text.get()[length])
      return std::string(text.get(), length);
    if (length == limit)
      throw Error(std::string(what) + " exceeds its byte budget");
  }
}
ios::Value nativeJSON(const char *raw, neverd_session_t session,
                      Budget &budget, std::string_view what) {
  if (!raw)
    throw Error(std::string(what) + " failed: " +
                nativeText(neverd_last_error(session), budget, what));
  return parseJSON(nativeText(raw, budget, what), what);
}
void workerFile(const fs::path &path) {
  if (!fs::is_regular_file(fs::symlink_status(path)))
    throw Error("iOS worker artifact is not a regular file: " + pathText(path));
}
void checkStringArray(const ios::Array &values, Budget &budget) {
  for (const auto &value : values) {
    budget.tick();
    if (!value.getAsString())
      throw Error("invalid iOS worker limitation");
  }
}
ios::Object initialOutputs() {
  return ios::Object{{"selected_binary", "artifacts/selected.macho"},
                     {"objc_metadata", "metadata/objc.json"},
                     {"objc_declarations", "metadata/objc.h"},
                     {"swift_metadata", "metadata/swift.json"}};
}
ios::Object recoverWorkerImage(const ios::WorkerRequest &request,
                               const fs::path &staging, Budget &budget,
                               PhaseTimer &phases) {
  using namespace ios;
  std::unique_ptr<void, decltype(&neverd_session_destroy)> session(
      neverd_session_create(), neverd_session_destroy);
  if (!session)
    throw Error("cannot create iOS worker session");
  Object objc, swift;
  struct Observation {
    const WorkerRequest &request;
    Budget &budget;
    PhaseTimer &phases;
    Object &objc, &swift;
    const fs::path &staging;
  } observation{request, budget, phases, objc, swift, staging};
  auto observe = [](const BinaryImage &image, void *opaque) {
    auto &state = *static_cast<Observation *>(opaque);
    const char *architecture = image.Arch == Arch::AArch64 ? "arm64"
                               : image.Arch == Arch::X64  ? "x86_64"
                               : image.Arch == Arch::ARM  ? "arm"
                               : image.Arch == Arch::X86  ? "i386"
                                                         : "unknown";
    if (image.Format != BinaryFormat::MachO ||
        architecture != state.request.architecture ||
        (image.is64Bit() ? 8u : 4u) != state.request.pointer_size ||
        image.Raw.size() > state.budget.limits.max_bytes ||
        imageDigest(image.Raw, state.budget) != state.request.selected_sha256)
      throw Error("iOS worker loaded image disagrees with selected slice");
    state.phases.enter("objc_metadata");
    state.objc = objcMetadata(image);
    state.phases.enter("swift_metadata");
    state.swift = swiftMetadata(image, state.budget);
    publishJSON(state.staging / "metadata/swift.json", state.swift,
                state.budget);
    state.phases.enter("session_preparation");
  };
  const auto thin = staging / "artifacts/selected.macho";
  if (!sdk::loadNativeMobileSession(session.get(), pathText(thin).c_str(),
                                    observe, &observation))
    throw Error(nativeText(neverd_last_error(session.get()), budget,
                            "native session load"));
  Object outputs = initialOutputs();
  Value native_count(nullptr), objc_recovery(nullptr);
  Array native_limitations;
  // Release the native batch and source text before the Swift analysis begins.
  {
    phases.enter("native_export");
    auto value = nativeJSON(
        neverd_objc_methods_json(session.get(), request.max_functions),
        session.get(), budget, "Objective-C export");
    const auto &batch = object(value, "native source report");
    if (number(batch, "schema_version") != 1 ||
        str(batch, "status") != "success")
      throw Error("native source report has unsupported schema");
    auto native = requiredString(batch, "native_source");
    native_limitations = array(batch, "limitations");
    checkStringArray(native_limitations, budget);
    array(batch, "methods");
    const auto count = nativeFunctionCount(native);
    if (!count)
      throw Error("native decompiler recovered no function bodies; use "
                  "--metadata-only for metadata inspection");
    auto reported = batch.getInteger("native_function_count");
    if (!reported || *reported < 0 || uint64_t(*reported) != count)
      throw Error("native source report disagrees with emitted function count");
    if (!batch.getObject("objc_metadata"))
      throw Error("native source report has invalid Objective-C metadata");
    objc = *batch.getObject("objc_metadata");
    phases.enter("objc_sources");
    auto recovered = objcSources(batch, objc, request.pointer_size, budget);
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
  }
  phases.enter("swift_sources");
  Options options;
  options.max_functions = request.max_functions;
  auto swift_result = swiftSources(
      options, staging, thin, array(swift, "symbols"), request.pointer_size,
      budget, [&](std::string_view signatures) {
        // swiftSources owns the NUL-terminated signature string for this call.
        return nativeJSON(neverd_swift_methods_json(
                              session.get(), signatures.data(),
                              request.max_functions),
                          session.get(), budget, "Swift export");
      });
  for (auto &[key, value] : swift_result.outputs)
    outputs[key] = std::move(value);
  phases.enter("output_publication");
  publishJSON(staging / "metadata/objc.json", objc, budget);
  publish(staging / "metadata/objc.h", objcHeader(objc, request.pointer_size),
          budget);
  return Object{{"objc_metadata", std::move(objc)},
                {"swift_metadata", std::move(swift)},
                {"outputs", std::move(outputs)},
                {"native_function_count", std::move(native_count)},
                {"native_limitations", std::move(native_limitations)},
                {"objc_method_recovery", std::move(objc_recovery)},
                {"swift_method_recovery", std::move(swift_result.coverage)}};
}
void validateWorkerReport(const ios::Object &report, const fs::path &staging,
                           const ios::WorkerRequest &request, Budget &budget) {
  using namespace ios;
  if (report.size() != 7 || !report.getObject("objc_metadata") ||
      !report.getObject("swift_metadata") || !report.getObject("outputs") ||
      !report.getObject("objc_method_recovery") ||
      !report.getObject("swift_method_recovery"))
    throw Error("invalid iOS worker report");
  const auto &outputs = *report.getObject("outputs");
  const std::map<std::string, std::string> allowed{
      {"selected_binary", "artifacts/selected.macho"},
      {"objc_metadata", "metadata/objc.json"},
      {"objc_declarations", "metadata/objc.h"},
      {"swift_metadata", "metadata/swift.json"},
      {"native_source", "sources/native.c"},
      {"native_log", "logs/native.log"},
      {"objc_method_coverage", "metadata/objc-methods.json"},
      {"swift_signatures", "metadata/swift-signatures.json"},
      {"swift_method_coverage", "metadata/swift-methods.json"},
      {"objc_source", "sources/objc.m"},
      {"swift_source", "sources/swift.swift"},
      {"swift_native_log", "logs/native.log"}};
  for (const auto &[key, value] : outputs) {
    budget.tick();
    auto path = allowed.find(key.str());
    if (path == allowed.end() || value.getAsString() != path->second)
      throw Error("invalid iOS worker output path");
    workerFile(staging / path->second);
  }
  for (auto key : {"selected_binary", "objc_metadata", "objc_declarations",
                   "swift_metadata", "native_source", "native_log",
                   "objc_method_coverage", "swift_signatures",
                   "swift_method_coverage"})
    if (!outputs.getString(key))
      throw Error("iOS worker omitted a required output");
  auto equalJSON = [&](const char *name, const Object &expected) {
    budget.check();
    auto value = parseJSON(readFile(staging / name, budget.limits.max_bytes),
                            "iOS worker published metadata");
    if (!value.getAsObject() || *value.getAsObject() != expected)
      throw Error("iOS worker report disagrees with published metadata");
  };
  const auto &objc = *report.getObject("objc_metadata");
  const auto &swift = *report.getObject("swift_metadata");
  const auto &objc_coverage = *report.getObject("objc_method_recovery");
  const auto &swift_coverage = *report.getObject("swift_method_recovery");
  equalJSON("metadata/objc.json", objc);
  equalJSON("metadata/swift.json", swift);
  equalJSON("metadata/objc-methods.json", objc_coverage);
  equalJSON("metadata/swift-methods.json", swift_coverage);
  for (const auto *metadata : {&objc, &swift}) {
    requiredString(*metadata, "status");
    checkStringArray(array(*metadata, "limitations"), budget);
  }
  for (auto key : {"classes", "categories"})
    for (const auto &value : array(objc, key)) {
      budget.tick();
      const auto &entry = object(value, "Objective-C metadata owner");
      requiredString(entry, "name");
      if (!hexAddress(requiredString(entry, "address")))
        throw Error("invalid iOS worker Objective-C address");
      for (const auto &method : array(entry, "methods")) {
        budget.tick();
        const auto &row = object(method, "Objective-C runtime method");
        requiredString(row, "selector");
        requiredString(row, "type_encoding");
        if (!row.getBoolean("class_method") ||
            !row.getString("category_name") ||
            !hexAddress(requiredString(row, "category_address")) ||
            !hexAddress(requiredString(row, "implementation")))
          throw Error("invalid iOS worker Objective-C method identity");
      }
    }
  for (const auto &value : array(swift, "symbols")) {
    budget.tick();
    const auto &row = object(value, "Swift symbol");
    requiredString(row, "name");
    if (!row.getBoolean("defined") ||
        !hexAddress(requiredString(row, "address")))
      throw Error("invalid iOS worker Swift symbol identity");
  }
  for (const auto &value : array(swift, "types")) {
    budget.tick();
    const auto &row = object(value, "Swift nominal metadata");
    requiredString(row, "name");
    auto kind = row.getString("kind");
    if ((kind != "class" && kind != "struct" && kind != "enum") ||
        !hexAddress(requiredString(row, "descriptor")))
      throw Error("invalid iOS worker Swift type identity");
  }
  auto checkCoverage = [&](const Object &coverage, bool is_swift) {
    if (coverage.getInteger("schema_version") != 1)
      throw Error("invalid iOS worker coverage schema");
    requiredString(coverage, "status");
    const auto &methods = array(coverage, "methods");
    uint64_t recovered = 0, projections = 0;
    for (const auto &value : methods) {
      budget.tick();
      const auto &row = object(value, "iOS worker coverage method");
      auto status = row.getString("status");
      if (status != "recovered" && status != "unrecovered")
        throw Error("invalid iOS worker method status");
      if (status == "unrecovered")
        requiredString(row, "reason");
      else {
        ++recovered;
        if (is_swift) {
          const auto representation = row.getString("source_representation");
          if (representation == "compiler-generated-from-type")
            ++projections;
          else if (representation != "native-method-body")
            throw Error("invalid iOS worker Swift source representation");
        }
      }
    }
    auto count = [&](const char *key, uint64_t expected) {
      auto value = coverage.getInteger(key);
      if (!value || *value < 0 || uint64_t(*value) != expected)
        throw Error("inconsistent iOS worker coverage count");
    };
    count("method_count", methods.size());
    count("recovered_method_count", recovered);
    count("unrecovered_method_count", methods.size() - recovered);
    if (is_swift) {
      count("source_body_method_count", recovered - projections);
      count("compiler_projection_method_count", projections);
      count("symbol_count", array(swift, "symbols").size());
      array(coverage, "source_units");
      array(coverage, "non_method_symbols");
    }
    checkStringArray(array(coverage, "limitations"), budget);
    return recovered;
  };
  const auto objc_count = checkCoverage(objc_coverage, false);
  const auto swift_count = checkCoverage(swift_coverage, true);
  if (bool(outputs.get("objc_source")) != (objc_count != 0))
    throw Error("iOS worker Objective-C source presence is inconsistent");
  if (bool(outputs.get("swift_source")) != (swift_count != 0))
    throw Error("iOS worker Swift source presence is inconsistent");
  const auto signatures = parseJSON(
      readFile(staging / "metadata/swift-signatures.json",
               std::min<uint64_t>(budget.limits.max_bytes, 32 * 1024 * 1024)),
      "iOS worker Swift signatures");
  const auto &inventory = object(signatures, "iOS worker Swift signatures");
  if (inventory.getInteger("schema_version") != 1 ||
      inventory.getInteger("symbol_count") !=
          static_cast<int64_t>(array(swift, "symbols").size()) ||
      array(inventory, "methods").size() !=
          array(swift_coverage, "methods").size() ||
      bool(outputs.get("swift_native_log")) !=
          !array(inventory, "methods").empty())
    throw Error("iOS worker Swift inventory is inconsistent");
  for (auto key : {"objc_source", "swift_source"})
    if (!outputs.get(key) && fs::exists(staging / allowed.at(key)))
      throw Error("iOS worker left an unreported source file");
  budget.check();
  const auto native = readFile(staging / "sources/native.c", budget.limits.max_bytes);
  const auto count = nativeFunctionCount(native);
  auto reported = report.getInteger("native_function_count");
  if (!count || !reported || *reported < 0 || uint64_t(*reported) != count)
    throw Error("iOS worker native body count is inconsistent");
  if (readFile(staging / "metadata/objc.h", budget.limits.max_bytes) !=
      objcHeader(objc, request.pointer_size))
    throw Error("iOS worker declarations disagree with runtime metadata");
  if (imageDigest(readFile(staging / "artifacts/selected.macho",
                            budget.limits.max_bytes), budget) !=
      request.selected_sha256)
    throw Error("iOS worker changed the selected image");
  checkStringArray(array(report, "native_limitations"), budget);
}
llvm::json::Object recoverIOSImpl(const Options &options,
                                  const fs::path &staging, Budget &budget,
                                  PhaseTimer &phases) {
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
  phases.enter("slice_selection");
  auto selection = selectSlice(readFile(executable, budget.limits.max_bytes),
                               options.architecture, budget);
  phases.enter("build_target");
  auto build_target = buildTargetMetadata(selection, budget);
  if (selection.encrypted)
    throw Error("selected Mach-O slice is encrypted (cryptid != 0); supply a "
                "decrypted executable");
  auto selected = kind == "macho"
                      ? pathText(options.input.filename())
                      : pathText(executable.lexically_relative(input));
  phases.enter(options.metadata_only ? "macho_loading" : "worker_preparation");
  fs::create_directories(staging / "metadata");
  fs::create_directories(staging / "artifacts");
  auto thin = staging / "artifacts/selected.macho";
  publish(thin, selection.bytes, budget);
  budget.check();
  Object objc, swift, outputs = initialOutputs();
  Value native_count(nullptr), objc_recovery(nullptr), swift_recovery(nullptr);
  Array native_limitations;
  if (options.metadata_only) {
    MachOLoader loader;
    auto loaded = loader.load(thin);
    if (!loaded)
      throw Error("invalid selected Mach-O: " +
                  llvm::toString(loaded.takeError()));
    phases.enter("objc_metadata");
    objc = objcMetadata(*loaded);
    phases.enter("swift_metadata");
    swift = swiftMetadata(*loaded, budget);
    publishJSON(staging / "metadata/swift.json", swift, budget);
  } else {
    fs::create_directories(staging / "sources");
    fs::create_directories(staging / "logs");
    WorkerRequest request;
    request.limits = budget.limits;
    request.architecture = selection.architecture;
    request.pointer_size = selection.pointer_size;
    request.selected_sha256 = imageDigest(selection.bytes, budget);
    request.max_functions = options.max_functions;
    request.remaining = budget.remaining;
    request.output_bytes = budget.output_bytes;
    budget.check();
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        budget.deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0)
      throw Error("mobile analysis exceeded its time budget");
    request.time_remaining_ms = static_cast<uint64_t>(remaining);
    const auto control = workerRequestJSON(request);
    parseWorkerRequest(control);
    const auto text = jsonText(Value(Object(control)));
    if (text.size() > WorkerRequestByteLimit)
      throw Error("iOS worker request exceeds its byte budget");
    writeFile(staging / WorkerRequestPath, text);
    phases.enter("native_export");
    const std::vector<std::string> argv{options.executable, "mobile",
                                       "--internal-ios-worker",
                                       pathText(staging)};
    runTool(argv, staging / "logs/native.log", toolTimeout(budget), staging,
            budget.limits);
    workerFile(staging / WorkerResultPath);
    auto value = parseJSON(readFile(staging / WorkerResultPath,
                                    budget.limits.max_bytes),
                           "iOS worker result");
    const auto &result = object(value, "iOS worker result");
    // Merge the structurally validated envelope before charging validation
    // work. Any later failure still aborts the parent's publication transaction.
    mergeWorkerBudget(result, request, budget);
    const auto &report = *result.getObject("report");
    phases.enter("worker_validation");
    validateWorkerReport(report, staging, request, budget);
    objc = *report.getObject("objc_metadata");
    swift = *report.getObject("swift_metadata");
    outputs = *report.getObject("outputs");
    native_count = *report.get("native_function_count");
    native_limitations = array(report, "native_limitations");
    objc_recovery = *report.get("objc_method_recovery");
    swift_recovery = *report.get("swift_method_recovery");
    fs::remove(staging / WorkerRequestPath);
    fs::remove(staging / WorkerResultPath);
  }
  phases.enter("output_publication");
  if (options.metadata_only) {
    publishJSON(staging / "metadata/objc.json", objc, budget);
    publish(staging / "metadata/objc.h", objcHeader(objc, selection.pointer_size),
            budget);
  }
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
                {"build_target", std::move(build_target)},
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
} // namespace

void ios::runIOSWorker(const fs::path &staging) {
  PhaseTimer phases("worker_request");
  try {
    if (!staging.is_absolute() ||
        !fs::is_directory(fs::symlink_status(staging)) ||
        fs::canonical(staging) != staging)
      throw Error("iOS worker requires a canonical staging directory");
    for (auto name : {"artifacts", "metadata", "sources", "logs"})
      if (!fs::is_directory(fs::symlink_status(staging / name)))
        throw Error("iOS worker workspace directory is unavailable");
    workerFile(staging / WorkerRequestPath);
    workerFile(staging / "artifacts/selected.macho");
    auto value = parseJSON(readFile(staging / WorkerRequestPath,
                                    WorkerRequestByteLimit),
                           "iOS worker request");
    const auto request = parseWorkerRequest(object(value, "iOS worker request"));
    Budget budget(request.limits);
    budget.remaining = request.remaining;
    budget.output_bytes = request.output_bytes;
    const auto now = std::chrono::steady_clock::now();
    const auto available = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::time_point::max() - now).count();
    if (available < 0 || request.time_remaining_ms > uint64_t(available))
      throw Error("iOS worker deadline is not representable");
    budget.deadline = std::min(
        budget.deadline,
        now + std::chrono::milliseconds(request.time_remaining_ms));
    for (auto name : {"artifacts/ios-worker-result.json", "metadata/objc.json",
                      "metadata/swift.json", "metadata/objc.h",
                      "metadata/objc-methods.json", "metadata/swift-methods.json",
                      "metadata/swift-signatures.json", "sources/native.c",
                      "sources/objc.m", "sources/swift.swift"}) {
      std::error_code error;
      auto status = fs::symlink_status(staging / name, error);
      if ((error && error != std::errc::no_such_file_or_directory) ||
          fs::exists(status) || fs::is_symlink(status))
        throw Error("iOS worker output already exists or is inaccessible");
    }
    if (fs::file_size(staging / "artifacts/selected.macho") >
        budget.limits.max_bytes)
      throw Error("Mach-O input exceeds its byte budget");
    phases.enter("macho_loading");
    auto report = recoverWorkerImage(request, staging, budget, phases);
    auto result = workerResultJSON(request, budget, std::move(report));
    const auto text = jsonText(Value(std::move(result)));
    if (text.size() > budget.limits.max_bytes)
      throw Error("iOS worker result exceeds its byte budget");
    budget.check();
    writeFile(staging / WorkerResultPath, text);
  } catch (const Error &error) {
    std::optional<Error> diagnostic;
    try {
      diagnostic.emplace(std::string(error.what()) + "\n[neverd-ios-worker-phases] " +
                         phases.diagnostic());
    } catch (...) {
      // Optional diagnostics never replace the original worker failure.
    }
    if (diagnostic)
      throw *diagnostic;
    throw;
  }
}

llvm::json::Object recoverIOS(const Options &options, const fs::path &staging,
                              Budget &budget) {
  PhaseTimer phases;
  try {
    return recoverIOSImpl(options, staging, budget, phases);
  } catch (const Error &error) {
    std::optional<Error> diagnostic;
    try {
      diagnostic.emplace(std::string(error.what()) + "\n[neverd-ios-phases] " +
                         phases.diagnostic());
    } catch (...) {
      // Failure diagnostics must not replace the original recovery error.
    }
    if (diagnostic)
      throw *diagnostic;
    throw;
  }
}
} // namespace neverd::mobile
