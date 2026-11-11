//===- MobileDalvikEnums.cpp - Verified Java enum initialization
//-----------===//
#include "MobileDalvik.h"

#include <algorithm>
#include <tuple>

namespace neverd::mobile::dalvik {
bool isEnumConstantClass(const Class &cls, const ClassMap &classes) {
  if (cls.external || !cls.inner_class_present || !cls.enclosing ||
      cls.enclosing_method || cls.superclass != cls.enclosing ||
      (!cls.inner_name && !has(cls.access, "enum")))
    return false;
  const auto root = classes.find(*cls.enclosing);
  return root != classes.end() && !root->second.external &&
         has(root->second.access, "enum") &&
         root->second.superclass == "Ljava/lang/Enum;";
}
namespace {
[[noreturn]] void invalid(const std::string &where, const std::string &why) {
  throw Error("Android enum " + where + ": " + why);
}
struct Value {
  enum Kind {
    Unknown,
    Number,
    String,
    Parameter,
    Self,
    Object,
    Array,
    Constant,
    ClassLiteral,
    Clone,
    Lookup,
    Computed,
    Conditional,
    Uninitialized,
    High
  } kind = Unknown;
  int64_t number = 0;
  std::string text;
  bool wide = false;
  auto operator<=>(const Value &) const = default;
};
// All compiler-managed code is interpreted in a small, typed domain. Unknown
// producers, effects, control flow, aliases and mismatched carriers fail
// closed.
class Frame {
public:
  const Method &method;
  const ClassMap &classes;
  Budget &budget;
  std::vector<Value> registers;
  std::optional<Value> pending;
  std::string pending_opcode = "move-result-object";
  Frame(const Method &method, const ClassMap &classes, Budget &budget,
        bool linear = true, bool constructor_body_handlers = false)
      : method(method), classes(classes), budget(budget),
        registers(method.registers) {
    const auto &code = method.instructions;
    if (code.empty() || (!method.tries.empty() && !constructor_body_handlers) ||
        method.registers > 1024 || method.incomingWords() > method.registers ||
        method.code_end <= code.back().pc)
      fail("requires bounded original code without exception handlers");
    for (size_t i = 0; i < code.size(); ++i) {
      budget.tick();
      const auto &op = code[i];
      if ((i && code[i - 1].pc >= op.pc) ||
          (linear && (op.target || !op.targets.empty() || !op.keys.empty() ||
                      !op.data.empty())))
        fail("requires ordered linear instructions");
      for (auto reg : op.registers)
        if (reg >= registers.size())
          fail("register is outside its frame");
    }
    unsigned reg = method.registers - method.incomingWords();
    if (!has(method.access, "static"))
      put(reg++, {Value::Self, 0, method.reference.owner});
    for (size_t i = 0; i < method.reference.parameters.size(); ++i) {
      const auto &type = method.reference.parameters[i];
      put(reg, {Value::Parameter, int64_t(i), type, width(type) == 2});
      reg += width(type);
    }
  }
  [[noreturn]] void fail(const std::string &why) const {
    invalid(method.reference.identity(), why);
  }
  Value get(unsigned reg) const {
    if (reg >= registers.size() || registers[reg].kind == Value::Unknown ||
        registers[reg].kind == Value::High)
      fail("undefined symbolic register");
    auto value = registers[reg];
    if (value.wide &&
        (reg + 1 >= registers.size() || registers[reg + 1].kind != Value::High))
      fail("broken wide register pair");
    return value;
  }
  void put(unsigned reg, Value value) {
    unsigned words = value.wide ? 2 : 1;
    if (reg >= registers.size() || words > registers.size() - reg)
      fail("write exceeds register frame");
    for (unsigned r = reg; r < reg + words; ++r) {
      if (registers[r].wide && r + 1 < registers.size())
        registers[r + 1] = {};
      if (registers[r].kind == Value::High && r)
        registers[r - 1] = {};
      registers[r] = {};
    }
    registers[reg] = std::move(value);
    if (words == 2)
      registers[reg + 1] = {Value::High};
  }
  static bool reference(const Value &value) {
    return value.kind == Value::String || value.kind == Value::Self ||
           value.kind == Value::Object || value.kind == Value::Array ||
           value.kind == Value::Constant || value.kind == Value::ClassLiteral ||
           value.kind == Value::Clone || value.kind == Value::Lookup ||
           value.kind == Value::Uninitialized ||
           (value.kind == Value::Number && !value.number && !value.wide) ||
           ((value.kind == Value::Parameter || value.kind == Value::Computed ||
             value.kind == Value::Conditional) &&
            (value.text.starts_with('L') || value.text.starts_with('[')));
  }
  bool scalar(const Instruction &op) {
    budget.tick();
    if (pending) {
      if (op.opcode != pending_opcode || op.registers.size() != 1)
        fail("result is not consumed immediately");
      put(op.registers[0], *pending);
      pending.reset();
      pending_opcode = "move-result-object";
      return true;
    }
    if (op.opcode == "nop" && op.registers.empty())
      return true;
    static const Access constants{
        "const",      "const/4",       "const/16",      "const/high16",
        "const-wide", "const-wide/16", "const-wide/32", "const-wide/high16"};
    if (constants.contains(op.opcode)) {
      auto number = std::get_if<int64_t>(&op.literal);
      if (!number || op.registers.size() != 1)
        fail("invalid numeric constant");
      put(op.registers[0],
          {Value::Number, *number, {}, op.opcode.starts_with("const-wide")});
      return true;
    }
    if (op.opcode == "const-string" || op.opcode == "const-string/jumbo") {
      auto string = std::get_if<std::string>(&op.literal);
      if (!string || op.registers.size() != 1)
        fail("invalid string constant");
      put(op.registers[0], {Value::String, 0, *string});
      return true;
    }
    auto opcode = op.opcode.substr(0, op.opcode.find('/'));
    if (opcode == "move" || opcode == "move-object" || opcode == "move-wide") {
      if (op.registers.size() != 2)
        fail("invalid move");
      auto value = get(op.registers[1]);
      if ((opcode == "move-object" && !reference(value)) ||
          (opcode == "move-wide" && !value.wide) ||
          (opcode == "move" &&
           (value.wide || (reference(value) && value.kind != Value::Number))))
        fail("move carrier disagrees with its producer");
      put(op.registers[0], value);
      return true;
    }
    return false;
  }
  void require(const Value &value, const std::string &type) const {
    if ((value.kind == Value::Computed || value.kind == Value::Conditional) &&
        (value.text == type ||
         isAssignableReference(value.text, type, classes, budget)))
      return;
    if (value.kind == Value::Parameter && value.text == type)
      return;
    if (type.starts_with('L') || type.starts_with('[')) {
      if ((value.kind == Value::Number && !value.number && !value.wide) ||
          (value.kind == Value::String &&
           (type == "Ljava/lang/String;" || type == "Ljava/lang/Object;")))
        return;
    } else if (value.kind == Value::Number && value.wide == (width(type) == 2))
      return;
    fail("constructor argument has no exact typed literal or parameter");
  }
  std::vector<Value> arguments(const Instruction &op, const MethodRef &ref,
                               size_t cursor = 1) {
    if (cursor && op.registers.empty())
      fail("missing constructor receiver");
    std::vector<Value> result;
    for (const auto &type : ref.parameters) {
      if (cursor >= op.registers.size())
        fail("missing constructor argument");
      auto value = get(op.registers[cursor]);
      require(value, type);
      result.push_back(value);
      if (width(type) == 2 &&
          (cursor + 1 >= op.registers.size() ||
           op.registers[cursor + 1] != op.registers[cursor] + 1))
        fail("constructor argument has a broken wide pair");
      cursor += width(type);
    }
    if (cursor != op.registers.size())
      fail("constructor argument count disagrees");
    return result;
  }
};
bool metadata(const Method &method, bool signature_allowed = false) {
  return (method.generic_signature && !signature_allowed) ||
         method.declared_throws || method.deprecated || method.suppress_lint ||
         !method.annotations.empty() || !method.parameter_annotations.empty() ||
         method.annotation_default;
}
const Method &findMethod(const ClassMap &classes, const MethodRef &ref) {
  auto cls = classes.find(ref.owner);
  if (cls != classes.end())
    for (const auto &method : cls->second.methods)
      if (method.reference == ref)
        return method;
  invalid(ref.identity(), "missing original method");
}
struct Forward {
  MethodRef target;
  std::vector<Value> arguments;
};
class Planner {
  const ClassMap &classes;
  Budget &budget;
  EnumPlans plans;
  std::map<MethodRef, Forward> forwards;
  struct DefaultArgument {
    size_t mask_parameter;
    uint32_t mask;
    bool on_set;
    Value supplied;
    Value replacement;
  };
  std::vector<DefaultArgument> defaults;
  std::set<MethodRef> source_constructors;
  std::set<FieldRef> backing;

  void constructor(const Class &root, const Method &method, bool child) {
    const auto &ref = method.reference;
    Frame frame(method, classes, budget, false, true);
    const auto first_default = defaults.size();
    std::optional<uint32_t> argument_call;
    if (ref.parameters.size() < 2 ||
        ref.parameters[0] != "Ljava/lang/String;" || ref.parameters[1] != "I" ||
        ref.returns != "V" || has(method.access, "static") ||
        has(method.access, "native") || has(method.access, "abstract"))
      frame.fail("name and ordinal parameter prefix is not proven");
    for (size_t i = 0; i < method.instructions.size(); ++i) {
      const auto &op = method.instructions[i];
      if (!child && has(method.access, "synthetic") &&
          (op.opcode == "and-int/lit8" || op.opcode == "and-int/lit16")) {
        const auto *mask = std::get_if<int64_t>(&op.literal);
        if (metadata(method) || !method.tries.empty() ||
            op.registers.size() != 2 || !mask || !*mask || *mask < INT32_MIN ||
            *mask > INT32_MAX || op.target || !op.targets.empty() ||
            !op.keys.empty() || !op.data.empty() ||
            i + 3 >= method.instructions.size())
          frame.fail("default argument requires a bounded mask and pure guard");
        const auto parameter = frame.get(op.registers[1]);
        const auto &branch = method.instructions[i + 1];
        const auto &assignment = method.instructions[i + 2];
        if (parameter.kind != Value::Parameter || parameter.number < 2 ||
            parameter.text != "I" ||
            (branch.opcode != "if-eqz" && branch.opcode != "if-nez") ||
            branch.registers != std::vector<unsigned>{op.registers[0]} ||
            branch.target != method.instructions[i + 3].pc ||
            !branch.targets.empty() || !branch.keys.empty() ||
            !branch.data.empty() || assignment.registers.empty() ||
            assignment.target || !assignment.targets.empty() ||
            !assignment.keys.empty() || !assignment.data.empty())
          frame.fail(
              "default argument requires a forward single-assignment guard");
        // The mask result is not a forwarded constructor value. Invalidating
        // its register also prevents accidental uses of the pre-mask value.
        frame.put(op.registers[0], {});
        const auto destination = assignment.registers[0];
        const auto supplied = frame.get(destination);
        if ((supplied.kind != Value::Parameter &&
             supplied.kind != Value::Conditional) ||
            (supplied.kind == Value::Parameter && supplied.number < 2) ||
            !frame.scalar(assignment))
          frame.fail(
              "default argument must replace an ordinary typed parameter");
        const auto replacement = frame.get(destination);
        frame.require(replacement, supplied.text);
        if (defaults.size() - first_default >= 128)
          frame.fail("default argument proof exceeds its choice budget");
        frame.put(destination, {Value::Conditional, int64_t(defaults.size()),
                                supplied.text, supplied.wide});
        defaults.push_back({size_t(parameter.number), uint32_t(*mask),
                            branch.opcode == "if-eqz", supplied, replacement});
        i += 2;
        continue;
      }
      if (op.target || !op.targets.empty() || !op.keys.empty() ||
          !op.data.empty())
        frame.fail("enum constructor prefix requires linear instructions");
      if (frame.scalar(op))
        continue;
      auto target = std::get_if<MethodRef>(&op.reference);
      if (!child && has(method.access, "private") &&
          !has(method.access, "synthetic") && !argument_call && target &&
          target->name != "<init>" && target->parameters.empty() &&
          target->returns != "V" &&
          (op.opcode == "invoke-static" ||
           op.opcode == "invoke-static/range") &&
          op.registers.empty()) {
        argument_call = op.pc;
        frame.pending = Value{Value::Computed, 0, target->returns,
                              width(target->returns) == 2};
        frame.pending_opcode =
            target->returns.starts_with('L') || target->returns.starts_with('[')
                ? "move-result-object"
            : width(target->returns) == 2 ? "move-result-wide"
                                          : "move-result";
        continue;
      }
      if (!target || target->name != "<init>" || target->returns != "V" ||
          (op.opcode != "invoke-direct" &&
           op.opcode != "invoke-direct/range") ||
          op.registers.empty() ||
          frame.get(op.registers[0]).kind != Value::Self)
        frame.fail("constructor requires pure leading delegation");
      if (!method.tries.empty()) {
        if (child || target->owner != "Ljava/lang/Enum;")
          frame.fail("forwarding constructor exception handlers require a "
                     "separate proof");
        for (const auto &region : method.tries) {
          if (region.start <= op.pc ||
              std::any_of(region.handlers.begin(), region.handlers.end(),
                          [&](const Handler &handler) {
                            return handler.target <= op.pc;
                          }))
            frame.fail("constructor exception handlers must follow Enum "
                       "initialization");
        }
      }
      auto args = frame.arguments(op, *target);
      if (args.size() < 2 ||
          args[0] != Value{Value::Parameter, 0, "Ljava/lang/String;"} ||
          args[1] != Value{Value::Parameter, 1, "I"})
        frame.fail("constructor must forward its exact name and ordinal");
      const auto call_uses =
          std::count_if(args.begin(), args.end(), [](const Value &value) {
            return value.kind == Value::Computed;
          });
      if (call_uses != (argument_call ? 1 : 0))
        frame.fail("constructor argument call must be consumed exactly once");
      if (!child && has(method.access, "private") &&
          !method.parameter_annotations.empty() &&
          (method.parameter_annotations.size() != ref.parameters.size() ||
           !method.parameter_annotations[0].empty() ||
           !method.parameter_annotations[1].empty()))
        frame.fail("hidden enum parameters cannot carry source annotations");
      if (!child && target->owner == "Ljava/lang/Enum;") {
        if (target->parameters !=
                std::vector<std::string>{"Ljava/lang/String;", "I"} ||
            !has(method.access, "private") ||
            i + 1 == method.instructions.size())
          frame.fail(
              "requires a private enum constructor with an original body");
        source_constructors.insert(ref);
        plans.methods.emplace(ref, EnumMethodProjection{"enum-constructor",
                                                        false, *target, op.pc});
        return; // The ordinary body verifier owns all remaining instructions.
      }
      if (!child && target->owner == root.name &&
          has(method.access, "private") && !has(method.access, "synthetic")) {
        if (i + 2 != method.instructions.size() ||
            method.instructions.back().opcode != "return-void" ||
            !method.instructions.back().registers.empty())
          frame.fail(
              "enum this delegation requires a pure return continuation");
        source_constructors.insert(ref);
        plans.methods.emplace(ref, EnumMethodProjection{"enum-constructor",
                                                        false, *target, op.pc,
                                                        argument_call});
        return;
      }
      if (target->owner != root.name ||
          (!child && !has(method.access, "synthetic")) ||
          metadata(method, child) || i + 2 != method.instructions.size() ||
          method.instructions.back().opcode != "return-void" ||
          !method.instructions.back().registers.empty())
        frame.fail(
            "enum access constructor requires exact effect-free forwarding");
      forwards.emplace(ref, Forward{*target, std::move(args)});
      plans.methods.emplace(
          ref, EnumMethodProjection{child ? "enum-constant-constructor"
                                          : "enum-access-constructor",
                                    true, *target, op.pc});
      return;
    }
    frame.fail("constructor has no initialization boundary");
  }
  std::pair<MethodRef, std::vector<Value>> resolve(MethodRef ref,
                                                   std::vector<Value> args) {
    std::set<MethodRef> seen;
    while (!source_constructors.contains(ref)) {
      budget.tick();
      if (!seen.insert(ref).second || seen.size() > 128 ||
          !forwards.contains(ref))
        invalid(ref.identity(), "constructor forwarding is cyclic or unproven");
      const auto &next = forwards.at(ref);
      std::vector<Value> mapped;
      auto substitute = [&](auto &&self, const Value &arg,
                            unsigned depth) -> Value {
        budget.tick();
        if (depth > 128)
          invalid(ref.identity(),
                  "default argument proof exceeds its depth budget");
        if (arg.kind == Value::Parameter) {
          if (size_t(arg.number) >= args.size())
            invalid(ref.identity(),
                    "constructor forwarding parameter is absent");
          return args[arg.number];
        }
        if (arg.kind != Value::Conditional)
          return arg;
        const auto &choice = defaults.at(arg.number);
        if (choice.mask_parameter >= args.size())
          invalid(ref.identity(), "default argument mask parameter is absent");
        const auto &mask = args[choice.mask_parameter];
        if (mask.kind != Value::Number || mask.wide)
          invalid(ref.identity(),
                  "default argument mask must be a proven int literal");
        const bool set = (uint32_t(mask.number) & choice.mask) != 0;
        return self(self,
                    set == choice.on_set ? choice.replacement : choice.supplied,
                    depth + 1);
      };
      for (const auto &arg : next.arguments)
        mapped.push_back(substitute(substitute, arg, 0));
      ref = next.target;
      args = std::move(mapped);
    }
    return {std::move(ref), std::move(args)};
  }
  void intrinsic(const Class &root, const Method &method, bool values) {
    Frame frame(method, classes, budget);
    if (!has(method.access, "public") || !has(method.access, "static") ||
        metadata(method) ||
        std::any_of(method.access.begin(), method.access.end(),
                    [](const std::string &flag) {
                      return flag != "public" && flag != "static" &&
                             flag != "synthetic";
                    }))
      frame.fail(
          "compiler-managed enum method has unsupported declaration metadata");
    std::optional<Value> result;
    bool invoked = false;
    bool cast = false;
    std::optional<FieldRef> field;
    for (size_t i = 0; i < method.instructions.size(); ++i) {
      const auto &op = method.instructions[i];
      if (frame.scalar(op))
        continue;
      if (values && op.opcode == "sget-object" && op.registers.size() == 1 &&
          !field) {
        auto ref = std::get_if<FieldRef>(&op.reference);
        if (!ref || ref->owner != root.name || ref->type != "[" + root.name)
          frame.fail("values must read its own enum array");
        field = *ref;
        frame.put(op.registers[0], {Value::Array});
      } else if (!values && op.opcode == "const-class" &&
                 op.registers.size() == 1 &&
                 op.reference == Reference(root.name)) {
        frame.put(op.registers[0], {Value::ClassLiteral, 0, root.name});
      } else if (!invoked && values &&
                 (op.opcode == "invoke-virtual" ||
                  op.opcode == "invoke-virtual/range") &&
                 (op.reference == Reference(MethodRef{"[" + root.name,
                                                      "clone",
                                                      {},
                                                      "Ljava/lang/Object;"}) ||
                  op.reference == Reference(MethodRef{"Ljava/lang/Object;",
                                                      "clone",
                                                      {},
                                                      "Ljava/lang/Object;"}) ||
                  op.reference == Reference(MethodRef{"[Ljava/lang/Object;",
                                                      "clone",
                                                      {},
                                                      "Ljava/lang/Object;"}) ||
                  op.reference == Reference(MethodRef{"[Ljava/lang/Enum;",
                                                      "clone",
                                                      {},
                                                      "Ljava/lang/Object;"})) &&
                 op.registers.size() == 1 &&
                 frame.get(op.registers[0]).kind == Value::Array && field) {
        invoked = true;
        frame.pending = Value{Value::Clone};
      } else if (!invoked && !values &&
                 (op.opcode == "invoke-static" ||
                  op.opcode == "invoke-static/range") &&
                 op.reference == Reference(MethodRef{"Ljava/lang/Enum;",
                                                     "valueOf",
                                                     {"Ljava/lang/Class;",
                                                      "Ljava/lang/String;"},
                                                     "Ljava/lang/Enum;"}) &&
                 op.registers.size() == 2 &&
                 frame.get(op.registers[0]) ==
                     Value{Value::ClassLiteral, 0, root.name} &&
                 frame.get(op.registers[1]) ==
                     Value{Value::Parameter, 0, "Ljava/lang/String;"}) {
        invoked = true;
        frame.pending = Value{Value::Lookup};
      } else if (op.opcode == "check-cast" && !cast &&
                 op.registers.size() == 1 &&
                 op.reference ==
                     Reference(values ? "[" + root.name : root.name) &&
                 frame.get(op.registers[0]).kind ==
                     (values ? Value::Clone : Value::Lookup)) {
        cast = true;
        result = frame.get(op.registers[0]);
        result->text = "cast";
        frame.put(op.registers[0], *result);
      } else if (op.opcode == "return-object" && op.registers.size() == 1 &&
                 i + 1 == method.instructions.size() && result && invoked &&
                 cast && frame.get(op.registers[0]) == *result) {
        if (values) {
          plans.classes.at(root.name).values_field = *field;
          backing.insert(*field);
        }
        plans.methods.emplace(
            method.reference,
            EnumMethodProjection{values ? "enum-values" : "enum-valueOf",
                                 true});
        return;
      } else
        frame.fail(
            "enum intrinsic is not an exact clone or valueOf implementation");
    }
    frame.fail("enum intrinsic has no verified return");
  }
  void initialize(const Class &root, const Method &method, bool factory) {
    auto &plan = plans.classes.at(root.name);
    // Only the compiler-managed prefix must be linear. A continuation after
    // publication belongs to the ordinary body verifier, including its CFG.
    Frame frame(method, classes, budget, factory);
    if (!has(method.access, "static") || metadata(method))
      frame.fail("enum initialization has unsupported declaration metadata");
    std::map<std::string, size_t> fields;
    for (const auto &constant : plan.constants)
      fields.emplace(constant.field.name, fields.size());
    std::map<int64_t, std::vector<Value>> arrays;
    struct Object {
      std::string type;
      std::optional<EnumConstant> constant;
      bool stored = false;
    };
    std::vector<Object> objects;
    struct Expression {
      std::vector<Instruction> code;
      std::vector<size_t> effects;
      unsigned registers = 2;
    };
    std::vector<Expression> expressions;
    size_t effect_count = 0;
    size_t consumed_effects = 0;
    auto compose = [&](const std::vector<Value> &arguments,
                       const std::vector<std::string> &types,
                       const Expression *allocation = nullptr) {
      Expression expression;
      std::vector<Expression> pieces;
      std::vector<unsigned> call_registers;
      if (allocation) {
        pieces.push_back(*allocation);
        call_registers.push_back(0);
      }
      unsigned next = 2;
      for (size_t n = 0; n < arguments.size(); ++n) {
        budget.tick();
        const auto &value = arguments[n];
        frame.require(value, types[n]);
        if (value.kind == Value::Computed) {
          auto piece = expressions.at(value.number);
          if (piece.registers > 1024 - next)
            frame.fail("computed enum expression exceeds its register budget");
          for (auto &instruction : piece.code) {
            budget.tick();
            for (auto &reg : instruction.registers)
              reg += next;
          }
          pieces.push_back(std::move(piece));
        } else {
          Instruction literal;
          literal.registers = {next};
          if (value.kind == Value::String) {
            literal.opcode = "const-string";
            literal.literal = value.text;
          } else if (value.kind == Value::Number) {
            literal.opcode = value.wide ? "const-wide" : "const";
            literal.literal = value.number;
          } else
            frame.fail("computed enum invocation has an unproven argument");
          expression.code.push_back(std::move(literal));
        }
        for (unsigned word = 0; word < width(types[n]); ++word)
          call_registers.push_back(next + word);
        next += value.kind == Value::Computed
                    ? expressions.at(value.number).registers
                    : width(types[n]);
        if (next > 1024)
          frame.fail("computed enum expression exceeds its register budget");
      }
      // Inputs retain their original evaluation order even when invocation
      // operands use a different register order. The complete effect inventory
      // later rejects duplication, omission, or interleaving that this tree
      // cannot preserve.
      std::stable_sort(pieces.begin(), pieces.end(),
                       [](const auto &a, const auto &b) {
                         return a.effects.front() < b.effects.front();
                       });
      for (const auto &piece : pieces) {
        budget.tick(piece.code.size());
        if (expression.code.size() + piece.code.size() > 65536 ||
            expression.effects.size() + piece.effects.size() > 65536)
          frame.fail("computed enum expression exceeds its instruction budget");
        expression.code.insert(expression.code.end(), piece.code.begin(),
                               piece.code.end());
        expression.effects.insert(expression.effects.end(),
                                  piece.effects.begin(), piece.effects.end());
      }
      expression.registers = next;
      return std::pair{std::move(expression), std::move(call_registers)};
    };
    std::set<std::string> helper_names;
    for (const auto &m : root.methods)
      helper_names.insert(m.reference.name);
    bool stored_array = false;
    auto expected = [&](const std::vector<Value> &array) {
      if (array.size() != plan.constants.size())
        frame.fail("values array does not contain every enum constant");
      for (size_t n = 0; n < array.size(); ++n)
        if (array[n] !=
            Value{Value::Constant, int64_t(n), plan.constants[n].field.name})
          frame.fail("values array constant order disagrees with ordinals");
    };
    auto constantValue = [&](Value value) {
      if (value.kind == Value::Object) {
        auto &object = objects.at(value.number);
        if (!object.stored || !object.constant)
          frame.fail("unpublished enum constant in values array");
        auto n = fields.at(object.constant->field.name);
        return Value{Value::Constant, int64_t(n), object.constant->field.name};
      }
      if (value.kind != Value::Constant)
        frame.fail("values array element is not a proven enum constant");
      return value;
    };
    for (size_t i = 0; i < method.instructions.size(); ++i) {
      const auto &op = method.instructions[i];
      if (op.target || !op.targets.empty() || !op.keys.empty() ||
          !op.data.empty())
        frame.fail("enum initialization prefix requires linear instructions");
      if (frame.scalar(op))
        continue;
      auto field = std::get_if<FieldRef>(&op.reference);
      auto ref = std::get_if<MethodRef>(&op.reference);
      auto type = std::get_if<std::string>(&op.reference);
      if (!factory && op.opcode == "new-instance" && type &&
          op.registers.size() == 1) {
        if (*type != root.name && !plans.constant_classes.contains(*type) &&
            !plans.classes.contains(*type) && !objects.empty() &&
            !objects.back().stored && !objects.back().constant) {
          Expression expression;
          auto allocation = op;
          allocation.registers = {0};
          expression.code.push_back(std::move(allocation));
          expression.effects.push_back(effect_count++);
          frame.put(op.registers[0],
                    {Value::Uninitialized, int64_t(expressions.size()), *type});
          expressions.push_back(std::move(expression));
          continue;
        }
        if ((*type != root.name &&
             (!plans.constant_classes.contains(*type) ||
              plans.constant_classes.at(*type) != root.name)) ||
            (!objects.empty() && !objects.back().stored) || stored_array ||
            !arrays.empty())
          frame.fail("constant allocation order or implementation is unproven");
        frame.put(op.registers[0], {Value::Object, int64_t(objects.size())});
        objects.push_back({*type, {}, false});
      } else if (!factory && ref && ref->name == "<init>" &&
                 (op.opcode == "invoke-direct" ||
                  op.opcode == "invoke-direct/range") &&
                 !op.registers.empty()) {
        auto receiver = frame.get(op.registers[0]);
        if (receiver.kind == Value::Uninitialized) {
          if (receiver.text != ref->owner || ref->returns != "V")
            frame.fail("computed enum allocation requires its exact "
                       "constructor");
          auto [expression, registers] =
              compose(frame.arguments(op, *ref), ref->parameters,
                      &expressions.at(receiver.number));
          auto call = op;
          call.registers = std::move(registers);
          expression.code.push_back(std::move(call));
          expression.effects.push_back(effect_count++);
          const Value initialized{Value::Computed, int64_t(expressions.size()),
                                  ref->owner};
          for (auto &value : frame.registers)
            if (value == receiver)
              value = initialized;
          expressions.push_back(std::move(expression));
          continue;
        }
        if (receiver.kind != Value::Object)
          frame.fail("constant constructor receiver is not its allocation");
        auto &object = objects.at(receiver.number);
        if (object.type != ref->owner || object.constant || object.stored)
          frame.fail("constant constructor does not match a unique allocation");
        auto [target, args] = resolve(*ref, frame.arguments(op, *ref));
        if (target.owner != root.name || args.size() < 2 ||
            args[0].kind != Value::String ||
            args[1] != Value{Value::Number, receiver.number})
          frame.fail("constant name or contiguous ordinal is unproven");
        EnumConstant constant{
            {root.name, args[0].text, root.name}, target, object.type, {}};
        std::vector<size_t> ordered_effects;
        for (size_t n = 2; n < args.size(); ++n) {
          frame.require(args[n], target.parameters.at(n));
          constant.argument_helpers.emplace_back();
          if (args[n].kind == Value::Computed) {
            const auto &expression = expressions.at(args[n].number);
            ordered_effects.insert(ordered_effects.end(),
                                   expression.effects.begin(),
                                   expression.effects.end());
            Method helper;
            std::string name = "__neverdEnumArgument" +
                               std::to_string(receiver.number) + "_" +
                               std::to_string(n - 2);
            while (!helper_names.insert(name).second)
              name += '_';
            helper.reference = {root.name, name, {}, target.parameters[n]};
            helper.access = {"private", "static"};
            helper.registers = std::max(4u, expression.registers);
            helper.instructions = expression.code;
            Instruction returned;
            returned.opcode = target.parameters[n].starts_with('L') ||
                                      target.parameters[n].starts_with('[')
                                  ? "return-object"
                              : width(target.parameters[n]) == 2 ? "return-wide"
                                                                 : "return";
            returned.registers = {0};
            helper.instructions.push_back(std::move(returned));
            for (size_t pc = 0; pc < helper.instructions.size(); ++pc)
              helper.instructions[pc].pc = pc;
            helper.code_end = helper.instructions.size();
            constant.argument_helpers.back() = std::move(helper);
            constant.arguments.emplace_back();
          } else if (args[n].kind == Value::String)
            constant.arguments.emplace_back(args[n].text);
          else if (args[n].kind == Value::Number)
            constant.arguments.emplace_back(args[n].number);
          else
            frame.fail("enum constant requires literal constructor arguments");
        }
        if (ordered_effects.size() != effect_count - consumed_effects)
          frame.fail(
              "computed enum arguments duplicate or omit evaluation effects");
        for (size_t n = 0; n < ordered_effects.size(); ++n)
          if (ordered_effects[n] != consumed_effects + n)
            frame.fail("computed enum arguments change evaluation order");
        consumed_effects = effect_count;
        object.constant = std::move(constant);
      } else if (!factory && !objects.empty() && !objects.back().stored &&
                 !objects.back().constant && op.opcode.starts_with("sget") &&
                 field && op.registers.size() == 1) {
        if (field->type.starts_with('['))
          frame.fail(
              "computed enum array arguments require a separate alias proof");
        // Keep each effectful read/call in one argument helper. Its original
        // typed instructions are verified by the normal body emitter.
        Expression expression;
        auto read = op;
        read.registers = {0};
        expression.code.push_back(std::move(read));
        expression.effects.push_back(effect_count++);
        frame.put(op.registers[0],
                  {Value::Computed, int64_t(expressions.size()), field->type,
                   width(field->type) == 2});
        expressions.push_back(std::move(expression));
      } else if (!factory && !objects.empty() && !objects.back().stored &&
                 !objects.back().constant && ref &&
                 (ref->parameters.empty() ||
                  op.opcode.starts_with("invoke-static")) &&
                 ref->returns != "V" &&
                 (op.opcode == "invoke-virtual" ||
                  op.opcode == "invoke-virtual/range" ||
                  op.opcode == "invoke-static" ||
                  op.opcode == "invoke-static/range")) {
        const bool is_static = op.opcode.starts_with("invoke-static");
        if (ref->returns.starts_with('['))
          frame.fail(
              "computed enum array arguments require a separate alias proof");
        Expression expression;
        std::vector<unsigned> call_registers;
        if (!is_static) {
          if (op.registers.size() != 1)
            frame.fail("computed enum argument has an invalid getter frame");
          const auto receiver = frame.get(op.registers[0]);
          if (receiver.kind != Value::Computed || !Frame::reference(receiver))
            frame.fail(
                "computed enum getter needs one proven receiver expression");
          expression = expressions.at(receiver.number);
          call_registers.push_back(0);
        } else {
          std::tie(expression, call_registers) =
              compose(frame.arguments(op, *ref, 0), ref->parameters);
        }
        auto call = op;
        call.registers = std::move(call_registers);
        expression.code.push_back(std::move(call));
        Instruction result;
        result.opcode =
            ref->returns.starts_with('L') || ref->returns.starts_with('[')
                ? "move-result-object"
            : width(ref->returns) == 2 ? "move-result-wide"
                                       : "move-result";
        result.registers = {0};
        expression.code.push_back(result);
        expression.effects.push_back(effect_count++);
        frame.pending = Value{Value::Computed, int64_t(expressions.size()),
                              ref->returns, width(ref->returns) == 2};
        frame.pending_opcode = result.opcode;
        expressions.push_back(std::move(expression));
      } else if (!factory && !objects.empty() && !objects.back().stored &&
                 !objects.back().constant &&
                 Access{"add-int", "sub-int", "mul-int", "add-long", "sub-long",
                        "mul-long"}
                     .contains(op.opcode.substr(0, op.opcode.find('/')))) {
        const bool inplace = op.opcode.ends_with("/2addr");
        const auto base = op.opcode.substr(0, op.opcode.find('/'));
        if (op.opcode != base && !inplace)
          frame.fail("computed enum arithmetic has an unsupported encoding");
        if (op.registers.size() != (inplace ? 2 : 3))
          frame.fail("computed enum arithmetic has an invalid register frame");
        const auto left = frame.get(op.registers[inplace ? 0 : 1]);
        const auto right = frame.get(op.registers[inplace ? 1 : 2]);
        const bool left_expression = left.kind == Value::Computed;
        const auto &expression_value = left_expression ? left : right;
        const auto &literal = left_expression ? right : left;
        const bool wide = base.ends_with("-long");
        const std::string type = wide ? "J" : "I";
        if (expression_value.kind != Value::Computed ||
            expression_value.text != type || literal.kind != Value::Number ||
            literal.wide != wide)
          frame.fail("computed enum arithmetic requires one typed expression "
                     "and one literal operand");
        auto expression = expressions.at(expression_value.number);
        Instruction constant;
        constant.opcode = wide ? "const-wide" : "const";
        constant.registers = {2};
        constant.literal = literal.number;
        expression.code.push_back(std::move(constant));
        auto arithmetic = op;
        arithmetic.opcode = base;
        arithmetic.registers = left_expression ? std::vector<unsigned>{0, 0, 2}
                                               : std::vector<unsigned>{0, 2, 0};
        expression.code.push_back(std::move(arithmetic));
        frame.put(op.registers[0],
                  {Value::Computed, int64_t(expressions.size()), type, wide});
        expressions.push_back(std::move(expression));
      } else if (!factory && op.opcode == "sput-object" && field &&
                 op.registers.size() == 1 && field->owner == root.name) {
        auto value = frame.get(op.registers[0]);
        if (*field == plan.values_field) {
          if (stored_array || value.kind != Value::Array ||
              fields.size() != objects.size())
            frame.fail(
                "values field requires one complete array initialization");
          expected(arrays.at(value.number));
          stored_array = true;
          plan.array_publication_pc = op.pc;
          if (method.instructions.back().opcode != "return-void" ||
              !method.instructions.back().registers.empty())
            frame.fail("enum initializer continuation requires an original "
                       "return boundary");
          if (i + 2 != method.instructions.size() ||
              method.instructions.back().opcode != "return-void") {
            Method tail = method;
            tail.instructions.clear();
            auto append = [&](std::string opcode, unsigned reg, Literal literal,
                              Reference reference) {
              Instruction entry;
              entry.pc = tail.instructions.size();
              entry.opcode = std::move(opcode);
              entry.registers = {reg};
              entry.literal = std::move(literal);
              entry.reference = std::move(reference);
              tail.instructions.push_back(std::move(entry));
            };
            for (unsigned reg = 0; reg < frame.registers.size(); ++reg) {
              const auto &v = frame.registers[reg];
              if (v.kind == Value::Number)
                append(v.wide ? "const-wide" : "const", reg, v.number, {});
              else if (v.kind == Value::String)
                append("const-string", reg, v.text, {});
              else if (v.kind == Value::Array)
                append("sget-object", reg, {}, plan.values_field);
              else if (v.kind == Value::Constant)
                append("sget-object", reg, {},
                       plan.constants.at(v.number).field);
              else if (v.kind == Value::Object) {
                const auto &object = objects.at(v.number);
                if (!object.stored || !object.constant)
                  frame.fail(
                      "unpublished constant at initializer continuation");
                append("sget-object", reg, {}, object.constant->field);
              } else if (v.kind != Value::Unknown && v.kind != Value::High &&
                         v.kind != Value::Computed)
                frame.fail("unproven register at initializer continuation");
            }
            const auto offset = uint32_t(tail.instructions.size());
            for (size_t index = i + 1; index < method.instructions.size();
                 ++index) {
              auto instruction = method.instructions[index];
              instruction.pc += offset;
              if (instruction.target) {
                if (*instruction.target <= op.pc)
                  frame.fail("initializer continuation branches into constant "
                             "initialization");
                *instruction.target += offset;
              }
              for (auto &target : instruction.targets) {
                if (target <= op.pc)
                  frame.fail("initializer continuation branches into constant "
                             "initialization");
                target += offset;
              }
              tail.instructions.push_back(std::move(instruction));
            }
            tail.code_end += offset;
            plan.initializer_tail = std::move(tail);
            return;
          }
        } else {
          if (value.kind != Value::Object || stored_array)
            frame.fail("enum constant field write has no unique object");
          auto &object = objects.at(value.number);
          if (!object.constant || object.stored ||
              *field != object.constant->field ||
              fields.size() != size_t(value.number) ||
              !plans.managed_fields.contains(*field))
            frame.fail(
                "constant field name, write order or declaration disagrees");
          fields.emplace(field->name, fields.size());
          plan.constants.push_back(*object.constant);
          object.stored = true;
        }
      } else if (op.opcode == "sget-object" && field &&
                 op.registers.size() == 1 && field->owner == root.name &&
                 field->type == root.name && fields.contains(field->name)) {
        frame.put(
            op.registers[0],
            {Value::Constant, int64_t(fields.at(field->name)), field->name});
      } else if (op.opcode == "new-array" && type && *type == "[" + root.name &&
                 op.registers.size() == 2 && arrays.empty()) {
        auto size = frame.get(op.registers[1]);
        if (size != Value{Value::Number, int64_t(plan.constants.size())})
          frame.fail("values array size disagrees with constants");
        arrays.emplace(0, std::vector<Value>(size.number));
        frame.put(op.registers[0], {Value::Array});
      } else if (op.opcode == "aput-object" && op.registers.size() == 3) {
        auto array = frame.get(op.registers[1]);
        auto index = frame.get(op.registers[2]);
        if (array.kind != Value::Array || index.kind != Value::Number ||
            index.wide || index.number < 0 ||
            size_t(index.number) >= arrays.at(array.number).size() ||
            arrays.at(array.number)[index.number].kind != Value::Unknown ||
            stored_array)
          frame.fail("values array requires exactly one write to each slot");
        arrays.at(array.number)[index.number] =
            constantValue(frame.get(op.registers[0]));
      } else if ((op.opcode == "filled-new-array" ||
                  op.opcode == "filled-new-array/range") &&
                 type && *type == "[" + root.name && arrays.empty()) {
        std::vector<Value> values;
        for (auto reg : op.registers)
          values.push_back(constantValue(frame.get(reg)));
        expected(values);
        arrays.emplace(0, std::move(values));
        frame.pending = Value{Value::Array};
      } else if (!factory &&
                 (op.opcode == "invoke-static" ||
                  op.opcode == "invoke-static/range") &&
                 ref && ref->owner == root.name && ref->parameters.empty() &&
                 ref->returns == "[" + root.name && op.registers.empty() &&
                 arrays.empty() && !plan.array_factory) {
        const auto &builder = findMethod(classes, *ref);
        if (!has(builder.access, "synthetic") ||
            !has(builder.access, "private"))
          frame.fail("array factory requires a private synthetic declaration");
        initialize(root, builder, true);
        plan.array_factory = *ref;
        std::vector<Value> values;
        for (size_t n = 0; n < plan.constants.size(); ++n)
          values.push_back(
              {Value::Constant, int64_t(n), plan.constants[n].field.name});
        arrays.emplace(0, std::move(values));
        frame.pending = Value{Value::Array};
      } else if (factory && op.opcode == "return-object" &&
                 op.registers.size() == 1 &&
                 i + 1 == method.instructions.size()) {
        auto array = frame.get(op.registers[0]);
        if (array.kind != Value::Array)
          frame.fail("array factory returned a non-array value");
        expected(arrays.at(array.number));
        return;
      } else if (!factory && op.opcode == "return-void" &&
                 op.registers.empty() && i + 1 == method.instructions.size() &&
                 stored_array) {
        for (const auto &object : objects)
          if (!object.stored)
            frame.fail("unpublished enum allocation");
        return;
      } else
        frame.fail("enum initialization contains an unproven operation: " +
                   op.opcode);
    }
    frame.fail("enum initialization has no verified return");
  }

public:
  Planner(const ClassMap &classes, Budget &budget)
      : classes(classes), budget(budget) {}
  EnumPlans run() {
    std::set<std::string> method_names;
    for (const auto &[name, cls] : classes) {
      budget.tick();
      for (const auto &method : cls.methods)
        method_names.insert(method.reference.name);
      if (cls.external)
        continue;
      if (isEnumConstantClass(cls, classes)) {
        plans.constant_classes.emplace(name, *cls.enclosing);
        continue;
      }
      if (!has(cls.access, "enum"))
        continue;
      if (cls.superclass != "Ljava/lang/Enum;" || isAnonymous(cls) ||
          cls.enclosing_method ||
          (cls.enclosing && !has(cls.inner_access, "static")))
        invalid(name, "requires a named enum extending java.lang.Enum");
      if (classes.contains("Ljava/lang/Enum;") &&
          !classes.at("Ljava/lang/Enum;").external)
        invalid(name, "Enum must be the platform declaration");
      plans.classes.emplace(name, EnumClassPlan{});
    }
    for (const auto &[name, root] : plans.constant_classes) {
      const auto &cls = classes.at(name);
      if (!plans.classes.contains(root) || !cls.fields.empty() ||
          !cls.interfaces.empty() || cls.generic_signature ||
          !cls.marker_annotations.empty() || cls.deprecated ||
          cls.suppress_lint || !has(cls.access, "final") ||
          (cls.inner_name && !has(cls.inner_access, "static")))
        invalid(name, "constant-specific subclass requires a proven enum "
                      "enclosure and body");
      for (const auto &method : cls.methods) {
        if (method.reference.name == "<init>")
          constructor(classes.at(root), method, true);
        else if (has(method.access, "static") ||
                 has(method.access, "abstract") ||
                 has(method.access, "native") ||
                 method.reference.name.starts_with('<'))
          invalid(name,
                  "constant-specific body requires original instance methods");
      }
    }
    if (plans.classes.empty())
      return std::move(plans);
    for (auto &[name, plan] : plans.classes) {
      const auto &cls = classes.at(name);
      size_t field_count = 0;
      for (const auto &field : cls.fields) {
        if (has(field.access, "enum")) {
          if (field.reference.type != name || !has(field.access, "static") ||
              !has(field.access, "final") || !has(field.access, "public") ||
              !std::holds_alternative<std::monostate>(field.value))
            invalid(name, "enum constant field declaration disagrees");
          ++field_count;
          plans.managed_fields.insert(field.reference);
        } else if (has(field.access, "static") &&
                   !std::holds_alternative<std::monostate>(field.value)) {
          const auto *number = std::get_if<int64_t>(&field.value);
          const bool integral =
              number && (field.reference.type == "J" ||
                         (field.reference.type == "I" && *number >= INT32_MIN &&
                          *number <= INT32_MAX));
          const bool string = field.reference.type == "Ljava/lang/String;" &&
                              std::holds_alternative<std::string>(field.value);
          if (!has(field.access, "final") || (!integral && !string))
            invalid(name,
                    "encoded static values require a final int/long/String "
                    "constant initialization proof");
          std::string helper = "__neverdEnumRead" +
                               std::to_string(plans.encoded_constants.size());
          while (!method_names.insert(helper).second)
            helper += '_';
          plans.encoded_constants.emplace(field.reference, std::move(helper));
        }
      }
      for (const auto &method : cls.methods)
        if (method.reference.name == "<init>")
          constructor(cls, method, false);
      for (const auto &method : cls.methods) {
        if (!source_constructors.contains(method.reference))
          continue;
        auto current = method.reference;
        std::set<MethodRef> seen;
        for (;;) {
          budget.tick();
          if (!seen.insert(current).second || seen.size() > 128 ||
              !source_constructors.contains(current))
            invalid(
                method.reference.identity(),
                "enum this delegation is cyclic or has no source constructor");
          const auto &target = *plans.methods.at(current).target;
          if (target.owner == "Ljava/lang/Enum;")
            break;
          if (target.owner != name)
            invalid(method.reference.identity(),
                    "enum this delegation leaves its original class");
          current = target;
        }
      }
      auto values = MethodRef{name, "values", {}, "[" + name};
      intrinsic(cls, findMethod(classes, values), true);
      intrinsic(
          cls,
          findMethod(classes, {name, "valueOf", {"Ljava/lang/String;"}, name}),
          false);
      auto found = std::find_if(cls.fields.begin(), cls.fields.end(),
                                [&](const Field &field) {
                                  return field.reference == plan.values_field;
                                });
      if (found == cls.fields.end() || !has(found->access, "private") ||
          !has(found->access, "static") || !has(found->access, "final") ||
          !has(found->access, "synthetic") || found->generic_signature ||
          found->deprecated || found->suppress_lint ||
          !found->annotations.empty() ||
          !std::holds_alternative<std::monostate>(found->value))
        invalid(name,
                "values backing field has no compiler-managed declaration");
      plans.managed_fields.insert(plan.values_field);
      auto clinit = MethodRef{name, "<clinit>", {}, "V"};
      initialize(cls, findMethod(classes, clinit), false);
      if (plan.constants.size() != field_count)
        invalid(name, "constant initialization omitted an original enum field");
      plans.methods.emplace(
          clinit,
          EnumMethodProjection{"enum-initializer", !plan.initializer_tail});
    }
    std::set<std::string> used;
    for (const auto &[name, plan] : plans.classes)
      for (const auto &constant : plan.constants)
        if (constant.implementation != name &&
            !used.insert(constant.implementation).second)
          invalid(name,
                  "constant-specific subclass is allocated more than once");
    if (used.size() != plans.constant_classes.size())
      invalid("constant classes",
              "an original constant subclass has no allocation");
    for (const auto &[ref, forwarding] : forwards) {
      auto current = ref;
      std::set<MethodRef> seen;
      while (!source_constructors.contains(current)) {
        budget.tick();
        if (!seen.insert(current).second || seen.size() > 128 ||
            !forwards.contains(current))
          invalid(ref.identity(),
                  "constructor forwarding is cyclic or unproven");
        current = forwards.at(current).target;
      }
    }
    // Compiler-owned fields, constructors and unnameable subclass identities
    // cannot leak into unrelated bodies or declarations.
    for (const auto &[name, cls] : classes) {
      if (cls.external)
        continue;
      auto declaration = [&](const std::string &type) {
        auto n = type.find_first_not_of('[');
        if (n != type.npos && plans.constant_classes.contains(type.substr(n)))
          invalid(name,
                  "enum constant subclass identity escapes into a declaration");
      };
      for (const auto &field : cls.fields)
        declaration(field.reference.type);
      for (const auto &method : cls.methods) {
        const auto &identity = method.reference;
        declaration(identity.returns);
        if (!forwards.contains(identity))
          for (const auto &type : identity.parameters)
            declaration(type);
        for (const auto &op : method.instructions) {
          budget.tick();
          bool managed = plans.methods.contains(identity) &&
                         plans.methods.at(identity).compiler_managed;
          if (identity.name == "<clinit>" &&
              plans.classes.contains(identity.owner))
            managed |=
                op.pc <= plans.classes.at(identity.owner).array_publication_pc;
          if (auto field = std::get_if<FieldRef>(&op.reference)) {
            if (backing.contains(*field) && !managed &&
                (!plans.classes.at(field->owner).initializer_tail ||
                 op.opcode != "sget-object"))
              invalid(identity.identity(),
                      "values backing array escapes its verified intrinsic");
          } else if (auto ref = std::get_if<MethodRef>(&op.reference)) {
            const auto self = plans.methods.find(identity);
            const bool this_delegation =
                self != plans.methods.end() &&
                self->second.kind == "enum-constructor" &&
                self->second.delegation_pc == op.pc &&
                self->second.target == *ref;
            if (ref->name == "<init>" && plans.methods.contains(*ref) &&
                !managed && !this_delegation)
              invalid(
                  identity.identity(),
                  "enum constructor use is outside verified initialization");
            if (!managed) {
              declaration(ref->returns);
              for (const auto &type : ref->parameters)
                declaration(type);
              if (plans.constant_classes.contains(ref->owner) &&
                  name != ref->owner)
                invalid(
                    identity.identity(),
                    "constant-specific method has an external subtype caller");
            }
          } else if (auto type = std::get_if<std::string>(&op.reference)) {
            if (!managed)
              declaration(*type);
            if (op.opcode == "new-instance" && plans.classes.contains(*type) &&
                !managed)
              invalid(identity.identity(),
                      "enum allocation is outside verified initialization");
          }
        }
      }
    }
    verifyEnumArrayUses(classes, plans, budget);
    return std::move(plans);
  }
};
} // namespace
EnumPlans planEnumClasses(const ClassMap &classes, Budget &budget) {
  return Planner(classes, budget).run();
}
} // namespace neverd::mobile::dalvik
