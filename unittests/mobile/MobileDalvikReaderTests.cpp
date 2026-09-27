//===- MobileDalvikReaderTests.cpp - Independent native reader fixtures
//----===//
#include "MobileDalvik.h"
#include "MobileDalvikSignature.h"
#include "gtest/gtest.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SHA1.h"

#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <tuple>
#include <zlib.h>

using namespace neverd::mobile;
using namespace neverd::mobile::dalvik;
namespace {
void append(std::string &out, uint64_t value, unsigned size) {
  for (unsigned i = 0; i < size; ++i)
    out += char(value >> (i * 8));
}
void patch(std::string &out, size_t at, uint64_t value, unsigned size = 4) {
  ASSERT_LE(at + size, out.size());
  for (unsigned i = 0; i < size; ++i)
    out[at + i] = char(value >> (i * 8));
}
void uleb(std::string &out, uint64_t value) {
  do {
    unsigned byte = value & 127;
    value >>= 7;
    out += char(byte | (value ? 128 : 0));
  } while (value);
}
std::string seal(std::string out) {
  auto hash = llvm::SHA1::hash(llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(out.data() + 32), out.size() - 32));
  for (size_t i = 0; i < hash.size(); ++i)
    out[12 + i] = char(hash[i]);
  uint32_t a = 1, b = 0;
  for (size_t i = 12; i < out.size(); ++i) {
    a = (a + uint8_t(out[i])) % 65521;
    b = (b + a) % 65521;
  }
  patch(out, 8, a | (b << 16));
  return out;
}
std::u16string utf16(std::string_view ascii) {
  return std::u16string(ascii.begin(), ascii.end());
}
void mutf8(std::string &out, const std::u16string &text) {
  uleb(out, text.size());
  for (unsigned unit : text) {
    if (unit && unit < 128)
      out += char(unit);
    else if (unit < 2048) {
      out += char(0xc0 | (unit >> 6));
      out += char(0x80 | (unit & 63));
    } else {
      out += char(0xe0 | (unit >> 12));
      out += char(0x80 | ((unit >> 6) & 63));
      out += char(0x80 | (unit & 63));
    }
  }
  out += '\0';
}
struct Fixture;
struct FixtureAnnotation {
  std::string type;
  unsigned visibility;
  std::vector<std::string> extra_types;
  std::vector<std::u16string> extra_strings;
  std::vector<FieldRef> extra_fields;
  // Write encoded_annotation's element count and elements using real pool
  // indices. The fixture supplies the item visibility and annotation type.
  std::function<void(std::string &, const Fixture &)> elements;
  FixtureAnnotation(std::string type, unsigned visibility)
      : type(std::move(type)), visibility(visibility) {}
};
struct FixtureOptions {
  std::vector<uint16_t> words{0x000f};
  std::vector<std::string> params{"I"};
  std::string returns = "I", version = "035";
  unsigned registers = 1, flags = 9;
  std::vector<std::array<uint32_t, 3>> tries;
  std::string handlers;
  std::string debug_info;
  std::vector<std::u16string> extras;
  std::optional<std::string> static_value;
  std::string field_type = "I";
  std::optional<MethodRef> referenced_method;
  std::vector<MethodRef> extra_methods;
  std::vector<FieldRef> extra_fields;
  std::string owner = "Lfixture/Sample;";
  std::vector<FixtureAnnotation> annotations;
  std::vector<std::vector<size_t>> annotation_sets;
  std::optional<size_t> class_annotations, field_annotations,
      method_annotations;
  std::optional<std::vector<std::optional<size_t>>> parameter_annotations;
  std::string method_name = "value";
  unsigned class_flags = 1;
  std::vector<std::string> interfaces;
  bool define_method = true;
  bool define_referenced_method = false;
  bool separate_referenced_code = false;
  unsigned referenced_registers = 1;
  std::vector<uint16_t> referenced_words{0x000e};
  bool duplicate_class = false;
};
struct Fixture {
  std::string data;
  std::map<std::string, size_t> at;
  std::vector<std::u16string> strings;
  std::vector<std::string> types;
  std::vector<MethodRef> methods;
  std::vector<FieldRef> fields;
};
Fixture fixture(FixtureOptions options = {}) {
  std::string owner = options.owner, parent = "Ljava/lang/Object;";
  auto shortType = [](const std::string &t) {
    return t.starts_with('L') || t.starts_with('[') ? "L" : t;
  };
  using Proto = std::pair<std::string, std::vector<std::string>>;
  auto shortyFor = [&](const Proto &proto) {
    std::string shorty = shortType(proto.first);
    for (auto &p : proto.second)
      shorty += shortType(p);
    return shorty;
  };
  MethodRef definition{owner, options.method_name, options.params,
                       options.returns};
  std::vector<MethodRef> method_refs;
  if (options.define_method)
    method_refs.push_back(definition);
  if (options.referenced_method)
    method_refs.push_back(*options.referenced_method);
  method_refs.insert(method_refs.end(), options.extra_methods.begin(),
                     options.extra_methods.end());
  std::set<std::u16string> names{utf16(owner), utf16(parent)};
  std::set<std::string> type_set{owner, parent};
  std::set<FieldRef> field_set(options.extra_fields.begin(),
                               options.extra_fields.end());
  for (const auto &type : options.interfaces) {
    names.insert(utf16(type));
    type_set.insert(type);
  }
  std::set<Proto> proto_set;
  for (const auto &ref : method_refs) {
    names.insert(utf16(ref.name));
    for (const auto &typ : {ref.owner, ref.returns}) {
      names.insert(utf16(typ));
      type_set.insert(typ);
    }
    for (const auto &typ : ref.parameters) {
      names.insert(utf16(typ));
      type_set.insert(typ);
    }
    Proto proto{ref.returns, ref.parameters};
    names.insert(utf16(shortyFor(proto)));
    proto_set.insert(std::move(proto));
  }
  for (auto &extra : options.extras)
    names.insert(extra);
  for (const auto &annotation : options.annotations) {
    names.insert(utf16(annotation.type));
    type_set.insert(annotation.type);
    for (const auto &type : annotation.extra_types) {
      names.insert(utf16(type));
      type_set.insert(type);
    }
    names.insert(annotation.extra_strings.begin(),
                 annotation.extra_strings.end());
    field_set.insert(annotation.extra_fields.begin(),
                     annotation.extra_fields.end());
  }
  if (options.static_value) {
    names.insert(u"VALUE");
    names.insert(utf16(options.field_type));
    type_set.insert(options.field_type);
    field_set.insert({owner, "VALUE", options.field_type});
  }
  for (const auto &field : field_set) {
    names.insert(utf16(field.name));
    for (const auto &type : {field.owner, field.type}) {
      names.insert(utf16(type));
      type_set.insert(type);
    }
  }
  Fixture fixture;
  fixture.strings.assign(names.begin(), names.end());
  auto &out = fixture.data;
  auto &at = fixture.at;
  auto stringIndex = [&](const std::string &s) {
    return unsigned(
        std::find(fixture.strings.begin(), fixture.strings.end(), utf16(s)) -
        fixture.strings.begin());
  };
  auto &types = fixture.types;
  types.assign(type_set.begin(), type_set.end());
  std::sort(types.begin(), types.end(),
            [&](auto &a, auto &b) { return stringIndex(a) < stringIndex(b); });
  auto typeIndex = [&](const std::string &s) {
    return unsigned(std::find(types.begin(), types.end(), s) - types.begin());
  };
  fixture.fields.assign(field_set.begin(), field_set.end());
  std::sort(fixture.fields.begin(), fixture.fields.end(),
            [&](const auto &a, const auto &b) {
              return std::tuple{typeIndex(a.owner), stringIndex(a.name),
                                typeIndex(a.type)} <
                     std::tuple{typeIndex(b.owner), stringIndex(b.name),
                                typeIndex(b.type)};
            });
  const FieldRef defined_field{owner, "VALUE", options.field_type};
  unsigned field_index = unsigned(
      std::find(fixture.fields.begin(), fixture.fields.end(), defined_field) -
      fixture.fields.begin());
  std::vector<Proto> protos(proto_set.begin(), proto_set.end());
  auto protoKey = [&](const Proto &proto) {
    std::vector<unsigned> args;
    for (const auto &arg : proto.second)
      args.push_back(typeIndex(arg));
    return std::pair{typeIndex(proto.first), args};
  };
  std::sort(protos.begin(), protos.end(), [&](const auto &a, const auto &b) {
    return protoKey(a) < protoKey(b);
  });
  auto protoIndex = [&](const MethodRef &ref) {
    return unsigned(std::find(protos.begin(), protos.end(),
                              Proto{ref.returns, ref.parameters}) -
                    protos.begin());
  };
  auto methodKey = [&](const MethodRef &ref) {
    return std::tuple{typeIndex(ref.owner), stringIndex(ref.name),
                      protoIndex(ref)};
  };
  std::sort(method_refs.begin(), method_refs.end(),
            [&](const auto &a, const auto &b) {
              return methodKey(a) < methodKey(b);
            });
  fixture.methods = method_refs;
  auto definition_position =
      std::find(method_refs.begin(), method_refs.end(), definition);
  unsigned definition_index =
      unsigned(definition_position - method_refs.begin());
  std::vector<unsigned> defined_indices;
  if (options.define_method)
    defined_indices.push_back(definition_index);
  if (options.define_referenced_method) {
    if (!options.define_method || !options.referenced_method)
      throw Error("shared code fixture requires two defined methods");
    defined_indices.push_back(
        unsigned(std::find(method_refs.begin(), method_refs.end(),
                           *options.referenced_method) -
                 method_refs.begin()));
    std::sort(defined_indices.begin(), defined_indices.end());
  }
  unsigned incoming = !(options.flags & 8);
  for (auto &p : options.params)
    incoming += p == "J" || p == "D" ? 2 : 1;
  out.resize(112);
  std::vector<std::array<size_t, 3>> sections{{0, 0, 1}};
  auto align = [&] {
    while (out.size() % 4)
      out += '\0';
  };
  auto section = [&](unsigned kind, size_t count, std::string_view raw) {
    size_t offset = out.size();
    out += raw;
    sections.push_back({kind, offset, count});
    return offset;
  };
  at["strings"] = section(1, fixture.strings.size(),
                          std::string(fixture.strings.size() * 4, '\0'));
  std::string raw;
  for (auto &typ : types)
    append(raw, stringIndex(typ), 4);
  at["types"] = section(2, types.size(), raw);
  if (!protos.empty())
    at["protos"] =
        section(3, protos.size(), std::string(12 * protos.size(), '\0'));
  if (!fixture.fields.empty()) {
    raw.clear();
    for (const auto &field : fixture.fields) {
      append(raw, typeIndex(field.owner), 2);
      append(raw, typeIndex(field.type), 2);
      append(raw, stringIndex(field.name), 4);
    }
    at["fields"] = section(4, fixture.fields.size(), raw);
  }
  raw.clear();
  for (const auto &ref : method_refs) {
    append(raw, typeIndex(ref.owner), 2);
    append(raw, protoIndex(ref), 2);
    append(raw, stringIndex(ref.name), 4);
  }
  if (!method_refs.empty())
    at["methods"] = section(5, method_refs.size(), raw);
  if (options.define_method)
    at["defined_method"] = at["methods"] + definition_index * 8;
  const unsigned class_count = options.duplicate_class ? 2 : 1;
  at["class"] = section(6, class_count, std::string(32 * class_count, '\0'));
  size_t data_off = out.size();
  at["string_data"] = out.size();
  for (size_t i = 0; i < fixture.strings.size(); ++i) {
    patch(out, at["strings"] + i * 4, out.size());
    mutf8(out, fixture.strings[i]);
  }
  sections.push_back({0x2002, at["string_data"], fixture.strings.size()});
  align();
  std::map<std::vector<std::string>, size_t> parameter_offsets;
  size_t first_parameters = out.size();
  for (const auto &proto : protos) {
    if (proto.second.empty() || parameter_offsets.contains(proto.second))
      continue;
    parameter_offsets[proto.second] = out.size();
    append(out, proto.second.size(), 4);
    for (const auto &p : proto.second)
      append(out, typeIndex(p), 2);
    align();
  }
  size_t interfaces = 0;
  if (!options.interfaces.empty()) {
    auto found = parameter_offsets.find(options.interfaces);
    if (found != parameter_offsets.end())
      interfaces = found->second;
    else {
      interfaces = out.size();
      parameter_offsets[options.interfaces] = interfaces;
      append(out, options.interfaces.size(), 4);
      for (const auto &type : options.interfaces)
        append(out, typeIndex(type), 2);
      align();
    }
  }
  if (!parameter_offsets.empty())
    sections.push_back({0x1001, first_parameters, parameter_offsets.size()});
  bool no_code = !options.define_method || (options.flags & (0x100 | 0x400));
  size_t code = 0;
  if (!no_code) {
    raw.clear();
    append(raw, options.registers, 2);
    append(raw, incoming, 2);
    append(raw, 255, 2);
    append(raw, options.tries.size(), 2);
    append(raw, 0, 4);
    append(raw, options.words.size(), 4);
    for (auto word : options.words)
      append(raw, word, 2);
    if (!options.tries.empty() && options.words.size() % 2)
      append(raw, 0, 2);
    for (auto t : options.tries) {
      append(raw, t[0], 4);
      append(raw, t[1], 2);
      append(raw, t[2], 2);
    }
    raw += options.handlers;
    size_t second = 0;
    if (options.separate_referenced_code) {
      if (!options.define_referenced_method || !options.referenced_method)
        throw Error("separate code fixture needs referenced declaration");
      while (raw.size() % 4)
        raw += '\0';
      second = raw.size();
      unsigned incoming2 = !(options.flags & 8);
      for (const auto &arg : options.referenced_method->parameters)
        incoming2 += arg == "J" || arg == "D" ? 2 : 1;
      append(raw, options.referenced_registers, 2);
      append(raw, incoming2, 2);
      append(raw, 255, 2);
      append(raw, 0, 2);
      append(raw, 0, 4);
      append(raw, options.referenced_words.size(), 4);
      for (auto word : options.referenced_words)
        append(raw, word, 2);
    }
    code = section(0x2001, second ? 2 : 1, raw);
    at["referenced_code"] = second ? code + second : 0;
  }
  at["code"] = code;
  if (code && !options.debug_info.empty()) {
    at["debug"] = section(0x2003, 1, options.debug_info);
    patch(out, code + 8, at["debug"]);
    if (at["referenced_code"])
      patch(out, at["referenced_code"] + 8, at["debug"]);
  }
  bool direct = options.flags & (8 | 2 | 0x10000);
  raw.clear();
  uleb(raw, bool(options.static_value));
  uleb(raw, 0);
  uleb(raw, direct ? defined_indices.size() : 0);
  uleb(raw, direct ? 0 : defined_indices.size());
  if (options.static_value) {
    uleb(raw, field_index);
    uleb(raw, 0x19);
  }
  unsigned previous_definition = 0;
  for (auto index : defined_indices) {
    uleb(raw, index - previous_definition);
    uleb(raw, options.flags);
    uleb(raw, options.separate_referenced_code && index != definition_index
                  ? at["referenced_code"]
                  : code);
    previous_definition = index;
  }
  if (options.define_method || options.static_value)
    at["class_data"] = section(0x2000, 1, raw);
  if (options.static_value) {
    raw = "\1";
    raw += *options.static_value;
    at["values"] = section(0x2005, 1, raw);
  }
  if (!options.annotations.empty()) {
    at["annotation_items"] = out.size();
    for (size_t i = 0; i < options.annotations.size(); ++i) {
      const auto &annotation = options.annotations[i];
      at["annotation_item_" + std::to_string(i)] = out.size();
      append(out, annotation.visibility, 1);
      uleb(out, typeIndex(annotation.type));
      if (annotation.elements)
        annotation.elements(out, fixture);
      else
        uleb(out, 0);
    }
    sections.push_back(
        {0x2004, at["annotation_items"], options.annotations.size()});
  }
  if (!options.annotation_sets.empty()) {
    align();
    at["annotation_sets"] = out.size();
    for (size_t i = 0; i < options.annotation_sets.size(); ++i) {
      auto entries = options.annotation_sets[i];
      std::sort(entries.begin(), entries.end(), [&](size_t a, size_t b) {
        return typeIndex(options.annotations.at(a).type) <
               typeIndex(options.annotations.at(b).type);
      });
      at["annotation_set_" + std::to_string(i)] = out.size();
      append(out, entries.size(), 4);
      for (size_t entry : entries)
        append(out, at.at("annotation_item_" + std::to_string(entry)), 4);
    }
    sections.push_back(
        {0x1003, at["annotation_sets"], options.annotation_sets.size()});
  }
  auto annotationOffset = [&](std::optional<size_t> set) -> size_t {
    return set ? at.at("annotation_set_" + std::to_string(*set)) : 0;
  };
  if (options.parameter_annotations) {
    align();
    at["parameter_annotations"] = out.size();
    append(out, options.parameter_annotations->size(), 4);
    for (auto slot : *options.parameter_annotations)
      append(out, annotationOffset(slot), 4);
    sections.push_back({0x1002, at["parameter_annotations"], 1});
  }
  if (options.class_annotations || options.field_annotations ||
      options.method_annotations || options.parameter_annotations) {
    if (options.field_annotations && !options.static_value)
      throw Error("annotation fixture needs a defined field");
    if ((options.method_annotations || options.parameter_annotations) &&
        !options.define_method)
      throw Error("annotation fixture needs a defined method");
    align();
    at["annotations"] = out.size();
    append(out, annotationOffset(options.class_annotations), 4);
    append(out, bool(options.field_annotations), 4);
    append(out, bool(options.method_annotations), 4);
    append(out, bool(options.parameter_annotations), 4);
    if (options.field_annotations) {
      append(out, field_index, 4);
      append(out, annotationOffset(options.field_annotations), 4);
    }
    if (options.method_annotations) {
      append(out, definition_index, 4);
      append(out, annotationOffset(options.method_annotations), 4);
    }
    if (options.parameter_annotations) {
      append(out, definition_index, 4);
      append(out, at.at("parameter_annotations"), 4);
    }
    sections.push_back({0x2006, at["annotations"], 1});
  }
  align();
  at["map"] = out.size();
  sections.push_back({0x1000, out.size(), 1});
  append(out, sections.size(), 4);
  for (auto s : sections) {
    append(out, s[0], 2);
    append(out, 0, 2);
    append(out, s[2], 4);
    append(out, s[1], 4);
  }
  out.replace(0, 8, "dex\n" + options.version + std::string(1, '\0'));
  patch(out, 32, out.size());
  patch(out, 36, 112);
  patch(out, 40, 0x12345678);
  patch(out, 52, at["map"]);
  for (auto [kind, count, offset] : std::vector<std::array<size_t, 3>>{
           {1, fixture.strings.size(), at["strings"]},
           {2, types.size(), at["types"]},
           {3, protos.size(), at["protos"]},
           {4, fixture.fields.size(), at["fields"]},
           {5, method_refs.size(), at["methods"]},
           {6, class_count, at["class"]}}) {
    patch(out, 56 + (kind - 1) * 8, count);
    patch(out, 60 + (kind - 1) * 8, offset);
  }
  patch(out, 104, out.size() - data_off);
  patch(out, 108, data_off);
  for (size_t i = 0; i < protos.size(); ++i) {
    const auto &proto = protos[i];
    patch(out, at["protos"] + 12 * i, stringIndex(shortyFor(proto)));
    patch(out, at["protos"] + 12 * i + 4, typeIndex(proto.first));
    patch(out, at["protos"] + 12 * i + 8,
          proto.second.empty() ? 0 : parameter_offsets.at(proto.second));
  }
  std::array<uint32_t, 8> cls{typeIndex(owner),
                              options.class_flags,
                              typeIndex(parent),
                              uint32_t(interfaces),
                              UINT32_MAX,
                              uint32_t(at["annotations"]),
                              uint32_t(at["class_data"]),
                              uint32_t(at["values"])};
  for (size_t i = 0; i < cls.size(); ++i)
    patch(out, at["class"] + i * 4, cls[i]);
  if (options.duplicate_class)
    out.replace(at["class"] + 32, 32, out.substr(at["class"], 32));
  out = seal(std::move(out));
  return fixture;
}
std::string storedDexArchive(
    const std::vector<std::pair<std::string, std::string>> &members) {
  std::string result, directory;
  for (const auto &[name, bytes] : members) {
    const auto crc =
        crc32(0, reinterpret_cast<const Bytef *>(bytes.data()), bytes.size());
    const auto local_offset = result.size();
    std::string local(30, '\0');
    patch(local, 0, 0x04034b50);
    patch(local, 4, 20, 2);
    patch(local, 6, 0x800, 2);
    patch(local, 14, crc);
    patch(local, 18, bytes.size());
    patch(local, 22, bytes.size());
    patch(local, 26, name.size(), 2);
    result += local + name + bytes;
    std::string central(46, '\0');
    patch(central, 0, 0x02014b50);
    patch(central, 4, 0x314, 2);
    patch(central, 6, 20, 2);
    patch(central, 8, 0x800, 2);
    patch(central, 16, crc);
    patch(central, 20, bytes.size());
    patch(central, 24, bytes.size());
    patch(central, 28, name.size(), 2);
    patch(central, 38, 0100644u << 16);
    patch(central, 42, local_offset);
    directory += central + name;
  }
  std::string end(22, '\0');
  patch(end, 0, 0x06054b50);
  patch(end, 8, members.size(), 2);
  patch(end, 10, members.size(), 2);
  patch(end, 12, directory.size());
  patch(end, 16, result.size());
  return result + directory + end;
}
std::vector<Class> parse(const std::string &data) {
  Budget budget;
  return parseDex(data, "classes2.dex", budget);
}
struct DexRecoveryDirectory {
  fs::path path;
  DexRecoveryDirectory() {
    llvm::SmallString<256> created;
    auto prefix = pathText(fs::temp_directory_path() / "neverd-dex-budget");
    if (auto error = llvm::sys::fs::createUniqueDirectory(prefix, created))
      throw Error("cannot create DEX recovery fixture: " + error.message());
    path = pathFromUTF8(created.str().str());
  }
  ~DexRecoveryDirectory() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};
Fixture stringReturningFixture(const std::u16string &literal) {
  FixtureOptions options;
  options.params.clear();
  options.returns = "Ljava/lang/String;";
  options.words = {0x001a, 0, 0x0011};
  options.extras = {literal};
  auto f = fixture(options);
  auto found = std::find(f.strings.begin(), f.strings.end(), literal);
  EXPECT_NE(found, f.strings.end());
  patch(f.data, f.at["code"] + 18, found - f.strings.begin(), 2);
  f.data = seal(std::move(f.data));
  return f;
}
void expectDexError(const std::string &data, std::string_view reason) {
  try {
    (void)parse(data);
    ADD_FAILURE() << "Expected DEX rejection: " << reason;
  } catch (const Error &error) {
    EXPECT_EQ(std::string_view(error.what()), reason);
  }
}
unsigned fixtureTypeIndex(const Fixture &f, const std::string &type) {
  auto found = std::find(f.types.begin(), f.types.end(), type);
  EXPECT_NE(found, f.types.end());
  return unsigned(found - f.types.begin());
}
unsigned fixtureStringIndex(const Fixture &f, const std::string &text) {
  auto found = std::find(f.strings.begin(), f.strings.end(), utf16(text));
  EXPECT_NE(found, f.strings.end());
  return unsigned(found - f.strings.begin());
}
Fixture referenceFixture() {
  FixtureOptions options;
  options.params = {};
  options.returns = "V";
  options.registers = 2;
  options.extras = {u"needle", u"a needle suffix"};
  options.referenced_method = MethodRef{"Lfixture/Target;", "call", {}, "V"};
  options.extra_methods = {{"Lfixture/Target;", "call", {"I"}, "V"},
                           {"Lother/Target;", "call", {}, "V"}};
  options.extra_fields = {{"Lfixture/Target;", "VALUE", "I"},
                          {"Lfixture/Target;", "VALUE", "J"},
                          {"Lother/Target;", "VALUE", "I"}};
  // Apparent references at PCs 1, 4, 6 and 9 are immediate words. Actual
  // reference instructions begin at PCs 11, 13, 15 and 17.
  options.words = {0x0014, 0x001a, 0,      0x0018, 0x0071, 0, 0x0060,
                   0,      0x0014, 0x001c, 0,      0x001a, 0, 0x001c,
                   0,      0x0060, 0,      0x0071, 0,      0, 0x000e};
  auto f = fixture(options);
  const auto method_index = std::find(f.methods.begin(), f.methods.end(),
                                      *options.referenced_method) -
                            f.methods.begin();
  const auto field_index =
      std::find(f.fields.begin(), f.fields.end(), options.extra_fields[0]) -
      f.fields.begin();
  for (auto [pc, index] : std::vector<std::pair<size_t, size_t>>{
           {2, fixtureStringIndex(f, "needle")},
           {5, size_t(method_index)},
           {7, size_t(field_index)},
           {10, fixtureTypeIndex(f, "Lfixture/Target;")},
           {12, fixtureStringIndex(f, "needle")},
           {14, fixtureTypeIndex(f, "Lfixture/Target;")},
           {16, size_t(field_index)},
           {18, size_t(method_index)}})
    patch(f.data, f.at["code"] + 16 + pc * 2, index, 2);
  f.data = seal(std::move(f.data));
  return f;
}
DexReferenceResult references(const Fixture &f, DexReferenceKind kind,
                              std::string text, bool exact = false,
                              std::optional<std::string> owner = {}) {
  Budget budget;
  return findDexReferences(
      f.data, {kind, std::move(text), exact, std::move(owner)}, budget);
}
void annotationIndex(std::string &out, unsigned kind, unsigned index) {
  // A four-byte unsigned pool index, encoded with value_arg == 3.
  append(out, kind | 0x60, 1);
  append(out, index, 4);
}
FixtureAnnotation enumAnnotation(std::string type, std::vector<FieldRef> values,
                                 bool array, unsigned kind = 0x1b) {
  FixtureAnnotation result{std::move(type), 1};
  result.extra_fields = values;
  result.extra_strings = {u"value"};
  for (const auto &value : values)
    result.extra_strings.push_back(
        utf16(value.owner + "->" + value.name + ":" + value.type));
  result.elements = [values, array, kind](std::string &out, const Fixture &f) {
    uleb(out, 1);
    uleb(out, fixtureStringIndex(f, "value"));
    if (array) {
      append(out, 0x1c, 1);
      uleb(out, values.size());
    }
    for (const auto &value : values) {
      auto found = std::find(f.fields.begin(), f.fields.end(), value);
      ASSERT_NE(found, f.fields.end());
      unsigned index = unsigned(found - f.fields.begin());
      if (kind == 0x17)
        index = fixtureStringIndex(f, value.owner + "->" + value.name + ":" +
                                          value.type);
      else if (kind == 0x18)
        index = fixtureTypeIndex(f, value.owner);
      annotationIndex(out, kind, index);
    }
  };
  return result;
}
FixtureAnnotation retentionAnnotation(std::string policy = "RUNTIME") {
  const std::string type = "Ljava/lang/annotation/RetentionPolicy;";
  return enumAnnotation("Ljava/lang/annotation/Retention;",
                        {{type, std::move(policy), type}}, false);
}
FixtureAnnotation targetAnnotation(std::vector<std::string> targets) {
  const std::string type = "Ljava/lang/annotation/ElementType;";
  std::vector<FieldRef> values;
  for (const auto &target : targets)
    values.push_back({type, target, type});
  return enumAnnotation("Ljava/lang/annotation/Target;", std::move(values),
                        true);
}
FixtureOptions markerOptions(std::vector<FixtureAnnotation> annotations = {}) {
  FixtureOptions options;
  options.owner = "Lfixture/ZMarker;";
  options.class_flags = 0x2601;
  options.interfaces = {"Ljava/lang/annotation/Annotation;"};
  options.define_method = false;
  options.annotations = std::move(annotations);
  if (!options.annotations.empty()) {
    options.annotation_sets.emplace_back();
    for (size_t i = 0; i < options.annotations.size(); ++i)
      options.annotation_sets[0].push_back(i);
    options.class_annotations = 0;
  }
  return options;
}
ClassMap linkedDex(const FixtureOptions &options) {
  auto classes = parse(fixture(options).data);
  Budget budget;
  return linkClasses(std::move(classes), budget);
}
FixtureAnnotation enclosingClassAnnotation() {
  FixtureAnnotation result{"Ldalvik/annotation/EnclosingClass;", 2};
  result.extra_types = {"Lfixture/Outer;"};
  result.extra_strings = {u"value"};
  result.elements = [](std::string &out, const Fixture &f) {
    uleb(out, 1);
    uleb(out, fixtureStringIndex(f, "value"));
    annotationIndex(out, 0x18, fixtureTypeIndex(f, "Lfixture/Outer;"));
  };
  return result;
}
FixtureAnnotation signatureAnnotation(std::vector<std::string> pieces) {
  FixtureAnnotation result{"Ldalvik/annotation/Signature;", 2};
  result.extra_strings = {u"value"};
  for (const auto &piece : pieces)
    result.extra_strings.push_back(utf16(piece));
  result.elements = [pieces](std::string &out, const Fixture &f) {
    uleb(out, 1);
    uleb(out, fixtureStringIndex(f, "value"));
    append(out, 0x1c, 1);
    uleb(out, pieces.size());
    for (const auto &piece : pieces)
      annotationIndex(out, 0x17, fixtureStringIndex(f, piece));
  };
  return result;
}
FixtureAnnotation innerClassAnnotation(unsigned access = 9) {
  FixtureAnnotation result{"Ldalvik/annotation/InnerClass;", 2};
  result.extra_strings = {u"accessFlags", u"name", u"Nested"};
  result.elements = [access](std::string &out, const Fixture &f) {
    uleb(out, 2);
    uleb(out, fixtureStringIndex(f, "accessFlags"));
    append(out, 0x64, 1); // VALUE_INT, four bytes.
    append(out, access, 4);
    uleb(out, fixtureStringIndex(f, "name"));
    annotationIndex(out, 0x17, fixtureStringIndex(f, "Nested"));
  };
  return result;
}
FixtureAnnotation
typeArrayAnnotation(std::string type = "Ldalvik/annotation/MemberClasses;",
                    std::vector<std::string> values = {}) {
  FixtureAnnotation result{std::move(type), 2};
  result.extra_types = values;
  result.extra_strings = {u"value"};
  result.elements = [values](std::string &out, const Fixture &f) {
    uleb(out, 1);
    uleb(out, fixtureStringIndex(f, "value"));
    append(out, 0x1c, 1); // VALUE_ARRAY.
    uleb(out, values.size());
    for (const auto &value : values)
      annotationIndex(out, 0x18, fixtureTypeIndex(f, value));
  };
  return result;
}
FixtureAnnotation nestedAnnotation(std::string nested_type) {
  FixtureAnnotation result{"Lfixture/Unknown;", 1};
  result.extra_types = {nested_type};
  result.extra_strings = {u"value"};
  result.elements = [nested_type](std::string &out, const Fixture &f) {
    uleb(out, 1);
    uleb(out, fixtureStringIndex(f, "value"));
    append(out, 0x1d, 1); // VALUE_ANNOTATION has no visibility byte.
    uleb(out, fixtureTypeIndex(f, nested_type));
    uleb(out, 0);
  };
  return result;
}
FixtureAnnotation enclosingMethodAnnotation(const MethodRef &method) {
  FixtureAnnotation result{"Ldalvik/annotation/EnclosingMethod;", 2};
  result.extra_strings = {u"value"};
  result.elements = [method](std::string &out, const Fixture &f) {
    auto found = std::find(f.methods.begin(), f.methods.end(), method);
    EXPECT_NE(found, f.methods.end());
    uleb(out, 1);
    uleb(out, fixtureStringIndex(f, "value"));
    annotationIndex(out, 0x1a, unsigned(found - f.methods.begin()));
  };
  return result;
}
void attachFixtureAnnotation(FixtureOptions &options,
                             std::string_view attachment, size_t set = 0) {
  if (attachment == "class")
    options.class_annotations = set;
  else if (attachment == "field") {
    options.static_value = std::string("\x04\x00", 2);
    options.field_annotations = set;
  } else if (attachment == "method")
    options.method_annotations = set;
  else if (attachment == "parameter") {
    std::vector<std::optional<size_t>> parameters(options.params.size());
    parameters.at(0) = set;
    options.parameter_annotations = std::move(parameters);
  } else
    throw Error("unknown annotation fixture attachment");
}
// The first instruction is an invoke whose method index is patched from the
// actual sorted method table, not an assumed index or an unused reference.
Fixture invokingFixture(const MethodRef &callee, uint8_t opcode = 0x6e) {
  FixtureOptions o;
  o.params = {callee.owner};
  o.returns = callee.returns;
  o.registers = 2;
  o.referenced_method = callee;
  o.words = {uint16_t(0x1000 | opcode), 0, 1};
  if (callee.returns == "V")
    o.words.push_back(0x000e);
  else if (callee.returns.starts_with('L') || callee.returns.starts_with('[')) {
    o.words.push_back(0x000c);
    o.words.push_back(0x0011);
  } else {
    o.words.push_back(0x000a);
    o.words.push_back(0x000f);
  }
  auto f = fixture(o);
  auto found = std::find(f.methods.begin(), f.methods.end(), callee);
  EXPECT_NE(found, f.methods.end());
  patch(f.data, f.at["code"] + 18, found - f.methods.begin(), 2);
  f.data = seal(std::move(f.data));
  return f;
}
Class smali(std::string_view body) {
  Budget budget;
  return parseSmali(body, "owned.smali", budget);
}
std::string methodText(std::string body, std::string signature = "value(I)I",
                       unsigned registers = 4) {
  return ".class public Lfixture/Test;\n.super Ljava/lang/Object;\n.method "
         "public static " +
         signature + "\n.registers " + std::to_string(registers) + "\n" + body +
         "\n.end method\n";
}
Class localClassModel(const std::string &owner, const MethodRef &enclosing,
                      std::string_view source = "local.smali",
                      bool anonymous = false) {
  const std::string text =
      ".class final " + owner +
      "\n.super Ljava/lang/Object;\n"
      ".annotation system Ldalvik/annotation/EnclosingMethod;\nvalue = " +
      enclosing.identity() +
      "\n.end annotation\n"
      ".annotation system Ldalvik/annotation/InnerClass;\nname = " +
      (anonymous ? "null" : "\"Worker\"") +
      "\naccessFlags = 0x10\n.end annotation\n"
      ".method constructor <init>()V\n.registers 2\n"
      "invoke-direct {p0},Ljava/lang/Object;-><init>()V\n"
      "sget v0,Lfixture/Outer;->count:I\nadd-int/lit8 v0,v0,1\n"
      "sput v0,Lfixture/Outer;->count:I\nreturn-void\n.end method\n"
      ".method public apply(I)I\n.registers 2\n"
      "add-int/lit8 v0,p1,7\nreturn v0\n.end method\n";
  Budget budget;
  return parseSmali(text, source, budget);
}
Class localOwnerModel(const std::string &method_name,
                      const std::string &local_owner,
                      std::string_view source = "outer.smali") {
  const std::string text =
      ".class public Lfixture/Outer;\n.super Ljava/lang/Object;\n"
      ".field public static count:I\n.method public static " +
      method_name + "(I)I\n.registers 2\nnew-instance v0," + local_owner +
      "\ninvoke-direct {v0}," + local_owner +
      "-><init>()V\ninvoke-virtual {v0,p0}," + local_owner +
      "->apply(I)I\nmove-result v0\nreturn v0\n.end method\n";
  Budget budget;
  return parseSmali(text, source, budget);
}
void expectLocalScopeError(std::vector<Class> classes, const Class &local,
                           std::string_view reason,
                           bool direct_scope_api = false) {
  Budget budget;
  try {
    if (direct_scope_api) {
      ClassMap map;
      for (auto &cls : classes) {
        auto name = cls.name;
        map.emplace(std::move(name), std::move(cls));
      }
      validateSourceScopes(map, budget);
    } else {
      (void)linkClasses(std::move(classes), budget);
    }
    ADD_FAILURE() << "Expected local source rejection: " << reason;
  } catch (const Error &error) {
    const std::string message = error.what();
    EXPECT_NE(message.find(reason), std::string::npos) << message;
    EXPECT_NE(message.find(local.name), std::string::npos) << message;
    EXPECT_NE(message.find(local.source_id), std::string::npos) << message;
    if (local.enclosing_method)
      EXPECT_NE(message.find(local.enclosing_method->identity()),
                std::string::npos)
          << message;
  }
}
TEST(MobileDalvikReader, DexVersionsAndExactMethodInventory) {
  for (auto version : {"035", "037", "038", "039", "040"}) {
    FixtureOptions o;
    o.version = version;
    auto result = parse(fixture(o).data);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].source_id, "classes2.dex");
    auto &m = result[0].methods[0];
    EXPECT_EQ(m.reference.identity(), "Lfixture/Sample;->value(I)I");
    ASSERT_EQ(m.instructions.size(), 1u);
    EXPECT_EQ(m.instructions[0].opcode, "return");
    EXPECT_EQ(m.instructions[0].registers, std::vector<unsigned>{0});
  }
}
TEST(MobileDalvikReader,
     DexClassInventorySkipsUnsupportedBodiesAndUnusedStrings) {
  for (auto version : {"035", "037", "038", "039", "040"}) {
    FixtureOptions options;
    options.version = version;
    options.words = {0xffff};
    options.extras = {std::u16string(40000, u'x')};
    auto f = fixture(options);
    Budget budget;
    // The integrity scan and selected class metadata fit; decoding the unused
    // string or attempting the unsupported method does not.
    budget.remaining = 10000;
    EXPECT_EQ(listDexClasses(f.data, budget),
              std::vector<std::string>{"Lfixture/Sample;"});
    EXPECT_GT(budget.remaining, 0u);
    EXPECT_THROW(parse(f.data), Error);
  }
}
TEST(MobileDalvikReader, DexClassInventoryPreservesUnicodeDescriptors) {
  FixtureOptions options;
  options.extras = {u"Lfixture/\u03bb\U0001f600;"};
  auto f = fixture(options);
  const auto class_type =
      std::find(f.types.begin(), f.types.end(), options.owner) -
      f.types.begin();
  const auto unicode_string =
      std::find(f.strings.begin(), f.strings.end(), options.extras[0]) -
      f.strings.begin();
  patch(f.data, f.at["types"] + class_type * 4, unicode_string);
  f.data = seal(std::move(f.data));
  Budget budget;
  const std::string name = "Lfixture/\xce\xbb\xf0\x9f\x98\x80;";
  EXPECT_EQ(listDexClasses(f.data, budget), std::vector<std::string>{name});
  EXPECT_EQ(parse(f.data)[0].name, name);
}
TEST(MobileDalvikReader, DexClassInventoryKeepsDefinitionOrder) {
  FixtureOptions options;
  options.duplicate_class = true;
  options.referenced_method = MethodRef{"Lfixture/Other;", "unused", {}, "V"};
  auto f = fixture(options);
  patch(f.data, f.at["class"] + 32, fixtureTypeIndex(f, "Lfixture/Other;"));
  patch(f.data, f.at["class"] + 32 + 24, 0);
  f.data = seal(std::move(f.data));
  Budget budget;
  EXPECT_EQ(listDexClasses(f.data, budget),
            (std::vector<std::string>{"Lfixture/Sample;", "Lfixture/Other;"}));
  EXPECT_EQ(parse(f.data).size(), 2u);
}
TEST(MobileDalvikReader, DexClassInventoryRejectsInvalidReferencedMetadata) {
  auto f = fixture();
  const auto class_type =
      std::find(f.types.begin(), f.types.end(), "Lfixture/Sample;") -
      f.types.begin();
  const auto class_string =
      std::find(f.strings.begin(), f.strings.end(), u"Lfixture/Sample;") -
      f.strings.begin();
  const auto primitive_type =
      std::find(f.types.begin(), f.types.end(), "I") - f.types.begin();
  for (auto [offset, value] : std::vector<std::pair<size_t, uint64_t>>{
           {f.at["class"], UINT32_MAX},
           {f.at["class"], uint64_t(primitive_type)},
           {f.at["class"] + 4, 0x80000000},
           {f.at["class"] + 8, uint64_t(class_type)},
           {f.at["class"] + 8, uint64_t(primitive_type)},
           {f.at["class"] + 12, f.at["string_data"]},
           {f.at["class"] + 16, f.strings.size()},
           {f.at["class"] + 20, f.at["string_data"]},
           {f.at["class"] + 24, f.at["string_data"]},
           {f.at["class"] + 28, f.at["string_data"]},
           {f.at["types"] + class_type * 4, UINT32_MAX},
           {f.at["strings"] + class_string * 4, 0},
           {f.at["map"] + 8, 0}}) {
    auto broken = f.data;
    patch(broken, offset, value);
    Budget budget;
    EXPECT_THROW(listDexClasses(seal(std::move(broken)), budget), Error)
        << "offset=" << offset << " value=" << value;
  }
  uint32_t string_offset = 0;
  for (unsigned i = 0; i < 4; ++i)
    string_offset |=
        uint32_t(uint8_t(f.data[f.at["strings"] + class_string * 4 + i]))
        << (i * 8);
  for (auto invalid : {char(0xf0), '.'}) {
    auto broken = f.data;
    broken[string_offset + 2] = invalid;
    Budget budget;
    EXPECT_THROW(listDexClasses(seal(std::move(broken)), budget), Error);
  }
  FixtureOptions duplicate;
  duplicate.duplicate_class = true;
  Budget duplicate_budget;
  EXPECT_THROW(listDexClasses(fixture(duplicate).data, duplicate_budget),
               Error);
  FixtureOptions interface;
  interface.interfaces = {"I"};
  Budget interface_budget;
  EXPECT_THROW(listDexClasses(fixture(interface).data, interface_budget),
               Error);
}
TEST(MobileDalvikReader, DexClassInventoryRetainsIntegrityAndResourceBounds) {
  auto f = fixture();
  auto corrupted = f.data;
  corrupted.back() ^= 1;
  Budget integrity;
  EXPECT_THROW(listDexClasses(corrupted, integrity), Error);
  for (auto version : {"036", "041", "999"}) {
    auto broken = f.data;
    broken.replace(4, 3, version);
    Budget budget;
    EXPECT_THROW(listDexClasses(seal(std::move(broken)), budget), Error);
  }
  Budget work;
  work.remaining = 1;
  EXPECT_THROW(listDexClasses(f.data, work), Error);
  Budget time;
  time.deadline = std::chrono::steady_clock::time_point::min();
  EXPECT_THROW(listDexClasses(f.data, time), Error);
  Limits limits;
  limits.max_bytes = f.data.size() - 1;
  Budget bytes(limits);
  EXPECT_THROW(listDexClasses(f.data, bytes), Error);
  FixtureOptions duplicate;
  duplicate.duplicate_class = true;
  limits = Limits{};
  limits.max_files = 1;
  Budget files(limits);
  EXPECT_THROW(listDexClasses(fixture(duplicate).data, files), Error);
}
TEST(MobileDalvikReader,
     DexClassInventoryCLIReportsScopeAndFiltersWithoutRecovery) {
  DexRecoveryDirectory temporary;
  const auto source = temporary.path / "unsupported.dex";
  FixtureOptions options;
  options.words = {0xffff};
  const auto f = fixture(options);
  writeFile(source, f.data);
  EXPECT_THROW(parse(f.data), Error);
  unsigned invocation = 0;
  auto invoke = [&](std::initializer_list<std::string> extra) {
    std::vector<std::string> command{NEVERD_MOBILE_CLI, "mobile",
                                     pathText(source), "--list-classes"};
    command.insert(command.end(), extra.begin(), extra.end());
    const auto log = temporary.path / (std::to_string(invocation++) + ".log");
    runTool(command, log, 30);
    return readFile(log, 100000);
  };
  EXPECT_EQ(invoke({}), "Lfixture/Sample;\n");
  EXPECT_EQ(invoke({"--class-prefix=Lfixture/"}), "Lfixture/Sample;\n");
  EXPECT_EQ(invoke({"--class-prefix=fixture.Sample"}), "Lfixture/Sample;\n");
  EXPECT_EQ(invoke({"--class-prefix=other."}), "");
  auto report = parseJSON(invoke({"--json"}), "class inventory");
  const auto *object = report.getAsObject();
  ASSERT_NE(object, nullptr);
  EXPECT_EQ(object->getString("status"), "success");
  EXPECT_EQ(object->getString("validation_scope"),
            "dex-envelope-and-class-identities");
  EXPECT_EQ(object->getBoolean("method_bodies_validated"), false);
  EXPECT_EQ(object->getBoolean("unselected_zip_payloads_validated"), false);
  EXPECT_EQ(object->getInteger("class_count"), 1);
  EXPECT_EQ(object->getInteger("total_class_count"), 1);
  EXPECT_EQ(object->getInteger("dex_count"), 1);
  const auto *classes = object->getArray("classes");
  ASSERT_NE(classes, nullptr);
  ASSERT_EQ(classes->size(), 1u);
  EXPECT_EQ((*classes)[0].getAsString(), "Lfixture/Sample;");
  auto filtered = parseJSON(invoke({"--json", "--class-prefix=other."}),
                            "filtered class inventory");
  const auto *filtered_object = filtered.getAsObject();
  ASSERT_NE(filtered_object, nullptr);
  EXPECT_EQ(filtered_object->getInteger("class_count"), 0);
  EXPECT_EQ(filtered_object->getInteger("total_class_count"), 1);
  EXPECT_EQ(readFile(source, 100000), f.data);
}
TEST(MobileDalvikReader,
     DexClassInventoryCLICreatesRelativeFileWithoutOverwrite) {
  DexRecoveryDirectory temporary;
  const auto source = temporary.path / "sample.dex";
  writeFile(source, fixture().data);
  struct RestoreDirectory {
    fs::path previous = fs::current_path();
    ~RestoreDirectory() {
      std::error_code error;
      fs::current_path(previous, error);
      EXPECT_FALSE(error);
    }
  } restore;
  fs::current_path(temporary.path);
  const std::vector<std::string> command{
      NEVERD_MOBILE_CLI, "mobile", pathText(source),
      "--list-classes",  "-o",     "classes.txt"};
  runTool(command, temporary.path / "first.log", 30);
  const auto output = temporary.path / "classes.txt";
  EXPECT_EQ(readFile(output, 100000), "Lfixture/Sample;\n");
  EXPECT_THROW(runTool(command, temporary.path / "second.log", 30), Error);
  EXPECT_EQ(readFile(output, 100000), "Lfixture/Sample;\n");
}
TEST(MobileDalvikReader,
     DexClassInventoryCLIRejectsUnrepresentablePublication) {
  FixtureOptions options;
  options.extras = {std::u16string(u"Lfixture/") + char16_t(0xd800) + u";"};
  auto f = fixture(options);
  const auto class_type = fixtureTypeIndex(f, options.owner);
  const auto surrogate_string =
      std::find(f.strings.begin(), f.strings.end(), options.extras[0]) -
      f.strings.begin();
  patch(f.data, f.at["types"] + class_type * 4, surrogate_string);
  f.data = seal(std::move(f.data));
  Budget model;
  EXPECT_EQ(listDexClasses(f.data, model),
            std::vector<std::string>{"Lfixture/\xed\xa0\x80;"});
  DexRecoveryDirectory temporary;
  const auto source = temporary.path / "surrogate.dex";
  const auto output = temporary.path / "classes.txt";
  const auto log = temporary.path / "failure.log";
  writeFile(source, f.data);
  const std::vector<std::string> command{
      NEVERD_MOBILE_CLI, "mobile", pathText(source), "--list-classes",
      "--json",          "-o",     pathText(output)};
  EXPECT_THROW(runTool(command, log, 30), Error);
  auto report = parseJSON(readFile(log, 100000), "class inventory failure");
  ASSERT_NE(report.getAsObject(), nullptr);
  EXPECT_EQ(report.getAsObject()->getString("status"), "error");
  EXPECT_FALSE(fs::exists(output));
}
TEST(MobileDalvikReader, DexClassInventoryCLIAcceptsUppercaseLDottedPackages) {
  FixtureOptions options;
  options.owner = "LLibrary/example/Sample;";
  DexRecoveryDirectory temporary;
  const auto source = temporary.path / "library.dex";
  const auto log = temporary.path / "inventory.log";
  writeFile(source, fixture(options).data);
  runTool({NEVERD_MOBILE_CLI, "mobile", pathText(source), "--list-classes",
           "--class-prefix=Library.example"},
          log, 30);
  EXPECT_EQ(readFile(log, 100000), "LLibrary/example/Sample;\n");
}
TEST(MobileDalvikReader,
     DexClassInventoryAPKIncludesOnlyRootMultidexDefinitions) {
  FixtureOptions other;
  other.owner = "Lfixture/Other;";
  DexRecoveryDirectory temporary;
  const auto source = temporary.path / "multidex.apk";
  writeFile(source,
            storedDexArchive({{"assets/classes.dex", "not a DEX"},
                              {"classes.dex", fixture().data},
                              {"classes2.dex", fixture(other).data},
                              {"classes1.dex", "not a root code name"},
                              {"classes01.dex", "not a root code name"}}));
  Budget budget;
  const auto inventory = listAndroidClasses(source, {}, budget);
  EXPECT_EQ(inventory.dex_count, 2u);
  EXPECT_EQ(inventory.total_class_count, 2u);
  EXPECT_EQ(inventory.classes,
            (std::vector<std::string>{"Lfixture/Sample;", "Lfixture/Other;"}));
  const auto log = temporary.path / "inventory.log";
  runTool({NEVERD_MOBILE_CLI, "mobile", pathText(source), "--list-classes"},
          log, 30);
  EXPECT_EQ(readFile(log, 100000), "Lfixture/Sample;\nLfixture/Other;\n");
}
TEST(MobileDalvikReader,
     DexClassInventoryAPKRejectsFilteredCrossDexDuplicates) {
  const auto f = fixture();
  DexRecoveryDirectory temporary;
  const auto source = temporary.path / "duplicate.apk";
  const auto output = temporary.path / "classes.txt";
  const auto log = temporary.path / "failure.log";
  writeFile(source, storedDexArchive(
                        {{"classes.dex", f.data}, {"classes2.dex", f.data}}));
  Budget budget;
  EXPECT_THROW(listAndroidClasses(source, "other.", budget), Error);
  const std::vector<std::string> command{NEVERD_MOBILE_CLI,
                                         "mobile",
                                         pathText(source),
                                         "--list-classes",
                                         "--class-prefix=other.",
                                         "--json",
                                         "-o",
                                         pathText(output)};
  EXPECT_THROW(runTool(command, log, 30), Error);
  auto report = parseJSON(readFile(log, 100000), "duplicate class inventory");
  ASSERT_NE(report.getAsObject(), nullptr);
  EXPECT_EQ(report.getAsObject()->getString("status"), "error");
  auto error = report.getAsObject()->getString("error");
  ASSERT_TRUE(error);
  EXPECT_NE(error->find("duplicate Android class definition"),
            llvm::StringRef::npos);
  EXPECT_FALSE(fs::exists(output));
}
TEST(MobileDalvikReader, DexClassInventoryAPKBoundsTheAggregateClassCount) {
  FixtureOptions first;
  first.duplicate_class = true;
  first.referenced_method = MethodRef{"Lfixture/Other;", "unused", {}, "V"};
  auto f = fixture(first);
  patch(f.data, f.at["class"] + 32, fixtureTypeIndex(f, "Lfixture/Other;"));
  patch(f.data, f.at["class"] + 32 + 24, 0);
  f.data = seal(std::move(f.data));
  FixtureOptions second;
  second.owner = "Lfixture/Third;";
  DexRecoveryDirectory temporary;
  const auto source = temporary.path / "three-classes.apk";
  const auto output = temporary.path / "classes.txt";
  writeFile(source, storedDexArchive({{"classes.dex", f.data},
                                      {"classes2.dex", fixture(second).data}}));
  Limits limits;
  limits.max_files = 2;
  Budget budget(limits);
  try {
    (void)listAndroidClasses(source, {}, budget);
    FAIL() << "Two DEX entries must not bypass the three-class inventory limit";
  } catch (const Error &error) {
    EXPECT_STREQ(error.what(),
                 "Android class inventory exceeds the file limit");
  }
  const std::vector<std::string> command{
      NEVERD_MOBILE_CLI, "mobile", pathText(source), "--list-classes",
      "--max-files=2",   "-o",     pathText(output)};
  EXPECT_THROW(runTool(command, temporary.path / "failure.log", 30), Error);
  EXPECT_FALSE(fs::exists(output));
}
TEST(MobileDalvikReader, DexClassInventoryCLIRejectsConflictingOptions) {
  DexRecoveryDirectory temporary;
  const auto source = temporary.path / "sample.dex";
  const auto output = temporary.path / "classes.txt";
  writeFile(source, fixture().data);
  unsigned invocation = 0;
  for (const auto *conflict :
       {"--platform=ios", "--arch=x86_64", "--metadata-only",
        "--jadx=missing-backend", "--artifact=App", "--max-func=1"}) {
    const auto log = temporary.path / (std::to_string(invocation++) + ".log");
    const std::vector<std::string> command{NEVERD_MOBILE_CLI,
                                           "mobile",
                                           pathText(source),
                                           "--list-classes",
                                           conflict,
                                           "--json",
                                           "-o",
                                           pathText(output)};
    EXPECT_THROW(runTool(command, log, 30), Error) << conflict;
    auto report = parseJSON(readFile(log, 100000), "conflicting query options");
    ASSERT_NE(report.getAsObject(), nullptr);
    EXPECT_EQ(report.getAsObject()->getString("status"), "error");
    EXPECT_FALSE(fs::exists(output));
  }
  const auto log = temporary.path / "prefix-only.log";
  const std::vector<std::string> command{
      NEVERD_MOBILE_CLI, "mobile", pathText(source), "--class-prefix=fixture.",
      "--json",          "-o",     pathText(output)};
  EXPECT_THROW(runTool(command, log, 30), Error);
  auto report = parseJSON(readFile(log, 100000), "prefix without inventory");
  ASSERT_NE(report.getAsObject(), nullptr);
  EXPECT_EQ(report.getAsObject()->getString("error"),
            "--class-prefix requires --list-classes");
  EXPECT_FALSE(fs::exists(output));
}
TEST(MobileDalvikReader, DexReferencesUseOnlyDecodedOperandBoundaries) {
  const auto f = referenceFixture();
  ASSERT_NO_THROW(parse(f.data));
  for (const auto &[kind, target, pc, opcode] : std::vector<
           std::tuple<DexReferenceKind, std::string, uint32_t, std::string>>{
           {DexReferenceKind::String, "needle", 11, "const-string"},
           {DexReferenceKind::Type, "Lfixture/Target;", 13, "const-class"},
           {DexReferenceKind::Field, "Lfixture/Target;->VALUE:I", 15, "sget"},
           {DexReferenceKind::Method, "Lfixture/Target;->call()V", 17,
            "invoke-static"}}) {
    const auto result = references(f, kind, target, true);
    EXPECT_TRUE(result.code_scan_complete);
    EXPECT_EQ(result.class_descriptors,
              std::vector<std::string>{"Lfixture/Sample;"});
    EXPECT_EQ(result.defined_method_count, 1u);
    EXPECT_EQ(result.scanned_method_count, 1u);
    EXPECT_EQ(result.scanned_code_item_count, 1u);
    EXPECT_EQ(result.matching_pool_entries, 1u);
    ASSERT_EQ(result.references.size(), 1u);
    const auto &site = result.references[0];
    EXPECT_EQ(site.method.identity(), "Lfixture/Sample;->value()V");
    EXPECT_EQ(site.pc_code_units, pc);
    EXPECT_EQ(site.opcode, opcode);
    EXPECT_EQ(site.target, target);
    uint32_t encoded_index = 0;
    for (unsigned i = 0; i < 2; ++i)
      encoded_index |=
          uint32_t(uint8_t(f.data[f.at.at("code") + 16 + (pc + 1) * 2 + i]))
          << (i * 8);
    EXPECT_EQ(site.target_index, encoded_index);
    if (kind == DexReferenceKind::String) {
      ASSERT_TRUE(site.target_utf16);
      EXPECT_EQ(*site.target_utf16, u"needle");
    } else
      EXPECT_FALSE(site.target_utf16);
  }
}
TEST(MobileDalvikReader,
     DexReferenceMatchingKeepsOwnersOverloadsAndUnusedPools) {
  const auto f = referenceFixture();
  auto strings = references(f, DexReferenceKind::String, "needle");
  EXPECT_EQ(strings.matching_pool_entries, 2u);
  EXPECT_EQ(strings.references.size(), 1u);
  auto unused =
      references(f, DexReferenceKind::String, "a needle suffix", true);
  EXPECT_EQ(unused.matching_pool_entries, 1u);
  EXPECT_TRUE(unused.references.empty());
  EXPECT_TRUE(unused.code_scan_complete);
  EXPECT_EQ(unused.scanned_method_count, 1u);
  auto methods = references(f, DexReferenceKind::Method, "call");
  EXPECT_EQ(methods.matching_pool_entries, 3u);
  EXPECT_EQ(methods.references.size(), 1u);
  auto overload = references(f, DexReferenceKind::Method,
                             "Lfixture/Target;->call(I)V", true);
  EXPECT_EQ(overload.matching_pool_entries, 1u);
  EXPECT_TRUE(overload.references.empty());
  auto other =
      references(f, DexReferenceKind::Method, "call", false, "Lother/Target;");
  EXPECT_EQ(other.matching_pool_entries, 1u);
  EXPECT_TRUE(other.references.empty());
  auto fields = references(f, DexReferenceKind::Field, "VALUE", false,
                           "Lfixture/Target;");
  EXPECT_EQ(fields.matching_pool_entries, 2u);
  ASSERT_EQ(fields.references.size(), 1u);
  EXPECT_EQ(fields.references[0].target, "Lfixture/Target;->VALUE:I");
  auto absent = references(f, DexReferenceKind::String, "absent from pool");
  EXPECT_EQ(absent.matching_pool_entries, 0u);
  EXPECT_TRUE(absent.references.empty());
  EXPECT_TRUE(absent.code_scan_complete);
  EXPECT_EQ(absent.scanned_method_count, 1u);
}
TEST(MobileDalvikReader, DexExactReferencesChargeOnlyComparedTargetBytes) {
  FixtureOptions options;
  for (char letter : {'a', 'b'})
    options.extra_methods.push_back(
        {"Lexternal/Target;", std::string(8192, letter), {}, "V"});
  const auto f = fixture(options);
  const DexReferenceQuery short_query{DexReferenceKind::Method, "absent!",
                                      true};
  const auto target_size = options.extra_methods[0].identity().size();
  const DexReferenceQuery same_length_query{
      DexReferenceKind::Method, std::string(target_size, '?'), true};
  Budget short_budget, same_length_budget;
  const auto initial_work = short_budget.remaining;
  const auto short_result =
      findDexReferences(f.data, short_query, short_budget);
  const auto same_length_result =
      findDexReferences(f.data, same_length_query, same_length_budget);
  EXPECT_TRUE(short_result.references.empty());
  EXPECT_TRUE(same_length_result.references.empty());
  EXPECT_TRUE(short_result.code_scan_complete);
  EXPECT_TRUE(same_length_result.code_scan_complete);
  // Both scan the same complete DEX. Only the two equal-length target
  // comparisons require byte work; unequal lengths need a constant check.
  EXPECT_EQ(short_budget.remaining - same_length_budget.remaining,
            2 * target_size);
  const auto short_work = initial_work - short_budget.remaining;
  Budget bounded_short;
  bounded_short.remaining = short_work;
  EXPECT_NO_THROW(findDexReferences(f.data, short_query, bounded_short));
  Budget bounded_same_length;
  bounded_same_length.remaining = short_work + 2 * target_size - 1;
  EXPECT_THROW(
      findDexReferences(f.data, same_length_query, bounded_same_length), Error);
}
TEST(MobileDalvikReader, DexReferenceQueryRejectsInvalidFilters) {
  const auto f = referenceFixture();
  EXPECT_THROW(references(f, DexReferenceKind::String, ""), Error);
  EXPECT_THROW(references(f, DexReferenceKind::String, "needle", false,
                          "Lfixture/Target;"),
               Error);
  EXPECT_THROW(references(f, DexReferenceKind::Type, "Target", false,
                          "Lfixture/Target;"),
               Error);
  EXPECT_THROW(
      references(f, DexReferenceKind::Method, "call", false, "fixture.Target"),
      Error);
  EXPECT_THROW(references(f, DexReferenceKind::Method, "call", false, "I"),
               Error);
  EXPECT_THROW(references(f, DexReferenceKind::Field, "VALUE", false, "[I"),
               Error);
  EXPECT_THROW(references(f, static_cast<DexReferenceKind>(255), "needle"),
               Error);
}
TEST(MobileDalvikReader, DexReferenceMethodOwnersCanBeArrayDescriptors) {
  FixtureOptions options;
  options.params = {"[I"};
  options.returns = "V";
  options.referenced_method =
      MethodRef{"[I", "clone", {}, "Ljava/lang/Object;"};
  options.words = {0x106e, 0, 0, 0x000e};
  auto f = fixture(options);
  const auto index = std::find(f.methods.begin(), f.methods.end(),
                               *options.referenced_method) -
                     f.methods.begin();
  patch(f.data, f.at["code"] + 18, index, 2);
  f.data = seal(std::move(f.data));
  const auto result =
      references(f, DexReferenceKind::Method, "clone", false, "[I");
  ASSERT_EQ(result.references.size(), 1u);
  EXPECT_EQ(result.references[0].target, "[I->clone()Ljava/lang/Object;");
  EXPECT_EQ(result.references[0].pc_code_units, 0u);
}
TEST(MobileDalvikReader,
     DexReferencesNeverInterpretPayloadWordsAsInstructions) {
  for (unsigned payload_kind = 0; payload_kind < 3; ++payload_kind) {
    FixtureOptions options;
    options.params = {};
    options.returns = "V";
    options.extras = {u"needle"};
    if (payload_kind < 2)
      options.words = {0x001a,
                       0,
                       0x0012,
                       uint16_t(payload_kind ? 0x002c : 0x002b),
                       5,
                       0,
                       0x000e,
                       0,
                       uint16_t(payload_kind ? 0x0200 : 0x0100),
                       1,
                       0x001a,
                       0,
                       3,
                       0};
    else {
      options.params = {"[I"};
      options.registers = 2;
      options.words = {0x001a, 0, 0x0126, 4,      0, 0x000e, 0x0300,
                       4,      2, 0,      0x001a, 0, 0x001c, 0};
    }
    auto f = fixture(options);
    const auto string_index = fixtureStringIndex(f, "needle");
    patch(f.data, f.at["code"] + 18, string_index, 2);
    patch(f.data, f.at["code"] + 16 + 11 * 2, string_index, 2);
    if (payload_kind == 2)
      patch(f.data, f.at["code"] + 16 + 13 * 2, fixtureTypeIndex(f, "[I"), 2);
    f.data = seal(std::move(f.data));
    ASSERT_NO_THROW(parse(f.data));
    const auto result = references(f, DexReferenceKind::String, "needle", true);
    ASSERT_EQ(result.references.size(), 1u) << payload_kind;
    EXPECT_EQ(result.references[0].pc_code_units, 0u) << payload_kind;
    if (payload_kind == 2)
      EXPECT_TRUE(
          references(f, DexReferenceKind::Type, "[I", true).references.empty());
  }
}
TEST(MobileDalvikReader,
     DexReferenceSharedCodeRetainsEveryDeclaredMethodOwner) {
  FixtureOptions options;
  options.params = {};
  options.returns = "V";
  options.extras = {u"needle"};
  options.referenced_method = MethodRef{options.owner, "another", {}, "V"};
  options.define_referenced_method = true;
  options.words = {0x001a, 0, 0x000e};
  auto f = fixture(options);
  patch(f.data, f.at["code"] + 18, fixtureStringIndex(f, "needle"), 2);
  f.data = seal(std::move(f.data));
  ASSERT_EQ(parse(f.data)[0].methods.size(), 2u);
  const auto result = references(f, DexReferenceKind::String, "needle", true);
  EXPECT_EQ(result.defined_method_count, 2u);
  EXPECT_EQ(result.scanned_method_count, 2u);
  EXPECT_EQ(result.scanned_code_item_count, 1u);
  ASSERT_EQ(result.references.size(), 2u);
  EXPECT_EQ(result.references[0].method.identity(),
            "Lfixture/Sample;->another()V");
  EXPECT_EQ(result.references[1].method.identity(),
            "Lfixture/Sample;->value()V");
  EXPECT_EQ(result.references[0].pc_code_units, 0u);
  EXPECT_EQ(result.references[1].pc_code_units, 0u);
  Limits limits;
  limits.max_files = 1;
  Budget budget(limits);
  EXPECT_THROW(
      findDexReferences(f.data, {DexReferenceKind::String, "needle"}, budget),
      Error);
  options.referenced_method->returns = "I";
  EXPECT_THROW(references(fixture(options), DexReferenceKind::String, "needle"),
               Error);
  options.referenced_method->returns = "V";
  options.referenced_method->owner = "Lother/Target;";
  EXPECT_THROW(references(fixture(options), DexReferenceKind::String, "needle"),
               Error);
}
TEST(MobileDalvikReader, SharedCodeRequiresExactPrototypeEvenWithoutMatches) {
  for (const std::string parameter : {"F", "Ljava/lang/Object;"}) {
    FixtureOptions options;
    options.params = {"I"};
    options.returns = "V";
    options.words = {0x000e};
    options.referenced_method =
        MethodRef{options.owner, "another", {parameter}, "V"};
    options.define_referenced_method = true;
    auto f = fixture(options);
    const char *error =
        "Invalid DEX: shared data item has inconsistent declaration context";
    expectDexError(f.data, error);
    try {
      (void)references(f, DexReferenceKind::String, "absent");
      FAIL() << "same-width parameter types shared a code context";
    } catch (const Error &e) {
      EXPECT_STREQ(e.what(), error);
    }
  }
}

TEST(MobileDalvikReader, QueryRangesCheckReorderedAndOverlappingStringItems) {
  auto stringOffset = [](const Fixture &f, const std::string &text) {
    size_t row = f.at.at("strings") + fixtureStringIndex(f, text) * 4;
    uint32_t offset = 0;
    for (unsigned i = 0; i < 4; ++i)
      offset |= uint32_t(uint8_t(f.data[row + i])) << (8 * i);
    return std::pair{row, offset};
  };
  FixtureOptions options;
  options.extras = {u"alpha", u"bravo"};
  auto f = fixture(options);
  const auto [row_a, a] = stringOffset(f, "alpha");
  const auto [row_b, b] = stringOffset(f, "bravo");
  const auto first = f.data.substr(a, 7), second = f.data.substr(b, 7);
  f.data.replace(a, 7, second);
  f.data.replace(b, 7, first);
  patch(f.data, row_a, b);
  patch(f.data, row_b, a);
  f.data = seal(std::move(f.data));
  EXPECT_NO_THROW(parse(f.data));
  EXPECT_TRUE(
      references(f, DexReferenceKind::String, "absent").code_scan_complete);

  options.extras = {std::u16string{1, u'a'}, u"a"};
  f = fixture(options);
  const auto [outer_row, outer] = stringOffset(f, std::string("\1a", 2));
  const auto [inner_row, inner] = stringOffset(f, "a");
  patch(f.data, inner_row, outer + 1);
  f.data = seal(std::move(f.data));
  EXPECT_THROW(parse(f.data), Error);
  try {
    (void)references(f, DexReferenceKind::String, "absent");
    FAIL() << "overlapping valid string encodings were accepted";
  } catch (const Error &error) {
    EXPECT_STREQ(error.what(),
                 "Invalid DEX: overlapping variable-length data items");
  }
}

TEST(MobileDalvikReader, DexReferenceStringsKeepNulPairsAndIsolatedSurrogates) {
  const std::u16string literal{u'n', u'e', u'e',   u'd',   u'l',
                               u'e', 0,    0xd800, 0xd83d, 0xde00};
  const auto f = stringReturningFixture(literal);
  const auto result = references(f, DexReferenceKind::String, "needle");
  ASSERT_EQ(result.references.size(), 1u);
  const auto &site = result.references[0];
  ASSERT_TRUE(site.target_utf16);
  EXPECT_EQ(*site.target_utf16, literal);
  EXPECT_EQ(site.target,
            std::string("needle\0", 7) + "\xed\xa0\x80\xf0\x9f\x98\x80");
  EXPECT_EQ(references(f, DexReferenceKind::String, site.target, true)
                .references.size(),
            1u);
}
TEST(MobileDalvikReader,
     QuerySubstringSearchCrossesBlocksAndKeepsLongFallback) {
  for (const std::string needle :
       {"z", "abcdefghijklmnop", "abcdefghijklmnopq"}) {
    for (size_t prefix : {4095u, 4096u, 4097u, 8191u}) {
      const std::string text = std::string(prefix, 'x') + needle + "tail";
      const auto f = stringReturningFixture(utf16(text));
      const auto result = references(f, DexReferenceKind::String, needle);
      ASSERT_EQ(result.references.size(), 1u);
      EXPECT_EQ(result.references[0].target, text);
      EXPECT_EQ(result.references[0].pc_code_units, 0u);
      EXPECT_TRUE(result.code_scan_complete);
    }
  }
  const auto repeated = stringReturningFixture(std::u16string(65536, u'a'));
  for (size_t length : {16u, 17u}) {
    const auto result = references(repeated, DexReferenceKind::String,
                                   std::string(length - 1, 'a') + 'b');
    EXPECT_TRUE(result.references.empty());
    EXPECT_TRUE(result.code_scan_complete);
  }
}

TEST(MobileDalvikReader, DexReferenceQueriesRejectMalformedAndUnmatchedBodies) {
  const auto f = referenceFixture();
  for (auto [pc, value] :
       std::vector<std::pair<unsigned, unsigned>>{{12, 65535},
                                                  {14, 65535},
                                                  {16, 65535},
                                                  {18, 65535},
                                                  {11, 0xffff},
                                                  {20, 0x001a}}) {
    auto changed = f;
    patch(changed.data, changed.at["code"] + 16 + pc * 2, value, 2);
    changed.data = seal(std::move(changed.data));
    for (const auto &[kind, matching] :
         std::vector<std::pair<DexReferenceKind, std::string>>{
             {DexReferenceKind::String, "needle"},
             {DexReferenceKind::Type, "Lfixture/Target;"},
             {DexReferenceKind::Method, "call"},
             {DexReferenceKind::Field, "VALUE"}}) {
      SCOPED_TRACE(pc);
      SCOPED_TRACE(matching);
      // Empty selection and unrelated matching targets must both retain
      // every operand's validation, including the other three pool kinds.
      EXPECT_THROW(references(changed, kind, "absent"), Error);
      EXPECT_THROW(references(changed, kind, matching), Error);
    }
  }
  FixtureOptions branch;
  branch.params = {};
  branch.returns = "V";
  branch.words = {0x0029, 1, 0x000e};
  EXPECT_THROW(references(fixture(branch), DexReferenceKind::String, "absent"),
               Error);
  auto wrong_owner = f;
  patch(wrong_owner.data, wrong_owner.at["defined_method"],
        fixtureTypeIndex(wrong_owner, "Lfixture/Target;"), 2);
  wrong_owner.data = seal(std::move(wrong_owner.data));
  EXPECT_THROW(references(wrong_owner, DexReferenceKind::String, "needle"),
               Error);
  FixtureOptions native;
  native.flags |= 0x100;
  const auto declarations =
      references(fixture(native), DexReferenceKind::String, "absent");
  EXPECT_EQ(declarations.defined_method_count, 1u);
  EXPECT_EQ(declarations.scanned_method_count, 0u);
  EXPECT_EQ(declarations.scanned_code_item_count, 0u);
  EXPECT_TRUE(declarations.code_scan_complete);
}
TEST(MobileDalvikReader, DexReferenceQueriesBoundWorkTimeSitesAndStorage) {
  const auto f = referenceFixture();
  const DexReferenceQuery query{DexReferenceKind::String, "needle"};
  Budget work;
  work.remaining = 1;
  EXPECT_THROW(findDexReferences(f.data, query, work), Error);
  Budget time;
  time.deadline = std::chrono::steady_clock::time_point::min();
  EXPECT_THROW(findDexReferences(f.data, query, time), Error);
  FixtureOptions many;
  many.params = {};
  many.returns = "V";
  many.extras = {u"needle"};
  many.words.clear();
  for (unsigned i = 0; i < 32; ++i)
    many.words.insert(many.words.end(), {0x001a, 0});
  many.words.push_back(0x000e);
  auto expanded = fixture(many);
  for (unsigned i = 0; i < 32; ++i)
    patch(expanded.data, expanded.at["code"] + 18 + i * 4,
          fixtureStringIndex(expanded, "needle"), 2);
  expanded.data = seal(std::move(expanded.data));
  Limits limits;
  limits.max_files = 1;
  Budget sites(limits);
  EXPECT_THROW(findDexReferences(expanded.data, query, sites), Error);
  limits = Limits{};
  limits.max_bytes = expanded.data.size();
  Budget storage(limits);
  try {
    (void)findDexReferences(expanded.data, query, storage);
    FAIL() << "retained reference rows escaped the byte bound";
  } catch (const Error &error) {
    EXPECT_STREQ(error.what(), "reference query storage exceeds byte limit");
  }
}
TEST(MobileDalvikReader, DexReferenceScanDoesNotCopyUnusedLiteralsPerSite) {
  FixtureOptions options;
  options.params = {};
  options.returns = "V";
  const std::string literal(50000, 'q');
  options.extras = {utf16(literal)};
  options.words.clear();
  for (unsigned i = 0; i < 1000; ++i)
    options.words.insert(options.words.end(), {0x001a, 0});
  options.words.push_back(0x000e);
  auto f = fixture(options);
  for (unsigned i = 0; i < 1000; ++i)
    patch(f.data, f.at["code"] + 18 + i * 4, fixtureStringIndex(f, literal), 2);
  f.data = seal(std::move(f.data));
  Limits limits;
  limits.max_bytes = 2 * 1024 * 1024;
  Budget budget(limits);
  const auto result =
      findDexReferences(f.data, {DexReferenceKind::String, "absent"}, budget);
  EXPECT_TRUE(result.references.empty());
  EXPECT_EQ(result.scanned_code_item_count, 1u);
  EXPECT_TRUE(result.code_scan_complete);
}
TEST(MobileDalvikReader, DexReferencePoolExpansionIsBoundedBeforeCopies) {
  const std::string long_type = "L" + std::string(4096, 'x') + ";";
  for (bool repeated_parameters : {false, true}) {
    FixtureOptions options;
    if (repeated_parameters) {
      options.referenced_method =
          MethodRef{"Lfixture/Target;", "many",
                    std::vector<std::string>(64, long_type), "V"};
    } else {
      for (unsigned i = 0; i < 64; ++i)
        options.extra_methods.push_back(
            {long_type, "method" + std::to_string(i), {}, "V"});
    }
    const auto f = fixture(options);
    Limits limits;
    limits.max_bytes = 128 * 1024;
    ASSERT_LT(f.data.size(), limits.max_bytes);
    if (!repeated_parameters) {
      // Unused IDs share their owner descriptor; no expanded pool copy remains.
      Budget budget(limits);
      const auto result = findDexReferences(
          f.data, {DexReferenceKind::String, "absent"}, budget);
      EXPECT_TRUE(result.code_scan_complete);
      EXPECT_TRUE(result.references.empty());
      EXPECT_EQ(result.class_descriptors,
                std::vector<std::string>{options.owner});
      EXPECT_EQ(result.defined_method_count, 1u);
      EXPECT_EQ(result.scanned_method_count, 1u);
      EXPECT_EQ(result.scanned_code_item_count, 1u);
      EXPECT_EQ(result.matching_pool_entries, 0u);
      continue;
    }
    Budget budget(limits);
    try {
      (void)findDexReferences(f.data, {DexReferenceKind::String, "absent"},
                              budget);
      FAIL() << "expanded pool copies escaped the byte bound";
    } catch (const Error &error) {
      EXPECT_STREQ(error.what(), "reference query storage exceeds byte limit");
    }
    if (repeated_parameters) {
      // A large byte allowance must not let repeated descriptor copies escape
      // the separate work bound merely because their encoded indices are tiny.
      Budget work;
      work.remaining = 20000;
      EXPECT_THROW(
          findDexReferences(f.data, {DexReferenceKind::String, "absent"}, work),
          Error);
    }
  }
}
TEST(MobileDalvikReader, DexDebugInfoAcceptsZeroLinesAndRejectsNegativeLines) {
  FixtureOptions options;
  // Zero start line, one unnamed parameter, a position at PC zero/line zero.
  options.debug_info =
      std::string({char(0), char(1), char(0), char(14), char(0)});
  const auto zero = fixture(options);
  ASSERT_NO_THROW(parse(zero.data));
  EXPECT_TRUE(
      references(zero, DexReferenceKind::String, "absent").code_scan_complete);
  // DBG_ADVANCE_LINE -1 must still fail, even without an emitted position.
  options.debug_info =
      std::string({char(0), char(1), char(0), char(2), char(0x7f), char(0)});
  const auto negative = fixture(options);
  EXPECT_THROW(parse(negative.data), Error);
  EXPECT_THROW(references(negative, DexReferenceKind::String, "absent"), Error);
  options.debug_info =
      std::string({char(0), char(1), char(0), char(10), char(0)});
  EXPECT_THROW(parse(fixture(options).data), Error);
}
TEST(MobileDalvikReader, DexHeaderIntegrityAndUnsupportedContainers) {
  auto f = fixture();
  EXPECT_THROW(parse(f.data.substr(0, 111)), Error);
  for (auto version : {"036", "041", "999"}) {
    auto broken = f.data;
    broken.replace(4, 3, version);
    EXPECT_THROW(parse(seal(broken)), Error);
  }
  for (auto [offset, value] :
       std::vector<std::pair<size_t, uint64_t>>{{32, f.data.size() - 1},
                                                {36, 120},
                                                {40, 0x78563412},
                                                {44, 8},
                                                {52, UINT32_MAX}}) {
    auto broken = f.data;
    patch(broken, offset, value);
    EXPECT_THROW(parse(seal(broken)), Error);
  }
  auto broken = f.data;
  broken.back() ^= 1;
  EXPECT_THROW(parse(broken), Error);
}
TEST(MobileDalvikReader, DexTableAndMapReferencesCannotEscapeSections) {
  auto f = fixture();
  for (auto [offset, value] : std::vector<std::pair<size_t, uint64_t>>{
           {f.at["strings"], 0},
           {f.at["types"], UINT32_MAX},
           {f.at["protos"], UINT32_MAX},
           {f.at["protos"] + 4, UINT32_MAX},
           {f.at["methods"] + 4, UINT32_MAX},
           {f.at["class"], UINT32_MAX},
           {f.at["map"] + 8, 0},
           {f.at["class"] + 24, f.at["string_data"]}}) {
    auto broken = f.data;
    patch(broken, offset, value);
    EXPECT_THROW(parse(seal(broken)), Error);
  }
}
TEST(MobileDalvikReader, DexMUTF8PreservesExactUTF16CodeUnits) {
  FixtureOptions o;
  o.params = {};
  o.returns = "Ljava/lang/String;";
  o.words = {0x001a, 0, 0x0011};
  o.extras = {std::u16string{u'A', 0, 0x03bb, 0xd83d, 0xde00, 0xd800}};
  auto f = fixture(o);
  auto index = std::find(f.strings.begin(), f.strings.end(), o.extras[0]) -
               f.strings.begin();
  patch(f.data, f.at["code"] + 18, index, 2);
  auto classes = parse(seal(f.data));
  EXPECT_EQ(
      std::get<std::string>(classes[0].methods[0].instructions[0].literal),
      std::string("A\0", 2) + "\xce\xbb\xf0\x9f\x98\x80\xed\xa0\x80");
  // A resealed malformed sequence must fail before any source declaration.
  auto broken = f.data;
  auto offset =
      uint8_t(broken[f.at["strings"] + index * 4]) |
      (unsigned(uint8_t(broken[f.at["strings"] + index * 4 + 1])) << 8);
  broken[offset + 1] = char(0xf0);
  EXPECT_THROW(parse(seal(broken)), Error);
}
TEST(MobileDalvikReader, DexInstructionFormatsKeepLiteralsAndRegisterWords) {
  FixtureOptions o;
  o.registers = 3;
  o.words = {0x2112, 0x0113, 0xffff, 0x0115, 0x8000,
             0x0290, 0x0100, 0x00d8, 0x8001, 0x000f};
  auto classes = parse(fixture(o).data);
  auto &ins = classes[0].methods[0].instructions;
  ASSERT_EQ(ins.size(), 6u);
  EXPECT_EQ(std::get<int64_t>(ins[0].literal), 2);
  EXPECT_EQ(std::get<int64_t>(ins[1].literal), -1);
  EXPECT_EQ(std::get<int64_t>(ins[2].literal), INT32_MIN);
  EXPECT_EQ(ins[3].pc, 5u);
  EXPECT_EQ(ins[3].registers, (std::vector<unsigned>{2, 0, 1}));
  EXPECT_EQ(std::get<int64_t>(ins[4].literal), -128);
  o.params = {"J"};
  o.returns = "J";
  o.registers = 2;
  o.words = {0x0010};
  EXPECT_EQ(parse(fixture(o).data)[0].methods[0].incomingWords(), 2u);
  o.words = {0x0110};
  EXPECT_THROW(parse(fixture(o).data), Error);
}
TEST(MobileDalvikReader, DexLargeBodiesPreserveLittleEndianWideLiterals) {
  for (unsigned prefix : {2046, 2047, 2048, 4094, 4095, 4096}) {
    SCOPED_TRACE(prefix);
    FixtureOptions options;
    options.params = {};
    options.returns = "J";
    options.registers = 2;
    options.words.assign(prefix, 0);
    options.words.insert(options.words.end(),
                         {0x0018, 0xcdef, 0x89ab, 0x4567, 0x0123, 0x0010});
    const auto f = fixture(options);
    const auto classes = parse(f.data);
    const auto &instructions = classes[0].methods[0].instructions;
    ASSERT_EQ(instructions.size(), prefix + 2u);
    EXPECT_EQ(instructions[prefix].pc, prefix);
    EXPECT_EQ(instructions[prefix].opcode, "const-wide");
    EXPECT_EQ(std::get<int64_t>(instructions[prefix].literal),
              INT64_C(0x0123456789abcdef));
    EXPECT_EQ(instructions.back().pc, prefix + 5u);
    const auto result = references(f, DexReferenceKind::String, "absent");
    EXPECT_TRUE(result.references.empty());
    EXPECT_EQ(result.scanned_code_item_count, 1u);
    EXPECT_TRUE(result.code_scan_complete);
  }
}
TEST(MobileDalvikReader, DexInvokesValidateReferencesAndArgumentWords) {
  FixtureOptions o;
  o.words = {0x1071, 0, 0, 0x000a, 0x000f};
  auto result = parse(fixture(o).data);
  EXPECT_EQ(std::get<MethodRef>(result[0].methods[0].instructions[0].reference)
                .identity(),
            "Lfixture/Sample;->value(I)I");
  for (auto code :
       std::vector<std::vector<uint16_t>>{{0x2071, 0, 0, 0x000f},
                                          {0x1071, 65535, 0, 0x000f},
                                          {0x00fa, 0, 0, 0, 0x000f}}) {
    o.words = code;
    EXPECT_THROW(parse(fixture(o).data), Error);
  }
}
TEST(MobileDalvikReader, DexRangeInvokesKeepEveryArgumentRegister) {
  for (unsigned count : {5, 6, 255}) {
    SCOPED_TRACE(count);
    FixtureOptions options;
    options.params.assign(count, "I");
    options.returns = "V";
    options.registers = count;
    options.referenced_method =
        MethodRef{"Lexternal/Target;", "accept", options.params, "V"};
    options.words = {uint16_t((count << 8) | 0x77), 0, 0, 0x000e};
    auto f = fixture(options);
    const auto index = std::find(f.methods.begin(), f.methods.end(),
                                 *options.referenced_method) -
                       f.methods.begin();
    patch(f.data, f.at["code"] + 18, index, 2);
    f.data = seal(std::move(f.data));
    const auto classes = parse(f.data);
    const auto &invoke = classes[0].methods[0].instructions[0];
    ASSERT_EQ(invoke.registers.size(), count);
    for (unsigned i = 0; i < count; ++i)
      EXPECT_EQ(invoke.registers[i], i);
    const auto found = references(f, DexReferenceKind::Method, "accept");
    ASSERT_EQ(found.references.size(), 1u);
    EXPECT_EQ(found.references[0].target,
              options.referenced_method->identity());
    // Moving the same range by one register must reject its final operand.
    patch(f.data, f.at["code"] + 20, 1, 2);
    f.data = seal(std::move(f.data));
    EXPECT_THROW(parse(f.data), Error);
    EXPECT_THROW(references(f, DexReferenceKind::String, "absent"), Error);
  }
}
TEST(MobileDalvikReader, DexWideRegistersPreserveScalarAndPairBoundaries) {
  // Three registers make v2 valid as a scalar, but invalid as a wide pair.
  // Exercise both recovery and query decoding through their shared authority.
  for (const auto &[name, words, valid] :
       std::vector<std::tuple<std::string, std::vector<uint16_t>, bool>>{
           {"move-wide", {0x1004}, true},
           {"move-wide destination", {0x0204}, false},
           {"move-wide source", {0x2004}, false},
           {"move-wide/from16", {0x0005, 1}, true},
           {"move-wide/from16 source", {0x0005, 2}, false},
           {"move-wide/16", {0x0006, 0, 1}, true},
           {"move-wide/16 destination", {0x0006, 2, 0}, false},
           {"const-wide", {0x0016, 0}, true},
           {"const-wide destination", {0x0216, 0}, false},
           {"int-to-long scalar source", {0x2081}, true},
           {"int-to-long destination", {0x0281}, false},
           {"long-to-int scalar destination", {0x0284}, true},
           {"long-to-int source", {0x2084}, false},
           {"double-to-long", {0x018b}, true},
           {"double-to-long destination", {0x028b}, false},
           {"double-to-long source", {0x208b}, false},
           {"cmp-long scalar destination", {0x0231, 0x0100}, true},
           {"cmp-long first source", {0x0231, 0x0002}, false},
           {"cmp-long second source", {0x0231, 0x0200}, false},
           {"cmpl-double scalar destination", {0x022f, 0x0100}, true},
           {"cmpl-double second source", {0x022f, 0x0200}, false},
           {"aget-wide scalar array and index", {0x0045, 0x0202}, true},
           {"aget-wide destination", {0x0245, 0x0202}, false},
           {"aput-wide scalar array and index", {0x004c, 0x0202}, true},
           {"aput-wide value", {0x024c, 0x0202}, false},
           {"neg-long", {0x107d}, true},
           {"neg-long source", {0x207d}, false},
           {"add-long", {0x019b, 0x0100}, true},
           {"add-long destination", {0x029b, 0x0100}, false},
           {"add-long source", {0x019b, 0x0200}, false},
           {"shl-long scalar shift", {0x00a3, 0x0201}, true},
           {"shl-long destination", {0x02a3, 0x0201}, false},
           {"shl-long source", {0x00a3, 0x0202}, false},
           {"shr-long scalar shift", {0x00a4, 0x0201}, true},
           {"ushr-long scalar shift", {0x00a5, 0x0201}, true},
           {"shl-long/2addr scalar shift", {0x21c3}, true},
           {"shl-long/2addr destination", {0x02c3}, false},
           {"shr-long/2addr scalar shift", {0x21c4}, true},
           {"ushr-long/2addr scalar shift", {0x21c5}, true}}) {
    SCOPED_TRACE(name);
    FixtureOptions options;
    options.params = {};
    options.returns = "V";
    options.registers = 3;
    options.words = words;
    options.words.push_back(0x000e);
    const auto f = fixture(options);
    if (valid) {
      EXPECT_NO_THROW(parse(f.data));
      EXPECT_TRUE(
          references(f, DexReferenceKind::String, "absent").code_scan_complete);
    } else {
      expectDexError(f.data, "Invalid DEX: wide register pair exceeds frame");
      EXPECT_THROW(references(f, DexReferenceKind::String, "absent"), Error);
    }
  }
}
TEST(MobileDalvikReader, DexArrayCloneCallsAgreeWithSmali) {
  for (const std::string owner : {"[I", "[Ljava/lang/reflect/Type;", "[[I"}) {
    SCOPED_TRACE(owner);
    MethodRef callee{owner, "clone", {}, "Ljava/lang/Object;"};
    auto f = invokingFixture(callee);
    auto classes = parse(f.data);
    ASSERT_EQ(classes.size(), 1u);
    ASSERT_EQ(classes[0].methods.size(), 1u);
    const auto &dex_method = classes[0].methods[0];
    // The external array method must not enter the definition inventory.
    EXPECT_EQ(dex_method.reference.identity(),
              "Lfixture/Sample;->value(" + owner + ")Ljava/lang/Object;");
    ASSERT_EQ(dex_method.instructions.size(), 3u);
    EXPECT_EQ(dex_method.instructions[0].opcode, "invoke-virtual");
    EXPECT_EQ(dex_method.instructions[0].registers, (std::vector<unsigned>{1}));
    const auto &invocation = dex_method.instructions[0];
    EXPECT_EQ(std::get<MethodRef>(invocation.reference), callee);
    EXPECT_EQ(dex_method.instructions[1].opcode, "move-result-object");
    EXPECT_EQ(dex_method.instructions[2].opcode, "return-object");
    auto smali_body = "invoke-virtual {p0}, " + callee.identity() +
                      "\nmove-result-object v0\nreturn-object v0";
    auto signature = "value(" + owner + ")Ljava/lang/Object;";
    auto smali_class = smali(methodText(smali_body, signature, 2));
    const auto &smali_method = smali_class.methods[0];
    ASSERT_EQ(smali_method.instructions.size(), dex_method.instructions.size());
    // DEX positions count 16-bit code units; smali positions are logical
    // instruction ordinals. The three-unit invoke has the same operation.
    const std::array<uint32_t, 3> dex_positions{0, 3, 4};
    EXPECT_EQ(dex_method.code_end, 5u);
    EXPECT_EQ(smali_method.code_end, 3u);
    EXPECT_EQ(smali_method.registers, dex_method.registers);
    for (size_t i = 0; i < dex_method.instructions.size(); ++i) {
      const auto &a = dex_method.instructions[i];
      const auto &b = smali_method.instructions[i];
      EXPECT_EQ(a.pc, dex_positions[i]);
      EXPECT_EQ(b.pc, i);
      EXPECT_EQ(a.opcode, b.opcode);
      EXPECT_EQ(a.registers, b.registers);
      EXPECT_EQ(a.reference, b.reference);
    }
  }
}
TEST(MobileDalvikReader, DexArrayOwnersAreNotRestrictedToClone) {
  MethodRef callee{"[I", "hashCode", {}, "I"};
  auto classes = parse(invokingFixture(callee).data);
  ASSERT_EQ(classes.size(), 1u);
  ASSERT_EQ(classes[0].methods.size(), 1u);
  const auto &ins = classes[0].methods[0].instructions;
  ASSERT_EQ(ins.size(), 3u);
  EXPECT_EQ(std::get<MethodRef>(ins[0].reference), callee);
  EXPECT_EQ(ins[1].opcode, "move-result");
  EXPECT_EQ(ins[2].opcode, "return");
}
TEST(MobileDalvikReader, DexMemberOwnerKindsRemainStrict) {
  FixtureOptions o;
  o.params = {"I", "[I"};
  o.registers = 2;
  o.static_value = std::string("\x04\0", 2);
  auto f = fixture(o);
  ASSERT_NO_THROW(parse(f.data));
  for (const std::string owner : {"I", "[I"}) {
    SCOPED_TRACE(owner);
    auto broken = f.data;
    patch(broken, f.at["fields"], fixtureTypeIndex(f, owner), 2);
    expectDexError(seal(std::move(broken)),
                   "Invalid DEX: invalid member owner/name");
  }
  auto broken = f.data;
  patch(broken, f.at["defined_method"], fixtureTypeIndex(f, "I"), 2);
  expectDexError(seal(std::move(broken)),
                 "Invalid DEX: invalid member owner/name");

  o.returns = "V";
  o.words = {0x000e};
  f = fixture(o);
  ASSERT_NO_THROW(parse(f.data));
  for (const std::string table : {"fields", "defined_method"}) {
    SCOPED_TRACE(table);
    broken = f.data;
    patch(broken, f.at[table], fixtureTypeIndex(f, "V"), 2);
    expectDexError(seal(std::move(broken)),
                   "Invalid DEX: void outside return type");
  }
}
TEST(MobileDalvikReader, DexArrayMethodReferencesCannotBecomeDefinitions) {
  FixtureOptions o;
  o.params = {"[I"};
  o.returns = "[I";
  o.words = {0x0011};
  auto f = fixture(o);
  ASSERT_NO_THROW(parse(f.data));
  unsigned array_index = fixtureTypeIndex(f, "[I");
  auto broken = f.data;
  patch(broken, f.at["class"], array_index);
  expectDexError(seal(std::move(broken)),
                 "Invalid DEX: invalid/duplicate class definition");
  broken = f.data;
  patch(broken, f.at["defined_method"], array_index, 2);
  expectDexError(seal(std::move(broken)),
                 "Invalid DEX: class-data member owner mismatch");
}
TEST(MobileDalvikReader, DexArrayInitializerCallsRemainInvalid) {
  for (const std::string name : {"<init>", "<clinit>"}) {
    SCOPED_TRACE(name);
    MethodRef callee{"[I", name, {}, "V"};
    auto f = invokingFixture(callee, 0x70);
    constexpr std::string_view reason =
        "Invalid DEX: array type cannot own an initializer invocation";
    expectDexError(f.data, reason);
    EXPECT_THROW(smali(methodText("invoke-direct {p0}, " + callee.identity() +
                                      "\nreturn-void",
                                  "value([I)V", 2)),
                 Error);
  }
}
TEST(MobileDalvikReader, DexSwitchAndArrayPayloadsAreNotExecutable) {
  FixtureOptions o;
  o.words = {0x002b, 4, 0, 0x000f, 0x100, 2, 0xffff, 0xffff, 3, 0, 3, 0};
  auto result = parse(fixture(o).data);
  auto &ins = result[0].methods[0].instructions;
  ASSERT_EQ(ins.size(), 2u);
  EXPECT_EQ(ins[0].keys, (std::vector<int32_t>{-1, 0}));
  EXPECT_EQ(ins[0].targets, (std::vector<uint32_t>{3, 3}));
  o.words = {0x002c, 4,   0, 0x000f, 0x200, 2, 0xfffe,
             0xffff, 100, 0, 3,      0,     3, 0};
  result = parse(fixture(o).data);
  EXPECT_EQ(result[0].methods[0].instructions[0].keys,
            (std::vector<int32_t>{-2, 100}));
  o.params = {"[I"};
  o.returns = "[I";
  o.words = {0x0026, 4, 0, 0x0011, 0x300,  4,      3,
             0,      1, 0, 0xfffe, 0xffff, 0xffff, 0x7fff};
  result = parse(fixture(o).data);
  EXPECT_EQ(result[0].methods[0].instructions[0].data,
            (std::vector<uint64_t>{1, 0xfffffffe, 0x7fffffff}));
}
TEST(MobileDalvikReader, DexBadBranchesPayloadsAndUnknownOpcodesFail) {
  for (auto code : std::vector<std::vector<uint16_t>>{
           {0x0014, 1},
           {0x0238, 1, 0x000f},
           {0x0029, 1, 0x000f},
           {0x0000},
           {0x00e3, 0x000f},
           {0x002b, 4, 0, 0x000f, 0x200, 0},
           {0x002b, 4, 0, 0x000f, 0x100, 1, 0, 0, 1, 0},
           {0x0000, 0x300, 1, 1, 0, 0}}) {
    FixtureOptions o;
    o.words = code;
    EXPECT_THROW(parse(fixture(o).data), Error);
  }
}
TEST(MobileDalvikReader, DexExceptionRangesAndHandlerEntriesAreValidated) {
  FixtureOptions o;
  o.words = {0x0093, 0x0100, 0x000f, 0x000d, 0xf012, 0x000f};
  o.params = {"I", "I"};
  o.registers = 2;
  o.tries = {{{0, 2, 1}}};
  o.handlers = std::string("\1\0\3", 3);
  auto result = parse(fixture(o).data);
  auto &region = result[0].methods[0].tries[0];
  EXPECT_EQ(region.start, 0u);
  EXPECT_EQ(region.end, 2u);
  ASSERT_EQ(region.handlers.size(), 1u);
  EXPECT_FALSE(region.handlers[0].type);
  EXPECT_EQ(region.handlers[0].target, 3u);
  o.tries = {{{1, 1, 1}}};
  EXPECT_THROW(parse(fixture(o).data), Error);
  o.tries = {{{0, 2, 2}}};
  EXPECT_THROW(parse(fixture(o).data), Error);
  o.tries = {{{0, 2, 1}}};
  o.handlers = std::string("\1\0\1", 3);
  EXPECT_THROW(parse(fixture(o).data), Error);
}
TEST(MobileDalvikReader, DexMoveResultsCannotBorrowControlFlowProducers) {
  for (auto code : std::vector<std::vector<uint16_t>>{
           {0x000d, 0x000f},
           {0x000a, 0x000f},
           {0x1071, 0, 0, 0x000b, 0x000f},
           {0x0028, 0x000f},
           {0x0038, 5, 0x1071, 0, 0, 0x000a, 0x000f}}) {
    FixtureOptions o;
    o.words = code;
    EXPECT_THROW(parse(fixture(o).data), Error);
  }
}
TEST(MobileDalvikReader, DexEncodedFloatBitsAndBodylessInventoryRemainExact) {
  FixtureOptions o;
  o.static_value = std::string("\x70\x01\x00\x80\x7f", 5);
  o.field_type = "F";
  auto result = parse(fixture(o).data);
  auto value = std::get<FloatBits>(result[0].fields[0].value);
  EXPECT_EQ(value.bits, 0x7f800001u);
  EXPECT_FALSE(value.wide);
  o.static_value = std::string("\x11\x80", 2);
  o.field_type = "D";
  value = std::get<FloatBits>(parse(fixture(o).data)[0].fields[0].value);
  EXPECT_EQ(value.bits, 0x8000000000000000ULL);
  EXPECT_TRUE(value.wide);
  o.static_value.reset();
  o.flags = 0x401;
  result = parse(fixture(o).data);
  EXPECT_TRUE(result[0].methods[0].instructions.empty());
  EXPECT_TRUE(has(result[0].methods[0].access, "abstract"));
}
TEST(MobileDalvikReader, DexResourceLimitsApplyBeforeExpansion) {
  auto f = fixture();
  Budget work;
  work.remaining = 1;
  EXPECT_THROW(parseDex(f.data, "owned", work), Error);
  Budget time;
  time.deadline = std::chrono::steady_clock::time_point::min();
  EXPECT_THROW(parseDex(f.data, "owned", time), Error);
  Limits limits;
  limits.max_bytes = 112;
  Budget bytes(limits);
  EXPECT_THROW(parseDex(f.data, "owned", bytes), Error);
}
TEST(MobileDalvikReader, DexStringRecoveryChargesReaderAndOutputWorkOnce) {
  const std::string literal(40000, 'x');
  auto f = stringReturningFixture(utf16(literal));
  DexRecoveryDirectory temporary;
  auto source = temporary.path / "literal.dex",
       output = temporary.path / "output";
  writeFile(source, f.data);
  ASSERT_TRUE(fs::create_directory(output));
  Options options;
  options.input = source;
  options.output = output;
  Budget budget;
  // About 40k actual MUTF8 bytes are decoded and another 40k are emitted.
  // An additional full-input debit cannot fit this allowance. The large
  // string is referenced by const-string and returned, not unused padding.
  budget.remaining = 100000;
  auto report = recoverAndroid(options, output, budget);
  EXPECT_EQ(report.getString("status"), "success");
  EXPECT_EQ(report.getInteger("dex_count"), 1);
  EXPECT_EQ(report.getInteger("java_source_count"), 1);
  auto *coverage = report.getObject("android_method_recovery");
  ASSERT_NE(coverage, nullptr);
  EXPECT_EQ(coverage->getInteger("method_count"), 1);
  EXPECT_EQ(coverage->getInteger("recovered_method_count"), 1);
  EXPECT_EQ(coverage->getInteger("declaration_only_method_count"), 0);
  auto *methods = coverage->getArray("methods");
  ASSERT_NE(methods, nullptr);
  ASSERT_EQ(methods->size(), 1u);
  auto *method = (*methods)[0].getAsObject();
  ASSERT_NE(method, nullptr);
  EXPECT_EQ(method->getString("identity"),
            "Lfixture/Sample;->value()Ljava/lang/String;");
  EXPECT_EQ(method->getString("status"), "recovered");
  EXPECT_EQ(method->getInteger("instruction_count"), 2);
  auto *sources = report.getArray("java_sources");
  ASSERT_NE(sources, nullptr);
  ASSERT_EQ(sources->size(), 1u);
  EXPECT_EQ((*sources)[0].getAsString(), "sources/fixture/Sample.java");
  auto java = readFile(output / "sources/fixture/Sample.java", 100000);
  EXPECT_NE(java.find("class Sample"), std::string::npos);
  EXPECT_NE(java.find("value()"), std::string::npos);
  EXPECT_NE(java.find("\"" + literal + "\""), std::string::npos);
  EXPECT_NE(java.find("return "), std::string::npos);
  EXPECT_GT(budget.remaining, 0u);
  EXPECT_LT(budget.remaining, 20000u);
  EXPECT_EQ(readFile(source, 100000), f.data);
  EXPECT_FALSE(fs::exists(output / ".android-work"));
}
TEST(MobileDalvikReader, DexStringReaderRetainsWorkAndIntegrityLimits) {
  auto f = stringReturningFixture(std::u16string(40000, u'x'));
  Budget reader_budget;
  reader_budget.remaining = 30000;
  try {
    (void)parseDex(f.data, "literal.dex", reader_budget);
    FAIL() << "DEX string decoding escaped its own work budget";
  } catch (const Error &error) {
    EXPECT_STREQ(error.what(), "mobile analysis exceeded its work budget");
  }
  // The header scan alone fits. Exhaustion must occur in the reader's actual
  // table/string work, without any Android wrapper debit.
  EXPECT_LT(reader_budget.remaining, 100u);

  DexRecoveryDirectory temporary;
  auto source = temporary.path / "literal.dex",
       output = temporary.path / "output";
  writeFile(source, f.data);
  ASSERT_TRUE(fs::create_directory(output));
  Options options;
  options.input = source;
  options.output = output;
  Budget limited;
  limited.remaining = 30000;
  try {
    (void)recoverAndroid(options, output, limited);
    FAIL() << "Android recovery bypassed the reader's work budget";
  } catch (const Error &error) {
    EXPECT_STREQ(error.what(), "mobile analysis exceeded its work budget");
  }
  EXPECT_TRUE(fs::is_empty(output));
  EXPECT_EQ(readFile(source, 100000), f.data);

  auto corrupt = f.data;
  corrupt[8] ^= 1;
  expectDexError(corrupt, "Invalid DEX: checksum mismatch");
  corrupt = f.data;
  patch(corrupt, f.at["code"] + 18, 65535, 2);
  expectDexError(seal(std::move(corrupt)),
                 "Invalid DEX: string index out of bounds");
}
TEST(MobileDalvikReader, DexASCIIStringRunsRetainExactByteWork) {
  const auto ascii = stringReturningFixture(std::u16string(8192, u'x'));
  const auto unicode = stringReturningFixture(std::u16string(4096, u'\u03bb'));
  // Both strings have exactly 8192 MUTF-8 payload bytes and two-byte UTF-16
  // lengths. Everything else, including envelope scan charges, has equal size.
  ASSERT_EQ(ascii.data.size(), unicode.data.size());
  Budget ascii_budget, unicode_budget;
  auto ascii_classes = parseDex(ascii.data, "ascii.dex", ascii_budget);
  auto unicode_classes = parseDex(unicode.data, "unicode.dex", unicode_budget);
  ASSERT_EQ(ascii_classes.size(), 1u);
  ASSERT_EQ(unicode_classes.size(), 1u);
  EXPECT_EQ(ascii_budget.remaining, unicode_budget.remaining);
  EXPECT_EQ(std::get<std::string>(
                ascii_classes[0].methods[0].instructions[0].literal),
            std::string(8192, 'x'));
  const auto &decoded = std::get<std::string>(
      unicode_classes[0].methods[0].instructions[0].literal);
  EXPECT_EQ(decoded.size(), 8192u);
  EXPECT_EQ(decoded.substr(0, 2), "\xce\xbb");
  Budget initial;
  const auto spent = initial.remaining - ascii_budget.remaining;
  Budget exact;
  exact.remaining = spent;
  EXPECT_NO_THROW(parseDex(ascii.data, "ascii.dex", exact));
  EXPECT_EQ(exact.remaining, 0u);
  Budget insufficient;
  insufficient.remaining = spent - 1;
  EXPECT_THROW(parseDex(ascii.data, "ascii.dex", insufficient), Error);
  EXPECT_EQ(insufficient.remaining, 0u);
}
TEST(MobileDalvikReader, DexASCIIChunkBoundariesPreserveMUTF8AndLengthChecks) {
  for (const size_t prefix : {4095, 4096, 4097}) {
    std::u16string literal(prefix, u'x');
    literal += std::u16string{0, 0x03bb, 0xd83d, 0xde00, 0xd800, u'z'};
    auto f = stringReturningFixture(literal);
    auto classes = parse(f.data);
    const auto &decoded =
        std::get<std::string>(classes[0].methods[0].instructions[0].literal);
    EXPECT_EQ(decoded, std::string(prefix, 'x') + std::string(1, '\0') +
                           "\xce\xbb\xf0\x9f\x98\x80\xed\xa0\x80z");
    const auto index = std::find(f.strings.begin(), f.strings.end(), literal) -
                       f.strings.begin();
    uint32_t offset = 0;
    for (unsigned i = 0; i < 4; ++i)
      offset |= uint32_t(uint8_t(f.data[f.at["strings"] + index * 4 + i]))
                << (i * 8);
    std::string length;
    uleb(length, literal.size());
    const auto boundary = offset + length.size() + prefix;
    auto broken = f.data;
    broken[boundary] = char(0xf0);
    expectDexError(seal(std::move(broken)),
                   "Invalid DEX: invalid MUTF-8 leading byte");
    broken = f.data;
    broken[boundary + 1] = ' ';
    expectDexError(seal(std::move(broken)),
                   "Invalid DEX: invalid MUTF-8 continuation");
    for (const auto expected : {literal.size() - 1, literal.size() + 1}) {
      std::string changed_length;
      uleb(changed_length, expected);
      ASSERT_EQ(changed_length.size(), length.size());
      broken = f.data;
      broken.replace(offset, length.size(), changed_length);
      expectDexError(seal(std::move(broken)),
                     "Invalid DEX: UTF-16 string length mismatch");
    }
  }
}
TEST(MobileDalvikReader, SmaliAbsoluteWordAliasesAndBodylessDeclarations) {
  auto cls =
      smali(".class public abstract Lfixture/Test;\n.super "
            "Ljava/lang/Object;\n.method public static value(JD)J\n.locals "
            "2\nmove-wide v0,p0\nreturn-wide v0\n.end method\n.method public "
            "abstract missing(I)I\n.end method\n.method public native "
            "nativeValue(J)J\n.end method\n");
  ASSERT_EQ(cls.methods.size(), 3u);
  auto &m = cls.methods[0];
  EXPECT_EQ(m.registers, 6u);
  EXPECT_EQ(m.incomingWords(), 4u);
  EXPECT_EQ(m.instructions[0].registers, (std::vector<unsigned>{0, 2}));
  EXPECT_TRUE(cls.methods[1].instructions.empty());
  EXPECT_TRUE(cls.methods[2].instructions.empty());
}
TEST(MobileDalvikReader, SmaliInstructionFormsPreserveTypedOperands) {
  auto cls = smali(methodText(
      "move/from16 v0,p0\nconst/16 v1,-1\nconst v2,0x80000000\nadd-int "
      "v0,v0,v1\nadd-int/lit8 v0,v0,-128\nreturn v0"));
  auto &ins = cls.methods[0].instructions;
  ASSERT_EQ(ins.size(), 6u);
  EXPECT_EQ(ins[0].registers, (std::vector<unsigned>{0, 3}));
  EXPECT_EQ(std::get<int64_t>(ins[1].literal), -1);
  EXPECT_EQ(std::get<int64_t>(ins[2].literal), INT32_MIN);
  EXPECT_EQ(std::get<int64_t>(ins[4].literal), -128);
}
TEST(MobileDalvikReader, SmaliQuotedStringsCharactersAndCommentsRemainExact) {
  auto cls = smali(methodText(
      "const-string v0,\"a,#\\n\\u03bb\\ud800\" # outside\nreturn-object v0",
      "value()Ljava/lang/String;", 1));
  EXPECT_EQ(std::get<std::string>(cls.methods[0].instructions[0].literal),
            "a,#\n\xce\xbb\xed\xa0\x80");
  cls = smali(methodText("const/16 v0,'A'\nreturn v0"));
  EXPECT_EQ(std::get<int64_t>(cls.methods[0].instructions[0].literal), 65);
  EXPECT_THROW(smali(methodText("const/16 v0,'ab'\nreturn v0")), Error);
  EXPECT_THROW(
      smali(methodText("const-string v0,\"bad\\x41\"\nreturn-object v0",
                       "value()Ljava/lang/String;", 1)),
      Error);
}
TEST(MobileDalvikReader, SmaliFieldRawBitsKeepNegativeZeroAndNaN) {
  auto cls = smali(
      ".class public Lfixture/Bits;\n.super Ljava/lang/Object;\n.field public "
      "static a:F = -0.0f\n.field public static b:D = -0.0\n.field public "
      "static c:F = NaNf\n.field public static narrow:B = 0xfft\n.field public "
      "static letter:C = 'A'\n.field public static truth:Z = true\n");
  ASSERT_EQ(cls.fields.size(), 6u);
  EXPECT_EQ(std::get<FloatBits>(cls.fields[0].value).bits, 0x80000000u);
  EXPECT_EQ(std::get<FloatBits>(cls.fields[1].value).bits,
            0x8000000000000000ULL);
  EXPECT_EQ(std::get<FloatBits>(cls.fields[2].value).bits, 0x7fc00000u);
  EXPECT_EQ(std::get<int64_t>(cls.fields[3].value), -1);
  EXPECT_EQ(std::get<int64_t>(cls.fields[4].value), 65);
  EXPECT_TRUE(std::get<bool>(cls.fields[5].value));
}
TEST(MobileDalvikReader, SmaliFloatLiteralsRoundDirectlyToTheirDeclaredWidth) {
  struct Case {
    const char *text;
    uint32_t expected;
  };
  // Exact halfway values select the even significand. Values on either side
  // must not first round to the halfway double and then round a second time.
  const Case cases[] = {
      {"1.000000059604644775390625f", 0x3f800000},
      {"1.000000059604644775390625000000000000000000000000001f", 0x3f800001},
      {"1.000000059604644775390624999999999999999999999999999f", 0x3f800000},
      {"-1.000000059604644775390625f", 0xbf800000},
      {"-1.000000059604644775390625000000000000000000000000001f", 0xbf800001},
      {"-1.000000059604644775390624999999999999999999999999999f", 0xbf800000},
      {"1.000000178813934326171875f", 0x3f800002},
      {"1.000000178813934326171874999999999999999999999999999f", 0x3f800001},
      {"-1.000000178813934326171875f", 0xbf800002},
      {"-1.000000178813934326171874999999999999999999999999999f", 0xbf800001},
      {"0x1p-150f", 0x00000000},
      {"0x1.0000000000000000000000000000001p-150f", 0x00000001},
      {"-0x1p-150f", 0x80000000},
      {"-0x1.0000000000000000000000000000001p-150f", 0x80000001},
      {"0x1p-149f", 0x00000001},
      {"-0x1p-149f", 0x80000001},
  };
  for (const auto &entry : cases) {
    SCOPED_TRACE(entry.text);
    std::string literal = entry.text;
    auto cls =
        smali(".class public Lfixture/Rounding;\n.super Ljava/lang/Object;\n"
              ".field public static value:F = " +
              literal +
              "\n.method public static scalar()F\n.registers 1\nconst v0, " +
              literal +
              "\nreturn v0\n.end method\n"
              ".method public static array()[F\n.registers 2\nconst/4 v1, 1\n"
              "new-array v0, v1, [F\nfill-array-data v0, :data\n"
              "return-object v0\n:data\n.array-data 4\n" +
              literal + "\n.end array-data\n.end method\n");
    ASSERT_EQ(cls.fields.size(), 1u);
    ASSERT_EQ(cls.methods.size(), 2u);
    EXPECT_EQ(std::get<FloatBits>(cls.fields[0].value).bits, entry.expected);
    EXPECT_EQ(static_cast<uint32_t>(
                  std::get<int64_t>(cls.methods[0].instructions[0].literal)),
              entry.expected);
    ASSERT_EQ(cls.methods[1].instructions[2].data.size(), 1u);
    EXPECT_EQ(static_cast<uint32_t>(cls.methods[1].instructions[2].data[0]),
              entry.expected);
  }
}
TEST(MobileDalvikReader,
     SmaliFloatWidthParsingKeepsSpecialValuesAndBoundaries) {
  struct Case {
    const char *text;
    const char *type;
    uint64_t expected;
  };
  const Case cases[] = {
      {"0.0f", "F", 0x00000000},
      {"-0.0f", "F", 0x80000000},
      {"Infinityf", "F", 0x7f800000},
      {"-Infinityf", "F", 0xff800000},
      {"NaNf", "F", 0x7fc00000},
      {"-NaNf", "F", 0xffc00000},
      {"0x1.fffffep127f", "F", 0x7f7fffff},
      {"0.0", "D", 0x0000000000000000ULL},
      {"-0.0", "D", 0x8000000000000000ULL},
      {"Infinity", "D", 0x7ff0000000000000ULL},
      {"-Infinity", "D", 0xfff0000000000000ULL},
      {"NaN", "D", 0x7ff8000000000000ULL},
      {"-NaN", "D", 0xfff8000000000000ULL},
      {"0x1.fffffffffffffp1023", "D", 0x7fefffffffffffffULL},
      {"0x1p-1074", "D", 0x0000000000000001ULL},
      {"1.00000000000000011102230246251565404236316680908203125", "D",
       0x3ff0000000000000ULL},
      {"1.000000000000000111022302462515654042363166809082031251", "D",
       0x3ff0000000000001ULL},
  };
  for (const auto &entry : cases) {
    SCOPED_TRACE(entry.text);
    auto cls =
        smali(".class public Lfixture/Special;\n.super Ljava/lang/Object;\n"
              ".field public static value:" +
              std::string(entry.type) + " = " + entry.text + "\n");
    ASSERT_EQ(cls.fields.size(), 1u);
    const auto value = std::get<FloatBits>(cls.fields[0].value);
    EXPECT_EQ(value.wide, std::string_view(entry.type) == "D");
    EXPECT_EQ(value.bits, entry.expected);
  }
  for (const auto *text : {"3.5e38f", "-3.5e38f"}) {
    SCOPED_TRACE(text);
    EXPECT_THROW(smali(".class public Lfixture/Overflow;\n"
                       ".super Ljava/lang/Object;\n.field static value:F = " +
                       std::string(text) + "\n"),
                 Error);
  }
}
TEST(MobileDalvikReader, SmaliSwitchPayloadsAttachToActualInstructions) {
  auto cls = smali(
      methodText("packed-switch p0,:data\nconst/4 v0,-1\nreturn "
                 "v0\n:one\nconst/4 v0,7\nreturn v0\n:data\n.packed-switch "
                 "-1\n:one\n:one\n.end packed-switch"));
  auto &m = cls.methods[0];
  ASSERT_EQ(m.instructions.size(), 5u);
  EXPECT_EQ(m.instructions[0].keys, (std::vector<int32_t>{-1, 0}));
  EXPECT_EQ(m.instructions[0].targets, (std::vector<uint32_t>{3, 3}));
  EXPECT_EQ(m.code_end, 6u);
  cls = smali(methodText("sparse-switch p0,:data\nreturn p0\n:one\nreturn "
                         "p0\n:data\n.sparse-switch\n-2 -> :one\n100 -> "
                         ":one\n.end sparse-switch"));
  EXPECT_EQ(cls.methods[0].instructions[0].keys,
            (std::vector<int32_t>{-2, 100}));
}
TEST(MobileDalvikReader, SmaliArrayPayloadStoresRawBitsAndExactWidth) {
  auto cls = smali(
      methodText("fill-array-data p0,:data\nreturn-object "
                 "p0\n:data\n.array-data 4\n1,-2,0x7fffffff\n.end array-data",
                 "value([I)[I", 1));
  auto &ins = cls.methods[0].instructions[0];
  EXPECT_EQ(ins.element_width, 4u);
  ASSERT_EQ(ins.data.size(), 3u);
  EXPECT_EQ(uint32_t(ins.data[1]), 0xfffffffeu);
  cls =
      smali(methodText("fill-array-data p0,:data\nreturn-object "
                       "p0\n:data\n.array-data 4\n-0.0f,1.5f\n.end array-data",
                       "value([F)[F", 1));
  EXPECT_EQ(uint32_t(cls.methods[0].instructions[0].data[0]), 0x80000000u);
}
TEST(MobileDalvikReader, SmaliExceptionRegionAndHandlerOrderAreExact) {
  auto cls = smali(methodText(
      ":start\ndiv-int v0,p0,p1\n:end\nreturn v0\n:handler\nmove-exception "
      "v0\nconst/4 v0,-1\nreturn v0\n.catch Ljava/lang/ArithmeticException; "
      "{:start .. :end} :handler\n.catchall {:start .. :end} :handler",
      "value(II)I", 3));
  auto &region = cls.methods[0].tries[0];
  EXPECT_EQ(region.start, 0u);
  EXPECT_EQ(region.end, 1u);
  ASSERT_EQ(region.handlers.size(), 2u);
  EXPECT_EQ(region.handlers[0].type, "Ljava/lang/ArithmeticException;");
  EXPECT_FALSE(region.handlers[1].type);
  EXPECT_EQ(region.handlers[1].target, 2u);
}
FixtureAnnotation suppressLintAnnotation(std::vector<std::string> values,
                                         unsigned visibility = 0) {
  FixtureAnnotation result{"Landroid/annotation/SuppressLint;", visibility};
  result.extra_strings = {u"value"};
  for (const auto &value : values)
    result.extra_strings.push_back(utf16(value));
  result.elements = [values](std::string &out, const Fixture &f) {
    uleb(out, 1);
    uleb(out, fixtureStringIndex(f, "value"));
    append(out, 0x1c, 1);
    uleb(out, values.size());
    for (const auto &value : values)
      annotationIndex(out, 0x17, fixtureStringIndex(f, value));
  };
  return result;
}

TEST(MobileDalvikReader, DexSuppressLintPreservesDeclarationValues) {
  for (const auto &values :
       {std::vector<std::string>{},
        std::vector<std::string>{"PrivateApi", "", "PrivateApi"}}) {
    for (const auto *site : {"class", "field", "method"}) {
      SCOPED_TRACE(site);
      FixtureOptions options;
      options.annotations = {suppressLintAnnotation(values)};
      options.annotation_sets = {{0}};
      attachFixtureAnnotation(options, site);
      const auto classes = parse(fixture(options).data);
      ASSERT_EQ(classes.size(), 1u);
      const auto &cls = classes.front();
      if (std::string_view(site) == "class")
        EXPECT_EQ(cls.suppress_lint, std::optional(values));
      else if (std::string_view(site) == "field")
        EXPECT_EQ(cls.fields.at(0).suppress_lint, std::optional(values));
      else
        EXPECT_EQ(cls.methods.at(0).suppress_lint, std::optional(values));
    }
  }
}

TEST(MobileDalvikReader,
     DexSuppressLintRejectsVisibilityShapeAndDuplicateLoss) {
  for (const auto *site : {"class", "field", "method", "parameter"}) {
    SCOPED_TRACE(site);
    FixtureOptions options;
    options.annotations = {suppressLintAnnotation({"PrivateApi"})};
    options.annotation_sets = {{0}};
    attachFixtureAnnotation(options, site);
    if (std::string_view(site) == "parameter")
      EXPECT_THROW(parse(fixture(options).data), Error);
    for (unsigned visibility : {1u, 2u}) {
      auto changed = options;
      changed.annotations[0].visibility = visibility;
      EXPECT_THROW(parse(fixture(changed).data), Error);
    }
    auto changed = options;
    changed.annotations[0].elements = {};
    EXPECT_THROW(parse(fixture(changed).data), Error);
    changed = options;
    changed.annotations[0].elements = [](std::string &out, const Fixture &f) {
      uleb(out, 1);
      uleb(out, fixtureStringIndex(f, "value"));
      append(out, 0x1c, 1);
      uleb(out, 1);
      append(out, 0x1e, 1);
    };
    EXPECT_THROW(parse(fixture(changed).data), Error);
    changed = options;
    changed.annotation_sets = {{0, 0}};
    EXPECT_THROW(parse(fixture(changed).data), Error);
  }
}

std::string suppressLintSmali(std::string values = "\"PrivateApi\"") {
  return ".annotation build Landroid/annotation/SuppressLint;\nvalue = {" +
         values + "}\n.end annotation\n";
}

TEST(MobileDalvikReader, SmaliSuppressLintPreservesEmptyAndEscapedValues) {
  const auto cls =
      smali(".class public Lfixture/Lint;\n.super Ljava/lang/Object;\n" +
            suppressLintSmali("") + ".field public value:I\n" +
            suppressLintSmali("\"a\\n\\\"\\u0000\\ud800\"") +
            ".end field\n.method public native call()V\n" +
            suppressLintSmali() + ".end method\n");
  ASSERT_TRUE(cls.suppress_lint);
  EXPECT_TRUE(cls.suppress_lint->empty());
  EXPECT_EQ(cls.fields.at(0).suppress_lint,
            std::optional(std::vector<std::string>{std::string("a\n\"\0", 4) +
                                                   "\xed\xa0\x80"}));
  EXPECT_EQ(cls.methods.at(0).suppress_lint,
            std::optional(std::vector<std::string>{"PrivateApi"}));
  Budget budget;
  const auto report = recoverJava(linkClasses({cls}, budget), budget);
  const auto *units = report.getArray("source_units");
  ASSERT_NE(units, nullptr);
  ASSERT_EQ(units->size(), 1u);
  const auto *unit = units->front().getAsObject();
  ASSERT_NE(unit, nullptr);
  const auto text = unit->getString("source");
  ASSERT_TRUE(text);
  EXPECT_NE(text->str().find("@android.annotation.SuppressLint({})"),
            std::string::npos);
  EXPECT_NE(
      text->str().find("@android.annotation.SuppressLint({\"PrivateApi\"})"),
      std::string::npos);
}

TEST(MobileDalvikReader, SmaliSuppressLintRejectsDuplicatesAndParameterLoss) {
  const std::string header =
      ".class public Lfixture/Lint;\n.super Ljava/lang/Object;\n"
      ".method public call(JI)V\n.registers 5\n";
  EXPECT_THROW(smali(header + ".param p3\n" + suppressLintSmali() +
                     ".end param\nreturn-void\n.end method\n"),
               Error);
  EXPECT_THROW(smali(header + suppressLintSmali("") + suppressLintSmali() +
                     "return-void\n.end method\n"),
               Error);
  for (const auto *visibility : {"runtime", "system"})
    EXPECT_THROW(smali(header + ".annotation " + visibility +
                       " Landroid/annotation/SuppressLint;\nvalue = {}\n"
                       ".end annotation\nreturn-void\n.end method\n"),
                 Error);
  for (const auto *values : {"1", "null", "\"ok\", 1"})
    EXPECT_THROW(smali(header + suppressLintSmali(values) +
                       "return-void\n.end method\n"),
                 Error);
}

TEST(MobileDalvikReader, DexAttachedUnknownAnnotationsCannotLoseSemantics) {
  for (const auto &type : {"Lfixture/Unknown;"}) {
    for (unsigned visibility : {0u, 1u, 2u}) {
      for (const auto &attachment : {"class", "field", "method", "parameter"}) {
        SCOPED_TRACE(std::string(type) + " " + attachment + " visibility " +
                     std::to_string(visibility));
        FixtureOptions options;
        options.annotations = {{type, visibility}};
        options.annotation_sets = {{0}};
        attachFixtureAnnotation(options, attachment);
        if (std::string_view(attachment) == "class" && visibility != 2) {
          auto classes = parse(fixture(options).data);
          ASSERT_EQ(classes.size(), 1u);
          ASSERT_EQ(classes[0].marker_annotations.size(), 1u);
          EXPECT_EQ(classes[0].marker_annotations[0].type, type);
          EXPECT_EQ(classes[0].marker_annotations[0].visibility, visibility);
          Budget budget;
          EXPECT_THROW(linkClasses(std::move(classes), budget), Error);
          continue;
        }
        std::string declaration;
        if (std::string_view(attachment) == "class")
          declaration = "class Lfixture/Sample;";
        else if (std::string_view(attachment) == "field")
          declaration = "field Lfixture/Sample;->VALUE:I";
        else if (std::string_view(attachment) == "method")
          declaration = "method Lfixture/Sample;->value(I)I";
        else
          declaration = "parameter 0 of method Lfixture/Sample;->value(I)I";
        expectDexError(fixture(options).data,
                       "Invalid DEX: unsupported annotation " +
                           std::string(type) + " on " + declaration);
      }
    }
  }
}

TEST(MobileDalvikReader, DexMarkerUsesRealEnumPoolAndHasNoElementMethods) {
  const std::vector<std::string> targets{"TYPE_USE", "FIELD", "TYPE"};
  auto options = markerOptions({retentionAnnotation(),
                                targetAnnotation(targets),
                                {"Ljava/lang/annotation/Documented;", 1},
                                {"Ljava/lang/annotation/Inherited;", 1},
                                {"Ljava/lang/Deprecated;", 1}});
  const auto f = fixture(options);
  ASSERT_EQ(f.fields.size(), 4u);
  EXPECT_TRUE(f.methods.empty());
  auto classes = parse(f.data);
  ASSERT_EQ(classes.size(), 1u);
  const auto &marker = classes[0];
  EXPECT_EQ(marker.access,
            (Access{"public", "interface", "abstract", "annotation"}));
  EXPECT_EQ(marker.interfaces,
            (std::vector<std::string>{"Ljava/lang/annotation/Annotation;"}));
  EXPECT_TRUE(marker.fields.empty());
  EXPECT_TRUE(marker.methods.empty());
  EXPECT_TRUE(marker.deprecated);
  EXPECT_EQ(marker.annotation_metadata.retention, "RUNTIME");
  ASSERT_TRUE(marker.annotation_metadata.targets);
  EXPECT_EQ(*marker.annotation_metadata.targets, targets);
  EXPECT_TRUE(marker.annotation_metadata.documented);
  EXPECT_TRUE(marker.annotation_metadata.inherited);
  Budget budget;
  auto linked = linkClasses(std::move(classes), budget);
  validateSourceScopes(linked, budget);
  EXPECT_EQ(*linked.at(options.owner).annotation_metadata.targets, targets);
}

TEST(MobileDalvikReader, DexMarkerPreservesExplicitAndAbsentMetaValues) {
  const std::vector<std::optional<std::string>> policies{std::nullopt, "SOURCE",
                                                         "CLASS", "RUNTIME"};
  const std::vector<std::optional<std::vector<std::string>>> targets{
      std::nullopt, std::vector<std::string>{},
      std::vector<std::string>{"ANNOTATION_TYPE", "TYPE"}};
  for (const auto &policy : policies) {
    for (const auto &target : targets) {
      SCOPED_TRACE(policy.value_or("absent") + " target " +
                   (target ? std::to_string(target->size()) : "absent"));
      std::vector<FixtureAnnotation> annotations;
      if (policy)
        annotations.push_back(retentionAnnotation(*policy));
      if (target)
        annotations.push_back(targetAnnotation(*target));
      const auto options = markerOptions(std::move(annotations));
      auto classes = linkedDex(options);
      Budget budget;
      validateSourceScopes(classes, budget);
      validateSourceScopes(classes, budget);
      const auto &meta = classes.at(options.owner).annotation_metadata;
      EXPECT_EQ(meta.retention, policy);
      EXPECT_EQ(meta.targets, target);
      EXPECT_FALSE(meta.documented);
      EXPECT_FALSE(meta.inherited);
    }
  }
}

TEST(MobileDalvikReader, DexMetaEnumsRequireExactKindOwnerTypeAndConstant) {
  for (bool target : {false, true}) {
    const std::string annotation = target ? "Ljava/lang/annotation/Target;"
                                          : "Ljava/lang/annotation/Retention;";
    const std::string owner = target ? "Ljava/lang/annotation/ElementType;"
                                     : "Ljava/lang/annotation/RetentionPolicy;";
    const FieldRef valid{owner, target ? "TYPE" : "RUNTIME", owner};
    const auto valid_options =
        markerOptions({enumAnnotation(annotation, {valid}, target)});
    ASSERT_NO_THROW(linkedDex(valid_options));
    for (unsigned kind : {0x17u, 0x18u, 0x19u}) {
      SCOPED_TRACE(annotation + " encoded kind " + std::to_string(kind));
      EXPECT_THROW(linkedDex(markerOptions(
                       {enumAnnotation(annotation, {valid}, target, kind)})),
                   Error);
    }
    for (unsigned mutation = 0; mutation != 3; ++mutation) {
      SCOPED_TRACE(annotation + " enum identity " + std::to_string(mutation));
      auto field = valid;
      if (mutation == 0)
        field.owner = "Lfixture/OtherEnum;";
      else if (mutation == 1)
        field.type = "Ljava/lang/String;";
      else
        field.name = "UNKNOWN";
      EXPECT_THROW(linkedDex(markerOptions(
                       {enumAnnotation(annotation, {field}, target)})),
                   Error);
    }
    EXPECT_THROW(linkedDex(markerOptions(
                     {enumAnnotation(annotation, {valid}, !target)})),
                 Error);
  }
  EXPECT_THROW(linkedDex(markerOptions({targetAnnotation({"TYPE", "TYPE"})})),
               Error);
  for (const auto &newer : {"MODULE", "RECORD_COMPONENT"})
    EXPECT_THROW(linkedDex(markerOptions({targetAnnotation({newer})})), Error);
  const std::vector<std::string> java8_targets{
      "TYPE_USE",       "TYPE_PARAMETER",
      "PACKAGE",        "ANNOTATION_TYPE",
      "LOCAL_VARIABLE", "CONSTRUCTOR",
      "PARAMETER",      "METHOD",
      "FIELD",          "TYPE"};
  auto all = linkedDex(markerOptions({targetAnnotation(java8_targets)}));
  ASSERT_TRUE(all.at("Lfixture/ZMarker;").annotation_metadata.targets);
  EXPECT_EQ(*all.at("Lfixture/ZMarker;").annotation_metadata.targets,
            java8_targets);
  auto out_of_bounds = retentionAnnotation();
  out_of_bounds.elements = [](std::string &out, const Fixture &f) {
    uleb(out, 1);
    uleb(out, fixtureStringIndex(f, "value"));
    annotationIndex(out, 0x1b, UINT32_MAX);
  };
  expectDexError(fixture(markerOptions({out_of_bounds})).data,
                 "Invalid DEX: field index out of bounds");
}

TEST(MobileDalvikReader, DexMetaValuesCannotBeMissingExtraOrRepeated) {
  for (const auto &type :
       {"Ljava/lang/annotation/Retention;", "Ljava/lang/annotation/Target;"}) {
    SCOPED_TRACE(type);
    EXPECT_THROW(linkedDex(markerOptions({{type, 1}})), Error);
  }
  auto duplicate = retentionAnnotation();
  EXPECT_THROW(linkedDex(markerOptions({duplicate, duplicate})), Error);
  for (const auto &base :
       {retentionAnnotation(), targetAnnotation({"TYPE"}),
        FixtureAnnotation{"Ljava/lang/annotation/Documented;", 1},
        FixtureAnnotation{"Ljava/lang/annotation/Inherited;", 1}}) {
    SCOPED_TRACE(base.type);
    auto extra = base;
    extra.extra_strings = {u"other", u"value"};
    extra.elements = [](std::string &out, const Fixture &f) {
      uleb(out, 2);
      for (const auto &key : {"other", "value"}) {
        uleb(out, fixtureStringIndex(f, key));
        append(out, 0x1f, 1); // Two well-formed false values, neither ignored.
      }
    };
    EXPECT_THROW(linkedDex(markerOptions({extra})), Error);
  }
}

TEST(MobileDalvikReader, DexMetaVisibilityAndDeclarationRolesRemainExact) {
  for (const auto &annotation :
       {retentionAnnotation(), targetAnnotation({"TYPE"}),
        FixtureAnnotation{"Ljava/lang/annotation/Documented;", 1},
        FixtureAnnotation{"Ljava/lang/annotation/Inherited;", 1}}) {
    for (unsigned visibility : {0u, 2u, 3u}) {
      SCOPED_TRACE(annotation.type + " visibility " +
                   std::to_string(visibility));
      auto wrong = annotation;
      wrong.visibility = visibility;
      EXPECT_THROW(linkedDex(markerOptions({wrong})), Error);
    }
    for (const auto &site : {"class", "field", "method", "parameter"}) {
      SCOPED_TRACE(annotation.type + " on " + site);
      FixtureOptions options;
      options.annotations = {annotation};
      options.annotation_sets = {{0}};
      attachFixtureAnnotation(options, site);
      EXPECT_THROW(linkedDex(options), Error);
    }
  }
}

TEST(MobileDalvikReader, DexMarkerApplicationsLinkAcrossInputsAndRetainPolicy) {
  for (const auto &policy : {"absent", "CLASS", "RUNTIME"}) {
    SCOPED_TRACE(policy);
    const unsigned visibility = std::string_view(policy) == "RUNTIME" ? 1 : 0;
    auto definition = markerOptions({targetAnnotation({"TYPE"})});
    if (std::string_view(policy) != "absent") {
      definition.annotations.push_back(retentionAnnotation(policy));
      definition.annotation_sets[0].push_back(1);
    }
    FixtureOptions use;
    use.owner = "Lfixture/AUse;";
    use.annotations = {{definition.owner, visibility}};
    use.annotation_sets = {{0}};
    use.class_annotations = 0;
    auto users = parse(fixture(use).data);
    ASSERT_EQ(users.size(), 1u);
    ASSERT_EQ(users[0].marker_annotations.size(), 1u);
    EXPECT_EQ(users[0].marker_annotations[0].type, definition.owner);
    EXPECT_EQ(users[0].marker_annotations[0].visibility, visibility);
    Budget unresolved_budget;
    EXPECT_THROW(linkClasses(users, unresolved_budget), Error);
    auto definitions = parse(fixture(definition).data);
    users[0].source_id = "classes.dex";
    definitions[0].source_id = "classes2.dex";
    users.push_back(definitions[0]);
    Budget budget;
    auto linked = linkClasses(users, budget);
    validateSourceScopes(linked, budget);
    EXPECT_EQ(linked.at(use.owner).marker_annotations[0].visibility,
              visibility);
    EXPECT_EQ(linked.at(definition.owner).source_id, "classes2.dex");
    use.annotations[0].visibility = 1 - visibility;
    auto wrong = parse(fixture(use).data);
    wrong.push_back(definitions[0]);
    Budget wrong_budget;
    EXPECT_THROW(linkClasses(std::move(wrong), wrong_budget), Error);
  }
}

TEST(MobileDalvikReader, DexMarkerApplicationsRejectSourceAndPayloadLoss) {
  auto definition = markerOptions({retentionAnnotation("SOURCE")});
  auto definitions = parse(fixture(definition).data);
  for (unsigned visibility : {0u, 1u}) {
    FixtureOptions use;
    use.annotations = {{definition.owner, visibility}};
    use.annotation_sets = {{0}};
    use.class_annotations = 0;
    auto classes = parse(fixture(use).data);
    classes.push_back(definitions[0]);
    Budget budget;
    EXPECT_THROW(linkClasses(std::move(classes), budget), Error);
    use.annotations[0].extra_strings = {u"value"};
    use.annotations[0].elements = [](std::string &out, const Fixture &f) {
      uleb(out, 1);
      uleb(out, fixtureStringIndex(f, "value"));
      append(out, 0x1f, 1);
    };
    EXPECT_THROW(parse(fixture(use).data), Error);
    use.annotations[0].elements = {};
    use.annotations.push_back({definition.owner, visibility});
    use.annotation_sets[0].push_back(1);
    EXPECT_THROW(parse(fixture(use).data), Error);
  }
}

TEST(MobileDalvikReader, DexReservedEmptyAnnotationsCannotBecomeMarkers) {
  for (const auto &Type : {"Ldalvik/annotation/Throws;",
                           "Ldalvik/annotation/AnnotationDefault;"}) {
    SCOPED_TRACE(Type);
    auto definition = markerOptions({retentionAnnotation()});
    definition.owner = Type;
    FixtureOptions use;
    use.annotations = {{Type, 1}};
    use.annotation_sets = {{0}};
    use.class_annotations = 0;
    auto classes = parse(fixture(use).data);
    ASSERT_EQ(classes.size(), 1u);
    ASSERT_EQ(classes[0].marker_annotations.size(), 1u);
    EXPECT_EQ(classes[0].marker_annotations[0].type, Type);
    auto definitions = parse(fixture(definition).data);
    classes.push_back(definitions[0]);
    Budget budget;
    try {
      (void)linkClasses(std::move(classes), budget);
      FAIL() << "Reserved annotation was reinterpreted as a user marker";
    } catch (const Error &error) {
      EXPECT_NE(std::string(error.what()).find("reserved marker annotation"),
                std::string::npos);
    }
  }
}

TEST(MobileDalvikReader, SmaliMarkerMetaGrammarPreservesEnumsAndOrder) {
  const std::string header =
      ".class public interface abstract annotation Lfixture/Marker;\n"
      ".super Ljava/lang/Object;\n"
      ".implements Ljava/lang/annotation/Annotation;\n";
  for (const auto &policy : {"SOURCE", "CLASS", "RUNTIME"}) {
    SCOPED_TRACE(policy);
    auto cls =
        smali(header +
              ".annotation runtime Ljava/lang/annotation/Retention;\n"
              "value = .enum Ljava/lang/annotation/RetentionPolicy;->" +
              policy +
              ":Ljava/lang/annotation/RetentionPolicy;\n.end annotation\n"
              ".annotation runtime Ljava/lang/annotation/Target;\n"
              "value = {\n.enum Ljava/lang/annotation/ElementType;->"
              "ANNOTATION_TYPE:Ljava/lang/annotation/ElementType;,\n"
              ".enum Ljava/lang/annotation/ElementType;->"
              "TYPE:Ljava/lang/annotation/ElementType;\n}\n.end annotation\n"
              ".annotation runtime Ljava/lang/annotation/Documented;\n"
              ".end annotation\n"
              ".annotation runtime Ljava/lang/annotation/Inherited;\n"
              ".end annotation\n");
    EXPECT_EQ(cls.annotation_metadata.retention, policy);
    ASSERT_TRUE(cls.annotation_metadata.targets);
    EXPECT_EQ(*cls.annotation_metadata.targets,
              (std::vector<std::string>{"ANNOTATION_TYPE", "TYPE"}));
    EXPECT_TRUE(cls.annotation_metadata.documented);
    EXPECT_TRUE(cls.annotation_metadata.inherited);
    EXPECT_TRUE(cls.fields.empty());
    EXPECT_TRUE(cls.methods.empty());
    Budget budget;
    EXPECT_NO_THROW(linkClasses({cls}, budget));
  }
  auto absent = smali(header);
  auto empty =
      smali(header + ".annotation runtime Ljava/lang/annotation/Target;\n"
                     "value = {}\n.end annotation\n");
  EXPECT_FALSE(absent.annotation_metadata.targets);
  ASSERT_TRUE(empty.annotation_metadata.targets);
  EXPECT_TRUE(empty.annotation_metadata.targets->empty());
  EXPECT_FALSE(empty.annotation_metadata.retention);
}

TEST(MobileDalvikReader, SmaliMarkerMetadataRejectsMalformedEnumAndMetaSites) {
  const std::string header =
      ".class public interface abstract annotation Lfixture/Marker;\n"
      ".super Ljava/lang/Object;\n"
      ".implements Ljava/lang/annotation/Annotation;\n";
  const std::string start =
      ".annotation runtime Ljava/lang/annotation/Retention;\n";
  const std::string valid =
      "value = .enum Ljava/lang/annotation/RetentionPolicy;->"
      "RUNTIME:Ljava/lang/annotation/RetentionPolicy;\n";
  for (const auto &value :
       {"value = Ljava/lang/annotation/RetentionPolicy;->RUNTIME:"
        "Ljava/lang/annotation/RetentionPolicy;\n",
        "value = .enum Lfixture/Policy;->RUNTIME:"
        "Ljava/lang/annotation/RetentionPolicy;\n",
        "value = .enum Ljava/lang/annotation/RetentionPolicy;->RUNTIME:"
        "Ljava/lang/String;\n",
        "value = .enum Ljava/lang/annotation/RetentionPolicy;->UNKNOWN:"
        "Ljava/lang/annotation/RetentionPolicy;\n",
        "value = {}\n", "other = 1\n", ""}) {
    SCOPED_TRACE(value);
    Budget budget;
    EXPECT_THROW(
        linkClasses({smali(header + start + value + ".end annotation\n")},
                    budget),
        Error);
  }
  EXPECT_THROW(smali(header + start + valid + valid + ".end annotation\n"),
               Error);
  const auto annotation = start + valid + ".end annotation\n";
  EXPECT_THROW(smali(header + annotation + annotation), Error);
  for (const auto &visibility : {"build", "system"})
    EXPECT_THROW(smali(header + ".annotation " + visibility +
                       " Ljava/lang/annotation/Retention;\n" + valid +
                       ".end annotation\n"),
                 Error);
  for (const auto &meta : {"Retention", "Target", "Documented", "Inherited"}) {
    SCOPED_TRACE(meta);
    const std::string bare = ".annotation runtime Ljava/lang/annotation/" +
                             std::string(meta) + ";\n.end annotation\n";
    EXPECT_THROW(smali(header + ".field public static final flag:I = 0\n" +
                       bare + ".end field\n"),
                 Error);
    EXPECT_THROW(smali(header + ".method public abstract item()I\n" + bare +
                       ".end method\n"),
                 Error);
  }
}

TEST(MobileDalvikReader, SmaliEmptyClassApplicationsRequireLinkedDefinition) {
  const std::string definition =
      ".class public interface abstract annotation Lfixture/ZMarker;\n"
      ".super Ljava/lang/Object;\n"
      ".implements Ljava/lang/annotation/Annotation;\n";
  const std::string use =
      ".class public Lfixture/AUse;\n.super Ljava/lang/Object;\n"
      ".annotation build Lfixture/ZMarker;\n.end annotation\n";
  auto cls = smali(use);
  ASSERT_EQ(cls.marker_annotations.size(), 1u);
  EXPECT_EQ(cls.marker_annotations[0].type, "Lfixture/ZMarker;");
  EXPECT_EQ(cls.marker_annotations[0].visibility, 0u);
  Budget missing_budget;
  EXPECT_THROW(linkClasses({cls}, missing_budget), Error);
  Budget linked_budget;
  auto linked = linkClasses({cls, smali(definition)}, linked_budget);
  EXPECT_EQ(linked.at(cls.name).marker_annotations[0].visibility, 0u);
  EXPECT_FALSE(linked.at("Lfixture/ZMarker;").annotation_metadata.retention);
  EXPECT_THROW(smali(use + ".annotation build Lfixture/ZMarker;\n"
                           ".end annotation\n"),
               Error);
  EXPECT_THROW(smali(".class public Lfixture/AUse;\n"
                     ".super Ljava/lang/Object;\n"
                     ".annotation build Lfixture/ZMarker;\n"
                     "value = 1\n.end annotation\n"),
               Error);
}

TEST(MobileDalvikReader, DexDeprecatedBindsOnlyItsExactDeclaration) {
  for (const auto &attachment : {"class", "field", "method"}) {
    SCOPED_TRACE(attachment);
    FixtureOptions options;
    options.annotations = {{"Ljava/lang/Deprecated;", 1}};
    options.annotation_sets = {{0}};
    attachFixtureAnnotation(options, attachment);
    auto classes = parse(fixture(options).data);
    ASSERT_EQ(classes.size(), 1u);
    const auto &cls = classes[0];
    EXPECT_EQ(cls.deprecated, std::string_view(attachment) == "class");
    ASSERT_EQ(cls.methods.size(), 1u);
    EXPECT_EQ(cls.methods[0].deprecated,
              std::string_view(attachment) == "method");
    EXPECT_EQ(cls.methods[0].reference.identity(),
              "Lfixture/Sample;->value(I)I");
    ASSERT_EQ(cls.methods[0].instructions.size(), 1u);
    EXPECT_EQ(cls.methods[0].instructions[0].opcode, "return");
    if (std::string_view(attachment) == "field") {
      ASSERT_EQ(cls.fields.size(), 1u);
      EXPECT_TRUE(cls.fields[0].deprecated);
      EXPECT_EQ(cls.fields[0].reference.name, "VALUE");
    }
  }
  auto classes = parse(fixture().data);
  EXPECT_FALSE(classes[0].deprecated);
  EXPECT_FALSE(classes[0].methods[0].deprecated);
}

TEST(MobileDalvikReader, DexDeprecatedCachedSetBindsEveryDefinedSite) {
  FixtureOptions options;
  options.annotations = {{"Ljava/lang/Deprecated;", 1}};
  options.annotation_sets = {{0}, {0}};
  options.class_annotations = 0;
  attachFixtureAnnotation(options, "field");
  options.method_annotations = 0;
  auto classes = parse(fixture(options).data);
  ASSERT_EQ(classes.size(), 1u);
  EXPECT_TRUE(classes[0].deprecated);
  ASSERT_EQ(classes[0].fields.size(), 1u);
  EXPECT_TRUE(classes[0].fields[0].deprecated);
  ASSERT_EQ(classes[0].methods.size(), 1u);
  EXPECT_TRUE(classes[0].methods[0].deprecated);
  EXPECT_EQ(classes[0].methods[0].reference.identity(),
            "Lfixture/Sample;->value(I)I");

  // An unattached, structurally valid marker must not mark a declaration.
  options.class_annotations.reset();
  options.field_annotations.reset();
  options.method_annotations.reset();
  classes = parse(fixture(options).data);
  EXPECT_FALSE(classes[0].deprecated);
  EXPECT_FALSE(classes[0].fields[0].deprecated);
  EXPECT_FALSE(classes[0].methods[0].deprecated);
}

TEST(MobileDalvikReader, DexDeprecatedRejectsVisibilityAndElementLoss) {
  for (const auto &attachment : {"class", "field", "method"}) {
    SCOPED_TRACE(attachment);
    FixtureOptions options;
    options.annotations = {{"Ljava/lang/Deprecated;", 1}};
    options.annotation_sets = {{0}};
    attachFixtureAnnotation(options, attachment);
    std::string declaration;
    if (std::string_view(attachment) == "class")
      declaration = "class Lfixture/Sample;";
    else if (std::string_view(attachment) == "field")
      declaration = "field Lfixture/Sample;->VALUE:I";
    else
      declaration = "method Lfixture/Sample;->value(I)I";
    for (unsigned visibility : {0u, 2u}) {
      auto changed = options;
      changed.annotations[0].visibility = visibility;
      expectDexError(fixture(changed).data,
                     "Invalid DEX: Deprecated annotation on " + declaration +
                         " requires runtime visibility");
    }
    for (const auto &key : {"since", "forRemoval", "value"}) {
      SCOPED_TRACE(key);
      auto changed = options;
      changed.annotations[0].extra_strings = {utf16(key), u"9"};
      changed.annotations[0].elements = [key](std::string &out,
                                              const Fixture &f) {
        uleb(out, 1);
        uleb(out, fixtureStringIndex(f, key));
        if (std::string_view(key) == "since")
          annotationIndex(out, 0x17, fixtureStringIndex(f, "9"));
        else if (std::string_view(key) == "forRemoval")
          append(out, 0x3f, 1); // VALUE_BOOLEAN, true.
        else {
          append(out, 0x04, 1); // VALUE_INT, one byte.
          append(out, 1, 1);
        }
      };
      expectDexError(fixture(changed).data,
                     "Invalid DEX: Deprecated annotation on " + declaration +
                         " must have no elements");
    }
    auto changed = options;
    changed.annotations.push_back({"Ljava/lang/Deprecated;", 1});
    changed.annotation_sets[0].push_back(1);
    expectDexError(fixture(changed).data,
                   "Invalid DEX: annotation types are duplicate or unordered");
  }
}

TEST(MobileDalvikReader, DexDeprecatedConstructorRetainsItsRealInvoke) {
  FixtureOptions options;
  options.method_name = "<init>";
  options.params.clear();
  options.returns = "V";
  options.flags = 0x10001;
  options.words = {0x1070, 0, 0, 0x000e};
  options.referenced_method =
      MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"};
  options.annotations = {{"Ljava/lang/Deprecated;", 1}};
  options.annotation_sets = {{0}};
  options.method_annotations = 0;
  auto f = fixture(options);
  auto found =
      std::find(f.methods.begin(), f.methods.end(), *options.referenced_method);
  ASSERT_NE(found, f.methods.end());
  patch(f.data, f.at.at("code") + 18, found - f.methods.begin(), 2);
  auto classes = parse(seal(f.data));
  ASSERT_EQ(classes.size(), 1u);
  ASSERT_EQ(classes[0].methods.size(), 1u);
  const auto &method = classes[0].methods[0];
  EXPECT_TRUE(method.deprecated);
  EXPECT_EQ(method.reference.identity(), "Lfixture/Sample;-><init>()V");
  ASSERT_EQ(method.instructions.size(), 2u);
  EXPECT_EQ(method.instructions[0].opcode, "invoke-direct");
  EXPECT_EQ(std::get<MethodRef>(method.instructions[0].reference),
            *options.referenced_method);
  EXPECT_EQ(method.instructions[1].opcode, "return-void");
}

TEST(MobileDalvikReader, SmaliDeprecatedPreservesDeclarationsAndConstructor) {
  const std::string marker =
      ".annotation runtime Ljava/lang/Deprecated;\n.end annotation\n";
  auto cls =
      smali(".class public Lfixture/Sample;\n.super Ljava/lang/Object;\n" +
            marker + ".field public value:I\n" + marker +
            ".end field\n"
            ".method public static value(I)I\n.registers 1\n" +
            marker +
            "return p0\n.end method\n"
            ".method public constructor <init>()V\n.registers 1\n" +
            marker +
            "invoke-direct {p0}, Ljava/lang/Object;-><init>()V\n"
            "return-void\n.end method\n");
  EXPECT_TRUE(cls.deprecated);
  ASSERT_EQ(cls.fields.size(), 1u);
  EXPECT_TRUE(cls.fields[0].deprecated);
  ASSERT_EQ(cls.methods.size(), 2u);
  EXPECT_TRUE(cls.methods[0].deprecated);
  EXPECT_TRUE(cls.methods[1].deprecated);
  EXPECT_EQ(cls.methods[0].reference.identity(), "Lfixture/Sample;->value(I)I");
  ASSERT_EQ(cls.methods[0].instructions.size(), 1u);
  EXPECT_EQ(cls.methods[0].instructions[0].opcode, "return");
  EXPECT_EQ(cls.methods[1].reference.identity(), "Lfixture/Sample;-><init>()V");
  ASSERT_EQ(cls.methods[1].instructions.size(), 2u);
  EXPECT_EQ(cls.methods[1].instructions[0].opcode, "invoke-direct");
  EXPECT_EQ(std::get<MethodRef>(cls.methods[1].instructions[0].reference),
            (MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}));
  EXPECT_EQ(cls.methods[1].instructions[1].opcode, "return-void");
  auto plain = smali(methodText("return p0", "value(I)I", 1));
  EXPECT_FALSE(plain.deprecated);
  EXPECT_FALSE(plain.methods[0].deprecated);
}

TEST(MobileDalvikReader, SmaliDeprecatedRetainsVisibilityShapeAndSiteGuards) {
  const std::string start =
      ".class public Lfixture/Sample;\n.super Ljava/lang/Object;\n";
  const std::string method = ".method public static value(I)I\n.registers 1\n";
  const std::string end = "return p0\n.end method\n";
  const std::string marker =
      ".annotation runtime Ljava/lang/Deprecated;\n.end annotation\n";
  auto source = [&](std::string_view site, const std::string &annotation) {
    if (site == "class")
      return start + annotation + method + end;
    if (site == "field")
      return start + ".field public value:I\n" + annotation + ".end field\n" +
             method + end;
    return start + method + annotation + end;
  };
  auto reject = [&](const std::string &text, std::string_view reason) {
    try {
      smali(text);
      FAIL() << "Expected Deprecated metadata rejection";
    } catch (const Error &error) {
      EXPECT_NE(std::string(error.what()).find(reason), std::string::npos)
          << error.what();
    }
  };
  for (const auto &site : {"class", "field", "method"}) {
    SCOPED_TRACE(site);
    for (const auto &visibility : {"build", "system"})
      reject(source(site, ".annotation " + std::string(visibility) +
                              " Ljava/lang/Deprecated;\n.end annotation\n"),
             "requires runtime visibility");
    for (const auto &element :
         {"since = \"9\"", "forRemoval = true", "value = 1"})
      reject(source(site, ".annotation runtime Ljava/lang/Deprecated;\n" +
                              std::string(element) + "\n.end annotation\n"),
             "must have no elements");
    reject(source(site, marker + marker), "duplicate Deprecated annotation");
  }
  reject(start + method + ".param p0\n" + marker + ".end param\n" + end,
         "parameter annotations are not represented");
  auto cls = smali(start + method + ".param p0\n.end param\n" + marker + end);
  EXPECT_TRUE(cls.methods[0].deprecated);
  reject(start + method +
             ".annotation system Lfixture/Unknown;\n.end annotation\n" + end,
         "unsupported annotation Lfixture/Unknown;");
}

TEST(MobileDalvikReader, DexSystemClassAnnotationsKeepStructureAndVisibility) {
  FixtureOptions options;
  options.owner = "Lfixture/Outer$Nested;";
  options.annotations = {enclosingClassAnnotation(), innerClassAnnotation(),
                         typeArrayAnnotation()};
  options.annotation_sets = {{2, 1, 0}};
  options.class_annotations = 0;
  auto parsed = parse(fixture(options).data);
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_EQ(parsed[0].name, "Lfixture/Outer$Nested;");
  EXPECT_EQ(parsed[0].enclosing, "Lfixture/Outer;");
  EXPECT_TRUE(parsed[0].inner_class_present);
  EXPECT_FALSE(parsed[0].enclosing_method);
  EXPECT_EQ(parsed[0].inner_name, "Nested");
  EXPECT_EQ(parsed[0].inner_access, (Access{"public", "static"}));
  ASSERT_EQ(parsed[0].methods.size(), 1u);
  EXPECT_EQ(parsed[0].methods[0].reference.identity(),
            "Lfixture/Outer$Nested;->value(I)I");
  ASSERT_EQ(parsed[0].methods[0].instructions.size(), 1u);
  EXPECT_EQ(parsed[0].methods[0].instructions[0].opcode, "return");
  for (size_t index = 0; index < options.annotations.size(); ++index) {
    for (unsigned visibility : {0u, 1u}) {
      auto changed = options;
      changed.annotations[index].visibility = visibility;
      const auto &type = changed.annotations[index].type;
      SCOPED_TRACE(type + " visibility " + std::to_string(visibility));
      expectDexError(fixture(changed).data,
                     "Invalid DEX: structural annotation " + type +
                         " on class " + options.owner +
                         " requires system visibility");
    }
  }
}

TEST(MobileDalvikReader, DexKnownClassAnnotationsDoNotAuthorizeMemberUses) {
  FixtureOptions options;
  options.annotations = {typeArrayAnnotation()};
  // Two different sets share the same correctly encoded annotation item.
  options.annotation_sets = {{0}, {0}};
  options.class_annotations = 0;
  for (size_t set : {size_t(0), size_t(1)}) {
    // Also attach the class's exact nonempty set to the method.
    options.method_annotations = set;
    expectDexError(fixture(options).data,
                   "Invalid DEX: unsupported annotation "
                   "Ldalvik/annotation/MemberClasses; "
                   "on method Lfixture/Sample;->value(I)I");
  }

  options.annotations = {typeArrayAnnotation("Ldalvik/annotation/Throws;",
                                             {"Ljava/io/IOException;"})};
  options.class_annotations.reset();
  options.method_annotations = 0;
  options.annotation_sets = {{0}};
  auto parsed = parse(fixture(options).data);
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_EQ(parsed[0].methods[0].declared_throws,
            (std::vector<std::string>{"Ljava/io/IOException;"}));
  options.method_annotations.reset();
  attachFixtureAnnotation(options, "field");
  expectDexError(fixture(options).data, "Invalid DEX: unsupported annotation "
                                        "Ldalvik/annotation/Throws; on field "
                                        "Lfixture/Sample;->VALUE:I");
}

TEST(MobileDalvikReader, DexSignaturesBindAllDeclarationsAndExactFragments) {
  FixtureOptions options;
  options.params = {"Ljava/lang/Object;"};
  options.returns = "Ljava/lang/Object;";
  options.words = {0x0011};
  options.field_type = "Ljava/util/List;";
  options.static_value = std::string(1, '\x1e');
  options.annotations = {
      signatureAnnotation({"<T:Ljava/lang/", "Object;>Ljava/lang/Object;"}),
      signatureAnnotation({"Ljava/util/L", "ist<Ljava/lang/String;>;"}),
      signatureAnnotation(
          {"<U:Ljava/lang/Object;>(T", "U;)TU;", "^Ljava/lang/Exception;"}),
      typeArrayAnnotation("Ldalvik/annotation/Throws;",
                          {"Ljava/lang/Exception;"}),
      {"Ljava/lang/Deprecated;", 1}};
  options.annotation_sets = {{0, 4}, {1, 4}, {2, 3, 4}};
  options.class_annotations = 0;
  options.field_annotations = 1;
  options.method_annotations = 2;
  auto classes = parse(fixture(options).data);
  ASSERT_EQ(classes.size(), 1u);
  const auto &cls = classes[0];
  EXPECT_TRUE(cls.deprecated);
  EXPECT_EQ(cls.generic_signature, "<T:Ljava/lang/Object;>Ljava/lang/Object;");
  ASSERT_EQ(cls.fields.size(), 1u);
  EXPECT_TRUE(cls.fields[0].deprecated);
  EXPECT_EQ(cls.fields[0].generic_signature,
            "Ljava/util/List<Ljava/lang/String;>;");
  ASSERT_EQ(cls.methods.size(), 1u);
  EXPECT_TRUE(cls.methods[0].deprecated);
  EXPECT_EQ(cls.methods[0].generic_signature,
            "<U:Ljava/lang/Object;>(TU;)TU;^Ljava/lang/Exception;");
  EXPECT_EQ(cls.methods[0].declared_throws,
            (std::vector<std::string>{"Ljava/lang/Exception;"}));
  ASSERT_EQ(cls.methods[0].instructions.size(), 1u);
  EXPECT_EQ(cls.methods[0].instructions[0].opcode, "return-object");
  Budget budget;
  auto linked = linkClasses(std::move(classes), budget);
  auto plans = validateGenericSignatures(linked, budget);
  const auto &method = linked.at(options.owner).methods[0];
  EXPECT_EQ(plans.methods.at(method.reference)
                .types[*plans.methods.at(method.reference).result]
                .erasure,
            method.reference.returns);
}

TEST(MobileDalvikReader, DexSourceAnnotationsRejectWrongSitesAndValueKinds) {
  FixtureOptions options;
  options.annotations = {signatureAnnotation({"(I)I"})};
  options.annotation_sets = {{0}};
  options.method_annotations = 0;
  for (unsigned visibility : {0u, 1u}) {
    auto changed = options;
    changed.annotations[0].visibility = visibility;
    expectDexError(fixture(changed).data,
                   "Invalid DEX: source annotation "
                   "Ldalvik/annotation/Signature; on method "
                   "Lfixture/Sample;->value(I)I requires system visibility");
  }
  auto changed = options;
  changed.method_annotations.reset();
  attachFixtureAnnotation(changed, "parameter");
  expectDexError(fixture(changed).data,
                 "Invalid DEX: unsupported annotation "
                 "Ldalvik/annotation/Signature; on "
                 "parameter 0 of method Lfixture/Sample;->value(I)I");
  changed = options;
  changed.annotations[0].elements = [](std::string &out, const Fixture &f) {
    uleb(out, 1);
    uleb(out, fixtureStringIndex(f, "value"));
    annotationIndex(out, 0x17, fixtureStringIndex(f, "(I)I"));
  };
  expectDexError(fixture(changed).data,
                 "Invalid DEX: invalid Ldalvik/annotation/Signature; value "
                 "on method Lfixture/Sample;->value(I)I");
  changed.annotations[0].elements = [](std::string &out, const Fixture &f) {
    uleb(out, 1);
    uleb(out, fixtureStringIndex(f, "value"));
    append(out, 0x1c, 1);
    uleb(out, 1);
    annotationIndex(out, 0x18, fixtureTypeIndex(f, "Ljava/lang/Object;"));
  };
  expectDexError(fixture(changed).data,
                 "Invalid DEX: Signature fragment is not a string on method "
                 "Lfixture/Sample;->value(I)I");
  changed = options;
  changed.annotations = {typeArrayAnnotation("Ldalvik/annotation/Throws;",
                                             {"[Ljava/lang/Exception;"})};
  expectDexError(fixture(changed).data,
                 "Invalid DEX: Throws entry is not a class on method "
                 "Lfixture/Sample;->value(I)I");
}

TEST(MobileDalvikReader, DexSharedSourceSetsBindEachDefinedDeclaration) {
  FixtureOptions options;
  options.field_type = "Ljava/lang/Object;";
  options.static_value = std::string(1, '\x1e');
  options.annotations = {signatureAnnotation({"Ljava/lang/Object;"})};
  options.annotation_sets = {{0}};
  options.class_annotations = 0;
  options.field_annotations = 0;
  auto classes = parse(fixture(options).data);
  ASSERT_EQ(classes.size(), 1u);
  EXPECT_EQ(classes[0].generic_signature, "Ljava/lang/Object;");
  ASSERT_EQ(classes[0].fields.size(), 1u);
  EXPECT_EQ(classes[0].fields[0].generic_signature, "Ljava/lang/Object;");
  EXPECT_FALSE(classes[0].methods[0].generic_signature);
  Budget budget;
  auto linked = linkClasses(std::move(classes), budget);
  EXPECT_EQ(linked.at(options.owner).methods.size(), 1u);
}

TEST(MobileDalvikReader, DexAnnotatedMethodMustHaveActualClassDataDefinition) {
  for (const auto &annotation :
       {signatureAnnotation({"(I)I"}),
        FixtureAnnotation{"Ljava/lang/Deprecated;", 1}}) {
    SCOPED_TRACE(annotation.type);
    for (bool foreign_owner : {false, true}) {
      SCOPED_TRACE(foreign_owner);
      FixtureOptions options;
      options.annotations = {annotation};
      options.annotation_sets = {{0}};
      options.method_annotations = 0;
      options.referenced_method =
          MethodRef{foreign_owner ? "Lfixture/Other;" : options.owner,
                    "unused",
                    {"I"},
                    "I"};
      auto f = fixture(options);
      auto found = std::find(f.methods.begin(), f.methods.end(),
                             *options.referenced_method);
      ASSERT_NE(found, f.methods.end());
      patch(f.data, f.at.at("annotations") + 16,
            unsigned(found - f.methods.begin()));
      expectDexError(seal(f.data),
                     foreign_owner
                         ? "Invalid DEX: annotation directory owner mismatch"
                         : "Invalid DEX: annotated method has no class_data "
                           "definition");
    }
  }
}

TEST(MobileDalvikReader, EmptyAndNonJvmSignatureMetadataIsNotSilentlyLost) {
  for (const auto &pieces :
       {std::vector<std::string>{}, std::vector<std::string>{"not JVM"}}) {
    FixtureOptions options;
    options.annotations = {signatureAnnotation(pieces)};
    options.annotation_sets = {{0}};
    options.method_annotations = 0;
    auto classes = parse(fixture(options).data);
    ASSERT_TRUE(classes[0].methods[0].generic_signature);
    Budget budget;
    try {
      linkClasses(std::move(classes), budget);
      FAIL() << "Signature metadata was silently discarded";
    } catch (const Error &error) {
      EXPECT_NE(std::string(error.what()).find("Invalid JVM generic signature"),
                std::string::npos);
      EXPECT_NE(std::string(error.what()).find("Lfixture/Sample;->value(I)I"),
                std::string::npos);
    }
  }
}

TEST(MobileDalvikReader, SmaliSourceMetadataMatchesDexTypedBinding) {
  auto cls = smali(
      ".class public Lfixture/Box;\n.super Ljava/lang/Object;\n"
      ".annotation system Ldalvik/annotation/Signature;\n"
      "value = {\"<T:Ljava/lang/\", \"Object;>Ljava/lang/Object;\"}\n"
      ".end annotation\n"
      ".field public value:Ljava/lang/Object;\n"
      ".annotation system Ldalvik/annotation/Signature;\n"
      "value = {\"T\", \"T;\"}\n.end annotation\n.end field\n"
      ".method public static identity(Ljava/lang/Object;)Ljava/lang/Object;\n"
      ".registers 1\n"
      ".annotation system Ldalvik/annotation/Signature;\n"
      "value = {\"<U:Ljava/lang/Object;>(TU;)TU;\"}\n.end annotation\n"
      ".annotation system Ldalvik/annotation/Throws;\n"
      "value = {Ljava/lang/Exception;}\n.end annotation\n"
      "return-object p0\n.end method\n");
  EXPECT_EQ(cls.generic_signature, "<T:Ljava/lang/Object;>Ljava/lang/Object;");
  ASSERT_EQ(cls.fields.size(), 1u);
  EXPECT_EQ(cls.fields[0].generic_signature, "TT;");
  ASSERT_EQ(cls.methods.size(), 1u);
  EXPECT_EQ(cls.methods[0].generic_signature, "<U:Ljava/lang/Object;>(TU;)TU;");
  EXPECT_EQ(cls.methods[0].declared_throws,
            (std::vector<std::string>{"Ljava/lang/Exception;"}));
  Budget budget;
  auto linked = linkClasses({cls}, budget);
  auto plans = validateGenericSignatures(linked, budget);
  EXPECT_EQ(plans.methods.at(cls.methods[0].reference).parameters.size(), 1u);
  EXPECT_EQ(plans.fields.at(cls.fields[0].reference)
                .types[*plans.fields.at(cls.fields[0].reference).field_type]
                .variable_owner,
            cls.name);
}

TEST(MobileDalvikReader, SmaliGenericAnnotationsRetainSiteAndDuplicateGuards) {
  const std::string start =
      ".class public Lfixture/Box;\n.super Ljava/lang/Object;\n"
      ".method public static value(I)I\n.registers 1\n";
  const std::string annotation =
      ".annotation system Ldalvik/annotation/Signature;\n"
      "value = {\"(I)I\"}\n.end annotation\n";
  auto reject = [&](const std::string &text, std::string_view reason) {
    try {
      smali(text);
      FAIL() << "Expected metadata rejection";
    } catch (const Error &error) {
      EXPECT_NE(std::string(error.what()).find(reason), std::string::npos)
          << error.what();
    }
  };
  reject(start + annotation + annotation + "return p0\n.end method\n",
         "duplicate source annotation");
  reject(start + ".param p0\n" + annotation +
             ".end param\nreturn p0\n.end method\n",
         "parameter annotations are not represented");
  auto cls = smali(start + ".param p0\n.end param\n" + annotation +
                   "return p0\n.end method\n");
  EXPECT_EQ(cls.methods[0].generic_signature, "(I)I");
  reject(start + ".annotation runtime Ldalvik/annotation/Signature;\n"
                 "value = {\"(I)I\"}\n.end annotation\n"
                 "return p0\n.end method\n",
         "unsupported source annotation visibility");
}

TEST(MobileDalvikReader, DexSharedEmptyAnnotationSetsKeepCompleteMethods) {
  FixtureOptions options;
  options.params = {"I", "J"};
  options.registers = 3;
  options.annotation_sets = {{}};
  options.class_annotations = 0;
  attachFixtureAnnotation(options, "field");
  options.method_annotations = 0;
  options.parameter_annotations =
      std::vector<std::optional<size_t>>{std::nullopt, 0};
  auto parsed = parse(fixture(options).data);
  ASSERT_EQ(parsed.size(), 1u);
  ASSERT_EQ(parsed[0].fields.size(), 1u);
  ASSERT_EQ(parsed[0].methods.size(), 1u);
  EXPECT_EQ(parsed[0].methods[0].reference.identity(),
            "Lfixture/Sample;->value(IJ)I");
  Budget budget;
  auto result = recoverJava(linkClasses(std::move(parsed), budget), budget);
  EXPECT_EQ(result.getInteger("method_count"), 1);
  EXPECT_EQ(result.getInteger("recovered_method_count"), 1);
  ASSERT_NE(result.getArray("source_units"), nullptr);
  EXPECT_EQ(result.getArray("source_units")->size(), 1u);

  FixtureOptions no_parameters;
  no_parameters.params.clear();
  no_parameters.words = {0x1012, 0x000f};
  no_parameters.parameter_annotations = std::vector<std::optional<size_t>>{};
  auto zero = parse(fixture(no_parameters).data);
  ASSERT_EQ(zero.size(), 1u);
  ASSERT_EQ(zero[0].methods.size(), 1u);
  EXPECT_EQ(zero[0].methods[0].reference.identity(),
            "Lfixture/Sample;->value()I");
}

TEST(MobileDalvikReader, DexParameterAnnotationsUseLogicalSlotsAndExactCounts) {
  FixtureOptions options;
  options.params = {"J", "I"};
  options.registers = 3;
  options.words = {0x020f};
  options.annotations = {{"Ljava/lang/Deprecated;", 1}};
  options.annotation_sets = {{0}};
  options.parameter_annotations =
      std::vector<std::optional<size_t>>{std::nullopt, 0};
  expectDexError(fixture(options).data,
                 "Invalid DEX: unsupported annotation Ljava/lang/Deprecated; "
                 "on parameter 1 of method Lfixture/Sample;->value(JI)I");
  options.parameter_annotations = std::vector<std::optional<size_t>>{0};
  expectDexError(fixture(options).data,
                 "Invalid DEX: parameter annotation count mismatch");
  options.parameter_annotations = std::vector<std::optional<size_t>>{0, 0, 0};
  expectDexError(fixture(options).data,
                 "Invalid DEX: parameter annotation count mismatch");
}

TEST(MobileDalvikReader, DexOrphanAnnotationsRetainNestedFormatChecks) {
  FixtureOptions options;
  options.annotations = {typeArrayAnnotation(),
                         nestedAnnotation("Lfixture/NestedAnnotation;")};
  // The second set reuses the class's item through the item cache. The
  // third set is unattached, so its nested annotation has no source policy.
  options.annotation_sets = {{0}, {0}, {1}};
  options.class_annotations = 0;
  for (unsigned visibility : {0u, 1u, 2u}) {
    options.annotations[1].visibility = visibility;
    auto parsed = parse(fixture(options).data);
    ASSERT_EQ(parsed.size(), 1u);
    ASSERT_EQ(parsed[0].methods.size(), 1u);
    EXPECT_EQ(parsed[0].methods[0].reference.identity(),
              "Lfixture/Sample;->value(I)I");
  }
  options.annotations[1] = nestedAnnotation("I");
  expectDexError(fixture(options).data,
                 "Invalid DEX: annotation type is not a class");
  options.annotations[1] = nestedAnnotation("Lfixture/NestedAnnotation;");
  options.annotations[1].visibility = 3;
  expectDexError(fixture(options).data,
                 "Invalid DEX: invalid annotation visibility");

  options.annotations = {typeArrayAnnotation(), typeArrayAnnotation()};
  options.annotation_sets = {{0, 1}};
  expectDexError(fixture(options).data,
                 "Invalid DEX: annotation types are duplicate or unordered");
}

TEST(MobileDalvikReader,
     DexEnclosingMethodRetainsTypedContextWithoutSourceClaim) {
  FixtureOptions options;
  options.owner = "Lfixture/Outer$1Nested;";
  options.flags = 1;
  options.registers = 2;
  options.words = {0x010f};
  options.referenced_method = MethodRef{"Lfixture/Outer;", "factory", {}, "V"};
  options.annotations = {enclosingMethodAnnotation(*options.referenced_method),
                         innerClassAnnotation(0)};
  options.annotation_sets = {{0, 1}};
  options.class_annotations = 0;
  auto parsed = parse(fixture(options).data);
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_EQ(parsed[0].name, options.owner);
  EXPECT_TRUE(parsed[0].inner_class_present);
  EXPECT_EQ(parsed[0].inner_name, "Nested");
  EXPECT_FALSE(parsed[0].enclosing);
  ASSERT_TRUE(parsed[0].enclosing_method);
  EXPECT_EQ(*parsed[0].enclosing_method, *options.referenced_method);
  ASSERT_EQ(parsed[0].methods.size(), 1u);
  EXPECT_EQ(parsed[0].methods[0].reference.identity(),
            "Lfixture/Outer$1Nested;->value(I)I");
  ASSERT_EQ(parsed[0].methods[0].instructions.size(), 1u);
  EXPECT_EQ(parsed[0].methods[0].instructions[0].opcode, "return");
  auto text =
      smali(".class public Lfixture/Outer$1Nested;\n.super Ljava/lang/Object;\n"
            ".annotation system Ldalvik/annotation/EnclosingMethod;\n"
            "value = Lfixture/Outer;->factory()V\n.end annotation\n"
            ".annotation system Ldalvik/annotation/InnerClass;\n"
            "name = \"Nested\"\naccessFlags = 0\n.end annotation\n"
            ".method public value(I)I\n.registers 2\nreturn p1\n.end method\n");
  EXPECT_EQ(parsed[0].enclosing_method, text.enclosing_method);
  EXPECT_EQ(parsed[0].inner_class_present, text.inner_class_present);
  EXPECT_EQ(parsed[0].inner_name, text.inner_name);
  EXPECT_EQ(parsed[0].inner_access, text.inner_access);
  EXPECT_EQ(parsed[0].methods[0].reference, text.methods[0].reference);
  EXPECT_EQ(parsed[0].methods[0].instructions[0].registers,
            text.methods[0].instructions[0].registers);
  // The old input is retained intact: its public class flags are outside
  // Java 8 local-class syntax. Parsing metadata is not a recovery claim.
  Budget budget;
  try {
    (void)linkClasses(std::move(parsed), budget);
    FAIL() << "Unsupported local source shape reached the emitter";
  } catch (const Error &error) {
    EXPECT_STREQ(error.what(),
                 "Android class Lfixture/Outer$1Nested; from classes2.dex "
                 "enclosing Lfixture/Outer;->factory()V: Java 8 local class "
                 "permits only final/synthetic access flags");
  }
}

TEST(MobileDalvikReader, DexEnclosingMethodRequiresSystemTypedMetadata) {
  FixtureOptions options;
  options.owner = "Lfixture/Outer$1Nested;";
  options.referenced_method =
      MethodRef{"Lfixture/Outer;", "factory", {"J"}, "I"};
  options.annotations = {enclosingMethodAnnotation(*options.referenced_method),
                         innerClassAnnotation(0)};
  options.annotation_sets = {{0, 1}};
  options.class_annotations = 0;
  for (unsigned visibility : {0u, 1u}) {
    auto changed = options;
    changed.annotations[0].visibility = visibility;
    expectDexError(fixture(changed).data,
                   "Invalid DEX: structural annotation "
                   "Ldalvik/annotation/EnclosingMethod; on class "
                   "Lfixture/Outer$1Nested; requires system visibility");
  }
  auto changed = options;
  changed.annotations[0].elements = [](std::string &out, const Fixture &f) {
    uleb(out, 1);
    uleb(out, fixtureStringIndex(f, "value"));
    annotationIndex(out, 0x18, fixtureTypeIndex(f, "Ljava/lang/Object;"));
  };
  expectDexError(fixture(changed).data,
                 "Invalid DEX: invalid EnclosingMethod annotation for "
                 "Lfixture/Outer$1Nested;");
  changed.annotations[0].elements = [](std::string &out, const Fixture &f) {
    uleb(out, 1);
    uleb(out, fixtureStringIndex(f, "value"));
    annotationIndex(out, 0x1a, UINT32_MAX);
  };
  expectDexError(fixture(changed).data,
                 "Invalid DEX: method index out of bounds");
  changed = options;
  changed.annotation_sets = {{0}};
  expectDexError(fixture(changed).data,
                 "Invalid DEX: incomplete inner class metadata");
  changed = options;
  changed.annotations.push_back(enclosingClassAnnotation());
  changed.annotation_sets = {{0, 1, 2}};
  expectDexError(fixture(changed).data,
                 "Invalid DEX: conflicting enclosing annotations for "
                 "Lfixture/Outer$1Nested;");
}

TEST(MobileDalvikReader, DexInnerClassAbsentAndNullAreDifferentModelStates) {
  auto ordinary = parse(fixture().data);
  ASSERT_EQ(ordinary.size(), 1u);
  EXPECT_FALSE(ordinary[0].inner_class_present);
  EXPECT_FALSE(ordinary[0].inner_name);
  EXPECT_FALSE(ordinary[0].enclosing_method);

  FixtureOptions options;
  options.owner = "Lfixture/Outer$1;";
  options.referenced_method = MethodRef{"Lfixture/Outer;", "factory", {}, "I"};
  auto inner = innerClassAnnotation(0);
  inner.elements = [](std::string &out, const Fixture &f) {
    uleb(out, 2);
    uleb(out, fixtureStringIndex(f, "accessFlags"));
    append(out, 0x04, 1);
    append(out, 0, 1);
    uleb(out, fixtureStringIndex(f, "name"));
    append(out, 0x1e, 1); // VALUE_NULL, not a missing annotation.
  };
  options.annotations = {enclosingMethodAnnotation(*options.referenced_method),
                         inner};
  options.annotation_sets = {{0, 1}};
  options.class_annotations = 0;
  auto anonymous = parse(fixture(options).data);
  ASSERT_EQ(anonymous.size(), 1u);
  EXPECT_TRUE(anonymous[0].inner_class_present);
  EXPECT_FALSE(anonymous[0].inner_name);
  EXPECT_EQ(anonymous[0].enclosing_method, options.referenced_method);
  Budget budget;
  try {
    (void)linkClasses(std::move(anonymous), budget);
    FAIL() << "Anonymous class was treated as an ordinary named class";
  } catch (const Error &error) {
    EXPECT_STREQ(error.what(),
                 "Android class Lfixture/Outer$1; from classes2.dex "
                 "enclosing Lfixture/Outer;->factory()I: anonymous class "
                 "source context is unsupported");
  }
}

TEST(MobileDalvikReader, DexRuntimeAnnotationFailureCannotPublishJava) {
  FixtureOptions options;
  options.annotations = {{"Lfixture/Unknown;", 1}};
  options.annotation_sets = {{0}};
  options.class_annotations = 0;
  const auto f = fixture(options);
  DexRecoveryDirectory temporary;
  auto input = temporary.path / "annotated.dex";
  auto output = temporary.path / "output";
  writeFile(input, f.data);
  ASSERT_TRUE(fs::create_directory(output));
  Options recovery;
  recovery.input = input;
  recovery.output = output;
  Budget budget;
  try {
    (void)recoverAndroid(recovery, output, budget);
    FAIL() << "Runtime annotation was silently dropped during recovery";
  } catch (const Error &error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("unresolved marker annotation definition"),
              std::string::npos);
    EXPECT_NE(message.find("Lfixture/Unknown;"), std::string::npos);
    EXPECT_NE(message.find("Lfixture/Sample;"), std::string::npos);
  }
  EXPECT_TRUE(fs::is_empty(output));
  EXPECT_EQ(readFile(input, 1024 * 1024), f.data);
}

TEST(MobileDalvikReader, SmaliStructuralAnnotationsPreserveNestedOwnership) {
  auto cls = smali(
      ".class public final Lfixture/Outer$Nested;\n.super "
      "Ljava/lang/Object;\n.annotation system "
      "Ldalvik/annotation/EnclosingClass;\nvalue = Lfixture/Outer;\n.end "
      "annotation\n.annotation system Ldalvik/annotation/InnerClass;\nname = "
      "\"Nested\"\naccessFlags = 0x19\n.end annotation\n");
  EXPECT_EQ(cls.enclosing, "Lfixture/Outer;");
  EXPECT_TRUE(cls.inner_class_present);
  EXPECT_FALSE(cls.enclosing_method);
  EXPECT_EQ(cls.inner_name, "Nested");
  EXPECT_TRUE(has(cls.inner_access, "static"));
  auto unknown =
      smali(".class public LBad;\n.super Ljava/lang/Object;\n.annotation "
            "runtime LUnknown;\n.end annotation\n");
  ASSERT_EQ(unknown.marker_annotations.size(), 1u);
  EXPECT_EQ(unknown.marker_annotations[0].type, "LUnknown;");
  Budget budget;
  EXPECT_THROW(linkClasses({unknown}, budget), Error);
}
TEST(MobileDalvikReader, SmaliLocalMetadataRetainsExactMethodAndPresence) {
  const MethodRef enclosing{"Lfixture/Outer;", "first", {"I"}, "I"};
  auto local = localClassModel("Lfixture/Outer$37Worker;", enclosing);
  EXPECT_TRUE(local.inner_class_present);
  EXPECT_EQ(local.inner_name, "Worker");
  EXPECT_FALSE(local.enclosing);
  ASSERT_TRUE(local.enclosing_method);
  EXPECT_EQ(*local.enclosing_method, enclosing);
  EXPECT_EQ(local.access, Access{"final"});
  EXPECT_EQ(local.inner_access, Access{"final"});
  ASSERT_EQ(local.methods.size(), 2u);
  EXPECT_EQ(local.methods[0].reference.name, "<init>");
  ASSERT_EQ(local.methods[0].instructions.size(), 5u);
  EXPECT_EQ(local.methods[0].instructions[3].opcode, "sput");
  EXPECT_EQ(local.methods[1].reference.signature(), "(I)I");

  auto ordinary = smali(methodText("return p0"));
  EXPECT_FALSE(ordinary.inner_class_present);
  EXPECT_FALSE(ordinary.inner_name);
  EXPECT_FALSE(ordinary.enclosing_method);
  auto anonymous =
      localClassModel("Lfixture/Outer$1;", enclosing, "anonymous.smali", true);
  EXPECT_TRUE(anonymous.inner_class_present);
  EXPECT_FALSE(anonymous.inner_name);
  EXPECT_EQ(anonymous.enclosing_method, local.enclosing_method);
  expectLocalScopeError({anonymous}, anonymous,
                        "anonymous class source context is unsupported");
}

TEST(MobileDalvikReader, SmaliEnclosingMethodRequiresOneTypedSystemContext) {
  const std::string header =
      ".class final Lfixture/Outer$1Worker;\n.super Ljava/lang/Object;\n";
  const std::string inner =
      ".annotation system Ldalvik/annotation/InnerClass;\n"
      "name = \"Worker\"\naccessFlags = 0x10\n.end annotation\n";
  const auto annotation = [](std::string_view value) {
    return ".annotation system Ldalvik/annotation/EnclosingMethod;\n"
           "value = " +
           std::string(value) + "\n.end annotation\n";
  };
  const auto rejected = [&](const std::string &text, std::string_view reason) {
    try {
      (void)smali(text);
      ADD_FAILURE() << "Expected structural scope rejection: " << reason;
    } catch (const Error &error) {
      EXPECT_NE(std::string_view(error.what()).find(reason),
                std::string_view::npos)
          << error.what();
    }
  };
  for (auto value :
       {"null", "Ljava/lang/Object;", "I->first(I)I", "Lfixture/Outer;->first"})
    rejected(header + annotation(value) + inner,
             "invalid EnclosingMethod reference for "
             "Lfixture/Outer$1Worker;");
  const auto valid = annotation("Lfixture/Outer;->first(I)I");
  rejected(header + valid, "incomplete inner class metadata");
  rejected(header + valid + valid + inner,
           "duplicate smali structural annotation");
  const std::string enclosing_class =
      ".annotation system Ldalvik/annotation/EnclosingClass;\n"
      "value = Lfixture/Outer;\n.end annotation\n";
  rejected(header + valid + enclosing_class + inner,
           "conflicting enclosing annotations for Lfixture/Outer$1Worker;");
  rejected(header + enclosing_class + valid + inner,
           "conflicting enclosing annotations for Lfixture/Outer$1Worker;");
  rejected(header +
               ".annotation runtime Ldalvik/annotation/EnclosingMethod;\n"
               "value = Lfixture/Outer;->first(I)I\n.end annotation\n" +
               inner,
           "unsupported smali annotation visibility");
}

TEST(MobileDalvikReader, NamedLocalScopesLinkExactMethodsAcrossInputFiles) {
  const MethodRef first{"Lfixture/Outer;", "first", {"I"}, "I"};
  const MethodRef second{"Lfixture/Outer;", "second", {"I"}, "I"};
  auto local =
      localClassModel("Lfixture/Outer$37Worker;", first, "first-local.smali");
  auto other =
      localClassModel("Lfixture/Outer$2Worker;", second, "second-local.smali");
  auto outer = localOwnerModel("first", local.name);
  outer.methods.push_back(localOwnerModel("second", other.name).methods[0]);
  Budget budget;
  // Local declarations deliberately precede their enclosing input. Numeric
  // suffixes have no role in resolving the exact typed method relationship.
  auto linked = linkClasses({local, other, outer}, budget);
  ASSERT_EQ(linked.size(), 3u);
  EXPECT_EQ(linked.at(local.name).source_id, "first-local.smali");
  EXPECT_EQ(linked.at(other.name).source_id, "second-local.smali");
  EXPECT_EQ(linked.at(outer.name).source_id, "outer.smali");
  EXPECT_EQ(linked.at(local.name).enclosing_method, first);
  EXPECT_EQ(linked.at(other.name).enclosing_method, second);
  size_t methods = 0;
  for (const auto &[name, cls] : linked)
    for (const auto &method : cls.methods) {
      ++methods;
      EXPECT_FALSE(method.instructions.empty()) << method.reference.identity();
    }
  EXPECT_EQ(methods, 6u);
  ASSERT_EQ(linked.at(outer.name).methods[0].instructions.size(), 5u);
  EXPECT_EQ(std::get<std::string>(
                linked.at(outer.name).methods[0].instructions[0].reference),
            local.name);
  EXPECT_EQ(std::get<MethodRef>(
                linked.at(outer.name).methods[1].instructions[2].reference)
                .owner,
            other.name);
}

TEST(MobileDalvikReader, LocalScopesRejectMissingOverloadedOrDuplicateOwners) {
  const MethodRef first{"Lfixture/Outer;", "first", {"I"}, "I"};
  auto local = localClassModel("Lfixture/Outer$1Worker;", first);
  auto outer = localOwnerModel("first", local.name);
  expectLocalScopeError({local}, local,
                        "exact enclosing method definition is unavailable");
  auto changed = local;
  changed.enclosing_method = MethodRef{first.owner, first.name, {"J"}, "J"};
  expectLocalScopeError({outer, changed}, changed,
                        "exact enclosing method definition is unavailable");
  auto instance = outer;
  instance.methods[0].access.erase("static");
  expectLocalScopeError({instance, local}, local,
                        "enclosing method must be ordinary static with a body");
  auto duplicate = outer;
  duplicate.methods.push_back(duplicate.methods[0]);
  expectLocalScopeError({duplicate, local}, local,
                        "exact enclosing method definition is duplicated",
                        true);
  auto namesake = localClassModel("Lfixture/Outer$99Worker;", first,
                                  "duplicate-local.smali");
  expectLocalScopeError({outer, local, namesake}, namesake,
                        "duplicate local source name in one enclosing method");
  changed = local;
  changed.enclosing_method->name = "<init>";
  expectLocalScopeError({outer, changed}, changed, "ordinary enclosing method");
}

TEST(MobileDalvikReader, SameNamedLocalClassesUseTheWholeEnclosingPrototype) {
  const MethodRef first{"Lfixture/Outer;", "first", {"I"}, "I"};
  const MethodRef overload{"Lfixture/Outer;", "first", {"I", "I"}, "I"};
  auto local = localClassModel("Lfixture/Outer$39Worker;", first);
  auto other = localClassModel("Lfixture/Outer$4Worker;", overload,
                               "overloaded-local.smali");
  auto outer = localOwnerModel("first", local.name);
  auto overloaded =
      smali(".class public Lfixture/Outer;\n.super Ljava/lang/Object;\n"
            ".method public static first(II)I\n.registers 3\n"
            "new-instance v0,Lfixture/Outer$4Worker;\n"
            "invoke-direct {v0},Lfixture/Outer$4Worker;-><init>()V\n"
            "invoke-virtual {v0,p1},Lfixture/Outer$4Worker;->apply(I)I\n"
            "move-result v0\nreturn v0\n.end method\n");
  outer.methods.push_back(overloaded.methods[0]);
  Budget budget;
  auto linked = linkClasses({other, outer, local}, budget);
  ASSERT_EQ(linked.size(), 3u);
  EXPECT_EQ(linked.at(local.name).enclosing_method, first);
  EXPECT_EQ(linked.at(other.name).enclosing_method, overload);
  ASSERT_EQ(linked.at(outer.name).methods.size(), 2u);
  EXPECT_EQ(std::get<MethodRef>(
                linked.at(outer.name).methods[1].instructions[2].reference)
                .owner,
            other.name);
}

TEST(MobileDalvikReader, LocalScopeShapeCannotAdmitJava8ModifiersOrCapture) {
  const MethodRef first{"Lfixture/Outer;", "first", {"I"}, "I"};
  auto local = localClassModel("Lfixture/Outer$1Worker;", first);
  auto outer = localOwnerModel("first", local.name);
  for (auto flag : {"public", "private", "protected", "static", "interface",
                    "annotation", "enum", "abstract"})
    for (bool inner : {false, true}) {
      SCOPED_TRACE(std::string(flag) + (inner ? " InnerClass" : " class"));
      auto changed = local;
      (inner ? changed.inner_access : changed.access).insert(flag);
      // Exercise the shared policy directly, as recoverJava(ClassMap) does;
      // unrelated generic interface/constructor errors cannot mask this gate.
      expectLocalScopeError({outer, changed}, changed,
                            "permits only final/synthetic access flags", true);
    }
  for (bool inner : {false, true}) {
    auto changed = local;
    (inner ? changed.inner_access : changed.access).erase("final");
    expectLocalScopeError({outer, changed}, changed,
                          "final flag disagrees with InnerClass metadata");
  }
  auto changed = local;
  changed.inner_class_present = false;
  expectLocalScopeError({outer, changed}, changed,
                        "requires an InnerClass annotation");
  changed = local;
  changed.fields.push_back({{local.name, "captured", "I"}, {"final"}, {}});
  expectLocalScopeError({outer, changed}, changed,
                        "fields or capture storage are unsupported");
  changed = local;
  changed.methods[0].reference.parameters = {"I"};
  expectLocalScopeError({outer, changed}, changed,
                        "captured constructor arguments are unsupported");
  changed = local;
  changed.methods.erase(changed.methods.begin());
  expectLocalScopeError({outer, changed}, changed,
                        "exactly one real no-argument constructor");
  changed = local;
  changed.methods[1].access.insert("static");
  expectLocalScopeError({outer, changed}, changed,
                        "methods require real instance bodies");
  changed = local;
  changed.methods[1].reference.owner = outer.name;
  expectLocalScopeError({outer, changed}, changed,
                        "local method owner disagrees with its declaration",
                        true);
  changed = local;
  changed.methods[1].reference.name = "<other>";
  expectLocalScopeError({outer, changed}, changed,
                        "invalid ordinary local method identity", true);
}

TEST(MobileDalvikReader, LocalScopeRejectsEveryTypedReferenceOutsideItsMethod) {
  const MethodRef first{"Lfixture/Outer;", "first", {"I"}, "I"};
  auto local = localClassModel("Lfixture/Outer$1Worker;", first);
  auto outer = localOwnerModel("first", local.name);
  auto wrong_scope = outer.methods[0];
  wrong_scope.reference.name = "other";
  outer.methods.push_back(wrong_scope);
  expectLocalScopeError({outer, local}, local,
                        "outside its exact method scope: "
                        "Lfixture/Outer;->other(I)I");
  outer.methods.pop_back();
  // The shared scope validator examines all reference components, independent
  // of the later opcode/register verifier. Each case isolates one component.
  std::vector<Reference> references{
      local.name,
      "[" + local.name,
      FieldRef{"Lfixture/Foreign;", "value", local.name},
      FieldRef{local.name, "value", "I"},
      MethodRef{"Lfixture/Foreign;", "consume", {local.name}, "V"},
      MethodRef{"Lfixture/Foreign;", "produce", {}, local.name},
      MethodRef{local.name, "apply", {"I"}, "I"}};
  for (const auto &reference : references) {
    auto changed = outer;
    auto method = wrong_scope;
    method.instructions.resize(1);
    method.instructions[0].reference = reference;
    changed.methods.push_back(std::move(method));
    expectLocalScopeError({changed, local}, local,
                          "outside its exact method scope", true);
  }
  auto changed = outer;
  changed.fields.push_back({{outer.name, "escaped", "[" + local.name}, {}, {}});
  expectLocalScopeError({changed, local}, local,
                        "unavailable in a declaration or handler");
  changed = outer;
  auto method = wrong_scope;
  method.reference.parameters = {local.name};
  changed.methods.push_back(method);
  expectLocalScopeError({changed, local}, local,
                        "unavailable in a declaration or handler", true);
  changed = outer;
  method = wrong_scope;
  method.reference.returns = local.name;
  changed.methods.push_back(method);
  expectLocalScopeError({changed, local}, local,
                        "unavailable in a declaration or handler", true);
  changed = outer;
  changed.methods[0].tries.push_back({0, 1, {{local.name, 0}}});
  expectLocalScopeError({changed, local}, local,
                        "unavailable in a declaration or handler", true);
  // A hand-built ClassMap cannot use a forged MethodRef.owner to grant a
  // foreign declaration access to a local class's lexical scope.
  auto foreign = outer;
  foreign.name = "Lfixture/Foreign;";
  foreign.fields.clear();
  foreign.methods.resize(1);
  foreign.methods[0].reference.owner = local.name;
  expectLocalScopeError({outer, foreign, local}, local,
                        "outside its exact method scope", true);
  foreign.methods[0].reference = first;
  expectLocalScopeError({outer, foreign, local}, local,
                        "outside its exact method scope", true);
}

TEST(MobileDalvikReader,
     LocalScopeRejectsNestedDescendantsAndHeaderReferences) {
  const MethodRef first{"Lfixture/Outer;", "first", {"I"}, "I"};
  auto local = localClassModel("Lfixture/Outer$1Worker;", first);
  auto outer = localOwnerModel("first", local.name);
  auto member = smali(".class public Lfixture/Outer$1Worker$Child;\n"
                      ".super Ljava/lang/Object;\n"
                      ".annotation system Ldalvik/annotation/EnclosingClass;\n"
                      "value = Lfixture/Outer$1Worker;\n.end annotation\n"
                      ".annotation system Ldalvik/annotation/InnerClass;\n"
                      "name = \"Child\"\naccessFlags = 9\n.end annotation\n");
  expectLocalScopeError({outer, member, local}, local,
                        "method-local nested descendants are unsupported");
  member.enclosing.reset();
  member.inner_class_present = false;
  member.inner_name.reset();
  member.inner_access.clear();
  member.superclass = local.name;
  // Isolate lexical header validation from the generic final-parent check.
  expectLocalScopeError({outer, member, local}, local,
                        "unavailable in a declaration or handler", true);
  member.superclass = "Ljava/lang/Object;";
  member.interfaces.push_back(local.name);
  expectLocalScopeError({outer, member, local}, local,
                        "unavailable in a declaration or handler", true);
}
TEST(MobileDalvikReader, SmaliWideInvokeArgumentsRequireAdjacentWords) {
  auto cls = smali(methodText(
      "invoke-static/range {p0 .. "
      "p3},Lfixture/Target;->run(JD)J\nmove-result-wide v0\nreturn-wide v0",
      "value(JD)J", 6));
  auto &ins = cls.methods[0].instructions[0];
  EXPECT_EQ(ins.registers, (std::vector<unsigned>{2, 3, 4, 5}));
  EXPECT_EQ(std::get<MethodRef>(ins.reference).signature(), "(JD)J");
  EXPECT_THROW(
      smali(methodText(
          "invoke-static {v0,v2},Lfixture/Target;->run(J)J\nreturn p0")),
      Error);
  EXPECT_THROW(smali(methodText(
                   "invoke-static {v0},Lfixture/Target;->run(J)J\nreturn p0")),
               Error);
}
TEST(MobileDalvikReader, SmaliMalformedFramesTargetsAndClosersFail) {
  for (auto code : {"return v4", "goto :missing", "const/4 v0,15\nreturn v0",
                    "move-wide v3,v0\nreturn p0", "invented v0\nreturn p0",
                    "const/high16 v0,1\nreturn v0",
                    "goto :data\n:data\n.array-data 1\n1\n.end array-data",
                    "const/4 v0,1\n:data\n.array-data 1\n1\n.end array-data"})
    EXPECT_THROW(smali(methodText(code)), Error);
  EXPECT_THROW(smali(".class public LBad;\n.super Ljava/lang/Object;\n.method "
                     "public static f()V\n.registers 0\nreturn-void\n"),
               Error);
  EXPECT_THROW(
      smali(".class public LBad;\n.super Ljava/lang/Object;\n.method public "
            "abstract f()V\n.registers 0\nreturn-void\n.end method\n"),
      Error);
}
TEST(MobileDalvikReader, SmaliDebugDirectivesDoNotConsumeCodePositions) {
  auto cls = smali(methodText(
      ".param p0,\"input\"\n.prologue\n.line 7\n.local v0,\"copy\":I\nmove "
      "v0,p0\n.end local v0\n.restart local v0\nreturn v0"));
  ASSERT_EQ(cls.methods[0].instructions.size(), 2u);
  EXPECT_EQ(cls.methods[0].instructions[1].pc, 1u);
}
TEST(MobileDalvikReader, SmaliResourceLimitsAndInvalidUTF8FailExplicitly) {
  auto text = methodText("return p0");
  Budget work;
  work.remaining = 1;
  EXPECT_THROW(parseSmali(text, "owned", work), Error);
  Limits limits;
  limits.max_bytes = 8;
  Budget bytes(limits);
  EXPECT_THROW(parseSmali(text, "owned", bytes), Error);
  auto broken = text;
  broken += char(0xff);
  EXPECT_THROW(smali(broken), Error);
}
TEST(MobileDalvikReader, SmaliIntegerNegativeZeroFitsUnsignedOperands) {
  auto cls = smali(".class public LZero;\n.super Ljava/lang/Object;\n"
                   ".field public static zero:C = -0\n"
                   ".method public static run()V\n.registers -0\n"
                   "return-void\n.end method\n");
  ASSERT_EQ(cls.fields.size(), 1u);
  EXPECT_EQ(std::get<int64_t>(cls.fields[0].value), 0);
  ASSERT_EQ(cls.methods.size(), 1u);
  EXPECT_EQ(cls.methods[0].registers, 0u);
}

// The fixture strings use hex-encoded UTF-8/WTF-8 so that JSON itself never
// normalizes or rejects isolated UTF-16 surrogate values recovered from DEX.
using llvm::json::Array;
using llvm::json::Object;
using llvm::json::Value;
std::string hexBytes(std::string_view s) {
  std::string out;
  for (unsigned char c : s) {
    out += "0123456789abcdef"[c >> 4];
    out += "0123456789abcdef"[c & 15];
  }
  return out;
}
Value modelJSON(const std::string &s) { return hexBytes(s); }
Value modelJSON(int64_t n) { return n; }
Value modelJSON(uint64_t n) { return n; }
Value modelJSON(unsigned n) { return n; }
Value modelJSON(bool b) { return b; }
Value modelJSON(std::monostate) { return nullptr; }
Value modelJSON(const FloatBits &v) {
  return Object{{"kind", hexBytes(v.wide ? "double-bits" : "float-bits")},
                {"bits", v.bits}};
}
template <class T> Value modelJSON(const std::optional<T> &o) {
  return o ? modelJSON(*o) : Value(nullptr);
}
template <class T> Value modelJSON(const std::vector<T> &v) {
  Array a;
  for (auto &x : v)
    a.push_back(modelJSON(x));
  return a;
}
Value modelJSON(const Access &v) {
  Array a;
  for (auto &x : v)
    a.push_back(modelJSON(x));
  return a;
}
Value modelJSON(const MethodRef &r) {
  return Object{{"owner", modelJSON(r.owner)},
                {"name", modelJSON(r.name)},
                {"parameters", modelJSON(r.parameters)},
                {"returns", modelJSON(r.returns)}};
}
Value modelJSON(const FieldRef &r) {
  return Object{{"owner", modelJSON(r.owner)},
                {"name", modelJSON(r.name)},
                {"type", modelJSON(r.type)}};
}
template <class... Ts> Value modelJSON(const std::variant<Ts...> &v) {
  return std::visit([](const auto &x) { return modelJSON(x); }, v);
}
Value modelJSON(const Instruction &i) {
  Array keys, targets, data;
  for (auto n : i.keys)
    keys.push_back(int64_t(n));
  for (auto n : i.targets)
    targets.push_back(n);
  for (auto n : i.data)
    data.push_back(n);
  return Object{{"pc", i.pc},
                {"opcode", modelJSON(i.opcode)},
                {"registers", modelJSON(i.registers)},
                {"literal", modelJSON(i.literal)},
                {"target", modelJSON(i.target)},
                {"reference", modelJSON(i.reference)},
                {"keys", std::move(keys)},
                {"targets", std::move(targets)},
                {"data", std::move(data)},
                {"element_width", i.element_width}};
}
Value modelJSON(const TryRegion &r) {
  Array handlers;
  for (auto &h : r.handlers)
    handlers.push_back(Array{modelJSON(h.type), Value(h.target)});
  return Object{
      {"start", r.start}, {"end", r.end}, {"handlers", std::move(handlers)}};
}
Value modelJSON(const Method &m) {
  Array code, tries;
  for (auto &i : m.instructions)
    code.push_back(modelJSON(i));
  for (auto &t : m.tries)
    tries.push_back(modelJSON(t));
  return Object{{"reference", modelJSON(m.reference)},
                {"access", modelJSON(m.access)},
                {"registers", m.registers},
                {"instructions", std::move(code)},
                {"tries", std::move(tries)},
                {"code_end", m.code_end}};
}
Value modelJSON(const Field &f) {
  return Object{{"reference", modelJSON(f.reference)},
                {"access", modelJSON(f.access)},
                {"value", modelJSON(f.value)}};
}
Value modelJSON(const Class &c) {
  // Version-1 persistent vectors retain their original complete field set.
  // New scope presence/MethodRef fields have dedicated exact assertions above.
  Array fields, methods;
  for (auto &f : c.fields)
    fields.push_back(modelJSON(f));
  for (auto &m : c.methods)
    methods.push_back(modelJSON(m));
  return Object{{"name", modelJSON(c.name)},
                {"superclass", modelJSON(c.superclass)},
                {"access", modelJSON(c.access)},
                {"source_id", modelJSON(c.source_id)},
                {"interfaces", modelJSON(c.interfaces)},
                {"fields", std::move(fields)},
                {"methods", std::move(methods)},
                {"enclosing", modelJSON(c.enclosing)},
                {"inner_name", modelJSON(c.inner_name)},
                {"inner_access", modelJSON(c.inner_access)}};
}

std::string unhexBytes(llvm::StringRef text) {
  if (text.size() % 2)
    throw Error("reader fixture has an odd hexadecimal length");
  auto digit = [](char c) -> unsigned {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    throw Error("reader fixture has an invalid hexadecimal digit");
  };
  std::string bytes;
  bytes.reserve(text.size() / 2);
  for (size_t i = 0; i < text.size(); i += 2)
    bytes += char((digit(text[i]) << 4) | digit(text[i + 1]));
  return bytes;
}

TEST(MobileDalvikReader, ClassDescriptorsRejectUnicodeWhitespace) {
  for (unsigned codepoint :
       {0x85u, 0xa0u, 0x1680u, 0x2000u, 0x2001u, 0x2002u, 0x2003u, 0x2004u,
        0x2005u, 0x2006u, 0x2007u, 0x2008u, 0x2009u, 0x200au, 0x2028u, 0x2029u,
        0x202fu, 0x205fu, 0x3000u}) {
    SCOPED_TRACE("Unicode whitespace " + std::to_string(codepoint));
    std::string space;
    if (codepoint < 0x800) {
      space += char(0xc0 | (codepoint >> 6));
      space += char(0x80 | (codepoint & 63));
    } else {
      space += char(0xe0 | (codepoint >> 12));
      space += char(0x80 | ((codepoint >> 6) & 63));
      space += char(0x80 | (codepoint & 63));
    }
    EXPECT_THROW(descriptor("Lpackage/A" + space + "B;"), Error);
    EXPECT_THROW(prototype("(Lpackage/A" + space + "B;)V"), Error);
  }
  EXPECT_EQ(descriptor("L包/類;"), "L包/類;");
  EXPECT_EQ(descriptor("Lfixture/\xed\xa0\x80;"), "Lfixture/\xed\xa0\x80;");
  for (const auto &bad : {"Lfixture/\xc0\xa0;", "Lfixture/\xe2\x82;",
                          "Lfixture/\xf4\x90\x80\x80;"})
    EXPECT_THROW(descriptor(bad), Error);
  EXPECT_EQ(descriptor("Lfixture/A\xe2\x80\x8b"
                       "B;"),
            "Lfixture/A\xe2\x80\x8b"
            "B;");
}

TEST(MobileDalvikReader, PersistentReaderVectorsMatchCompleteTypedModels) {
#ifdef NEVERD_MOBILE_FIXTURES
  const fs::path fixtures = pathFromUTF8(NEVERD_MOBILE_FIXTURES);
#else
  // Standalone developer builds may compile this source without CMake.
  const fs::path fixtures =
      fs::path(__FILE__).parent_path().parent_path().parent_path() /
      "scripts/tests/fixtures/mobile";
#endif
  auto document =
      parseJSON(readFile(fixtures / "native-readers/reader-vectors.json",
                         16 * 1024 * 1024),
                "native reader fixtures");
  auto *root = document.getAsObject();
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->getInteger("schema_version"), 1);
  ASSERT_EQ(root->getString("string_encoding"), "utf8-or-wtf8-hex");
  auto *cases = root->getArray("cases");
  ASSERT_NE(cases, nullptr);
  ASSERT_EQ(cases->size(), 156u);
  for (size_t index = 0; index < cases->size(); ++index) {
    SCOPED_TRACE("reader vector " + std::to_string(index));
    const auto *item = (*cases)[index].getAsObject();
    ASSERT_NE(item, nullptr);
    auto kind = item->getString("kind");
    auto input_id = item->getString("input_id");
    auto input_hex = item->getString("input_hex");
    auto max_bytes = item->getInteger("max_bytes");
    auto remaining = item->getInteger("remaining");
    auto expired = item->getBoolean("expired");
    auto rejected = item->getBoolean("rejected");
    ASSERT_TRUE(kind && (*kind == "dex" || *kind == "smali"));
    ASSERT_TRUE(input_id);
    ASSERT_TRUE(input_hex);
    ASSERT_TRUE(max_bytes && *max_bytes > 0);
    ASSERT_TRUE(remaining && *remaining >= 0);
    ASSERT_TRUE(expired);
    ASSERT_TRUE(rejected);
    const auto bytes = unhexBytes(*input_hex);
    Limits limits;
    limits.max_bytes = uint64_t(*max_bytes);
    Budget budget(limits);
    budget.remaining = uint64_t(*remaining);
    if (*expired)
      budget.deadline = std::chrono::steady_clock::time_point::min();
    auto parseModel = [&]() -> Value {
      if (*kind == "smali") {
        auto cls = parseSmali(bytes, input_id->str(), budget);
        // The unchanged unknown-marker negative vector is now rejected at
        // linking, after the reader retains its empty application record.
        if (*rejected && !cls.marker_annotations.empty())
          (void)linkClasses({cls}, budget);
        return modelJSON(cls);
      }
      Array classes;
      for (const auto &cls : parseDex(bytes, input_id->str(), budget))
        classes.push_back(modelJSON(cls));
      return Value(std::move(classes));
    };
    if (*rejected) {
      ASSERT_EQ(item->get("expected"), nullptr);
      EXPECT_THROW(parseModel(), Error);
    } else {
      const auto *expected = item->get("expected");
      ASSERT_NE(expected, nullptr);
      Value actual = parseModel();
      EXPECT_EQ(actual, *expected) << "actual model: " << jsonText(actual)
                                   << "expected model: " << jsonText(*expected);
    }
  }
}

TEST(MobileDalvikReader, DexCodeWordsReadUnalignedHostStorageInLittleEndian) {
  FixtureOptions options;
  options.params = {};
  options.returns = "J";
  options.registers = 2;
  options.words = {0x0018, 0x3210, 0x7654, 0xba98, 0xfedc, 0x0010};
  const auto f = fixture(options);
  const std::string padded = "x" + f.data;
  const auto unaligned = std::string_view(padded).substr(1);
  Budget aligned_budget, unaligned_budget;
  auto aligned = parseDex(f.data, "fixture.dex", aligned_budget);
  auto result = parseDex(unaligned, "fixture.dex", unaligned_budget);
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(modelJSON(result[0]), modelJSON(aligned[0]));
  EXPECT_EQ(unaligned_budget.remaining, aligned_budget.remaining);
  EXPECT_EQ(std::get<int64_t>(result[0].methods[0].instructions[0].literal),
            std::bit_cast<int64_t>(uint64_t(0xfedcba9876543210)));
  Budget query_budget;
  DexReferenceQuery query{DexReferenceKind::String, "absent", true, {}};
  const auto refs = findDexReferences(unaligned, query, query_budget);
  EXPECT_EQ(refs.scanned_code_item_count, 1u);
  EXPECT_TRUE(refs.code_scan_complete);
}

TEST(MobileDalvikReader, CompactFlowResultKindsPreserveRecoveryAndQueryErrors) {
  for (const std::string type :
       {"V", "I", "F", "J", "D", "Ljava/lang/Object;", "[I"}) {
    for (unsigned opcode : {0xau, 0xbu, 0xcu}) {
      SCOPED_TRACE(type + ":" + std::to_string(opcode));
      FixtureOptions options;
      options.params = {};
      options.returns = "V";
      options.registers = 2;
      options.referenced_method =
          MethodRef{"Lexternal/Target;", "run", {}, type};
      options.words = {0x0071, 0, 0, uint16_t(opcode), 0x000e};
      auto f = fixture(options);
      auto index = std::find(f.methods.begin(), f.methods.end(),
                             *options.referenced_method) -
                   f.methods.begin();
      patch(f.data, f.at["code"] + 18, index, 2);
      f.data = seal(std::move(f.data));
      const auto expected = type == "J" || type == "D" ? 0xbu
                            : type.starts_with('L') || type.starts_with('[')
                                ? 0xcu
                                : 0xau;
      if (type != "V" && opcode == expected) {
        EXPECT_NO_THROW(parse(f.data));
        EXPECT_TRUE(references(f, DexReferenceKind::String, "absent")
                        .code_scan_complete);
      } else {
        expectDexError(
            f.data,
            "Invalid DEX: move-result kind disagrees with invocation result");
        EXPECT_THROW(references(f, DexReferenceKind::String, "absent"), Error);
      }
    }
  }
  FixtureOptions none;
  none.params = {};
  none.returns = "V";
  none.words = {0x0000, 0x000a, 0x000e};
  const auto f = fixture(none);
  expectDexError(
      f.data,
      "Invalid DEX: move-result does not immediately follow an invocation");
  EXPECT_THROW(references(f, DexReferenceKind::String, "absent"), Error);
}

TEST(MobileDalvikReader, SharedDebugAlwaysChecksEachRegisterFrame) {
  FixtureOptions o;
  o.params = {};
  o.returns = "V";
  o.words = {0x000e};
  o.registers = 2;
  o.referenced_method = MethodRef{o.owner, "zsecond", {}, "V"};
  o.define_referenced_method = true;
  o.separate_referenced_code = true;
  o.referenced_registers = 2;
  o.debug_info = std::string(
      {char(0), char(0), char(3), char(1), char(0), char(0), char(0)});
  auto valid = fixture(o);
  ASSERT_NO_THROW(parse(valid.data));
  EXPECT_TRUE(
      references(valid, DexReferenceKind::String, "absent").code_scan_complete);
  o.referenced_registers = 1;
  auto invalid = fixture(o);
  EXPECT_THROW(parse(invalid.data), Error);
  EXPECT_THROW(references(invalid, DexReferenceKind::String, "absent"), Error);
}
TEST(MobileDalvikReader, SharedDebugAlwaysChecksEachCodeExtent) {
  FixtureOptions o;
  o.params = {};
  o.returns = "V";
  o.words = {0, 0, 0x000e};
  o.registers = 1;
  o.referenced_method = MethodRef{o.owner, "zsecond", {}, "V"};
  o.define_referenced_method = true;
  o.separate_referenced_code = true;
  o.referenced_words = o.words;
  o.debug_info = std::string({char(0), char(0), char(1), char(2), char(0)});
  auto valid = fixture(o);
  ASSERT_NO_THROW(parse(valid.data));
  EXPECT_TRUE(
      references(valid, DexReferenceKind::String, "absent").code_scan_complete);
  o.referenced_words = {0x000e};
  auto invalid = fixture(o);
  EXPECT_THROW(parse(invalid.data), Error);
  EXPECT_THROW(references(invalid, DexReferenceKind::String, "absent"), Error);
}
TEST(MobileDalvikReader, SharedDebugAlwaysChecksEachParameterCount) {
  FixtureOptions o;
  o.params = {"I"};
  o.returns = "V";
  o.words = {0x000e};
  o.registers = 1;
  o.referenced_method = MethodRef{o.owner, "zsecond", {"I"}, "V"};
  o.define_referenced_method = true;
  o.separate_referenced_code = true;
  o.debug_info = std::string({char(0), char(1), char(0), char(0)});
  auto valid = fixture(o);
  ASSERT_NO_THROW(parse(valid.data));
  EXPECT_TRUE(
      references(valid, DexReferenceKind::String, "absent").code_scan_complete);
  o.referenced_method->parameters.clear();
  auto invalid = fixture(o);
  EXPECT_THROW(parse(invalid.data), Error);
  EXPECT_THROW(references(invalid, DexReferenceKind::String, "absent"), Error);
}
using MemberGroups = std::array<std::vector<std::array<uint32_t, 3>>, 4>;
Fixture memberGroups(Fixture input, const MemberGroups &groups) {
  std::string raw;
  for (const auto &group : groups)
    uleb(raw, group.size());
  for (size_t group = 0; group < groups.size(); ++group) {
    uint32_t previous = 0;
    for (const auto &member : groups[group]) {
      uleb(raw, member[0] - previous);
      uleb(raw, member[1]);
      if (group >= 2)
        uleb(raw, member[2]);
      previous = member[0];
    }
  }
  auto &data = input.data;
  const auto map = data.substr(input.at.at("map"));
  data.resize(input.at.at("class_data"));
  data += raw;
  while (data.size() % 4)
    data += '\0';
  input.at["map"] = data.size();
  data += map;
  auto u32 = [&](size_t offset) {
    uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i)
      result |= uint32_t(uint8_t(data.at(offset + i))) << (8 * i);
    return result;
  };
  patch(data, 32, data.size());
  patch(data, 52, input.at["map"]);
  patch(data, 104, data.size() - u32(108));
  const auto count = u32(input.at["map"]);
  for (unsigned i = 0; i < count; ++i) {
    const auto at = input.at["map"] + 4 + 12 * i;
    if ((u32(at) & 0xffff) == 0x1000)
      patch(data, at + 8, input.at["map"]);
  }
  data = seal(std::move(data));
  return input;
}
Fixture memberGroupFixture(unsigned fields = 2, unsigned methods = 2) {
  FixtureOptions options;
  options.params = {};
  options.returns = "V";
  options.flags = 0x109;
  for (unsigned i = 0; i < fields; ++i)
    options.extra_fields.push_back(
        {options.owner, "field" + std::to_string(i), "I"});
  for (unsigned i = 0; i < methods; ++i)
    options.extra_methods.push_back(
        {options.owner, "method" + std::to_string(i), {}, "V"});
  return fixture(options);
}
TEST(MobileDalvikReader, ClassDataGroupDuplicatesPreserveDiagnostics) {
  const auto base = memberGroupFixture();
  for (unsigned kind = 0; kind < 2; ++kind) {
    MemberGroups groups;
    const unsigned first = kind ? 2 : 0, second = first + 1;
    groups[first].push_back({0, kind ? 0x109u : 9u, 0});
    groups[second].push_back({0, kind ? 0x101u : 1u, 0});
    auto bad = memberGroups(base, groups);
    expectDexError(bad.data, "Invalid DEX: duplicate defined member");
    EXPECT_THROW(references(bad, DexReferenceKind::String, "absent"), Error);
    groups[second].clear();
    groups[first].push_back(groups[first].front());
    bad = memberGroups(base, groups);
    expectDexError(bad.data, "Invalid DEX: duplicate class-data member index");
    EXPECT_THROW(references(bad, DexReferenceKind::String, "absent"), Error);
  }
}
TEST(MobileDalvikReader, ClassDataEmptyGroupsPreserveDeclarations) {
  const auto base = memberGroupFixture();
  for (unsigned mask = 0; mask < 16; ++mask) {
    SCOPED_TRACE(mask);
    MemberGroups groups;
    for (unsigned g = 0; g < 4; ++g)
      if (mask & (1u << g))
        groups[g].push_back(
            {g & 1, g < 2 ? (g == 0 ? 9u : 1u) : (g == 2 ? 0x109u : 0x101u),
             0});
    const auto input = memberGroups(base, groups);
    const auto cls = parse(input.data).front();
    EXPECT_EQ(cls.fields.size(), bool(mask & 1) + bool(mask & 2));
    EXPECT_EQ(cls.methods.size(), bool(mask & 4) + bool(mask & 8));
    const auto result = references(input, DexReferenceKind::String, "absent");
    EXPECT_TRUE(result.code_scan_complete);
    EXPECT_EQ(result.defined_method_count, cls.methods.size());
    EXPECT_EQ(result.scanned_method_count, 0u);
  }
}
TEST(MobileDalvikReader, ClassDataLargeInterleavedGroupsStayBounded) {
  const auto base = memberGroupFixture(1024, 4096);
  MemberGroups groups;
  for (uint32_t index = 0; index < base.fields.size(); ++index)
    groups[index & 1].push_back({index, (index & 1) ? 1u : 9u, 0});
  for (uint32_t index = 0; index < base.methods.size(); ++index)
    groups[2 + (index & 1)].push_back(
        {index, (index & 1) ? 0x101u : 0x109u, 0});
  const auto input = memberGroups(base, groups);
  const auto cls = parse(input.data).front();
  EXPECT_EQ(cls.fields.size(), base.fields.size());
  EXPECT_EQ(cls.methods.size(), base.methods.size());
  const auto result = references(input, DexReferenceKind::String, "absent");
  EXPECT_TRUE(result.code_scan_complete);
  EXPECT_EQ(result.defined_method_count, base.methods.size());
  groups[3].insert(groups[3].begin(), {0, 0x101, 0});
  const auto duplicate = memberGroups(base, groups);
  expectDexError(duplicate.data, "Invalid DEX: duplicate defined member");
  EXPECT_THROW(references(duplicate, DexReferenceKind::String, "absent"),
               Error);
  Budget constrained;
  constrained.remaining = 100;
  EXPECT_THROW(findDexReferences(input.data,
                                 {DexReferenceKind::String, "absent"},
                                 constrained),
               Error);
}
TEST(MobileDalvikReader,
     BorrowedPoolModelsOwnMemberTextAfterParserDestruction) {
  const std::string owner = "Lfixture/" + std::string(80, 'O') + ";";
  const std::string parameter = "Lfixture/" + std::string(80, 'P') + ";";
  const MethodRef callee{owner, std::string(60, 'm'), {"J", parameter}, "V"};
  const FieldRef field{owner, "VALUE", parameter};
  std::vector<Class> model;
  DexReferenceResult sites;
  {
    FixtureOptions options;
    options.owner = owner;
    options.method_name = std::string(60, 'd');
    options.params = {parameter};
    options.returns = "V";
    options.registers = 3;
    options.static_value = std::string(1, char(0x1e));
    options.field_type = parameter;
    options.referenced_method = callee;
    options.words = {0x0062, 0, 0x3071, 0, 0x0210, 0x000e};
    for (unsigned i = 0; i < 200; ++i) {
      const auto type = "Lextra/T" + std::to_string(i) + ";";
      options.extra_methods.push_back({type, "extra", {type}, type});
      options.extra_fields.push_back({type, "extra", type});
    }
    auto f = fixture(options);
    auto member_index = std::find(f.methods.begin(), f.methods.end(), callee) -
                        f.methods.begin();
    auto field_index =
        std::find(f.fields.begin(), f.fields.end(), field) - f.fields.begin();
    patch(f.data, f.at["code"] + 18, field_index, 2);
    patch(f.data, f.at["code"] + 22, member_index, 2);
    f.data = seal(std::move(f.data));
    model = parse(f.data);
    sites = references(f, DexReferenceKind::Method, callee.identity(), true);
    std::fill(f.data.begin(), f.data.end(), char(0xdd));
  }
  // Both Dex instances, their source tables and the input storage are gone.
  std::vector<std::string> churn(4096, std::string(160, 'z'));
  ASSERT_EQ(model.size(), 1u);
  ASSERT_EQ(model[0].fields.size(), 1u);
  EXPECT_EQ(model[0].fields[0].reference, field);
  ASSERT_EQ(model[0].methods.size(), 1u);
  const MethodRef caller{owner, std::string(60, 'd'), {parameter}, "V"};
  EXPECT_EQ(model[0].methods[0].reference, caller);
  const auto &ins = model[0].methods[0].instructions;
  ASSERT_EQ(ins.size(), 3u);
  EXPECT_EQ(std::get<FieldRef>(ins[0].reference), field);
  EXPECT_EQ(std::get<MethodRef>(ins[1].reference), callee);
  ASSERT_EQ(sites.references.size(), 1u);
  EXPECT_EQ(sites.references[0].method, caller);
  EXPECT_EQ(sites.references[0].target, callee.identity());
  EXPECT_EQ(sites.references[0].pc_code_units, 2u);
}

TEST(MobileDalvikReader, BorrowedPoolMaterializedReferenceCopiesStayBounded) {
  FixtureOptions options;
  options.params = {};
  options.returns = "V";
  const MethodRef target{"L" + std::string(4096, 'x') + ";", "many", {}, "V"};
  options.referenced_method = target;
  options.words.clear();
  for (unsigned i = 0; i < 64; ++i)
    options.words.insert(options.words.end(), {0x0071, 0, 0});
  options.words.push_back(0x000e);
  auto f = fixture(options);
  const auto index =
      std::find(f.methods.begin(), f.methods.end(), target) - f.methods.begin();
  for (unsigned i = 0; i < 64; ++i)
    patch(f.data, f.at["code"] + 18 + i * 6, index, 2);
  f.data = seal(std::move(f.data));
  const DexReferenceQuery query{DexReferenceKind::Method, target.identity(),
                                true};
  Budget normal;
  const auto result = findDexReferences(f.data, query, normal);
  ASSERT_EQ(result.references.size(), 64u);
  EXPECT_TRUE(result.code_scan_complete);
  for (size_t i = 0; i < result.references.size(); ++i) {
    EXPECT_EQ(result.references[i].target, target.identity());
    EXPECT_EQ(result.references[i].pc_code_units, i * 3);
  }
  Limits limits;
  limits.max_bytes = 128 * 1024;
  ASSERT_LT(f.data.size(), limits.max_bytes);
  Budget storage(limits);
  try {
    (void)findDexReferences(f.data, query, storage);
    FAIL() << "actual result strings escaped the byte bound";
  } catch (const Error &error) {
    EXPECT_STREQ(error.what(), "reference query storage exceeds byte limit");
  }
  Budget work;
  work.remaining = 20000;
  EXPECT_THROW(findDexReferences(f.data, query, work), Error);
}
void expectQueryStorageLimit(const Fixture &input, uint64_t low,
                             uint64_t high) {
  Limits limits;
  limits.max_bytes = low;
  Budget small(limits);
  try {
    (void)findDexReferences(input.data, {DexReferenceKind::String, "absent"},
                            small);
    FAIL() << "query accepted insufficient transient storage";
  } catch (const Error &error) {
    EXPECT_EQ(std::string(error.what()),
              "reference query storage exceeds byte limit");
  }
  limits.max_bytes = high;
  Budget adequate(limits);
  const auto result = findDexReferences(
      input.data, {DexReferenceKind::String, "absent"}, adequate);
  EXPECT_TRUE(result.code_scan_complete);
  EXPECT_TRUE(result.references.empty());
}
TEST(MobileDalvikReader,
     QueryStorageChargesBranchFlowCapacityBeforeAllocation) {
  FixtureOptions options;
  options.params = {};
  options.returns = "V";
  options.words = {0x0012};
  for (unsigned i = 0; i < 10000; ++i)
    options.words.insert(options.words.end(), {0x0038, 2});
  options.words.push_back(0x000e);
  const auto input = fixture(options);
  EXPECT_EQ(parse(input.data).front().methods.front().instructions.size(),
            10002u);
  expectQueryStorageLimit(input, 600000, 2000000);
}
TEST(MobileDalvikReader,
     QueryStorageChargesSharedSwitchTargetsAndPendingEdges) {
  FixtureOptions options;
  options.params = {};
  options.returns = "V";
  options.words.clear();
  constexpr unsigned switches = 1024, targets = 128,
                     payload_pc = switches * 3 + 2;
  for (unsigned i = 0; i < switches; ++i) {
    const uint32_t delta = payload_pc - i * 3;
    options.words.insert(options.words.end(),
                         {0x002b, uint16_t(delta), uint16_t(delta >> 16)});
  }
  options.words.insert(options.words.end(), {0x000e, 0, 0x0100, targets, 0, 0});
  for (unsigned i = 0; i < targets; ++i)
    options.words.insert(options.words.end(), {3, 0});
  const auto input = fixture(options);
  EXPECT_EQ(parse(input.data)
                .front()
                .methods.front()
                .instructions.front()
                .targets.size(),
            targets);
  expectQueryStorageLimit(input, 1000000, 8000000);
}
TEST(MobileDalvikReader, QueryStorageChargesTryRegionsAndSharedHandlerCopies) {
  FixtureOptions options;
  options.params = {};
  options.returns = "V";
  constexpr unsigned count = 4096;
  options.words.assign(count + 1, 0x000e);
  for (unsigned i = 0; i < count; ++i)
    options.tries.push_back({i, 1, 1});
  options.handlers = std::string("\1\0", 2);
  uleb(options.handlers, count);
  const auto input = fixture(options);
  EXPECT_EQ(parse(input.data).front().methods.front().tries.size(), count);
  expectQueryStorageLimit(input, 650000, 2000000);
}

} // namespace
