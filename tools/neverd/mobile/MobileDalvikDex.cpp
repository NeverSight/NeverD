//===- MobileDalvikDex.cpp - Bounded standard DEX reader
//-------------------===//
#include "MobileDalvik.h"
#include "MobileDalvikAccess.h"
#include "MobileDalvikIdentity.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/SHA1.h"

#include <algorithm>
#include <any>
#include <array>
#include <bit>
#include <functional>
#include <limits>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <zlib.h>

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
// Share deadline checkpoints across bounded operations, including nested
// readers and short methods. Every operation still debits its work immediately.
void boundedWork(Budget &budget, unsigned &steps, uint64_t count = 1) {
  if (count > 128 || ++steps >= 128) {
    budget.tick(count);
    steps = 0;
  } else
    budget.consumeWork(count);
}
struct Cursor {
  std::string_view data;
  size_t pos, end;
  Budget &budget;
  unsigned &small_reads;
  Cursor(std::string_view data, size_t offset, size_t end, Budget &budget,
         unsigned &small_reads)
      : data(data), pos(offset), end(end), budget(budget),
        small_reads(small_reads) {
    if (offset > end || end > data.size())
      bad("item lies outside its section");
  }
  std::string_view take(size_t size) {
    // Check the first read, then at most 128 bounded scalar reads apart
    // across this DEX's cursors, including nested readers.
    // Larger reads always check; work limits still apply to every read.
    if (size > 16 || ++small_reads >= 128) {
      budget.tick(1 + size / 16);
      small_reads = 0;
    } else
      budget.consumeWork(1 + size / 16);
    if (size > end - pos)
      bad("truncated item");
    auto result = data.substr(pos, size);
    pos += size;
    return result;
  }
  std::string_view takeCodeUnits(uint32_t count) {
    // Match count separate u16() reads without a clock call per code unit.
    boundedWork(budget, small_reads, count);
    if (count > (end - pos) / 2)
      bad("truncated item");
    const auto bytes = size_t(count) * 2;
    auto result = data.substr(pos, bytes);
    pos += bytes;
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
constexpr uint32_t formatCode(std::string_view form) {
  return (uint32_t(uint8_t(form[0])) << 16) |
         (uint32_t(uint8_t(form[1])) << 8) | uint8_t(form[2]);
}
enum class Format : uint32_t {
  F10t = formatCode("10t"),
  F10x = formatCode("10x"),
  F11n = formatCode("11n"),
  F11x = formatCode("11x"),
  F12x = formatCode("12x"),
  F20t = formatCode("20t"),
  F21c = formatCode("21c"),
  F21h = formatCode("21h"),
  F21s = formatCode("21s"),
  F21t = formatCode("21t"),
  F22b = formatCode("22b"),
  F22c = formatCode("22c"),
  F22s = formatCode("22s"),
  F22t = formatCode("22t"),
  F22x = formatCode("22x"),
  F23x = formatCode("23x"),
  F30t = formatCode("30t"),
  F31c = formatCode("31c"),
  F31i = formatCode("31i"),
  F31t = formatCode("31t"),
  F32x = formatCode("32x"),
  F35c = formatCode("35c"),
  F3rc = formatCode("3rc"),
  F51l = formatCode("51l"),
};
struct WideOperands {
  unsigned mask = 0;
  bool all = false, except_last = false;
};
WideOperands wideOperands(std::string_view name) {
  WideOperands result;
  const auto base = name.substr(0, name.find('/'));
  if (base == "move-wide")
    result.mask = 3;
  else if (base == "move-result-wide" || base == "return-wide" ||
           base == "const-wide" || base == "aget-wide" || base == "aput-wide" ||
           base == "iget-wide" || base == "iput-wide" || base == "sget-wide" ||
           base == "sput-wide")
    result.mask = 1;
  else if (auto at = base.find("-to-"); at != std::string_view::npos) {
    const auto source = base.substr(0, at), dest = base.substr(at + 4);
    if (dest == "long" || dest == "double")
      result.mask |= 1;
    if (source == "long" || source == "double")
      result.mask |= 2;
  } else if (base == "cmp-long" || base == "cmpl-double" ||
             base == "cmpg-double")
    result.mask = 6;
  else if (base.ends_with("-long") || base.ends_with("-double")) {
    result.all = true;
    result.except_last = base.starts_with("shl-") || base.starts_with("shr-") ||
                         base.starts_with("ushr-");
  }
  return result;
}
enum class ResultKind : uint8_t { None, Void, Scalar, Wide, Object };
enum class FlowControl : uint8_t { Fallthrough, Conditional, Goto, Stop };
enum class ProducerKind : uint8_t { None, Method, Array };
struct FlowProperties {
  FlowControl control = FlowControl::Fallthrough;
  ResultKind result_use = ResultKind::None;
  ProducerKind producer = ProducerKind::None;
  uint16_t payload_ident = 0;
  bool move_exception = false, zero_branch = false;
};
FlowProperties flowProperties(std::string_view name) {
  FlowProperties result;
  if (name.starts_with("if-"))
    result.control = FlowControl::Conditional;
  else if (name.starts_with("goto"))
    result.control = FlowControl::Goto;
  else if (name.starts_with("return") || name == "throw")
    result.control = FlowControl::Stop;
  if (name == "move-result")
    result.result_use = ResultKind::Scalar;
  else if (name == "move-result-wide")
    result.result_use = ResultKind::Wide;
  else if (name == "move-result-object")
    result.result_use = ResultKind::Object;
  result.move_exception = name == "move-exception";
  result.zero_branch = name == "goto/32";
  if (name.starts_with("invoke-"))
    result.producer = ProducerKind::Method;
  else if (name.starts_with("filled-new-array"))
    result.producer = ProducerKind::Array;
  if (name == "packed-switch")
    result.payload_ident = 0x100;
  else if (name == "sparse-switch")
    result.payload_ident = 0x200;
  else if (name == "fill-array-data")
    result.payload_ident = 0x300;
  return result;
}
ResultKind resultKind(std::string_view descriptor) {
  if (descriptor == "V")
    return ResultKind::Void;
  if (descriptor == "J" || descriptor == "D")
    return ResultKind::Wide;
  if (descriptor.starts_with('L') || descriptor.starts_with('['))
    return ResultKind::Object;
  return ResultKind::Scalar;
}
// Decoder-local targets begin as PCs. Before instructions() returns, branch
// and switch edges become ordinals from its authoritative boundary index;
// public Instruction targets remain PCs. Payload pointers are decode-only.
struct FlowInstruction {
  uint32_t pc = 0;
  uint8_t opcode = 0, width = 0;
  ResultKind produced = ResultKind::None;
  std::optional<uint32_t> target;
  std::vector<uint32_t> targets;
};
struct OpSpec {
  std::string name;
  Format form{};
  unsigned size = 0;
  WideOperands wide;
  char pool = 0;
  bool reserved_high = false;
  FlowProperties flow;
};
const std::array<OpSpec, 256> &opcodes() {
  static const auto table = [] {
    std::array<OpSpec, 256> ops{};
    auto put = [&](unsigned code, std::string name, std::string form,
                   char pool = 0) {
      if (form.size() != 3)
        bad("invalid instruction format label");
      const auto format = static_cast<Format>(formatCode(form));
      ops[code] = {name,
                   format,
                   unsigned(form[0] - '0'),
                   wideOperands(name),
                   pool,
                   format == Format::F10x || format == Format::F20t ||
                       format == Format::F30t || format == Format::F32x,
                   flowProperties(name)};
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
using ItemKey = std::pair<unsigned, size_t>;
using ItemRanges = std::map<ItemKey, size_t>;
struct QueryRange {
  uint32_t start, end, order;
};
struct Section {
  size_t start, end;
  uint32_t count;
  std::optional<ItemRanges::iterator> final_range;
  std::vector<QueryRange> query_ranges;
  bool ranges_ordered = true;
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
struct CodeReference {
  uint32_t pc, index;
  uint8_t opcode;
  char pool;
};
struct CodeUnits {
  std::string_view bytes;
  size_t size() const { return bytes.size() / 2; }
  uint16_t operator[](size_t index) const {
    const auto offset = index * 2;
    return uint16_t(uint8_t(bytes[offset])) |
           (uint16_t(uint8_t(bytes[offset + 1])) << 8);
  }
};
// Private views borrow only completed immutable Dex-owned tables. Public
// models are materialized explicitly and retain no references to this storage.
struct FieldPoolEntry {
  const std::string &owner, &name, &type;
};
struct TypeListEntry {
  std::vector<std::string> names;
  std::vector<unsigned> indices;
};
struct PrototypePoolEntry {
  const std::vector<std::string> &parameters;
  const std::string &returns;
};
struct MethodPoolEntry {
  const std::string &owner, &name;
  const std::vector<std::string> &parameters;
  const std::string &returns;
  uint16_t prototype;
  std::string identity() const {
    return detail::methodIdentity(owner, name, parameters, returns);
  }
};
class Dex {
  std::string_view data;
  std::string source_id;
  Budget &budget;
  unsigned scalar_reads = 127;
  using Key = ItemKey;
  std::map<unsigned, Section> sections;
  std::map<Key, std::any> cache;
  ItemRanges ranges;
  llvm::SmallVector<Key, 8> active;
  std::map<Key, std::string> contexts;
  std::map<uint32_t, uint32_t> code_contexts;
  std::vector<std::string> strings, types;
  std::map<std::string, unsigned> type_indices;
  std::vector<PrototypePoolEntry> protos;
  std::vector<FieldPoolEntry> fields;
  std::vector<MethodPoolEntry> methods;
  std::map<std::string, std::vector<std::string>> members;
  struct Match {
    std::string text;
    std::optional<std::u16string> utf16;
  };
  using ReferenceMatches = std::map<uint32_t, Match>;
  char reference_pool = 0;
  const ReferenceMatches *reference_matches = nullptr;
  uint64_t *query_retained = nullptr;
  uint64_t query_persistent = 0;
  void retainQueryStorage(uint64_t bytes) {
    if (!query_retained)
      return;
    if (bytes > budget.limits.max_bytes - *query_retained)
      throw Error("reference query storage exceeds byte limit");
    *query_retained += bytes;
  }
  void retainQueryPersistent(uint64_t bytes) {
    if (!query_retained)
      return;
    retainQueryStorage(bytes);
    query_persistent += bytes;
  }
  template <class T>
  void queryReserve(std::vector<T> &values, size_t count, bool exact = false) {
    if (!query_retained || count <= values.capacity())
      return;
    const auto maximum = values.max_size();
    if (count > maximum)
      throw Error("reference query storage exceeds byte limit");
    const size_t capacity =
        exact
            ? count
            : std::max(count, values.capacity() > maximum / 2
                                  ? maximum
                                  : std::max<size_t>(1, values.capacity() * 2));
    const uint64_t previous = uint64_t(values.capacity()) * sizeof(T);
    retainQueryStorage(uint64_t(capacity) * sizeof(T));
    values.reserve(capacity);
    *query_retained -= previous;
  }
  template <class Container> static constexpr uint64_t queryNodeBytes() {
    // Cover tree links, color/padding, and value alignment before allocation.
    return sizeof(typename Container::value_type) + 4 * sizeof(void *) +
           alignof(typename Container::value_type);
  }
  template <class Set, class Value>
  auto queryInsert(Set &values, const Value &value) {
    if (!query_retained)
      return values.insert(value);
    const auto position = values.lower_bound(value);
    if (position != values.end() && !values.key_comp()(value, *position))
      return std::pair{position, false};
    retainQueryStorage(queryNodeBytes<Set>());
    return std::pair{values.emplace_hint(position, value), true};
  }
  struct QueryTemporary {
    uint64_t *total;
    uint64_t bytes = 0;
    ~QueryTemporary() {
      if (total)
        *total -= bytes;
    }
  };
  void queryCopyWork(uint64_t bytes) {
    // Cursor and instruction loops already charge fixed-size construction.
    // Charge separately only deep text copies whose size can be amplified by
    // repeated pool references, without checking the clock for every field.
    if (query_retained && bytes >= 16) {
      // Small copies are bounded by the surrounding scalar/instruction loop's
      // checkpoints. Large copies always check before allocating or copying.
      if (bytes < 2048)
        budget.consumeWork(bytes / 16);
      else
        budget.tick(bytes / 16);
    }
  }
  void retainStrings(const std::vector<std::string> &values) {
    if (!query_retained)
      return;
    retainQueryStorage(uint64_t(values.size()) * sizeof(std::string));
    uint64_t text_bytes = 0;
    for (const auto &value : values)
      text_bytes += value.size();
    retainQueryStorage(text_bytes);
    queryCopyWork(text_bytes);
  }
  template <class MethodReference>
  void retainMethod(const MethodReference &method) {
    if (!query_retained)
      return;
    retainQueryStorage(sizeof(MethodRef));
    retainQueryStorage(method.owner.size());
    retainQueryStorage(method.name.size());
    retainQueryStorage(method.returns.size());
    retainStrings(method.parameters);
    queryCopyWork(method.owner.size() + method.name.size() +
                  method.returns.size());
  }
  MethodRef materializeMethod(const MethodPoolEntry &method) {
    retainMethod(method);
    return {method.owner, method.name, method.parameters, method.returns};
  }
  FieldRef materializeField(const FieldPoolEntry &field) {
    retainQueryStorage(sizeof(FieldRef) + field.owner.size() +
                       field.name.size() + field.type.size());
    queryCopyWork(field.owner.size() + field.name.size() + field.type.size());
    return {field.owner, field.name, field.type};
  }
  template <class T> void retainItemCopy(const T &value) {
    if constexpr (std::is_same_v<T, DecodedString>) {
      retainQueryStorage(sizeof(DecodedString));
      retainQueryStorage(value.text.size());
      retainQueryStorage(uint64_t(value.units.size()) * sizeof(char16_t));
      queryCopyWork(value.text.size() +
                    uint64_t(value.units.size()) * sizeof(char16_t));
    } else if constexpr (std::is_same_v<T, std::vector<std::string>>)
      retainStrings(value);
  }
  Cursor cursor(size_t offset) {
    return Cursor(data, offset, data.size(), budget, scalar_reads);
  }
  Cursor cursor(size_t offset, size_t end) {
    return Cursor(data, offset, end, budget, scalar_reads);
  }
  template <class T>
  const T &at(const std::vector<T> &values, uint64_t index,
              std::string_view name) {
    if (index >= values.size())
      bad(std::string(name) + " index out of bounds");
    return values[size_t(index)];
  }
  std::string string(uint64_t index) { return at(strings, index, "string"); }
  const std::string &typeValue(uint64_t index, bool allow_void = false) {
    const auto &result = at(types, index, "type");
    if (result == "V" && !allow_void)
      bad("void outside return type");
    return result;
  }
  std::string type(uint64_t index, bool allow_void = false) {
    return typeValue(index, allow_void);
  }
  template <class T, class F>
  T item(unsigned kind, size_t offset, F read, unsigned alignment = 1,
         bool cache_result = true) {
    Key key{kind, offset};
    if (cache_result) {
      if (auto found = cache.find(key); found != cache.end()) {
        const auto &value = std::any_cast<const T &>(found->second);
        retainItemCopy(value);
        return value;
      }
    }
    if (std::find(active.begin(), active.end(), key) != active.end())
      bad("cyclic data item reference");
    auto section = sections.find(kind);
    if (section == sections.end() || offset % alignment ||
        offset < section->second.start || offset >= section->second.end)
      bad("item points outside its mapped section");
    auto reader = cursor(offset, section->second.end);
    active.push_back(key);
    struct ActiveItem {
      llvm::SmallVector<Key, 8> &items;
      ~ActiveItem() { items.pop_back(); }
    } active_item{active};
    T result = read(reader);
    recordRange(key, reader.pos);
    if (cache_result) {
      retainItemCopy(result);
      // Nested item reads may populate the cache; obtain the insertion
      // hint only after the callback has finished.
      const auto position = cache.lower_bound(key);
      if (position != cache.end() && position->first == key)
        position->second = result;
      else {
        retainQueryPersistent(sizeof(decltype(cache)::value_type) +
                              4 * sizeof(void *));
        cache.emplace_hint(position, key, result);
      }
    }
    return result;
  }
  void recordRange(const Key &key, size_t end) {
    if (query_retained) {
      auto &section = sections.at(key.first);
      auto &visits = section.query_ranges;
      // The standard header has bounded every offset and end to uint32_t.
      if (visits.size() >= UINT32_MAX)
        bad("too many variable-length data item visits");
      const auto previous = visits.capacity();
      queryReserve(visits, visits.size() + 1);
      query_persistent +=
          uint64_t(visits.capacity() - previous) * sizeof(QueryRange);
      if (!visits.empty() && visits.back().start > key.second)
        section.ranges_ordered = false;
      visits.push_back(
          {uint32_t(key.second), uint32_t(end), uint32_t(visits.size())});
      return;
    }
    auto &last = sections.at(key.first).final_range;
    // Each section normally arrives in file order, even while item readers
    // interleave sections. The final node gives an exact insertion hint; an
    // out-of-order read still uses the ordered lookup and duplicate check.
    const auto found =
        last && (*last)->first.second <= key.second
            ? ((*last)->first.second == key.second ? *last : std::next(*last))
            : ranges.lower_bound(key);
    if (found != ranges.end() && found->first == key) {
      found->second = end;
      return;
    }
    retainQueryPersistent(sizeof(decltype(ranges)::value_type) +
                          4 * sizeof(void *));
    auto inserted = ranges.emplace_hint(found, key, end);
    if (!last || (*last)->first.second < key.second)
      last = inserted;
  }
  void checkRangeOverlaps() {
    if (query_retained) {
      size_t comparisons = 0;
      budget.check();
      for (auto &[kind, section] : sections) {
        auto &visits = section.query_ranges;
        // File-order sections need no sorting. Shared and out-of-order visits
        // retain their last recorded extent, matching ordered-map assignment.
        if (!section.ranges_ordered)
          std::sort(visits.begin(), visits.end(),
                    [&](const QueryRange &a, const QueryRange &b) {
                      if (!(comparisons++ % 4096))
                        budget.check();
                      return std::tie(a.start, a.order) <
                             std::tie(b.start, b.order);
                    });
        const QueryRange *previous = nullptr;
        for (size_t i = 0; i < visits.size(); ++i) {
          if (!(i % 4096))
            budget.check();
          const auto &range = visits[i];
          if (i + 1 < visits.size() && visits[i + 1].start == range.start)
            continue;
          if (previous && previous->end > range.start)
            bad("overlapping variable-length data items");
          previous = &range;
        }
      }
    } else {
      std::optional<std::pair<Key, size_t>> previous;
      for (const auto &[key, end] : ranges) {
        if (previous && previous->first.first == key.first &&
            previous->second > key.second)
          bad("overlapping variable-length data items");
        previous = std::pair{key, end};
      }
    }
  }
  void context(unsigned kind, size_t offset, const std::string &identity) {
    const Key key{kind, offset};
    const auto found = contexts.lower_bound(key);
    if (found != contexts.end() && found->first == key) {
      if (found->second != identity)
        bad("shared data item has inconsistent declaration context");
      return;
    }
    retainQueryPersistent(sizeof(decltype(contexts)::value_type) +
                          4 * sizeof(void *) + identity.size());
    contexts.emplace_hint(found, key, identity);
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
    uLong actual_checksum = adler32(0, Z_NULL, 0);
    for (size_t start = 12; start < data.size();) {
      const auto length = std::min<size_t>(5552, data.size() - start);
      budget.tick();
      actual_checksum = adler32(
          actual_checksum, reinterpret_cast<const Bytef *>(data.data() + start),
          static_cast<uInt>(length));
      start += length;
    }
    if (actual_checksum != checksum)
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
    // One decoded unit needs at most three WTF-8 bytes and two UTF-16 bytes.
    // Reserve the logical expansion before decoding; item() accounts its copy.
    retainQueryStorage(sizeof(DecodedString) + uint64_t(expected) * 5);
    DecodedString result;
    const size_t text_start = reader.pos;
    bool ascii = true;
    while (true) {
      // ASCII consumes one UTF-16 unit per byte. Keep the same byte-work
      // charge while checking the deadline at bounded run boundaries.
      size_t limit = std::min<size_t>(4096, reader.end - reader.pos);
      if (budget.remaining < limit)
        limit = size_t(budget.remaining);
      if (expected - result.units.size() < limit)
        limit = expected - result.units.size() + 1;
      size_t run = 0;
      while (run < limit) {
        const auto byte = uint8_t(reader.data[reader.pos + run]);
        if (!byte || byte >= 0x80)
          break;
        ++run;
      }
      if (run) {
        // Short ASCII runs are bounded by the shared scalar-read checkpoint
        // (length prefix and terminator); longer runs check independently.
        if (run < 128)
          budget.consumeWork(run);
        else
          budget.tick(run);
        result.units.append(reader.data.begin() + reader.pos,
                            reader.data.begin() + reader.pos + run);
        reader.pos += run;
        if (result.units.size() > expected)
          bad("UTF-16 string length mismatch");
        continue;
      }
      unsigned first = reader.u8(), unit;
      if (!first)
        break;
      ascii &= first < 0x80;
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
    if (ascii) {
      result.text.assign(reader.data.substr(text_start, expected));
      return result;
    }
    for (size_t i = 0; i < result.units.size(); ++i) {
      uint32_t cp = result.units[i];
      if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < result.units.size() &&
          result.units[i + 1] >= 0xdc00 && result.units[i + 1] <= 0xdfff)
        cp = 0x10000 + ((cp - 0xd800) << 10) + (result.units[++i] - 0xdc00);
      appendUTF8(result.text, cp);
    }
    return result;
  }
  const TypeListEntry &typeListEntry(uint32_t offset) {
    static const TypeListEntry empty;
    if (!offset)
      return empty;
    // The item cache owns the immutable list. Prototypes borrow its names and
    // compare its original type IDs without reconstructing a reverse index.
    const auto entry = item<std::shared_ptr<TypeListEntry>>(
        0x1001, offset,
        [&](Cursor &reader) {
          auto size = reader.u32();
          if (size > (reader.end - reader.pos) / 2)
            bad("truncated type list");
          retainQueryStorage(sizeof(TypeListEntry) + 4 * sizeof(void *));
          auto result = std::make_shared<TypeListEntry>();
          queryReserve(result->names, size, true);
          queryReserve(result->indices, size, true);
          result->names.reserve(size);
          result->indices.reserve(size);
          for (uint32_t i = 0; i < size; ++i) {
            const auto index = reader.u16();
            const auto &value = typeValue(index);
            retainQueryStorage(value.size());
            queryCopyWork(value.size());
            result->names.push_back(value);
            result->indices.push_back(index);
          }
          return result;
        },
        4);
    return *entry;
  }
  std::vector<std::string> typeList(uint32_t offset) {
    const auto &entry = typeListEntry(offset);
    retainStrings(entry.names);
    return entry.names;
  }
  void readTables(const Tables &tables) {
    auto records = [&](unsigned kind) {
      auto [count, offset] = tables.at(kind);
      return cursor(offset, count ? sections.at(kind).end : offset);
    };
    auto reader = records(1);
    const auto string_count = tables.at(1).first;
    retainQueryStorage(uint64_t(string_count) * sizeof(std::string));
    strings.reserve(string_count);
    std::optional<std::u16string> previous_string;
    for (uint32_t i = 0; i < string_count; ++i) {
      auto value = item<DecodedString>(
          0x2002, reader.u32(), [&](Cursor &r) { return mutf8(r); }, 1, false);
      if (previous_string && *previous_string >= value.units)
        bad("string IDs are duplicate or unordered");
      previous_string = std::move(value.units);
      strings.push_back(std::move(value.text));
    }
    auto type_reader = records(2);
    const auto type_count = tables.at(2).first;
    retainQueryStorage(uint64_t(type_count) * sizeof(std::string));
    types.reserve(type_count);
    int64_t previous = -1;
    for (uint32_t i = 0; i < type_count; ++i) {
      uint32_t index = type_reader.u32();
      if (int64_t(index) <= previous)
        bad("type IDs are duplicate or unordered");
      previous = index;
      const auto &value = at(strings, index, "string");
      retainQueryStorage(value.size());
      queryCopyWork(value.size());
      types.push_back(descriptor(value, true));
      if (!query_retained)
        type_indices[types.back()] = unsigned(types.size() - 1);
    }
    auto proto_reader = records(3);
    const auto proto_count = tables.at(3).first;
    retainQueryStorage(uint64_t(proto_count) * sizeof(PrototypePoolEntry));
    protos.reserve(proto_count);
    uint32_t previous_return = 0;
    const std::vector<unsigned> *previous_parameters = nullptr;
    for (uint32_t i = 0; i < proto_count; ++i) {
      uint32_t shorty = proto_reader.u32(), result = proto_reader.u32(),
               parameters = proto_reader.u32();
      const auto &return_type = typeValue(result, true);
      const auto &args = typeListEntry(parameters);
      auto shortType = [](const std::string &t) {
        return t.starts_with('L') || t.starts_with('[') ? "L" : t;
      };
      std::string expected = shortType(return_type);
      for (auto &arg : args.names) {
        expected += shortType(arg);
      }
      if (string(shorty) != expected)
        bad("prototype shorty disagrees with descriptors");
      if (previous_parameters &&
          (result < previous_return ||
           (result == previous_return && args.indices <= *previous_parameters)))
        bad("prototype IDs are duplicate or unordered");
      previous_return = result;
      previous_parameters = &args.indices;
      protos.push_back({args.names, return_type});
    }
    for (unsigned kind : {4, 5}) {
      auto r = records(kind);
      const auto count = tables.at(kind).first;
      if (kind == 4) {
        retainQueryStorage(uint64_t(count) * sizeof(FieldPoolEntry));
        fields.reserve(count);
      } else {
        retainQueryStorage(uint64_t(count) * sizeof(MethodPoolEntry));
        methods.reserve(count);
      }
      std::optional<std::tuple<unsigned, uint32_t, unsigned>> previous_member;
      for (uint32_t i = 0; i < count; ++i) {
        unsigned owner = r.u16(), typ = r.u16();
        uint32_t name = r.u32();
        const auto &owner_name = typeValue(owner);
        const auto &member = at(strings, name, "string");
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
        if (kind == 4) {
          const auto &field_type = typeValue(typ);
          fields.push_back({owner_name, member, field_type});
        } else {
          const auto &[args, result] = at(protos, typ, "prototype");
          methods.push_back({owner_name, member, args, result, uint16_t(typ)});
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
  bool suppressLint(const AnnotationItem &entry,
                    std::optional<std::vector<std::string>> &result,
                    const std::string &declaration) {
    const auto &[name, elements] = entry.value;
    if (name != "Landroid/annotation/SuppressLint;")
      return false;
    if (entry.visibility != 0)
      bad("SuppressLint annotation on " + declaration +
          " requires build visibility");
    if (result)
      bad("duplicate SuppressLint annotation on " + declaration);
    auto value = elements.find("value");
    if (elements.size() != 1 || value == elements.end() ||
        value->second.kind != 0x1c)
      bad("SuppressLint annotation on " + declaration +
          " requires exactly one string-array value");
    std::vector<std::string> values;
    for (const auto &part : value->second.array) {
      budget.tick();
      if (part.kind != 0x17)
        bad("SuppressLint value is not a string on " + declaration);
      values.push_back(std::get<std::string>(part.value));
    }
    result = std::move(values);
    return true;
  }
  void sourceAnnotations(const AnnotationSet &values,
                         std::optional<std::string> &signature,
                         std::optional<std::vector<std::string>> *throws_types,
                         bool &deprecated,
                         std::optional<std::vector<std::string>> &suppress_lint,
                         const std::string &declaration) {
    for (const auto &entry : values) {
      budget.tick();
      const auto &[name, elements] = entry.value;
      if (suppressLint(entry, suppress_lint, declaration))
        continue;
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
      metadata.retention =
          enumValue(value->second, "Ljava/lang/annotation/RetentionPolicy;");
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
          auto found = defined_fields.find(materializeField(ref));
          if (found == defined_fields.end())
            bad("annotated field has no class_data definition");
          sourceAnnotations(
              annotationSet(annotation_off), found->second->generic_signature,
              nullptr, found->second->deprecated, found->second->suppress_lint,
              "field " + ref.owner + "->" + ref.name + ":" + ref.type);
          continue;
        }
        const auto &ref = at(methods, index, "annotated method");
        if (ref.owner != cls.name)
          bad("annotation directory owner mismatch");
        auto found = defined_methods.find(materializeMethod(ref));
        if (found == defined_methods.end())
          bad("annotated method has no class_data definition");
        if (kind == 1) {
          sourceAnnotations(
              annotationSet(annotation_off), found->second->generic_signature,
              &found->second->declared_throws, found->second->deprecated,
              found->second->suppress_lint, "method " + ref.identity());
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
          name == "Ljava/lang/Deprecated;" ||
          name == "Landroid/annotation/SuppressLint;") {
        sourceAnnotations({entry}, cls.generic_signature, nullptr,
                          cls.deprecated, cls.suppress_lint,
                          "class " + cls.name);
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
  Payload payload(CodeUnits words, size_t pc, bool materialize = true) {
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
        if (materialize)
          for (uint64_t i = 0; i < count; ++i)
            result.keys.push_back(int32_t(first + int64_t(i)));
        start = pc + 4;
      } else {
        std::optional<int32_t> previous;
        for (size_t i = 0; i < count; ++i) {
          int32_t key = int32_t(signedValue(value(pc + 2 + i * 2), 32));
          if (previous && key <= *previous)
            bad("sparse switch keys unordered");
          previous = key;
          if (materialize)
            result.keys.push_back(key);
        }
        start = pc + 2 + size_t(count) * 2;
      }
      queryReserve(result.targets, size_t(count), true);
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
      if (!materialize)
        return result;
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
  static void wideRegisters(const WideOperands &wide,
                            llvm::ArrayRef<unsigned> registers,
                            unsigned count) {
    auto check = [&](size_t index) {
      if (index >= registers.size() || registers[index] + 1 >= count)
        bad("wide register pair exceeds frame");
    };
    for (unsigned index = 0, mask = wide.mask; mask; ++index, mask >>= 1)
      if (mask & 1)
        check(index);
    if (wide.all)
      for (size_t i = 0; i < registers.size(); ++i)
        if (!wide.except_last || i + 1 != registers.size())
          check(i);
  }
  class InstructionLengths {
    using Entry = std::pair<uint32_t, unsigned>;
    std::vector<Entry> entries;
    auto find(uint32_t pc) const {
      return std::lower_bound(entries.begin(), entries.end(), pc,
                              [](const Entry &entry, uint32_t target) {
                                return entry.first < target;
                              });
    }

  public:
    void reserve(size_t count, Dex &dex) {
      if (dex.query_retained)
        dex.queryReserve(entries, count, true);
      else
        entries.reserve(count);
    }
    // The instruction decoder appends starts in strictly increasing PC order.
    void append(uint32_t pc, unsigned width, Dex &dex) {
      dex.queryReserve(entries, entries.size() + 1);
      entries.emplace_back(pc, width);
    }
    std::optional<uint32_t> ordinal(uint32_t pc) const {
      const auto found = find(pc);
      if (found == entries.end() || found->first != pc)
        return std::nullopt;
      return uint32_t(found - entries.begin());
    }
    bool contains(uint32_t pc) const { return ordinal(pc).has_value(); }
  };
  struct Instructions {
    std::vector<Instruction> recovery;
    std::vector<FlowInstruction> flow;
    InstructionLengths lengths;
  };
  Instructions instructions(CodeUnits words, unsigned registers, unsigned outs,
                            std::vector<CodeReference> *references = nullptr) {
    Instructions result;
    auto &[code, flow, lengths] = result;
    const auto &specs = opcodes();
    const size_t initial = std::min<size_t>(words.size(), 32);
    if (query_retained)
      queryReserve(flow, initial, true);
    else
      flow.reserve(initial);
    lengths.reserve(initial, *this);
    std::map<uint32_t, Payload> payloads;
    auto retain = [&](uint64_t bytes) {
      if (references)
        retainQueryStorage(bytes);
    };
    for (size_t pc = 0; pc < words.size();) {
      boundedWork(budget, scalar_reads);
      unsigned word = words[pc], opcode = word & 255;
      if (!opcode && word) {
        auto data_payload = payload(words, pc, !references);
        size_t size = data_payload.size;
        retain(queryNodeBytes<decltype(payloads)>());
        payloads.emplace(uint32_t(pc), std::move(data_payload));
        pc += size;
        continue;
      }
      auto &spec = specs[opcode];
      if (spec.name.empty())
        bad("unsupported opcode at code unit " + std::to_string(pc));
      auto &name = spec.name;
      auto &form = spec.form;
      unsigned size = spec.size;
      if (size > words.size() - pc)
        bad("truncated instruction");
      unsigned high = word >> 8;
      FlowInstruction fact;
      fact.pc = uint32_t(pc);
      fact.opcode = uint8_t(opcode);
      fact.width = uint8_t(size);
      std::optional<Instruction> ins;
      if (!references) {
        ins.emplace();
        ins->pc = uint32_t(pc);
        ins->opcode = name;
      }
      QueryTemporary register_storage{query_retained};
      llvm::SmallVector<unsigned, 5> regs;
      auto tail = [&](unsigned i) { return words[pc + 1 + i]; };
      auto tailValue = [&] {
        uint64_t value = 0;
        for (unsigned i = 0; i < size - 1; ++i)
          value |= uint64_t(tail(i)) << (i * 16);
        return value;
      };
      uint64_t index = 0;
      if (spec.reserved_high && high)
        bad("nonzero reserved instruction bits");
      switch (form) {
      case Format::F10x:
        break;
      case Format::F12x:
        regs = {high & 15, high >> 4};
        break;
      case Format::F11x:
        regs = {high};
        break;
      case Format::F11n:
        regs = {high & 15};
        if (ins)
          ins->literal = signedValue(high >> 4, 4);
        break;
      case Format::F22x:
        regs = {high, tail(0)};
        break;
      case Format::F32x:
        regs = {tail(0), tail(1)};
        break;
      case Format::F23x:
        regs = {high, unsigned(tail(0) & 255), unsigned(tail(0) >> 8)};
        break;
      case Format::F22b:
        regs = {high, unsigned(tail(0) & 255)};
        if (ins)
          ins->literal = signedValue(tail(0) >> 8, 8);
        break;
      case Format::F21s:
      case Format::F21h:
      case Format::F31i:
      case Format::F51l: {
        regs = {high};
        int64_t value = signedValue(tailValue(), 16 * (size - 1));
        if (form == Format::F21h)
          value = std::bit_cast<int64_t>(uint64_t(value)
                                         << (opcode == 0x19 ? 48 : 16));
        if (ins)
          ins->literal = value;
        break;
      }
      case Format::F22s:
        regs = {high & 15, high >> 4};
        if (ins)
          ins->literal = signedValue(tail(0), 16);
        break;
      case Format::F21c:
      case Format::F31c:
        regs = {high};
        index = tailValue();
        break;
      case Format::F22c:
        regs = {high & 15, high >> 4};
        index = tail(0);
        break;
      case Format::F21t:
      case Format::F22t:
      case Format::F31t:
        regs = form == Format::F22t
                   ? llvm::SmallVector<unsigned, 5>{high & 15, high >> 4}
                   : llvm::SmallVector<unsigned, 5>{high};
        fact.target =
            targetPC(uint32_t(pc), signedValue(tailValue(), 16 * (size - 1)));
        break;
      case Format::F10t:
      case Format::F20t:
      case Format::F30t:
        fact.target = targetPC(uint32_t(pc),
                               form == Format::F10t
                                   ? signedValue(high, 8)
                                   : signedValue(tailValue(), 16 * (size - 1)));
        break;
      case Format::F35c: {
        unsigned count = high >> 4;
        index = tail(0);
        if (count > 5)
          bad("invoke register count exceeds instruction format");
        for (unsigned i = 0; i < std::min(count, 4u); ++i)
          regs.push_back((tail(1) >> (4 * i)) & 15);
        if (count == 5)
          regs.push_back(high & 15);
        break;
      }
      case Format::F3rc:
        if (query_retained && high > regs.capacity()) {
          // The range count is one byte; bound SmallVector's growth before it
          // can spill, and release this per-instruction temporary on exit.
          const uint64_t bytes = uint64_t(2 * high + 1) * sizeof(unsigned);
          retainQueryStorage(bytes);
          register_storage.bytes = bytes;
          regs.reserve(high);
        }
        index = tail(0);
        for (unsigned i = 0; i < high; ++i)
          regs.push_back(tail(1) + i);
        break;
      default:
        bad("unsupported instruction format");
      }
      for (auto reg : regs)
        if (reg >= registers)
          bad("instruction register exceeds frame");
      if (spec.pool == 's') {
        const auto &value = at(strings, index, "string");
        if (ins)
          ins->literal = value;
      } else if (spec.pool == 't') {
        const auto &value = typeValue(index);
        if (ins)
          ins->reference = value;
        if (spec.flow.producer == ProducerKind::Array)
          fact.produced = ResultKind::Object;
      } else if (spec.pool == 'f') {
        const auto &value = at(fields, index, "field");
        if (ins)
          ins->reference = materializeField(value);
      } else if (spec.pool == 'm') {
        const auto &ref = at(methods, index, "method");
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
        if (spec.flow.producer == ProducerKind::Method)
          fact.produced = resultKind(ref.returns);
        if (ins)
          ins->reference = materializeMethod(ref);
      }
      if (spec.pool == 't') {
        const auto &ref = typeValue(index);
        if (name.starts_with("filled-new-array") &&
            (!ref.starts_with('[') || std::string_view(ref).substr(1) == "J" ||
             std::string_view(ref).substr(1) == "D"))
          bad("filled-new-array requires a single-word array component");
        if (name == "new-instance" && !ref.starts_with('L'))
          bad("new-instance requires class type");
        if (name == "new-array" && !ref.starts_with('['))
          bad("new-array requires array type");
        if ((name == "check-cast" || name == "instance-of") &&
            !ref.starts_with('L') && !ref.starts_with('['))
          bad("reference operation requires object type");
      }
      wideRegisters(spec.wide, regs, registers);
      if (references && spec.pool == reference_pool &&
          reference_matches->contains(uint32_t(index))) {
        queryReserve(*references, references->size() + 1);
        references->push_back(
            {uint32_t(pc), uint32_t(index), uint8_t(opcode), spec.pool});
      }
      lengths.append(uint32_t(pc), size, *this);
      if (ins) {
        ins->target = fact.target;
        ins->registers.assign(regs.begin(), regs.end());
        code.push_back(std::move(*ins));
      }
      queryReserve(flow, flow.size() + 1);
      flow.push_back(std::move(fact));
      pc += size;
    }
    std::set<uint32_t> used_payloads;
    for (size_t i = 0; i < flow.size(); ++i) {
      auto &ins = flow[i];
      const auto &properties = specs[ins.opcode].flow;
      if (properties.payload_ident) {
        auto found = payloads.find(*ins.target);
        if (found == payloads.end() ||
            found->second.ident != properties.payload_ident)
          bad("instruction has missing/mismatched payload");
        queryInsert(used_payloads, *ins.target);
        const auto &p = found->second;
        if (!references) {
          code[i].keys = p.keys;
          code[i].data = p.data;
          code[i].element_width = p.element_width;
        }
        for (auto delta : p.targets) {
          budget.tick();
          auto target = targetPC(ins.pc, delta);
          const auto index = lengths.ordinal(target);
          if (!index)
            bad("switch target is not an instruction boundary");
          queryReserve(ins.targets, ins.targets.size() + 1);
          ins.targets.push_back(*index);
          if (!references)
            code[i].targets.push_back(target);
        }
      } else if (ins.target) {
        const auto index = lengths.ordinal(*ins.target);
        if (!index)
          bad("branch target is not an instruction boundary");
        if (*ins.target == ins.pc && !properties.zero_branch)
          bad("zero branch displacement");
        // Public instructions already own their code-unit target. Flow uses
        // the resolved ordinal from this same boundary check.
        ins.target = *index;
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
    item<bool>(
        0x2003, offset,
        [&](Cursor &reader) {
          int64_t line = reader.leb(), count = reader.leb();
          if (line < 0 || uint64_t(count) != parameter_count)
            bad("debug header disagrees with method");
          auto optionalString = [&] {
            auto index = reader.leb() - 1;
            if (index >= 0)
              at(strings, uint64_t(index), "string");
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
                typeValue(uint64_t(typ));
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
            if (address > code_end || line < 0)
              bad("debug position outside method");
          }
          return true;
        },
        1, false);
  }
  void codeFlow(const std::vector<FlowInstruction> &instructions,
                const std::vector<TryRegion> &tries,
                const InstructionLengths &lengths) {
    const auto &specs = opcodes();
    enum : uint8_t { Explicit = 1, HandlerEntry = 2, Reached = 4 };
    std::vector<uint8_t> flags;
    auto mark = [&](uint32_t index, uint8_t bits) {
      if (flags.empty()) {
        queryReserve(flags, instructions.size(), true);
        flags.resize(instructions.size(), 0);
      }
      flags[index] |= bits;
    };
    for (const auto &region : tries)
      for (const auto &handler : region.handlers)
        mark(*lengths.ordinal(handler.target), Explicit | HandlerEntry);
    for (const auto &ins : instructions) {
      for (auto index : ins.targets)
        mark(index, Explicit);
      const auto control = specs[ins.opcode].flow.control;
      if (control == FlowControl::Conditional || control == FlowControl::Goto)
        mark(*ins.target, Explicit);
    }
    const FlowInstruction *previous = nullptr;
    for (size_t i = 0; i < instructions.size(); ++i) {
      const auto &ins = instructions[i];
      const unsigned entry = flags.empty() ? 0 : flags[i];
      const auto &properties = specs[ins.opcode].flow;
      if (properties.move_exception && !(entry & HandlerEntry))
        bad("move-exception outside an exception handler entry");
      if (properties.result_use != ResultKind::None) {
        if ((entry & Explicit) || !previous ||
            previous->pc + previous->width != ins.pc)
          bad("move-result has an invalid control-flow predecessor");
        if (previous->produced == ResultKind::None)
          bad("move-result does not immediately follow an invocation");
        if (previous->produced == ResultKind::Void ||
            properties.result_use != previous->produced)
          bad("move-result kind disagrees with invocation result");
      }
      previous = &ins;
    }
    auto visitWork = [&] { boundedWork(budget, scalar_reads); };
    if (flags.empty()) {
      // With no branch or handler entries the reachable path is a prefix.
      // Validate every predecessor above, including unreachable instructions,
      // then follow that prefix without allocating a queue and visited set.
      uint32_t pc = 0;
      for (const auto &ins : instructions) {
        visitWork();
        if (ins.pc != pc)
          bad("normal or handler execution falls outside executable "
              "instructions");
        if (specs[ins.opcode].flow.control == FlowControl::Stop)
          return;
        pc += ins.width;
      }
      visitWork();
      bad("normal or handler execution falls outside executable "
          "instructions");
    }
    std::vector<std::pair<uint32_t, bool>> pending;
    auto enqueue = [&](uint32_t index, bool exception_edge) {
      queryReserve(pending, pending.size() + 1);
      pending.emplace_back(index, exception_edge);
    };
    enqueue(lengths.ordinal(0).value_or(UINT32_MAX), false);
    // Ordinals follow PC order, preserving the handler queue's traversal order.
    for (size_t i = 0; i < flags.size(); ++i)
      if (flags[i] & HandlerEntry)
        enqueue(uint32_t(i), true);
    while (!pending.empty()) {
      visitWork();
      auto [index, exception_edge] = pending.back();
      pending.pop_back();
      // Invalid fallthrough is diagnosed when visited, preserving traversal
      // order even when a branch or exception entry was queued later.
      if (index == UINT32_MAX)
        bad("normal or handler execution falls outside executable "
            "instructions");
      const auto &ins = instructions[index];
      const auto &properties = specs[ins.opcode].flow;
      if (properties.move_exception && !exception_edge)
        bad("normal execution enters move-exception");
      if (flags[index] & Reached)
        continue;
      flags[index] |= Reached;
      if (properties.control == FlowControl::Stop)
        continue;
      if (properties.control == FlowControl::Goto)
        enqueue(*ins.target, false);
      else {
        const auto next = index + 1;
        enqueue(next < instructions.size() &&
                        instructions[next].pc == ins.pc + ins.width
                    ? next
                    : UINT32_MAX,
                false);
        if (properties.control == FlowControl::Conditional)
          enqueue(*ins.target, false);
        for (auto target : ins.targets)
          enqueue(target, false);
      }
    }
  }
  struct MethodContext {
    const MethodPoolEntry &reference;
    bool is_static;
    uint32_t identity() const {
      return uint32_t(reference.prototype) * 2 + is_static;
    }
    unsigned incomingWords() const {
      return detail::incomingWords(reference.parameters, is_static);
    }
  };
  static void requireCodeContext(uint32_t actual, uint32_t expected) {
    if (actual != expected)
      bad("shared data item has inconsistent declaration context");
  }
  void codeContext(uint32_t offset, const MethodContext &method) {
    const auto found = code_contexts.lower_bound(offset);
    if (found != code_contexts.end() && found->first == offset) {
      requireCodeContext(found->second, method.identity());
      return;
    }
    code_contexts.emplace_hint(found, offset, method.identity());
  }
  Code code(uint32_t offset, const MethodContext &method,
            std::vector<CodeReference> *references = nullptr) {
    struct ScratchStorage {
      uint64_t *total;
      uint64_t previous;
      const uint64_t &persistent;
      uint64_t previous_persistent;
      ~ScratchStorage() {
        if (total)
          *total = previous + (persistent - previous_persistent);
      }
    } scratch{query_retained, query_retained ? *query_retained : 0,
              query_persistent, query_persistent};
    // Reference queries keep this context beside the decoded sites. Recovery
    // retains it separately from the public, owned body cache.
    if (!references)
      codeContext(offset, method);
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
          // The validated input owns these bytes throughout decoding. Load
          // little-endian units directly, including unaligned host addresses.
          const CodeUnits words{reader.takeCodeUnits(size)};
          auto [ins, flow, lengths] =
              instructions(words, registers, outgoing, references);
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
            queryReserve(raw_tries, raw_tries.size() + 1);
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
              std::set<uint64_t> caught_types;
              for (uint64_t j = 0; j < magnitude; ++j) {
                const auto type_index = uint64_t(reader.leb());
                const auto &typ = typeValue(type_index);
                auto target = uint32_t(reader.leb());
                if (!typ.starts_with('L') || !lengths.contains(target) ||
                    !queryInsert(caught_types, type_index).second)
                  bad("invalid or duplicate typed exception handler");
                queryReserve(entries, entries.size() + 1);
                entries.push_back(
                    {references ? std::optional<std::string>{} : typ, target});
              }
              if (length <= 0) {
                auto target = uint32_t(reader.leb());
                if (!lengths.contains(target))
                  bad("catch-all target is not an instruction");
                queryReserve(entries, entries.size() + 1);
                entries.push_back({std::nullopt, target});
              }
              retainQueryStorage(queryNodeBytes<decltype(handlers)>());
              handlers.emplace(handler_offset, std::move(entries));
            }
          }
          std::vector<TryRegion> tries;
          for (auto [start, end, handler] : raw_tries) {
            auto found = handlers.find(handler);
            if (found == handlers.end())
              bad("try references non-handler offset");
            queryReserve(tries, tries.size() + 1);
            retainQueryStorage(uint64_t(found->second.size()) *
                               sizeof(Handler));
            budget.tick(found->second.size());
            tries.push_back({start, end, found->second});
          }
          codeFlow(flow, tries, lengths);
          debugInfo(debug, registers, size, method.reference.parameters.size());
          return Code{registers, incoming, std::move(ins), std::move(tries),
                      size};
        },
        4, !references);
    if (result.incoming != method.incomingWords())
      bad("shared code item signature mismatch");
    return result;
  }
  using MethodVisitor = std::function<void(const MethodContext &, uint32_t)>;
  void classData(Class &cls, uint32_t offset, const MethodVisitor &visit = {}) {
    if (!offset)
      return;
    context(0x2000, offset, cls.name);
    item<bool>(
        0x2000, offset,
        [&](Cursor &reader) {
          std::array<uint64_t, 4> counts{};
          uint64_t total = 0;
          for (auto &count : counts) {
            count = uint64_t(reader.leb());
            total += count;
          }
          budget.tick(total);
          std::vector<uint64_t> first_fields, first_methods;
          struct MemberStorage {
            uint64_t *total;
            uint64_t bytes = 0;
            ~MemberStorage() {
              if (total)
                *total -= bytes;
            }
          } member_storage{query_retained};
          auto remember = [&](std::vector<uint64_t> &first, uint64_t index,
                              uint64_t count) {
            if (first.size() == first.capacity()) {
              const uint64_t previous =
                  uint64_t(first.capacity()) * sizeof(uint64_t);
              const size_t next = size_t(std::min<uint64_t>(
                  count,
                  std::max<uint64_t>(4, uint64_t(first.capacity()) * 2)));
              const uint64_t bytes = uint64_t(next) * sizeof(uint64_t);
              // Account both buffers while reserve copies the bounded integer
              // list.
              retainQueryStorage(bytes);
              if (query_retained)
                member_storage.bytes += bytes;
              first.reserve(next);
              if (query_retained) {
                *query_retained -= previous;
                member_storage.bytes -= previous;
              }
            }
            first.push_back(index);
          };
          for (unsigned group = 0; group < 4; ++group) {
            uint64_t index = 0;
            for (uint64_t member_index = 0; member_index < counts[group];
                 ++member_index) {
              auto diff = uint64_t(reader.leb());
              auto flags = uint32_t(reader.leb());
              if (member_index && !diff)
                bad("duplicate class-data member index");
              index += diff;
              detail::validateAccessBits(flags);
              auto &first = group < 2 ? first_fields : first_methods;
              if (!(group & 1)) {
                if (counts[group + 1])
                  remember(first, index, counts[group]);
              } else if (std::binary_search(first.begin(), first.end(), index))
                bad("duplicate defined member");
              if (group < 2) {
                const auto &reference = at(fields, index, "defined member");
                if (reference.owner != cls.name)
                  bad("class-data member owner mismatch");
                if (bool(flags & detail::Static) != (group == 0))
                  bad("field storage/access mismatch");
                if (!visit)
                  cls.fields.push_back(
                      {materializeField(reference), accessFlags(flags), {}});
              } else {
                const auto &reference = at(methods, index, "defined member");
                if (reference.owner != cls.name)
                  bad("class-data member owner mismatch");
                bool direct = (flags & (detail::Static | detail::Private |
                                        detail::Constructor)) ||
                              reference.name == "<init>" ||
                              reference.name == "<clinit>";
                if (direct != (group == 2))
                  bad("direct/virtual method classification mismatch");
                uint32_t code_off = uint32_t(reader.leb());
                bool no_body = flags & (detail::Native | detail::Abstract);
                if (bool(code_off) == no_body)
                  bad("method code/access mismatch");
                const MethodContext method_context{
                    reference, bool(flags & detail::Static)};
                if (visit)
                  visit(method_context, code_off);
                else {
                  Method method;
                  method.reference = materializeMethod(reference);
                  method.access = accessFlags(flags);
                  if (method.access.erase("volatile"))
                    method.access.insert("bridge");
                  if (method.access.erase("transient"))
                    method.access.insert("varargs");
                  if (code_off) {
                    auto body = code(code_off, method_context);
                    method.registers = body.registers;
                    method.instructions = std::move(body.instructions);
                    method.tries = std::move(body.tries);
                    method.code_end = body.size;
                  }
                  cls.methods.push_back(std::move(method));
                }
              }
            }
          }
          return true;
        },
        1, false);
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
  // Pool entries refer to this object's table storage.
  Dex(const Dex &) = delete;
  Dex &operator=(const Dex &) = delete;
  Dex(Dex &&) = delete;
  Dex &operator=(Dex &&) = delete;
  Dex(std::string_view data, std::string_view source_id, Budget &budget)
      : data(data), source_id(source_id), budget(budget) {}
  std::vector<std::string> listClasses() {
    const auto tables = header();
    return classDefinitions(tables);
  }
  using ClassVisitor =
      std::function<void(const std::string &, uint32_t, uint32_t)>;
  enum class ClassTables { Sparse, Validated };
  std::vector<std::string>
  classDefinitions(const Tables &tables, const ClassVisitor &visit = {},
                   ClassTables mode = ClassTables::Sparse) {
    const auto [count, offset] = tables.at(6);
    if (count > budget.limits.max_files)
      bad("class inventory exceeds file limit");
    auto record = [&](unsigned kind, uint32_t index, unsigned width) {
      const auto [size, start] = tables.at(kind);
      if (index >= size)
        bad(kind == 1 ? "string index out of bounds"
                      : "type index out of bounds");
      return cursor(size_t(start) + size_t(index) * width,
                    sections.at(kind).end);
    };
    auto dataOffset = [&](unsigned kind, uint32_t at, unsigned alignment) {
      const auto section = sections.find(kind);
      if (section == sections.end() || at % alignment ||
          at < section->second.start || at >= section->second.end)
        bad("item points outside its mapped section");
    };
    std::map<uint32_t, std::string> referenced_types;
    uint64_t type_bytes = 0;
    auto classType = [&](uint32_t index) -> const std::string & {
      if (mode == ClassTables::Validated) {
        // readTables has already decoded every string and validated every
        // type descriptor. Reuse that authoritative result in full queries.
        const auto &name = at(types, index, "type");
        if (name == "V")
          (void)descriptor(name);
        if (!name.starts_with('L'))
          bad("class metadata requires class type");
        return name;
      }
      if (const auto found = referenced_types.find(index);
          found != referenced_types.end())
        return found->second;
      auto type_record = record(2, index, 4);
      auto string_record = record(1, type_record.u32(), 4);
      const auto string_offset = string_record.u32();
      dataOffset(0x2002, string_offset, 1);
      auto string_reader = cursor(string_offset, sections.at(0x2002).end);
      // The type cache owns the validated name. Retain only the decoded item
      // range here, without keeping a second UTF-8/UTF-16 string inventory.
      auto decoded = mutf8(string_reader);
      recordRange(Key{0x2002, string_offset}, string_reader.pos);
      auto name = descriptor(decoded.text);
      if (!name.starts_with('L'))
        bad("class metadata requires class type");
      if (name.size() > budget.limits.max_bytes - type_bytes)
        bad("class metadata exceeds byte limit");
      type_bytes += name.size();
      return referenced_types.emplace(index, std::move(name)).first->second;
    };
    auto reader = cursor(offset, count ? sections.at(6).end : offset);
    std::vector<std::string> result;
    result.reserve(count);
    std::set<std::string> names;
    std::vector<uint8_t> defined_types;
    std::vector<uint32_t> interface_classes;
    QueryTemporary identity_storage{query_retained};
    if (mode == ClassTables::Validated) {
      const uint64_t bytes =
          uint64_t(types.size()) * (sizeof(uint8_t) + sizeof(uint32_t));
      retainQueryStorage(bytes);
      identity_storage.bytes = bytes;
      defined_types.resize(types.size(), 0);
      interface_classes.resize(types.size(), UINT32_MAX);
    }
    uint64_t name_bytes = 0;
    for (uint32_t i = 0; i < count; ++i) {
      budget.tick();
      const uint32_t index = reader.u32(), flags = reader.u32(),
                     parent = reader.u32(), interfaces = reader.u32(),
                     source = reader.u32(), annotations = reader.u32(),
                     class_data = reader.u32(), values = reader.u32();
      const auto &name = classType(index);
      if (mode == ClassTables::Validated) {
        // Full table validation proved that type IDs have unique names.
        if (defined_types[index])
          bad("invalid/duplicate class definition");
        defined_types[index] = 1;
      } else if (!names.insert(name).second)
        bad("invalid/duplicate class definition");
      detail::validateAccessBits(flags);
      if (parent != UINT32_MAX && classType(parent) == name)
        bad("invalid superclass");
      if (interfaces) {
        dataOffset(0x1001, interfaces, 4);
        auto list = cursor(interfaces, sections.at(0x1001).end);
        const auto length = list.u32();
        if (length > (list.end - list.pos) / 2)
          bad("truncated type list");
        std::set<std::string> implemented;
        for (uint32_t j = 0; j < length; ++j) {
          const auto interface = list.u16();
          const auto &type = classType(interface);
          if (mode == ClassTables::Validated) {
            if (interface_classes[interface] == i)
              bad("invalid/duplicate interface");
            interface_classes[interface] = i;
          } else if (!implemented.insert(type).second)
            bad("invalid/duplicate interface");
        }
        recordRange(Key{0x1001, interfaces}, list.pos);
      }
      if (source != UINT32_MAX) {
        auto source_record = record(1, source, 4);
        dataOffset(0x2002, source_record.u32(), 1);
      }
      if (annotations)
        dataOffset(0x2006, annotations, 4);
      if (class_data)
        dataOffset(0x2000, class_data, 1);
      if (values)
        dataOffset(0x2005, values, 1);
      if (name.size() > budget.limits.max_bytes - name_bytes)
        bad("class inventory exceeds byte limit");
      name_bytes += name.size();
      result.push_back(name);
      if (visit)
        visit(name, flags, class_data);
    }
    checkRangeOverlaps();
    return result;
  }
  DexReferenceResult findReferences(const DexReferenceQuery &query) {
    if (query.text.empty())
      throw Error("reference query text must not be empty");
    char pool;
    switch (query.kind) {
    case DexReferenceKind::String:
      pool = 's';
      break;
    case DexReferenceKind::Type:
      pool = 't';
      break;
    case DexReferenceKind::Method:
      pool = 'm';
      break;
    case DexReferenceKind::Field:
      pool = 'f';
      break;
    default:
      throw Error("invalid reference query kind");
    }
    if (query.owner) {
      if (pool != 'm' && pool != 'f')
        throw Error("reference owner filter requires a method or field query");
      const auto owner = descriptor(*query.owner);
      if (!owner.starts_with('L') && !(pool == 'm' && owner.starts_with('[')))
        throw Error("invalid reference owner descriptor");
    }
    uint64_t retained = 0;
    query_retained = &retained;
    auto retain = [&](uint64_t bytes) { retainQueryStorage(bytes); };
    retain(query.text.size());
    // Bound substring matching linearly even for repeated pool/query prefixes.
    std::vector<size_t> prefix;
    if (!query.exact) {
      if (query.text.size() > budget.limits.max_bytes / sizeof(size_t))
        throw Error("reference query storage exceeds byte limit");
      retain(query.text.size() * sizeof(size_t));
      budget.tick(query.text.size());
      budget.tick(query.text.size());
      prefix.resize(query.text.size());
      for (size_t i = 1, matched = 0; i < query.text.size(); ++i) {
        if (!(i % 4096))
          budget.check();
        while (matched && query.text[i] != query.text[matched])
          matched = prefix[matched - 1];
        if (query.text[i] == query.text[matched])
          ++matched;
        prefix[i] = matched;
      }
    }
    auto matches = [&](std::string_view text) {
      if (query.exact) {
        boundedWork(budget, scalar_reads);
        if (text.size() != query.text.size())
          return false;
        boundedWork(budget, scalar_reads, text.size());
        return text == query.text;
      }
      boundedWork(budget, scalar_reads, text.size());
      budget.consumeWork(text.size());
      if (query.text.size() <= 16) {
        // Fixed short needles bound comparison work independently of the
        // input and use the library's byte search. Overlapping bounded blocks
        // preserve matches across block edges and deadline checks.
        for (size_t start = 0; start < text.size(); start += 4096) {
          if (start)
            budget.check();
          const auto count = std::min<size_t>(4096 + query.text.size() - 1,
                                              text.size() - start);
          if (text.substr(start, count).find(query.text) !=
              std::string_view::npos)
            return true;
        }
        return false;
      }
      for (size_t i = 0, matched = 0; i < text.size(); ++i) {
        if (i && !(i % 4096))
          budget.check();
        while (matched && text[i] != query.text[matched])
          matched = prefix[matched - 1];
        if (text[i] == query.text[matched])
          ++matched;
        if (matched == query.text.size())
          return true;
      }
      return false;
    };
    const auto tables = header();
    readTables(tables);
    ReferenceMatches selected;
    auto select = [&](uint32_t index, const std::string &text) {
      if (!matches(text))
        return;
      retainQueryPersistent(sizeof(ReferenceMatches::value_type) +
                            4 * sizeof(void *));
      retain(text.size());
      Match match{text, {}};
      if (pool == 's') {
        auto row = cursor(size_t(tables.at(1).second) + size_t(index) * 4,
                          sections.at(1).end);
        auto value = item<DecodedString>(
            0x2002, row.u32(), [&](Cursor &r) { return mutf8(r); }, 1, false);
        match.utf16 = std::move(value.units);
      }
      selected.emplace(index, std::move(match));
    };
    if (pool == 's' || pool == 't') {
      const auto &values = pool == 's' ? strings : types;
      for (size_t i = 0; i < values.size(); ++i)
        select(uint32_t(i), values[i]);
    } else if (pool == 'm') {
      for (size_t i = 0; i < methods.size(); ++i) {
        budget.tick();
        if (!query.owner || methods[i].owner == *query.owner)
          select(uint32_t(i), methods[i].identity());
      }
    } else {
      for (size_t i = 0; i < fields.size(); ++i) {
        budget.tick();
        const auto &field = fields[i];
        if (!query.owner || field.owner == *query.owner)
          select(uint32_t(i),
                 field.owner + "->" + field.name + ":" + field.type);
      }
    }
    DexReferenceResult result;
    result.matching_pool_entries = selected.size();
    reference_pool = pool;
    reference_matches = &selected;
    struct ReferencedCode {
      uint32_t context;
      std::vector<CodeReference> sites;
    };
    std::map<uint32_t, ReferencedCode> code_references;
    result.class_descriptors = classDefinitions(
        tables,
        [&](const std::string &name, uint32_t flags, uint32_t offset) {
          retain(sizeof(std::string));
          retain(name.size());
          Class cls;
          queryCopyWork(name.size());
          cls.name = name;
          classData(
              cls, offset,
              [&](const MethodContext &method, uint32_t code_offset) {
                boundedWork(budget, scalar_reads);
                if (result.defined_method_count >= budget.limits.max_files)
                  bad("defined method inventory exceeds file limit");
                ++result.defined_method_count;
                if (!code_offset)
                  return;
                // Code items are commonly visited in file order. Both paths
                // produce an exact insertion hint and still check every
                // shared-code context.
                auto found =
                    code_references.empty() ||
                            code_references.rbegin()->first < code_offset
                        ? code_references.end()
                        : code_references.lower_bound(code_offset);
                if (found == code_references.end() ||
                    found->first != code_offset) {
                  std::vector<CodeReference> references;
                  (void)code(code_offset, method, &references);
                  // Recovery owns typed bodies. Queries retain matching operand
                  // sites after full validation and replay every method owner.
                  retain(uint64_t(references.capacity()) *
                         sizeof(CodeReference));
                  retainQueryPersistent(
                      sizeof(decltype(code_references)::value_type) +
                      4 * sizeof(void *));
                  found = code_references.emplace_hint(
                      found, code_offset,
                      ReferencedCode{method.identity(), std::move(references)});
                  ++result.scanned_code_item_count;
                } else
                  requireCodeContext(found->second.context, method.identity());
                ++result.scanned_method_count;
                for (const auto &reference : found->second.sites) {
                  budget.tick();
                  if (reference.pool != pool)
                    continue;
                  const auto match = selected.find(reference.index);
                  if (match == selected.end())
                    continue;
                  if (result.references.size() >= budget.limits.max_files)
                    bad("reference site inventory exceeds file limit");
                  retain(sizeof(DexReferenceSite));
                  const auto &opcode = opcodes()[reference.opcode].name;
                  retain(opcode.size());
                  retain(match->second.text.size());
                  if (match->second.utf16)
                    retain(uint64_t(match->second.utf16->size()) *
                           sizeof(char16_t));
                  queryCopyWork(match->second.text.size() +
                                (match->second.utf16
                                     ? uint64_t(match->second.utf16->size()) *
                                           sizeof(char16_t)
                                     : 0));
                  result.references.push_back(
                      {materializeMethod(method.reference), reference.pc,
                       opcode, reference.index, match->second.text,
                       match->second.utf16});
                }
              });
        },
        ClassTables::Validated);
    budget.check();
    result.code_scan_complete = true;
    return result;
  }
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
std::vector<std::string> listDexClasses(std::string_view bytes,
                                        Budget &budget) {
  return Dex(bytes, {}, budget).listClasses();
}
DexReferenceResult findDexReferences(std::string_view bytes,
                                     const DexReferenceQuery &query,
                                     Budget &budget) {
  return Dex(bytes, {}, budget).findReferences(query);
}
} // namespace neverd::mobile::dalvik
