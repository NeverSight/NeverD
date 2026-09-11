#include "MobileIOSInternal.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <cctype>
#include <new>
#include <regex>
#include <sstream>

namespace neverd::mobile::ios {
namespace {
const std::set<std::string> keywords = {
    "auto",       "break",     "case",           "char",
    "const",      "continue",  "default",        "do",
    "double",     "else",      "enum",           "extern",
    "float",      "for",       "goto",           "if",
    "inline",     "int",       "long",           "register",
    "restrict",   "return",    "short",          "signed",
    "sizeof",     "static",    "struct",         "switch",
    "typedef",    "union",     "unsigned",       "void",
    "volatile",   "while",     "_Alignas",       "_Alignof",
    "_Atomic",    "_Bool",     "_Complex",       "_Generic",
    "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local",
    "self",       "_cmd",      "super",          "id",
    "Class",      "SEL",       "BOOL",           "nil",
    "YES",        "NO"};
const std::set<std::string> foundation = {
#define NEVERD_FOUNDATION_COMMON_CLASS(Name) #Name,
#define NEVERD_FOUNDATION_PLATFORM_CLASS(Name)
#include "MobileFoundationClasses.inc"
#undef NEVERD_FOUNDATION_COMMON_CLASS
#undef NEVERD_FOUNDATION_PLATFORM_CLASS
};
const std::set<std::string> importedFoundation = {
#define NEVERD_FOUNDATION_COMMON_CLASS(Name) #Name,
#define NEVERD_FOUNDATION_PLATFORM_CLASS(Name) #Name,
#include "MobileFoundationClasses.inc"
#undef NEVERD_FOUNDATION_COMMON_CLASS
#undef NEVERD_FOUNDATION_PLATFORM_CLASS
};
// These imported scalar and geometry typedefs occupy the same identifier
// namespace as Objective-C classes. Even a forward @class would conflict.
const std::set<std::string> importedValueTypes = {
    "NSInteger",    "NSUInteger",     "NSTimeInterval", "NSComparisonResult",
    "NSRange",      "NSRangePointer", "NSPoint",        "NSPointPointer",
    "NSPointArray", "NSSize",         "NSSizePointer",  "NSSizeArray",
    "NSRect",       "NSRectPointer",  "NSRectArray",    "NSZone",
    "CGFloat",      "unichar",        "int8_t",         "uint8_t",
    "int16_t",      "uint16_t",       "int32_t",        "uint32_t",
    "int64_t",      "uint64_t",       "intptr_t",       "uintptr_t",
    "size_t",       "ptrdiff_t"};
bool safe(const std::string &s) { return identifier(s) && !keywords.count(s); }
const std::set<std::string> qualifiers = {"const", "volatile", "restrict",
                                          "__restrict", "__restrict__"};
const std::set<std::string> storage = {"static", "inline", "extern",
                                       "_Noreturn"};
struct Token {
  std::string text;
  size_t start, end;
  unsigned kind = 0;
};
std::vector<Token> tokens(std::string_view s) {
  std::vector<Token> result;
  size_t p = 0;
  while (p < s.size()) {
    auto start = p;
    char c = s[p];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++p;
      continue;
    }
    if (s.substr(p).starts_with("//")) {
      for (;;) {
        auto end = s.find('\n', p);
        if (end == s.npos) {
          p = s.size();
          break;
        }
        auto q = end;
        if (q && s[q - 1] == '\r')
          --q;
        bool continued = q && s[q - 1] == '\\';
        p = end + 1;
        if (!continued)
          break;
      }
      continue;
    }
    if (s.substr(p).starts_with("/*")) {
      auto end = s.find("*/", p + 2);
      if (end == s.npos)
        throw Error("unterminated C comment");
      p = end + 2;
      continue;
    }
    size_t prefix = 0;
    for (auto pre : {"u8", "u", "U", "L"}) {
      auto n = std::char_traits<char>::length(pre);
      if (s.substr(p).starts_with(pre) && p + n < s.size() &&
          (s[p + n] == '\'' || s[p + n] == '"')) {
        prefix = n;
        break;
      }
    }
    if (c == '\'' || c == '"' || prefix) {
      p += prefix;
      char quote = s[p++];
      bool closed = false;
      while (p < s.size()) {
        if (s[p] == '\\') {
          p += 2;
        } else if (s[p] == quote) {
          ++p;
          closed = true;
          break;
        } else if (s[p] == '\r' || s[p] == '\n')
          throw Error("unescaped newline in C literal");
        else
          ++p;
      }
      if (!closed || p > s.size())
        throw Error("unterminated C literal");
      result.push_back({std::string(s.substr(start, p - start)), start, p, 1});
      continue;
    }
    if (c == '#') {
      auto line = s.rfind('\n', p);
      line = line == s.npos ? 0 : line + 1;
      if (!llvm::StringRef(s.data() + line, p - line).trim().empty())
        throw Error("preprocessor marker outside a directive");
      for (;;) {
        auto end = s.find('\n', p);
        if (end == s.npos) {
          p = s.size();
          break;
        }
        auto q = end;
        if (q && s[q - 1] == '\r')
          --q;
        p = end + 1;
        if (!q || s[q - 1] != '\\')
          break;
      }
      result.push_back({std::string(s.substr(start, p - start)), start, p, 2});
      continue;
    }
    if (identifier(std::string_view(&c, 1))) {
      ++p;
      while (p < s.size() && ((std::isalnum(static_cast<unsigned char>(s[p])) &&
                               static_cast<unsigned char>(s[p]) < 128) ||
                              s[p] == '_'))
        ++p;
    } else if (s.substr(p).starts_with("->"))
      p += 2;
    else
      ++p;
    result.push_back({std::string(s.substr(start, p - start)), start, p, 0});
  }
  return result;
}
std::vector<size_t> pairs(const std::vector<Token> &t) {
  std::vector<size_t> result(t.size(), t.size()), stack;
  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i].kind)
      continue;
    auto v = t[i].text;
    if (v == "(" || v == "[" || v == "{")
      stack.push_back(i);
    else if (v == ")" || v == "]" || v == "}") {
      std::string open = v == ")" ? "(" : v == "]" ? "[" : "{";
      if (stack.empty() || t[stack.back()].text != open)
        throw Error("unbalanced C delimiters");
      auto q = stack.back();
      stack.pop_back();
      result[i] = q;
      result[q] = i;
    }
  }
  if (!stack.empty())
    throw Error("unbalanced C delimiters");
  return result;
}
struct Definition {
  std::string name;
  size_t start, name_index, po, pc, bo, bc;
};
std::vector<Definition> definitions(const std::vector<Token> &t,
                                    const std::vector<size_t> &m) {
  std::vector<Definition> out;
  size_t start = 0;
  for (size_t i = 0; i < t.size(); ++i) {
    auto v = t[i].text;
    if (t[i].kind == 2) {
      auto text = llvm::StringRef(v).drop_front().ltrim();
      auto d = text.take_front(text.find_first_of(" \t\r\n"));
      if (d != "include" && d != "import" && d != "pragma")
        throw Error(
            "conditional or mutating preprocessor directives are unsupported");
      start = i + 1;
    } else if (v == ";")
      start = i + 1;
    else if (!t[i].kind && v == "{") {
      auto end = m[i];
      if (i && t[i - 1].text == ")") {
        auto po = m[i - 1];
        if (!po || po - 1 < start || !safe(t[po - 1].text))
          throw Error("unsupported C function declarator");
        out.push_back({t[po - 1].text, start, po - 1, po, i - 1, i, end});
        start = end + 1;
      }
      i = end;
    } else if (!t[i].kind && (v == "(" || v == "["))
      i = m[i];
  }
  return out;
}
std::vector<std::string> words(std::string_view s) {
  std::vector<std::string> r;
  for (const auto &t : tokens(s)) {
    if (t.kind || (!identifier(t.text) && t.text != "*"))
      throw Error("unsupported native scalar/pointer type");
    r.push_back(t.text);
  }
  return r;
}
std::pair<std::string, unsigned> abi(std::string_view s, unsigned ptr) {
  auto w = words(s);
  std::string base;
  bool pointer = false;
  for (auto &v : w) {
    if (qualifiers.count(v))
      continue;
    if (v == "*") {
      pointer = true;
      continue;
    }
    if (pointer)
      throw Error("unsupported native pointer declarator");
    if (!base.empty())
      base += ' ';
    base += v;
  }
  static const std::map<std::string, unsigned> integers = {
      {"char", 1},
      {"signed char", 1},
      {"unsigned char", 1},
      {"int8_t", 1},
      {"uint8_t", 1},
      {"BOOL", 1},
      {"bool", 1},
      {"_Bool", 1},
      {"short", 2},
      {"short int", 2},
      {"signed short", 2},
      {"signed short int", 2},
      {"unsigned short", 2},
      {"unsigned short int", 2},
      {"int16_t", 2},
      {"uint16_t", 2},
      {"int", 4},
      {"signed", 4},
      {"signed int", 4},
      {"unsigned", 4},
      {"unsigned int", 4},
      {"int32_t", 4},
      {"uint32_t", 4},
      {"long long", 8},
      {"long long int", 8},
      {"signed long long", 8},
      {"signed long long int", 8},
      {"unsigned long long", 8},
      {"unsigned long long int", 8},
      {"int64_t", 8},
      {"uint64_t", 8}};
  static const std::set<std::string> wordints = {
      "long",          "long int",          "signed long", "signed long int",
      "unsigned long", "unsigned long int", "intptr_t",    "uintptr_t",
      "size_t",        "ptrdiff_t"};
  auto it = integers.find(base);
  bool known = it != integers.end() || wordints.count(base) || base == "void" ||
               base == "float" || base == "double" || base == "id" ||
               base == "Class" || base == "SEL";
  if (!known)
    throw Error("unsupported native scalar/pointer type");
  if (pointer || base == "id" || base == "Class" || base == "SEL")
    return {"pointer", ptr};
  if (it != integers.end())
    return {"integer", it->second};
  if (wordints.count(base))
    return {"integer", ptr};
  return {base, base == "float" ? 4u : base == "double" ? 8u : 0u};
}
using Identity =
    std::tuple<std::string, std::string, bool, std::string, std::string>;
Identity identity(const Object &m, std::string owner = {}) {
  return {str(m, "class_name", owner), str(m, "selector"),
          flag(m, "class_method"), str(m, "category_name"),
          str(m, "category_address", "0x0")};
}
bool diagnosticText(llvm::StringRef text, size_t maximum) noexcept {
  return !text.empty() && text.size() <= maximum && llvm::json::isUTF8(text) &&
         std::all_of(text.begin(), text.end(), [](unsigned char c) {
           return (c >= 0x20 && c != 0x7f) || c == '\n' || c == '\r' ||
                  c == '\t';
         });
}
size_t backendDiagnosticSize(const Object &native, const Object &runtime,
                             const std::string &owner) noexcept {
  auto name = native.getString("class_name");
  if (!name || *name != owner)
    return 0;
  for (auto key : {"selector", "category_name", "category_address",
                   "implementation", "type_encoding"}) {
    auto a = native.getString(key), b = runtime.getString(key);
    if (!a || !b || *a != *b ||
        (a->empty() && llvm::StringRef(key) != "category_name"))
      return 0;
    if ((llvm::StringRef(key) == "implementation" ||
         llvm::StringRef(key) == "category_address") &&
        !hexAddress(*a))
      return 0;
  }
  auto a = native.getBoolean("class_method"),
       b = runtime.getBoolean("class_method");
  if (!a || !b || *a != *b)
    return 0;
  auto status = native.getString("status"), reason = native.getString("reason");
  if (!status || (*status != "recovered" && *status != "unrecovered"))
    return 0;
  size_t bytes = 0;
  if (*status == "unrecovered") {
    if (!reason || !diagnosticText(*reason, 2048))
      return 0;
    bytes += reason->size();
  } else if (native.get("reason") && (!reason || !reason->empty()))
    return 0;
  auto diagnostics = native.getArray("diagnostics");
  if (!diagnostics || diagnostics->size() > 8)
    return 0;
  for (const auto &value : *diagnostics) {
    auto text = value.getAsString();
    if (!text || !diagnosticText(*text, 512))
      return 0;
    bytes += text->size();
  }
  // Six bytes per input byte covers JSON escaping; fixed overhead covers the
  // bounded array and indentation. Normal publication accounts each embedding.
  return 512 + 6 * bytes;
}
struct Inventory {
  std::map<std::string, Object> classes;
  // Retain duplicate runtime class records in the method denominator even
  // when their names cannot provide one source declaration or layout.
  std::vector<Object> class_records;
  Array external;
  std::set<std::string> conflicts;
};
Inventory inventory(const Object &metadata) {
  Inventory out;
  for (const auto &v : array(metadata, "classes")) {
    const auto &c = object(v, "Objective-C class");
    auto name = requiredString(c, "name");
    array(c, "methods");
    out.class_records.push_back(c);
    auto [it, newentry] = out.classes.emplace(name, c);
    if (!newentry && Value(Object(it->second)) != v)
      out.conflicts.insert(name);
  }
  if (metadata.get("categories") && !metadata.getArray("categories"))
    throw Error("invalid Objective-C category inventory");
  if (auto cats = metadata.getArray("categories"))
    for (const auto &v : *cats) {
      const auto &c = object(v, "Objective-C category");
      auto name = requiredString(c, "name"),
           owner = requiredString(c, "class_name"),
           address = requiredString(c, "address");
      const auto &methods = array(c, "methods");
      for (const auto &m : methods) {
        const auto &method = object(m, "category method");
        if (str(method, "category_name") != name ||
            str(method, "category_address") != address ||
            !method.getBoolean("class_method"))
          throw Error("Objective-C category identity disagrees with owner");
        requiredString(method, "selector");
        requiredString(method, "implementation");
        requiredString(method, "type_encoding");
      }
      if (auto it = out.classes.find(owner); it != out.classes.end()) {
        auto *a = it->second.getArray("methods");
        for (const auto &m : methods)
          if (std::find(a->begin(), a->end(), m) == a->end())
            a->push_back(m);
        for (auto &record : out.class_records) {
          if (str(record, "name") != owner)
            continue;
          auto *record_methods = record.getArray("methods");
          for (const auto &method : methods)
            if (std::find(record_methods->begin(), record_methods->end(),
                          method) == record_methods->end())
              record_methods->push_back(method);
        }
      } else if (std::find(out.external.begin(), out.external.end(), v) ==
                 out.external.end())
        out.external.push_back(v);
    }
  return out;
}
struct DeclarationPlan {
  std::vector<std::string> ordered;
  std::map<std::string, std::string> errors;
};

// A method's source needs its complete class declaration even when it never
// accesses instance storage. Resolve this separately from ivar-layout proofs,
// and share the same plan with the header to avoid emitting an unusable class
// alongside otherwise recoverable methods.
DeclarationPlan declarationPlan(const Inventory &inv) {
  DeclarationPlan plan;
  std::set<std::string> available;
  for (const auto &[name, ignored] : inv.classes) {
    if (available.count(name) || plan.errors.count(name))
      continue;
    std::vector<std::string> chain;
    std::set<std::string> pending;
    std::string current = name, error;
    for (;;) {
      if (available.count(current))
        break;
      if (auto it = plan.errors.find(current); it != plan.errors.end()) {
        error = it->second;
        break;
      }
      if (!pending.insert(current).second) {
        error = "cyclic superclass declarations";
        break;
      }
      chain.push_back(current);
      const auto &cls = inv.classes.at(current);
      auto parent = str(cls, "superclass");
      if (!safe(current) || inv.conflicts.count(current)) {
        error = "invalid or conflicting class declaration";
        break;
      }
      if (importedFoundation.count(current) ||
          importedValueTypes.count(current)) {
        error = "class declaration conflicts with an imported Foundation type";
        break;
      }
      auto root = cls.getBoolean("root_class");
      if (!root || (*root && !parent.empty())) {
        error = "class declaration has inconsistent inheritance metadata";
        break;
      }
      if (*root)
        break;
      if (!safe(parent)) {
        error = "superclass declaration is unresolved or invalid";
        break;
      }
      if (!inv.classes.count(parent)) {
        if (!foundation.count(parent))
          error = "superclass declaration is unavailable: " + parent;
        break;
      }
      current = std::move(parent);
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      if (!error.empty()) {
        plan.errors[*it] = error;
      } else {
        available.insert(*it);
        plan.ordered.push_back(*it);
      }
    }
  }
  return plan;
}

// A local superclass needs an actual class definition at link time. Resolve
// that obligation after method-level conflicts: losing the only ordinary
// implementation can invalidate descendants and other class dependencies.
// Each class/edge/method is queued at most once; ancestor chains are shared.
std::set<std::string> closeClassDefinitions(
    const Inventory &inv, const DeclarationPlan &declarations,
    bool complete_metadata,
    const std::map<std::string, std::string> &layout_errors,
    const std::map<size_t, std::set<std::string>> &dependencies,
    Array &coverage, Budget &budget) {
  struct ClassState {
    std::string parent, reason;
    std::vector<std::string> children;
    std::vector<size_t> users;
    size_t bodies = 0;
    bool shell = false, available = false;
  };
  std::map<std::string, ClassState> classes;
  std::map<std::string, bool> complete_layout;
  for (const auto &name : declarations.ordered) {
    budget.tick();
    auto parent = str(inv.classes.at(name), "superclass");
    complete_layout[name] =
        !layout_errors.count(name) &&
        (!inv.classes.count(parent) || complete_layout[parent]);
  }
  for (const auto &[name, record] : inv.classes) {
    budget.tick();
    auto &state = classes[name];
    auto parent = str(record, "superclass");
    if (inv.classes.count(parent))
      state.parent = parent;
    bool has_methods = false;
    for (const auto &method : array(record, "methods")) {
      budget.tick();
      has_methods |= str(object(method, "method"), "category_name").empty();
    }
    if (auto it = declarations.errors.find(name);
        it != declarations.errors.end())
      state.reason = it->second;
    else if (has_methods)
      state.reason = "local class has no recovered ordinary implementation";
    else if (!complete_metadata)
      state.reason = "local class method inventory is incomplete";
    else if (!complete_layout[name])
      state.reason = "local class or ancestor layout is unavailable";
    else
      state.shell = true;
  }
  std::vector<bool> active(coverage.size());
  std::map<size_t, std::string> body_owner;
  for (const auto &[row, required] : dependencies) {
    budget.tick();
    const auto &method = object(coverage[row], "method coverage");
    if (str(method, "status") != "recovered")
      continue;
    active[row] = true;
    auto owner = str(method, "class_name");
    if (classes.count(owner) && str(method, "category_name").empty()) {
      ++classes.at(owner).bodies;
      body_owner.emplace(row, owner);
    }
    for (const auto &name : required) {
      budget.tick();
      classes.at(name).users.push_back(row);
    }
  }
  std::vector<std::string> missing;
  for (auto &[name, state] : classes) {
    budget.tick();
    state.available =
        !declarations.errors.count(name) && (state.shell || state.bodies != 0);
    if (!state.parent.empty())
      classes.at(state.parent).children.push_back(name);
    if (!state.available)
      missing.push_back(name);
  }
  auto invalidate = [&](const std::string &name, const std::string &reason) {
    auto &state = classes.at(name);
    if (state.available) {
      state.available = false;
      state.reason = reason;
      missing.push_back(name);
    }
  };
  for (size_t next = 0; next < missing.size(); ++next) {
    budget.tick();
    // Copy before pushing further missing classes can reallocate the queue.
    const auto name = missing[next];
    const auto &state = classes.at(name);
    const auto reason =
        "required local class definition is unavailable: " + name + ": " +
        state.reason;
    for (const auto &child : state.children) {
      budget.tick();
      invalidate(child, "local ancestor definition is unavailable: " + name);
    }
    for (size_t row : state.users) {
      budget.tick();
      if (!active[row])
        continue;
      active[row] = false;
      auto &method = *coverage[row].getAsObject();
      method["status"] = "unrecovered";
      method["reason"] = reason;
      if (auto owner = body_owner.find(row); owner != body_owner.end()) {
        auto &defining = classes.at(owner->second);
        --defining.bodies;
        if (!defining.bodies && !defining.shell)
          invalidate(owner->second,
                     "local class has no recovered ordinary implementation");
      }
    }
  }
  std::set<std::string> needed, shells;
  std::vector<std::string> pending;
  for (const auto &[row, required] : dependencies) {
    budget.tick();
    if (active[row])
      for (const auto &name : required) {
        budget.tick();
        pending.push_back(name);
      }
  }
  while (!pending.empty()) {
    budget.tick();
    auto name = std::move(pending.back());
    pending.pop_back();
    if (!needed.insert(name).second)
      continue;
    const auto &state = classes.at(name);
    if (state.available && state.shell && !state.bodies)
      shells.insert(name);
    if (!state.parent.empty())
      pending.push_back(state.parent);
  }
  return shells;
}

std::vector<std::string> layout(const Object &c,
                                const std::map<std::string, Object> &classes,
                                unsigned ptr) {
  if (ptr != 8 || str(c, "ivar_status") != "recovered")
    throw Error(
        "Instance-variable layout metadata is unavailable or incomplete.");
  auto start = number(c, "instance_start", -1),
       size = number(c, "instance_size", -1);
  if (start < 0 || size < start || size > (1 << 24) ||
      !c.getBoolean("root_class"))
    throw Error("Invalid instance size or root-class flag.");
  auto parent = str(c, "superclass");
  if (flag(c, "root_class")) {
    if (start || !parent.empty())
      throw Error("Root-class layout has inconsistent inheritance.");
  } else if (auto it = classes.find(parent); it != classes.end()) {
    if (number(it->second, "instance_size", -1) != start)
      throw Error("Subclass storage does not follow its superclass.");
  } else if (parent != "NSObject" || start != ptr)
    throw Error("Superclass instance-variable layout is unavailable.");
  std::set<std::string> names, ancestors{str(c, "name")};
  auto ancestor = parent;
  while (classes.count(ancestor)) {
    if (!ancestors.insert(ancestor).second)
      throw Error("Cyclic superclass instance layout.");
    const auto &p = classes.at(ancestor);
    if (auto *a = p.getArray("ivars"))
      for (const auto &i : *a)
        names.insert(str(object(i, "ivar"), "name"));
    ancestor = str(p, "superclass");
  }
  std::vector<std::tuple<int64_t, unsigned, std::string, std::string>> fields;
  const auto &ivars = array(c, "ivars");
  if (ivars.size() > 100000)
    throw Error("oversized ivar inventory");
  for (const auto &v : ivars) {
    const auto &i = object(v, "ivar");
    auto name = str(i, "name");
    if (!safe(name) || !names.insert(name).second)
      throw Error("Invalid or duplicate instance-variable name.");
    auto types = objcTypes(str(i, "type_encoding"));
    if (types.size() != 1)
      throw Error("Unsupported instance-variable type encoding.");
    auto [kind, width] = abi(types[0], ptr);
    auto offset = number(i, "offset", -1);
    if (!width || number(i, "size", -1) != width ||
        number(i, "alignment", -1) != width || offset < start ||
        offset > size - int64_t(width) || offset % width)
      throw Error("Inconsistent instance-variable width, alignment or offset.");
    fields.emplace_back(offset, width, name, types[0]);
  }
  std::sort(fields.begin(), fields.end());
  std::vector<std::string> lines;
  int64_t cursor = start;
  auto padding = [&](int64_t n) {
    auto name = "neverd_objc_padding_" + llvm::utohexstr(cursor, true);
    while (names.count(name))
      name += '_';
    names.insert(name);
    lines.push_back("    unsigned char " + name + "[" + std::to_string(n) +
                    "];");
  };
  for (auto &[off, width, name, type] : fields) {
    if (off < cursor)
      throw Error("Overlapping instance-variable storage.");
    if (off > cursor)
      padding(off - cursor);
    lines.push_back("    " + type + " " + name + ";");
    cursor = off + width;
  }
  if (size > cursor)
    padding(size - cursor);
  return lines;
}
std::vector<std::string> selectorParts(const Object &m) {
  auto s = requiredString(m, "selector");
  std::vector<std::string> parts;
  size_t start = 0;
  for (size_t i = 0; i < s.size(); ++i)
    if (s[i] == ':') {
      parts.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  if (parts.empty())
    parts.push_back(s);
  else if (start != s.size())
    throw Error("unsafe Objective-C selector");
  for (const auto &p : parts)
    if (!safe(p))
      throw Error("unsafe Objective-C selector");
  return parts;
}
std::string declaration(const Object &m,
                        const std::vector<std::string> &arguments = {}) {
  auto types = objcTypes(str(m, "type_encoding"));
  auto selector = requiredString(m, "selector");
  auto count = std::count(selector.begin(), selector.end(), ':');
  auto parts = selectorParts(m);
  if (types.size() != size_t(count + 3) ||
      (types[1] != "id" && types[1] != "Class") || types[2] != "SEL" ||
      !m.getBoolean("class_method"))
    throw Error("unsupported Objective-C type encoding");
  std::string out = (flag(m, "class_method") ? "+ (" : "- (") + types[0] + ")";
  if (!count)
    return out + selector;
  for (int i = 0; i < count; ++i) {
    if (types[i + 3] == "void")
      throw Error("void method argument");
    if (i)
      out += ' ';
    out += parts[i] + ":(" + types[i + 3] + ")" +
           (arguments.empty() ? "arg" + std::to_string(i) : arguments[i]);
  }
  return out;
}
std::string rewrite(std::string_view source,
                    const std::map<std::string, std::string> &names) {
  std::string out;
  size_t p = 0;
  for (const auto &t : tokens(source))
    if (!t.kind && names.count(t.text)) {
      out += source.substr(p, t.start - p);
      out += names.at(t.text);
      p = t.end;
    }
  out += source.substr(p);
  return out;
}
struct Rendered {
  std::string method, support;
  std::map<std::string, std::string> externals;
  std::map<std::string, std::pair<std::string, std::string>> shared;
};
Rendered render(const Object &native, const Object &runtime,
                const std::string &owner, unsigned ptr) {
  if (!safe(owner) || (!str(runtime, "category_name").empty() &&
                       !safe(str(runtime, "category_name"))))
    throw Error("unsafe Objective-C class or category");
  declaration(runtime);
  if (identity(native) != identity(runtime, owner))
    throw Error("native method identity disagrees with runtime metadata");
  for (auto k : {"selector", "class_method", "implementation", "type_encoding"})
    if (!native.get(k) || !runtime.get(k) || *native.get(k) != *runtime.get(k))
      throw Error("native method identity disagrees with runtime metadata");
  if (str(native, "status") != "recovered")
    throw Error(
        str(native, "reason", "native backend did not recover this method"));
  auto name = requiredString(native, "function_name");
  if (!safe(name))
    throw Error("unsafe native function identifier");
  auto types = objcTypes(str(runtime, "type_encoding"));
  const auto &params = array(native, "parameters");
  if (params.size() + 1 != types.size())
    throw Error("native parameter count disagrees with runtime signature");
  std::vector<std::string> expected,
      native_types{requiredString(native, "return_type")};
  for (size_t i = 0; i < params.size(); ++i) {
    expected.push_back(i == 0   ? "objc_self"
                       : i == 1 ? "objc_cmd"
                                : "arg" + std::to_string(i - 2));
    const auto &p = object(params[i], "parameter");
    if (str(p, "name") != expected.back())
      throw Error("native parameter names disagree with bound ABI");
    native_types.push_back(requiredString(p, "type"));
  }
  for (size_t i = 0; i < types.size(); ++i)
    if (abi(types[i], ptr) != abi(native_types[i], ptr))
      throw Error("native scalar/pointer ABI disagrees with runtime encoding");
  auto source = requiredString(native, "source");
  if (source.find('\0') != source.npos)
    throw Error("native method source contains NUL");
  auto t = tokens(source);
  auto match = pairs(t);
  auto defs = definitions(t, match);
  const Definition *target = nullptr;
  std::set<std::string> defined;
  for (const auto &d : defs) {
    if (!defined.insert(d.name).second)
      throw Error("duplicate C function definition");
    if (d.name == name)
      target = &d;
  }
  if (!target)
    throw Error("expected exactly one definition for function_name");
  auto &d = *target;
  std::vector<std::string> ret;
  for (size_t i = d.start; i < d.name_index; ++i) {
    if (t[i].text == "__attribute__")
      throw Error("native attributes cannot transfer to Objective-C method");
    if (!storage.count(t[i].text))
      ret.push_back(t[i].text);
  }
  if (ret != words(native_types[0]))
    throw Error("emitted return type disagrees with native report");
  std::vector<std::vector<std::string>> groups(1);
  for (size_t i = d.po + 1; i < d.pc; ++i)
    if (t[i].text == ",")
      groups.emplace_back();
    else
      groups.back().push_back(t[i].text);
  if (groups.size() != params.size())
    throw Error("emitted parameter count disagrees with native report");
  for (size_t i = 0; i < params.size(); ++i) {
    auto w = words(native_types[i + 1]);
    w.push_back(expected[i]);
    if (w != groups[i])
      throw Error("emitted parameter declaration disagrees with native report");
  }
  bool content = false, declaring = false, statement = true;
  int depth = 0;
  for (size_t i = d.bo + 1; i < d.bc; ++i) {
    auto v = t[i].text;
    content |= v != ";";
    if (t[i].kind == 2 || v == "self" || v == "_cmd" || v == "super")
      throw Error("native method body conflicts with implicit bindings");
    if (v == "{")
      ++depth;
    else if (v == "}")
      --depth;
    else if (!depth) {
      if (statement) {
        declaring = qualifiers.count(v);
        if (!declaring)
          try {
            abi(v, ptr);
            declaring = true;
          } catch (const Error &) {
          }
        statement = false;
      } else if (declaring && std::find(expected.begin(), expected.end(), v) !=
                                  expected.end())
        throw Error("native body redeclares a bound parameter");
      if (v == "=")
        declaring = false;
      else if (v == ";") {
        statement = true;
        declaring = false;
      }
    }
  }
  if (!content)
    throw Error("native method body is empty");
  auto fingerprint =
      jsonText(Value(Object{{"class", owner}, {"method", Object(runtime)}}));
  auto hash = llvm::SHA256::hash(llvm::arrayRefFromStringRef(fingerprint));
  auto tag = llvm::toHex(hash, true).substr(0, 16);
  std::set<std::string> sharednames;
  if (native.get("shared_block_functions") &&
      !native.getArray("shared_block_functions"))
    throw Error("invalid shared Block function inventory");
  if (auto *shared = native.getArray("shared_block_functions"))
    for (const auto &v : *shared) {
      auto s = v.getAsString();
      if (!s || !sharednames.insert(s->str()).second ||
          !defined.count(s->str()) || s == name ||
          !std::regex_match(
              s->str(), std::regex("neverd_block_(invoke_[0-9a-f]+|(descriptor|"
                                   "literal)_[0-9a-f]+_address)")))
        throw Error("invalid shared Block function inventory");
    }
  std::map<std::string, std::string> rename;
  for (const auto &f : defs)
    if (!sharednames.count(f.name))
      rename[f.name] = "neverd_objc_" + tag + "_" + f.name;
  for (const auto &token : t)
    for (const auto &[old, n] : rename)
      if (token.text == n)
        throw Error("generated support name collision");
  Rendered out;
  std::string support;
  size_t cursor = 0;
  for (const auto &f : defs)
    if (sharednames.count(f.name)) {
      auto start = t[f.start].start, end = t[f.bc].end;
      auto proto =
          llvm::StringRef(source).slice(start, t[f.bo].start).rtrim().str() +
          ";";
      auto full = source.substr(start, end - start);
      out.shared[f.name] = {rewrite(proto, rename), rewrite(full, rename)};
      support += source.substr(cursor, start - cursor) + proto;
      cursor = end;
    }
  support += source.substr(cursor);
  out.support = rewrite(support, rename);
  std::vector<std::string> args;
  for (size_t i = 2; i < params.size(); ++i) {
    args.push_back("neverd_objc_argument_" + tag + "_" + std::to_string(i - 2));
    for (const auto &tok : t)
      if (tok.text == args.back())
        throw Error("generated method argument collision");
  }
  out.method = declaration(runtime, args) + "\n{\n";
  for (size_t i = 0; i < params.size(); ++i)
    out.method += "    " + native_types[i + 1] + " " + expected[i] + " = (" +
                  native_types[i + 1] + ")" +
                  (i == 0   ? "self"
                   : i == 1 ? "_cmd"
                            : args[i - 2]) +
                  ";\n";
  out.method += rewrite(std::string_view(source).substr(
                            t[d.bo].end, t[d.bc].start - t[d.bo].end),
                        rename) +
                "\n}\n";
  for (size_t i = 0; i < t.size();) {
    if (t[i].kind == 2) {
      ++i;
      continue;
    }
    auto f = std::find_if(defs.begin(), defs.end(),
                          [&](const auto &v) { return v.start == i; });
    if (f != defs.end()) {
      i = f->bc + 1;
      continue;
    }
    size_t end = i;
    while (end < t.size() && t[end].text != ";") {
      if (!t[end].kind &&
          (t[end].text == "(" || t[end].text == "[" || t[end].text == "{"))
        end = match[end];
      ++end;
    }
    if (end == t.size())
      throw Error("unrecognized trailing C source");
    if (end > i) {
      std::string symbol, spelling;
      size_t opening = i;
      while (opening < end && t[opening].text != "(")
        ++opening;
      if (opening > i && opening < end && t[end - 1].text == ")")
        symbol = t[opening - 1].text;
      else if (t[i].text == "extern" && safe(t[end - 1].text))
        symbol = t[end - 1].text;
      else if (end - i == 6 && t[i].text == "extern" &&
               t[i + 1].text == "void" && t[i + 2].text == "*" &&
               (t[i + 3].text == "_NSConcreteStackBlock" ||
                t[i + 3].text == "_NSConcreteGlobalBlock") &&
               t[i + 4].text == "[" && t[i + 5].text == "]")
        symbol = t[i + 3].text;
      else
        throw Error("unsupported non-function C support declaration");
      if (!safe(symbol))
        throw Error("unsupported external declaration");
      if (!rename.count(symbol)) {
        for (size_t j = i; j < end; ++j)
          if (t[j].text != "extern")
            spelling += t[j].text + " ";
        auto [it, fresh] = out.externals.emplace(symbol, spelling);
        if (!fresh && it->second != spelling)
          throw Error("conflicting external declarations");
      }
    }
    i = end + 1;
  }
  return out;
}
} // namespace
std::vector<std::string> objcTypes(std::string_view e) {
  static const std::map<char, std::string> primitive = {
      {'v', "void"},
      {'c', "signed char"},
      {'C', "unsigned char"},
      {'s', "short"},
      {'S', "unsigned short"},
      {'i', "int"},
      {'I', "unsigned int"},
      {'l', "int"},
      {'L', "unsigned int"},
      {'q', "long long"},
      {'Q', "unsigned long long"},
      {'f', "float"},
      {'d', "double"},
      {'B', "BOOL"},
      {'@', "id"},
      {'#', "Class"},
      {':', "SEL"},
      {'*', "char *"}};
  std::vector<std::string> out;
  size_t p = 0;
  while (p < e.size()) {
    unsigned pointers = 0;
    while (p < e.size() &&
           (std::isdigit(static_cast<unsigned char>(e[p])) ||
            std::string_view("rnNoORV+-").find(e[p]) != std::string_view::npos))
      ++p;
    if (p == e.size())
      break;
    while (p < e.size() && e[p] == '^') {
      ++p;
      ++pointers;
      while (p < e.size() &&
             std::string_view("rnNoORV").find(e[p]) != std::string_view::npos)
        ++p;
    }
    if (p == e.size() || !primitive.count(e[p]))
      return {};
    char marker = e[p++];
    auto value = primitive.at(marker);
    if (marker == '@' && p < e.size() && e[p] == '"') {
      auto end = e.find('"', p + 1);
      if (end == e.npos)
        return {};
      p = end + 1;
    }
    if (marker == '@' && p < e.size() && e[p] == '?')
      ++p;
    while (pointers--)
      value += " *";
    out.push_back(value);
  }
  return out;
}
std::string objcHeader(const Object &metadata, unsigned ptr) {
  auto inv = inventory(metadata);
  auto declarations = declarationPlan(inv);
  std::string
      out = "// Recovered Objective-C declarations. See objc.json for coverage "
            "and raw encodings.\n#import <Foundation/Foundation.h>\n\n",
      categories;
  for (const auto &[name, c] : inv.classes)
    if (safe(name) && !declarations.errors.count(name))
      out += "@class " + name + ";\n";
  for (const auto &[name, error] : declarations.errors)
    out += "// Class declaration omitted" + (safe(name) ? " for " + name : "") +
           ": " + error + ".\n";
  auto methods = [&](const Array &ms) {
    std::string text;
    for (const auto &m : ms)
      try {
        text += declaration(object(m, "method")) + ";\n";
      } catch (const Error &) {
        text += "// Method declaration omitted: unsupported selector or type "
                "encoding; see objc.json.\n";
      }
    return text;
  };
  for (const auto &name : declarations.ordered) {
    const auto &c = inv.classes.at(name);
    auto p = str(c, "superclass");
    out += '\n';
    if (p.empty() && flag(c, "root_class"))
      out += "__attribute__((objc_root_class))\n";
    out += "@interface " + name + (p.empty() ? "" : " : " + p) + "\n";
    try {
      auto lines = layout(c, inv.classes, ptr);
      if (!lines.empty()) {
        out += "{\n@protected\n";
        for (const auto &line : lines)
          out += line + "\n";
        out += "}\n";
      }
    } catch (const Error &e) {
      out += "// Instance-variable declarations omitted: " +
             std::string(e.what()) + "\n";
    }
    Array base;
    std::map<std::string, Array> cats;
    for (const auto &m : array(c, "methods")) {
      auto cat = str(object(m, "method"), "category_name");
      if (cat.empty())
        base.push_back(m);
      else if (identifier(cat))
        cats[cat].push_back(m);
    }
    out += methods(base) + "@end\n";
    for (const auto &[cat, ms] : cats)
      categories +=
          "\n@interface " + name + " (" + cat + ")\n" + methods(ms) + "@end\n";
  }
  for (const auto &v : inv.external) {
    const auto &c = object(v, "category");
    auto name = str(c, "class_name"), cat = str(c, "name");
    if (safe(name) && !importedValueTypes.count(name))
      out += "@class " + name + ";\n";
    if (foundation.count(name) && identifier(cat))
      categories += "\n@interface " + name + " (" + cat + ")\n" +
                    methods(array(c, "methods")) + "@end\n";
    else
      categories += "// External category declaration omitted: its class "
                    "declaration is unavailable.\n";
  }
  return out + categories;
}
uint64_t nativeFunctionCount(std::string_view source) {
  auto t = tokens(source);
  auto ds = definitions(t, pairs(t));
  uint64_t count = 0;
  for (const auto &d : ds) {
    bool stat = false, inl = false;
    for (size_t i = d.start; i < d.name_index; ++i) {
      stat |= t[i].text == "static";
      inl |= t[i].text == "inline";
    }
    if (!(stat && inl))
      ++count;
  }
  return count;
}
SourceResult objcSources(const Object &batch, const Object &metadata,
                         unsigned ptr, Budget &budget) {
  if (number(batch, "schema_version") != 1)
    throw Error("unsupported Objective-C report schema");
  if (batch.get("pointer_size") && (!batch.getInteger("pointer_size") ||
                                    number(batch, "pointer_size") != ptr))
    throw Error(
        "native Objective-C pointer size disagrees with selected image");
  auto inv = inventory(metadata);
  auto declarations = declarationPlan(inv);
  std::map<Identity, std::vector<Object>> batches;
  for (const auto &v : array(batch, "methods")) {
    const auto &m = object(v, "native method");
    batches[identity(m)].push_back(m);
  }
  std::map<std::string, std::string> errors;
  for (const auto &[name, c] : inv.classes)
    try {
      layout(c, inv.classes, ptr);
    } catch (const Error &e) {
      errors[name] = std::string(e.what());
    }
  std::vector<std::pair<std::string, Object>> runtime;
  for (const auto &record : inv.class_records)
    runtime.emplace_back(str(record, "name"), record);
  std::map<std::string, Object> external_owners;
  for (const auto &v : inv.external) {
    const auto &cat = object(v, "category");
    auto owner = str(cat, "class_name");
    auto [it, fresh] = external_owners.emplace(
        owner,
        Object{{"name", owner}, {"methods", Array{}}, {"external", true}});
    for (const auto &m : array(cat, "methods"))
      it->second.getArray("methods")->push_back(m);
  }
  for (auto &[name, record] : external_owners)
    runtime.emplace_back(name, std::move(record));
  struct Candidate {
    size_t row;
    Rendered render;
  };
  std::vector<Candidate> candidates;
  std::map<size_t, std::set<std::string>> class_dependencies;
  Array coverage;
  struct RuntimeMatch {
    size_t row;
    const Object *method;
    const std::string *owner;
    bool duplicate = false;
  };
  std::map<Identity, RuntimeMatch> encountered;
  std::map<std::pair<std::string, std::string>, std::set<std::string>>
      cataddresses;
  for (const auto &[name, c] : runtime)
    for (const auto &v : array(c, "methods")) {
      const auto &m = object(v, "method");
      if (!str(m, "category_name").empty())
        cataddresses[{name, str(m, "category_name")}].insert(
            str(m, "category_address"));
    }
  for (const auto &[name, c] : runtime)
    for (const auto &v : array(c, "methods")) {
      budget.tick();
      const auto &m = object(v, "method");
      auto key = identity(m, name);
      Object row{{"class_name", name},
                 {"selector", str(m, "selector")},
                 {"class_method", flag(m, "class_method")},
                 {"category_name", str(m, "category_name")},
                 {"category_address", str(m, "category_address", "0x0")},
                 {"implementation", str(m, "implementation")},
                 {"status", "unrecovered"},
                 {"diagnostics", Array{}}};
      try {
        auto [entry, fresh] =
            encountered.emplace(key, RuntimeMatch{coverage.size(), &m, &name});
        if (!fresh) {
          entry->second.duplicate = true;
          throw Error("duplicate runtime method identity");
        }
        if (inv.conflicts.count(name))
          throw Error("duplicate inconsistent runtime class records");
        if (auto it = declarations.errors.find(name);
            it != declarations.errors.end())
          throw Error("required class declaration is unavailable: " + name +
                      ": " + it->second);
        if (flag(c, "external") && !foundation.count(name))
          throw Error("external category class declaration is unavailable");
        if (!str(m, "category_name").empty() &&
            cataddresses[{name, str(m, "category_name")}].size() != 1)
          throw Error(
              "duplicate category names have distinct runtime identities");
        auto it = batches.find(key);
        if (it == batches.end())
          throw Error("native backend omitted this runtime method");
        const auto &native = it->second[0];
        for (const auto &other : it->second)
          if (Value(Object(other)) != Value(Object(native)))
            throw Error("duplicate inconsistent native method records");
        if (native.get("diagnostics") && !native.getArray("diagnostics"))
          throw Error("invalid native diagnostics");
        if (auto *d = native.getArray("diagnostics")) {
          for (const auto &s : *d)
            if (!s.getAsString())
              throw Error("invalid native diagnostics");
          row["diagnostics"] = Array(*d);
        }
        std::vector<std::string> deps;
        std::set<std::string> required_classes;
        if (inv.classes.count(name))
          required_classes.insert(name);
        if (auto *a = native.getArray("instance_layout_classes")) {
          for (const auto &d : *a) {
            if (!d.getAsString())
              throw Error("invalid instance-layout dependency");
            deps.push_back(d.getAsString()->str());
            required_classes.insert(deps.back());
          }
        } else if (native.get("instance_layout_classes"))
          throw Error("invalid instance-layout dependency inventory");
        else if (c.get("ivar_status") && errors.count(name))
          deps.push_back(name);
        std::set<std::string> checked;
        while (!deps.empty()) {
          auto dep = deps.back();
          deps.pop_back();
          if (!checked.insert(dep).second)
            continue;
          if (!inv.classes.count(dep) || inv.conflicts.count(dep) ||
              errors.count(dep))
            throw Error("required instance-variable layout is unavailable: " +
                        dep);
          auto parent = str(inv.classes.at(dep), "superclass");
          if (inv.classes.count(parent))
            deps.push_back(parent);
        }
        auto r = render(native, m, name, ptr);
        row["status"] = "recovered";
        class_dependencies.emplace(coverage.size(),
                                   std::move(required_classes));
        candidates.push_back({coverage.size(), std::move(r)});
      } catch (const Error &e) {
        row["reason"] = std::string(e.what());
      }
      coverage.push_back(std::move(row));
    }
  for (const auto &[key, entries] : batches)
    if (!encountered.count(key)) {
      auto row = entries[0];
      row.erase("source");
      row.erase("parameters");
      row.erase("native_backend");
      row["status"] = "unrecovered";
      row["reason"] = "native method has no matching runtime metadata";
      coverage.push_back(std::move(row));
    }
  std::map<std::string, std::vector<std::pair<size_t, std::string>>> external,
      shared;
  for (const auto &c : candidates) {
    for (const auto &[name, spelling] : c.render.externals)
      external[name].push_back({c.row, spelling});
    for (const auto &[name, def] : c.render.shared)
      shared[name].push_back({c.row, def.first + "\n" + def.second});
  }
  auto conflicts = [&](const auto &all, const char *reason) {
    for (const auto &[name, uses] : all) {
      bool mismatch = false;
      for (const auto &use : uses) {
        budget.tick();
        mismatch |= use.second != uses[0].second;
      }
      if (mismatch)
        for (const auto &use : uses) {
          budget.tick();
          auto *r = coverage[use.first].getAsObject();
          (*r)["status"] = "unrecovered";
          (*r)["reason"] = reason;
        }
    }
  };
  conflicts(external, "conflicting external declarations across methods");
  conflicts(shared, "conflicting shared Block function definitions");
  auto shells = closeClassDefinitions(
      inv, declarations, str(metadata, "status") == "recovered", errors,
      class_dependencies, coverage, budget);
  std::string source = "// Objective-C bodies reconstructed from native code; "
                       "see objc-methods.json for coverage.\n#include "
                       "<stdint.h>\n#include <stdbool.h>\n" +
                       objcHeader(metadata, ptr);
  std::map<std::pair<std::string, std::string>, std::vector<std::string>>
      methods;
  std::map<std::string, std::pair<std::string, std::string>> sharedout;
  uint64_t recovered = 0;
  for (const auto &c : candidates) {
    const auto &r = *coverage[c.row].getAsObject();
    if (str(r, "status") != "recovered")
      continue;
    ++recovered;
    source += c.render.support + "\n";
    methods[{str(r, "class_name"), str(r, "category_name")}].push_back(
        c.render.method);
    sharedout.insert(c.render.shared.begin(), c.render.shared.end());
  }
  for (const auto &[name, def] : sharedout)
    source += def.first + "\n";
  for (const auto &[name, def] : sharedout)
    source += def.second + "\n";
  for (const auto &name : shells)
    methods.try_emplace({name, ""});
  for (const auto &[owner, ms] : methods) {
    source += "@implementation " + owner.first +
              (owner.second.empty() ? "" : " (" + owner.second + ")") + "\n";
    for (const auto &m : ms)
      source += m;
    source += "@end\n\n";
  }
  size_t diagnostic_bytes = 256 * 1024;
  for (const auto &[key, match] : encountered) {
    if (diagnostic_bytes < 512)
      break;
    budget.tick();
    auto &row = *coverage[match.row].getAsObject();
    auto native = batches.find(key);
    if (match.duplicate || inv.conflicts.count(*match.owner) ||
        row.getString("status") != "unrecovered" || native == batches.end() ||
        native->second.size() != 1)
      continue;
    const auto &record = native->second.front();
    const auto bytes =
        backendDiagnosticSize(record, *match.method, *match.owner);
    if (!bytes || bytes > diagnostic_bytes)
      continue;
    try {
      Object summary{{"status", record.getString("status")->str()},
                     {"reason", nullptr},
                     {"diagnostics", Array(*record.getArray("diagnostics"))}};
      if (record.getString("status") == "unrecovered")
        summary["reason"] = record.getString("reason")->str();
      row["native_backend"] = std::move(summary);
      diagnostic_bytes -= bytes;
    } catch (const std::bad_alloc &) {
      row.erase("native_backend");
    }
  }
  std::string status =
      coverage.empty() ? "no-methods"
      : !recovered     ? "unrecovered"
      : recovered != coverage.size() || str(metadata, "status") != "recovered"
          ? "partial"
          : "recovered";
  return {
      std::move(source),
      Object{
          {"schema_version", 1},
          {"status", status},
          {"method_count", coverage.size()},
          {"recovered_method_count", recovered},
          {"unrecovered_method_count", coverage.size() - recovered},
          {"methods", std::move(coverage)},
          {"limitations",
           Array{"Objective-C method bodies are projections of native C; "
                 "original source and method-level semantic equivalence are "
                 "not guaranteed.",
                 "Only supported scalar and pointer runtime signatures with a "
                 "verified native definition are emitted.",
                 "Runtime @? Block parameters are declared as id; invocation "
                 "requires a separately verified native signature.",
                 "Coverage applies only to the discovered runtime method "
                 "inventory; an empty inventory does not prove that no methods "
                 "exist.",
                 "Verified identical Block storage helpers share object "
                 "identity across methods. External dependencies may require "
                 "manual linking."}}}};
}
} // namespace neverd::mobile::ios
