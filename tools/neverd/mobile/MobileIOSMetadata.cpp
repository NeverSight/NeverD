#include "MobileIOSInternal.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/MachO.h"

#include <algorithm>
#include <limits>

namespace neverd::mobile::ios {
const Object &object(const Value &v, std::string_view what) {
  if (auto *o = v.getAsObject())
    return *o;
  throw Error(std::string(what) + " must be an object");
}
const Array &array(const Object &o, llvm::StringRef key) {
  if (auto *a = o.getArray(key))
    return *a;
  throw Error("invalid iOS report array: " + key.str());
}
std::string requiredString(const Object &o, llvm::StringRef key) {
  if (auto s = o.getString(key))
    return s->str();
  throw Error("invalid iOS report string: " + key.str());
}
bool identifier(std::string_view s) {
  auto start = [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };
  if (s.empty() || !start(s[0]))
    return false;
  for (unsigned char c : s)
    if (!start(c) && !(c >= '0' && c <= '9'))
      return false;
  return true;
}
bool hexAddress(std::string_view s) {
  return s.starts_with("0x") && s.size() > 2 && s.size() <= 18 &&
         std::all_of(s.begin() + 2, s.end(),
                     [](unsigned char c) { return llvm::isHexDigit(c); });
}
std::string hex(uint64_t v) { return "0x" + llvm::utohexstr(v, true); }
void appendUnique(Array &to, const Array &from) {
  for (const auto &v : from) {
    if (!v.getAsString())
      throw Error("invalid iOS limitation text");
    if (std::find(to.begin(), to.end(), v) == to.end())
      to.push_back(v);
  }
}
namespace {
std::string presentation(llvm::StringRef value) {
  return llvm::json::isUTF8(value) ? value.str() : llvm::json::fixUTF8(value);
}
struct Bytes {
  std::string_view d;
  void range(uint64_t p, uint64_t n) const {
    if (p > d.size() || n > d.size() - p)
      throw Error("Mach-O or plist range extends outside the file");
  }
  uint64_t get(uint64_t p, unsigned n, bool little = true) const {
    range(p, n);
    if (!n || n > 8)
      throw Error("invalid bounded integer width");
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
      v |= uint64_t(uint8_t(d[p + i])) << (8 * (little ? i : n - i - 1));
    return v;
  }
};
std::string_view symbolName(const Bytes &bytes, uint64_t strings,
                            uint64_t string_size, uint64_t index,
                            Budget &budget) {
  if (index >= string_size)
    throw Error("Mach-O symbol string index is out of bounds");
  auto name = bytes.d.substr(strings + index,
                             std::min<uint64_t>(16384, string_size - index));
  auto end = name.find('\0');
  // Account for repeated references to long strings without allocating copies.
  // Each search is bounded even when the name is malformed.
  auto scanned = end == name.npos ? name.size() : end + 1;
  budget.tick((scanned + 63) / 64);
  if (end == name.npos)
    throw Error("unterminated or oversized Mach-O symbol name");
  return name.substr(0, end);
}
std::string arch(uint32_t cpu) {
  switch (cpu) {
  case 0x100000c:
    return "arm64";
  case 12:
    return "arm";
  case 0x1000007:
    return "x86_64";
  case 7:
    return "i386";
  default:
    return {};
  }
}
void accountBuildTarget(size_t tools, uint64_t &bytes, const Budget &budget) {
  // Bound both the retained records and their JSON representation before
  // allocation. This is construction accounting, not published output bytes.
  constexpr uint64_t record_bytes = 1024, tool_bytes = 256;
  if (bytes > budget.limits.max_bytes ||
      record_bytes > budget.limits.max_bytes - bytes ||
      tools > (budget.limits.max_bytes - bytes - record_bytes) / tool_bytes)
    throw Error(
        "Mach-O build-target metadata construction exceeds byte budget");
  bytes += record_bytes + uint64_t(tools) * tool_bytes;
}
std::string_view buildPlatform(uint32_t platform) {
  switch (platform) {
#define PLATFORM(symbol, id, name, build_name, target, tapi_target, marketing) \
  case id:                                                                     \
    return #tapi_target;
#include "llvm/BinaryFormat/MachO.def"
  default:
    return "unknown";
  }
}
std::string_view buildCommand(uint32_t command) {
  switch (command) {
  case llvm::MachO::LC_BUILD_VERSION:
    return "LC_BUILD_VERSION";
  case llvm::MachO::LC_VERSION_MIN_MACOSX:
    return "LC_VERSION_MIN_MACOSX";
  case llvm::MachO::LC_VERSION_MIN_IPHONEOS:
    return "LC_VERSION_MIN_IPHONEOS";
  case llvm::MachO::LC_VERSION_MIN_TVOS:
    return "LC_VERSION_MIN_TVOS";
  case llvm::MachO::LC_VERSION_MIN_WATCHOS:
    return "LC_VERSION_MIN_WATCHOS";
  default:
    return "unknown";
  }
}
std::string_view buildTool(uint32_t tool) {
  switch (tool) {
  case llvm::MachO::TOOL_CLANG:
    return "clang";
  case llvm::MachO::TOOL_SWIFT:
    return "swift";
  case llvm::MachO::TOOL_LD:
    return "ld";
  case llvm::MachO::TOOL_LLD:
    return "lld";
  default:
    return "unknown";
  }
}
std::string buildVersion(uint32_t version) {
  return std::to_string(version >> 16) + "." +
         std::to_string((version >> 8) & 255) + "." +
         std::to_string(version & 255);
}
Selection thin(std::string_view bytes, Budget &budget) {
  Bytes b{bytes};
  b.range(0, 28);
  auto magic = b.get(0, 4);
  if (magic != 0xfeedface && magic != 0xfeedfacf)
    throw Error("expected a little-endian Mach-O executable");
  bool wide = magic == 0xfeedfacf;
  uint64_t header = wide ? 32 : 28;
  b.range(0, header);
  Selection s;
  s.cpu_type = b.get(4, 4);
  s.cpu_subtype = b.get(8, 4);
  s.pointer_size = wide ? 8 : 4;
  s.architecture = arch(s.cpu_type);
  if (s.architecture.empty() || bool(s.cpu_type & 0x1000000) != wide)
    throw Error("unsupported Mach-O CPU type");
  auto filetype = b.get(12, 4), n = b.get(16, 4), size = b.get(20, 4);
  if (n > 100000 || n > size / 8)
    throw Error("invalid Mach-O load-command count");
  b.range(header, size);
  uint64_t p = header, end = header + size, total_sections = 0,
           target_metadata_bytes = 0;
  bool symtab = false, chained = false;
  std::vector<std::pair<uint64_t, uint64_t>> mappings;
  for (uint64_t i = 0; i < n; ++i) {
    budget.tick();
    b.range(p, 8);
    auto cmd = b.get(p, 4), len = b.get(p + 4, 4);
    if (len < 8 || len % 4 || len > end - p)
      throw Error("invalid Mach-O load-command size");
    if (cmd == 1 || cmd == 0x19) {
      if ((cmd == 0x19) != wide)
        throw Error("Mach-O segment width disagrees with its header");
      uint64_t base = wide ? 72 : 56, secsize = wide ? 80 : 68;
      if (len < base)
        throw Error("truncated Mach-O segment command");
      auto va = b.get(p + 24, s.pointer_size),
           vs = b.get(p + 24 + s.pointer_size, s.pointer_size),
           off = b.get(p + 24 + 2 * s.pointer_size, s.pointer_size),
           fsz = b.get(p + 24 + 3 * s.pointer_size, s.pointer_size);
      auto count = b.get(p + (wide ? 64 : 48), 4);
      if (count > 100000 - total_sections || count > (len - base) / secsize)
        throw Error("invalid Mach-O section count");
      total_sections += count;
      b.range(off, fsz);
      if (fsz > vs || vs > std::numeric_limits<uint64_t>::max() - va ||
          (!wide && va + vs > (uint64_t(1) << 32)))
        throw Error("invalid Mach-O segment range");
      if (vs)
        mappings.emplace_back(va, va + vs);
      for (uint64_t j = 0; j < count; ++j) {
        budget.tick();
        auto q = p + base + j * secsize;
        auto a = b.get(q + 32, s.pointer_size),
             z = b.get(q + 32 + s.pointer_size, s.pointer_size),
             o = b.get(q + 32 + 2 * s.pointer_size, 4),
             flags = b.get(q + (wide ? 64 : 56), 4) & 255;
        if (a < va || a - va > vs || z > vs - (a - va))
          throw Error("Mach-O section lies outside its segment");
        if (flags != 1 && flags != 12 && flags != 18) {
          b.range(o, z);
          if (o < off || o - off > fsz || z > fsz - (o - off))
            throw Error("Mach-O section file range lies outside its segment");
          if (filetype != 1 && z && o - off != a - va)
            throw Error("Mach-O section file and virtual mappings disagree");
        }
      }
    } else if (cmd == 0x21 || cmd == 0x2c) {
      if (len < (cmd == 0x2c ? 24 : 20))
        throw Error("truncated Mach-O encryption command");
      b.range(b.get(p + 8, 4), b.get(p + 12, 4));
      s.encrypted |= b.get(p + 16, 4) != 0;
    } else if (cmd == 2) {
      if (len < 24 || symtab)
        throw Error("invalid Mach-O symbol-table command");
      symtab = true;
      auto count = b.get(p + 12, 4);
      // nsyms is a 32-bit field. Validate the complete table before iteration;
      // file size and the shared work budget bound large, legitimate tables.
      b.range(b.get(p + 8, 4), count * (wide ? 16 : 12));
      b.range(b.get(p + 16, 4), b.get(p + 20, 4));
      auto symbols = b.get(p + 8, 4), strings = b.get(p + 16, 4),
           string_size = b.get(p + 20, 4);
      for (uint64_t i = 0; i < count; ++i) {
        budget.tick();
        auto entry = symbols + i * (wide ? 16 : 12);
        auto index = b.get(entry, 4), type = b.get(entry + 4, 1);
        if (!index || (type & 0xe0))
          continue;
        symbolName(b, strings, string_size, index, budget);
      }
    } else if (cmd == llvm::MachO::LC_BUILD_VERSION) {
      // Apple's mach-o/loader.h defines a 24-byte command followed by exactly
      // ntools 8-byte build_tool_version entries. Never read into the next LC.
      if (len < 24)
        throw Error("truncated Mach-O build-version command");
      const auto count = b.get(p + 20, 4);
      if ((len - 24) % 8 || count != (len - 24) / 8)
        throw Error(
            "Mach-O build-version tool count disagrees with command size");
      accountBuildTarget(count, target_metadata_bytes, budget);
      BuildTarget target;
      target.command = cmd;
      target.load_command_index = i;
      target.platform = b.get(p + 8, 4);
      target.minos = b.get(p + 12, 4);
      target.sdk = b.get(p + 16, 4);
      for (uint64_t j = 0; j < count; ++j) {
        budget.tick();
        const auto entry = p + 24 + j * 8;
        target.tools.push_back(
            {uint32_t(b.get(entry, 4)), uint32_t(b.get(entry + 4, 4))});
      }
      s.build_targets.push_back(std::move(target));
    } else if (cmd == llvm::MachO::LC_VERSION_MIN_MACOSX ||
               cmd == llvm::MachO::LC_VERSION_MIN_IPHONEOS ||
               cmd == llvm::MachO::LC_VERSION_MIN_TVOS ||
               cmd == llvm::MachO::LC_VERSION_MIN_WATCHOS) {
      if (len != 16)
        throw Error("invalid Mach-O legacy minimum-version command size");
      accountBuildTarget(0, target_metadata_bytes, budget);
      BuildTarget target;
      target.command = cmd;
      target.load_command_index = i;
      target.minos = b.get(p + 8, 4);
      target.sdk = b.get(p + 12, 4);
      s.build_targets.push_back(std::move(target));
    } else if (cmd == 0x80000034) {
      if (len < 16 || chained)
        throw Error("invalid or duplicate Mach-O chained-fixup command");
      chained = true;
      b.range(b.get(p + 8, 4), b.get(p + 12, 4));
    }
    p += len;
  }
  if (p != end)
    throw Error("Mach-O load commands do not fill their declared region");
  std::sort(mappings.begin(), mappings.end());
  for (size_t i = 1; i < mappings.size(); ++i)
    if (mappings[i - 1].second > mappings[i].first)
      throw Error("overlapping Mach-O virtual segments");
  s.bytes = std::string(bytes);
  return s;
}
Object methodMetadata(const ObjCMethod &m) {
  return Object{{"selector", presentation(m.Selector)},
                {"type_encoding", presentation(m.TypeEncoding)},
                {"implementation", hex(m.Implementation)},
                {"class_method", m.IsClassMethod},
                {"category_name", presentation(m.CategoryName)},
                {"category_address", hex(m.CategoryAddress)}};
}
} // namespace
Selection selectSlice(std::string_view bytes, std::string_view architecture,
                      Budget &budget) {
  budget.check();
  if (bytes.size() > budget.limits.max_bytes)
    throw Error("Mach-O input exceeds its byte budget");
  static const std::vector<std::string> preference = {"arm64", "arm", "x86_64",
                                                      "i386"};
  if (architecture != "auto" && std::find(preference.begin(), preference.end(),
                                          architecture) == preference.end())
    throw Error("unsupported iOS architecture");
  Bytes b{bytes};
  auto magic = b.get(0, 4, false);
  if (magic != 0xcafebabe && magic != 0xbebafeca && magic != 0xcafebabf &&
      magic != 0xbfbafeca) {
    auto s = thin(bytes, budget);
    if (architecture != "auto" && s.architecture != architecture)
      throw Error("requested iOS architecture is not present");
    s.available = {s.architecture};
    return s;
  }
  bool little = magic == 0xbebafeca || magic == 0xbfbafeca,
       wide = magic == 0xcafebabf || magic == 0xbfbafeca;
  auto count = b.get(4, 4, little);
  auto entry = wide ? 32u : 20u;
  if (!count || count > 64)
    throw Error("invalid Mach-O universal slice count");
  b.range(8, count * entry);
  std::vector<Selection> slices;
  std::vector<std::pair<uint64_t, uint64_t>> ranges;
  std::set<std::pair<uint32_t, uint32_t>> cpus;
  for (uint64_t i = 0; i < count; ++i) {
    budget.tick();
    auto p = 8 + i * entry;
    auto cpu = b.get(p, 4, little), sub = b.get(p + 4, 4, little),
         off = b.get(p + 8, wide ? 8 : 4, little),
         size = b.get(p + (wide ? 16 : 12), wide ? 8 : 4, little),
         align = b.get(p + (wide ? 24 : 16), 4, little);
    if (!size || off < 8 + count * entry || align > 30 ||
        off % (uint64_t(1) << align))
      throw Error("invalid Mach-O universal slice range or alignment");
    b.range(off, size);
    ranges.emplace_back(off, off + size);
    if (!cpus.emplace(cpu, sub).second)
      throw Error("duplicate Mach-O universal architecture");
    auto s = thin(bytes.substr(off, size), budget);
    if (s.cpu_type != cpu || s.cpu_subtype != sub)
      throw Error("Mach-O universal CPU record disagrees with its slice");
    slices.push_back(std::move(s));
  }
  std::sort(ranges.begin(), ranges.end());
  for (size_t i = 1; i < ranges.size(); ++i)
    if (ranges[i - 1].second > ranges[i].first)
      throw Error("overlapping Mach-O universal slices");
  std::vector<std::string> available;
  for (const auto &s : slices)
    if (std::find(available.begin(), available.end(), s.architecture) ==
        available.end())
      available.push_back(s.architecture);
  std::string selected(architecture);
  if (selected == "auto")
    for (const auto &a : preference)
      if (std::find(available.begin(), available.end(), a) != available.end()) {
        selected = a;
        break;
      }
  std::vector<Selection *> matches;
  for (auto &s : slices)
    if (s.architecture == selected)
      matches.push_back(&s);
  if (matches.empty())
    throw Error("requested iOS architecture is not present");
  // Preserve the generic subtype preference independently of table order.
  std::sort(matches.begin(), matches.end(), [](const auto *a, const auto *b) {
    return std::pair(a->cpu_subtype & 0x00ffffff, a->cpu_subtype) <
           std::pair(b->cpu_subtype & 0x00ffffff, b->cpu_subtype);
  });
  auto result = std::move(*matches[0]);
  result.available = std::move(available);
  return result;
}
Object buildTargetMetadata(const Selection &selection, Budget &budget) {
  budget.check();
  Array commands;
  uint64_t metadata_bytes = 0;
  for (const auto &target : selection.build_targets) {
    budget.tick();
    accountBuildTarget(target.tools.size(), metadata_bytes, budget);
    const bool modern = target.command == llvm::MachO::LC_BUILD_VERSION;
    Array tools;
    for (const auto &tool : target.tools) {
      budget.tick();
      tools.push_back(Object{{"tool_id", tool.tool},
                             {"tool", std::string(buildTool(tool.tool))},
                             {"version_raw", tool.version},
                             {"version", buildVersion(tool.version)}});
    }
    commands.push_back(Object{
        {"command_id", target.command},
        {"command", std::string(buildCommand(target.command))},
        {"load_command_index", target.load_command_index},
        {"platform_id", modern ? Value(target.platform) : Value(nullptr)},
        {"platform",
         modern ? std::string(buildPlatform(target.platform)) : "unknown"},
        {"minos_raw", target.minos},
        {"minos", buildVersion(target.minos)},
        {"sdk_raw", target.sdk},
        {"sdk", buildVersion(target.sdk)},
        {"tools", std::move(tools)}});
  }
  Object report{{"status", "unknown"},    {"platform", "unknown"},
                {"platform_id", nullptr}, {"minos_raw", nullptr},
                {"minos", nullptr},       {"sdk_raw", nullptr},
                {"sdk", nullptr},         {"commands", std::move(commands)}};
  if (selection.build_targets.empty()) {
    report["reason"] = "no explicit Mach-O build-target command";
    return report;
  }
  // Multiple LC_BUILD_VERSION records are valid for zippered dylibs. Retain
  // every command, including identical duplicates, without choosing a target
  // from command order, CPU architecture, bundle suffix, or the host platform.
  if (selection.build_targets.size() != 1) {
    report["reason"] =
        "multiple Mach-O build-target commands; target selection "
        "is ambiguous";
    return report;
  }
  const auto &target = selection.build_targets.front();
  report["minos_raw"] = target.minos;
  report["minos"] = buildVersion(target.minos);
  report["sdk_raw"] = target.sdk;
  report["sdk"] = buildVersion(target.sdk);
  if (target.command != llvm::MachO::LC_BUILD_VERSION) {
    report["reason"] = "legacy minimum-version command retained without "
                       "inferring a device or simulator build target";
    return report;
  }
  const auto platform = buildPlatform(target.platform);
  report["platform_id"] = target.platform;
  report["platform"] = std::string(platform);
  if (platform == "unknown")
    report["reason"] = "unrecognized LC_BUILD_VERSION platform value";
  else if (!target.minos || !target.sdk)
    report["reason"] = "LC_BUILD_VERSION has an unspecified minimum OS or SDK "
                       "version";
  else if (target.minos > target.sdk)
    report["reason"] = "LC_BUILD_VERSION minimum OS exceeds its SDK version";
  else
    report["status"] = "known";
  return report;
}
Object objcMetadata(const BinaryImage &image) {
  Array classes, categories, limitations;
  std::map<uint64_t, size_t> category_indices;
  std::map<uint64_t, Array> methods_by_class;
  for (const auto &method : image.ObjCMethods)
    methods_by_class[method.ClassAddress].push_back(methodMetadata(method));
  for (const auto &c : image.ObjCClasses) {
    Array methods = methods_by_class[c.Address], ivars;
    for (const auto &i : c.Ivars)
      ivars.push_back(Object{{"name", presentation(i.Name)},
                             {"type_encoding", presentation(i.TypeEncoding)},
                             {"offset", i.Offset},
                             {"size", i.Size},
                             {"alignment", i.Alignment}});
    classes.push_back(
        Object{{"name", presentation(c.Name)},
               {"address", hex(c.Address)},
               {"superclass_address", hex(c.SuperclassAddress)},
               {"superclass", c.SuperclassName.empty()
                                  ? Value(nullptr)
                                  : Value(presentation(c.SuperclassName))},
               {"root_class", c.RootClass},
               {"instance_start", c.InstanceStart},
               {"instance_size", c.InstanceSize},
               {"ivar_status", presentation(c.IvarStatus)},
               {"ivars", std::move(ivars)},
               {"inheritance_status", presentation(c.InheritanceStatus)},
               {"methods", std::move(methods)}});
  }
  for (const auto &m : image.ObjCMethods)
    if (m.CategoryAddress && !m.CategoryName.empty()) {
      auto [it, inserted] =
          category_indices.emplace(m.CategoryAddress, categories.size());
      if (inserted)
        categories.push_back(Object{{"name", presentation(m.CategoryName)},
                                    {"class_name", presentation(m.ClassName)},
                                    {"address", hex(m.CategoryAddress)},
                                    {"methods", Array{}}});
      categories[it->second].getAsObject()->getArray("methods")->push_back(
          methodMetadata(m));
    }
  limitations.push_back("Runtime metadata does not recover properties, "
                        "protocols, or dynamically registered classes.");
  for (const auto &d : image.ObjCMetadataDiagnostics)
    limitations.push_back(presentation(d));
  std::string status = !image.ObjCMetadataDiagnostics.empty() ? "partial"
                       : classes.empty()                      ? "section-absent"
                                                              : "recovered";
  return Object{{"status", status},
                {"classes", std::move(classes)},
                {"categories", std::move(categories)},
                {"limitations", std::move(limitations)}};
}
Object swiftMetadata(const BinaryImage &image, Budget &budget) {
  Array symbols, types;
  uint64_t metadata_bytes = 0;
  Array limitations{
      "Swift symbol names are preserved in their mangled form.",
      "Only nominal type names are recovered; original Swift source and method "
      "bodies cannot be reconstructed from metadata alone."};
  auto add_symbol = [&](std::string_view raw_name, uint64_t address,
                        bool defined) {
    auto name = llvm::StringRef(raw_name.data(), raw_name.size()).ltrim('_');
    if (name.starts_with("$s") || name.starts_with("$S") ||
        name.starts_with("T0")) {
      if (raw_name.size() >= 16384)
        throw Error("oversized Swift symbol name");
      // Charge before constructing strings or JSON nodes. Six bytes per input
      // byte covers JSON escaping and UTF-8 replacement; fixed overhead covers
      // this record's keys, address, boolean, punctuation and indentation.
      // This bounds construction independently of the actual publication bytes
      // that publishJSON charges later. Repeated names each consume a record.
      budget.check();
      const uint64_t record_bytes = raw_name.size() * 6 + 256;
      if (record_bytes > budget.limits.max_bytes - metadata_bytes)
        throw Error("metadata construction exceeds byte budget");
      metadata_bytes += record_bytes;
      symbols.push_back(Object{{"name", presentation(raw_name)},
                               {"address", hex(address)},
                               {"defined", defined}});
    }
  };
  if (!image.Raw.empty()) {
    budget.check();
    if (image.Raw.size() > budget.limits.max_bytes)
      throw Error("Mach-O input exceeds its byte budget");
    // The loader's function-oriented Symbol view deliberately omits undefined
    // zero-address entries. Coverage describes the complete nlist inventory,
    // including imported metadata witnesses.
    Bytes raw{std::string_view(reinterpret_cast<const char *>(image.Raw.data()),
                               image.Raw.size())};
    auto magic = raw.get(0, 4);
    if (magic != 0xfeedfacf && magic != 0xfeedface)
      throw Error(
          "Swift symbol inventory requires a little-endian thin Mach-O");
    const bool wide = magic == 0xfeedfacf;
    uint64_t cursor = wide ? 32 : 28;
    auto command_count = raw.get(16, 4), command_bytes = raw.get(20, 4);
    raw.range(cursor, command_bytes);
    if (command_count > 100000 || command_count > command_bytes / 8)
      throw Error("invalid Swift symbol load-command inventory");
    auto command_end = cursor + command_bytes;
    bool found = false;
    for (uint64_t i = 0; i < command_count; ++i) {
      budget.tick();
      auto command = raw.get(cursor, 4), size = raw.get(cursor + 4, 4);
      if (size < 8 || size > command_end - cursor)
        throw Error("invalid Swift symbol load-command size");
      if (command == 2) {
        if (found || size < 24)
          throw Error("invalid Swift symbol-table command");
        found = true;
        auto offset = raw.get(cursor + 8, 4), count = raw.get(cursor + 12, 4),
             strings = raw.get(cursor + 16, 4), bytes = raw.get(cursor + 20, 4);
        raw.range(offset, count * (wide ? 16 : 12));
        raw.range(strings, bytes);
        for (uint64_t j = 0; j < count; ++j) {
          budget.tick();
          auto entry = offset + j * (wide ? 16 : 12);
          auto index = raw.get(entry, 4), type = raw.get(entry + 4, 1);
          if (!index || (type & 0xe0))
            continue;
          auto name = symbolName(raw, strings, bytes, index, budget);
          add_symbol(name, raw.get(entry + 8, wide ? 8 : 4),
                     (type & 0x0e) != 0);
        }
      }
      cursor += size;
    }
    if (cursor != command_end)
      throw Error("Swift symbol commands do not fill their declared region");
  } else {
    // Synthetic native image models have no original nlist bytes.
    for (const auto &symbol : image.Symbols) {
      budget.tick();
      add_symbol(symbol.Name, symbol.Addr, symbol.Addr != 0);
    }
  }
  const Section *section = nullptr;
  for (const auto &candidate : image.Sections) {
    if (candidate.Name != "__swift5_types")
      continue;
    if (section)
      throw Error("ambiguous Mach-O metadata section __swift5_types");
    section = &candidate;
  }
  std::string status = "section-absent";
  if (section) {
    if ((section->Type & 255) == 1 || (section->Type & 255) == 12 ||
        (section->Type & 255) == 18 || section->Size % 4 ||
        section->Size / 4 > 100000 || !image.readVA(section->VA, section->Size))
      throw Error("invalid or non-file-backed Swift type-reference section");
    status = "recovered";
    auto read = [&](uint64_t address, unsigned width) {
      budget.tick();
      const auto *segment = image.getSegmentFor(address);
      if (!segment || address - segment->VA > segment->FileSz ||
          width > segment->FileSz - (address - segment->VA))
        throw Error("Swift metadata address is not file-backed");
      auto *data = image.readVA(address, width);
      if (!data)
        throw Error("unmapped Swift metadata address");
      uint64_t value = 0;
      for (unsigned i = 0; i < width; ++i)
        value |= uint64_t(data[i]) << (8 * i);
      return value;
    };
    auto relative = [](uint64_t address, int32_t displacement) {
      if ((displacement < 0 && uint64_t(-int64_t(displacement)) > address) ||
          (displacement >= 0 && uint64_t(displacement) > UINT64_MAX - address))
        throw Error("Swift relative metadata address overflows");
      return address + displacement;
    };
    for (uint64_t off = 0; off < section->Size; off += 4) {
      auto slot = section->VA + off;
      try {
        auto record = uint32_t(read(slot, 4));
        if (!record)
          continue;
        auto tag = record & 3;
        if (tag >= 2)
          throw Error("Objective-C interoperability type-reference records are "
                      "not decoded");
        auto descriptor = relative(slot, int32_t(record & ~uint32_t(3)));
        if (tag == 1) {
          if (image.IsRelocatable || image.MachOChainedFixupsAmbiguous ||
              (image.MachOHasChainedFixups &&
               !image.MachOResolvedChainedPointerSlots.count(descriptor)))
            throw Error(
                "indirect type references require resolved absolute pointers");
          descriptor = read(descriptor, image.getPointerSize());
        }
        auto kind = read(descriptor, 4) & 31;
        if (kind < 16 || kind > 18)
          throw Error("unsupported nominal type descriptor kind");
        if (descriptor > UINT64_MAX - 8)
          throw Error("Swift descriptor address overflows");
        auto name_address =
            relative(descriptor + 8, int32_t(read(descriptor + 8, 4)));
        std::string name;
        bool terminated = false;
        for (unsigned i = 0; i < 16384; ++i) {
          if (i > UINT64_MAX - name_address)
            throw Error("Swift name address overflows");
          auto byte = read(name_address + i, 1);
          if (!byte) {
            terminated = true;
            break;
          }
          name += char(byte);
        }
        if (!terminated || !llvm::json::isUTF8(name))
          throw Error("invalid or oversized Swift nominal name");
        types.push_back(Object{{"name", std::move(name)},
                               {"kind", kind == 16   ? "class"
                                        : kind == 17 ? "struct"
                                                     : "enum"},
                               {"descriptor", hex(descriptor)}});
      } catch (const Error &error) {
        budget.check();
        if (!budget.remaining)
          throw;
        status = "partial";
        limitations.push_back("Swift type reference at " + hex(slot) +
                              " could not be decoded: " + error.what());
      }
    }
  }
  return Object{{"status", status},
                {"types", std::move(types)},
                {"symbols", std::move(symbols)},
                {"limitations", std::move(limitations)}};
}
} // namespace neverd::mobile::ios
