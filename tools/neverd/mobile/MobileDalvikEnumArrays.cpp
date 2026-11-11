//===- MobileDalvikEnumArrays.cpp - Read-only enum array ownership proof
//---===//
#include "MobileDalvik.h"

#include <algorithm>
#include <deque>

namespace neverd::mobile::dalvik {
namespace {
enum Origin : uint8_t { Other = 1, Array = 2, Self = 4, Null = 8 };
using State = std::vector<uint8_t>;
class ArrayUses {
  const ClassMap &classes;
  const EnumPlans &enums;
  Budget &budget;
  std::map<MethodRef, const Method *> methods;
  std::map<FieldRef, const Field *> fields;
  std::map<FieldRef, std::set<MethodRef>> readers;
  std::set<FieldRef> array_fields;
  std::map<MethodRef, std::set<unsigned>> parameters;
  std::set<MethodRef> pending;
  [[noreturn]] void fail(const MethodRef &ref, const std::string &why) const {
    throw Error("Android enum array " + ref.identity() + ": " + why);
  }
  const Method *resolve(MethodRef ref) {
    std::set<std::string> seen;
    for (;;) {
      budget.tick();
      if (!seen.insert(ref.owner).second || seen.size() > 128)
        fail(ref, "cyclic call owner hierarchy");
      if (auto found = methods.find(ref); found != methods.end())
        return found->second;
      auto cls = classes.find(ref.owner);
      if (ref.name == "<init>" || cls == classes.end() ||
          !cls->second.superclass)
        return nullptr;
      ref.owner = *cls->second.superclass;
    }
  }
  void field(const FieldRef &ref, const MethodRef &where) {
    auto definition = fields.find(ref);
    if (definition == fields.end() ||
        !has(definition->second->access, "private") ||
        !has(definition->second->access, "final") || !ref.type.starts_with('['))
      fail(where, "array storage requires a known private final array field");
    if (array_fields.insert(ref).second) {
      for (const auto &reader : readers[ref])
        pending.insert(reader);
    }
  }
  void analyze(const Method &method) {
    const auto &ref = method.reference;
    const auto &code = method.instructions;
    const auto count = method.registers;
    if (code.empty() || has(method.access, "native") ||
        has(method.access, "abstract") || count > 1024 ||
        method.incomingWords() > count ||
        code.size() > 5000000 / std::max(1U, count))
      fail(ref, "array consumer has no bounded original body");
    std::map<uint32_t, size_t> positions;
    for (size_t n = 0; n < code.size(); ++n) {
      budget.tick();
      if (!positions.emplace(code[n].pc, n).second ||
          (n && code[n - 1].pc >= code[n].pc))
        fail(ref, "invalid consumer instruction boundaries");
      for (auto reg : code[n].registers)
        if (reg >= count)
          fail(ref, "consumer register is outside its frame");
    }
    if (method.code_end <= code.back().pc)
      fail(ref, "invalid consumer code end");
    std::vector<std::optional<State>> states(code.size());
    State entry(count, Other);
    unsigned reg = count - method.incomingWords();
    if (!has(method.access, "static"))
      entry[reg++] = Self;
    for (unsigned n = 0; n < ref.parameters.size(); ++n) {
      if (parameters[ref].contains(n))
        entry[reg] = Array;
      reg += width(ref.parameters[n]);
    }
    states[0] = std::move(entry);
    std::deque<size_t> work{0};
    auto merge = [&](uint32_t pc, const State &state) {
      auto found = positions.find(pc);
      if (found == positions.end())
        fail(ref, "consumer branch has no instruction target");
      auto &old = states[found->second];
      bool changed = !old;
      if (!old)
        old = state;
      else
        for (size_t n = 0; n < count; ++n) {
          auto joined = uint8_t((*old)[n] | state[n]);
          changed |= joined != (*old)[n];
          (*old)[n] = joined;
        }
      if (changed)
        work.push_back(found->second);
    };
    while (!work.empty()) {
      auto n = work.front();
      work.pop_front();
      const auto &op = code[n];
      const auto &r = op.registers;
      const auto &before = *states[n];
      auto after = before;
      budget.tick(1 + count / 16);
      const auto base = op.opcode.substr(0, op.opcode.find('/'));
      auto arity = [&](size_t size) {
        if (r.size() != size)
          fail(ref, "invalid array-consumer instruction arity");
      };
      auto hasArray = [&] {
        return std::any_of(r.begin(), r.end(),
                           [&](unsigned reg) { return before[reg] & Array; });
      };
      bool branch = base.starts_with("if-") || base == "goto" ||
                    base == "packed-switch" || base == "sparse-switch";
      bool terminal = base.starts_with("return") || base == "throw";
      if (base == "move-object") {
        arity(2);
        after[r[0]] = before[r[1]];
      } else if (base == "check-cast") {
        arity(1);
      } else if (base == "sget-object" || base == "iget-object") {
        arity(base == "sget-object" ? 1 : 2);
        const auto *f = std::get_if<FieldRef>(&op.reference);
        if (!f || (r.size() == 2 && (before[r[1]] & Array)))
          fail(ref, "array receiver used as an object field owner");
        after[r[0]] = array_fields.contains(*f) ? Array : Other;
      } else if (base == "iput-object") {
        arity(2);
        const auto *f = std::get_if<FieldRef>(&op.reference);
        if (!f || (before[r[1]] & Array))
          fail(ref, "invalid array field write");
        if (before[r[0]] & Array) {
          if (ref.name != "<init>" || f->owner != ref.owner ||
              before[r[1]] != Self)
            fail(ref, "array escapes outside its receiver's constructor");
          field(*f, ref);
        }
      } else if (base == "array-length") {
        arity(2);
        after[r[0]] = Other;
      } else if (base == "aget-object") {
        arity(3);
        if (before[r[2]] & Array)
          fail(ref, "array used as an element index");
        // The only tracked arrays contain enum references, never arrays.
        after[r[0]] = Other;
      } else if (base.starts_with("invoke-")) {
        auto called = std::get_if<MethodRef>(&op.reference);
        if (!called)
          fail(ref, "consumer invocation has no method reference");
        if (hasArray()) {
          const bool platform_object =
              !classes.contains("Ljava/lang/Object;") ||
              classes.at("Ljava/lang/Object;").external;
          const bool array_class = base == "invoke-virtual" &&
                                   platform_object &&
                                   *called == MethodRef{"Ljava/lang/Object;",
                                                        "getClass",
                                                        {},
                                                        "Ljava/lang/Class;"} &&
                                   r.size() == 1 && before[r[0]] == Array;
          const bool array_clone = base == "invoke-virtual" &&
                                   platform_object && called->name == "clone" &&
                                   called->parameters.empty() &&
                                   called->returns == "Ljava/lang/Object;" &&
                                   (called->owner == "Ljava/lang/Object;" ||
                                    called->owner.starts_with('[')) &&
                                   r.size() == 1 && before[r[0]] == Array;
          if (!array_class && !array_clone) {
            auto target = resolve(*called);
            if (!target || classes.at(target->reference.owner).external ||
                (base != "invoke-static" && base != "invoke-direct"))
              fail(ref, "array call requires an exact original static or "
                        "direct body: " +
                            called->identity());
            bool is_static = has(target->access, "static");
            if (is_static != (base == "invoke-static"))
              fail(ref, "array call dispatch disagrees with its declaration");
            if (!is_static &&
                (r.empty() || (before[r[0]] & Array) ||
                 (called->name != "<init>" && !has(target->access, "private"))))
              fail(ref,
                   "array receiver or virtual direct dispatch is unproven");
            size_t cursor = is_static ? 0 : 1;
            for (unsigned p = 0; p < called->parameters.size(); ++p) {
              if (cursor >= r.size())
                fail(ref, "array call argument is absent");
              if (before[r[cursor]] & Array) {
                const auto &type = called->parameters[p];
                if (!type.starts_with('[') && type != "Ljava/lang/Object;")
                  fail(ref, "array call argument is not a reference carrier");
                if (parameters[target->reference].insert(p).second)
                  pending.insert(target->reference);
              }
              cursor += width(called->parameters[p]);
            }
            if (cursor != r.size())
              fail(ref, "array call argument count disagrees");
          }
        }
      } else if (branch) {
        if (hasArray()) {
          if ((base == "if-eqz" || base == "if-nez") && r.size() == 1) {
            // A null check observes no array identity or storage.
          } else
            fail(ref,
                 "array identity comparison is outside the ownership proof");
        }
      } else if (terminal || base == "fill-array-data" ||
                 base.starts_with("aput") || base.starts_with("sput") ||
                 base.starts_with("iput") || base == "filled-new-array" ||
                 base.starts_with("monitor-")) {
        if (hasArray())
          fail(ref,
               "array is mutated, returned, published or identity-observed: " +
                   op.opcode);
      } else if (base != "nop") {
        // Ordinary producers overwrite their destination; unknown operations
        // that consume an alias are rejected, even if another verifier accepts
        // their type-level behavior.
        if (r.empty())
          fail(ref, "consumer producer has no destination");
        for (size_t p = 1; p < r.size(); ++p)
          if (before[r[p]] & Array)
            fail(ref, "unsupported array consumer: " + op.opcode);
        after[r[0]] = Other;
        if (base == "const" || op.opcode.starts_with("const/"))
          if (const auto *v = std::get_if<int64_t>(&op.literal); v && !*v)
            after[r[0]] = Null;
        if (op.opcode.find("wide") != std::string::npos ||
            op.opcode.find("long") != std::string::npos ||
            op.opcode.find("double") != std::string::npos)
          if (r[0] + 1 < count)
            after[r[0] + 1] = Other;
      }
      if (!terminal && base != "goto" && n + 1 < code.size())
        merge(code[n + 1].pc, after);
      if (branch) {
        if (op.target)
          merge(*op.target, after);
        for (auto pc : op.targets)
          merge(pc, after);
      }
      for (const auto &region : method.tries)
        if (region.start <= op.pc && op.pc < region.end)
          for (const auto &handler : region.handlers)
            merge(handler.target, before);
    }
  }

public:
  ArrayUses(const ClassMap &classes, const EnumPlans &enums, Budget &budget)
      : classes(classes), enums(enums), budget(budget) {}
  void run() {
    bool aliases = std::any_of(
        enums.classes.begin(), enums.classes.end(), [](const auto &entry) {
          return entry.second.initializer_tail.has_value();
        });
    if (!aliases)
      return;
    for (const auto &[name, cls] : classes) {
      for (const auto &f : cls.fields)
        fields.emplace(f.reference, &f);
      for (const auto &m : cls.methods) {
        const Method *body = &m;
        if (auto e = enums.classes.find(name); e != enums.classes.end() &&
                                               m.reference.name == "<clinit>" &&
                                               e->second.initializer_tail)
          body = &*e->second.initializer_tail;
        methods.emplace(m.reference, body);
        if (cls.external)
          continue;
        for (const auto &op : body->instructions) {
          budget.tick();
          if (const auto *f = std::get_if<FieldRef>(&op.reference);
              f && (op.opcode == "iget-object" || op.opcode == "sget-object"))
            readers[*f].insert(m.reference);
        }
      }
    }
    for (const auto &[name, plan] : enums.classes)
      if (plan.initializer_tail)
        field(plan.values_field, plan.initializer_tail->reference);
    size_t iterations = 0;
    while (!pending.empty()) {
      if (++iterations > 65536)
        throw Error(
            "Android enum array ownership proof exceeds its method limit");
      auto ref = *pending.begin();
      pending.erase(pending.begin());
      if (auto e = enums.methods.find(ref);
          e != enums.methods.end() && e->second.compiler_managed)
        continue;
      analyze(*methods.at(ref));
    }
  }
};
} // namespace
void verifyEnumArrayUses(const ClassMap &classes, const EnumPlans &enums,
                         Budget &budget) {
  ArrayUses(classes, enums, budget).run();
}
} // namespace neverd::mobile::dalvik
