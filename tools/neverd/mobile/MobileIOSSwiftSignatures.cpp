#include "MobileIOSInternal.h"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <memory>
#include <regex>
#include <sstream>

namespace neverd::mobile::ios {
namespace {
const std::set<std::string> callables = {"Function",
                                         "Allocator",
                                         "Constructor",
                                         "Destructor",
                                         "Deallocator",
                                         "Getter",
                                         "Setter",
                                         "GlobalGetter",
                                         "ModifyAccessor",
                                         "ReadAccessor",
                                         "MaterializeForSet",
                                         "WillSet",
                                         "DidSet",
                                         "ExplicitClosure",
                                         "ImplicitClosure",
                                         "DefaultArgumentInitializer",
                                         "Initializer",
                                         "IVarInitializer",
                                         "IVarDestroyer",
                                         "UnsafeAddressor",
                                         "UnsafeMutableAddressor",
                                         "OwningAddressor",
                                         "OwningMutableAddressor",
                                         "NativeOwningAddressor",
                                         "NativeOwningMutableAddressor",
                                         "NativePinningAddressor",
                                         "NativePinningMutableAddressor",
                                         "ProtocolWitness",
                                         "ReabstractionThunk",
                                         "ReabstractionThunkHelper",
                                         "CurryThunk",
                                         "VTableThunk",
                                         "DispatchThunk",
                                         "MethodLookupFunction",
                                         "TypeMetadataAccessFunction"};
const std::set<std::string> metadata = {
    "ModuleDescriptor",
    "NominalTypeDescriptor",
    "ProtocolDescriptor",
    "TypeMetadata",
    "FullTypeMetadata",
    "TypeMetadataPattern",
    "Metaclass",
    "ClassMetadataBaseOffset",
    "PropertyDescriptor",
    "FieldOffset",
    "MethodDescriptor",
    "ProtocolWitnessTable",
    "ValueWitnessTable",
    "ReflectionMetadataFieldDescriptor",
    "ReflectionMetadataBuiltinDescriptor",
    "ReflectionMetadataAssocTypeDescriptor",
    "ReflectionMetadataSuperclassDescriptor"};
struct Node {
  std::string kind;
  std::optional<std::string> text;
  std::optional<uint64_t> index;
  std::vector<std::unique_ptr<Node>> children;
};
std::unique_ptr<Node> tree(std::string_view text) {
  std::unique_ptr<Node> root;
  std::vector<Node *> stack;
  size_t pos = 0, count = 0;
  while (pos < text.size()) {
    auto end = text.find('\n', pos);
    if (end == text.npos)
      end = text.size();
    auto line = text.substr(pos, end - pos);
    pos = end < text.size() ? end + 1 : end;
    if (line.ends_with('\r'))
      line.remove_suffix(1);
    if (line.empty())
      continue;
    size_t indent = 0;
    while (indent < line.size() && line[indent] == ' ')
      ++indent;
    auto depth = indent / 2;
    if (indent % 2 || depth > 64 || ++count > 10000 || depth > stack.size())
      throw Error("invalid or excessive Swift demangling tree depth");
    line.remove_prefix(indent);
    if (!line.starts_with("kind="))
      throw Error("malformed Swift demangling tree");
    line.remove_prefix(5);
    auto comma = line.find(',');
    auto kind = line.substr(0, comma);
    if (!identifier(kind))
      throw Error("malformed Swift node kind");
    auto node = std::make_unique<Node>();
    node->kind = kind;
    if (comma != line.npos)
      line.remove_prefix(comma);
    else
      line = {};
    while (!line.empty()) {
      if (line.starts_with(", text=")) {
        if (node->text)
          throw Error("duplicate Swift node text");
        line.remove_prefix(7);
        if (line.empty() || line[0] != '"')
          throw Error("invalid Swift node text");
        size_t p = 1;
        bool closed = false;
        while (p < line.size()) {
          if (line[p] == '\\')
            p += 2;
          else if (line[p++] == '"') {
            closed = true;
            break;
          }
        }
        if (!closed || p > line.size())
          throw Error("unterminated Swift node text");
        auto value = parseJSON(line.substr(0, p), "Swift node text");
        auto s = value.getAsString();
        if (!s)
          throw Error("invalid Swift node text");
        node->text = s->str();
        line.remove_prefix(p);
      } else if (line.starts_with(", index=")) {
        if (node->index)
          throw Error("duplicate Swift node index");
        line.remove_prefix(8);
        size_t p = 0;
        while (p < line.size() && line[p] >= '0' && line[p] <= '9')
          ++p;
        uint64_t n = 0;
        auto r = std::from_chars(line.data(), line.data() + p, n);
        if (!p || r.ec != std::errc())
          throw Error("invalid Swift node index");
        node->index = n;
        line.remove_prefix(p);
      } else
        throw Error("invalid Swift demangling node attribute");
    }
    auto raw = node.get();
    if (!depth) {
      if (root)
        throw Error("multiple Swift demangling roots");
      root = std::move(node);
    } else
      stack[depth - 1]->children.push_back(std::move(node));
    stack.resize(depth);
    stack.push_back(raw);
  }
  if (!root)
    throw Error("empty Swift demangling tree");
  return root;
}
const Node &one(const Node &n, std::string_view kind = {}) {
  if (n.children.size() != 1 || (!kind.empty() && n.children[0]->kind != kind))
    throw Error("unsupported Swift type or declaration shape");
  return *n.children[0];
}
std::string ident(const Node &n, std::string_view kind = "Identifier") {
  if (n.kind != kind || !n.children.empty() || !n.text || !identifier(*n.text))
    throw Error("unsafe or unsupported Swift declaration identifier");
  return *n.text;
}
std::pair<std::string, std::string> nominal(const Node &n) {
  if (n.children.size() != 2)
    throw Error("nested or generic Swift contexts are unsupported");
  return {ident(*n.children[0], "Module"), ident(*n.children[1])};
}
struct Context {
  std::string module, name, kind;
};
Context context(const Node &n) {
  if (n.kind == "Module")
    return {ident(n, "Module"), "", "global"};
  if (n.kind == "Class" || n.kind == "Structure") {
    auto [m, name] = nominal(n);
    return {m, name, n.kind == "Class" ? "class" : "struct"};
  }
  throw Error("unsupported Swift declaration context");
}
Object type(const Node &n, unsigned pointer_size, unsigned depth = 0) {
  if (depth > 8)
    throw Error("Swift pointer type is too deep");
  if (n.kind == "Type")
    return type(one(n), pointer_size, depth);
  if (n.kind == "Tuple" && n.children.empty())
    return Object{{"kind", "void"}, {"name", "Void"}};
  if (n.kind == "BoundGenericStructure") {
    if (n.children.size() != 2 || n.children[0]->kind != "Type" ||
        n.children[1]->kind != "TypeList")
      throw Error("unsupported Swift generic type");
    auto [module, name] = nominal(one(*n.children[0], "Structure"));
    if (module != "Swift" ||
        (name != "UnsafePointer" && name != "UnsafeMutablePointer"))
      throw Error(
          "Swift generic types other than ordinary pointers are unsupported");
    auto pointee = type(one(*n.children[1], "Type"), pointer_size, depth + 1);
    if (str(pointee, "kind") == "void")
      throw Error("typed Swift pointer has void pointee");
    return Object{
        {"kind", "pointer"}, {"name", name}, {"pointee", std::move(pointee)}};
  }
  if (n.kind != "Structure")
    throw Error("unsupported Swift signature type: " + n.kind);
  auto [module, name] = nominal(n);
  if (module != "Swift")
    throw Error("user-defined Swift value layouts are not established");
  if (name == "Float" || name == "Double")
    return Object{
        {"kind", "float"}, {"name", name}, {"bits", name == "Float" ? 32 : 64}};
  if (name == "Bool")
    return Object{{"kind", "bool"}, {"name", name}, {"bits", 1}};
  if (name == "UnsafeRawPointer" || name == "UnsafeMutableRawPointer")
    return Object{{"kind", "pointer"}, {"name", name}};
  std::smatch match;
  if (!std::regex_match(name, match, std::regex("(U?Int)(8|16|32|64)?")))
    throw Error("unsupported Swift scalar type: " + name);
  return Object{
      {"kind", "integer"},
      {"name", name},
      {"bits", match[2].matched ? std::stoi(match[2]) : int(pointer_size * 8)},
      {"signed", match[1] == "Int"}};
}
void putContext(Object &row, const Context &c) {
  row["module"] = c.module;
  row["context_name"] = c.name;
  row["context_kind"] = c.kind;
}
struct Property {
  Context ctx;
  std::string name;
  Object value;
};
Property property(const Node &n, bool is_static, unsigned ptr) {
  const auto &v = one(n, "Variable");
  if (v.children.size() != 3)
    throw Error("unsupported Swift property shape");
  auto c = context(*v.children[0]);
  if (c.kind == "global" || is_static)
    throw Error(
        "global and static Swift property conventions are not established");
  if (v.children[2]->kind != "Type")
    throw Error("Swift property type wrapper is missing");
  auto t = type(*v.children[2], ptr);
  if (str(t, "kind") == "void")
    throw Error("Swift scalar property has a void type");
  return {c, ident(*v.children[1]), std::move(t)};
}
void runtime(Object &row, const Node &node, bool is_static, unsigned ptr,
             bool continuation = false) {
  Context c;
  std::string name, kind;
  if (node.kind == "ModifyAccessor") {
    auto p = property(node, is_static, ptr);
    c = p.ctx;
    name = p.name;
    row["property_type"] = std::move(p.value);
    kind = continuation ? "modify_resume" : "modify_accessor";
  } else {
    if (is_static || continuation)
      throw Error("unsupported Swift runtime wrapper");
    auto *n = &one(node);
    if (node.kind == "TypeMetadataAccessFunction" && n->kind == "Type")
      n = &one(*n);
    c = context(*n);
    if (c.kind == "global" ||
        (node.kind != "TypeMetadataAccessFunction" && c.kind != "class"))
      throw Error("unsupported Swift runtime context");
    kind = node.kind == "Destructor"    ? "destructor"
           : node.kind == "Deallocator" ? "deallocator"
                                        : "type_metadata_accessor";
    name =
        node.kind == "TypeMetadataAccessFunction" ? "typeMetadata" : "deinit";
  }
  putContext(row, c);
  row["declaration_kind"] = "runtime";
  row["runtime_source_kind"] = kind;
  row["name"] = name;
  row["parameters"] = Array{};
  row["labels"] = Array{};
  row["return_type"] = Object{{"kind", "void"}, {"name", "Void"}};
  row["is_static"] = false;
  row["is_mutating"] = false;
  row["is_mutating_known"] = false;
  row["requires_runtime_source_proof"] = true;
  throw Error("Swift compiler entry requires native effect and recovered "
              "source dependency proof");
}
bool swiftName(std::string_view s) {
  while (s.starts_with('_'))
    s.remove_prefix(1);
  return s.starts_with("$s") || s.starts_with("$S") || s.starts_with("T0");
}
} // namespace
Object swiftSignature(std::string_view entry, std::string_view mangled,
                      std::string_view expanded, unsigned ptr) {
  Object row{{"entry", std::string(entry)},
             {"mangled_symbol", std::string(mangled)},
             {"status", "unsupported"},
             {"classification", "unknown"}};
  try {
    if (ptr != 8 || mangled.size() > 65536 || !swiftName(mangled) ||
        !hexAddress(entry))
      throw Error("invalid Swift symbol identity or unsupported pointer size");
    auto root = tree(expanded);
    if (root->kind != "Global")
      throw Error("Swift root is not a global symbol");
    if (root->children.size() == 2 &&
        callables.count(root->children[0]->kind) &&
        root->children[1]->kind == "Suffix" &&
        root->children[1]->children.empty() && root->children[1]->text &&
        std::regex_match(*root->children[1]->text,
                         std::regex("\\.resume\\.[0-9]+"))) {
      row["classification"] = "callable";
      row["node_kind"] = "CoroutineContinuation";
      row["continuation_of"] = root->children[0]->kind;
      row["compiler_suffix"] = *root->children[1]->text;
      if (root->children[0]->kind == "ModifyAccessor")
        runtime(row, *root->children[0], false, ptr, true);
      throw Error("Swift coroutine continuation requires suspended-frame "
                  "calling convention");
    }
    auto *n = &one(*root);
    bool is_static = n->kind == "Static";
    if (is_static)
      n = &one(*n);
    row["node_kind"] = n->kind;
    row["classification"] = callables.count(n->kind)  ? "callable"
                            : metadata.count(n->kind) ? "metadata"
                                                      : "unknown";
    if (n->kind == "Getter" || n->kind == "Setter") {
      auto p = property(*n, is_static, ptr);
      putContext(row, p.ctx);
      bool setter = n->kind == "Setter";
      row["declaration_kind"] = setter ? "setter" : "getter";
      row["name"] = p.name;
      row["labels"] = setter ? Array{"_"} : Array{};
      row["parameters"] =
          setter ? Array{Object{{"name", "arg0"}, {"type", Object(p.value)}}}
                 : Array{};
      row["return_type"] =
          setter ? Object{{"kind", "void"}, {"name", "Void"}} : p.value;
      row["is_static"] = false;
      row["is_mutating"] = false;
      row["is_mutating_known"] = p.ctx.kind == "class";
      if (p.ctx.kind == "struct") {
        row["requires_self_abi_proof"] = true;
        throw Error("value-type accessor self layout and mutating convention "
                    "require native ABI proof");
      }
      row["status"] = "supported";
      return row;
    }
    if (n->kind == "Destructor" || n->kind == "Deallocator" ||
        n->kind == "TypeMetadataAccessFunction" || n->kind == "ModifyAccessor")
      runtime(row, *n, is_static, ptr);
    bool initializer = n->kind == "Constructor" || n->kind == "Allocator";
    const Node *ctx, *labels, *fn;
    std::string name;
    if (initializer && n->children.size() == 3) {
      ctx = n->children[0].get();
      labels = n->children[1].get();
      fn = n->children[2].get();
      name = "init";
    } else if (n->kind == "Function" && n->children.size() == 4) {
      ctx = n->children[0].get();
      name = ident(*n->children[1]);
      labels = n->children[2].get();
      fn = n->children[3].get();
    } else
      throw Error("Swift symbol is not a plain fixed-signature function or "
                  "initializing constructor");
    auto c = context(*ctx);
    if (is_static && c.kind == "global")
      throw Error("global Swift function has a static marker");
    putContext(row, c);
    row["declaration_kind"] = initializer ? "initializer" : "function";
    row["name"] = name;
    if (initializer && ((c.kind != "class" && c.kind != "struct") || is_static))
      throw Error("unsupported Swift initializer context");
    if (labels->kind != "LabelList" || fn->kind != "Type")
      throw Error("unsupported Swift labels or function type");
    fn = &one(*fn, "FunctionType");
    if (fn->children.size() != 2 || fn->children[0]->kind != "ArgumentTuple" ||
        fn->children[1]->kind != "ReturnType")
      throw Error("async, throwing, generic or other Swift conventions are "
                  "unsupported");
    const auto &arg = one(*fn->children[0], "Type");
    const auto &argvalue = one(arg);
    Array arguments;
    if (argvalue.kind == "Tuple") {
      for (const auto &e : argvalue.children) {
        if (e->kind != "TupleElement")
          throw Error("unsupported Swift argument tuple");
        arguments.push_back(type(one(*e, "Type"), ptr));
      }
    } else
      arguments.push_back(type(arg, ptr));
    for (const auto &v : arguments)
      if (str(object(v, "argument"), "kind") == "void")
        throw Error("Swift function has a void argument");
    Array parameter_labels;
    for (const auto &label : labels->children) {
      if (label->kind == "FirstElementMarker" && label->children.empty() &&
          !label->text)
        parameter_labels.push_back("_");
      else
        parameter_labels.push_back(ident(*label));
    }
    if (parameter_labels.empty())
      for (size_t i = 0; i < arguments.size(); ++i)
        parameter_labels.push_back("_");
    if (parameter_labels.size() != arguments.size())
      throw Error("Swift parameter labels disagree with argument count");
    auto &returned = one(*fn->children[1], "Type");
    Object returntype;
    if (initializer) {
      auto identity =
          nominal(one(returned, c.kind == "class" ? "Class" : "Structure"));
      if (identity != std::pair{c.module, c.name})
        throw Error("Swift initializer return identity disagrees with context");
      returntype =
          c.kind == "class"
              ? Object{{"kind", "pointer"}, {"name", "UnsafeMutableRawPointer"}}
              : Object{{"kind", "nominal"},
                       {"module", c.module},
                       {"name", c.name},
                       {"context_kind", "struct"}};
    } else
      returntype = type(returned, ptr);
    Array params;
    for (size_t i = 0; i < arguments.size(); ++i)
      params.push_back(
          Object{{"name", "arg" + std::to_string(i)}, {"type", arguments[i]}});
    row["labels"] = std::move(parameter_labels);
    row["parameters"] = std::move(params);
    row["return_type"] = std::move(returntype);
    row["is_static"] = is_static;
    row["is_mutating"] = false;
    row["is_mutating_known"] = initializer || c.kind != "struct" || is_static;
    if (n->kind == "Allocator" && c.kind == "class") {
      row["declaration_kind"] = "runtime";
      row["runtime_source_kind"] = "allocating_initializer";
      row["requires_runtime_source_proof"] = true;
      throw Error("Swift allocating constructor requires native allocation and "
                  "initializer effect proof");
    }
    if (initializer && c.kind == "struct") {
      row["requires_storage_abi_proof"] = true;
      throw Error("value-type initializer return layout is not established by "
                  "mangling");
    }
    if (c.kind == "struct" && !is_static) {
      row["requires_self_abi_proof"] = true;
      throw Error("value-type self layout and mutating convention are not "
                  "established by mangling");
    }
    row["status"] = "supported";
  } catch (const Error &e) {
    row["reason"] = std::string(e.what());
  }
  return row;
}
Object swiftSignatures(const Array &symbols, const std::string &demangler,
                       const fs::path &logs, unsigned ptr, Budget &budget) {
  if (symbols.size() > budget.limits.max_files)
    throw Error("Swift symbol inventory exceeds file limit");
  fs::create_directories(logs);
  Array rows, log_names;
  std::vector<std::vector<std::pair<std::string, std::string>>> batches(1);
  size_t command_bytes = 0;
  for (const auto &v : symbols) {
    budget.tick();
    const auto &s = object(v, "Swift symbol");
    auto name = requiredString(s, "name"),
         address = requiredString(s, "address");
    if (!swiftName(name))
      continue;
    if (name.size() > 8000 ||
        name.find_first_of(std::string("\r\n\0", 3)) != name.npos) {
      rows.push_back(Object{
          {"entry", address},
          {"mangled_symbol", name},
          {"status", "unsupported"},
          {"classification", "unknown"},
          {"reason", "Swift symbol exceeds safe demangler input limits"}});
      continue;
    }
    if (command_bytes + name.size() > 12000 || batches.back().size() == 64) {
      batches.emplace_back();
      command_bytes = 0;
    }
    batches.back().emplace_back(name, address);
    command_bytes += name.size() + 3;
  }
  for (size_t i = 0; i < batches.size(); ++i) {
    if (batches[i].empty())
      continue;
    budget.check();
    std::ostringstream filename;
    filename << "swift-demangle-" << std::setw(4) << std::setfill('0') << i
             << ".log";
    auto log = logs / filename.str();
    std::vector<std::string> argv{demangler, "--expand", "--tree-only"};
    for (const auto &[name, address] : batches[i])
      argv.push_back(name);
    runTool(argv, log, toolTimeout(budget), {}, budget.limits);
    auto output = readFile(log, budget.limits.max_bytes);
    log_names.push_back(filename.str());
    size_t p = 0;
    for (const auto &[name, address] : batches[i]) {
      std::string title = "Demangling for " + name + "\n";
      if (output.compare(p, title.size(), title) != 0)
        throw Error("Swift demangler symbol identity disagrees with input");
      p += title.size();
      auto end = output.find("\nDemangling for ", p);
      if (end == output.npos)
        end = output.size();
      rows.push_back(swiftSignature(
          address, name, std::string_view(output).substr(p, end - p), ptr));
      p = end == output.size() ? end : end + 1;
    }
    if (p != output.size())
      throw Error("Swift demangler returned unexpected structured output");
  }
  Array methods, other;
  uint64_t supported = 0, unknown = 0;
  for (auto &v : rows) {
    const auto &r = object(v, "Swift row");
    if (str(r, "classification") == "callable") {
      supported += str(r, "status") == "supported";
      methods.push_back(std::move(v));
    } else {
      unknown += str(r, "classification") == "unknown";
      other.push_back(std::move(v));
    }
  }
  auto count = methods.size(), total = count + other.size();
  return Object{
      {"schema_version", 1},
      {"method_count", count},
      {"symbol_count", total},
      {"unclassified_symbol_count", unknown},
      {"supported_signature_count", supported},
      {"unsupported_signature_count", count - supported},
      {"methods", std::move(methods)},
      {"symbols", std::move(other)},
      {"logs", std::move(log_names)},
      {"limitations",
       Array{"Mangled symbols describe source types, not authenticated ABI or "
             "complete native decoding.",
             "Swift value-type self layout and mutating convention require "
             "evidence beyond a function mangling.",
             "Compiler runtime entries retain their context and require native "
             "effect and recovered source dependency proofs; demangling alone "
             "never recovers their bodies.",
             "Stripped symbols, generic, async, throwing and aggregate "
             "signatures are not recovered by this signature reader.",
             "Pure metadata symbols are excluded from method coverage; "
             "unclassified symbols may conceal additional callable "
             "declarations."}}};
}
} // namespace neverd::mobile::ios
