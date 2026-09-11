//===- MobileDalvikDex.cpp - Bounded standard DEX reader
//-------------------===//
#include "MobileDalvik.h"

#include "llvm/Support/SHA1.h"

#include <algorithm>
#include <any>
#include <array>
#include <bit>
#include <functional>
#include <limits>
#include <tuple>
#include <utility>

namespace neverd::mobile::dalvik {
namespace {
[[noreturn]] void bad(std::string message) {
  throw Error("Invalid DEX: " + message);
}
int64_t signedValue(uint64_t value, unsigned bits) {
  if (bits < 64 && (value & (uint64_t(1) << (bits - 1))))
    value |= (~uint64_t(0)) << bits;
  return std::bit_cast<int64_t>(value);
}
uint32_t targetPC(uint32_t pc, int64_t delta) {
  int64_t target = int64_t(pc) + delta;
  if (target < 0 || uint64_t(target) > UINT32_MAX)
    bad("branch target outside code address space");
  return uint32_t(target);
}
struct Cursor {
  std::string_view data;
  size_t pos, end;
  Budget &budget;
  Cursor(std::string_view data, size_t offset, size_t end, Budget &budget)
      : data(data), pos(offset), end(end), budget(budget) {
    if (offset > end || end > data.size())
      bad("item lies outside its section");
  }
  std::string_view take(size_t size) {
    budget.tick(1 + size / 16);
    if (size > end - pos)
      bad("truncated item");
    auto result = data.substr(pos, size);
    pos += size;
    return result;
  }
  uint64_t integer(unsigned size) {
    if (size > 8)
      bad("invalid integer width");
    auto bytes = take(size);
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i)
      value |= uint64_t(uint8_t(bytes[i])) << (i * 8);
    return value;
  }
  uint8_t u8() { return uint8_t(integer(1)); }
  uint16_t u16() { return uint16_t(integer(2)); }
  uint32_t u32() { return uint32_t(integer(4)); }
  int64_t leb(bool is_signed = false) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 5; ++i) {
      unsigned byte = u8();
      value |= uint64_t(byte & 127) << (i * 7);
      if (!(byte & 128)) {
        if (is_signed) {
          int64_t result = signedValue(value, (i + 1) * 7);
          if (result < INT32_MIN || result > INT32_MAX)
            bad("SLEB128 overflow");
          return result;
        }
        if (value > UINT32_MAX)
          bad("ULEB128 overflow");
        return int64_t(value);
      }
    }
    bad("unterminated LEB128");
  }
};
void appendUTF8(std::string &text, uint32_t cp) {
  if (cp < 0x80)
    text += char(cp);
  else if (cp < 0x800) {
    text += char(0xc0 | (cp >> 6));
    text += char(0x80 | (cp & 63));
  } else if (cp < 0x10000) {
    text += char(0xe0 | (cp >> 12));
    text += char(0x80 | ((cp >> 6) & 63));
    text += char(0x80 | (cp & 63));
  } else {
    text += char(0xf0 | (cp >> 18));
    text += char(0x80 | ((cp >> 12) & 63));
    text += char(0x80 | ((cp >> 6) & 63));
    text += char(0x80 | (cp & 63));
  }
}
struct DecodedString {
  std::string text;
  std::u16string units;
};
struct Encoded {
  unsigned kind = 0;
  FieldValue value;
  std::vector<Encoded> array;
  std::string annotation_type;
  std::map<std::string, Encoded> elements;
};
using Annotation = std::pair<std::string, std::map<std::string, Encoded>>;
struct AnnotationItem {
  unsigned visibility;
  Annotation value;
};
using AnnotationSet = std::vector<AnnotationItem>;
struct AnnotationDirectory {
  AnnotationSet classes;
  std::array<std::vector<std::pair<uint32_t, uint32_t>>, 3> members;
};
struct OpSpec {
  std::string name, form;
  char pool = 0;
};
const std::array<OpSpec, 256> &opcodes() {
  static const auto table = [] {
    std::array<OpSpec, 256> ops{};
    auto put = [&](unsigned code, std::string name, std::string form,
                   char pool = 0) {
      ops[code] = {std::move(name), std::move(form), pool};
    };
    for (auto [base, name] :
         {std::pair{1, "move"}, {4, "move-wide"}, {7, "move-object"}}) {
      put(base, name, "12x");
      put(base + 1, std::string(name) + "/from16", "22x");
      put(base + 2, std::string(name) + "/16", "32x");
    }
    unsigned code = 0xa;
    for (auto name : {"move-result", "move-result-wide", "move-result-object",
                      "move-exception"})
      put(code++, name, "11x");
    put(0, "nop", "10x");
    put(0xe, "return-void", "10x");
    code = 0xf;
    for (auto name : {"return", "return-wide", "return-object"})
      put(code++, name, "11x");
    put(0x12, "const/4", "11n");
    put(0x13, "const/16", "21s");
    put(0x14, "const", "31i");
    put(0x15, "const/high16", "21h");
    put(0x16, "const-wide/16", "21s");
    put(0x17, "const-wide/32", "31i");
    put(0x18, "const-wide", "51l");
    put(0x19, "const-wide/high16", "21h");
    put(0x1a, "const-string", "21c", 's');
    put(0x1b, "const-string/jumbo", "31c", 's');
    put(0x1c, "const-class", "21c", 't');
    put(0x1d, "monitor-enter", "11x");
    put(0x1e, "monitor-exit", "11x");
    put(0x1f, "check-cast", "21c", 't');
    put(0x20, "instance-of", "22c", 't');
    put(0x21, "array-length", "12x");
    put(0x22, "new-instance", "21c", 't');
    put(0x23, "new-array", "22c", 't');
    put(0x24, "filled-new-array", "35c", 't');
    put(0x25, "filled-new-array/range", "3rc", 't');
    put(0x26, "fill-array-data", "31t");
    put(0x27, "throw", "11x");
    put(0x28, "goto", "10t");
    put(0x29, "goto/16", "20t");
    put(0x2a, "goto/32", "30t");
    put(0x2b, "packed-switch", "31t");
    put(0x2c, "sparse-switch", "31t");
    code = 0x2d;
    for (auto name :
         {"cmpl-float", "cmpg-float", "cmpl-double", "cmpg-double", "cmp-long"})
      put(code++, name, "23x");
    code = 0;
    for (auto name : {"eq", "ne", "lt", "ge", "gt", "le"}) {
      put(0x32 + code, "if-" + std::string(name), "22t");
      put(0x38 + code++, "if-" + std::string(name) + "z", "21t");
    }
    for (auto [base, name, form, pool] :
         {std::tuple{0x44, "aget", "23x", char(0)},
          {0x4b, "aput", "23x", char(0)},
          {0x52, "iget", "22c", 'f'},
          {0x59, "iput", "22c", 'f'},
          {0x60, "sget", "21c", 'f'},
          {0x67, "sput", "21c", 'f'}}) {
      unsigned delta = 0;
      for (auto suffix :
           {"", "-wide", "-object", "-boolean", "-byte", "-char", "-short"})
        put(base + delta++, std::string(name) + suffix, form, pool);
    }
    code = 0;
    for (auto name : {"virtual", "super", "direct", "static", "interface"}) {
      put(0x6e + code, "invoke-" + std::string(name), "35c", 'm');
      put(0x74 + code++, "invoke-" + std::string(name) + "/range", "3rc", 'm');
    }
    code = 0x7b;
    for (auto name : {"neg-int",       "not-int",        "neg-long",
                      "not-long",      "neg-float",      "neg-double",
                      "int-to-long",   "int-to-float",   "int-to-double",
                      "long-to-int",   "long-to-float",  "long-to-double",
                      "float-to-int",  "float-to-long",  "float-to-double",
                      "double-to-int", "double-to-long", "double-to-float",
                      "int-to-byte",   "int-to-char",    "int-to-short"})
      put(code++, name, "12x");
    code = 0;
    for (auto type : {"int", "long", "float", "double"}) {
      bool integer =
          std::string_view(type) == "int" || std::string_view(type) == "long";
      for (auto operation : {"add", "sub", "mul", "div", "rem", "and", "or",
                             "xor", "shl", "shr", "ushr"}) {
        if (!integer && std::string_view(operation) == "and")
          break;
        auto name = std::string(operation) + "-" + type;
        put(0x90 + code, name, "23x");
        put(0xb0 + code++, name + "/2addr", "12x");
      }
    }
    code = 0;
    for (auto operation : {"add", "rsub", "mul", "div", "rem", "and", "or",
                           "xor", "shl", "shr", "ushr"}) {
      auto name = std::string(operation) + "-int";
      if (code < 8)
        put(0xd0 + code, name + (code == 1 ? "" : "/lit16"), "22s");
      put(0xd8 + code++, name + "/lit8", "22b");
    }
    return ops;
  }();
  return table;
}
struct Section {
  size_t start, end;
  uint32_t count;
};
struct Payload {
  size_t size = 0;
  unsigned ident = 0, element_width = 0;
  std::vector<int32_t> keys, targets;
  std::vector<uint64_t> data;
};
struct Code {
  unsigned registers, incoming;
  std::vector<Instruction> instructions;
  std::vector<TryRegion> tries;
  uint32_t size;
};
class Dex {
  std::string_view data;
  std::string source_id;
  Budget &budget;
  using Key = std::pair<unsigned, size_t>;
  std::map<unsigned, Section> sections;
  std::map<Key, std::any> cache;
  std::map<Key, size_t> ranges;
  std::set<Key> active;
  std::map<Key, std::string> contexts;
  std::vector<std::string> strings, types;
  std::map<std::string, unsigned> type_indices;
  std::vector<std::pair<std::vector<std::string>, std::string>> protos;
  std::vector<FieldRef> fields;
  std::vector<MethodRef> methods;
  std::map<std::string, std::vector<std::string>> members;
  Cursor cursor(size_t offset) {
    return Cursor(data, offset, data.size(), budget);
  }
  Cursor cursor(size_t offset, size_t end) {
    return Cursor(data, offset, end, budget);
  }
  template <class T>
  const T &at(const std::vector<T> &values, uint64_t index,
              std::string_view name) {
    if (index >= values.size())
      bad(std::string(name) + " index out of bounds");
    return values[size_t(index)];
  }
  std::string string(uint64_t index) { return at(strings, index, "string"); }
  std::string type(uint64_t index, bool allow_void = false) {
    auto result = at(types, index, "type");
    if (result == "V" && !allow_void)
      bad("void outside return type");
    return result;
  }
  template <class T, class F>
  T item(unsigned kind, size_t offset, F read, unsigned alignment = 1) {
    Key key{kind, offset};
    if (auto found = cache.find(key); found != cache.end())
      return std::any_cast<const T &>(found->second);
    if (active.contains(key))
      bad("cyclic data item reference");
    auto section = sections.find(kind);
    if (section == sections.end() || offset % alignment ||
        offset < section->second.start || offset >= section->second.end)
      bad("item points outside its mapped section");
    auto reader = cursor(offset, section->second.end);
    active.insert(key);
    try {
      T result = read(reader);
      active.erase(key);
      ranges[key] = reader.pos;
      cache[key] = result;
      return result;
    } catch (...) {
      active.erase(key);
      throw;
    }
  }
  void context(unsigned kind, size_t offset, const std::string &identity) {
    auto [it, inserted] = contexts.emplace(Key{kind, offset}, identity);
    if (!inserted && it->second != identity)
      bad("shared data item has inconsistent declaration context");
  }
  using Tables = std::map<unsigned, std::pair<uint32_t, uint32_t>>;
  Tables header() {
    budget.tick(1 + data.size() / 32);
    if (data.size() > budget.limits.max_bytes)
      bad("input exceeds byte limit");
    if (data.size() < 112 || data.substr(0, 4) != "dex\n" || data[7])
      bad("missing standard header");
    auto version = data.substr(4, 3);
    if (version != "035" && version != "037" && version != "038" &&
        version != "039" && version != "040")
      bad("unsupported DEX version (supported: 035, 037-040; 041 containers "
          "are unsupported)");
    auto reader = cursor(8, 112);
    uint32_t checksum = reader.u32();
    auto signature = reader.take(20);
    uint32_t a = 1, b = 0;
    for (size_t start = 12; start < data.size();) {
      size_t end = std::min(data.size(), start + 5552);
      budget.tick();
      for (; start < end; ++start) {
        a += uint8_t(data[start]);
        b += a;
      }
      a %= 65521;
      b %= 65521;
    }
    if ((a | (b << 16)) != checksum)
      bad("checksum mismatch");
    auto hash = llvm::SHA1::hash(llvm::ArrayRef<uint8_t>(
        reinterpret_cast<const uint8_t *>(data.data() + 32), data.size() - 32));
    if (!std::equal(hash.begin(), hash.end(),
                    reinterpret_cast<const uint8_t *>(signature.data())))
      bad("signature mismatch");
    uint32_t size = reader.u32(), header_size = reader.u32(),
             endian = reader.u32();
    if (size != data.size() || header_size != 112)
      bad("header/file size mismatch");
    if (endian != 0x12345678)
      bad("unsupported or invalid endian tag");
    uint32_t link_size = reader.u32(), link_off = reader.u32(),
             map_off = reader.u32();
    if (link_size || link_off)
      bad("statically linked DEX data is unsupported");
    Tables tables;
    for (unsigned kind = 1; kind <= 6; ++kind) {
      auto count = reader.u32(), offset = reader.u32();
      tables[kind] = {count, offset};
    }
    auto data_size = reader.u32(), data_off = reader.u32();
    if (data_off < 112 || data_off > size || data_off % 4 ||
        data_size != size - data_off)
      bad("invalid data section bounds");
    if (map_off % 4 || map_off < data_off || map_off > size - 4)
      bad("invalid map offset");
    auto map = cursor(map_off);
    unsigned count = map.u32();
    if (!count || count > 32)
      bad("invalid section map size");
    const std::map<unsigned, unsigned> fixed{{0, 112}, {1, 4}, {2, 4},
                                             {3, 12},  {4, 8}, {5, 8},
                                             {6, 32},  {7, 4}, {8, 8}};
    const std::set<unsigned> variable{0x1000, 0x1001, 0x1002, 0x1003,
                                      0x2000, 0x2001, 0x2002, 0x2003,
                                      0x2004, 0x2005, 0x2006};
    std::vector<std::tuple<unsigned, uint32_t, uint32_t>> entries;
    for (unsigned i = 0; i < count; ++i) {
      unsigned kind = map.u16(), reserved = map.u16();
      auto length = map.u32(), start = map.u32();
      if (reserved || !length || start >= size ||
          (!entries.empty() && start <= std::get<1>(entries.back())))
        bad("unordered, empty or invalid section map");
      if (!fixed.contains(kind) && !variable.contains(kind))
        bad("unsupported map section");
      if (kind == 7 || kind == 8)
        bad("method handles and custom call sites are unsupported");
      entries.emplace_back(kind, start, length);
    }
    for (size_t i = 0; i < entries.size(); ++i) {
      auto [kind, start, length] = entries[i];
      size_t end = i + 1 < entries.size() ? std::get<1>(entries[i + 1]) : size;
      if (sections.contains(kind))
        bad("duplicate map section");
      if (fixed.contains(kind) &&
          (start % 4 || length > (end - start) / fixed.at(kind)))
        bad("overlapping or truncated fixed table");
      if (kind >= 0x1000 && start < data_off)
        bad("data item outside data section");
      sections[kind] = {start, end, length};
    }
    if (!sections.contains(0) || sections.at(0).start != 0 ||
        sections.at(0).count != 1)
      bad("map lacks the unique header");
    if (!sections.contains(0x1000) || sections.at(0x1000).start != map_off ||
        sections.at(0x1000).count != 1 || map.pos > sections.at(0x1000).end)
      bad("map does not describe itself");
    for (auto [kind, record] : tables) {
      auto [length, start] = record;
      auto section = sections.find(kind);
      if (!length) {
        if (start || section != sections.end())
          bad("empty table has storage");
      } else if (section == sections.end() || section->second.start != start ||
                 section->second.count != length || start < 112 ||
                 section->second.end > data_off)
        bad("header and map tables disagree");
      if ((kind == 2 || kind == 3) && length > 65535)
        bad("type/prototype table exceeds index limit");
      budget.tick(length);
    }
    return tables;
  }
  DecodedString mutf8(Cursor &reader) {
    size_t expected = size_t(reader.leb());
    if (expected > reader.end - reader.pos)
      bad("impossible UTF-16 string length");
    DecodedString result;
    while (true) {
      unsigned first = reader.u8(), unit;
      if (!first)
        break;
      if (first < 0x80)
        unit = first;
      else if (first >= 0xc0 && first <= 0xdf) {
        unsigned second = reader.u8();
        if ((second & 0xc0) != 0x80)
          bad("invalid MUTF-8 continuation");
        unit = ((first & 31) << 6) | (second & 63);
        if (unit < 0x80 && unit)
          bad("overlong MUTF-8");
      } else if (first >= 0xe0 && first <= 0xef) {
        unsigned second = reader.u8(), third = reader.u8();
        if ((second & 0xc0) != 0x80 || (third & 0xc0) != 0x80)
          bad("invalid MUTF-8 continuation");
        unit = ((first & 15) << 12) | ((second & 63) << 6) | (third & 63);
        if (unit < 0x800)
          bad("overlong MUTF-8");
      } else
        bad("invalid MUTF-8 leading byte");
      result.units += char16_t(unit);
      if (result.units.size() > expected)
        bad("UTF-16 string length mismatch");
    }
    if (result.units.size() != expected)
      bad("UTF-16 string length mismatch");
    for (size_t i = 0; i < result.units.size(); ++i) {
      uint32_t cp = result.units[i];
      if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < result.units.size() &&
          result.units[i + 1] >= 0xdc00 && result.units[i + 1] <= 0xdfff)
        cp = 0x10000 + ((cp - 0xd800) << 10) + (result.units[++i] - 0xdc00);
      appendUTF8(result.text, cp);
    }
    return result;
  }
  std::vector<std::string> typeList(uint32_t offset) {
    if (!offset)
      return {};
    return item<std::vector<std::string>>(
        0x1001, offset,
        [&](Cursor &reader) {
          auto size = reader.u32();
          if (size > (reader.end - reader.pos) / 2)
            bad("truncated type list");
          std::vector<std::string> result;
          for (uint32_t i = 0; i < size; ++i)
            result.push_back(type(reader.u16()));
          return result;
        },
        4);
  }
  void readTables(const Tables &tables) {
    auto records = [&](unsigned kind) {
      auto [count, offset] = tables.at(kind);
      return cursor(offset, count ? sections.at(kind).end : offset);
    };
    auto reader = records(1);
    std::optional<std::u16string> previous_string;
    for (uint32_t i = 0; i < tables.at(1).first; ++i) {
      auto value = item<DecodedString>(0x2002, reader.u32(),
                                       [&](Cursor &r) { return mutf8(r); });
      if (previous_string && *previous_string >= value.units)
        bad("string IDs are duplicate or unordered");
      previous_string = value.units;
      strings.push_back(std::move(value.text));
    }
    auto type_reader = records(2);
    int64_t previous = -1;
    for (uint32_t i = 0; i < tables.at(2).first; ++i) {
      uint32_t index = type_reader.u32();
      if (int64_t(index) <= previous)
        bad("type IDs are duplicate or unordered");
      previous = index;
      types.push_back(descriptor(string(index), true));
      type_indices[types.back()] = unsigned(types.size() - 1);
    }
    auto proto_reader = records(3);
    std::optional<std::pair<uint32_t, std::vector<unsigned>>> previous_proto;
    for (uint32_t i = 0; i < tables.at(3).first; ++i) {
      uint32_t shorty = proto_reader.u32(), result = proto_reader.u32(),
               parameters = proto_reader.u32();
      auto returns = type(result, true);
      auto args = typeList(parameters);
      auto shortType = [](const std::string &t) {
        return t.starts_with('L') || t.starts_with('[') ? "L" : t;
      };
      std::string expected = shortType(returns);
      std::vector<unsigned> indices;
      for (auto &arg : args) {
        expected += shortType(arg);
        indices.push_back(type_indices.at(arg));
      }
      if (string(shorty) != expected)
        bad("prototype shorty disagrees with descriptors");
      auto key = std::pair{result, indices};
      if (previous_proto && key <= *previous_proto)
        bad("prototype IDs are duplicate or unordered");
      previous_proto = std::move(key);
      protos.emplace_back(std::move(args), std::move(returns));
    }
    for (unsigned kind : {4, 5}) {
      auto r = records(kind);
      std::optional<std::tuple<unsigned, uint32_t, unsigned>> previous_member;
      for (uint32_t i = 0; i < tables.at(kind).first; ++i) {
        unsigned owner = r.u16(), typ = r.u16();
        uint32_t name = r.u32();
        auto owner_name = type(owner), member = string(name);
        // A method ID may name an array's clone or inherited Object method.
        // Field IDs still require a class owner; type() validates descriptors.
        bool valid_owner = owner_name.starts_with('L') ||
                           (kind == 5 && owner_name.starts_with('['));
        if (!valid_owner || member.empty())
          bad("invalid member owner/name");
        auto key = std::tuple{owner, name, typ};
        if (previous_member && key <= *previous_member)
          bad("member IDs are duplicate or unordered");
        previous_member = key;
        if (kind == 4)
          fields.push_back({owner_name, member, type(typ)});
        else {
          auto [args, result] = at(protos, typ, "prototype");
          methods.push_back({owner_name, member, args, result});
        }
      }
    }
  }
  Encoded encoded(Cursor &reader, unsigned depth = 0) {
    if (depth > 64)
      bad("encoded value nesting exceeds limit");
    unsigned tag = reader.u8(), kind = tag & 31, arg = tag >> 5;
    Encoded result;
    result.kind = kind;
    static const std::map<unsigned, unsigned> sizes{
        {0, 1},    {2, 2},    {3, 2},    {4, 4},    {6, 8},
        {0x10, 4}, {0x11, 8}, {0x15, 4}, {0x16, 4}, {0x17, 4},
        {0x18, 4}, {0x19, 4}, {0x1a, 4}, {0x1b, 4}};
    if (auto found = sizes.find(kind); found != sizes.end()) {
      unsigned size = arg + 1;
      if (size > found->second)
        bad("encoded value has invalid width");
      uint64_t value = reader.integer(size);
      if (kind == 0 || kind == 2 || kind == 4 || kind == 6)
        result.value = signedValue(value, size * 8);
      else if (kind == 0x10 || kind == 0x11)
        result.value =
            FloatBits{value << ((found->second - size) * 8), kind == 0x11};
      else if (kind == 0x17)
        result.value = string(value);
      else if (kind == 0x18)
        result.value = type(value, true);
      else if (kind == 0x19 || kind == 0x1b) {
        auto &ref = at(fields, value, "field");
        result.value = ref.owner + "->" + ref.name + ":" + ref.type;
      } else if (kind == 0x1a)
        result.value = at(methods, value, "method").identity();
      else if (kind == 0x15) {
        auto &[args, returns] = at(protos, value, "prototype");
        std::string text = "(";
        for (auto &arg_type : args)
          text += arg_type;
        result.value = text + ")" + returns;
      } else if (kind == 0x16)
        bad("encoded method handles are unsupported");
      else
        result.value = int64_t(value);
      return result;
    }
    if (kind == 0x1f && arg <= 1) {
      result.value = bool(arg);
      return result;
    }
    if (arg)
      bad("encoded value has invalid argument bits");
    if (kind == 0x1e)
      return result;
    if (kind == 0x1c) {
      auto count = reader.leb();
      budget.tick(uint64_t(count));
      for (int64_t i = 0; i < count; ++i)
        result.array.push_back(encoded(reader, depth + 1));
      return result;
    }
    if (kind == 0x1d) {
      auto [name, elements] = annotation(reader, depth + 1);
      result.annotation_type = name;
      result.elements = std::move(elements);
      return result;
    }
    bad("unknown encoded value");
  }
  Annotation annotation(Cursor &reader, unsigned depth = 0) {
    if (depth > 64)
      bad("annotation nesting exceeds limit");
    auto name = type(uint64_t(reader.leb()));
    if (!name.starts_with('L'))
      bad("annotation type is not a class");
    auto count = reader.leb();
    budget.tick(uint64_t(count));
    std::map<std::string, Encoded> elements;
    int64_t previous = -1;
    for (int64_t i = 0; i < count; ++i) {
      auto index = reader.leb();
      if (index <= previous)
        bad("annotation elements are duplicate or unordered");
      previous = index;
      auto key = string(uint64_t(index));
      elements.emplace(std::move(key), encoded(reader, depth + 1));
    }
    return {std::move(name), std::move(elements)};
  }
  AnnotationSet annotationSet(uint32_t offset) {
    if (!offset)
      return {};
    return item<AnnotationSet>(
        0x1003, offset,
        [&](Cursor &reader) {
          auto count = reader.u32();
          if (count > (reader.end - reader.pos) / 4)
            bad("truncated annotation set");
          AnnotationSet result;
          int64_t previous = -1;
          for (uint32_t i = 0; i < count; ++i) {
            auto entry =
                item<AnnotationItem>(0x2004, reader.u32(), [&](Cursor &r) {
                  unsigned visibility = r.u8();
                  if (visibility > 2)
                    bad("invalid annotation visibility");
                  return AnnotationItem{visibility, annotation(r)};
                });
            unsigned index = type_indices.at(entry.value.first);
            if (int64_t(index) <= previous)
              bad("annotation types are duplicate or unordered");
            previous = index;
            result.push_back(std::move(entry));
          }
          return result;
        },
        4);
  }
  void requireUnannotated(const AnnotationSet &values,
                          const std::string &declaration) {
    if (!values.empty())
      bad("unsupported annotation " + values.front().value.first + " on " +
          declaration);
  }
  void sourceAnnotations(const AnnotationSet &values,
                         std::optional<std::string> &signature,
                         std::optional<std::vector<std::string>> *throws_types,
                         bool &deprecated, const std::string &declaration) {
    for (const auto &entry : values) {
      budget.tick();
      const auto &[name, elements] = entry.value;
      if (name == "Ljava/lang/Deprecated;") {
        if (entry.visibility != 1)
          bad("Deprecated annotation on " + declaration +
              " requires runtime visibility");
        if (!elements.empty())
          bad("Deprecated annotation on " + declaration +
              " must have no elements");
        if (deprecated)
          bad("duplicate Deprecated annotation on " + declaration);
        deprecated = true;
        continue;
      }
      bool is_signature = name == "Ldalvik/annotation/Signature;";
      bool is_throws = name == "Ldalvik/annotation/Throws;" && throws_types;
      if (!is_signature && !is_throws)
        bad("unsupported annotation " + name + " on " + declaration);
      if (entry.visibility != 2)
        bad("source annotation " + name + " on " + declaration +
            " requires system visibility");
      auto found = elements.find("value");
      if (elements.size() != 1 || found == elements.end() ||
          found->second.kind != 0x1c)
        bad("invalid " + name + " value on " + declaration);
      if (is_signature) {
        if (signature)
          bad("duplicate Signature annotation on " + declaration);
        std::string joined;
        for (const auto &part : found->second.array) {
          budget.tick();
          if (part.kind != 0x17)
            bad("Signature fragment is not a string on " + declaration);
          const auto &text = std::get<std::string>(part.value);
          budget.tick(1 + text.size() / 16);
          if (text.size() > budget.limits.max_bytes - joined.size())
            bad("Signature exceeds byte budget on " + declaration);
          joined += text;
        }
        signature = std::move(joined);
      } else {
        if (*throws_types)
          bad("duplicate Throws annotation on " + declaration);
        std::vector<std::string> types;
        for (const auto &part : found->second.array) {
          budget.tick();
          if (part.kind != 0x18 ||
              !std::get<std::string>(part.value).starts_with('L'))
            bad("Throws entry is not a class on " + declaration);
          types.push_back(std::get<std::string>(part.value));
        }
        *throws_types = std::move(types);
      }
    }
  }
  bool annotationMetadata(const AnnotationItem &entry, Class &cls) {
    const auto &[name, elements] = entry.value;
    bool retention = name == "Ljava/lang/annotation/Retention;";
    bool target = name == "Ljava/lang/annotation/Target;";
    bool documented = name == "Ljava/lang/annotation/Documented;";
    bool inherited = name == "Ljava/lang/annotation/Inherited;";
    if (!retention && !target && !documented && !inherited)
      return false;
    if (entry.visibility != 1)
      bad("annotation metadata " + name + " on " + cls.name +
          " requires runtime visibility");
    auto &metadata = cls.annotation_metadata;
    if (documented || inherited) {
      if (!elements.empty())
        bad("annotation metadata " + name + " must have no elements");
      bool &present = documented ? metadata.documented : metadata.inherited;
      if (present)
        bad("duplicate annotation metadata " + name);
      present = true;
      return true;
    }
    auto value = elements.find("value");
    if (elements.size() != 1 || value == elements.end())
      bad("annotation metadata " + name + " requires exactly value");
    auto enumValue = [&](const Encoded &encoded, const std::string &owner) {
      budget.tick();
      if (encoded.kind != 0x1b)
        bad("annotation metadata " + name + " requires an enum value");
      const auto ref = fieldRef(std::get<std::string>(encoded.value));
      if (ref.owner != owner || ref.type != owner)
        bad("annotation metadata " + name + " has an invalid enum identity");
      return ref.name;
    };
    if (retention) {
      if (metadata.retention)
        bad("duplicate Retention annotation");
      metadata.retention = enumValue(
          value->second, "Ljava/lang/annotation/RetentionPolicy;");
    } else {
      if (metadata.targets)
        bad("duplicate Target annotation");
      if (value->second.kind != 0x1c)
        bad("Target annotation requires an enum array");
      std::vector<std::string> targets;
      for (const auto &item : value->second.array)
        targets.push_back(
            enumValue(item, "Ljava/lang/annotation/ElementType;"));
      metadata.targets = std::move(targets);
    }
    return true;
  }
  void annotations(Class &cls, uint32_t offset) {
    if (!offset)
      return;
    auto directory = item<AnnotationDirectory>(
        0x2006, offset,
        [&](Cursor &reader) {
          auto class_off = reader.u32();
          AnnotationDirectory result;
          std::array<uint32_t, 3> counts{reader.u32(), reader.u32(),
                                         reader.u32()};
          for (unsigned kind = 0; kind < 3; ++kind) {
            auto count = counts[kind];
            if (count > (reader.end - reader.pos) / 8)
              bad("truncated annotation directory");
            int64_t previous = -1;
            for (uint32_t i = 0; i < count; ++i) {
              auto index = reader.u32(), annotation_off = reader.u32();
              if (int64_t(index) <= previous)
                bad("annotation directory members unordered");
              previous = index;
              result.members[kind].emplace_back(index, annotation_off);
            }
          }
          result.classes = annotationSet(class_off);
          return result;
        },
        4);
    if (std::any_of(directory.members.begin(), directory.members.end(),
                    [](const auto &members) { return !members.empty(); }))
      context(0x2006, offset, cls.name);
    std::map<FieldRef, Field *> defined_fields;
    std::map<MethodRef, Method *> defined_methods;
    for (auto &field : cls.fields)
      defined_fields.emplace(field.reference, &field);
    for (auto &method : cls.methods)
      defined_methods.emplace(method.reference, &method);
    // Cached directory rows have no binding side effects. Every use is bound
    // to the exact class_data declaration, including shared annotation sets.
    for (unsigned kind = 0; kind < 3; ++kind) {
      for (auto [index, annotation_off] : directory.members[kind]) {
        budget.tick();
        if (kind == 0) {
          const auto &ref = at(fields, index, "annotated field");
          if (ref.owner != cls.name)
            bad("annotation directory owner mismatch");
          auto found = defined_fields.find(ref);
          if (found == defined_fields.end())
            bad("annotated field has no class_data definition");
          sourceAnnotations(
              annotationSet(annotation_off), found->second->generic_signature,
              nullptr, found->second->deprecated,
              "field " + ref.owner + "->" + ref.name + ":" + ref.type);
          continue;
        }
        const auto &ref = at(methods, index, "annotated method");
        if (ref.owner != cls.name)
          bad("annotation directory owner mismatch");
        auto found = defined_methods.find(ref);
        if (found == defined_methods.end())
          bad("annotated method has no class_data definition");
        if (kind == 1) {
          sourceAnnotations(
              annotationSet(annotation_off), found->second->generic_signature,
              &found->second->declared_throws, found->second->deprecated,
              "method " + ref.identity());
          continue;
        }
        auto parameters = item<std::vector<uint32_t>>(
            0x1002, annotation_off,
            [&](Cursor &r) {
              auto count = r.u32();
              if (count > (r.end - r.pos) / 4)
                bad("truncated parameter annotation list");
              std::vector<uint32_t> values;
              for (uint32_t p = 0; p < count; ++p)
                values.push_back(r.u32());
              return values;
            },
            4);
        if (parameters.size() != ref.parameters.size())
          bad("parameter annotation count mismatch");
        for (size_t p = 0; p < parameters.size(); ++p)
          requireUnannotated(annotationSet(parameters[p]),
                             "parameter " + std::to_string(p) + " of method " +
                                 ref.identity());
      }
    }
    std::map<std::string, std::map<std::string, Encoded>> values;
    for (const auto &entry : directory.classes) {
      budget.tick();
      const auto &[name, elements] = entry.value;
      if (name == "Ldalvik/annotation/Signature;" ||
          name == "Ljava/lang/Deprecated;") {
        sourceAnnotations({entry}, cls.generic_signature, nullptr,
                          cls.deprecated, "class " + cls.name);
        continue;
      }
      if (annotationMetadata(entry, cls))
        continue;
      if (name != "Ldalvik/annotation/EnclosingClass;" &&
          name != "Ldalvik/annotation/EnclosingMethod;" &&
          name != "Ldalvik/annotation/InnerClass;" &&
          name != "Ldalvik/annotation/MemberClasses;") {
        if (entry.visibility == 2 || !elements.empty())
          bad("unsupported annotation " + name + " on class " + cls.name);
        // The complete input must later prove an exact marker declaration,
        // its retention and its class target. Parsing is not acceptance.
        cls.marker_annotations.push_back({name, entry.visibility});
        continue;
      }
      if (entry.visibility != 2)
        bad("structural annotation " + name + " on class " + cls.name +
            " requires system visibility");
      values.emplace(name, elements);
    }
    auto get =
        [&](std::string_view key) -> const std::map<std::string, Encoded> * {
      auto found = values.find(std::string(key));
      return found == values.end() ? nullptr : &found->second;
    };
    auto enclosing = get("Ldalvik/annotation/EnclosingClass;"),
         enclosing_method = get("Ldalvik/annotation/EnclosingMethod;"),
         inner = get("Ldalvik/annotation/InnerClass;");
    if (enclosing && enclosing_method)
      bad("conflicting enclosing annotations for " + cls.name);
    if (enclosing) {
      auto entry = enclosing->find("value");
      if (enclosing->size() != 1 || entry == enclosing->end() ||
          entry->second.kind != 0x18 ||
          !std::get<std::string>(entry->second.value).starts_with('L'))
        bad("invalid EnclosingClass annotation");
      cls.enclosing = std::get<std::string>(entry->second.value);
    }
    if (enclosing_method) {
      auto entry = enclosing_method->find("value");
      if (enclosing_method->size() != 1 || entry == enclosing_method->end() ||
          entry->second.kind != 0x1a)
        bad("invalid EnclosingMethod annotation for " + cls.name);
      auto ref = methodRef(std::get<std::string>(entry->second.value));
      if (!ref.owner.starts_with('L'))
        bad("EnclosingMethod owner is not a class for " + cls.name);
      cls.enclosing_method = std::move(ref);
    }
    if (inner) {
      cls.inner_class_present = true;
      auto name = inner->find("name"), flags = inner->find("accessFlags");
      if (inner->size() != 2 || name == inner->end() ||
          (name->second.kind != 0x17 && name->second.kind != 0x1e) ||
          flags == inner->end() || flags->second.kind != 4)
        bad("invalid InnerClass annotation");
      if (name->second.kind == 0x17)
        cls.inner_name = std::get<std::string>(name->second.value);
      cls.inner_access =
          accessFlags(uint32_t(std::get<int64_t>(flags->second.value)));
    }
    if (bool(inner) != bool(enclosing || enclosing_method))
      bad("incomplete inner class metadata");
    if (auto member = get("Ldalvik/annotation/MemberClasses;")) {
      auto value = member->find("value");
      if (member->size() != 1 || value == member->end() ||
          value->second.kind != 0x1c)
        bad("invalid MemberClasses annotation");
      std::vector<std::string> names;
      std::set<std::string> seen;
      for (auto &entry : value->second.array) {
        if (entry.kind != 0x18 ||
            !std::get<std::string>(entry.value).starts_with('L'))
          bad("invalid MemberClasses annotation");
        auto name = std::get<std::string>(entry.value);
        if (!seen.insert(name).second)
          bad("duplicate MemberClasses entry");
        names.push_back(std::move(name));
      }
      members[cls.name] = std::move(names);
    }
  }
  Payload payload(const std::vector<uint16_t> &words, size_t pc) {
    if (pc % 2 || words.size() - pc < 2)
      bad("unaligned/truncated payload");
    Payload result;
    result.ident = words[pc];
    uint64_t count = words[pc + 1];
    auto value = [&](size_t at) {
      if (at > words.size() || words.size() - at < 2)
        bad("truncated payload value");
      return uint32_t(words[at]) | (uint32_t(words[at + 1]) << 16);
    };
    if (result.ident == 0x100 || result.ident == 0x200) {
      result.size =
          size_t(result.ident == 0x100 ? 4 + count * 2 : 2 + count * 4);
      if (result.size > words.size() - pc)
        bad("truncated switch payload");
      budget.tick(count);
      size_t start;
      if (result.ident == 0x100) {
        int64_t first = signedValue(value(pc + 2), 32);
        if (count && first + int64_t(count) - 1 > INT32_MAX)
          bad("packed switch key overflow");
        for (uint64_t i = 0; i < count; ++i)
          result.keys.push_back(int32_t(first + int64_t(i)));
        start = pc + 4;
      } else {
        for (size_t i = 0; i < count; ++i) {
          int32_t key = int32_t(signedValue(value(pc + 2 + i * 2), 32));
          if (!result.keys.empty() && key <= result.keys.back())
            bad("sparse switch keys unordered");
          result.keys.push_back(key);
        }
        start = pc + 2 + size_t(count) * 2;
      }
      for (size_t i = 0; i < count; ++i)
        result.targets.push_back(
            int32_t(signedValue(value(start + i * 2), 32)));
      return result;
    }
    if (result.ident == 0x300) {
      result.element_width = unsigned(count);
      count = value(pc + 2);
      if (result.element_width != 1 && result.element_width != 2 &&
          result.element_width != 4 && result.element_width != 8)
        bad("invalid array payload element width");
      uint64_t bytes = count * result.element_width, size = 4 + (bytes + 1) / 2;
      if (size > words.size() - pc)
        bad("truncated array payload");
      result.size = size_t(size);
      budget.tick(bytes);
      for (uint64_t i = 0; i < count; ++i) {
        uint64_t bits = 0;
        for (unsigned j = 0; j < result.element_width; ++j) {
          size_t byte = size_t(i * result.element_width + j);
          unsigned unit = words[pc + 4 + byte / 2];
          bits |= uint64_t((unit >> ((byte % 2) * 8)) & 255) << (j * 8);
        }
        result.data.push_back(bits);
      }
      return result;
    }
    bad("unknown payload pseudo-opcode");
  }
  static void wideRegisters(const std::string &name,
                            const std::vector<unsigned> &registers,
                            unsigned count) {
    std::set<size_t> wide;
    auto base = name.substr(0, name.find('/'));
    if (base == "move-wide")
      wide = {0, 1};
    else if (base == "move-result-wide" || base == "return-wide" ||
             base == "const-wide" || base == "aget-wide" ||
             base == "aput-wide" || base == "iget-wide" ||
             base == "iput-wide" || base == "sget-wide" || base == "sput-wide")
      wide = {0};
    else if (auto at = base.find("-to-"); at != std::string::npos) {
      auto source = base.substr(0, at), dest = base.substr(at + 4);
      if (source == "long" || source == "double")
        wide.insert(1);
      if (dest == "long" || dest == "double")
        wide.insert(0);
    } else if (base == "cmp-long" || base == "cmpl-double" ||
               base == "cmpg-double")
      wide = {1, 2};
    else if (base.ends_with("-long") || base.ends_with("-double")) {
      for (size_t i = 0; i < registers.size(); ++i)
        wide.insert(i);
      if (base.starts_with("shl-") || base.starts_with("shr-") ||
          base.starts_with("ushr-"))
        wide.erase(registers.size() - 1);
    }
    for (auto index : wide)
      if (index >= registers.size() || registers[index] + 1 >= count)
        bad("wide register pair exceeds frame");
  }
  using Instructions =
      std::pair<std::vector<Instruction>, std::map<uint32_t, unsigned>>;
  Instructions instructions(const std::vector<uint16_t> &words,
                            unsigned registers, unsigned outs) {
    Instructions result;
    auto &[code, lengths] = result;
    std::map<uint32_t, Payload> payloads;
    for (size_t pc = 0; pc < words.size();) {
      budget.tick();
      unsigned word = words[pc], opcode = word & 255;
      if (!opcode && word) {
        auto data_payload = payload(words, pc);
        size_t size = data_payload.size;
        payloads.emplace(uint32_t(pc), std::move(data_payload));
        pc += size;
        continue;
      }
      auto &spec = opcodes()[opcode];
      if (spec.name.empty())
        bad("unsupported opcode at code unit " + std::to_string(pc));
      auto &name = spec.name;
      auto &form = spec.form;
      unsigned size = unsigned(form[0] - '0');
      if (size > words.size() - pc)
        bad("truncated instruction");
      unsigned high = word >> 8;
      Instruction ins;
      ins.pc = uint32_t(pc);
      ins.opcode = name;
      auto &regs = ins.registers;
      auto tail = [&](unsigned i) { return words[pc + 1 + i]; };
      auto tailValue = [&] {
        uint64_t value = 0;
        for (unsigned i = 0; i < size - 1; ++i)
          value |= uint64_t(tail(i)) << (i * 16);
        return value;
      };
      uint64_t index = 0;
      if ((form == "10x" || form == "20t" || form == "30t" || form == "32x") &&
          high)
        bad("nonzero reserved instruction bits");
      if (form == "12x")
        regs = {high & 15, high >> 4};
      else if (form == "11x")
        regs = {high};
      else if (form == "11n") {
        regs = {high & 15};
        ins.literal = signedValue(high >> 4, 4);
      } else if (form == "22x")
        regs = {high, tail(0)};
      else if (form == "32x")
        regs = {tail(0), tail(1)};
      else if (form == "23x")
        regs = {high, unsigned(tail(0) & 255), unsigned(tail(0) >> 8)};
      else if (form == "22b") {
        regs = {high, unsigned(tail(0) & 255)};
        ins.literal = signedValue(tail(0) >> 8, 8);
      } else if (form == "21s" || form == "21h" || form == "31i" ||
                 form == "51l") {
        regs = {high};
        int64_t value = signedValue(tailValue(), 16 * (size - 1));
        if (form == "21h")
          value = std::bit_cast<int64_t>(uint64_t(value)
                                         << (opcode == 0x19 ? 48 : 16));
        ins.literal = value;
      } else if (form == "22s") {
        regs = {high & 15, high >> 4};
        ins.literal = signedValue(tail(0), 16);
      } else if (form == "21c" || form == "31c") {
        regs = {high};
        index = tailValue();
      } else if (form == "22c") {
        regs = {high & 15, high >> 4};
        index = tail(0);
      } else if (form == "21t" || form == "22t" || form == "31t") {
        regs = form == "22t" ? std::vector<unsigned>{high & 15, high >> 4}
                             : std::vector<unsigned>{high};
        ins.target =
            targetPC(uint32_t(pc), signedValue(tailValue(), 16 * (size - 1)));
      } else if (form == "10t" || form == "20t" || form == "30t") {
        ins.target =
            targetPC(uint32_t(pc),
                     form == "10t" ? signedValue(high, 8)
                                   : signedValue(tailValue(), 16 * (size - 1)));
      } else if (form == "35c") {
        unsigned count = high >> 4;
        index = tail(0);
        if (count > 5)
          bad("invoke register count exceeds instruction format");
        for (unsigned i = 0; i < std::min(count, 4u); ++i)
          regs.push_back((tail(1) >> (4 * i)) & 15);
        if (count == 5)
          regs.push_back(high & 15);
      } else if (form == "3rc") {
        index = tail(0);
        for (unsigned i = 0; i < high; ++i)
          regs.push_back(tail(1) + i);
      }
      for (auto reg : regs)
        if (reg >= registers)
          bad("instruction register exceeds frame");
      if (spec.pool == 's')
        ins.literal = string(index);
      else if (spec.pool == 't')
        ins.reference = type(index);
      else if (spec.pool == 'f')
        ins.reference = at(fields, index, "field");
      else if (spec.pool == 'm') {
        auto ref = at(methods, index, "method");
        if (ref.owner.starts_with('[') &&
            (ref.name == "<init>" || ref.name == "<clinit>"))
          bad("array type cannot own an initializer invocation");
        size_t expected = !name.starts_with("invoke-static");
        for (auto &typ : ref.parameters)
          expected += width(typ);
        if (regs.size() != expected || expected > outs)
          bad("invoke argument words disagree with prototype/frame");
        size_t word_index = !name.starts_with("invoke-static");
        for (auto &typ : ref.parameters) {
          if (width(typ) == 2 && regs[word_index + 1] != regs[word_index] + 1)
            bad("invoke wide argument is not an adjacent register pair");
          word_index += width(typ);
        }
        ins.reference = std::move(ref);
      }
      if (spec.pool == 't') {
        auto &ref = std::get<std::string>(ins.reference);
        if (name.starts_with("filled-new-array") &&
            (!ref.starts_with('[') || ref.substr(1) == "J" ||
             ref.substr(1) == "D"))
          bad("filled-new-array requires a single-word array component");
        if (name == "new-instance" && !ref.starts_with('L'))
          bad("new-instance requires class type");
        if (name == "new-array" && !ref.starts_with('['))
          bad("new-array requires array type");
        if ((name == "check-cast" || name == "instance-of") &&
            !ref.starts_with('L') && !ref.starts_with('['))
          bad("reference operation requires object type");
      }
      wideRegisters(name, regs, registers);
      lengths[uint32_t(pc)] = size;
      code.push_back(std::move(ins));
      pc += size;
    }
    std::set<uint32_t> used_payloads;
    for (auto &ins : code) {
      if (ins.opcode == "packed-switch" || ins.opcode == "sparse-switch" ||
          ins.opcode == "fill-array-data") {
        auto found = payloads.find(*ins.target);
        unsigned expected = ins.opcode == "packed-switch"   ? 0x100
                            : ins.opcode == "sparse-switch" ? 0x200
                                                            : 0x300;
        if (found == payloads.end() || found->second.ident != expected)
          bad("instruction has missing/mismatched payload");
        used_payloads.insert(*ins.target);
        auto &p = found->second;
        ins.keys = p.keys;
        ins.data = p.data;
        ins.element_width = p.element_width;
        for (auto delta : p.targets) {
          auto target = targetPC(ins.pc, delta);
          if (!lengths.contains(target))
            bad("switch target is not an instruction boundary");
          ins.targets.push_back(target);
        }
      } else if (ins.target) {
        if (!lengths.contains(*ins.target))
          bad("branch target is not an instruction boundary");
        if (*ins.target == ins.pc && ins.opcode != "goto/32")
          bad("zero branch displacement");
      }
    }
    if (used_payloads.size() != payloads.size())
      bad("unreferenced payload");
    return result;
  }
  void debugInfo(uint32_t offset, unsigned registers, uint32_t code_end,
                 size_t parameter_count) {
    if (!offset)
      return;
    cache.erase(Key{0x2003, offset});
    item<bool>(0x2003, offset, [&](Cursor &reader) {
      int64_t line = reader.leb(), count = reader.leb();
      if (line < 1 || uint64_t(count) != parameter_count)
        bad("debug header disagrees with method");
      auto optionalString = [&] {
        auto index = reader.leb() - 1;
        if (index >= 0)
          string(uint64_t(index));
      };
      for (int64_t i = 0; i < count; ++i)
        optionalString();
      uint64_t address = 0;
      while (true) {
        unsigned opcode = reader.u8();
        if (!opcode)
          break;
        if (opcode == 1)
          address += uint64_t(reader.leb());
        else if (opcode == 2)
          line += reader.leb(true);
        else if (opcode == 3 || opcode == 4) {
          if (uint64_t(reader.leb()) >= registers)
            bad("debug register outside frame");
          optionalString();
          auto typ = reader.leb() - 1;
          if (typ >= 0)
            type(uint64_t(typ));
          if (opcode == 4)
            optionalString();
        } else if (opcode == 5 || opcode == 6) {
          if (uint64_t(reader.leb()) >= registers)
            bad("debug register outside frame");
        } else if (opcode == 9)
          optionalString();
        else if (opcode >= 10) {
          address += (opcode - 10) / 15;
          line += int((opcode - 10) % 15) - 4;
        }
        if (address > code_end || line < 1)
          bad("debug position outside method");
      }
      return true;
    });
  }
  void codeFlow(const std::vector<Instruction> &instructions,
                const std::map<uint32_t, unsigned> &lengths,
                const std::vector<TryRegion> &tries) {
    std::map<uint32_t, const Instruction *> by_pc;
    std::set<uint32_t> handlers, explicit_targets;
    for (auto &region : tries)
      for (auto &handler : region.handlers)
        handlers.insert(handler.target);
    explicit_targets = handlers;
    for (auto &ins : instructions) {
      by_pc[ins.pc] = &ins;
      explicit_targets.insert(ins.targets.begin(), ins.targets.end());
      if (ins.opcode.starts_with("if-") || ins.opcode.starts_with("goto"))
        explicit_targets.insert(*ins.target);
    }
    const Instruction *previous = nullptr;
    for (auto &ins : instructions) {
      if (ins.opcode == "move-exception" && !handlers.contains(ins.pc))
        bad("move-exception outside an exception handler entry");
      if (ins.opcode.starts_with("move-result")) {
        if (explicit_targets.contains(ins.pc) || !previous ||
            previous->pc + lengths.at(previous->pc) != ins.pc)
          bad("move-result has an invalid control-flow predecessor");
        std::string result;
        if (previous->opcode.starts_with("invoke-") &&
            std::holds_alternative<MethodRef>(previous->reference))
          result = std::get<MethodRef>(previous->reference).returns;
        else if (previous->opcode.starts_with("filled-new-array"))
          result = std::get<std::string>(previous->reference);
        else
          bad("move-result does not immediately follow an invocation");
        auto expected = result == "J" || result == "D" ? "move-result-wide"
                        : result.starts_with('L') || result.starts_with('[')
                            ? "move-result-object"
                            : "move-result";
        if (result == "V" || ins.opcode != expected)
          bad("move-result kind disagrees with invocation result");
      }
      previous = &ins;
    }
    std::vector<std::pair<uint32_t, bool>> pending{{0, false}};
    std::set<uint32_t> reached;
    for (auto handler : handlers)
      pending.emplace_back(handler, true);
    while (!pending.empty()) {
      budget.tick();
      auto [pc, exception_edge] = pending.back();
      pending.pop_back();
      auto found = by_pc.find(pc);
      if (found == by_pc.end())
        bad("normal or handler execution falls outside executable "
            "instructions");
      auto &ins = *found->second;
      if (ins.opcode == "move-exception" && !exception_edge)
        bad("normal execution enters move-exception");
      if (!reached.insert(pc).second)
        continue;
      if (ins.opcode.starts_with("return") || ins.opcode == "throw")
        continue;
      if (ins.opcode.starts_with("goto"))
        pending.emplace_back(*ins.target, false);
      else {
        pending.emplace_back(pc + lengths.at(pc), false);
        if (ins.opcode.starts_with("if-"))
          pending.emplace_back(*ins.target, false);
        for (auto target : ins.targets)
          pending.emplace_back(target, false);
      }
    }
  }
  void code(uint32_t offset, Method &method) {
    context(0x2001, offset,
            method.reference.signature() +
                (has(method.access, "static") ? ":static" : ":instance"));
    auto result = item<Code>(
        0x2001, offset,
        [&](Cursor &reader) {
          unsigned registers = reader.u16(), incoming = reader.u16(),
                   outgoing = reader.u16(), tries_count = reader.u16();
          uint32_t debug = reader.u32(), size = reader.u32();
          if (!size || size > (reader.end - reader.pos) / 2)
            bad("empty/truncated method instructions");
          if (incoming != method.incomingWords() || incoming > registers)
            bad("incoming register words disagree with method");
          std::vector<uint16_t> words;
          words.reserve(size);
          for (uint32_t i = 0; i < size; ++i)
            words.push_back(reader.u16());
          auto [ins, lengths] = instructions(words, registers, outgoing);
          std::vector<std::tuple<uint32_t, uint32_t, unsigned>> raw_tries;
          if (tries_count && size % 2 && reader.u16())
            bad("nonzero try alignment padding");
          for (unsigned i = 0; i < tries_count; ++i) {
            uint32_t start = reader.u32();
            unsigned length = reader.u16(), handler = reader.u16();
            uint64_t end = uint64_t(start) + length;
            if (!length || !lengths.contains(start) || end > size ||
                (end != size && !lengths.contains(uint32_t(end))) ||
                (!raw_tries.empty() && start < std::get<1>(raw_tries.back())))
              bad("invalid or overlapping try range");
            raw_tries.emplace_back(start, uint32_t(end), handler);
          }
          std::map<size_t, std::vector<Handler>> handlers;
          if (tries_count) {
            size_t base = reader.pos;
            auto count = reader.leb();
            budget.tick(uint64_t(count));
            for (int64_t i = 0; i < count; ++i) {
              size_t handler_offset = reader.pos - base;
              int64_t length = reader.leb(true);
              uint64_t magnitude = uint64_t(length < 0 ? -length : length);
              budget.tick(magnitude);
              std::vector<Handler> entries;
              std::set<std::string> caught_types;
              for (uint64_t j = 0; j < magnitude; ++j) {
                auto typ = type(uint64_t(reader.leb()));
                auto target = uint32_t(reader.leb());
                if (!typ.starts_with('L') || !lengths.contains(target) ||
                    !caught_types.insert(typ).second)
                  bad("invalid or duplicate typed exception handler");
                entries.push_back({typ, target});
              }
              if (length <= 0) {
                auto target = uint32_t(reader.leb());
                if (!lengths.contains(target))
                  bad("catch-all target is not an instruction");
                entries.push_back({std::nullopt, target});
              }
              handlers.emplace(handler_offset, std::move(entries));
            }
          }
          std::vector<TryRegion> tries;
          for (auto [start, end, handler] : raw_tries) {
            auto found = handlers.find(handler);
            if (found == handlers.end())
              bad("try references non-handler offset");
            tries.push_back({start, end, found->second});
          }
          codeFlow(ins, lengths, tries);
          debugInfo(debug, registers, size, method.reference.parameters.size());
          return Code{registers, incoming, std::move(ins), std::move(tries),
                      size};
        },
        4);
    if (result.incoming != method.incomingWords())
      bad("shared code item signature mismatch");
    method.registers = result.registers;
    method.instructions = std::move(result.instructions);
    method.tries = std::move(result.tries);
    method.code_end = result.size;
  }
  void classData(Class &cls, uint32_t offset) {
    if (!offset)
      return;
    context(0x2000, offset, cls.name);
    item<bool>(0x2000, offset, [&](Cursor &reader) {
      std::array<uint64_t, 4> counts{};
      uint64_t total = 0;
      for (auto &count : counts) {
        count = uint64_t(reader.leb());
        total += count;
      }
      budget.tick(total);
      std::set<uint64_t> seen_fields, seen_methods;
      for (unsigned group = 0; group < 4; ++group) {
        uint64_t index = 0;
        for (uint64_t member_index = 0; member_index < counts[group];
             ++member_index) {
          auto diff = uint64_t(reader.leb());
          auto flags = uint32_t(reader.leb());
          if (member_index && !diff)
            bad("duplicate class-data member index");
          index += diff;
          auto access = accessFlags(flags);
          auto &seen = group < 2 ? seen_fields : seen_methods;
          if (!seen.insert(index).second)
            bad("duplicate defined member");
          if (group < 2) {
            auto reference = at(fields, index, "defined member");
            if (reference.owner != cls.name)
              bad("class-data member owner mismatch");
            if (has(access, "static") != (group == 0))
              bad("field storage/access mismatch");
            cls.fields.push_back({reference, std::move(access), {}});
          } else {
            auto reference = at(methods, index, "defined member");
            if (reference.owner != cls.name)
              bad("class-data member owner mismatch");
            if (access.erase("volatile"))
              access.insert("bridge");
            if (access.erase("transient"))
              access.insert("varargs");
            bool direct = has(access, "static") || has(access, "private") ||
                          has(access, "constructor") ||
                          reference.name == "<init>" ||
                          reference.name == "<clinit>";
            if (direct != (group == 2))
              bad("direct/virtual method classification mismatch");
            uint32_t code_off = uint32_t(reader.leb());
            bool no_body = has(access, "native") || has(access, "abstract");
            if (bool(code_off) == no_body)
              bad("method code/access mismatch");
            Method method;
            method.reference = std::move(reference);
            method.access = std::move(access);
            if (code_off)
              code(code_off, method);
            cls.methods.push_back(std::move(method));
          }
        }
      }
      return true;
    });
  }
  void staticValues(Class &cls, uint32_t offset) {
    if (!offset)
      return;
    auto values =
        item<std::vector<Encoded>>(0x2005, offset, [&](Cursor &reader) {
          auto count = reader.leb();
          budget.tick(uint64_t(count));
          std::vector<Encoded> result;
          for (int64_t i = 0; i < count; ++i)
            result.push_back(encoded(reader));
          return result;
        });
    std::vector<Field *> static_fields;
    for (auto &field : cls.fields)
      if (has(field.access, "static"))
        static_fields.push_back(&field);
    if (values.size() > static_fields.size())
      bad("static values exceed static field count");
    static const std::map<std::string, unsigned> kinds{
        {"B", 0}, {"S", 2},    {"C", 3},    {"I", 4},
        {"J", 6}, {"F", 0x10}, {"D", 0x11}, {"Z", 0x1f}};
    for (size_t i = 0; i < values.size(); ++i) {
      auto &field = *static_fields[i];
      auto &value = values[i];
      auto &typ = field.reference.type;
      if (auto found = kinds.find(typ); found != kinds.end()) {
        if (value.kind != found->second)
          bad("static value type disagrees with field");
      } else if (value.kind == 0x17) {
        if (typ != "Ljava/lang/String;")
          bad("string static value has incompatible field");
      } else if (value.kind != 0x1e)
        bad("unsupported reference static initializer");
      field.value = value.value;
    }
  }

public:
  Dex(std::string_view data, std::string_view source_id, Budget &budget)
      : data(data), source_id(source_id), budget(budget) {}
  std::vector<Class> parse() {
    auto tables = header();
    readTables(tables);
    auto [count, offset] = tables.at(6);
    if (count > budget.limits.max_files)
      bad("class inventory exceeds file limit");
    auto reader = cursor(offset, count ? sections.at(6).end : offset);
    std::vector<Class> classes;
    std::set<std::string> names;
    for (uint32_t i = 0; i < count; ++i) {
      budget.tick();
      uint32_t index = reader.u32(), flags = reader.u32(),
               parent = reader.u32(), interfaces = reader.u32();
      uint32_t source = reader.u32(), annotation_off = reader.u32(),
               class_data = reader.u32(), values = reader.u32();
      auto name = type(index);
      if (!name.starts_with('L') || !names.insert(name).second)
        bad("invalid/duplicate class definition");
      Class cls;
      cls.name = name;
      cls.access = accessFlags(flags);
      cls.source_id = source_id;
      if (parent != UINT32_MAX) {
        cls.superclass = type(parent);
        if (!cls.superclass->starts_with('L') || *cls.superclass == name)
          bad("invalid superclass");
      }
      cls.interfaces = typeList(interfaces);
      std::set<std::string> implemented;
      for (auto &typ : cls.interfaces)
        if (!typ.starts_with('L') || !implemented.insert(typ).second)
          bad("invalid/duplicate interface");
      if (source != UINT32_MAX)
        string(source);
      classData(cls, class_data);
      staticValues(cls, values);
      annotations(cls, annotation_off);
      classes.push_back(std::move(cls));
    }
    std::map<std::string, const Class *> by_name;
    for (auto &cls : classes)
      by_name[cls.name] = &cls;
    for (auto &[owner, children] : members)
      for (auto &name : children) {
        auto found = by_name.find(name);
        if (found != by_name.end() && found->second->enclosing != owner)
          bad("MemberClasses/EnclosingClass disagreement");
      }
    if (auto found = sections.find(0x1003); found != sections.end()) {
      auto section = found->second;
      size_t at = section.start;
      for (uint32_t i = 0; i < section.count; ++i) {
        annotationSet(uint32_t(at));
        at = (ranges.at(Key{0x1003, at}) + 3) & ~size_t(3);
        if (at > section.end)
          bad("annotation set exceeds mapped section");
      }
    }
    for (auto &[kind, section] : sections) {
      if (kind <= 0x1000)
        continue;
      std::vector<std::pair<size_t, size_t>> items;
      for (auto &[key, last] : ranges)
        if (key.first == kind)
          items.emplace_back(key.second, last);
      if (items.size() != section.count || items.empty() ||
          items.front().first != section.start)
        bad("mapped item inventory mismatch");
      for (size_t i = 1; i < items.size(); ++i)
        if (items[i - 1].second > items[i].first)
          bad("overlapping variable-length data items");
    }
    return classes;
  }
};
} // namespace
std::vector<Class> parseDex(std::string_view bytes, std::string_view input_id,
                            Budget &budget) {
  return Dex(bytes, input_id, budget).parse();
}
} // namespace neverd::mobile::dalvik
