#include "MobileIOSInternal.h"

#include "llvm/Demangle/SwiftDemangle.h"

#include <algorithm>
#include <regex>

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
using Node = llvm::SwiftDemangleNode;
const Node &one(const Node &n, std::string_view kind = {}) {
  if (n.Children.size() != 1 || (!kind.empty() && n.Children[0].Kind != kind))
    throw Error("unsupported Swift type or declaration shape");
  return n.Children[0];
}
std::string ident(const Node &n, std::string_view kind = "Identifier") {
  if (n.Kind != kind || !n.Children.empty() || !n.Text || !identifier(*n.Text))
    throw Error("unsafe or unsupported Swift declaration identifier");
  return *n.Text;
}
std::pair<std::string, std::string> nominal(const Node &n) {
  if (n.Children.size() != 2)
    throw Error("nested or generic Swift contexts are unsupported");
  return {ident(n.Children[0], "Module"), ident(n.Children[1])};
}
struct Context {
  std::string module, name, kind;
};
Context context(const Node &n) {
  if (n.Kind == "Module")
    return {ident(n, "Module"), "", "global"};
  if (n.Kind == "Class" || n.Kind == "Structure") {
    auto [m, name] = nominal(n);
    return {m, name, n.Kind == "Class" ? "class" : "struct"};
  }
  throw Error("unsupported Swift declaration context");
}
Object type(const Node &n, unsigned pointer_size, unsigned depth = 0) {
  if (depth > 8)
    throw Error("Swift pointer type is too deep");
  if (n.Kind == "Type")
    return type(one(n), pointer_size, depth);
  if (n.Kind == "Tuple" && n.Children.empty())
    return Object{{"kind", "void"}, {"name", "Void"}};
  if (n.Kind == "BoundGenericStructure") {
    if (n.Children.size() != 2 || n.Children[0].Kind != "Type" ||
        n.Children[1].Kind != "TypeList")
      throw Error("unsupported Swift generic type");
    auto [module, name] = nominal(one(n.Children[0], "Structure"));
    if (module != "Swift" ||
        (name != "UnsafePointer" && name != "UnsafeMutablePointer"))
      throw Error(
          "Swift generic types other than ordinary pointers are unsupported");
    auto pointee = type(one(n.Children[1], "Type"), pointer_size, depth + 1);
    if (str(pointee, "kind") == "void")
      throw Error("typed Swift pointer has void pointee");
    return Object{
        {"kind", "pointer"}, {"name", name}, {"pointee", std::move(pointee)}};
  }
  if (n.Kind != "Structure")
    throw Error("unsupported Swift signature type: " + n.Kind);
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
  if (v.Children.size() != 3)
    throw Error("unsupported Swift property shape");
  auto c = context(v.Children[0]);
  if (c.kind == "global" || is_static)
    throw Error(
        "global and static Swift property conventions are not established");
  if (v.Children[2].Kind != "Type")
    throw Error("Swift property type wrapper is missing");
  auto t = type(v.Children[2], ptr);
  if (str(t, "kind") == "void")
    throw Error("Swift scalar property has a void type");
  return {c, ident(v.Children[1]), std::move(t)};
}
void runtime(Object &row, const Node &node, bool is_static, unsigned ptr,
             bool continuation = false) {
  Context c;
  std::string name, kind;
  if (node.Kind == "ModifyAccessor") {
    auto p = property(node, is_static, ptr);
    c = p.ctx;
    name = p.name;
    row["property_type"] = std::move(p.value);
    kind = continuation ? "modify_resume" : "modify_accessor";
  } else {
    if (is_static || continuation)
      throw Error("unsupported Swift runtime wrapper");
    auto *n = &one(node);
    if (node.Kind == "TypeMetadataAccessFunction" && n->Kind == "Type")
      n = &one(*n);
    c = context(*n);
    if (c.kind == "global" ||
        (node.Kind != "TypeMetadataAccessFunction" && c.kind != "class"))
      throw Error("unsupported Swift runtime context");
    kind = node.Kind == "Destructor"    ? "destructor"
           : node.Kind == "Deallocator" ? "deallocator"
                                        : "type_metadata_accessor";
    name =
        node.Kind == "TypeMetadataAccessFunction" ? "typeMetadata" : "deinit";
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
Object swiftDemanglerInfo() {
  return Object{{"name", "llvm-swift-demangle"},
                {"execution", "builtin"},
                {"version", llvm::swiftDemangleVersion()}};
}
Object swiftSignature(std::string_view entry, std::string_view mangled,
                      unsigned ptr) {
  Object row{{"entry", std::string(entry)},
             {"mangled_symbol", std::string(mangled)},
             {"status", "unsupported"},
             {"classification", "unknown"}};
  try {
    if (ptr != 8 || mangled.size() > 8000 || !swiftName(mangled) ||
        !hexAddress(entry))
      throw Error("invalid Swift symbol identity or unsupported pointer size");
    auto demangled = llvm::swiftDemangle(mangled);
    if (!demangled.Root)
      throw Error(demangled.Error);
    const auto &root = demangled.Root;
    if (root->Kind != "Global")
      throw Error("Swift root is not a global symbol");
    if (root->Children.size() == 2 && callables.count(root->Children[0].Kind) &&
        root->Children[1].Kind == "Suffix" &&
        root->Children[1].Children.empty() && root->Children[1].Text &&
        std::regex_match(*root->Children[1].Text,
                         std::regex("\\.resume\\.[0-9]+"))) {
      row["classification"] = "callable";
      row["node_kind"] = "CoroutineContinuation";
      row["continuation_of"] = root->Children[0].Kind;
      row["compiler_suffix"] = *root->Children[1].Text;
      if (root->Children[0].Kind == "ModifyAccessor")
        runtime(row, root->Children[0], false, ptr, true);
      throw Error("Swift coroutine continuation requires suspended-frame "
                  "calling convention");
    }
    auto *n = &one(*root);
    bool is_static = n->Kind == "Static";
    if (is_static)
      n = &one(*n);
    row["node_kind"] = n->Kind;
    row["classification"] = callables.count(n->Kind)  ? "callable"
                            : metadata.count(n->Kind) ? "metadata"
                                                      : "unknown";
    if (n->Kind == "Getter" || n->Kind == "Setter") {
      auto p = property(*n, is_static, ptr);
      putContext(row, p.ctx);
      bool setter = n->Kind == "Setter";
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
    if (n->Kind == "Destructor" || n->Kind == "Deallocator" ||
        n->Kind == "TypeMetadataAccessFunction" || n->Kind == "ModifyAccessor")
      runtime(row, *n, is_static, ptr);
    bool initializer = n->Kind == "Constructor" || n->Kind == "Allocator";
    const Node *ctx, *labels = nullptr, *fn;
    std::string name;
    if (initializer && (n->Children.size() == 2 || n->Children.size() == 3)) {
      ctx = &n->Children[0];
      if (n->Children.size() == 3)
        labels = &n->Children[1];
      fn = &n->Children.back();
      name = "init";
    } else if (n->Kind == "Function" &&
               (n->Children.size() == 3 || n->Children.size() == 4)) {
      ctx = &n->Children[0];
      name = ident(n->Children[1]);
      if (n->Children.size() == 4)
        labels = &n->Children[2];
      fn = &n->Children.back();
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
    if ((labels && labels->Kind != "LabelList") || fn->Kind != "Type")
      throw Error("unsupported Swift labels or function type");
    fn = &one(*fn, "FunctionType");
    if (fn->Children.size() != 2 || fn->Children[0].Kind != "ArgumentTuple" ||
        fn->Children[1].Kind != "ReturnType")
      throw Error("async, throwing, generic or other Swift conventions are "
                  "unsupported");
    const auto &arg = one(fn->Children[0], "Type");
    const auto &argvalue = one(arg);
    Array arguments;
    if (argvalue.Kind == "Tuple") {
      for (const auto &e : argvalue.Children) {
        if (e.Kind != "TupleElement")
          throw Error("unsupported Swift argument tuple");
        arguments.push_back(type(one(e, "Type"), ptr));
      }
    } else
      arguments.push_back(type(arg, ptr));
    for (const auto &v : arguments)
      if (str(object(v, "argument"), "kind") == "void")
        throw Error("Swift function has a void argument");
    // The mangling omits LabelList for a genuinely empty argument tuple.
    // Its absence supplies no label evidence for a function taking arguments.
    if (!labels && !arguments.empty())
      throw Error("Swift parameter labels are missing for nonempty arguments");
    Array parameter_labels;
    if (labels)
      for (const auto &label : labels->Children) {
        if (label.Kind == "FirstElementMarker" && label.Children.empty() &&
            !label.Text)
          parameter_labels.push_back("_");
        else
          parameter_labels.push_back(ident(label));
      }
    if (parameter_labels.empty())
      for (size_t i = 0; i < arguments.size(); ++i)
        parameter_labels.push_back("_");
    if (parameter_labels.size() != arguments.size())
      throw Error("Swift parameter labels disagree with argument count");
    auto &returned = one(fn->Children[1], "Type");
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
    if (n->Kind == "Allocator" && c.kind == "class") {
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
Object swiftSignatures(const Array &symbols, unsigned ptr, Budget &budget) {
  if (symbols.size() > budget.limits.max_files)
    throw Error("Swift symbol inventory exceeds file limit");
  Array rows;
  uint64_t input_bytes = 0;
  for (const auto &v : symbols) {
    budget.tick();
    const auto &s = object(v, "Swift symbol");
    auto name = requiredString(s, "name"),
         address = requiredString(s, "address");
    if (!swiftName(name))
      continue;
    if (name.size() > budget.limits.max_bytes - input_bytes)
      throw Error("Swift symbol inventory exceeds byte limit");
    input_bytes += name.size();
    rows.push_back(swiftSignature(address, name, ptr));
    budget.check();
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
      {"demangler", swiftDemanglerInfo()},
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
