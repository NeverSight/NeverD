// Deterministic C ABI fake for protocol/lifecycle tests; never linked into the
// shipped worker. It deliberately logs through ordinary stdout during work.
#define NEVERD_EXPORTS 1
#include "Protocol.h"

#include "neverd/sdk/NeverDCAPIDisasm.h"
#include "neverd/sdk/NeverDCAPIPersist.h"
#include "neverd/sdk/NeverDCAPIQuery.h"
#include "neverd/sdk/NeverDCAPISession.h"
#include "neverd/sdk/NeverDCAPISigs.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

using neverd::worker::hexAddress;
using neverd::worker::Json;
using neverd::worker::parseAddress;
namespace {
constexpr std::uint64_t Base = 0xffff800012340000ULL;
constexpr std::uint64_t CodeBase = Base + 0x2700;
constexpr unsigned char CodeBytes[] = {0x90, 0x48, 0x83, 0xc4,
                                       0x28, 0xc3, 0x0f};
// A read-only data section of relocated pointers: slot i points to function
// i + 1.
constexpr std::uint64_t DataBase = Base + 0x3000, DataSlots = 8;
// Read-only data holding a UTF-8 and a UTF-16LE string.
constexpr std::uint64_t RodataBase = Base + 0x3100, RodataSize = 0x20;
// Unwritten data: the C library's stderr, copied in as an ELF program's,
// then data only code reaches.
constexpr std::uint64_t BssBase = Base + 0x3200, BssSize = 0x40;
/// The pointer data slot \p index holds: function_(index + 1), except the
/// last slot, which holds the UTF-16LE string.
std::uint64_t slotTarget(std::uint64_t index) {
  return index == DataSlots - 1 ? RodataBase + 8 : Base + 16 * (index + 1);
}
constexpr int ImageFunctions = 600;
constexpr std::uint64_t CodeSize = 16 * ImageFunctions;
/// The image's functions, one every 16 bytes, with the user's edits applied
/// (true created, false deleted), in address order.
std::vector<std::uint64_t>
listedFunctions(const std::map<std::uint64_t, bool> &edits,
                int imageFunctions = ImageFunctions) {
  std::vector<std::uint64_t> result;
  for (int i = 0; i < imageFunctions; ++i)
    if (!edits.count(Base + 16 * i))
      result.push_back(Base + 16 * i);
  for (const auto &[address, created] : edits)
    if (created)
      result.push_back(address);
  std::sort(result.begin(), result.end());
  return result;
}
struct MockSession {
  std::string path, error;
  /// Whether loading reads the debug information beside the input.
  bool debugInfo = true;
  int imageFunctions = ImageFunctions;
  std::map<std::uint64_t, std::string> annotations, names;
  std::map<std::uint64_t, bool> functionEdits;
  /// The user's data items, as neverd_items_json lists them.
  std::map<std::uint64_t, Json> items;
  /// The user's operand formats by address and operand index.
  std::map<std::uint64_t, std::map<int, Json>> operandFormats;
  std::vector<std::uint64_t> functions = listedFunctions({});
  neverd_load_progress_fn progress = nullptr;
  void *progressUser = nullptr;
  /// The loader the next load reads with, and the one the image was read
  /// with, in neverd_session_set_load_options JSON; null reads the sidecar.
  Json requestedLoad, loadedWith = {{"loader", "auto"}};
  /// Source-document tests distinguish page reuse from a fresh preparation,
  /// and refuse graph/IR reads against a different restricted pipeline.
  std::uint64_t preparedEntry = 0;
  unsigned sourcePrepares = 0, sourcePages = 0, sourceRenders = 0;
  bool rejectedPreparation = false;
  bool wholeProgramPrepared = false;
  bool sourceCacheVMFixture() const {
    return path.ends_with("source-cache-evm.bin") ||
           path.ends_with("source-cache-sbf.bin");
  }
  bool sourceCacheFixture() const {
    return path.ends_with("source-cache.bin") ||
           path.ends_with("source-cache-budget.bin") ||
           path.ends_with("source-cache-prepare-fail.bin") ||
           sourceCacheVMFixture();
  }
};
MockSession *session(neverd_session_t s) {
  return static_cast<MockSession *>(s);
}
std::uint64_t codeSize(neverd_session_t s) {
  return session(s)->path.ends_with("make-code.bin")
             ? CodeBase + sizeof(CodeBytes) - Base
             : CodeSize;
}
const char *copy(std::string value) {
  char *result = static_cast<char *>(std::malloc(value.size() + 1));
  std::memcpy(result, value.c_str(), value.size() + 1);
  return result;
}
Json read(const std::string &path) {
  std::ifstream input(path);
  if (!input)
    return Json::array();
  return Json::parse(input);
}
} // namespace
extern "C" {
neverd_session_t neverd_session_create() {
  std::puts("native stdout: session created");
  std::fflush(stdout);
  return new MockSession;
}
void neverd_session_destroy(neverd_session_t s) { delete session(s); }
void neverd_session_set_load_progress(neverd_session_t s,
                                      neverd_load_progress_fn callback,
                                      void *userData) {
  if (s) {
    session(s)->progress = callback;
    session(s)->progressUser = userData;
  }
}
int neverd_session_load(neverd_session_t s, const char *path) {
  const auto notify = [&](const char *phase, unsigned long long done) {
    if (session(s)->progress)
      session(s)->progress(session(s)->progressUser, phase, done, 1, path);
  };
  notify("image", 0);
  if (std::string(path).find("bad-input") != std::string::npos) {
    session(s)->error = "mock load failure";
    return 0;
  }
  session(s)->path = path;
  session(s)->loadedWith = session(s)->requestedLoad.is_null()
                               ? read(std::string(path) + ".neverd-load.json")
                               : session(s)->requestedLoad;
  if (!session(s)->loadedWith.is_object())
    session(s)->loadedWith = {{"loader", "auto"}};
  // A large table for GUI paging tests; its functions are never executed.
  session(s)->imageFunctions =
      session(s)->path.ends_with("many-functions.bin") ? 20000 : ImageFunctions;
  session(s)->functions = listedFunctions({}, session(s)->imageFunctions);
  notify("ready", 1);
  return 1;
}
int neverd_session_is_loaded(neverd_session_t s) {
  return !session(s)->path.empty();
}
int neverd_apply_signature_file(neverd_session_t s, const char *path) {
  session(s)->error.clear();
  if (!std::ifstream(path)) {
    session(s)->error = "mock signature file is missing";
    return -1;
  }
  return 0;
}
int neverd_auto_apply_signatures(neverd_session_t s, const char *path) {
  return neverd_apply_signature_file(s, path);
}
int neverd_session_analyze(neverd_session_t s) {
  if (session(s)->sourceCacheVMFixture()) {
    session(s)->wholeProgramPrepared = true;
    ++session(s)->sourcePrepares;
  }
  std::puts("python-style print during analysis");
  std::fflush(stdout);
  std::this_thread::sleep_for(std::chrono::milliseconds(1800));
  return 1;
}
const char *neverd_session_file_path(neverd_session_t s) {
  return copy(session(s)->path);
}
const char *neverd_session_arch_name(neverd_session_t s) {
  if (session(s)->path.ends_with("source-cache-evm.bin"))
    return copy("evm");
  if (session(s)->path.ends_with("source-cache-sbf.bin"))
    return copy("sbf");
  return copy("x86_64");
}
const char *neverd_session_format_name(neverd_session_t) { return copy("ELF"); }
int neverd_session_bitness(neverd_session_t) { return 64; }
unsigned long long neverd_session_file_size(neverd_session_t) { return 8192; }
neverd_va_t neverd_session_base_addr(neverd_session_t) { return Base; }
neverd_va_t neverd_session_entry_addr(neverd_session_t s) {
  return session(s)->path.find("unknown-entry") == std::string::npos ? Base : 0;
}
const char *neverd_session_load_diagnostics_json(neverd_session_t s) {
  if (s && session(s)->path.find("unknown-entry") != std::string::npos)
    return copy(Json::array({{{"code", "pe.entry_unmapped"},
                              {"message", "Fixture PE entry is unknown."}}})
                    .dump());
  return copy("[]");
}
int neverd_session_segment_count(neverd_session_t) { return 2; }
int neverd_session_section_count(neverd_session_t) { return 2; }
int neverd_session_import_count(neverd_session_t) { return 0; }
int neverd_session_export_count(neverd_session_t) { return 1; }
int neverd_session_symbol_count(neverd_session_t) { return 600; }
const char *neverd_last_error(neverd_session_t s) {
  return copy(session(s)->error);
}
void neverd_free_string(const char *value) {
  std::free(const_cast<char *>(value));
}
const char *neverd_version_number() { return copy("test-1.0"); }
// The mock loads any file as its fixture image, which it lists as a real
// engine lists a header's loader, before the binary file.
const char *neverd_identify_json(const char *path) {
  std::error_code error;
  const auto input =
      path
          ? std::filesystem::path(std::u8string(path, path + std::strlen(path)))
          : std::filesystem::path();
  if (!path || !std::filesystem::is_regular_file(input, error))
    return copy(Json{
        {"rows", Json::array()},
        {"error", std::string("not a regular file: ") + (path ? path : "")}}
                    .dump());
  Json rows = Json::array();
  rows.push_back({{"loader", "fixture"},
                  {"text", "Fixture image"},
                  {"processor", "x86_64"},
                  {"bits", 64},
                  {"endian", "little"},
                  {"loadable", true}});
  rows.push_back({{"loader", "binary"},
                  {"text", "Binary file"},
                  {"processor", ""},
                  {"bits", 0},
                  {"endian", "little"},
                  {"loadable", true}});
  return copy(Json{{"rows", rows}}.dump());
}
// The loader choice as the engine takes it: auto, evm, or a binary file
// with a processor; kept in the input's load options sidecar.
int neverd_session_set_load_options(neverd_session_t s, const char *json) {
  if (!json) {
    session(s)->requestedLoad = nullptr;
    return 0;
  }
  const Json options = Json::parse(json, nullptr, false);
  const std::string loader =
      options.is_object() ? options.value("loader", "auto") : "";
  // A binary file's processor is named, or read from the bytes when absent
  // or auto.
  static const std::set<std::string> processors{
      "", "auto", "x86", "x86_64", "arm", "thumb", "aarch64"};
  if ((loader != "auto" && loader != "evm" && loader != "binary") ||
      (loader == "binary" &&
       !processors.count(options.value("processor", "")))) {
    session(s)->error = "mock: unusable load options";
    return -1;
  }
  session(s)->requestedLoad = options;
  return 0;
}
const char *neverd_session_load_options_json(neverd_session_t s) {
  return copy(session(s)->loadedWith.dump());
}
void neverd_session_set_debug_info_enabled(neverd_session_t s, int enabled) {
  session(s)->debugInfo = enabled != 0;
}
const char *neverd_headers_json(neverd_session_t s) {
  // The Rust and Go fixtures read in their own language too.
  const std::string runtime =
      session(s)->path.ends_with("pseudocode-rust.bin") ? "rust"
      : session(s)->path.ends_with("pseudocode-go.bin") ? "go"
      : (session(s)->path.ends_with("pseudocode-cpp.bin") ||
         session(s)->path.ends_with("code-edits-cpp.bin"))
          ? "cpp"
          : "c";
  Json pseudocode = Json::array({"c"});
  if (runtime != "c")
    pseudocode.push_back(runtime);
  return copy(Json{{"language",
                    {{"runtime", runtime},
                     {"secondary", Json::array()},
                     {"evidence", Json::array()},
                     {"pseudocode", pseudocode}}}}
                  .dump());
}

// Deterministic fixture-only identity. The real engine uses SHA-256; the
// production real-engine test verifies that digest against Python hashlib.
std::string fixtureDigest(neverd_session_t s) {
  std::ifstream input(session(s)->path, std::ios::binary);
  std::uint64_t value = 1469598103934665603ULL;
  char byte;
  while (input.get(byte)) {
    value ^= static_cast<unsigned char>(byte);
    value *= 1099511628211ULL;
  }
  auto digest = hexAddress(value).substr(2);
  digest.insert(0, 16 - digest.size(), '0');
  return digest + digest + digest + digest;
}
const char *neverd_session_input_sha256(neverd_session_t s) {
  return copy(fixtureDigest(s));
}
const char *neverd_dashboard_json(neverd_session_t s) {
  return copy(Json{{"hashes", {{"sha256", fixtureDigest(s)}}}}.dump());
}
int neverd_func_count(neverd_session_t s) {
  return static_cast<int>(session(s)->functions.size());
}
// The fixture's detector finds nothing the image does not list.
int neverd_session_discover_functions(neverd_session_t s) {
  return neverd_func_count(s);
}
neverd_va_t neverd_func_entry(neverd_session_t s, int index) {
  const auto &functions = session(s)->functions;
  return index >= 0 && index < static_cast<int>(functions.size())
             ? functions[index]
             : 0;
}
// Up to the next function, at most the 16 bytes of an image function.
int neverd_func_size(neverd_session_t s, int index) {
  const auto &functions = session(s)->functions;
  if (index < 0 || index >= static_cast<int>(functions.size()))
    return 16;
  const auto end = index + 1 < static_cast<int>(functions.size())
                       ? functions[index + 1]
                       : Base + codeSize(s);
  return static_cast<int>(std::min<std::uint64_t>(16, end - functions[index]));
}
const char *neverd_func_name(neverd_session_t s, int index) {
  const auto address = neverd_func_entry(s, index);
  auto it = session(s)->names.find(address);
  if (it == session(s)->names.end() && address == Base + 0x150 &&
      session(s)->path.ends_with("pseudocode-navigation.bin"))
    return copy("_ZN3BarC1Ev");
  if (it != session(s)->names.end())
    return copy(it->second);
  if ((address - Base) % 16 == 0)
    return copy("function_" + std::to_string((address - Base) / 16));
  char name[32];
  std::snprintf(name, sizeof name, "sub_%llX",
                static_cast<unsigned long long>(address));
  return copy(name);
}
int neverd_func_find_by_addr(neverd_session_t s, neverd_va_t address) {
  const auto &functions = session(s)->functions;
  const auto it = std::lower_bound(functions.begin(), functions.end(), address);
  return it != functions.end() && *it == address
             ? static_cast<int>(it - functions.begin())
             : -1;
}
int neverd_func_find_by_name(neverd_session_t s, const char *name) {
  if (std::string(name).starts_with("delayed_")) {
    std::puts("fixture delayed lookup started");
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    return std::string(name) == "delayed_function" ? 22 : -1;
  }
  for (int i = 0, count = neverd_func_count(s); i < count; ++i) {
    const auto *value = neverd_func_name(s, i);
    const bool match = std::string(value) == name;
    neverd_free_string(value);
    if (match)
      return i;
  }
  return -1;
}
const char *neverd_resolve_addr(neverd_session_t s, neverd_va_t address) {
  const auto index = neverd_func_find_by_addr(s, address);
  if (index < 0)
    return nullptr;
  const auto *raw = neverd_func_name(s, index);
  const std::string name(raw);
  neverd_free_string(raw);
  return copy(Json{
      {"type", "function"},
      {"addr", hexAddress(address)},
      {"name", name},
      {"display_name", name},
      {"linkage_name",
       name}}.dump());
}
int neverd_read_bytes(neverd_session_t, neverd_va_t address,
                      unsigned char *buffer, int size) {
  if (size >= 0 && address >= CodeBase &&
      address - CodeBase < sizeof(CodeBytes)) {
    const auto available = static_cast<int>(
        std::min<std::uint64_t>(size, CodeBase + sizeof(CodeBytes) - address));
    std::copy_n(CodeBytes + (address - CodeBase), available, buffer);
    return available;
  }
  if (size >= 0 && address >= RodataBase && address - RodataBase < RodataSize) {
    // "\u4e2d\u6587" in UTF-8 at +0 and "Wide" in UTF-16LE at +8, zero filled.
    static constexpr unsigned char Rodata[RodataSize] = {
        0xe4, 0xb8, 0xad, 0xe6, 0x96, 0x87, 0, 0, 'W', 0, 'i', 0, 'd', 0, 'e'};
    const auto available = static_cast<int>(
        std::min<std::uint64_t>(size, RodataBase + RodataSize - address));
    std::copy_n(Rodata + (address - RodataBase), available, buffer);
    return available;
  }
  if (size >= 0 && address >= DataBase && address - DataBase < DataSlots * 8) {
    const auto available = static_cast<int>(
        std::min<std::uint64_t>(size, DataBase + DataSlots * 8 - address));
    for (int i = 0; i < available; ++i) {
      const auto offset = address - DataBase + i;
      const std::uint64_t pointer = slotTarget(offset / 8);
      buffer[i] = static_cast<unsigned char>(pointer >> (8 * (offset % 8)));
    }
    return available;
  }
  if (address < Base || address - Base >= 9600 || size < 0)
    return 0;
  // Each 16-byte function body is the byte ramp 00 01 02 ... 0f.
  const auto available =
      static_cast<int>(std::min<std::uint64_t>(size, Base + 9600 - address));
  for (int i = 0; i < available; ++i)
    buffer[i] = static_cast<unsigned char>((address - Base + i) & 0xf);
  return available;
}
// Each 16-byte fixture function is one-byte instructions: nops, a jump back
// to its entry at offset 8 and a return at offset 15.
constexpr std::uint64_t JumpOffset = 8, ReturnOffset = 15;
/// Functions with stack frames: offset -> mnemonic, operands, stack pointer
/// move and, for a conditional jump, its target offset.
struct FrameRow {
  const char *mnemonic, *operands;
  int move;
  int branch = -1;
};
/// function_5 keeps a frame without a frame pointer.  Its loop back to the
/// entry restores the stack pointer, and the code after the loop is
/// unreachable.
const std::map<std::uint64_t, FrameRow> FrameFunction = {
    {0, {"push", "rbx", -8}},
    {1, {"sub", "rsp, 0x20", -0x20}},
    {2, {"mov", "qword ptr [rsp + 0x18], rax", 0}},
    {3, {"lea", "rdi, [rsp + 8]", 0}},
    {4, {"mov", "rax, qword ptr [rsp + 0x30]", 0}},
    {5, {"movups", "xmmword ptr [rsp + 8], xmm0", 0}},
    {6, {"add", "rsp, 0x20", 0x20}},
    {7, {"pop", "rbx", 8}},
    {9, {"mov", "rax, qword ptr [rsp + 8]", 0}},
};
constexpr std::uint64_t FrameFunctionEntry = Base + 0x50;
/// function_6 reaches offset 4 both past a push and around it, so the stack
/// pointer's distance there is unknown.
const std::map<std::uint64_t, FrameRow> JoinFunction = {
    {0, {"push", "rbx", -8}},
    {1, {"mov", "rax, qword ptr [rsp]", 0}},
    {2, {"je", "4", 0, 4}},
    {3, {"push", "rcx", -8}},
    {4, {"mov", "rax, qword ptr [rsp + 8]", 0}},
    {7, {"ret", "", 0}},
};
constexpr std::uint64_t JoinFunctionEntry = Base + 0x60;

Json fixtureInstructions(neverd_va_t address, int count, bool flow,
                         bool stack) {
  Json result = Json::array();
  if (address >= CodeBase && address < CodeBase + sizeof(CodeBytes)) {
    for (int i = 0; i < count; ++i) {
      const auto offset = address - CodeBase;
      if (offset != 0 && offset != 1 && offset != 5)
        break; // The last byte is a truncated x86 opcode.
      const bool add = offset == 1, ret = offset == 5;
      Json row{{"addr", hexAddress(address)},
               {"size", add ? 4 : 1},
               {"mnemonic", add   ? "add"
                            : ret ? "ret"
                                  : "nop"},
               {"op_str", add ? "rsp, 0x28" : ""},
               {"bytes", add   ? "4883c428"
                         : ret ? "c3"
                               : "90"}};
      if (flow && ret)
        row["flow"] = "ret";
      result.push_back(std::move(row));
      address += add ? 4 : 1;
    }
    return result;
  }
  for (int i = 0; i < count && address >= Base && address + i - Base < 9600;
       ++i) {
    const auto at = address + i;
    const auto offset = (at - Base) % 16;
    const auto entry = at - offset;
    Json row = {{"addr", hexAddress(at)},
                {"size", 1},
                {"mnemonic", "nop"},
                {"op_str", ""},
                {"bytes", "90"}};
    if (stack)
      row["sp"] = 0;
    const auto *rows = entry == FrameFunctionEntry  ? &FrameFunction
                       : entry == JoinFunctionEntry ? &JoinFunction
                                                    : nullptr;
    if (const auto frame = rows ? rows->find(offset) : FrameFunction.end();
        rows && frame != rows->end()) {
      row["mnemonic"] = frame->second.mnemonic;
      row["op_str"] = frame->second.operands;
      if (stack)
        row["sp"] = frame->second.move;
      if (flow && frame->second.branch >= 0) {
        row["op_str"] = hexAddress(entry + frame->second.branch);
        row["flow"] = "cjump";
        row["target"] = hexAddress(entry + frame->second.branch);
      } else if (flow && std::string_view(frame->second.mnemonic) == "ret") {
        row["flow"] = "ret";
      }
    }
    if (offset == JumpOffset) {
      row["mnemonic"] = "jmp";
      row["op_str"] = hexAddress(entry);
      row["bytes"] = "eb";
      if (flow) {
        row["flow"] = "jump";
        row["target"] = hexAddress(entry);
      }
    } else if (offset == ReturnOffset) {
      row["mnemonic"] = "ret";
      row["bytes"] = "c3";
      if (flow)
        row["flow"] = "ret";
    }
    result.push_back(std::move(row));
  }
  return result;
}
const char *neverd_disasm_json(neverd_session_t s, neverd_va_t address,
                               int count) {
  session(s)->error.clear();
  return copy(fixtureInstructions(address, count, false, false).dump());
}
const char *neverd_disasm_json_ex(neverd_session_t s, neverd_va_t address,
                                  int count, unsigned options) {
  session(s)->error.clear();
  return copy(fixtureInstructions(address, count, options & NEVERD_DISASM_FLOW,
                                  options & NEVERD_DISASM_STACK)
                  .dump());
}
const char *neverd_code_refs_json(neverd_session_t s, neverd_va_t firstEntry,
                                  int maxFunctions) {
  session(s)->error.clear();
  maxFunctions = std::clamp(maxFunctions, 1, 4096);
  Json refs = Json::array();
  int first =
      firstEntry <= Base ? 0 : static_cast<int>((firstEntry - Base + 15) / 16);
  int index = first;
  for (; index < 600 && index < first + maxFunctions; ++index) {
    const auto entry = Base + 16 * static_cast<std::uint64_t>(index);
    refs.push_back({hexAddress(entry + JumpOffset), hexAddress(entry), "jump"});
    // function_3 calls function_2 through data slot 1.
    if (index == 3)
      refs.push_back({hexAddress(entry + 4), hexAddress(Base + 0x20), "icall"});
    // function_7 takes the UTF-8 string's address, reads the slot that
    // holds the UTF-16LE string's, and points into both strings: at the
    // second character of the UTF-8 one and at an odd byte of the other.
    if (index == 7) {
      refs.push_back({hexAddress(entry + 2), hexAddress(RodataBase), "offset"});
      refs.push_back({hexAddress(entry + 3),
                      hexAddress(DataBase + (DataSlots - 1) * 8), "read", 8});
      refs.push_back(
          {hexAddress(entry + 4), hexAddress(RodataBase + 3), "offset"});
      refs.push_back(
          {hexAddress(entry + 5), hexAddress(RodataBase + 11), "offset"});
      // It also reads eight bytes of unwritten data no symbol names.
      refs.push_back(
          {hexAddress(entry + 6), hexAddress(BssBase + 0x30), "read", 8});
    }
  }
  return copy(
      Json{{"refs", refs},
           {"next_entry",
            index < 600 ? Json(hexAddress(Base + 16 * index)) : Json(nullptr)},
           {"function_count", 600}}
          .dump());
}
const char *neverd_string_refs_json(neverd_session_t s, const char *options,
                                    neverd_va_t firstEntry, int maxFunctions) {
  session(s)->error.clear();
  const Json parsed =
      options ? Json::parse(options, nullptr, false) : Json::object();
  const auto wanted = [&](const char *encoding) {
    if (!parsed.contains("encodings"))
      return true;
    for (const auto &name : parsed["encodings"])
      if (name == encoding)
        return true;
    return false;
  };
  const auto minimum = parsed.value("min_length", 4);
  // function_7's references, as the engine joins them with the strings: the
  // UTF-8 string whole and from its second character ("\u6587"), and the
  // UTF-16LE one through data slot 7 and from its third character ("de").
  constexpr std::uint64_t Entry = Base + 16 * 7;
  const std::uint64_t slot = DataBase + (DataSlots - 1) * 8;
  Json refs = Json::array();
  if (firstEntry <= Entry && maxFunctions >= 1) {
    if (wanted("utf-8"))
      refs.push_back({hexAddress(Entry + 2), hexAddress(RodataBase),
                      hexAddress(RodataBase), 0, "offset", nullptr, "nop"});
    if (wanted("utf-16le"))
      refs.push_back({hexAddress(Entry + 3), hexAddress(RodataBase + 8),
                      hexAddress(RodataBase + 8), 0, "read", hexAddress(slot),
                      "nop"});
    if (wanted("utf-8") && minimum <= 1)
      refs.push_back({hexAddress(Entry + 4), hexAddress(RodataBase + 3),
                      hexAddress(RodataBase), 3, "offset", nullptr, "nop"});
    if (wanted("utf-16le") && minimum <= 2)
      refs.push_back({hexAddress(Entry + 5), hexAddress(RodataBase + 11),
                      hexAddress(RodataBase + 8), 2, "offset", nullptr, "nop"});
  }
  return copy(
      Json{{"refs", refs}, {"next_entry", nullptr}, {"function_count", 600}}
          .dump());
}
const char *neverd_pointer_refs_json(neverd_session_t s, neverd_va_t firstSlot,
                                     int maxSlots) {
  session(s)->error.clear();
  Json refs = Json::array();
  std::uint64_t slot = std::max<std::uint64_t>(firstSlot, DataBase);
  slot = DataBase + (slot - DataBase + 7) / 8 * 8;
  for (int count = 0; slot < DataBase + DataSlots * 8 && count < maxSlots;
       ++count, slot += 8)
    refs.push_back({hexAddress(slot),
                    hexAddress(slotTarget((slot - DataBase) / 8)), "offset"});
  return copy(Json{{"refs", refs},
                   {"next_slot", slot < DataBase + DataSlots * 8
                                     ? Json(hexAddress(slot))
                                     : Json(nullptr)}}
                  .dump());
}
int neverd_pointer_at(neverd_session_t, neverd_va_t address, neverd_va_t *slot,
                      neverd_va_t *target) {
  if (address < DataBase || address >= DataBase + DataSlots * 8)
    return 0;
  const auto first = DataBase + (address - DataBase) / 8 * 8;
  if (slot)
    *slot = first;
  if (target)
    *target = slotTarget((first - DataBase) / 8);
  return 1;
}
const char *neverd_unwind_frame_json(neverd_session_t, neverd_va_t address) {
  // function_0 has a plain frame; function_1 names a personality.
  if (address >= Base && address < Base + 16)
    return copy(Json{
        {"begin", hexAddress(Base)},
        {"end", hexAddress(Base + 16)},
        {"encoding", "dwarf-fde"},
        {"language_data",
         false}}.dump());
  if (address >= Base + 16 && address < Base + 32)
    return copy(Json{
        {"begin", hexAddress(Base + 16)},
        {"end", hexAddress(Base + 32)},
        {"encoding", "dwarf-fde"},
        {"personality", "__gxx_personality_v0"},
        {"language_data",
         true}}.dump());
  return copy("null");
}
static int prepareMockFunction(neverd_session_t s, neverd_va_t address) {
  session(s)->error.clear();
  if (session(s)->sourceCacheFixture()) {
    ++session(s)->sourcePrepares;
    session(s)->preparedEntry = 0;
    if (session(s)->path.ends_with("source-cache-prepare-fail.bin") &&
        address == Base + 16 && !session(s)->rejectedPreparation) {
      session(s)->rejectedPreparation = true;
      session(s)->error = "fixture preparation failed after retiring old IR";
      return 0;
    }
    session(s)->preparedEntry = address;
  }
  if (session(s)->path.ends_with("pseudocode-parallel.bin") &&
      (address == Base || address == Base + 16)) {
    std::printf("fixture parallel decompile %s started\n",
                hexAddress(address).c_str());
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  if (session(s)->path.ends_with("pseudocode-slow.bin") && address == Base) {
    std::puts("fixture slow decompile started");
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(30));
  }
  return 1;
}
int neverd_prepare_function(neverd_session_t s, neverd_va_t address) {
  return prepareMockFunction(s, address);
}
const char *neverd_decompile(neverd_session_t s, neverd_va_t address) {
  ++session(s)->sourceRenders;
  if (!prepareMockFunction(s, address))
    return copy("");
  if (session(s)->path.ends_with("pseudocode-import.bin"))
    return copy("int caller(void) {\n  return function_22();\n}\n");
  if (session(s)->path.ends_with("pseudocode-global.bin"))
    return copy("extern int global_value; /* 0xffff800012343000 */\n"
                "int caller(void) {\n  return global_value;\n}\n");
  if (session(s)->path.ends_with("pseudocode-delayed.bin"))
    return copy("int caller(void) {\n  return delayed_function();\n}\n");
  if (session(s)->path.ends_with("pseudocode-delayed-error.bin"))
    return copy("int caller(void) {\n  return delayed_missing();\n}\n");
  if (session(s)->path.ends_with("pseudocode-navigation.bin"))
    return copy(address == Base + 0x140
                    ? "int function_20(void) {\n  return function_22();\n}\n"
                    : "int called(void) {\n  return 42;\n}\n");
  std::string text;
  for (int i = 0; i < 700; ++i)
    text += "// code line " + std::to_string(i) + "\n";
  return copy(text);
}
const char *neverd_ir_low(neverd_session_t s, neverd_va_t a) {
  return neverd_decompile(s, a);
}
const char *neverd_ir_med(neverd_session_t s, neverd_va_t a) {
  return neverd_decompile(s, a);
}
const char *neverd_ir_high(neverd_session_t s, neverd_va_t a) {
  return neverd_decompile(s, a);
}
const char *neverd_ir_llvm(neverd_session_t s, neverd_va_t a) {
  return neverd_decompile(s, a);
}
const char *neverd_decompile_llvm(neverd_session_t s, neverd_va_t a) {
  return neverd_decompile(s, a);
}
namespace {
/// A source page in a language: C as neverd_decompile gives it, or the Rust
/// and Go views, whose source name `core::fmt::write` the page locates.
std::string spelledPage(neverd_session_t s, neverd_va_t address,
                        const std::string &representation, std::size_t offset,
                        std::size_t limit) {
  std::string language = representation;
  if (representation == "source")
    language = session(s)->path.ends_with("pseudocode-rust.bin")  ? "rust"
               : session(s)->path.ends_with("pseudocode-go.bin")  ? "go"
               : session(s)->path.ends_with("pseudocode-cpp.bin") ? "cpp"
                                                                  : "c";
  std::string full;
  if (language == "c") {
    const char *c = neverd_decompile(s, address);
    full = c;
    std::free(const_cast<char *>(c));
  } else if (language == "cpp") {
    full = "int Demo::length(const std::string *text) {\n"
           "    return text->size();\n"
           "}\n";
  } else if (language == "rust") {
    full = "// NeverD pseudocode in Rust syntax\n"
           "unsafe fn rust_probe::main() -> i64 {\n"
           "    let mut v0: i64;\n"
           "    'switch1: {\n"
           "        v0 = core::fmt::write(1);\n"
           "    }\n"
           "    return v0 as i64;\n"
           "}\n";
  } else {
    full = "// NeverD pseudocode in Go syntax\n"
           "func main.main() int64 {\n"
           "    var v0 int64\n"
           "    v0 = core::fmt::write(1)\n"
           "    return v0\n"
           "}\n";
  }
  std::vector<std::size_t> starts = {0};
  for (std::size_t i = 0; i + 1 < full.size(); ++i)
    if (full[i] == '\n')
      starts.push_back(i + 1);
  const auto total = starts.size();
  const auto first = std::min(offset, total);
  const auto end = first + std::min(limit, total - first);
  const auto begin = starts[std::min(first, total - 1)];
  const auto stop = end < total ? starts[end] : full.size();
  Json rows = Json::array();
  for (auto i = first; i < end; ++i)
    rows.push_back(
        {{"line", i},
         {"object_id", representation + ":line:" + std::to_string(i)},
         {"kind", "source"},
         {"mapping_status", "unmapped"},
         {"addresses", Json::array()}});
  Json names = Json::array();
  const std::string name = "core::fmt::write";
  for (auto at = full.find(name); at != std::string::npos;
       at = full.find(name, at + 1))
    if (at >= begin && at < stop)
      names.push_back({{"begin_byte", at},
                       {"end_byte", at + name.size()},
                       {"identifier", "core_fmt_write"},
                       {"symbol", "_ZN4core3fmt5write17h0123456789abcdefE"},
                       {"address", hexAddress(Base + 0x160)}});
  return Json{{"schema_version", 1},
              {"address", hexAddress(address)},
              {"representation", representation},
              {"mapping_status", "library_regions"},
              {"text", full.substr(begin, stop - begin)},
              {"rows", rows},
              {"library_regions", Json::array()},
              {"offset", first},
              {"byte_offset", begin},
              {"total_lines", total},
              {"complete", end == total},
              {"next_offset", end == total ? Json(nullptr) : Json(end)},
              {"dialect", language},
              {"unread", language == "go" ? Json::array({"a volatile access"})
                                          : Json::array()},
              {"source_names", names}}
      .dump();
}
} // namespace

const char *neverd_ir_view_json(neverd_session_t s, neverd_va_t address,
                                const char *representation, std::size_t offset,
                                std::size_t limit) {
  session(s)->error.clear();
  if (session(s)->sourceCacheFixture()) {
    auto &state = *session(s);
    if (!state.wholeProgramPrepared && state.preparedEntry != address) {
      state.error = "fixture source read used another function's pipeline";
      return nullptr;
    }
    ++state.sourcePages;
    const bool large = state.path.ends_with("source-cache-budget.bin");
    const std::size_t total = large ? 9000 : 4;
    const auto first = std::min(offset, total);
    const auto end = first + std::min(limit, total - first);
    std::string text;
    Json rows = Json::array();
    const auto line = [&](std::size_t index) {
      if (index == 0)
        return "int function_" + std::to_string((address - Base) / 16) +
               "(void) {\n";
      if (index == 1)
        return std::string("  int value = 1;\n");
      if (index == 2)
        return std::string("  return value;\n");
      if (index == 3)
        return std::string("}\n");
      return "// " + std::string(512, 'x') + "\n";
    };
    std::size_t begin = 0;
    for (std::size_t i = 0; i < first; ++i)
      begin += line(i).size();
    for (auto i = first; i < end; ++i) {
      text += line(i);
      rows.push_back(
          {{"line", i},
           {"object_id", "source:" + std::to_string(i)},
           {"kind", "source"},
           {"mapping_status", i == 2 ? "instruction_anchor" : "unmapped"},
           {"addresses",
            i == 2 ? Json::array({hexAddress(address + 2)}) : Json::array()}});
    }
    return copy(Json{{"schema_version", 1},
                     {"address", hexAddress(address)},
                     {"representation", representation},
                     {"dialect", "c"},
                     {"text", text},
                     {"rows", rows},
                     {"offset", first},
                     {"byte_offset", begin},
                     {"total_lines", total},
                     {"complete", end == total},
                     {"next_offset", end == total ? Json(nullptr) : Json(end)},
                     {"library_regions", Json::array()},
                     {"source_names", Json::array()},
                     {"fixture_prepares", state.sourcePrepares},
                     {"fixture_pages", state.sourcePages},
                     {"fixture_renders", state.sourceRenders}}
                    .dump());
  }
  if (session(s)->path.ends_with("code-edits.bin") ||
      session(s)->path.ends_with("code-edits-cpp.bin") ||
      session(s)->path.ends_with("code-edits-mapped.bin") ||
      session(s)->path.ends_with("tab-sync-paged.bin")) {
    const bool paged = session(s)->path.ends_with("tab-sync-paged.bin");
    std::string full = "#include <stdint.h>\n"
                       "int32_t function_20(int32_t v0) {\n"
                       "  int32_t v1 = v0 + function_22();\n"
                       "  const char *message = \"v0 v1 function_22\";\n"
                       "  return v1;\n"
                       "}\n";
    if (paged)
      for (int i = 6; i < 700; ++i)
        full += "// code line " + std::to_string(i) + "\n";
    std::vector<std::size_t> starts{0};
    for (std::size_t i = 0; i + 1 < full.size(); ++i)
      if (full[i] == '\n')
        starts.push_back(i + 1);
    const auto first = std::min(offset, starts.size());
    const auto end = first + std::min(limit, starts.size() - first);
    Json rows = Json::array();
    for (auto i = first; i < end; ++i) {
      const bool mapped =
          (session(s)->path.ends_with("code-edits-mapped.bin") && i == 2) ||
          (paged && i == 550);
      rows.push_back({{"line", i},
                      {"object_id", "source:" + std::to_string(i)},
                      {"kind", "source"},
                      {"mapping_status", mapped ? "mapped" : "unmapped"},
                      {"addresses",
                       mapped ? (paged ? Json::array({hexAddress(address + 8),
                                                      hexAddress(address + 9)})
                                       : Json::array({hexAddress(address + 2)}))
                              : Json::array()}});
    }
    const auto beginByte = first < starts.size() ? starts[first] : full.size();
    const auto endByte = end < starts.size() ? starts[end] : full.size();
    return copy(Json{
        {"schema_version", 1},
        {"address", hexAddress(address)},
        {"representation", representation},
        {"dialect", "c"},
        {"text", full.substr(beginByte, endByte - beginByte)},
        {"rows", rows},
        {"offset", first},
        {"byte_offset", beginByte},
        {"total_lines", starts.size()},
        {"complete", end == starts.size()},
        {"next_offset", end == starts.size() ? Json(nullptr) : Json(end)},
        {"prelude", {{"lines", 1}, {"end_byte", starts[1]}}},
        {"library_regions",
         paged ? Json::array(
                     {{{"id", "late-operation"},
                       {"display_name", "fixture operation"},
                       {"foldable", true},
                       {"mapping_status", "mapped"},
                       {"spans", Json::array({{{"begin_byte", starts[549]},
                                               {"end_byte", starts[552]}}})}}})
               : Json::array()},
        {"source_names", Json::array()}}
                    .dump());
  }
  if (std::string(representation) == "source" ||
      std::string(representation) == "rust" ||
      std::string(representation) == "go") {
    // One function of the Rust fixture is refused.
    if (session(s)->path.ends_with("pseudocode-rust.bin") &&
        address == Base + 0x180) {
      session(s)->error = "fixture refuses this function";
      return nullptr;
    }
    return copy(spelledPage(s, address, representation, offset, limit));
  }
  // LLVM-C pages place the definition after a three-line prelude.
  if (std::string(representation) == "llvmc") {
    // Globals declared as decompiled C and as C through LLVM declare them.
    const std::string prelude =
        "#include <stdint.h>\n"
        "typedef struct QDomNode QDomNode;\n"
        "extern int Bar_ctor() __asm__(\"_ZN3BarC1Ev\"); /* Bar::Bar() */\n"
        "/* neverd.image: 0x20 */\n"
        "int64_t dso_handle = 0x20;\n"
        "extern uint64_t qword_10; /* 0x10 */\n\n";
    std::vector<std::string> lines = {
        "#include <stdint.h>\n",
        "typedef struct QDomNode QDomNode;\n",
        "extern int Bar_ctor() __asm__(\"_ZN3BarC1Ev\"); /* "
        "Bar::Bar() */\n",
        "/* neverd.image: 0x20 */\n",
        "int64_t dso_handle = 0x20;\n",
        "extern uint64_t qword_10; /* 0x10 */\n",
        "\n",
        "/* neverd.entry */\n"};
    const bool large = session(s)->path.ends_with("pseudocode-large.bin");
    for (int i = 0; i < (large ? 20000 : 700); ++i) {
      if (session(s)->path.ends_with("pseudocode-navigation.bin") && i < 3) {
        if (i == 0)
          lines.push_back("int caller(void) {\n");
        else if (i == 1)
          lines.push_back(address == Base + 0x140 ? "  return Bar_ctor();\n"
                                                  : "  return 42;\n");
        else
          lines.push_back("}\n");
      } else if (large) {
        lines.push_back(
            "  value = (value ^ 12345) + helper(value); // code line " +
            std::to_string(i) + "\n");
      } else {
        lines.push_back("// code line " + std::to_string(i) + "\n");
      }
    }
    const auto total = lines.size();
    const auto first = std::min(offset, total);
    const auto end = first + std::min(limit, total - first);
    std::size_t byteOffset = 0;
    for (std::size_t i = 0; i < first; ++i)
      byteOffset += lines[i].size();
    std::string text;
    Json rows = Json::array();
    for (auto i = first; i < end; ++i) {
      text += lines[i];
      rows.push_back({{"line", i},
                      {"object_id", "llvmc:line:" + std::to_string(i)},
                      {"kind", "source"},
                      {"mapping_status", "unmapped"},
                      {"addresses", Json::array()}});
    }
    return copy(Json{{"schema_version", 1},
                     {"address", hexAddress(address)},
                     {"representation", representation},
                     {"mapping_status", "library_regions"},
                     {"text", text},
                     {"rows", rows},
                     {"library_regions", Json::array()},
                     {"offset", first},
                     {"byte_offset", byteOffset},
                     {"total_lines", total},
                     {"complete", end == total},
                     {"next_offset", end == total ? Json(nullptr) : Json(end)},
                     {"prelude", {{"lines", 7}, {"end_byte", prelude.size()}}}}
                    .dump());
  }
  // Otherwise this mock represents an older page API that maps Low/Med only;
  // C requests exercise the worker's legacy fallback.
  if (std::string(representation) != "low" &&
      std::string(representation) != "med")
    return copy(Json{{"mapping_status", "unsupported_representation"},
                     {"rows", Json::array()}}
                    .dump());
  Json rows = Json::array();
  std::string text;
  const auto end =
      std::min<std::size_t>(700, std::min<std::size_t>(offset, 700) + limit);
  for (auto i = offset; i < end; ++i) {
    text += "// code line " + std::to_string(i) + "\n";
    rows.push_back(
        {{"line", i},
         {"object_id",
          std::string(representation) + ":op:" + std::to_string(i)},
         {"kind", i == 0 ? "header" : "operation"},
         {"mapping_status", i == 0 ? "unmapped" : "instruction_anchor"},
         {"addresses",
          i == 0 ? Json::array() : Json::array({hexAddress(address + i)})}});
  }
  return copy(Json{{"schema_version", 1},
                   {"address", hexAddress(address)},
                   {"representation", representation},
                   {"text", text},
                   {"rows", rows},
                   {"mapping_status", "instruction_anchors"},
                   {"provenance_complete", false},
                   {"offset", offset},
                   {"total_lines", 700},
                   {"next_offset", end == 700 ? Json(nullptr) : Json(end)},
                   {"complete", end == 700}}
                  .dump());
}
// The fixture's function_9 reads as a demangled C++ method.
const char *neverd_demangle(const char *name) {
  if (!name)
    return nullptr;
  return copy(std::string_view(name) == "function_9" ? "Widget::draw()" : name);
}
const char *neverd_switches_json(neverd_session_t, neverd_va_t first, int) {
  // function_8 loads a table of three offsets from the table, in the free
  // end of the read-only data, and dispatches through it.
  constexpr std::uint64_t Entry = Base + 0x80, Table = RodataBase + 0x14;
  Json switches = Json::array();
  if (first <= Entry)
    switches.push_back(
        {{"function", hexAddress(Entry)},
         {"jump", hexAddress(Entry + 6)},
         {"load", hexAddress(Entry + 5)},
         {"table", hexAddress(Table)},
         {"entry_size", 4},
         {"stride", 4},
         {"storage", Json::array({Json::array({hexAddress(Table), 4, 4, 3})})},
         {"form", "table_relative"},
         {"targets",
          Json::array({Json::array({hexAddress(Entry + 8), 0, 0}),
                       Json::array({hexAddress(Entry + 10), 1, 1}),
                       Json::array({hexAddress(Entry + 8), 2, 2})})}});
  return copy(Json{{"switches", switches}, {"next_entry", nullptr}}.dump());
}
const char *neverd_string_encodings_json(void) {
  return copy(Json::array({{{"name", "ascii"},
                            {"spelling", ""},
                            {"unit", 1},
                            {"default", true}},
                           {{"name", "utf-8"},
                            {"spelling", "UTF-8"},
                            {"unit", 1},
                            {"default", true}},
                           {{"name", "utf-16le"},
                            {"spelling", "UTF-16LE"},
                            {"unit", 2},
                            {"default", true}},
                           {{"name", "gbk"},
                            {"spelling", "GBK"},
                            {"unit", 1},
                            {"default", false},
                            {"legacy", true}},
                           {{"name", "big5"},
                            {"spelling", "Big5"},
                            {"unit", 1},
                            {"default", false},
                            {"legacy", true}}})
                  .dump());
}
const char *neverd_strings_ex_json(neverd_session_t, const char *options) {
  const Json parsed =
      options ? Json::parse(options, nullptr, false) : Json::object();
  const auto wanted = [&](const char *encoding) {
    if (!parsed.contains("encodings"))
      return true;
    for (const auto &name : parsed["encodings"])
      if (name == encoding)
        return true;
    return false;
  };
  Json items = Json::array();
  if (wanted("utf-8"))
    items.push_back({{"addr", hexAddress(RodataBase)},
                     {"length", 6},
                     {"chars", 2},
                     {"encoding", "utf-8"},
                     {"value", "\xe4\xb8\xad\xe6\x96\x87"}});
  if (wanted("utf-16le"))
    items.push_back({{"addr", hexAddress(RodataBase + 8)},
                     {"length", 8},
                     {"chars", 4},
                     {"encoding", "utf-16le"},
                     {"value", "Wide"}});
  return copy(items.dump());
}
// The rows neverd_strings_ex_json lists, a page at a time.
const char *neverd_strings_page_json(neverd_session_t s, const char *options,
                                     neverd_va_t firstAddress, int maxRows) {
  const char *raw = neverd_strings_ex_json(s, options);
  if (!raw)
    return nullptr;
  const auto rows = Json::parse(raw);
  std::free(const_cast<char *>(raw));
  Json strings = Json::array();
  Json next = nullptr;
  for (const auto &row : rows) {
    if (parseAddress(row.at("addr").get<std::string>()) < firstAddress)
      continue;
    if (static_cast<int>(strings.size()) >= std::max(1, maxRows)) {
      next = row.at("addr");
      break;
    }
    strings.push_back(row);
  }
  return copy(Json{{"next_addr", next}, {"strings", strings}}.dump());
}
const char *neverd_decode_text_json(const unsigned char *bytes, int size,
                                    const char *encoding) {
  // ASCII shows printable bytes; UTF-16LE shows printable ASCII units at
  // their first byte and nothing at the second.  Others are unknown.
  const std::string_view name(encoding);
  if ((name != "ascii" && name != "utf-16le") || size < 0)
    return nullptr;
  const auto shown = [](unsigned value) {
    return value >= 0x20 && value < 0x7f
               ? Json(std::string(1, static_cast<char>(value)))
               : Json(nullptr);
  };
  Json cells = Json::array();
  for (int i = 0; i < size; ++i) {
    if (name == "ascii") {
      cells.push_back(shown(bytes[i]));
    } else if (i % 2 == 0 && i + 1 < size) {
      cells.push_back(shown(bytes[i] | bytes[i + 1] << 8));
    } else {
      cells.push_back(i % 2 ? Json("") : Json(nullptr));
    }
  }
  return copy(Json{{"cells", cells}}.dump());
}
const char *neverd_strings_json(neverd_session_t, int) {
  Json items = Json::array();
  for (int i = 0; i < 600; ++i)
    items.push_back({{"addr", hexAddress(Base + i)},
                     {"value", "string " + std::to_string(i)},
                     {"length", 10}});
  return copy(items.dump());
}
const char *neverd_segments_json(neverd_session_t s) {
  return copy(Json::array({{{"name", ".text"},
                            {"va", hexAddress(Base)},
                            {"size", hexAddress(codeSize(s))},
                            {"flags", "R-X"}},
                           {{"name", ".data.rel.ro"},
                            {"va", hexAddress(DataBase)},
                            {"size", "0x40"},
                            {"flags", "R--"}},
                           {{"name", ".rodata"},
                            {"va", hexAddress(RodataBase)},
                            {"size", "0x20"},
                            {"flags", "R--"}},
                           {{"name", ".bss"},
                            {"va", hexAddress(BssBase)},
                            {"size", "0x40"},
                            {"flags", "RW-"}}})
                  .dump());
}
const char *neverd_sections_json(neverd_session_t s) {
  // Spelled like the engine: numeric sizes and R/W/X flags.
  return copy(Json::array({{{"name", ".text"},
                            {"segment", ".text"},
                            {"va", hexAddress(Base)},
                            {"size", codeSize(s)},
                            {"file_off", 0x1000},
                            {"file_sz", codeSize(s)},
                            {"alignment", 16},
                            {"flags", "R-X"}},
                           {{"name", ".data.rel.ro"},
                            {"segment", ".data.rel.ro"},
                            {"va", hexAddress(DataBase)},
                            {"size", DataSlots * 8},
                            {"file_off", 0x4000},
                            {"file_sz", DataSlots * 8},
                            {"alignment", 8},
                            {"flags", "R--"}},
                           {{"name", ".rodata"},
                            {"segment", ".rodata"},
                            {"va", hexAddress(RodataBase)},
                            {"size", RodataSize},
                            {"file_off", 0x4100},
                            {"file_sz", RodataSize},
                            {"alignment", 16},
                            {"flags", "R--"}},
                           {{"name", ".bss"},
                            {"segment", ".bss"},
                            {"va", hexAddress(BssBase)},
                            {"size", BssSize},
                            {"file_off", 0x4120},
                            {"file_sz", 0},
                            {"alignment", 32},
                            {"flags", "RW-"}}})
                  .dump());
}
const char *neverd_symbols_json(neverd_session_t s) {
  Json rows = Json::array(
      {{{"addr", hexAddress(Base)},
        {"name", "function_0"},
        {"type", "function"}},
       {{"addr", hexAddress(BssBase)}, {"name", "stderr"}, {"size", 8}}});
  // The user's names of data replace the symbols' or add rows of their own.
  for (const auto &[address, name] : session(s)->names) {
    if (neverd_func_find_by_addr(s, address) >= 0)
      continue;
    bool replaced = false;
    for (auto &row : rows)
      if (parseAddress(row.at("addr").get<std::string>()) == address) {
        row["name"] = name;
        replaced = true;
      }
    if (!replaced)
      rows.push_back(
          {{"addr", hexAddress(address)}, {"name", name}, {"size", 0}});
  }
  return copy(rows.dump());
}
const char *neverd_imports_json(neverd_session_t s) {
  if (session(s)->path.ends_with("pseudocode-import.bin"))
    return copy(Json::array({{{"name", "function_22"},
                              {"module", "fixture"},
                              {"iat_addr", hexAddress(Base + 0x160)}}})
                    .dump());
  return copy(Json::array().dump());
}
const char *neverd_exports_json(neverd_session_t) {
  return copy(Json::array({{{"addr", hexAddress(Base)},
                            {"name", "function_0"},
                            {"ordinal", 1}}})
                  .dump());
}
const char *neverd_entrypoints_json(neverd_session_t) {
  return copy(
      Json::array(
          {{{"addr", hexAddress(Base)}, {"name", "start"}, {"type", "entry"}}})
          .dump());
}
// The fixture's "library" match names the second function.
const char *neverd_sig_matches_json(neverd_session_t) {
  return copy(Json::array({{{"addr", hexAddress(Base + 16)},
                            {"name", "function_1"},
                            {"source", "mock"}}})
                  .dump());
}
void neverd_session_restrict_function(neverd_session_t, neverd_va_t) {}
const char *neverd_search_bytes(neverd_session_t, const unsigned char *pattern,
                                int length, int limit) {
  Json hits = Json::array();
  if (pattern && length > 0 && length <= 16 && pattern[0] + length <= 16) {
    bool ramp = true;
    for (int i = 1; i < length; ++i)
      ramp = ramp && pattern[i] == pattern[0] + i;
    for (int i = 0; ramp && i < 600 && static_cast<int>(hits.size()) < limit;
         ++i)
      hits.push_back({{"addr", hexAddress(Base + 16 * i + pattern[0])}});
  }
  return copy(hits.dump());
}
const char *neverd_search_string(neverd_session_t, const char *pattern,
                                 int caseSensitive, int limit) {
  Json hits = Json::array();
  const auto fold = [caseSensitive](std::string text) {
    if (!caseSensitive)
      for (auto &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
  };
  const auto needle = fold(pattern ? pattern : "");
  for (int i = 0;
       !needle.empty() && i < 600 && static_cast<int>(hits.size()) < limit;
       ++i) {
    const auto value = "string " + std::to_string(i);
    if (fold(value).find(needle) != std::string::npos)
      hits.push_back({{"addr", hexAddress(Base + i)}, {"value", value}});
  }
  return copy(hits.dump());
}
const char *neverd_xrefs_to_json(neverd_session_t s, neverd_va_t address) {
  session(s)->error.clear();
  return copy(Json::array({{{"from", hexAddress(address + 8)},
                            {"func", "function_0"},
                            {"block", 0}}})
                  .dump());
}
const char *neverd_xrefs_from_json(neverd_session_t s, neverd_va_t address) {
  session(s)->error.clear();
  return copy(Json::array({{{"to", hexAddress(address + 8)},
                            {"func", "function_0"},
                            {"opcode", "COPY"}}})
                  .dump());
}
const char *neverd_cfg_json(neverd_session_t s, neverd_va_t address) {
  session(s)->error.clear();
  if (session(s)->sourceCacheFixture() &&
      !session(s)->wholeProgramPrepared &&
      session(s)->preparedEntry != address) {
    session(s)->error = "fixture graph read used another function's pipeline";
    return nullptr;
  }
  Json nodes = Json::array(), edges = Json::array();
  const int count = address == Base + 2   ? 10000
                    : address == Base + 1 ? 501
                    : session(s)->path.ends_with("code-edits-mapped.bin") ? 16
                                                                          : 2;
  for (int i = 0; i < count; ++i) {
    nodes.push_back({{"id", i},
                     {"start", hexAddress(address + i)},
                     {"end", hexAddress(address + i + 1)},
                     {"insn_count", 1},
                     {"disasm", {"nop"}}});
    if (i)
      edges.push_back({{"from", i - 1}, {"to", i}, {"type", "unconditional"}});
  }
  return copy(Json{{"nodes", nodes}, {"edges", edges}}.dump());
}
void neverd_annotation_set(neverd_session_t s, neverd_va_t address,
                           const char *text) {
  if (!text || !*text)
    session(s)->annotations.erase(address);
  else
    session(s)->annotations[address] = text;
}
const char *neverd_annotation_get(neverd_session_t s, neverd_va_t address) {
  auto it = session(s)->annotations.find(address);
  return it == session(s)->annotations.end() ? nullptr : copy(it->second);
}
const char *neverd_annotations_json(neverd_session_t s) {
  Json result = Json::array();
  for (const auto &[address, text] : session(s)->annotations)
    result.push_back({{"addr", hexAddress(address)}, {"text", text}});
  return copy(result.dump());
}
int neverd_annotations_load(neverd_session_t s) {
  try {
    auto values = read(session(s)->path + ".neverd-annotations.json");
    session(s)->annotations.clear();
    for (const auto &item : values)
      session(s)
          ->annotations[parseAddress(item.at("addr").get<std::string>())] =
          item.at("text");
    return 0;
  } catch (...) {
    return 1;
  }
}
const char *neverd_renames_json(neverd_session_t s) {
  Json result = Json::array();
  for (const auto &[address, name] : session(s)->names)
    result.push_back(
        {{"addr", hexAddress(address)},
         {"renamed", name},
         {"original",
          address == Base + 0x150 &&
                  session(s)->path.ends_with("pseudocode-navigation.bin")
              ? "_ZN3BarC1Ev"
              : "function_" + std::to_string((address - Base) / 16)}});
  return copy(result.dump());
}
int neverd_renames_load(neverd_session_t s) {
  try {
    auto values = read(session(s)->path + ".neverd-renames.json");
    session(s)->names.clear();
    for (const auto &item : values)
      session(s)->names[parseAddress(item.at("addr").get<std::string>())] =
          item.at("renamed");
    return 0;
  } catch (...) {
    return 1;
  }
}
// Function edits as the engine keeps them: the last edit at an address
// decides.
int neverd_func_create(neverd_session_t s, neverd_va_t address) {
  auto &state = *session(s);
  if (address < Base || address - Base >= codeSize(s)) {
    state.error = hexAddress(address) + " is not in executable code";
    return -1;
  }
  if (neverd_func_find_by_addr(s, address) >= 0) {
    state.error = "a function already starts at " + hexAddress(address);
    return -1;
  }
  state.functionEdits[address] = true;
  state.functions = listedFunctions(state.functionEdits, state.imageFunctions);
  return 0;
}
int neverd_func_delete(neverd_session_t s, neverd_va_t address) {
  auto &state = *session(s);
  if (neverd_func_find_by_addr(s, address) < 0) {
    state.error = "no function starts at " + hexAddress(address);
    return -1;
  }
  state.functionEdits[address] = false;
  state.functions = listedFunctions(state.functionEdits, state.imageFunctions);
  return 0;
}
const char *neverd_functions_json(neverd_session_t s) {
  Json result = Json::array();
  for (const auto &[address, created] : session(s)->functionEdits)
    result.push_back({{"addr", hexAddress(address)},
                      {"state", created ? "created" : "deleted"}});
  return copy(result.dump());
}
int neverd_functions_load(neverd_session_t s) {
  try {
    std::map<std::uint64_t, bool> edits;
    for (const auto &item : read(session(s)->path + ".neverd-functions.json"))
      edits[parseAddress(item.at("addr").get<std::string>())] =
          item.at("state").get<std::string>() == "created";
    session(s)->functionEdits = std::move(edits);
    session(s)->functions =
        listedFunctions(session(s)->functionEdits, session(s)->imageFunctions);
    return 0;
  } catch (...) {
    return 1;
  }
}
// Data items as the engine keeps them: values, strings and undefined runs
// that share no byte.
int neverd_item_set(neverd_session_t s, neverd_va_t address, const char *text) {
  auto &state = *session(s);
  try {
    const auto row = Json::parse(text);
    const auto kind = row.at("kind").get<std::string>();
    std::uint64_t size = kind == "byte"    ? 1
                         : kind == "word"  ? 2
                         : kind == "dword" ? 4
                         : kind == "qword" ? 8
                                           : 0;
    if (kind == "code") {
      const auto instructions = fixtureInstructions(address, 1, false, false);
      if (instructions.empty() ||
          (row.contains("size") &&
           row.at("size") != instructions.front().at("size"))) {
        state.error = "invalid code definition";
        return -1;
      }
      size = instructions.front().at("size").get<std::uint64_t>();
    }
    if (!size && kind != "string" && kind != "undefined") {
      state.error = "unknown data item kind " + kind;
      return -1;
    }
    if (!size)
      size = row.at("size").get<std::uint64_t>();
    // Undefined bytes give way and stay undefined around the item.
    std::map<std::uint64_t, Json> items;
    for (const auto &[at, item] : state.items) {
      const auto end = at + item.at("size").get<std::uint64_t>();
      if (end <= address || at >= address + size) {
        items[at] = item;
        continue;
      }
      if (item.at("kind") != "undefined") {
        if (at == address)
          continue;
        state.error = "the item shares bytes with the one at " + hexAddress(at);
        return -1;
      }
      if (at < address)
        items[at] = {{"addr", hexAddress(at)},
                     {"kind", "undefined"},
                     {"size", address - at}};
      if (end > address + size)
        items[address + size] = {{"addr", hexAddress(address + size)},
                                 {"kind", "undefined"},
                                 {"size", end - address - size}};
    }
    state.items = std::move(items);
    Json stored{{"addr", hexAddress(address)}, {"kind", kind}, {"size", size}};
    if (row.contains("encoding"))
      stored["encoding"] = row["encoding"];
    state.items[address] = std::move(stored);
    return 0;
  } catch (const Json::exception &) {
    state.error = "invalid data item";
    return -1;
  }
}
int neverd_item_clear(neverd_session_t s, neverd_va_t address) {
  if (!session(s)->items.erase(address)) {
    session(s)->error = "no data item starts at " + hexAddress(address);
    return -1;
  }
  return 0;
}
const char *neverd_items_json(neverd_session_t s) {
  Json result = Json::array();
  for (const auto &[address, item] : session(s)->items)
    result.push_back(item);
  return copy(result.dump());
}
int neverd_items_load(neverd_session_t s) {
  try {
    std::map<std::uint64_t, Json> items;
    for (const auto &item : read(session(s)->path + ".neverd-items.json"))
      items[parseAddress(item.at("addr").get<std::string>())] = item;
    session(s)->items = std::move(items);
    return 0;
  } catch (...) {
    return 1;
  }
}
// Operand formats as the engine keeps them.
int neverd_operand_format_set(neverd_session_t s, neverd_va_t address,
                              int operand, const char *text) {
  auto &state = *session(s);
  if (operand < 0 || operand > 7) {
    state.error = "an operand index is 0 to 7";
    return -1;
  }
  auto &operands = state.operandFormats[address];
  try {
    const auto format = text ? Json::parse(text) : Json(nullptr);
    const auto base = format.is_null() ? std::string("number")
                                       : format.at("base").get<std::string>();
    const bool negate = format.is_object() && format.value("negate", false);
    const bool invert = format.is_object() && format.value("invert", false);
    if (base == "number" && !negate && !invert)
      operands.erase(operand);
    else
      operands[operand] = {{"operand", operand},
                           {"base", base},
                           {"negate", negate},
                           {"invert", invert}};
  } catch (const Json::exception &) {
    state.error = "invalid operand format";
    return -1;
  }
  if (operands.empty())
    state.operandFormats.erase(address);
  return 0;
}
const char *neverd_operand_formats_json(neverd_session_t s) {
  Json rows = Json::array();
  for (const auto &[address, operands] : session(s)->operandFormats) {
    Json list = Json::array();
    for (const auto &[index, format] : operands)
      list.push_back(format);
    rows.push_back({{"addr", hexAddress(address)}, {"operands", list}});
  }
  return copy(rows.dump());
}
int neverd_operand_formats_load(neverd_session_t s) {
  try {
    std::map<std::uint64_t, std::map<int, Json>> formats;
    for (const auto &row : read(session(s)->path + ".neverd-operands.json"))
      for (const auto &format : row.at("operands"))
        formats[parseAddress(row.at("addr").get<std::string>())]
               [format.at("operand").get<int>()] = format;
    session(s)->operandFormats = std::move(formats);
    return 0;
  } catch (...) {
    return 1;
  }
}
// The strings neverd_strings_ex_json lists, read from their first byte.
const char *neverd_string_at(neverd_session_t s, neverd_va_t address,
                             const char *) {
  if (address == RodataBase)
    return copy(Json{{"addr", hexAddress(address)},
                     {"length", 6},
                     {"chars", 2},
                     {"encoding", "utf-8"},
                     {"value", "\xe4\xb8\xad\xe6\x96\x87"},
                     {"unit", 1}}
                    .dump());
  if (address == RodataBase + 8)
    return copy(Json{{"addr", hexAddress(address)},
                     {"length", 8},
                     {"chars", 4},
                     {"encoding", "utf-16le"},
                     {"value", "Wide"},
                     {"unit", 2}}
                    .dump());
  session(s)->error = "no string starts at " + hexAddress(address);
  return nullptr;
}
}
