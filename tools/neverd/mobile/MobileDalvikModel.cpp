//===- MobileDalvikModel.cpp - Shared Dalvik declaration validation
//--------===//
#include "MobileDalvik.h"
#include "MobileDalvikSignature.h"

#include <algorithm>
#include <array>
#include <deque>
#include <iterator>
#include <limits>

namespace neverd::mobile::dalvik {
bool has(const Access &access, std::string_view flag) {
  return access.contains(std::string(flag));
}

const Method *lookupMethod(const MethodRef &Reference, const ClassMap &Classes,
                           Budget &B, const GenericSignaturePlans *Signatures) {
  if (!Classes.contains(Reference.owner))
    return nullptr;
  const bool ClassMethod =
      Reference.name != "<init>" &&
      !has(Classes.at(Reference.owner).access, "interface");
  // A class declaration anywhere on the superclass chain precedes every
  // interface declaration. Reuse the same exact-descriptor/bridge checks in
  // both stages; the fallback retains the original interface traversal.
  // Constructors and fields keep their separate existing lookup policy.
  for (bool ClassOnly : {true, false}) {
    if (ClassOnly && !ClassMethod)
      continue;
    std::deque<std::string> Pending{Reference.owner};
    std::set<std::string> Seen;
    while (!Pending.empty()) {
      B.tick();
      std::string Owner = std::move(Pending.front());
      Pending.pop_front();
      auto I = Classes.find(Owner);
      if (I == Classes.end() || !Seen.insert(Owner).second)
        continue;
      const auto &C = I->second;
      bool MethodTypeMismatch = false;
      for (const auto &M : C.methods) {
        const auto &Actual = M.reference;
        if (Actual.name != Reference.name ||
            Actual.parameters != Reference.parameters)
          continue;
        if (Actual.returns != Reference.returns) {
          bool InheritedCheckedResult = false;
          if (Signatures && C.superclass) {
            auto Proof =
                Signatures->inherited_checked_result_bridges.lower_bound(
                    {Actual, MethodRef{}});
            for (;
                 Proof != Signatures->inherited_checked_result_bridges.end() &&
                 Proof->first.first == Actual;
                 ++Proof) {
              B.tick();
              auto Target = Reference;
              Target.owner = *C.superclass;
              InheritedCheckedResult |= Proof->second.target == Target;
            }
          }
          // Only this original compiler bridge is absent from Java source.
          // Its proved immediate-parent target still owns the broad runtime
          // entry. An ordinary return-type competitor retains the fail-loud
          // source-dispatch guard below.
          MethodTypeMismatch |= !InheritedCheckedResult;
          continue;
        }
        return &M;
      }
      // Match the complete runtime descriptor before rejecting a Java slot.
      // An absent local descriptor still needs a separate inherited override
      // proof; a different return type must not invent source dispatch.
      if (MethodTypeMismatch)
        throw std::runtime_error(
            Reference.owner +
            ": member type differs from the exact referenced declaration");
      if (Reference.name == "<init>")
        break;
      if (C.superclass)
        Pending.push_back(*C.superclass);
      if (!ClassOnly)
        for (const auto &Interface : C.interfaces)
          Pending.push_back(Interface);
    }
  }
  throw std::runtime_error(
      Reference.owner + ": referenced member has no proven local declaration");
}

FieldLookup lookupField(const FieldRef &reference, const ClassMap &classes,
                        Budget &budget) {
  if (!classes.contains(reference.owner))
    return {FieldLookupStatus::UnknownOwner, nullptr};
  std::deque<std::string> pending{reference.owner};
  std::set<std::string> seen;
  while (!pending.empty()) {
    budget.tick();
    auto owner = std::move(pending.front());
    pending.pop_front();
    const auto found = classes.find(owner);
    if (found == classes.end() || !seen.insert(owner).second)
      continue;
    const auto &cls = found->second;
    for (const auto &field : cls.fields) {
      if (field.reference.name != reference.name)
        continue;
      if (field.reference.type != reference.type)
        return {FieldLookupStatus::TypeMismatch, nullptr};
      return {FieldLookupStatus::Found, &field};
    }
    if (cls.superclass)
      pending.push_back(*cls.superclass);
    for (const auto &interface : cls.interfaces)
      pending.push_back(interface);
  }
  return {FieldLookupStatus::Missing, nullptr};
}

bool emptyNop(const Instruction &op) {
  return op.opcode == "nop" && op.registers.empty() && !op.target &&
         op.targets.empty() && op.keys.empty() && op.data.empty() &&
         !op.element_width &&
         std::holds_alternative<std::monostate>(op.reference) &&
         std::holds_alternative<std::monostate>(op.literal);
}

// DEX encoded arrays may explicitly spell the VM preparation default for a
// field which the original clinit subsequently assigns. Preserve the typed
// bit pattern: negative floating zero and every nonzero value are not defaults.
bool defaultFieldValue(const Field &F) {
  if (std::holds_alternative<std::monostate>(F.value))
    return true;
  if (const auto *Value = std::get_if<int64_t>(&F.value))
    return *Value == 0 && (F.reference.type == "B" || F.reference.type == "C" ||
                           F.reference.type == "S" || F.reference.type == "I" ||
                           F.reference.type == "J");
  if (const auto *Value = std::get_if<bool>(&F.value))
    return F.reference.type == "Z" && !*Value;
  if (const auto *Value = std::get_if<FloatBits>(&F.value))
    return Value->bits == 0 && F.reference.type == (Value->wide ? "D" : "F");
  return false;
}

// These exact Dalvik conversions are total local operations. The ordinary
// verifier, allocation graphs and constructor delegation planner share one
// source/destination type receipt. Body owns values and raw floating storage.
std::optional<std::pair<std::string, std::string>>
scalarConversion(std::string_view Opcode) {
  static const std::map<std::string_view, std::pair<std::string, std::string>>
      Conversions{
          {"int-to-long", {"I", "J"}},     {"int-to-float", {"I", "F"}},
          {"int-to-double", {"I", "D"}},   {"long-to-int", {"J", "I"}},
          {"long-to-float", {"J", "F"}},   {"long-to-double", {"J", "D"}},
          {"float-to-int", {"F", "I"}},    {"float-to-long", {"F", "J"}},
          {"float-to-double", {"F", "D"}}, {"double-to-int", {"D", "I"}},
          {"double-to-long", {"D", "J"}},  {"double-to-float", {"D", "F"}},
          {"int-to-byte", {"I", "B"}},     {"int-to-char", {"I", "C"}},
          {"int-to-short", {"I", "S"}}};
  const auto Found = Conversions.find(Opcode);
  return Found == Conversions.end() ? std::nullopt
                                    : std::optional(Found->second);
}

// Unary operations retain the ordinary primitive carrier and never throw.
// Allocation planners and statement emission share this exact type/symbol map.
std::optional<std::pair<std::string, std::string>>
scalarUnary(std::string_view Opcode) {
  static const std::map<std::string_view, std::pair<std::string, std::string>>
      Operations{{"neg-int", {"I", "-"}},   {"neg-long", {"J", "-"}},
                 {"neg-float", {"F", "-"}}, {"neg-double", {"D", "-"}},
                 {"not-int", {"I", "~"}},   {"not-long", {"J", "~"}}};
  const auto Found = Operations.find(Opcode);
  return Found == Operations.end() ? std::nullopt
                                   : std::optional(Found->second);
}

std::optional<ScalarComparison> scalarComparison(std::string_view opcode) {
  static const std::map<std::string_view, ScalarComparison> operations{
      {"cmp-long", {"J", false}},
      {"cmpl-float", {"F", false}},
      {"cmpg-float", {"F", true}},
      {"cmpl-double", {"D", false}},
      {"cmpg-double", {"D", true}}};
  const auto found = operations.find(opcode);
  return found == operations.end() ? std::nullopt
                                   : std::optional(found->second);
}

// Floating division and remainder are total, unlike their integer forms.
// Statements and allocation receipts use the same exact opcode family.
std::optional<FloatingArithmetic> floatingArithmetic(std::string_view Opcode) {
  const bool TwoAddress = Opcode.ends_with("/2addr");
  if (TwoAddress)
    Opcode.remove_suffix(6);
  static const std::map<std::string_view, std::pair<std::string, std::string>>
      Operations{{"add-float", {"F", "+"}},  {"sub-float", {"F", "-"}},
                 {"mul-float", {"F", "*"}},  {"div-float", {"F", "/"}},
                 {"rem-float", {"F", "%"}},  {"add-double", {"D", "+"}},
                 {"sub-double", {"D", "-"}}, {"mul-double", {"D", "*"}},
                 {"div-double", {"D", "/"}}, {"rem-double", {"D", "%"}}};
  const auto Found = Operations.find(Opcode);
  if (Found == Operations.end())
    return {};
  return FloatingArithmetic{Found->second.first, Found->second.second,
                            TwoAddress};
}

// Statements and allocation proofs share exact long operations. Both runtimes
// wrap arithmetic at 64 bits, including MIN_VALUE / -1; shifts use the low six
// bits of their int distance. Division/remainder can throw at a zero divisor,
// so total-only constructor and conditional regions must check that fact.
std::optional<LongBinary> longBinary(std::string_view Opcode) {
  const bool TwoAddress = Opcode.ends_with("/2addr");
  if (TwoAddress)
    Opcode.remove_suffix(6);
  static const std::map<std::string_view, std::pair<std::string, std::string>>
      Operations{{"add-long", {"+", "J"}},   {"sub-long", {"-", "J"}},
                 {"mul-long", {"*", "J"}},   {"and-long", {"&", "J"}},
                 {"div-long", {"/", "J"}},   {"rem-long", {"%", "J"}},
                 {"or-long", {"|", "J"}},    {"xor-long", {"^", "J"}},
                 {"shl-long", {"<<", "I"}},  {"shr-long", {">>", "I"}},
                 {"ushr-long", {">>>", "I"}}};
  const auto Found = Operations.find(Opcode);
  if (Found == Operations.end())
    return {};
  return LongBinary{Found->second.first, Found->second.second, TwoAddress,
                    Opcode == "div-long" || Opcode == "rem-long"};
}

std::optional<ConditionalBranch> conditionalBranch(std::string_view opcode) {
  const bool zero = opcode.ends_with('z');
  if (zero)
    opcode.remove_suffix(1);
  static const std::map<std::string_view, std::string_view> relations{
      {"if-eq", "=="}, {"if-ne", "!="}, {"if-lt", "<"},
      {"if-ge", ">="}, {"if-gt", ">"},  {"if-le", "<="}};
  const auto found = relations.find(opcode);
  if (found == relations.end())
    return {};
  return ConditionalBranch{std::string(found->second), zero};
}

std::optional<IntegerPreparation> integerPreparation(std::string_view opcode) {
  if (opcode == "rsub-int" || opcode == "rsub-int/lit8")
    return IntegerPreparation{"-", false, true, true};
  const bool two_address = opcode.ends_with("/2addr");
  const bool literal = opcode.ends_with("/lit8") || opcode.ends_with("/lit16");
  const bool wide_literal = opcode.ends_with("/lit16");
  if (two_address || opcode.ends_with("/lit16"))
    opcode.remove_suffix(6);
  else if (literal)
    opcode.remove_suffix(5);
  static const std::map<std::string_view, std::string_view> operations{
      {"add-int", "+"},  {"sub-int", "-"},  {"mul-int", "*"},
      {"and-int", "&"},  {"or-int", "|"},   {"xor-int", "^"},
      {"shl-int", "<<"}, {"shr-int", ">>"}, {"ushr-int", ">>>"}};
  const auto found = operations.find(opcode);
  if (found == operations.end() || (opcode == "sub-int" && literal) ||
      (wide_literal &&
       (opcode == "shl-int" || opcode == "shr-int" || opcode == "ushr-int")))
    return {};
  return IntegerPreparation{std::string(found->second), two_address, literal};
}

std::optional<IntegerDivision> integerDivision(const Instruction &op) {
  static const std::set<std::string_view> forms{
      "div-int",      "rem-int",      "div-int/2addr", "rem-int/2addr",
      "div-int/lit8", "rem-int/lit8", "div-int/lit16", "rem-int/lit16"};
  if (!forms.contains(op.opcode))
    return {};
  const bool two_address = op.opcode.ends_with("/2addr");
  const bool literal =
      op.opcode.ends_with("/lit8") || op.opcode.ends_with("/lit16");
  if (op.registers.size() != (two_address || literal ? 2u : 3u) || op.target ||
      !op.targets.empty() || !op.keys.empty() || !op.data.empty() ||
      op.element_width || !std::holds_alternative<std::monostate>(op.reference))
    return {};
  const auto *divisor = std::get_if<int64_t>(&op.literal);
  if (literal) {
    const int64_t limit = op.opcode.ends_with("/lit8") ? 128 : 32768;
    if (!divisor || *divisor < -limit || *divisor >= limit)
      return {};
  } else if (!std::holds_alternative<std::monostate>(op.literal)) {
    return {};
  }
  return IntegerDivision{op.opcode.starts_with("div-") ? "/" : "%", two_address,
                         literal, !literal || *divisor == 0};
}

std::optional<IntegerPreparation> integerPreparation(const Instruction &op) {
  if (auto total = integerPreparation(op.opcode)) {
    if (total->literal_first || total->symbol == "<<" || total->symbol == ">>" ||
        total->symbol == ">>>") {
      const auto *literal = std::get_if<int64_t>(&op.literal);
      const int64_t limit = op.opcode == "rsub-int" ? 32768 : 128;
      if (op.registers.size() !=
              (total->two_address || total->literal ? 2u : 3u) ||
          op.target || !op.targets.empty() || !op.keys.empty() ||
          !op.data.empty() || op.element_width ||
          !std::holds_alternative<std::monostate>(op.reference) ||
          (total->literal
               ? !literal || *literal < -limit || *literal >= limit
               : !std::holds_alternative<std::monostate>(op.literal)))
        return {};
      // Java and Dalvik int shifts consume the low five distance bits. Keep
      // both operands as int, including signed literal distances; the same
      // receipt serves ordinary statements and every preparation consumer.
    }
    return total;
  }
  const auto division = integerDivision(op);
  if (!division || division->can_throw)
    return {};
  return IntegerPreparation{division->symbol, division->two_address,
                            division->literal};
}

// Scalar ownership proofs inspect every physical operand word before Body
// checks values. In a comparison, only the two inputs can occupy wide pairs;
// an implicit high word may not hide an uninitialized this.
template <typename Forbidden>
static bool scalarWordsDisjoint(const Instruction &op, unsigned registers,
                                Forbidden forbidden) {
  const auto conversion = scalarConversion(op.opcode);
  const auto comparison = scalarComparison(op.opcode);
  const auto floating = floatingArithmetic(op.opcode);
  const auto binary = longBinary(op.opcode);
  const auto unary = scalarUnary(op.opcode);
  if (!conversion && !comparison && !floating && !binary && !unary)
    return true;
  const unsigned arity = binary       ? (binary->two_address ? 2 : 3)
                         : floating   ? (floating->two_address ? 2 : 3)
                         : comparison ? 3
                                      : 2;
  if (op.registers.size() != arity || op.target || !op.targets.empty() ||
      !op.keys.empty() || !op.data.empty() || op.element_width ||
      !std::holds_alternative<std::monostate>(op.literal) ||
      !std::holds_alternative<std::monostate>(op.reference))
    return false;
  std::array<unsigned, 3> words;
  if (binary)
    words = binary->two_address
                ? std::array<unsigned, 3>{2, width(binary->right_type), 0}
                : std::array<unsigned, 3>{2, 2, width(binary->right_type)};
  else if (unary)
    words.fill(width(unary->first));
  else if (floating)
    words.fill(width(floating->type));
  else if (comparison)
    words = {1, width(comparison->type), width(comparison->type)};
  else
    words = {width(conversion->second), width(conversion->first), 0};
  for (unsigned operand = 0; operand < arity; ++operand) {
    const auto start = op.registers[operand];
    if (start >= registers || words[operand] > registers - start)
      return false;
    for (unsigned word = 0; word < words[operand]; ++word)
      if (forbidden(start + word))
        return false;
  }
  return true;
}

// Constructor and allocation planning share the same exact Dalvik load family.
// Body owns the element descriptor and original array/index checks.
bool arrayLoad(std::string_view opcode) {
  return opcode == "aget" || opcode == "aget-object" || opcode == "aget-wide" ||
         opcode == "aget-boolean" || opcode == "aget-byte" ||
         opcode == "aget-char" || opcode == "aget-short";
}

bool isAssignableReference(const std::string &actual, const std::string &wanted,
                           const ClassMap &classes, Budget &budget) {
  auto reference = [](std::string_view type) {
    return type.starts_with('L') || type.starts_with('[');
  };
  if (!reference(actual) || !reference(wanted))
    return false;
  std::string_view source = actual, target = wanted;
  while (true) {
    budget.tick();
    if (source == target)
      return true;
    // Each array is an Object, Cloneable and Serializable. An array component
    // is covariant only if it is itself a reference; primitive components must
    // match exactly. Peel ranks iteratively so every consumer shares the same
    // rule without a recursive proof or synthetic array declarations.
    if (!reference(source) || !reference(target))
      return false;
    if (target == "Ljava/lang/Object;")
      return true;
    if (!source.starts_with('['))
      break;
    if (target == "Ljava/lang/Cloneable;" || target == "Ljava/io/Serializable;")
      return true;
    if (!target.starts_with('['))
      return false;
    source.remove_prefix(1);
    target.remove_prefix(1);
  }
  if (target.starts_with('['))
    return false;
  std::vector<std::string> pending{std::string(source)};
  std::set<std::string> seen;
  while (!pending.empty()) {
    budget.tick();
    auto type = std::move(pending.back());
    pending.pop_back();
    if (type == target)
      return true;
    if (!seen.insert(type).second)
      continue;
    auto found = classes.find(type);
    if (found == classes.end())
      continue;
    const auto &cls = found->second;
    if (cls.superclass)
      pending.push_back(*cls.superclass);
    pending.insert(pending.end(), cls.interfaces.begin(), cls.interfaces.end());
  }
  return false;
}

unsigned annotationTargetJavaRelease(std::string_view target) {
  if (target == "MODULE")
    return 9;
  if (target == "RECORD_COMPONENT")
    return 16;
  constexpr std::array<std::string_view, 10> java8{
      "TYPE",           "FIELD",          "METHOD",          "PARAMETER",
      "CONSTRUCTOR",    "LOCAL_VARIABLE", "ANNOTATION_TYPE", "PACKAGE",
      "TYPE_PARAMETER", "TYPE_USE"};
  return std::find(java8.begin(), java8.end(), target) != java8.end() ? 8 : 0;
}

bool isAnonymous(const Class &cls) {
  return !cls.external && cls.inner_class_present && !cls.inner_name;
}
bool isAnonymousLocal(const Class &cls) {
  return isAnonymous(cls) && cls.enclosing_method.has_value();
}
bool isConstructorlessAnonymousMember(const Class &cls) {
  return isAnonymous(cls) && cls.enclosing && !cls.enclosing_method &&
         has(cls.access, "synthetic") &&
         cls.superclass == "Ljava/lang/Object;" && cls.interfaces.empty() &&
         !cls.generic_signature &&
         std::all_of(
             cls.fields.begin(), cls.fields.end(),
             [](const Field &field) { return has(field.access, "static"); }) &&
         std::all_of(cls.methods.begin(), cls.methods.end(),
                     [](const Method &method) {
                       return has(method.access, "static") &&
                              method.reference.name != "<init>";
                     });
}
NamedAnonymousMembers planNamedAnonymousMembers(const ClassMap &classes,
                                                Budget &budget) {
  NamedAnonymousMembers candidates, result;
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (!isAnonymous(cls) || has(cls.access, "enum") || !cls.enclosing ||
        cls.enclosing_method || isEnumConstantClass(cls, classes))
      continue;
    candidates.insert(name);
    if (!cls.fields.empty() || !cls.annotations.empty() ||
        !cls.marker_annotations.empty() || cls.deprecated ||
        cls.suppress_lint || isConstructorlessAnonymousMember(cls) ||
        (cls.superclass && *cls.superclass != "Ljava/lang/Object;"))
      result.insert(name);
  }
  if (candidates.empty())
    return result;
  auto use = [&](const std::string &type) {
    budget.tick();
    const auto start = type.find_first_not_of('[');
    if (start != type.npos && candidates.contains(type.substr(start)))
      result.insert(type.substr(start));
  };
  // A stateless initializer body may also be a constructor access marker or
  // another declared type. Keep one named identity for all such uses instead
  // of emitting an anonymous body that its declarations cannot reference.
  for (const auto &[name, cls] : classes) {
    if (cls.external)
      continue;
    if (cls.superclass)
      use(*cls.superclass);
    for (const auto &type : cls.interfaces)
      use(type);
    for (const auto &field : cls.fields)
      use(field.reference.type);
    for (const auto &method : cls.methods) {
      use(method.reference.returns);
      for (const auto &type : method.reference.parameters)
        use(type);
      if (method.declared_throws)
        for (const auto &type : *method.declared_throws)
          use(type);
    }
  }
  return result;
}
bool isNamedAnonymousMember(const Class &cls,
                            const NamedAnonymousMembers &members) {
  return members.contains(cls.name);
}
bool isStaticOnlyInnerMember(const Class &cls) {
  return !cls.external && cls.enclosing && cls.inner_class_present &&
         cls.inner_name && !cls.enclosing_method &&
         !has(cls.inner_access, "static") && !has(cls.access, "interface") &&
         !has(cls.access, "annotation") && !has(cls.access, "enum") &&
         !cls.generic_signature && cls.superclass == "Ljava/lang/Object;" &&
         cls.interfaces.empty() && !cls.methods.empty() &&
         std::all_of(
             cls.fields.begin(), cls.fields.end(),
             [](const Field &field) { return has(field.access, "static"); }) &&
         std::all_of(cls.methods.begin(), cls.methods.end(),
                     [](const Method &method) {
                       return has(method.access, "static") &&
                              method.reference.name != "<init>";
                     });
}

static bool constructorAllocationRegister(const Method &method, unsigned reg) {
  if (reg >= method.registers)
    return false;
  const unsigned self = method.registers - method.incomingWords();
  // Register types belong to the current program point. A completed argument
  // allocation may free either word of an incoming primitive for a new value.
  // The shared this-origin proof protects every live self alias; ordinary Body
  // verification still rejects any later broken pair or incompatible read.
  return reg != self;
}

std::optional<unsigned> constructorPrefixFilledArray(const Method &method,
                                                     size_t instruction) {
  if (method.reference.name != "<init>" || has(method.access, "static") ||
      method.registers > 1024 || method.registers < method.incomingWords() ||
      instruction >= method.instructions.size() ||
      method.instructions.size() - instruction < 2)
    return {};
  const auto &array = method.instructions[instruction];
  const auto &result = method.instructions[instruction + 1];
  const auto *type = std::get_if<std::string>(&array.reference);
  if ((array.opcode != "filled-new-array" &&
       array.opcode != "filled-new-array/range") ||
      !type ||
      (*type != "[I" && !type->starts_with("[L") && !type->starts_with("[[")) ||
      result.opcode != "move-result-object" || result.registers.size() != 1 ||
      !constructorAllocationRegister(method, result.registers[0]) ||
      array.pc >= result.pc || result.pc >= method.code_end ||
      !std::holds_alternative<std::monostate>(result.reference) ||
      std::any_of(array.registers.begin(), array.registers.end(),
                  [&](unsigned r) { return r >= method.registers; }))
    return {};
  for (const auto *op : {&array, &result})
    if (op->target || !op->targets.empty() || !op->keys.empty() ||
        !op->data.empty() || op->element_width ||
        !std::holds_alternative<std::monostate>(op->literal))
      return {};
  return result.registers[0];
}

std::optional<MethodRef> constructorPrefixAllocation(const Method &method,
                                                     size_t instruction) {
  if (method.reference.name != "<init>" || has(method.access, "static") ||
      method.registers > 1024 || method.registers < method.incomingWords() ||
      instruction >= method.instructions.size() ||
      method.instructions.size() - instruction < 2)
    return {};
  const auto &allocation = method.instructions[instruction];
  const auto &initialization = method.instructions[instruction + 1];
  const auto *type = std::get_if<std::string>(&allocation.reference);
  const auto *call = std::get_if<MethodRef>(&initialization.reference);
  if (allocation.opcode != "new-instance" || !type || !type->starts_with('L') ||
      allocation.registers.size() != 1 ||
      !constructorAllocationRegister(method, allocation.registers[0]) ||
      (initialization.opcode != "invoke-direct" &&
       initialization.opcode != "invoke-direct/range") ||
      !call || call->owner != *type || call->name != "<init>" ||
      call->returns != "V" || initialization.registers.empty() ||
      initialization.registers.front() != allocation.registers.front() ||
      allocation.pc >= initialization.pc ||
      initialization.pc >= method.code_end)
    return {};
  size_t cursor = 1;
  const unsigned self = method.registers - method.incomingWords();
  for (const auto &parameter : call->parameters) {
    const unsigned words = width(parameter);
    if (cursor + words > initialization.registers.size())
      return {};
    for (unsigned part = 0; part < words; ++part) {
      const unsigned reg = initialization.registers[cursor + part];
      if (reg >= method.registers || reg == self ||
          reg == allocation.registers.front() ||
          reg != initialization.registers[cursor] + part)
        return {};
    }
    cursor += words;
  }
  if (cursor != initialization.registers.size())
    return {};
  for (const auto *op : {&allocation, &initialization})
    if (op->target || !op->targets.empty() || !op->keys.empty() ||
        !op->data.empty() ||
        !std::holds_alternative<std::monostate>(op->literal))
      return {};
  return *call;
}

std::optional<size_t> constructorPrefixAllocationEnd(const Method &method,
                                                     size_t instruction,
                                                     Budget &budget) {
  if (method.reference.name != "<init>" || has(method.access, "static") ||
      method.registers > 1024 || method.registers < method.incomingWords() ||
      instruction >= method.instructions.size() ||
      (!method.tries.empty() && !constructorPrefixAllocation(method, instruction)))
    return {};
  const auto &code = method.instructions;
  const auto &root = code[instruction];
  const auto *type = std::get_if<std::string>(&root.reference);
  if (root.opcode != "new-instance" || !type || !type->starts_with('L') ||
      root.registers.size() != 1 ||
      !constructorAllocationRegister(method, root.registers[0]))
    return {};
  const auto receiver = root.registers[0];
  std::map<unsigned, size_t> pending{{receiver, instruction}};
  bool nested = false, branched = false;
  for (size_t end = instruction + 1;
       end < code.size() && end - instruction <= 256; ++end) {
    budget.tick();
    const auto &op = code[end];
    const auto kind = op.opcode.substr(0, op.opcode.find('/'));
    const auto binary = longBinary(op.opcode);
    if (binary && binary->can_throw)
      return {};
    const bool branch = conditionalBranch(kind) || kind == "goto";
    if (branch != op.target.has_value() || !op.targets.empty() ||
        !op.keys.empty() || !op.data.empty() || op.element_width ||
        op.opcode.starts_with("return") || op.opcode == "throw")
      return {};
    // Nested allocations use the ordinary linear allocation graph. Keep
    // conditional regions in their existing separate proof domain.
    if (branch) {
      if (nested)
        return {};
      branched = true;
    }
    if (op.opcode == "new-instance") {
      const auto *allocated = std::get_if<std::string>(&op.reference);
      if (branched || !allocated || !allocated->starts_with('L') ||
          op.registers.size() != 1 ||
          !constructorAllocationRegister(method, op.registers[0]) ||
          !pending.emplace(op.registers[0], end).second)
        return {};
      nested = true;
      continue;
    }
    const auto *call = std::get_if<MethodRef>(&op.reference);
    if (call && call->name == "<init>") {
      if (op.registers.empty())
        return {};
      const auto allocation = pending.find(op.registers[0]);
      if (allocation == pending.end() ||
          std::any_of(op.registers.begin() + 1, op.registers.end(),
                      [&](unsigned r) { return pending.contains(r); }))
        return {};
      // Every initializer consumes its original allocation and exact physical
      // words. The common Body graph still proves types, declarations and the
      // complete evaluation/class-initialization order.
      Method pair = method;
      pair.instructions = {code[allocation->second], op};
      if (!constructorPrefixAllocation(pair, 0))
        return {};
      if (op.registers[0] != receiver) {
        pending.erase(allocation);
        continue;
      }
      if (pending.size() != 1)
        return {};
      // Prove the closed forward CFG independently of operation semantics.
      // Every physical node is reached, no path bypasses this initializer,
      // and no outside edge can enter the uninitialized argument region.
      std::map<uint32_t, size_t> positions;
      for (size_t at = instruction; at <= end; ++at) {
        budget.tick();
        if ((at > instruction && code[at - 1].pc >= code[at].pc) ||
            code[at].pc >= method.code_end ||
            !positions.emplace(code[at].pc, at).second)
          return {};
      }
      std::set<size_t> reached{instruction};
      for (size_t at = instruction; at < end; ++at) {
        budget.tick();
        if (!reached.contains(at))
          return {};
        const auto &step = code[at];
        const auto step_kind = step.opcode.substr(0, step.opcode.find('/'));
        if (step.target) {
          const auto target = positions.find(*step.target);
          if (target == positions.end() || target->second <= at)
            return {};
          reached.insert(target->second);
        }
        if (step_kind != "goto")
          reached.insert(at + 1);
      }
      if (!reached.contains(end))
        return {};
      auto enters = [&](uint32_t pc) { return pc > root.pc && pc <= op.pc; };
      for (size_t at = 0; at < code.size(); ++at) {
        budget.tick();
        if (at >= instruction && at <= end)
          continue;
        if ((code[at].target && enters(*code[at].target)) ||
            std::any_of(code[at].targets.begin(), code[at].targets.end(),
                        enters))
          return {};
      }
      for (const auto &region : method.tries) {
        budget.tick();
        if (region.start <= op.pc ||
            std::any_of(region.handlers.begin(), region.handlers.end(),
                        [&](const auto &handler) { return handler.target <= op.pc; }))
          return {};
      }
      return end;
    }
    // Neither a read nor a write may name any live uninitialized receiver;
    // aliases and overlapping wide words need a separate receipt.
    if (std::any_of(op.registers.begin(), op.registers.end(),
                    [&](unsigned r) { return pending.contains(r); }) ||
        !scalarWordsDisjoint(op, method.registers,
                             [&](unsigned r) { return pending.contains(r); }))
      return {};
    if ((kind == "const-wide" || kind == "move-wide" ||
         kind == "move-result-wide" || kind == "sget-wide") &&
        std::any_of(op.registers.begin(), op.registers.end(),
                    [&](unsigned r) { return pending.contains(r + 1); }))
      return {};
  }
  return {};
}

std::optional<ConstructorPrefixDelegation>
selectConstructorPrefixDelegation(const Method &method, Budget &budget) {
  if (method.reference.name != "<init>" || has(method.access, "static") ||
      method.instructions.empty())
    return {};
  const auto &code = method.instructions;
  ConstructorPrefixDelegation result{};
  const size_t limit = std::min<size_t>(code.size(), 257);
  for (size_t at = 0; at < limit; ++at) {
    budget.tick();
    if (code[at].opcode == "new-instance") {
      const auto end = constructorPrefixAllocationEnd(method, at, budget);
      if (!end || *end >= limit)
        return {};
      result.allocations.emplace(at, *end);
      result.requires_dispatch |=
          !constructorPrefixAllocationArgumentsEnd(method, at, budget);
      for (size_t member = at; member <= *end; ++member) {
        budget.tick();
        result.requires_dispatch |= code[member].target.has_value();
      }
      at = *end;
      continue;
    }
    const auto *call = std::get_if<MethodRef>(&code[at].reference);
    if (call && call->name == "<init>") {
      result.call = at;
      return result;
    }
    const auto division = integerDivision(code[at]);
    // Type tests and array lengths use the ordinary instruction verifier even
    // without a branch. Keep their scalar words and original failure order
    // under the same prefix interpreter as conditional constructor arguments.
    result.requires_dispatch |=
        code[at].target.has_value() || code[at].opcode == "new-array" ||
        code[at].opcode == "aput-object" || code[at].opcode == "instance-of" ||
        code[at].opcode == "array-length" || arrayLoad(code[at].opcode) ||
        (division && division->can_throw);
  }
  return {};
}

std::optional<std::string>
constructorPrefixClassLiteral(const Method &method, const Instruction &op) {
  const auto *type = std::get_if<std::string>(&op.reference);
  if (method.reference.name != "<init>" || has(method.access, "static") ||
      method.registers > 1024 || method.registers < method.incomingWords() ||
      op.opcode != "const-class" || !type ||
      (!type->starts_with('L') && !type->starts_with('[')) ||
      op.registers.size() != 1 ||
      op.registers[0] >= method.registers - method.incomingWords() ||
      op.target || !op.targets.empty() || !op.keys.empty() ||
      !op.data.empty() || op.element_width ||
      !std::holds_alternative<std::monostate>(op.literal))
    return {};
  return descriptor(*type);
}

std::optional<ConstructorPrefixConstant> integerConstant(const Instruction &op,
                                                         unsigned registers) {
  const bool wide = op.opcode == "const-wide" || op.opcode == "const-wide/16" ||
                    op.opcode == "const-wide/32" ||
                    op.opcode == "const-wide/high16";
  const bool narrow = op.opcode == "const" || op.opcode == "const/4" ||
                      op.opcode == "const/16" || op.opcode == "const/high16";
  const auto *bits = std::get_if<int64_t>(&op.literal);
  const unsigned words = wide ? 2 : 1;
  if ((!wide && !narrow) || !bits || op.registers.size() != 1 ||
      op.registers[0] >= registers || words > registers - op.registers[0] ||
      op.target || !op.targets.empty() || !op.keys.empty() ||
      !op.data.empty() || op.element_width ||
      !std::holds_alternative<std::monostate>(op.reference))
    return {};
  return ConstructorPrefixConstant{op.registers[0],
                                   wide    ? "bits64"
                                   : *bits ? "bits32"
                                           : "zero",
                                   *bits};
}

std::optional<ConstructorPrefixConstant>
constructorPrefixIntegerConstant(const Method &method, const Instruction &op) {
  if (method.reference.name != "<init>" || has(method.access, "static") ||
      method.registers > 1024 || method.registers < method.incomingWords() ||
      op.pc >= method.code_end)
    return {};
  return integerConstant(op, method.registers);
}

std::optional<ConstructorPrefixCopy>
constructorPrefixCopy(const Method &method, const Instruction &op) {
  static const std::map<std::string_view, std::pair<std::string_view, unsigned>>
      Copies{{"move", {"move", 1}},
             {"move/from16", {"move", 1}},
             {"move/16", {"move", 1}},
             {"move-object", {"move-object", 1}},
             {"move-object/from16", {"move-object", 1}},
             {"move-object/16", {"move-object", 1}},
             {"move-wide", {"move-wide", 2}},
             {"move-wide/from16", {"move-wide", 2}},
             {"move-wide/16", {"move-wide", 2}}};
  const auto Found = Copies.find(op.opcode);
  if (Found == Copies.end() || op.registers.size() != 2 ||
      method.reference.name != "<init>" || has(method.access, "static") ||
      method.registers > 1024 || method.registers < method.incomingWords() ||
      op.pc >= method.code_end || op.target || !op.targets.empty() ||
      !op.keys.empty() || !op.data.empty() || op.element_width ||
      !std::holds_alternative<std::monostate>(op.reference) ||
      !std::holds_alternative<std::monostate>(op.literal))
    return {};
  for (auto Register : op.registers)
    if (Register >= method.registers ||
        Found->second.second > method.registers - Register)
      return {};
  return ConstructorPrefixCopy{op.registers[0], op.registers[1],
                               Found->second.second,
                               std::string(Found->second.first)};
}

std::optional<size_t>
constructorPrefixAllocationArgumentsEnd(const Method &method,
                                        size_t instruction, Budget &budget) {
  if (constructorPrefixAllocation(method, instruction))
    return instruction + 1;
  const auto end = constructorPrefixAllocationEnd(method, instruction, budget);
  if (!end)
    return {};
  auto previous = method.instructions[instruction].pc;
  for (size_t at = instruction + 1; at < *end; ++at) {
    budget.tick();
    const auto &op = method.instructions[at];
    if (op.pc <= previous || op.pc >= method.instructions[*end].pc)
      return {};
    if (!emptyNop(op) && !constructorPrefixIntegerConstant(method, op)) {
      const auto Copy = constructorPrefixCopy(method, op);
      const auto Self = method.registers - method.incomingWords();
      if (!Copy ||
          (Copy->source <= Self && Self - Copy->source < Copy->words) ||
          (Copy->destination <= Self && Self - Copy->destination < Copy->words))
        return {};
    }
    previous = op.pc;
  }
  return end;
}

std::optional<ConstructorPrefixReferenceArrayLoop>
constructorPrefixReferenceArrayLoop(const Method &method, size_t instruction,
                                    Budget &budget) {
  const auto &code = method.instructions;
  if (method.reference.name != "<init>" || has(method.access, "static") ||
      method.registers > 1024 || method.registers < method.incomingWords() ||
      !method.tries.empty() || instruction >= code.size() ||
      code.size() - instruction < 9)
    return {};
  const unsigned self = method.registers - method.incomingWords();
  const auto &array = code[instruction];
  const auto *type = std::get_if<std::string>(&array.reference);
  if (array.opcode != "new-array" || array.registers.size() != 2 || !type ||
      !type->starts_with("[L") || !type->ends_with(';'))
    return {};
  const auto index =
      constructorPrefixIntegerConstant(method, code[instruction + 1]);
  const auto initializer = constructorPrefixAllocation(method, instruction + 3);
  if (!index || index->type != "zero" || index->bits || !initializer ||
      !initializer->parameters.empty())
    return {};
  const unsigned target = array.registers[0], length = array.registers[1];
  const unsigned counter = index->destination;
  const unsigned element = code[instruction + 3].registers[0];
  if (target >= self || counter >= self || element >= self ||
      length >= method.registers || length == self ||
      std::set<unsigned>{target, length, counter, element}.size() != 4)
    return {};
  const auto &guard = code[instruction + 2];
  const auto &store = code[instruction + 5];
  const auto &step = code[instruction + 6];
  const auto &back = code[instruction + 7];
  if (guard.opcode != "if-ge" ||
      guard.registers != std::vector<unsigned>{counter, length} ||
      guard.target != code[instruction + 8].pc ||
      store.opcode != "aput-object" ||
      store.registers != std::vector<unsigned>{element, target, counter} ||
      step.opcode != "add-int/lit8" ||
      step.registers != std::vector<unsigned>{counter, counter} ||
      !std::holds_alternative<int64_t>(step.literal) ||
      std::get<int64_t>(step.literal) != 1 || back.opcode != "goto" ||
      !back.registers.empty() || back.target != guard.pc)
    return {};
  for (size_t at = instruction; at <= instruction + 7; ++at) {
    budget.tick();
    const auto &op = code[at];
    if ((at && code[at - 1].pc >= op.pc) || op.pc >= code[instruction + 8].pc ||
        op.pc >= method.code_end || !op.targets.empty() || !op.keys.empty() ||
        !op.data.empty() || op.element_width ||
        (at != instruction + 2 && at != instruction + 7 && op.target) ||
        (at != instruction + 1 && at != instruction + 6 &&
         !std::holds_alternative<std::monostate>(op.literal)) ||
        (at != instruction && at != instruction + 3 && at != instruction + 4 &&
         !std::holds_alternative<std::monostate>(op.reference)))
      return {};
  }
  const auto inside = [&](uint32_t pc) {
    return pc >= array.pc && pc < code[instruction + 8].pc;
  };
  for (size_t at = 0; at < code.size(); ++at) {
    budget.tick();
    if (at >= instruction && at <= instruction + 7)
      continue;
    if ((code[at].target && inside(*code[at].target)) ||
        std::any_of(code[at].targets.begin(), code[at].targets.end(), inside))
      return {};
  }
  return ConstructorPrefixReferenceArrayLoop{
      instruction + 7, target, length, counter, element, *type, *initializer};
}

static std::optional<FieldRef>
constructorPrefixFieldRead(const Method &method, const Instruction &op,
                           const ClassMap &classes, Budget &budget,
                           bool is_static) {
  budget.tick();
  const auto *ref = std::get_if<FieldRef>(&op.reference);
  if (!ref || method.reference.name != "<init>" ||
      has(method.access, "static") || method.registers > 1024 ||
      method.registers < method.incomingWords() ||
      op.registers.size() != (is_static ? 1u : 2u) || op.target ||
      !op.targets.empty() || !op.keys.empty() || !op.data.empty() ||
      op.element_width || !std::holds_alternative<std::monostate>(op.literal) ||
      ref->type == "V")
    return {};
  const auto cls = classes.find(ref->owner);
  if (cls == classes.end())
    return {};
  const auto *field = lookupField(*ref, classes, budget).declaration;
  if (!field || has(field->access, "static") != is_static)
    return {};
  const auto declaration = classes.find(field->reference.owner);
  if (declaration == classes.end() ||
      std::none_of(declaration->second.fields.begin(),
                   declaration->second.fields.end(),
                   [&](const Field &candidate) { return &candidate == field; }))
    return {};
  if (field->reference.owner != ref->owner) {
    // Newly admitted inherited reads must have one complete class chain.
    // Interfaces on that path must be fully known and cannot hide this field.
    // Inherited instance reads retain their separate proof boundary.
    if (!is_static || has(field->access, "private"))
      return {};
    std::string current = ref->owner;
    std::set<std::string> seen;
    std::vector<std::pair<std::string, bool>> interfaces;
    const bool package_field =
        !has(field->access, "public") && !has(field->access, "protected");
    const bool interfaceField = has(declaration->second.access, "interface");
    if (interfaceField && field->access != Access{"public", "static", "final"})
      return {};
    while (current != field->reference.owner) {
      budget.tick();
      const auto step = classes.find(current);
      if (step == classes.end() || !seen.insert(current).second ||
          (package_field &&
           packageOf(current) != packageOf(field->reference.owner)))
        return {};
      if (interfaceField) {
        // The symbolic owner may inherit a static interface field. Java
        // resolves it by name; a competing declaration on either path could
        // select different storage or initialize a different interface.
        if (std::any_of(step->second.fields.begin(), step->second.fields.end(),
                        [&](const Field &other) {
                          budget.tick();
                          return other.reference.name == ref->name;
                        }))
          return {};
      }
      for (const auto &interface : step->second.interfaces)
        interfaces.emplace_back(interface, false);
      if (!step->second.superclass) {
        if (interfaceField && current == "Ljava/lang/Object;")
          break;
        return {};
      }
      current = *step->second.superclass;
    }
    std::set<std::string> active, complete;
    while (!interfaces.empty()) {
      budget.tick();
      auto [name, leaving] = std::move(interfaces.back());
      interfaces.pop_back();
      if (leaving) {
        active.erase(name);
        complete.insert(name);
        continue;
      }
      if (complete.contains(name))
        continue;
      const auto interface = classes.find(name);
      if (interface == classes.end() ||
          !has(interface->second.access, "interface") ||
          !active.insert(name).second)
        return {};
      for (const auto &candidate : interface->second.fields) {
        budget.tick();
        if (candidate.reference.name == ref->name &&
            (!interfaceField || &candidate != field))
          return {};
      }
      interfaces.emplace_back(name, true);
      for (const auto &parent : interface->second.interfaces)
        interfaces.emplace_back(parent, false);
    }
    if (interfaceField && !complete.contains(field->reference.owner))
      return {};
  }
  // Use the actual declaration for access and encoded-value facts, including
  // when the original spelling names this subclass. Preserve the existing
  // public/same-package boundary for foreign protected and nestmate accesses.
  if (field->reference.owner != method.reference.owner &&
      (has(field->access, "private") ||
       (!has(field->access, "public") &&
        packageOf(field->reference.owner) !=
            packageOf(method.reference.owner))))
    return {};
  if (ref->owner != method.reference.owner) {
    const auto caller = classes.find(method.reference.owner);
    const bool local_static_owner =
        is_static && caller != classes.end() && isAnonymousLocal(cls->second) &&
        localClassVisible(cls->second, caller->second, &method.reference,
                          classes, budget);
    // A top-level package class has ordinary source visibility in the same
    // runtime package. Nested/local owners retain their separate scope proof;
    // this receipt grants neither inherited protected nor nestmate access.
    const bool package_owner =
        !has(cls->second.access, "public") &&
        !has(cls->second.access, "private") &&
        !has(cls->second.access, "protected") &&
        !has(cls->second.access, "interface") &&
        !has(cls->second.access, "annotation") &&
        !has(cls->second.access, "enum") && !cls->second.enclosing &&
        !cls->second.enclosing_method && !cls->second.inner_class_present &&
        independentSourceScope(cls->second) &&
        packageOf(ref->owner) == packageOf(method.reference.owner);
    if (caller == classes.end() || ((!has(cls->second.access, "public") ||
                                     !independentSourceScope(cls->second)) &&
                                    !package_owner && !local_static_owner))
      return {};
  }
  // An encoded integer value initializes storage; it does not make a mutable
  // field a compile-time constant. Keep the runtime read and its initialization
  // effects, including when its value will change during the delegated call.
  const bool mutableInteger = is_static && !has(field->access, "final") &&
                              (ref->type == "I" || ref->type == "J") &&
                              std::holds_alternative<int64_t>(field->value);
  // Owned encoded preparation defaults use the same blank-final/write proof
  // as Java emission, which retains runtime reads via a nonconstant source
  // initializer. An unchanged SDK ConstantValue could still be inlined by
  // javac and therefore requires its separate initialization proof.
  const bool ownedDefault =
      is_static && !declaration->second.external && defaultFieldValue(*field);
  if (!std::holds_alternative<std::monostate>(field->value) &&
      !mutableInteger && !ownedDefault)
    return {};
  const auto self = method.registers - method.incomingWords();
  const auto words = width(ref->type);
  const auto destination = op.registers[0];
  // Parameters are ordinary writable DEX registers. A default-argument
  // prefix may replace one with a static receiver and then its getter result.
  // Keep the full destination in the frame without touching uninitialized
  // this; callers still prove aliases and the types at every control-flow join.
  if (destination >= method.registers ||
      words > method.registers - destination ||
      (destination <= self && self - destination < words) ||
      (!is_static &&
       (op.registers[1] >= method.registers || op.registers[1] == self)))
    return {};
  const auto suffix = ref->type.starts_with('L') || ref->type.starts_with('[')
                          ? "-object"
                      : words == 2       ? "-wide"
                      : ref->type == "Z" ? "-boolean"
                      : ref->type == "B" ? "-byte"
                      : ref->type == "C" ? "-char"
                      : ref->type == "S" ? "-short"
                                         : "";
  const auto expected = std::string(is_static ? "sget" : "iget") + suffix;
  return op.opcode == expected ? std::optional<FieldRef>(*ref) : std::nullopt;
}

std::optional<FieldRef> constructorPrefixStaticRead(const Method &method,
                                                    const Instruction &op,
                                                    const ClassMap &classes,
                                                    Budget &budget) {
  return constructorPrefixFieldRead(method, op, classes, budget, true);
}

std::optional<FieldRef> constructorPrefixInstanceRead(const Method &method,
                                                      const Instruction &op,
                                                      const ClassMap &classes,
                                                      Budget &budget) {
  return constructorPrefixFieldRead(method, op, classes, budget, false);
}

bool constructorPrefixValueCall(const Instruction &op) {
  const auto *call = std::get_if<MethodRef>(&op.reference);
  static const std::set<std::string_view> kinds{
      "invoke-static",        "invoke-static/range", "invoke-virtual",
      "invoke-virtual/range", "invoke-interface",    "invoke-interface/range",
      "invoke-direct",        "invoke-direct/range"};
  return call && call->returns != "V" && !call->name.starts_with('<') &&
         kinds.contains(op.opcode);
}

// Prove only the captured outer identity across a bounded default-argument
// prefix. Scalar branches and getter effects remain in Body; they are neither
// folded here nor treated as a pure prefix.
static std::optional<MethodRef> guardedOuterConstructorDelegation(
    const Class &cls, const Method &method, const ClassMap &classes,
    const ConstructorCaptures &captures, Budget &budget) {
  const auto &ref = method.reference;
  const auto &code = method.instructions;
  if (cls.external || !cls.enclosing || cls.enclosing_method ||
      !cls.inner_class_present || !cls.inner_name ||
      has(cls.inner_access, "static") || ref.name != "<init>" ||
      ref.returns != "V" || !has(method.access, "synthetic") ||
      has(method.access, "static") || method.registers > 1024 ||
      method.registers < method.incomingWords() || !method.tries.empty() ||
      ref.parameters.empty() || ref.parameters[0] != *cls.enclosing ||
      code.size() < 4 || code.size() > 256)
    return {};
  const auto self = method.registers - method.incomingWords();
  const auto outer = self + 1;
  const auto call_at = code.size() - 2;
  const auto &call = code[call_at];
  const auto *target = std::get_if<MethodRef>(&call.reference);
  auto emptyMetadata = [](const Instruction &op) {
    return op.keys.empty() && op.targets.empty() && op.data.empty() &&
           op.element_width == 0;
  };
  auto plain = [&](const Instruction &op) {
    return emptyMetadata(op) && !op.target &&
           std::holds_alternative<std::monostate>(op.reference) &&
           std::holds_alternative<std::monostate>(op.literal);
  };
  if (!target || target->owner != cls.name || target->name != "<init>" ||
      target->returns != "V" || target->parameters.empty() ||
      target->parameters[0] != *cls.enclosing ||
      (call.opcode != "invoke-direct" &&
       call.opcode != "invoke-direct/range") ||
      !emptyMetadata(call) || call.target ||
      !std::holds_alternative<std::monostate>(call.literal) ||
      call.registers.size() != target->parameters.size() + 1 ||
      call.registers[0] != self || call.registers[1] != outer ||
      std::any_of(target->parameters.begin() + 1, target->parameters.end(),
                  [](const std::string &type) { return type != "I"; }) ||
      code.back().opcode != "return-void" || !code.back().registers.empty() ||
      !plain(code.back()))
    return {};
  const auto capture = captures.find(*target);
  if (capture == captures.end() || capture->second.size() != 1 ||
      !capture->second.contains(0) ||
      capture->second.at(0).type != *cls.enclosing)
    return {};
  const auto target_decl = std::find_if(
      cls.methods.begin(), cls.methods.end(),
      [&](const Method &candidate) { return candidate.reference == *target; });
  if (target_decl == cls.methods.end() || has(target_decl->access, "static"))
    return {};
  std::map<uint32_t, size_t> positions;
  for (size_t at = 0; at < code.size(); ++at) {
    budget.tick();
    if ((at && code[at - 1].pc >= code[at].pc) ||
        !positions.emplace(code[at].pc, at).second ||
        std::any_of(code[at].registers.begin(), code[at].registers.end(),
                    [&](unsigned reg) { return reg >= method.registers; }))
      return {};
  }
  // Forward-only control flow makes each intersection complete before use.
  using Scalars = std::set<unsigned>;
  std::vector<std::optional<Scalars>> incoming(code.size());
  Scalars inputs;
  unsigned reg = outer;
  for (const auto &type : ref.parameters) {
    if (type == "I")
      inputs.insert(reg);
    reg += width(type);
  }
  incoming[0] = std::move(inputs);
  auto merge = [&](size_t at, const Scalars &state) {
    if (!incoming[at]) {
      incoming[at] = state;
      return;
    }
    Scalars intersection;
    std::set_intersection(incoming[at]->begin(), incoming[at]->end(),
                          state.begin(), state.end(),
                          std::inserter(intersection, intersection.end()));
    incoming[at] = std::move(intersection);
  };
  auto writable = [&](unsigned destination) {
    return destination != self && destination != outer &&
           destination < method.registers;
  };
  bool guarded = false;
  for (size_t at = 0; at < call_at; ++at) {
    budget.tick();
    if (!incoming[at])
      return {};
    auto state = *incoming[at];
    const auto &op = code[at];
    if (op.opcode == "if-eqz" || op.opcode == "if-nez") {
      const auto branch =
          op.target ? positions.find(*op.target) : positions.end();
      if (op.registers.size() != 1 || !state.contains(op.registers[0]) ||
          !emptyMetadata(op) ||
          !std::holds_alternative<std::monostate>(op.reference) ||
          !std::holds_alternative<std::monostate>(op.literal) ||
          branch == positions.end() || branch->second <= at ||
          branch->second > call_at ||
          code[branch->second].opcode == "move-result")
        return {};
      merge(branch->second, state);
      guarded = true;
    } else if (op.opcode == "const" || op.opcode == "const/4" ||
               op.opcode == "const/16" || op.opcode == "const/high16") {
      const auto *literal = std::get_if<int64_t>(&op.literal);
      if (!emptyMetadata(op) || op.target ||
          !std::holds_alternative<std::monostate>(op.reference) || !literal ||
          *literal < std::numeric_limits<int32_t>::min() ||
          *literal > std::numeric_limits<int32_t>::max() ||
          op.registers.size() != 1 || !writable(op.registers[0]))
        return {};
      state.insert(op.registers[0]);
    } else if (op.opcode == "move" || op.opcode == "move/from16" ||
               op.opcode == "move/16" || op.opcode == "and-int/lit8" ||
               op.opcode == "and-int/lit16") {
      const bool mask = op.opcode.starts_with("and-int/");
      const auto *literal = std::get_if<int64_t>(&op.literal);
      if (!emptyMetadata(op) || op.target ||
          !std::holds_alternative<std::monostate>(op.reference) ||
          (mask ? !literal || *literal < -32768 || *literal > 32767
                : !std::holds_alternative<std::monostate>(op.literal)) ||
          op.registers.size() != 2 || !writable(op.registers[0]) ||
          !state.contains(op.registers[1]))
        return {};
      state.insert(op.registers[0]);
    } else if (op.opcode == "invoke-virtual" ||
               op.opcode == "invoke-virtual/range") {
      const auto *getter = std::get_if<MethodRef>(&op.reference);
      if (!getter || getter->owner != *cls.enclosing ||
          !getter->parameters.empty() || getter->returns != "I" ||
          op.registers != std::vector<unsigned>{outer} || !emptyMetadata(op) ||
          op.target || !std::holds_alternative<std::monostate>(op.literal) ||
          at + 1 >= call_at || code[at + 1].opcode != "move-result" ||
          code[at + 1].registers.size() != 1 || !plain(code[at + 1]) ||
          !writable(code[at + 1].registers[0]) || incoming[at + 1])
        return {};
      const auto owner = classes.find(getter->owner);
      if (owner == classes.end())
        return {};
      const auto declaration = std::find_if(
          owner->second.methods.begin(), owner->second.methods.end(),
          [&](const Method &candidate) {
            return candidate.reference == *getter;
          });
      if (declaration == owner->second.methods.end() ||
          !has(declaration->access, "public") ||
          has(declaration->access, "static"))
        return {};
      state.insert(code[++at].registers[0]);
    } else {
      return {};
    }
    merge(at + 1, state);
  }
  if (!guarded || !incoming[call_at] ||
      std::any_of(call.registers.begin() + 2, call.registers.end(),
                  [&](unsigned argument) {
                    return !incoming[call_at]->contains(argument);
                  }))
    return {};
  return *target;
}

ConstructorCaptures
planConstructorCaptures(const ClassMap &classes, Budget &budget,
                        ConstructorDelegations *delegations) {
  ConstructorCaptures result;
  struct Forwarding {
    MethodRef target;
    std::map<size_t, size_t> parameters;
  };
  std::map<MethodRef, Forwarding> forwarding;
  if (delegations)
    delegations->clear();
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (cls.external || (!cls.enclosing && !cls.enclosing_method) ||
        !cls.inner_class_present || has(cls.access, "enum"))
      continue;
    for (const auto &method : cls.methods) {
      budget.tick();
      const auto &ref = method.reference;
      if (ref.name != "<init>" || has(method.access, "static") ||
          method.registers > 1024 || method.registers < method.incomingWords())
        continue;
      struct Value {
        std::string type;
        std::optional<size_t> parameter;
        bool self = false;
      };
      const auto self = method.registers - method.incomingWords();
      std::map<unsigned, Value> values;
      values[self] = {name, {}, true};
      unsigned reg = self + 1;
      for (size_t i = 0; i < ref.parameters.size(); ++i) {
        const auto &type = ref.parameters[i];
        values[reg] = {type, i};
        if (width(type) == 2)
          values[reg + 1] = {"wide-high", {}};
        reg += width(type);
      }
      auto wide = [](const std::string &type) {
        return type == "J" || type == "D" || type == "bits64";
      };
      auto reference = [](const std::string &type) {
        return type.starts_with('L') || type.starts_with('[');
      };
      auto write = [&](unsigned destination, const Value &value) {
        const unsigned words = wide(value.type) ? 2 : 1;
        // Incoming parameters are writable DEX registers. A prior capture
        // keeps the stored identity, while this write replaces the live
        // parameter origin, including both halves of an overlapping wide value.
        if (destination >= method.registers ||
            words > method.registers - destination ||
            (destination <= self && self - destination < words))
          return false;
        for (unsigned word = destination; word < destination + words; ++word) {
          if (auto previous = values.find(word); previous != values.end()) {
            if (wide(previous->second.type))
              values.erase(word + 1);
            else if (previous->second.type == "wide-high" && word)
              values.erase(word - 1);
          }
          values.erase(word);
        }
        values[destination] = value;
        if (words == 2)
          values[destination + 1] = {"wide-high", {}};
        return true;
      };
      auto copyValue = [&](const Instruction &op, bool Argument = false) {
        const auto Copy = constructorPrefixCopy(method, op);
        if (!Copy || !values.contains(Copy->source))
          return false;
        const auto value = values.at(Copy->source);
        if ((Argument && value.self) ||
            (Copy->kind == "move-object" && !reference(value.type) &&
             value.type != "zero") ||
            (Copy->kind == "move-wide" && !wide(value.type)) ||
            (Copy->kind == "move" &&
             (reference(value.type) || wide(value.type) ||
              value.type == "wide-high")) ||
            (wide(value.type) &&
             (!values.contains(Copy->source + 1) ||
              values.at(Copy->source + 1).type != "wide-high")))
          return false;
        return write(Copy->destination, value);
      };
      std::map<size_t, FieldRef> captures;
      bool pure = true;
      std::optional<std::string> getter_result;
      for (size_t at = 0; at < method.instructions.size(); ++at) {
        const auto &op = method.instructions[at];
        budget.tick();
        if (op.target || !op.targets.empty())
          break;
        if (getter_result) {
          const auto type = *getter_result;
          const auto expected = reference(type) ? "move-result-object"
                                : wide(type)    ? "move-result-wide"
                                                : "move-result";
          if (op.opcode != expected || op.registers.size() != 1 ||
              !write(op.registers[0], {type, {}}))
            break;
          getter_result.reset();
          continue;
        }
        if (op.opcode == "nop" && op.registers.empty())
          continue;
        if (op.opcode == "const-class") {
          if (!constructorPrefixClassLiteral(method, op) ||
              !write(op.registers[0], {"Ljava/lang/Class;", {}}))
            break;
          // Resolution may fail even when the argument is unused. Preserve
          // the effect in Body rather than treating this as a pure capture.
          pure = false;
          continue;
        }
        if (op.opcode.starts_with("sget")) {
          const auto field =
              constructorPrefixStaticRead(method, op, classes, budget);
          if (!field || !write(op.registers[0], {field->type, {}}))
            break;
          pure = false;
          continue;
        }
        if (op.opcode == "new-array" && at && op.registers.size() == 2) {
          const auto *type = std::get_if<std::string>(&op.reference);
          const auto size = constructorPrefixIntegerConstant(method, method.instructions[at - 1]);
          if (!size || size->bits != 0 || size->type == "bits64" ||
              size->destination != op.registers[1] || !type || !type->starts_with('[') ||
              !write(op.registers[0], {*type, {}}))
            break;
          // The allocation contributes no captured origin. The constructor
          // prefix emits it before this(...) and preserves allocation failure.
          continue;
        }
        if (op.opcode == "new-instance") {
          const auto end =
              constructorPrefixAllocationArgumentsEnd(method, at, budget);
          if (!end)
            break;
          bool valid = true;
          for (size_t prep = at + 1; prep < *end; ++prep) {
            const auto &prepare = method.instructions[prep];
            if (emptyNop(prepare))
              continue;
            const auto constant =
                constructorPrefixIntegerConstant(method, prepare);
            if (constant ? !write(constant->destination, {constant->type, {}})
                         : !copyValue(prepare, true)) {
              valid = false;
              break;
            }
          }
          const auto &allocation =
              std::get<MethodRef>(method.instructions[*end].reference);
          if (!valid || !write(op.registers[0], {allocation.owner, {}}))
            break;
          at = *end;
          pure = false;
          continue;
        }
        const auto kind = op.opcode.substr(0, op.opcode.find('/'));
        if (kind == "move" || kind == "move-object" || kind == "move-wide") {
          if (!copyValue(op))
            break;
          continue;
        }
        if (op.opcode == "check-cast" && op.registers.size() == 1) {
          const auto *type = std::get_if<std::string>(&op.reference);
          const auto value = values.find(op.registers[0]);
          if (!type || !reference(*type) || value == values.end() ||
              value->second.self ||
              (!reference(value->second.type) && value->second.type != "zero"))
            break;
          auto converted = value->second;
          converted.type = *type;
          if (!write(op.registers[0], converted))
            break;
          // The cast preserves identity on the normal path, but may throw.
          // Body retains that effect before super; it is not a pure
          // this-prefix.
          pure = false;
          continue;
        }
        if (const auto *call = std::get_if<MethodRef>(&op.reference)) {
          if ((op.opcode == "invoke-direct" ||
               op.opcode == "invoke-direct/range") &&
              call->name == "<init>" && call->returns == "V" &&
              !op.registers.empty() && values.contains(op.registers.front()) &&
              values.at(op.registers.front()).self &&
              (call->owner == name || cls.superclass == call->owner)) {
            size_t cursor = 1;
            Forwarding edge{*call, {}};
            bool valid = true;
            for (size_t i = 0; i < call->parameters.size(); ++i) {
              if (cursor >= op.registers.size()) {
                valid = false;
                break;
              }
              const auto argument = values.find(op.registers[cursor]);
              const auto words = width(call->parameters[i]);
              if (argument == values.end() || argument->second.self ||
                  (words == 2 &&
                   (cursor + 1 >= op.registers.size() ||
                    op.registers[cursor + 1] != op.registers[cursor] + 1 ||
                    !values.contains(op.registers[cursor] + 1) ||
                    values.at(op.registers[cursor] + 1).type != "wide-high"))) {
                valid = false;
                break;
              }
              if (argument->second.parameter &&
                  argument->second.type == call->parameters[i])
                edge.parameters.emplace(i, *argument->second.parameter);
              cursor += words;
            }
            if (valid && cursor == op.registers.size()) {
              if (call->owner != name && !captures.empty())
                result.emplace(ref, std::move(captures));
              else if (call->owner == name && pure)
                forwarding.emplace(ref, std::move(edge));
            }
            break;
          }
          // Body verifies invocation types and exception contracts. Here calls
          // may preserve capture origins, but cannot make a pure this-prefix.
          if (call->returns == "V" &&
              (op.opcode == "invoke-static" ||
               op.opcode == "invoke-static/range") &&
              std::none_of(op.registers.begin(), op.registers.end(),
                           [&](unsigned value) {
                             return values.contains(value) &&
                                    values.at(value).self;
                           })) {
            pure = false;
            continue;
          }
          if (!captures.empty() && constructorPrefixValueCall(op) &&
              (op.opcode == "invoke-static" || op.opcode == "invoke-static/range") &&
              call->parameters.size() == 1 && op.registers.size() == 1 &&
              call->parameters.front() == "I" && values.contains(op.registers[0]) &&
              values.at(op.registers[0]).type == "I") {
            // This call reads only an integer after the outer capture store.
            // Its result may overwrite an incoming word. Keep the saved field
            // origin while the ordinary verifier owns invocation semantics.
            getter_result = call->returns;
            pure = false;
            continue;
          }
          if (call->parameters.empty() && constructorPrefixValueCall(op) &&
              (((!op.opcode.starts_with("invoke-static")) &&
                op.registers.size() == 1 && values.contains(op.registers[0]) &&
                !values.at(op.registers[0]).self &&
                reference(values.at(op.registers[0]).type)) ||
               ((op.opcode == "invoke-static" ||
                 op.opcode == "invoke-static/range") &&
                op.registers.empty()))) {
            getter_result = call->returns;
            pure = false;
            continue;
          }
        }
        const bool string =
            op.opcode == "const-string" || op.opcode == "const-string/jumbo";
        if (const auto constant =
                constructorPrefixIntegerConstant(method, op)) {
          if (!write(constant->destination, {constant->type, {}}))
            break;
          continue;
        }
        if (string && op.registers.size() == 1 &&
            std::holds_alternative<std::string>(op.literal)) {
          if (!write(op.registers[0], {"Ljava/lang/String;", {}}))
            break;
          continue;
        }
        const auto *field = std::get_if<FieldRef>(&op.reference);
        if (!field || !op.opcode.starts_with("iput") || field->owner != name ||
            op.registers.size() != 2 || !values.contains(op.registers[1]) ||
            !values.at(op.registers[1]).self ||
            !values.contains(op.registers[0]))
          break;
        const auto &value = values.at(op.registers[0]);
        if (!value.parameter || value.self)
          break;
        const auto &type = value.type;
        const std::string expected = reference(type) ? "iput-object"
                                     : wide(type)    ? "iput-wide"
                                     : type == "Z"   ? "iput-boolean"
                                     : type == "B"   ? "iput-byte"
                                     : type == "C"   ? "iput-char"
                                     : type == "S"   ? "iput-short"
                                                     : "iput";
        const auto declaration = std::find_if(
            cls.fields.begin(), cls.fields.end(), [&](const Field &candidate) {
              return candidate.reference == *field;
            });
        if (declaration == cls.fields.end() ||
            has(declaration->access, "static") || field->type != type ||
            op.opcode != expected)
          break;
        pure = false;
        if (has(declaration->access, "synthetic") &&
            has(declaration->access, "final") &&
            std::holds_alternative<std::monostate>(declaration->value) &&
            !captures.emplace(*value.parameter, *field).second)
          break;
      }
    }
  }
  for (const auto &[name, cls] : classes) {
    for (const auto &method : cls.methods) {
      budget.tick();
      if (result.contains(method.reference) ||
          forwarding.contains(method.reference))
        continue;
      if (const auto target = guardedOuterConstructorDelegation(
              cls, method, classes, result, budget))
        forwarding.emplace(method.reference, Forwarding{*target, {{0, 0}}});
    }
  }
  std::map<MethodRef, unsigned> depth;
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[caller, edge] : forwarding) {
      budget.tick();
      if (result.contains(caller) || !result.contains(edge.target) ||
          depth[edge.target] >= 128)
        continue;
      std::map<size_t, FieldRef> captures;
      bool valid = true;
      for (const auto &[parameter, field] : result.at(edge.target)) {
        budget.tick();
        const auto source = edge.parameters.find(parameter);
        if (source == edge.parameters.end() ||
            caller.parameters[source->second] != field.type ||
            !captures.emplace(source->second, field).second) {
          valid = false;
          break;
        }
      }
      if (valid && !captures.empty()) {
        result.emplace(caller, std::move(captures));
        depth[caller] = depth[edge.target] + 1;
        if (delegations)
          delegations->emplace(caller, edge.target);
        changed = true;
      }
    }
  }
  return result;
}

ConstructorDelegations
planConstructorDelegations(const ClassMap &classes, Budget &budget,
                           const ConstructorDelegations &captured,
                           ConstructorSuperPrologues *super_prologues,
                           const ConstructorCaptures *captures) {
  if (super_prologues)
    super_prologues->clear();
  ConstructorDelegations result = captured, candidates;
  std::set<MethodRef> primary;
  for (const auto &[name, cls] : classes) {
    if (cls.external || has(cls.access, "enum"))
      continue;
    for (const auto &method : cls.methods) {
      budget.tick();
      const auto &code = method.instructions;
      if (method.reference.name != "<init>" || has(method.access, "static") ||
          method.registers > 1024 ||
          method.registers < method.incomingWords() || code.empty() ||
          captured.contains(method.reference))
        continue;
      const auto self = method.registers - method.incomingWords();
      const std::map<size_t, FieldRef> *capture_fields = nullptr;
      if (captures)
        if (const auto found = captures->find(method.reference);
            found != captures->end())
          capture_fields = &found->second;
      const auto prefix = selectConstructorPrefixDelegation(method, budget);
      if (!prefix)
        continue;
      const auto &allocations = prefix->allocations;
      const size_t delegation = prefix->call;
      const auto call = code.begin() + delegation;
      const auto *target = std::get_if<MethodRef>(&call->reference);
      if (!target || target->name != "<init>")
        continue;
      const bool is_primary = cls.superclass == target->owner;
      if ((!is_primary && target->owner != name) || target->returns != "V" ||
          call->registers.empty() ||
          (call->opcode != "invoke-direct" &&
           call->opcode != "invoke-direct/range") ||
          (!is_primary && (code.size() < 2 || !method.tries.empty() ||
                           code.back().opcode != "return-void" ||
                           !code.back().registers.empty() ||
                           std::none_of(cls.methods.begin(), cls.methods.end(),
                                        [&](const Method &m) {
                                          return m.reference == *target;
                                        }))))
        continue;
      std::map<uint32_t, size_t> positions;
      bool valid = true, guarded = prefix->requires_dispatch;
      for (size_t at = 0; at <= delegation; ++at) {
        budget.tick();
        const auto &op = code[at];
        if ((at && code[at - 1].pc >= op.pc) || op.pc >= method.code_end ||
            !positions.emplace(op.pc, at).second || !op.targets.empty() ||
            !op.keys.empty() || !op.data.empty() || op.element_width ||
            std::any_of(op.registers.begin(), op.registers.end(),
                        [&](unsigned r) { return r >= method.registers; })) {
          valid = false;
          break;
        }
      }
      if (!valid)
        continue;
      // Preserve must/may origins at all joins. Only exact reference copies
      // may name uninitialized this; no other prefix operation may observe it.
      // Operand types and original call semantics remain Body's responsibility.
      constexpr unsigned Self = 1, Other = 2;
      using Origins = std::vector<unsigned>;
      std::vector<std::optional<Origins>> incoming(delegation + 1);
      incoming[0] = Origins(method.registers, Other);
      (*incoming[0])[self] = Self;
      std::deque<size_t> queue;
      if (delegation)
        queue.push_back(0);
      std::set<size_t> queued{0};
      auto merge = [&](size_t at, const Origins &state) {
        Origins next = state;
        if (incoming[at])
          for (unsigned r = 0; r < method.registers; ++r)
            next[r] |= (*incoming[at])[r];
        if (incoming[at] && *incoming[at] == next)
          return;
        incoming[at] = std::move(next);
        if (at < delegation && queued.insert(at).second)
          queue.push_back(at);
      };
      while (!queue.empty() && valid) {
        size_t at = queue.front();
        queue.pop_front();
        queued.erase(at);
        budget.tick(method.registers + 1);
        const auto &op = code[at];
        if (!incoming[at]) {
          valid = false;
          break;
        }
        auto state = *incoming[at];
        const auto kind = op.opcode.substr(0, op.opcode.find('/'));
        if (const auto loop =
                constructorPrefixReferenceArrayLoop(method, at, budget)) {
          if (loop->end >= delegation) {
            valid = false;
            break;
          }
          for (size_t index = at; index <= loop->end; ++index) {
            const auto &prepare = code[index];
            if (std::any_of(prepare.registers.begin(), prepare.registers.end(),
                            [&](unsigned r) { return state[r] & Self; })) {
              valid = false;
              break;
            }
            incoming[index] = state;
          }
          if (valid) {
            guarded = true;
            merge(loop->end + 1, state);
          }
          continue;
        }
        if (op.opcode == "new-instance") {
          const auto allocation = allocations.find(at);
          if (allocation == allocations.end() ||
              allocation->second >= delegation) {
            valid = false;
            break;
          }
          // Check every physical word in the closed argument region against
          // the converged uninitialized-this origins. Body independently
          // verifies its operations using the ordinary allocation planner.
          for (size_t index = at; index <= allocation->second; ++index) {
            const auto &prepare = code[index];
            budget.tick(method.registers + 1);
            if (std::any_of(prepare.registers.begin(), prepare.registers.end(),
                            [&](unsigned r) { return state[r] & Self; }) ||
                !scalarWordsDisjoint(
                    prepare, method.registers,
                    [&](unsigned r) { return state[r] & Self; })) {
              valid = false;
              break;
            }
            const auto kind =
                prepare.opcode.substr(0, prepare.opcode.find('/'));
            if ((kind == "const-wide" || kind == "move-wide" ||
                 kind == "move-result-wide" || kind == "sget-wide") &&
                std::any_of(prepare.registers.begin(), prepare.registers.end(),
                            [&](unsigned r) {
                              return r + 1 >= method.registers ||
                                     (state[r + 1] & Self);
                            })) {
              valid = false;
              break;
            }
            incoming[index] = state;
          }
          if (valid)
            merge(allocation->second + 1, state);
          continue;
        }
        const bool self_copy = kind == "move-object" &&
                               op.registers.size() == 2 &&
                               state[op.registers[1]] == Self;
        const auto *stored_field = std::get_if<FieldRef>(&op.reference);
        const bool captured_store =
            is_primary && capture_fields && kind.starts_with("iput") &&
            stored_field && op.registers.size() == 2 &&
            std::any_of(capture_fields->begin(), capture_fields->end(),
                        [&](const auto &entry) {
                          return entry.second == *stored_field;
                        });
        if (self_copy) {
          if (op.target || op.registers[0] == self ||
              !std::holds_alternative<std::monostate>(op.reference) ||
              !std::holds_alternative<std::monostate>(op.literal)) {
            valid = false;
            break;
          }
          state[op.registers[0]] = Self;
        } else if (captured_store) {
          // The capture planner owns the declaration, exact opcode, incoming
          // parameter and store sequence. Preserve its original effect on
          // this; neither the value nor a wide high word may expose self.
          const bool wide =
              stored_field->type == "J" || stored_field->type == "D";
          if (op.target || state[op.registers[1]] != Self ||
              (state[op.registers[0]] & Self) ||
              (wide && (op.registers[0] + 1 >= method.registers ||
                        (state[op.registers[0] + 1] & Self)))) {
            valid = false;
            break;
          }
        } else {
          if (std::any_of(op.registers.begin(), op.registers.end(),
                          [&](unsigned r) { return state[r] & Self; }) ||
              !scalarWordsDisjoint(
                  op, method.registers,
                  [&](unsigned r) { return state[r] & Self; }) ||
              (kind == "iget-wide" && !op.registers.empty() &&
               (op.registers[0] + 1 >= method.registers ||
                (state[op.registers[0] + 1] & Self))) ||
              ((kind == "const-wide" || kind == "move-wide" ||
                kind == "move-result-wide" || kind == "sget-wide") &&
               std::any_of(
                   op.registers.begin(), op.registers.end(), [&](unsigned r) {
                     return r + 1 >= method.registers || (state[r + 1] & Self);
                   }))) {
            valid = false;
            break;
          }
          if (kind == "move-object" && op.registers.size() == 2)
            state[op.registers[0]] = state[op.registers[1]];
          const auto *ref = std::get_if<MethodRef>(&op.reference);
          const auto binary = longBinary(op.opcode);
          const auto division = integerDivision(op);
          if (!(scalarUnary(op.opcode) || scalarConversion(op.opcode) ||
                scalarComparison(op.opcode) ||
                floatingArithmetic(op.opcode) ||
                (binary && !binary->can_throw) || kind == "nop" ||
                kind == "const" || kind == "const-wide" ||
                kind == "const-string" || kind == "move" ||
                kind == "move-object" || kind == "move-wide" ||
                kind == "move-result" || kind == "move-result-object" ||
                kind == "move-result-wide" || kind == "check-cast" ||
                op.opcode == "instance-of" || op.opcode == "array-length" ||
                (arrayLoad(op.opcode) && op.registers.size() == 3 &&
                 (op.opcode != "aget-wide" ||
                  (op.registers[0] + 1 < method.registers &&
                   !(state[op.registers[0] + 1] & Self)))) ||
                integerPreparation(op) ||
                (division && division->can_throw && method.tries.empty()) ||
                conditionalBranch(kind) || kind == "goto" ||
                kind == "new-array" || kind == "aput-object" ||
                (kind == "filled-new-array" &&
                 constructorPrefixFilledArray(method, at)) ||
                (kind == "const-class" &&
                 constructorPrefixClassLiteral(method, op)) ||
                (kind.starts_with("sget") &&
                 constructorPrefixStaticRead(method, op, classes, budget)) ||
                (kind.starts_with("iget") &&
                 constructorPrefixInstanceRead(method, op, classes, budget)) ||
                (ref && !ref->name.starts_with('<') &&
                 (kind == "invoke-static" || kind == "invoke-virtual" ||
                  kind == "invoke-interface" ||
                  constructorPrefixValueCall(op))))) {
            valid = false;
            break;
          }
        }
        const bool branch = conditionalBranch(kind) || kind == "goto";
        if (branch != op.target.has_value()) {
          valid = false;
          break;
        }
        if (branch) {
          const auto edge = positions.find(*op.target);
          if (edge == positions.end() || edge->second == at ||
              code[edge->second].opcode.starts_with("move-result")) {
            valid = false;
            break;
          }
          guarded = true;
          merge(edge->second, state);
        }
        if (kind != "goto")
          merge(at + 1, state);
      }
      for (size_t at = 0; at <= delegation; ++at) {
        budget.tick();
        if (!incoming[at])
          valid = false;
      }
      if (!valid || call->target || !incoming[delegation] ||
          (*incoming[delegation])[call->registers[0]] != Self ||
          std::any_of(
              call->registers.begin() + 1, call->registers.end(),
              [&](unsigned r) { return (*incoming[delegation])[r] & Self; }))
        continue;
      if (is_primary) {
        primary.insert(method.reference);
        // Keep superclass receipts separate from this-delegation edges: the
        // primary still owns and must validate its original final stores.
        // An earlier argument allocation can invoke the same descriptor.
        if ((guarded || delegation != 0) && super_prologues)
          super_prologues->emplace(
              method.reference,
              ConstructorSuperPrologue{*target, call->pc, guarded});
        continue;
      }
      // The selected prefix has its own bounded proof. An initialized linear
      // continuation is checked under the ordinary budget; its length supplies
      // no evidence about whether uninitialized this escaped before the call.
      // A linear prefix and continuation share the original register state.
      // Continuation edges must stay after initialization; Body owns their
      // reachability, operand types and original operation semantics.
      // Guarded prefixes export their converged argument and scalar words at
      // this same selected boundary. They share the continuation edge proof;
      // Body rejects any suffix operand without a retained typed origin.
      for (size_t at = delegation + 1; at < code.size(); ++at) {
        budget.tick();
        const auto &op = code[at];
        if (code[at - 1].pc >= op.pc || op.pc >= method.code_end ||
            !positions.emplace(op.pc, at).second || !op.targets.empty() ||
            !op.keys.empty() || !op.data.empty() || op.element_width ||
            std::any_of(op.registers.begin(), op.registers.end(),
                        [&](unsigned r) { return r >= method.registers; }) ||
            (at + 1 != code.size() && (op.opcode.starts_with("return") ||
                                       op.opcode.starts_with("throw")))) {
          valid = false;
          break;
        }
      }
      for (size_t at = delegation + 1; valid && at < code.size(); ++at) {
        budget.tick();
        const auto &op = code[at];
        const auto kind = op.opcode.substr(0, op.opcode.find('/'));
        const bool branch = kind == "goto" || conditionalBranch(kind);
        if (branch != op.target.has_value()) {
          valid = false;
          break;
        }
        if (branch) {
          const auto edge = positions.find(*op.target);
          if (edge == positions.end() || edge->second <= delegation ||
              code[edge->second].opcode.starts_with("move-result"))
            valid = false;
        }
      }
      if (valid)
        candidates.emplace(method.reference, *target);
    }
  }
  // Cycles and missing endpoints never excuse blank-final writes.
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[caller, target] : candidates) {
      budget.tick();
      if (!result.contains(caller) &&
          (primary.contains(target) || result.contains(target))) {
        result.emplace(caller, target);
        changed = true;
      }
    }
  }
  return result;
}

CapturedInnerClasses
planCapturedInnerClasses(const ClassMap &classes, Budget &budget,
                         const ConstructorCaptures *captures) {
  const auto owned = captures ? ConstructorCaptures{}
                              : planConstructorCaptures(classes, budget);
  const auto &proofs = captures ? *captures : owned;
  CapturedInnerClasses result;
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (cls.external || !cls.inner_class_present || !cls.inner_name ||
        !cls.enclosing || cls.enclosing_method ||
        has(cls.inner_access, "static") || has(cls.access, "interface") ||
        has(cls.access, "enum"))
      continue;
    std::optional<FieldRef> capture;
    bool proven = true;
    for (const auto &method : cls.methods) {
      budget.tick();
      if (method.reference.name != "<init>")
        continue;
      const auto proof = proofs.find(method.reference);
      if (proof == proofs.end() || !proof->second.contains(0) ||
          proof->second.at(0).type != *cls.enclosing ||
          (capture && *capture != proof->second.at(0))) {
        proven = false;
        break;
      }
      capture = proof->second.at(0);
    }
    if (proven && capture)
      result.emplace(name, *capture);
  }
  return result;
}

const Class *localSourceOwner(const std::string &type, const ClassMap &classes,
                              Budget &budget) {
  auto start = type.find_first_not_of('[');
  auto found =
      start == type.npos ? classes.end() : classes.find(type.substr(start));
  std::set<std::string> seen;
  while (found != classes.end() && !found->second.external) {
    budget.tick();
    const auto &cls = found->second;
    if (!seen.insert(cls.name).second)
      throw Error("recursive nested-class ownership");
    if (seen.size() > 128)
      throw Error("nested-class ownership exceeds the source depth limit");
    if (cls.enclosing_method)
      return &cls;
    found = cls.enclosing ? classes.find(*cls.enclosing) : classes.end();
  }
  return nullptr;
}

bool localClassVisible(const Class &local, const Class &context,
                       const MethodRef *method, const ClassMap &classes,
                       Budget &budget) {
  if (method && local.enclosing_method &&
      context.name == local.enclosing_method->owner &&
      *method == *local.enclosing_method)
    return true;
  const Class *scope = &context;
  std::set<std::string> seen;
  while (scope) {
    budget.tick();
    if (!seen.insert(scope->name).second || seen.size() > 128)
      throw Error("cyclic or too deep local class ownership");
    if (scope->name == local.name)
      return true;
    auto parent =
        scope->enclosing_method
            ? std::optional<std::string>(scope->enclosing_method->owner)
            : scope->enclosing;
    if (!parent)
      break;
    auto found = classes.find(*parent);
    scope = found == classes.end() ? nullptr : &found->second;
  }
  return false;
}

SharedAnonymousClasses planSharedAnonymousClasses(const ClassMap &classes,
                                                  Budget &budget) {
  SharedAnonymousClasses uses, result;
  std::set<std::string> shared, self_allocating;
  std::map<std::string, const Class *> roots;
  std::map<std::string, std::set<std::string>> children, references;
  std::map<std::string, std::set<std::string>> subclasses;
  for (const auto &[name, cls] : classes) {
    auto parent = cls.enclosing_method
                      ? std::optional<std::string>(cls.enclosing_method->owner)
                      : cls.enclosing;
    // A local singleton in an instance scope cannot construct itself in its
    // static initializer. A static member of its source host can do so.
    if (!cls.external && isAnonymousLocal(cls))
      for (const auto &method : cls.methods) {
        if (!has(method.access, "static"))
          continue;
        for (const auto &op : method.instructions) {
          budget.tick();
          const auto *type = std::get_if<std::string>(&op.reference);
          if (op.opcode == "new-instance" && type && *type == name)
            self_allocating.insert(name);
        }
      }
    if (!cls.external && parent)
      children[*parent].insert(name);
    if (!cls.external && cls.superclass && !isAnonymous(cls) &&
        !cls.enclosing_method && !has(cls.access, "interface"))
      subclasses[*cls.superclass].insert(name);
  }
  auto root = [&](const Class &cls) -> const Class * {
    if (auto found = roots.find(cls.name); found != roots.end())
      return found->second;
    const Class *current = &cls;
    std::set<std::string> seen;
    while (current) {
      budget.tick();
      if (!seen.insert(current->name).second || seen.size() > 128)
        throw Error("cyclic or too deep shared anonymous ownership");
      // A named ordinary host may itself be a member of an interface.
      // Stop there if its name is available throughout its package.
      bool package_accessible = true;
      const Class *host = current;
      std::set<std::string> host_seen;
      while (host && host->enclosing) {
        budget.tick();
        if (!host_seen.insert(host->name).second || host_seen.size() > 128)
          throw Error("cyclic or too deep shared anonymous host");
        if (has(host->inner_access, "private"))
          package_accessible = false;
        auto found = classes.find(*host->enclosing);
        host = found == classes.end() ? nullptr : &found->second;
      }
      if (current != &cls && !isAnonymous(*current) &&
          (!has(current->access, "interface") || self_allocating.contains(cls.name)) &&
          !localSourceOwner(current->name, classes, budget) &&
          package_accessible && host)
        break;
      auto parent =
          current->enclosing_method
              ? std::optional<std::string>(current->enclosing_method->owner)
              : current->enclosing;
      if (!parent)
        break;
      auto found = classes.find(*parent);
      current = found == classes.end() ? nullptr : &found->second;
    }
    roots.emplace(cls.name, current);
    return current;
  };
  auto package = [](const std::string &name) {
    auto slash = name.rfind('/');
    return slash == name.npos ? std::string() : name.substr(0, slash);
  };
  std::map<std::string, std::string> public_qualifiers;
  auto publicQualifier = [&](const Class &owner) -> std::string {
    if (auto found = public_qualifiers.find(owner.name);
        found != public_qualifiers.end())
      return found->second;
    std::vector<const Class *> pending{&owner};
    std::set<std::string> seen;
    for (size_t next = 0; next < pending.size(); ++next) {
      budget.tick();
      const auto &candidate = *pending[next];
      if (!seen.insert(candidate.name).second)
        continue;
      // Retain a same-package, known superclass chain. No dependency class
      // or inaccessible intermediate source declaration is invented.
      if (candidate.external || package(candidate.name) != package(owner.name))
        continue;
      const Class *host = &candidate;
      std::set<std::string> enclosing;
      while (host && host->enclosing) {
        budget.tick();
        if (!enclosing.insert(host->name).second || enclosing.size() > 128)
          throw Error("cyclic or too deep shared anonymous qualifier");
        if (!host->inner_name || !has(host->inner_access, "public")) {
          host = nullptr;
          break;
        }
        auto found = classes.find(*host->enclosing);
        host = found == classes.end() ? nullptr : &found->second;
      }
      if (host && !host->external && !host->enclosing_method &&
          !isAnonymous(*host) && has(host->access, "public")) {
        public_qualifiers.emplace(owner.name, candidate.name);
        return candidate.name;
      }
      if (auto found = subclasses.find(candidate.name);
          found != subclasses.end())
        for (const auto &child : found->second)
          pending.push_back(&classes.at(child));
    }
    public_qualifiers.emplace(owner.name, std::string());
    return {};
  };
  for (const auto &[name, cls] : classes) {
    if (cls.external)
      continue;
    // Visibility depends on the immutable type, exact method and whether this
    // is a declaration header or a body use. Repeated bytecode references add
    // the same set facts, so inspect each context once. The instruction walk
    // still charges every instruction against the shared work budget.
    std::map<const MethodRef *, std::array<std::set<std::string>, 2>> checked;
    auto typeUse = [&](const std::string &type, const MethodRef *method,
                       bool declaration) {
      if (!checked[method][declaration].insert(type).second)
        return;
      budget.tick();
      const auto *local_owner = localSourceOwner(type, classes, budget);
      if (!local_owner || !isAnonymousLocal(*local_owner))
        return;
      const auto &local = *local_owner;
      const auto *owner = root(local);
      if (!owner || owner->external ||
          (has(owner->access, "interface") && !self_allocating.contains(local.name)))
        return;
      const auto qualifier = publicQualifier(*owner);
      if (package(name) != package(owner->name)) {
        // A public inherited member can be named through a public subclass
        // even when its declaring host is package-private (JLS 6.6.1, 8.5).
        // Named member suffixes retain their own access.
        if (!has(local.access, "public") || qualifier.empty())
          return;
        auto start = type.find_first_not_of('[');
        auto member = classes.find(type.substr(start));
        while (member != classes.end() && member->first != local.name) {
          budget.tick();
          if (!has(member->second.inner_access, "public") ||
              !member->second.enclosing)
            return;
          member = classes.find(*member->second.enclosing);
        }
        if (member == classes.end())
          return;
      }
      auto &use = uses[local.name];
      use.source_owner = owner->name;
      use.reference_owner = qualifier.empty() ? owner->name : qualifier;
      references[name].insert(local.name);
      if (method)
        use.methods.insert(*method);
      if (declaration)
        use.declarations.insert(name);
      if (!localClassVisible(local, cls, declaration ? nullptr : method,
                             classes, budget))
        shared.insert(local.name);
    };
    if (cls.superclass)
      typeUse(*cls.superclass, nullptr, true);
    for (const auto &type : cls.interfaces)
      typeUse(type, nullptr, true);
    for (const auto &field : cls.fields)
      typeUse(field.reference.type, nullptr, true);
    for (const auto &method : cls.methods) {
      typeUse(method.reference.returns, &method.reference, true);
      for (const auto &type : method.reference.parameters)
        typeUse(type, &method.reference, true);
      for (const auto &region : method.tries)
        for (const auto &handler : region.handlers)
          if (handler.type)
            typeUse(*handler.type, &method.reference, true);
      for (const auto &op : method.instructions) {
        budget.tick();
        if (auto type = std::get_if<std::string>(&op.reference))
          typeUse(*type, &method.reference, false);
        else if (auto field = std::get_if<FieldRef>(&op.reference)) {
          typeUse(field->owner, &method.reference, false);
          typeUse(field->type, &method.reference, false);
        } else if (auto ref = std::get_if<MethodRef>(&op.reference)) {
          typeUse(ref->owner, &method.reference, false);
          typeUse(ref->returns, &method.reference, false);
          for (const auto &type : ref->parameters)
            typeUse(type, &method.reference, false);
        }
      }
    }
  }
  for (const auto &name : self_allocating)
    if (uses.contains(name))
      shared.insert(name);
  // Relocation must also carry any enclosing local types stored or referenced
  // by the moved body. Keep their one original identity, never clone a class
  // into each caller's method or leave an out-of-scope capture type behind.
  std::vector<std::string> pending(shared.begin(), shared.end());
  for (size_t next = 0; next < pending.size(); ++next) {
    const auto name = pending[next];
    std::vector<std::string> scopes{name};
    for (size_t i = 0; i < scopes.size(); ++i) {
      budget.tick();
      const auto current = scopes[i];
      if (auto found = references.find(current); found != references.end())
        for (const auto &type : found->second) {
          if (!shared.contains(type) &&
              !localClassVisible(classes.at(name), classes.at(type), nullptr,
                                 classes, budget)) {
            shared.insert(type);
            pending.push_back(type);
          }
        }
      if (auto found = children.find(current); found != children.end())
        scopes.insert(scopes.end(), found->second.begin(), found->second.end());
      if (scopes.size() > classes.size())
        throw Error("cyclic shared anonymous descendants");
    }
  }
  for (const auto &name : shared)
    result.emplace(name, std::move(uses.at(name)));
  return result;
}

std::map<std::string, std::string> planLocalClassNames(const ClassMap &classes,
                                                       Budget &budget) {
  const auto members = planNamedAnonymousMembers(classes, budget);
  std::set<std::string> used;
  std::map<std::string, std::string> result;
  for (const auto &[name, cls] : classes) {
    budget.tick();
    const auto slash = name.rfind('/');
    const auto start = slash == name.npos ? 1 : slash + 1;
    used.insert(name.substr(start, name.size() - start - 1));
    if (cls.inner_name)
      used.insert(*cls.inner_name);
    if (!cls.external && cls.enclosing_method && cls.inner_name)
      result.emplace(name, *cls.inner_name);
  }
  unsigned ordinal = 0;
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (!isAnonymousLocal(cls) && !isNamedAnonymousMember(cls, members))
      continue;
    std::string alias;
    do {
      budget.tick();
      alias = "__neverdLocal" + std::to_string(++ordinal);
    } while (!used.insert(alias).second);
    result.emplace(name, std::move(alias));
  }
  return result;
}

unsigned width(std::string_view type) {
  return type == "J" || type == "D" ? 2 : 1;
}

std::string descriptor(std::string_view value, bool allow_void) {
  if (value.empty() || value.size() > 65535)
    throw Error("invalid Dalvik type descriptor");
  size_t depth = 0;
  while (depth < value.size() && value[depth] == '[')
    ++depth;
  auto leaf = value.substr(depth);
  if (depth > 255 || leaf.empty())
    throw Error("invalid Dalvik type descriptor");
  if (leaf.size() == 1) {
    if (std::string_view("ZBCSIJFD").find(leaf[0]) != std::string_view::npos ||
        (leaf == "V" && !depth && allow_void))
      return std::string(value);
    throw Error("invalid Dalvik primitive descriptor");
  }
  if (leaf.front() != 'L' || leaf.back() != ';' || leaf.size() < 3)
    throw Error("invalid Dalvik class descriptor");
  auto body = leaf.substr(1, leaf.size() - 2);
  if (body.front() == '/' || body.back() == '/' ||
      body.find("//") != std::string_view::npos)
    throw Error("empty Dalvik package or class name");
  for (size_t pos = 0; pos < body.size();) {
    uint32_t c = static_cast<unsigned char>(body[pos++]);
    unsigned following = 0;
    if (c >= 0xc2 && c <= 0xdf) {
      following = 1;
      c &= 0x1f;
    } else if (c >= 0xe0 && c <= 0xef) {
      following = 2;
      c &= 0x0f;
    } else if (c >= 0xf0 && c <= 0xf4) {
      following = 3;
      c &= 0x07;
    } else if (c >= 0x80) {
      throw Error("invalid encoding in Dalvik class descriptor");
    }
    if (following > body.size() - pos)
      throw Error("truncated encoding in Dalvik class descriptor");
    for (unsigned i = 0; i < following; ++i) {
      auto next = static_cast<unsigned char>(body[pos++]);
      if ((next & 0xc0) != 0x80)
        throw Error("invalid encoding in Dalvik class descriptor");
      c = (c << 6) | (next & 63);
    }
    // Preserve WTF-8 surrogate code units while rejecting malformed encodings.
    if ((following == 1 && c < 0x80) || (following == 2 && c < 0x800) ||
        (following == 3 && c < 0x10000) || c > 0x10ffff)
      throw Error("invalid encoding in Dalvik class descriptor");
    bool unicode_space =
        c == 0x85 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
        c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
    if (c <= 0x20 || c == 0x7f || unicode_space || c == ';' || c == '[' ||
        c == '.')
      throw Error("invalid character in Dalvik class descriptor");
  }
  return std::string(value);
}

std::pair<std::vector<std::string>, std::string>
prototype(std::string_view value) {
  if (value.empty() || value.front() != '(')
    throw Error("invalid Dalvik method prototype");
  auto close = value.find(')');
  if (close == std::string_view::npos)
    throw Error("invalid Dalvik method prototype");
  std::vector<std::string> parameters;
  size_t pos = 1;
  while (pos < close) {
    size_t start = pos;
    while (pos < close && value[pos] == '[')
      ++pos;
    if (pos == close)
      throw Error("invalid Dalvik parameter descriptor");
    if (value[pos] == 'L') {
      auto end = value.find(';', pos);
      if (end == std::string_view::npos || end >= close)
        throw Error("invalid Dalvik parameter descriptor");
      pos = end + 1;
    } else {
      ++pos;
    }
    parameters.push_back(descriptor(value.substr(start, pos - start)));
  }
  return {std::move(parameters), descriptor(value.substr(close + 1), true)};
}

std::string MethodRef::signature() const {
  std::string result = "(";
  for (const auto &p : parameters)
    result += p;
  return result + ")" + returns;
}
std::string MethodRef::identity() const {
  return owner + "->" + name + signature();
}
unsigned Method::incomingWords() const {
  uint64_t result = !has(access, "static");
  for (const auto &p : reference.parameters)
    result += width(p);
  if (result > 65535)
    throw Error("Dalvik incoming register frame exceeds its word limit");
  return static_cast<unsigned>(result);
}

MethodRef methodRef(std::string_view value) {
  auto arrow = value.find("->");
  auto open = value.find('(', arrow == std::string_view::npos ? 0 : arrow + 2);
  if (arrow == std::string_view::npos || open == std::string_view::npos ||
      open <= arrow + 2)
    throw Error("invalid Dalvik method reference");
  auto [parameters, returns] = prototype(value.substr(open));
  return {descriptor(value.substr(0, arrow)),
          std::string(value.substr(arrow + 2, open - arrow - 2)),
          std::move(parameters), std::move(returns)};
}

FieldRef fieldRef(std::string_view value) {
  auto arrow = value.find("->");
  auto colon = value.find(':', arrow == std::string_view::npos ? 0 : arrow + 2);
  if (arrow == std::string_view::npos || colon == std::string_view::npos ||
      colon <= arrow + 2)
    throw Error("invalid Dalvik field reference");
  return {descriptor(value.substr(0, arrow)),
          std::string(value.substr(arrow + 2, colon - arrow - 2)),
          descriptor(value.substr(colon + 1))};
}

Access accessFlags(uint32_t value) {
  static const std::pair<uint32_t, const char *> flags[] = {
      {1, "public"},
      {2, "private"},
      {4, "protected"},
      {8, "static"},
      {0x10, "final"},
      {0x20, "synchronized"},
      {0x40, "volatile"},
      {0x80, "transient"},
      {0x100, "native"},
      {0x200, "interface"},
      {0x400, "abstract"},
      {0x800, "strictfp"},
      {0x1000, "synthetic"},
      {0x2000, "annotation"},
      {0x4000, "enum"},
      {0x10000, "constructor"},
      {0x20000, "declared-synchronized"}};
  Access result;
  for (const auto &[bit, name] : flags) {
    if (value & bit)
      result.emplace(name);
    value &= ~bit;
  }
  if (value)
    throw Error("unknown Dalvik declaration access flags");
  return result;
}

namespace {
const Access visibility = {"public", "private", "protected"};
const Access classFlags = {"public",     "private",   "protected", "static",
                           "final",      "interface", "abstract",  "synthetic",
                           "annotation", "enum"};
const Access fieldFlags = {"public",    "private",   "protected",
                           "static",    "final",     "volatile",
                           "transient", "synthetic", "enum"};
const Access methodFlags = {"public",      "private",
                            "protected",   "static",
                            "final",       "synchronized",
                            "bridge",      "varargs",
                            "native",      "abstract",
                            "strictfp",    "synthetic",
                            "constructor", "declared-synchronized"};
bool any(const Access &flags, std::initializer_list<const char *> names) {
  return std::any_of(names.begin(), names.end(),
                     [&](auto name) { return has(flags, name); });
}
void checkFlags(const Access &flags, const Access &allowed,
                const std::string &id) {
  unsigned visible = 0;
  for (const auto &flag : flags) {
    if (!allowed.contains(flag))
      throw Error("invalid declaration access flags: " + id);
    visible += visibility.contains(flag);
  }
  if (visible > 1)
    throw Error("conflicting declaration visibility: " + id);
}
[[noreturn]] void scopeError(const Class &cls, const std::string &message) {
  std::string context =
      "Android class " + cls.name + " from " +
      (cls.source_id.empty() ? "<unspecified>" : cls.source_id);
  if (cls.enclosing_method)
    context += " enclosing " + cls.enclosing_method->identity();
  else if (cls.enclosing)
    context += " enclosing " + *cls.enclosing;
  throw Error(context + ": " + message);
}
bool scalarType(const std::string &type, bool allow_void = false) {
  return type.size() == 1 && (std::string_view("ZBCSIJFD").find(type[0]) !=
                                  std::string_view::npos ||
                              (allow_void && type == "V"));
}
void scalarSignature(const Class &cls, const MethodRef &ref, Budget &budget) {
  if (!scalarType(ref.returns, true))
    scopeError(cls, "method-local projection requires a scalar return: " +
                        ref.identity());
  for (const auto &parameter : ref.parameters) {
    budget.tick();
    if (!scalarType(parameter))
      scopeError(cls, "method-local projection requires scalar parameters: " +
                          ref.identity());
  }
}
void checkSourceScope(const Class &cls, Budget &budget) {
  if (cls.enclosing && cls.enclosing_method)
    scopeError(cls, "conflicting class and method enclosing contexts");
  // Dependency declarations are not projected into Java source. Their
  // ownership facts still participate in descriptor-based name lookup.
  if (cls.external)
    return;
  if (!cls.inner_class_present) {
    if (cls.enclosing || cls.enclosing_method || cls.inner_name ||
        !cls.inner_access.empty())
      scopeError(cls, "enclosing metadata requires an InnerClass annotation");
    return;
  }
  if (!cls.enclosing && !cls.enclosing_method)
    scopeError(cls, "InnerClass annotation has no enclosing context");
  if (isAnonymous(cls) && cls.enclosing && !cls.enclosing_method)
    return; // The complete model proves the unique static allocation site.
  if (!cls.inner_name && !isAnonymousLocal(cls))
    scopeError(cls, "anonymous class source context is unsupported");
  if (cls.inner_name && cls.inner_name->empty())
    scopeError(cls, "inner class source name is empty");
  if (!cls.enclosing_method)
    return;

  const auto &enclosing = *cls.enclosing_method;
  if (!enclosing.owner.starts_with('L') || enclosing.owner == cls.name ||
      enclosing.name.empty() ||
      (enclosing.name.starts_with('<') &&
       !(isAnonymousLocal(cls) && enclosing.name == "<init>")))
    scopeError(cls, "method-local class needs an ordinary enclosing method");
  // A hand-constructed model must have the same typed identity as a reader.
  bool valid_reference = false;
  try {
    valid_reference = methodRef(enclosing.identity()) == enclosing;
  } catch (const Error &) {
  }
  if (!valid_reference)
    scopeError(cls, "invalid typed EnclosingMethod reference");
  if (isAnonymousLocal(cls)) {
    if (any(cls.access, {"interface", "annotation", "enum", "abstract"}))
      scopeError(
          cls,
          "anonymous local projection requires an ordinary concrete class");
    unsigned constructors = 0;
    for (const auto &method : cls.methods) {
      budget.tick();
      if (method.reference.owner != cls.name ||
          any(method.access, {"native", "abstract"}) ||
          method.instructions.empty())
        scopeError(
            cls,
            "anonymous local projection requires original concrete bodies");
      constructors += method.reference.name == "<init>";
    }
    if (!constructors)
      scopeError(cls,
                 "anonymous local projection requires an original constructor");
    return;
  }
  for (const auto *flags : {&cls.access, &cls.inner_access})
    for (const auto &flag : *flags) {
      budget.tick();
      if (flag != "final" && flag != "synthetic")
        scopeError(cls, "Java 8 local class permits only final/synthetic "
                        "access flags");
    }
  if (has(cls.access, "final") != has(cls.inner_access, "final"))
    scopeError(cls,
               "local class final flag disagrees with InnerClass metadata");
  if (cls.superclass != "Ljava/lang/Object;" || !cls.interfaces.empty())
    scopeError(cls, "method-local projection requires direct Object "
                    "inheritance without interfaces");
  if (!cls.fields.empty())
    scopeError(cls, "method-local fields or capture storage are unsupported");
  unsigned constructors = 0;
  for (const auto &method : cls.methods) {
    budget.tick();
    const auto &ref = method.reference;
    if (ref.owner != cls.name)
      scopeError(cls, "local method owner disagrees with its declaration: " +
                          ref.identity());
    if (ref.name == "<clinit>")
      scopeError(cls, "method-local static initializer is unsupported");
    if (any(method.access, {"static", "native", "abstract"}) ||
        method.instructions.empty())
      scopeError(cls, "method-local methods require real instance bodies: " +
                          ref.identity());
    if (ref.name == "<init>") {
      ++constructors;
      if (!ref.parameters.empty() || ref.returns != "V")
        scopeError(cls, "method-local constructor must be no-argument; "
                        "captured constructor arguments are unsupported");
    } else {
      if (ref.name.empty() || ref.name.starts_with('<'))
        scopeError(cls,
                   "invalid ordinary local method identity: " + ref.identity());
      scalarSignature(cls, ref, budget);
    }
  }
  if (constructors != 1)
    scopeError(cls, "method-local class requires exactly one real "
                    "no-argument constructor");
}
void checkOwnedMethodDeclaration(const Method &method) {
  const auto &ref = method.reference;
  if (!method.instructions.empty() &&
      any(method.access, {"abstract", "native"}))
    throw Error("abstract or native method declares instructions: " +
                ref.identity());
  if (has(method.access, "varargs") &&
      (ref.parameters.empty() || !ref.parameters.back().starts_with('[')))
    throw Error("varargs method requires a final array parameter: " +
                ref.identity());
}

void checkFieldDeclarations(const Class &cls, Budget &budget) {
  const bool interface = has(cls.access, "interface");
  for (const auto &field : cls.fields) {
    budget.tick();
    checkFlags(field.access, fieldFlags, field.reference.name);
    if (field.reference.owner != cls.name)
      throw Error("field owner disagrees with its declaration");
    descriptor(field.reference.type);
    if (has(field.access, "final") && has(field.access, "volatile"))
      throw Error("field cannot be both final and volatile");
    if (interface &&
        (!has(field.access, "public") || !has(field.access, "static") ||
         !has(field.access, "final") ||
         any(field.access, {"volatile", "transient"})))
      throw Error("invalid interface field declaration");
  }
}

void checkClass(const Class &cls, Budget &budget) {
  checkFlags(cls.access, classFlags, cls.name);
  if (!descriptor(cls.name).starts_with('L'))
    throw Error("class declaration requires a class descriptor");
  if (!cls.superclass) {
    if (cls.name != "Ljava/lang/Object;")
      throw Error("class has no superclass");
  } else if (!descriptor(*cls.superclass).starts_with('L')) {
    throw Error("invalid superclass descriptor");
  }
  Access interfaces;
  for (const auto &interface : cls.interfaces) {
    budget.tick();
    if (!descriptor(interface).starts_with('L') ||
        !interfaces.insert(interface).second)
      throw Error("invalid or duplicate declared interface");
  }
  std::vector<const Access *> declarations = {&cls.access};
  if (cls.enclosing || cls.enclosing_method) {
    checkFlags(cls.inner_access, classFlags, cls.name);
    declarations.push_back(&cls.inner_access);
  } else if (any(cls.access, {"private", "protected", "static"})) {
    throw Error("top-level class has nested-only access flags");
  }
  for (const auto *flags : declarations) {
    if (has(*flags, "final") && any(*flags, {"abstract", "interface"}))
      throw Error("class cannot be both final and abstract/interface");
    if (has(*flags, "interface") && !has(*flags, "abstract"))
      throw Error("interface must be abstract");
    if (has(*flags, "annotation") && !has(*flags, "interface"))
      throw Error("annotation must be an interface");
  }
  bool interface = has(cls.access, "interface");
  if (interface && cls.superclass != "Ljava/lang/Object;")
    throw Error("interface has a non-Object superclass");
  checkFieldDeclarations(cls, budget);
  for (const auto &method : cls.methods) {
    budget.tick();
    const auto &ref = method.reference;
    const auto &flags = method.access;
    checkFlags(flags, methodFlags, ref.identity());
    if (ref.owner != cls.name)
      throw Error("method owner disagrees with its declaration");
    descriptor(ref.returns, true);
    for (const auto &parameter : ref.parameters) {
      budget.tick();
      descriptor(parameter);
    }
    // A compiler may append implicit capture parameters to a varargs
    // constructor. Opaque dependencies need no Java source spelling.
    if (!cls.external)
      checkOwnedMethodDeclaration(method);
    if (has(flags, "abstract")) {
      if (!has(cls.access, "abstract"))
        throw Error("concrete class declares an abstract method");
      if (any(flags, {"static", "private", "final", "native", "synchronized",
                      "strictfp", "constructor", "declared-synchronized"}))
        throw Error("incompatible abstract method flags");
    }
    if (has(flags, "native") && has(flags, "strictfp"))
      throw Error("native method cannot be strictfp");
    if (interface && any(flags, {"protected", "final", "synchronized", "native",
                                 "declared-synchronized"}))
      throw Error("invalid interface method flags");
    if (ref.name == "<init>") {
      checkFlags(flags,
                 {"public", "private", "protected", "constructor", "synthetic",
                  "varargs"},
                 ref.identity());
      if (ref.returns != "V" || interface)
        throw Error("invalid instance initializer declaration");
    } else if (ref.name == "<clinit>") {
      // DEX class initializers may carry visibility flags (D8 emits public
      // resource initializers). They have no Java static-block spelling.
      checkFlags(flags,
                 {"public", "private", "protected", "static", "constructor",
                  "synthetic", "strictfp"},
                 ref.identity());
      if (!ref.parameters.empty() || ref.returns != "V" ||
          !has(flags, "static"))
        throw Error("invalid class initializer declaration");
    } else if (ref.name.starts_with('<') || has(flags, "constructor")) {
      throw Error("invalid constructor identity");
    }
  }
}

SharedAnonymousClasses linkLocalScopes(const ClassMap &classes,
                                       Budget &budget) {
  const auto members = planNamedAnonymousMembers(classes, budget);
  const auto captured = planCapturedInnerClasses(classes, budget);
  const auto source_names = planLocalClassNames(classes, budget);
  auto shared = planSharedAnonymousClasses(classes, budget);
  std::map<std::string, const Class *> locals;
  std::map<MethodRef, std::pair<const Method *, const Class *>> definitions;
  std::map<MethodRef, std::set<std::string>> names;
  std::set<std::string> owners;
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (cls.external || !cls.enclosing_method)
      continue;
    const auto &ref = *cls.enclosing_method;
    locals.emplace(name, &cls);
    definitions.emplace(ref, std::pair{nullptr, &cls});
    owners.insert(ref.owner);
    if (!names[ref].insert(source_names.at(name)).second)
      scopeError(cls, "duplicate local source name in one enclosing method");
  }
  if (locals.empty())
    return shared;
  // Resolve each needed owner's method table once, including cross-DEX owners.
  for (const auto &owner : owners) {
    budget.tick();
    const auto found = classes.find(owner);
    if (found == classes.end())
      continue;
    for (const auto &method : found->second.methods) {
      budget.tick();
      if (auto entry = definitions.find(method.reference);
          entry != definitions.end()) {
        if (entry->second.first)
          scopeError(*entry->second.second,
                     "exact enclosing method definition is duplicated");
        entry->second.first = &method;
      }
    }
  }
  for (const auto &[name, cls] : locals) {
    budget.tick();
    const auto &ref = *cls->enclosing_method;
    const auto *method = definitions.at(ref).first;
    if (!method)
      scopeError(*cls, "exact enclosing method definition is unavailable");
    if (classes.at(ref.owner).enclosing_method &&
        !(isAnonymousLocal(*cls) && isAnonymousLocal(classes.at(ref.owner))))
      scopeError(*cls, "nested method-local contexts are unsupported");
    const Class *ancestor = cls;
    std::set<std::string> seen;
    while (ancestor) {
      budget.tick();
      if (!seen.insert(ancestor->name).second || seen.size() > 128)
        scopeError(*cls, "cyclic or too deep local class ownership");
      auto parent =
          ancestor->enclosing_method
              ? std::optional<std::string>(ancestor->enclosing_method->owner)
              : ancestor->enclosing;
      if (!parent)
        break;
      auto found = classes.find(*parent);
      if (found == classes.end())
        scopeError(*cls, "unresolved local enclosing class");
      ancestor = &found->second;
    }
    if ((!isAnonymousLocal(*cls) && !has(method->access, "static")) ||
        any(method->access, {"native", "abstract"}) ||
        method->instructions.empty())
      scopeError(*cls, "enclosing method must be ordinary static with a body");
    if (!isAnonymousLocal(*cls))
      scalarSignature(*cls, ref, budget);
  }
  auto localType = [&](const std::string &type) -> const Class * {
    return localSourceOwner(type, classes, budget);
  };
  auto declarationType = [&](const std::string &type, const Class &context,
                             const std::string &where) {
    if (const auto *local = localType(type);
        local &&
        !(shared.contains(local->name) &&
          shared.at(local->name).declarations.contains(context.name)) &&
        !(isAnonymousLocal(*local) &&
          localClassVisible(*local, context, nullptr, classes, budget)))
      scopeError(*local, "local type is unavailable in a declaration or "
                         "handler: " +
                             where);
  };
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (cls.external)
      continue;
    if (cls.enclosing) {
      if (const auto *local = localType(*cls.enclosing);
          local &&
          (!isAnonymousLocal(*local) ||
           (!isNamedAnonymousMember(cls, members) && !captured.contains(name) &&
            !isStaticOnlyInnerMember(cls) &&
            (!has(cls.inner_access, "static") || !cls.inner_name))))
        scopeError(*local,
                   "method-local nested descendants are unsupported: " + name);
    }
    if (cls.superclass)
      declarationType(*cls.superclass, cls, name + " superclass");
    for (const auto &interface : cls.interfaces)
      declarationType(interface, cls, name + " interface");
    for (const auto &field : cls.fields)
      declarationType(field.reference.type, cls,
                      name + "->" + field.reference.name);
    for (const auto &method : cls.methods) {
      budget.tick();
      const auto identity = method.reference.identity();
      declarationType(method.reference.returns, cls, identity);
      for (const auto &type : method.reference.parameters)
        declarationType(type, cls, identity);
      for (const auto &region : method.tries)
        for (const auto &handler : region.handlers)
          if (handler.type)
            declarationType(*handler.type, cls,
                            identity + " exception handler");
      auto instructionType = [&](const std::string &type) {
        if (const auto *local = localType(type))
          if (!(shared.contains(local->name) &&
                shared.at(local->name).methods.contains(method.reference)) &&
              !localClassVisible(*local, cls, &method.reference, classes,
                                 budget))
            scopeError(*local, "local type reference is outside its exact "
                               "method scope: " +
                                   identity);
      };
      for (const auto &instruction : method.instructions) {
        budget.tick();
        if (const auto *type =
                std::get_if<std::string>(&instruction.reference)) {
          instructionType(*type);
        } else if (const auto *field =
                       std::get_if<FieldRef>(&instruction.reference)) {
          instructionType(field->owner);
          instructionType(field->type);
        } else if (const auto *ref =
                       std::get_if<MethodRef>(&instruction.reference)) {
          instructionType(ref->owner);
          instructionType(ref->returns);
          for (const auto &type : ref->parameters)
            instructionType(type);
        }
      }
    }
  }
  return shared;
}
std::vector<const Class *>
annotationScopes(const Class &cls, const ClassMap &classes, Budget &budget) {
  std::set<std::string> seen;
  std::vector<const Class *> result;
  const Class *scope = &cls;
  while (scope) {
    budget.tick();
    if (!seen.insert(scope->name).second)
      scopeError(cls, "recursive marker annotation ownership");
    checkClass(*scope, budget);
    if (scope->enclosing_method ||
        (scope->inner_class_present && !scope->inner_name))
      scopeError(cls, "marker annotations require a proven non-local "
                      "declaration scope");
    result.push_back(scope);
    if (!scope->enclosing)
      break;
    auto parent = classes.find(*scope->enclosing);
    if (parent == classes.end())
      scopeError(cls, "unresolved marker annotation enclosing class");
    if (!scope->inner_class_present || !scope->inner_name ||
        scope->inner_name->empty() ||
        scope->name !=
            scope->enclosing->substr(0, scope->enclosing->size() - 1) + "$" +
                *scope->inner_name + ";")
      scopeError(cls, "marker declaration ownership changes its binary "
                      "identity");
    scope = &parent->second;
  }
  return result;
}
void checkAnnotationClass(const Class &cls, const ClassMap &classes,
                          const std::set<std::string> &owners, Budget &budget) {
  static const Access reserved = {"Ljava/lang/Deprecated;",
                                  "Landroid/annotation/SuppressLint;",
                                  "Ljava/lang/annotation/Retention;",
                                  "Ljava/lang/annotation/Target;",
                                  "Ljava/lang/annotation/Documented;",
                                  "Ljava/lang/annotation/Inherited;",
                                  "Ldalvik/annotation/Signature;",
                                  "Ldalvik/annotation/Throws;",
                                  "Ldalvik/annotation/InnerClass;",
                                  "Ldalvik/annotation/EnclosingClass;",
                                  "Ldalvik/annotation/EnclosingMethod;",
                                  "Ldalvik/annotation/MemberClasses;",
                                  "Ldalvik/annotation/AnnotationDefault;"};
  const auto &metadata = cls.annotation_metadata;
  bool annotation = has(cls.access, "annotation");
  if (!annotation && !metadata.empty())
    scopeError(cls, "standard annotation metadata requires an annotation "
                    "declaration");
  if (annotation) {
    checkClass(cls, budget);
    if (cls.name.starts_with("Ljava/"))
      scopeError(cls, "marker annotation requires a non-platform definition");
    if (cls.superclass != "Ljava/lang/Object;" ||
        cls.interfaces !=
            std::vector<std::string>{"Ljava/lang/annotation/Annotation;"} ||
        cls.generic_signature)
      scopeError(cls, "annotation requires Annotation inheritance without "
                      "class type parameters");
    for (const auto *flags : {&cls.access, &cls.inner_access}) {
      if (flags == &cls.inner_access && !cls.enclosing)
        continue;
      checkFlags(*flags,
                 {"public", "private", "protected", "static", "abstract",
                  "interface", "annotation"},
                 cls.name);
      if (!has(*flags, "annotation") || !has(*flags, "interface") ||
          !has(*flags, "abstract"))
        scopeError(cls, "marker annotation requires "
                        "annotation/interface/abstract flags");
    }
    if (cls.enclosing && !has(cls.inner_access, "static"))
      scopeError(cls, "member marker annotation requires static ownership");
  }
  if (metadata.retention &&
      !Access{"SOURCE", "CLASS", "RUNTIME"}.contains(*metadata.retention))
    scopeError(cls, "invalid marker annotation retention policy");
  if (metadata.targets) {
    Access seen;
    for (const auto &target : *metadata.targets) {
      budget.tick();
      if (!annotationTargetJavaRelease(target) || !seen.insert(target).second)
        scopeError(cls, "invalid or duplicate annotation target");
    }
  }
  std::vector<const Class *> scopes;
  if (annotation || !cls.marker_annotations.empty()) {
    scopes = annotationScopes(cls, classes, budget);
    for (const auto *type :
         {"Ljava/lang/annotation/Annotation;",
          "Ljava/lang/annotation/Retention;",
          "Ljava/lang/annotation/RetentionPolicy;",
          "Ljava/lang/annotation/Target;", "Ljava/lang/annotation/ElementType;",
          "Ljava/lang/annotation/Documented;",
          "Ljava/lang/annotation/Inherited;"})
      if (classes.contains(type) && !classes.at(type).external)
        scopeError(cls, "marker annotations require the platform definition: " +
                            std::string(type));
  }
  std::set<std::string> seen;
  for (const auto &use : cls.marker_annotations) {
    budget.tick();
    if (reserved.contains(use.type))
      scopeError(cls, "reserved marker annotation descriptor: " + use.type);
    if (!descriptor(use.type).starts_with('L') || !seen.insert(use.type).second)
      scopeError(cls, "invalid or duplicate marker annotation application");
    auto definition = classes.find(use.type);
    if (definition == classes.end() ||
        !has(definition->second.access, "annotation"))
      scopeError(cls, "unresolved marker annotation definition: " + use.type);
    auto definition_scopes =
        annotationScopes(definition->second, classes, budget);
    if (definition_scopes.back()->name != scopes.back()->name) {
      auto package = [](const Class &type) {
        auto slash = type.name.rfind('/');
        return slash == std::string::npos ? std::string()
                                          : type.name.substr(1, slash - 1);
      };
      for (const auto *scope : definition_scopes) {
        budget.tick();
        bool member = scope->enclosing.has_value();
        const auto &flags = member ? scope->inner_access : scope->access;
        if ((member || package(*scope) != package(*scopes.back())) &&
            !has(flags, "public"))
          scopeError(cls, "marker annotation definition is not accessible: " +
                              use.type);
      }
    }
    const auto &policy = definition->second.annotation_metadata;
    auto retention = policy.retention.value_or("CLASS");
    if (retention == "SOURCE" ||
        use.visibility != (retention == "RUNTIME" ? 1u : 0u))
      scopeError(cls, "marker annotation visibility disagrees with "
                      "retention: " +
                          use.type);
    if (policy.targets) {
      const auto &targets = *policy.targets;
      auto permits = [&](const char *target) {
        return std::find(targets.begin(), targets.end(), target) !=
               targets.end();
      };
      if (!permits("TYPE") && !permits("TYPE_USE") &&
          !(annotation && permits("ANNOTATION_TYPE")))
        scopeError(cls, "marker annotation target excludes this class "
                        "declaration: " +
                            use.type);
    }
  }
}
} // namespace

SourceScopePlans validateSourceScopes(const ClassMap &classes, Budget &budget) {
  std::set<std::string> owners;
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (name != cls.name)
      scopeError(cls, "class map key disagrees with its descriptor");
    // Annotation checks can inspect another class's definition or owners.
    // Validate every intrinsic scope before those dependent checks.
    checkFieldDeclarations(cls, budget);
    checkSourceScope(cls, budget);
    if (cls.enclosing) {
      if (!cls.external && !classes.contains(*cls.enclosing))
        scopeError(cls, "unresolved source enclosing class: " + *cls.enclosing);
      owners.insert(*cls.enclosing);
    }
    // linkLocalScopes resolves the complete enclosing method descriptor.
    if (cls.enclosing_method)
      owners.insert(cls.enclosing_method->owner);
  }
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (cls.external)
      continue;
    bool deprecated = cls.deprecated;
    bool suppress_lint = cls.suppress_lint.has_value();
    for (const auto &field : cls.fields) {
      budget.tick();
      deprecated |= field.deprecated;
      suppress_lint |= field.suppress_lint.has_value();
    }
    for (const auto &method : cls.methods) {
      budget.tick();
      checkOwnedMethodDeclaration(method);
      deprecated |= method.deprecated;
      suppress_lint |= method.suppress_lint.has_value();
      if (method.suppress_lint && method.reference.name == "<clinit>")
        scopeError(cls, "SuppressLint cannot annotate a class initializer: " +
                            method.reference.identity());
      if (method.deprecated && method.reference.name == "<clinit>")
        scopeError(cls, "Deprecated cannot annotate a class initializer: " +
                            method.reference.identity());
    }
    if (deprecated && classes.contains("Ljava/lang/Deprecated;") &&
        !classes.at("Ljava/lang/Deprecated;").external)
      scopeError(cls, "Deprecated requires the platform annotation definition");
    if (suppress_lint &&
        classes.contains("Landroid/annotation/SuppressLint;") &&
        !classes.at("Landroid/annotation/SuppressLint;").external)
      scopeError(cls,
                 "SuppressLint requires the platform annotation definition");
    checkAnnotationClass(cls, classes, owners, budget);
  }
  auto shared = linkLocalScopes(classes, budget);
  auto enums = planEnumClasses(classes, budget);
  planAnonymousClasses(classes, budget);
  validateAnnotations(classes, budget, &enums);
  planAnnotationFieldInitializers(classes, budget);
  return {std::move(shared), std::move(enums)};
}

ClassMap linkClasses(std::vector<Class> classes, Budget &budget) {
  ClassMap result;
  std::set<std::pair<std::string, bool>> definitions;
  for (auto &cls : classes) {
    budget.tick();
    if (!definitions.emplace(cls.name, cls.external).second)
      throw Error("duplicate Android class definition");
    checkClass(cls, budget);
    std::set<MethodRef> methods;
    std::set<FieldRef> fields;
    for (const auto &method : cls.methods) {
      budget.tick();
      if (!methods.insert(method.reference).second)
        throw Error("duplicate method definition");
      bool no_code =
          has(method.access, "abstract") || has(method.access, "native");
      if (cls.external && !method.instructions.empty())
        throw Error("dependency declaration contains executable input code");
      if (!cls.external && !no_code && method.instructions.empty())
        throw Error("method body/access mismatch");
      if (!cls.external && !no_code &&
          (method.registers < method.incomingWords() ||
           method.registers > 65535))
        throw Error("invalid register frame");
    }
    for (const auto &field : cls.fields)
      if (!fields.insert(field.reference).second)
        throw Error("duplicate field definition");
    auto key = cls.name;
    if (auto found = result.find(key);
        found != result.end() && found->second.external != cls.external) {
      // Input definitions override dependency declarations in either order.
      if (cls.external)
        continue;
      result.erase(found);
    }
    if (!result.emplace(std::move(key), std::move(cls)).second)
      throw Error("duplicate Android class definition");
  }
  std::map<std::string, std::vector<std::string>> parents;
  for (const auto &[name, cls] : result) {
    budget.tick();
    if (cls.superclass && result.contains(*cls.superclass)) {
      const auto &parent = result.at(*cls.superclass);
      if (any(parent.access, {"interface", "final"}))
        throw Error(name + ": invalid interface or final superclass " +
                    parent.name);
      parents[name].push_back(parent.name);
    }
    for (const auto &interface : cls.interfaces) {
      if (result.contains(interface)) {
        if (!has(result.at(interface).access, "interface"))
          throw Error("implemented type is not an interface");
        parents[name].push_back(interface);
      }
    }
  }
  std::map<std::string, unsigned> states;
  for (const auto &[root, cls] : result) {
    std::vector<std::pair<std::string, bool>> pending = {{root, false}};
    while (!pending.empty()) {
      budget.tick();
      auto [name, finish] = std::move(pending.back());
      pending.pop_back();
      if (finish) {
        states[name] = 2;
      } else if (states[name] != 2) {
        if (states[name] == 1)
          throw Error("cyclic class/interface inheritance");
        states[name] = 1;
        pending.emplace_back(name, true);
        for (const auto &parent : parents[name])
          pending.emplace_back(parent, false);
      }
    }
  }
  const auto scopes = validateSourceScopes(result, budget);
  validateGenericSignatures(result, budget, &scopes.enums, &scopes.shared);
  return result;
}
} // namespace neverd::mobile::dalvik
