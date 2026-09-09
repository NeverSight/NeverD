//===- MobileDalvikReaderTests.cpp - Independent native reader fixtures
//----===//
#include "MobileDalvik.h"
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
  std::vector<std::u16string> extras;
  std::optional<std::string> static_value;
  std::string field_type = "I";
  std::optional<MethodRef> referenced_method;
  std::string owner = "Lfixture/Sample;";
  std::vector<FixtureAnnotation> annotations;
  std::vector<std::vector<size_t>> annotation_sets;
  std::optional<size_t> class_annotations, field_annotations,
      method_annotations;
  std::optional<std::vector<std::optional<size_t>>> parameter_annotations;
};
struct Fixture {
  std::string data;
  std::map<std::string, size_t> at;
  std::vector<std::u16string> strings;
  std::vector<std::string> types;
  std::vector<MethodRef> methods;
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
  MethodRef definition{owner, "value", options.params, options.returns};
  std::vector<MethodRef> method_refs{definition};
  if (options.referenced_method)
    method_refs.push_back(*options.referenced_method);
  std::set<std::u16string> names{utf16(parent)};
  std::set<std::string> type_set{parent};
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
  }
  if (options.static_value) {
    names.insert(u"VALUE");
    names.insert(utf16(options.field_type));
    type_set.insert(options.field_type);
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
  at["protos"] =
      section(3, protos.size(), std::string(12 * protos.size(), '\0'));
  if (options.static_value) {
    raw.clear();
    append(raw, typeIndex(owner), 2);
    append(raw, typeIndex(options.field_type), 2);
    append(raw, stringIndex("VALUE"), 4);
    at["fields"] = section(4, 1, raw);
  }
  raw.clear();
  for (const auto &ref : method_refs) {
    append(raw, typeIndex(ref.owner), 2);
    append(raw, protoIndex(ref), 2);
    append(raw, stringIndex(ref.name), 4);
  }
  at["methods"] = section(5, method_refs.size(), raw);
  at["defined_method"] = at["methods"] + definition_index * 8;
  at["class"] = section(6, 1, std::string(32, '\0'));
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
  if (!parameter_offsets.empty())
    sections.push_back({0x1001, first_parameters, parameter_offsets.size()});
  bool no_code = options.flags & (0x100 | 0x400);
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
    code = section(0x2001, 1, raw);
  }
  at["code"] = code;
  bool direct = options.flags & (8 | 2 | 0x10000);
  raw.clear();
  uleb(raw, bool(options.static_value));
  uleb(raw, 0);
  uleb(raw, direct);
  uleb(raw, !direct);
  if (options.static_value) {
    uleb(raw, 0);
    uleb(raw, 0x19);
  }
  uleb(raw, definition_index);
  uleb(raw, options.flags);
  uleb(raw, code);
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
    align();
    at["annotations"] = out.size();
    append(out, annotationOffset(options.class_annotations), 4);
    append(out, bool(options.field_annotations), 4);
    append(out, bool(options.method_annotations), 4);
    append(out, bool(options.parameter_annotations), 4);
    if (options.field_annotations) {
      append(out, 0, 4);
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
           {4, size_t(bool(options.static_value)), at["fields"]},
           {5, method_refs.size(), at["methods"]},
           {6, 1, at["class"]}}) {
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
  std::array<uint32_t, 8> cls{
      typeIndex(owner), 1, typeIndex(parent), 0, UINT32_MAX,
      uint32_t(at["annotations"]), uint32_t(at["class_data"]),
      uint32_t(at["values"])};
  for (size_t i = 0; i < cls.size(); ++i)
    patch(out, at["class"] + i * 4, cls[i]);
  out = seal(std::move(out));
  return fixture;
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
void annotationIndex(std::string &out, unsigned kind, unsigned index) {
  // A four-byte unsigned pool index, encoded with value_arg == 3.
  append(out, kind | 0x60, 1);
  append(out, index, 4);
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
FixtureAnnotation typeArrayAnnotation(
    std::string type = "Ldalvik/annotation/MemberClasses;",
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
      ".class final " + owner + "\n.super Ljava/lang/Object;\n"
      ".annotation system Ldalvik/annotation/EnclosingMethod;\nvalue = " +
      enclosing.identity() + "\n.end annotation\n"
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
TEST(MobileDalvikReader, DexAttachedUnknownAnnotationsCannotLoseSemantics) {
  for (const auto &type : {"Lfixture/Unknown;", "Ljava/lang/Deprecated;"}) {
    for (unsigned visibility : {0u, 1u, 2u}) {
      for (const auto &attachment : {"class", "field", "method", "parameter"}) {
        SCOPED_TRACE(std::string(type) + " " + attachment + " visibility " +
                     std::to_string(visibility));
        FixtureOptions options;
        options.annotations = {{type, visibility}};
        options.annotation_sets = {{0}};
        attachFixtureAnnotation(options, attachment);
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
      expectDexError(
          fixture(changed).data,
          "Invalid DEX: structural annotation " + type + " on class " +
              options.owner + " requires system visibility");
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
    expectDexError(
        fixture(options).data,
        "Invalid DEX: unsupported annotation "
        "Ldalvik/annotation/MemberClasses; "
        "on method Lfixture/Sample;->value(I)I");
  }

  options.annotations = {typeArrayAnnotation(
      "Ldalvik/annotation/Throws;", {"Ljava/io/IOException;"})};
  options.class_annotations.reset();
  expectDexError(
      fixture(options).data,
      "Invalid DEX: unsupported annotation Ldalvik/annotation/Throws; "
      "on method Lfixture/Sample;->value(I)I");
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
  no_parameters.parameter_annotations =
      std::vector<std::optional<size_t>>{};
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
  expectDexError(
      fixture(options).data,
      "Invalid DEX: unsupported annotation Ljava/lang/Deprecated; "
      "on parameter 1 of method Lfixture/Sample;->value(JI)I");
  options.parameter_annotations = std::vector<std::optional<size_t>>{0};
  expectDexError(fixture(options).data,
                 "Invalid DEX: parameter annotation count mismatch");
  options.parameter_annotations =
      std::vector<std::optional<size_t>>{0, 0, 0};
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

TEST(MobileDalvikReader, DexEnclosingMethodRetainsTypedContextWithoutSourceClaim) {
  FixtureOptions options;
  options.owner = "Lfixture/Outer$1Nested;";
  options.flags = 1;
  options.registers = 2;
  options.words = {0x010f};
  options.referenced_method =
      MethodRef{"Lfixture/Outer;", "factory", {}, "V"};
  options.annotations = {
      enclosingMethodAnnotation(*options.referenced_method),
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
  auto text = smali(
      ".class public Lfixture/Outer$1Nested;\n.super Ljava/lang/Object;\n"
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
  options.annotations = {
      enclosingMethodAnnotation(*options.referenced_method),
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
  options.annotations = {
      enclosingMethodAnnotation(*options.referenced_method), inner};
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
  options.annotations = {{"Ljava/lang/Deprecated;", 1}};
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
    EXPECT_STREQ(error.what(),
                 "Invalid DEX: unsupported annotation Ljava/lang/Deprecated; "
                 "on class Lfixture/Sample;");
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
  EXPECT_THROW(
      smali(".class public LBad;\n.super Ljava/lang/Object;\n.annotation "
            "runtime LUnknown;\n.end annotation\n"),
      Error);
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
  auto anonymous = localClassModel("Lfixture/Outer$1;", enclosing,
                                   "anonymous.smali", true);
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
           "value = " + std::string(value) + "\n.end annotation\n";
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
  for (auto value : {"null", "Ljava/lang/Object;", "I->first(I)I",
                      "Lfixture/Outer;->first"})
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
  auto local = localClassModel("Lfixture/Outer$37Worker;", first,
                               "first-local.smali");
  auto other = localClassModel("Lfixture/Outer$2Worker;", second,
                               "second-local.smali");
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
  EXPECT_EQ(std::get<MethodRef>(linked.at(outer.name)
                                   .methods[1]
                                   .instructions[2]
                                   .reference)
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
                        "exact enclosing method definition is duplicated", true);
  auto namesake = localClassModel("Lfixture/Outer$99Worker;", first,
                                  "duplicate-local.smali");
  expectLocalScopeError({outer, local, namesake}, namesake,
                        "duplicate local source name in one enclosing method");
  changed = local;
  changed.enclosing_method->name = "<init>";
  expectLocalScopeError({outer, changed}, changed,
                        "ordinary enclosing method");
}

TEST(MobileDalvikReader, SameNamedLocalClassesUseTheWholeEnclosingPrototype) {
  const MethodRef first{"Lfixture/Outer;", "first", {"I"}, "I"};
  const MethodRef overload{"Lfixture/Outer;", "first", {"I", "I"}, "I"};
  auto local = localClassModel("Lfixture/Outer$39Worker;", first);
  auto other = localClassModel("Lfixture/Outer$4Worker;", overload,
                               "overloaded-local.smali");
  auto outer = localOwnerModel("first", local.name);
  auto overloaded = smali(
      ".class public Lfixture/Outer;\n.super Ljava/lang/Object;\n"
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
  EXPECT_EQ(std::get<MethodRef>(linked.at(outer.name)
                                   .methods[1]
                                   .instructions[2]
                                   .reference)
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
      local.name, "[" + local.name,
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

TEST(MobileDalvikReader, LocalScopeRejectsNestedDescendantsAndHeaderReferences) {
  const MethodRef first{"Lfixture/Outer;", "first", {"I"}, "I"};
  auto local = localClassModel("Lfixture/Outer$1Worker;", first);
  auto outer = localOwnerModel("first", local.name);
  auto member = smali(
      ".class public Lfixture/Outer$1Worker$Child;\n"
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
      if (*kind == "smali")
        return modelJSON(parseSmali(bytes, input_id->str(), budget));
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

} // namespace
