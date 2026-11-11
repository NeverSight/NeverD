// Typed Dalvik verification and standalone Java source generation.
// This is a native translation of NeverD's first-party Dalvik source engine.
#include "MobileDalvik.h"
#include "MobileDalvikSignature.h"
#include "MobileJavaFieldRestore.h"

#include "neverd/Limits.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/SaveAndRestore.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace neverd::mobile::dalvik {
namespace {
using Strings = std::set<std::string>;
using State = std::vector<Strings>;

[[noreturn]] void javaError(const std::string &Message) {
  throw std::runtime_error(Message);
}

bool starts(std::string_view Text, std::string_view Prefix) {
  return Text.starts_with(Prefix);
}

// Planning and its lookahead must select the same field preparation family.
// Ordinary Body verification still owns each descriptor, opcode and frame.
bool allocationFieldRead(std::string_view Opcode) {
  return (Opcode.starts_with("sget") || Opcode.starts_with("iget")) &&
         Strings{"", "-object", "-wide", "-boolean", "-byte", "-char", "-short"}
             .contains(std::string(Opcode.substr(4)));
}

std::string scalarComparisonExpression(const ScalarComparison &Comparison,
                                       const std::string &A,
                                       const std::string &B) {
  if (Comparison.greater_on_nan)
    return "((" + A + ") < (" + B + ") ? -1 : (" + A + ") == (" + B +
           ") ? 0 : 1)";
  return "((" + A + ") > (" + B + ") ? 1 : (" + A + ") == (" + B +
         ") ? 0 : -1)";
}

std::string integerExpression(const IntegerPreparation &operation,
                              const std::string &input,
                              const std::string &operand) {
  const auto &left = operation.literal_first ? operand : input;
  const auto &right = operation.literal_first ? input : operand;
  return "((" + left + ") " + operation.symbol + " (" + right + "))";
}

bool totalIntegerAllocationPreparation(const Instruction &Op) {
  return bool(integerPreparation(Op));
}
bool anyPrefix(std::string_view Text,
               std::initializer_list<std::string_view> Prefixes) {
  return std::any_of(Prefixes.begin(), Prefixes.end(),
                     [&](auto Prefix) { return starts(Text, Prefix); });
}
bool canThrow(const Instruction &Op) {
  if (const auto Binary = longBinary(Op.opcode))
    return Binary->can_throw;
  return anyPrefix(Op.opcode,
                   {"invoke-", "new-", "filled-", "aget", "aput", "iget",
                    "iput", "sget", "sput", "array-length", "fill-array-data",
                    "check-cast", "instance-of", "throw", "div-int", "rem-int",
                    "monitor-", "const-string", "const-class"});
}
// DEX/smali payload addresses retain data provenance in Instruction::target.
// They are not executable edges; switch destinations are in targets instead.
std::optional<uint32_t> branchTarget(const Instruction &Op) {
  if (Op.opcode == "fill-array-data" || Op.opcode == "packed-switch" ||
      Op.opcode == "sparse-switch")
    return {};
  return Op.target;
}
std::vector<uint32_t> normalSuccessors(const std::vector<Instruction> &Code,
                                       size_t Index) {
  const auto &Op = Code[Index];
  if (anyPrefix(Op.opcode, {"return", "throw"}))
    return {};
  auto destination = [&]() {
    const auto Target = branchTarget(Op);
    if (!Target)
      javaError("branch has no destination");
    return *Target;
  };
  if (starts(Op.opcode, "goto"))
    return {destination()};
  if (Index + 1 == Code.size())
    javaError("method can fall through its code boundary");
  std::vector<uint32_t> Result{Code[Index + 1].pc};
  if (starts(Op.opcode, "if-"))
    Result.push_back(destination());
  else if (Op.opcode.find("switch") != std::string::npos)
    Result.insert(Result.end(), Op.targets.begin(), Op.targets.end());
  std::set<uint32_t> Seen;
  std::erase_if(Result, [&](uint32_t PC) { return !Seen.insert(PC).second; });
  return Result;
}
std::string join(const std::vector<std::string> &Values,
                 std::string_view Separator) {
  std::string Result;
  for (const auto &Value : Values) {
    if (!Result.empty())
      Result += Separator;
    Result += Value;
  }
  return Result;
}
std::vector<std::string> split(std::string_view Text, char Separator) {
  std::vector<std::string> Result;
  size_t Begin = 0;
  for (;;) {
    size_t End = Text.find(Separator, Begin);
    Result.emplace_back(
        Text.substr(Begin, End == Text.npos ? End : End - Begin));
    if (End == Text.npos)
      return Result;
    Begin = End + 1;
  }
}
std::string baseOpcode(std::string_view Opcode) {
  return std::string(Opcode.substr(0, Opcode.find('/')));
}

bool registerCopyShape(const Instruction &Op) {
  const auto Kind = baseOpcode(Op.opcode);
  return (Kind == "move" || Kind == "move-object" || Kind == "move-wide") &&
         (Op.opcode == Kind || Op.opcode == Kind + "/from16" ||
          Op.opcode == Kind + "/16") &&
         Op.registers.size() == 2 && !Op.target && Op.targets.empty() &&
         Op.keys.empty() && Op.data.empty() && !Op.element_width &&
         std::holds_alternative<std::monostate>(Op.reference) &&
         std::holds_alternative<std::monostate>(Op.literal);
}

// Lookahead identifies a boundary without resolving members ahead of the
// ordinary verifier. The complete initializer proof adds type/access checks.
bool allocationInitializerShape(const Instruction &New,
                                const Instruction &Init) {
  const auto *Type = std::get_if<std::string>(&New.reference);
  const auto *Ref = std::get_if<MethodRef>(&Init.reference);
  return Type && New.registers.size() == 1 && Ref && Ref->name == "<init>" &&
         Ref->owner == *Type && Ref->returns == "V" &&
         !Init.registers.empty() && Init.registers[0] == New.registers[0] &&
         baseOpcode(Init.opcode) == "invoke-direct";
}

// These local preparations cannot throw or initialize a class. Linear and
// conditional allocation discovery share the family; ordinary operation
// verification still owns operand types, wide pairs and receiver identity.
bool pureAllocationPreparation(const Instruction &Op) {
  const auto Kind = baseOpcode(Op.opcode);
  return Kind == "move" || Kind == "move-object" || Kind == "move-wide" ||
         Kind == "const" || Kind == "const-wide" || Kind == "nop" ||
         totalIntegerAllocationPreparation(Op);
}
const std::map<std::string, std::string> Primitives = {
    {"V", "void"}, {"Z", "boolean"}, {"B", "byte"},
    {"C", "char"}, {"S", "short"},   {"I", "int"},
    {"J", "long"}, {"F", "float"},   {"D", "double"}};
const std::map<std::string, std::string> ArithmeticTypes = {
    {"int", "I"},  {"long", "J"}, {"float", "F"}, {"double", "D"},
    {"byte", "B"}, {"char", "C"}, {"short", "S"}};

// Dalvik scalar registers hold integer bits, including boolean field/call
// results. Ordinary registers, invocation results and expression temporaries
// must use the same conversion before a later typed read interprets them.
std::string registerStorage(const std::string &Type,
                            const std::string &Expression) {
  if (Type == "F")
    return "((java.lang.Float) null).floatToRawIntBits(" + Expression + ")";
  if (Type == "D")
    return "((java.lang.Double) null).doubleToRawLongBits(" + Expression + ")";
  if (Type == "Z")
    return "(" + Expression + ") ? 1 : 0";
  return Expression;
}

bool isJavaIdentifier(const std::string &Value) {
  static const Strings Keywords = [] {
    Strings Result;
    std::istringstream Words("abstract assert boolean break byte case catch "
                             "char class const continue default "
                             "do double else enum extends final finally float "
                             "for goto if implements import "
                             "instanceof int interface long native new package "
                             "private protected public return "
                             "short static strictfp super switch synchronized "
                             "this throw throws transient try "
                             "void volatile while true false null _");
    for (std::string Word; Words >> Word;)
      Result.insert(Word);
    return Result;
  }();
  auto First = [](unsigned char C) {
    return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || C == '_' ||
           C == '$';
  };
  return !Value.empty() && First(Value[0]) && !Keywords.contains(Value) &&
         std::all_of(Value.begin() + 1, Value.end(), [&](unsigned char C) {
           return First(C) || (C >= '0' && C <= '9');
         });
}
// A package-info class is compiler-owned package metadata, never a Java type
// declaration. Keep this source admission separate from ordinary identifiers.
// Empty metadata needs javac's explicit generation option to preserve its
// original binary identity, rather than omitting a methodless input class.
Strings planEmptyPackageMetadata(const ClassMap &Classes, Budget &B) {
  Strings Result;
  for (const auto &[Name, C] : Classes) {
    B.tick();
    if (C.external || !Name.ends_with("/package-info;"))
      continue;
    if (C.access != Access{"abstract", "interface", "synthetic"} ||
        C.superclass != "Ljava/lang/Object;" || !C.interfaces.empty() ||
        !C.fields.empty() || !C.methods.empty() || C.generic_signature ||
        C.enclosing || C.enclosing_method || C.inner_class_present ||
        C.inner_name || !C.inner_access.empty() ||
        !C.declared_member_types.empty() || C.deprecated ||
        !C.annotation_metadata.empty() || !C.marker_annotations.empty() ||
        C.source_debug_extension || C.suppress_lint || !C.annotations.empty())
      javaError("package-info requires canonical empty package metadata: " +
                Name);
    Result.insert(Name);
  }
  return Result;
}

std::string javaIdentifier(const std::string &Value) {
  if (!isJavaIdentifier(Value))
    javaError("Android identifier cannot be represented in Java: " + Value);
  return Value;
}

std::string javaTypeIdentifier(const std::string &Value) {
  static const Strings Restricted{"permits", "record", "sealed", "var",
                                  "yield"};
  if (Restricted.contains(Value))
    javaError("Android type identifier cannot be represented in Java: " +
              Value);
  return javaIdentifier(Value);
}

// A JVM name is not necessarily a Java identifier. A known public or private
// static body or private instance body has an independent source entry,
// regardless of its synthetic flag. A package-synthetic static body also has
// an independent entry when its declaring source scope is known; its calls
// must retain that exact owner and runtime package. Descriptor/Signature and
// invocation proofs retain ownership of its original contract; this plan
// changes only the source spelling. Virtual interface families need the
// separate complete-family proof below. Reserve names globally, including other
// projections, before sorting the complete runtime identities.
// Input/declaration iteration order cannot select a different spelling, and an
// inherited call cannot acquire a new collision.
bool planIndependentMethodIdentifiers(const ClassMap &Classes,
                                      GenericInheritancePlan &Plan, Budget &B) {
  Strings Reserved;
  std::map<MethodRef, bool> Candidates;
  std::map<MethodRef, MethodRef> hiddenStatics;
  std::set<MethodRef> MetadataReferences;
  for (const auto &[Name, C] : Classes) {
    B.tick();
    // Owned anonymous locals consume the disclosed source EnclosingMethod
    // mapping in the same emitter as the renamed static body. External and
    // named local metadata still require an unchanged declaration name.
    if (C.enclosing_method && (C.external || !isAnonymousLocal(C)))
      MetadataReferences.insert(*C.enclosing_method);
    for (const auto &M : C.methods) {
      B.tick();
      const auto &R = M.reference;
      Reserved.insert(R.name);
      const bool PackageStatic =
          has(M.access, "static") && has(M.access, "synthetic") &&
          !has(M.access, "public") && !has(M.access, "protected") &&
          !has(M.access, "private") && independentSourceScope(C);
      if (!C.external && C.superclass && has(M.access, "public") &&
          has(M.access, "static") && has(M.access, "synthetic") &&
          isJavaIdentifier(R.name) && !M.instructions.empty() &&
          !Plan.source_method_names.contains(R)) {
        const auto parent = Classes.find(*C.superclass);
        if (parent != Classes.end())
          for (const auto &member : parent->second.methods) {
            B.tick();
            const auto &base = member.reference;
            if (base.name == R.name && base.parameters == R.parameters &&
                base.returns != R.returns && has(member.access, "public") &&
                has(member.access, "final") && has(member.access, "static")) {
              hiddenStatics.emplace(R, base);
              Candidates.emplace(R, true);
              break;
            }
          }
      }
      if (!C.external &&
          (has(M.access, "public") || has(M.access, "private") ||
           PackageStatic) &&
          (has(M.access, "static") || has(M.access, "private")) &&
          !has(M.access, "bridge") && !has(M.access, "abstract") &&
          !has(M.access, "native") && !M.instructions.empty() &&
          R.name != "<init>" && R.name != "<clinit>" &&
          !isJavaIdentifier(R.name) && !Plan.source_method_names.contains(R))
        Candidates.emplace(R, has(M.access, "static"));
    }
  }
  for (const auto &[Ref, Alias] : Plan.source_method_names)
    Reserved.insert(Alias.name);
  for (const auto &[Ref, Name] : Plan.void_unit_targets)
    Reserved.insert(Name);
  size_t StaticSuffix = 0, PrivateSuffix = 0;
  bool Changed = false;
  for (const auto &[Ref, Static] : Candidates) {
    // Other EnclosingMethod forms retain their original runtime name.
    if (MetadataReferences.contains(Ref))
      continue;
    auto &Suffix = Static ? StaticSuffix : PrivateSuffix;
    std::string Name;
    do {
      B.tick();
      Name = (Static ? "__neverdStaticMethod" : "__neverdPrivateMethod") +
             std::to_string(Suffix++);
    } while (!Reserved.insert(Name).second);
    GenericInheritancePlan::SourceMethodName Alias{Name, Ref, {}};
    if (const auto conflict = hiddenStatics.find(Ref); conflict != hiddenStatics.end()) {
      Alias.conflicting_contracts.insert(conflict->second);
      Alias.static_return_slot = true;
    } else {
      Alias.identifier_only = true;
    }
    Plan.source_method_names.emplace(Ref, std::move(Alias));
    Changed = true;
  }
  return Changed;
}

bool isReference(std::string_view Type);
unsigned wordWidth(std::string_view Type);

bool planVirtualMethodIdentifiers(const ClassMap &Classes,
                                  const std::map<std::string, std::string> &LocalNames,
                                  const GenericSignaturePlans &Signatures,
                                  GenericInheritancePlan &Plan, Budget &B) {
  Strings Reserved, MetadataNames;
  std::set<MethodRef> MetadataReferences, ClosedCandidates, CarrierTargets,
      BoxedResultTargets;
  for (const auto &[Bridge, Proof] : Signatures.value_carrier_bridges) {
    B.tick();
    if (Bridge.owner == Proof.target.owner)
      CarrierTargets.insert(Proof.target);
  }
  // The canonical boxing receipt retains this exact original provider even
  // when its owner's source interface argument is widened to Object. Only a
  // final independent owner may use it as a spelling-only closed candidate.
  for (const auto &[References, Proof] : Signatures.boxed_interface_results) {
    B.tick();
    if (References.first.owner == Proof.target.owner)
      BoxedResultTargets.insert(Proof.target);
  }
  auto retainedInterfaceScope = [&](const Class &C,
                                    const GenericSignature &Shape,
                                    const auto &Change, const auto &Proofs) {
    if (!Change.scope_origin || !C.generic_signature ||
        Change.original_type != *C.generic_signature ||
        !Signatures.inheritance_contracts.contains(C.name))
      return false;
    return std::any_of(Proofs.begin(), Proofs.end(), [&](const auto &Entry) {
      const auto &[Bridge, Dispatch] = Entry.first;
      if (Bridge.owner != C.name || Entry.second.target != Bridge ||
          Bridge.identity() != *Change.scope_origin)
        return false;
      return std::any_of(Shape.interfaces.begin(), Shape.interfaces.end(),
                         [&](auto Id) {
                           const auto &Edge = Shape.types.at(Id);
                           return Edge.offset == Change.offset &&
                                  Edge.erasure == Dispatch.owner;
                         });
    });
  };
  auto projectedFieldAccessor = [&](const Class &C, const Method &M) {
    const auto Shape = Signatures.classes.find(C.name);
    const auto &R = M.reference;
    const unsigned Words = wordWidth(R.returns);
    const bool Scalar = R.returns != "V" && Primitives.contains(R.returns);
    const auto Element = R.returns.find_first_not_of('[');
    const bool PrimitiveArray = Element != R.returns.npos && Element > 0 &&
                                R.returns.substr(Element) != "V" &&
                                Primitives.contains(R.returns.substr(Element));
    if (!independentSourceScope(C) || Shape == Signatures.classes.end() ||
        Shape->second.source_projections.empty() || M.generic_signature ||
        !R.parameters.empty() || (!Scalar && !PrimitiveArray) ||
        M.instructions.size() != 2 || !M.tries.empty() ||
        M.registers != Words + 1 || !has(M.access, "final") ||
        !has(M.access, "synthetic"))
      return false;
    const auto &Read = M.instructions[0], &Return = M.instructions[1];
    const auto *Field = std::get_if<FieldRef>(&Read.reference);
    if (!Field || Field->owner != C.name || Field->type != R.returns ||
        Read.registers != std::vector<unsigned>{0, Words} ||
        !starts(Read.opcode, "iget") ||
        (PrimitiveArray && Read.opcode != "iget-object") ||
        Return.registers != std::vector<unsigned>{0} ||
        Return.opcode != (PrimitiveArray ? "return-object"
                          : Words == 2   ? "return-wide"
                                         : "return"))
      return false;
    return std::all_of(
        Shape->second.source_projections.begin(),
        Shape->second.source_projections.end(), [&](const auto &Change) {
          if (!Change.scope_origin)
            return false;
          if (Change.kind == "checked-inlined-interface")
            return Scalar && retainedInterfaceScope(
                                 C, Shape->second, Change,
                                 Signatures.checked_inlined_interface_bridges);
          if (Change.kind == "erased-terminal-consumer-raw-interface")
            return PrimitiveArray &&
                   retainedInterfaceScope(C, Shape->second, Change,
                                          Signatures.erased_terminal_consumers);
          if (!Scalar || Change.kind != "checked-value-carrier-interface")
            return false;
          return std::any_of(
              Signatures.value_carrier_bridges.begin(),
              Signatures.value_carrier_bridges.end(), [&](const auto &Entry) {
                const auto &[Bridge, Proof] = Entry;
                return Bridge.owner == C.name && !Proof.interface_owner &&
                       Bridge.identity() == *Change.scope_origin &&
                       Proof.original_interface == Change.original_type &&
                       Shape->second.types.at(Proof.interface_type).offset ==
                           Change.offset &&
                       Shape->second.types.at(Proof.interface_type).erasure ==
                           Change.source_type;
              });
        });
  };
  auto projectedScalarEntry = [&](const Class &C, const Method &M) {
    const auto Shape = Signatures.classes.find(C.name);
    const auto &R = M.reference;
    auto scalar = [&](const std::string &Type) {
      return Type != "V" && Primitives.contains(Type);
    };
    if (!independentSourceScope(C) || M.generic_signature ||
        R.parameters.empty() || !scalar(R.returns) ||
        !std::all_of(R.parameters.begin(), R.parameters.end(), scalar) ||
        Shape == Signatures.classes.end() ||
        Shape->second.source_projections.empty())
      return false;
    // Raw interface arguments cannot change this fixed scalar contract. The
    // ordinary final-owner ancestry and body/call checks still own its slot,
    // operations, result words and exceptions.
    return std::all_of(
        Shape->second.source_projections.begin(),
        Shape->second.source_projections.end(), [&](const auto &Change) {
          return Change.kind == "erased-terminal-consumer-raw-interface" &&
                 retainedInterfaceScope(C, Shape->second, Change,
                                        Signatures.erased_terminal_consumers);
        });
  };
  std::map<std::string, std::vector<const Method *>> Families;
  std::map<std::string, std::vector<const Method *>> IndependentDeclarations;
  std::set<MethodRef> CovariantNames, closed_protected;
  std::set<MethodRef> boxedScalarTargets;
  for (const auto &[bridge, proof] : Signatures.nominal_boxed_results)
    if (bridge.name == proof.target.name)
      boxedScalarTargets.insert(proof.target);
  for (const auto &[Ref, Proof] : Plan.bridges) {
    B.tick();
    if (Proof.target.owner == Ref.owner && Proof.target.name == Ref.name &&
        Proof.dispatch.name == Ref.name &&
        Proof.target.parameters == Ref.parameters &&
        Proof.dispatch.parameters == Ref.parameters &&
        Proof.dispatch.returns == Ref.returns &&
        Proof.target.returns != Ref.returns && isReference(Ref.returns) &&
        isReference(Proof.target.returns) && !Proof.result_field &&
        !Proof.preserve_body && !Proof.protected_contract &&
        !Proof.boolean_parameters && !Proof.terminal_throw_result &&
        !Proof.guarded_reference_result && !Proof.nonreturning_boolean_result &&
        !Proof.generated_parameter_adaptation &&
        (!Proof.boxed_reference_result ||
         (Signatures.nominal_boxed_results.contains(Ref) &&
          Signatures.nominal_boxed_results.at(Ref).forward_to == Proof.target)))
      CovariantNames.insert(Ref);
  }
  auto retainedScope = [&](const Class &C, const Method &M) {
    const auto Scope = Signatures.classes.find(C.name);
    const auto *Contract = sourceMethodSignature(M.reference, Signatures);
    return !Signatures.inheritance_contracts.contains(C.name) &&
           (Scope == Signatures.classes.end()
                ? !C.generic_signature
                : Scope->second.source_projections.empty()) &&
           (Contract ? Contract->source_projections.empty()
                     : !M.generic_signature);
  };
  auto protectedScope = [&](const Class &C, const Method &M) {
    if (!independentSourceScope(C) || C.generic_signature ||
        !retainedScope(C, M))
      return false;
    const auto *Contract = sourceMethodSignature(M.reference, Signatures);
    // A fixed callback Signature changes no virtual slot. Keep the validated
    // source contract intact; class or method formals need a separate proof.
    return !M.generic_signature ||
           (Contract && Contract->type_parameters.empty() &&
            std::none_of(Contract->types.begin(), Contract->types.end(),
                         [](const auto &Type) {
                           return Type.kind == GenericType::Kind::TypeVariable;
                         }));
  };
  for (const auto &[Name, C] : Classes) {
    B.tick();
    if (C.enclosing_method) {
      // Complete source virtual families can share a spelling with their
      // owned anonymous EnclosingMethod metadata. Other metadata remains an
      // unchanged external contract; generated-family conflicts are added
      // below.
      if (C.external || !isAnonymousLocal(C))
        MetadataNames.insert(C.enclosing_method->name);
      // Owned anonymous declarations use the common source EnclosingMethod
      // receipt for both complete families and independent closed entries.
      // External metadata and named locals still need the original spelling.
      if (C.external || !isAnonymousLocal(C))
        MetadataReferences.insert(*C.enclosing_method);
    }
    for (const auto &M : C.methods) {
      B.tick();
      const auto &R = M.reference;
      Reserved.insert(R.name);
      const auto typed_alias = Plan.source_method_names.find(R);
      if (typed_alias != Plan.source_method_names.end() &&
          boxedScalarTargets.contains(R))
        continue; // The incompatible scalar descriptor already has its own
                  // name.
      if (typed_alias != Plan.source_method_names.end() &&
          (typed_alias->second.checked_reference_bridge ||
           (typed_alias->second.identifier_only &&
            (has(M.access, "private") || has(M.access, "static"))))) {
        if (typed_alias->second.identifier_only &&
            (has(M.access, "private") || has(M.access, "static")))
          IndependentDeclarations[R.name].push_back(&M);
        continue;
      }
      if (!R.name.starts_with('<') && !isJavaIdentifier(R.name)) {
        Families[R.name].push_back(&M);
        const auto *contract = sourceMethodSignature(R, Signatures);
        // A raw ancestor or named local projection can change its owner's
        // source header without changing a descriptor-only method. The
        // complete ancestry check below still has to rule out overrides.
        const bool fixed = !M.generic_signature && !contract &&
                           !has(M.access, "synthetic") &&
                           !Signatures.boxed_superclass_source_results.contains(R) &&
                           !Plan.void_unit_targets.contains(R) &&
                           !Plan.nonreturning_results.contains(R);
        const bool FinalOwner =
            has(C.access, "final") && !has(C.access, "abstract") &&
            ((has(M.access, "public") &&
              ((independentSourceScope(C) &&
                (retainedScope(C, M) || BoxedResultTargets.contains(R))) ||
               CarrierTargets.contains(R) || projectedFieldAccessor(C, M) ||
               projectedScalarEntry(C, M))) ||
             ((has(M.access, "public") || has(M.access, "protected")) &&
              fixed && (independentSourceScope(C) || LocalNames.contains(C.name))));
        // A final method retains the class's variables in its unchanged
        // Signature. Renaming it introduces no substitution or new override.
        const bool class_formal = contract &&
            std::any_of(contract->types.begin(), contract->types.end(),
                        [&](const auto &type) {
                          return type.kind == GenericType::Kind::TypeVariable &&
                                 type.variable_owner == C.name;
                        });
        const bool ProtectedFinal =
            has(C.access, "public") && (!C.generic_signature || class_formal) &&
            independentSourceScope(C) && retainedScope(C, M) &&
            has(M.access, "protected") && has(M.access, "final") &&
            !has(M.access, "synthetic");
        // A final public instance entry cannot acquire overriding descendants,
        // even on an abstract or open owner. Retain its full method scope,
        // including compiler-synthetic generic callbacks. The same complete
        // known-ancestry proof below must still exclude inherited name slots.
        const bool PublicFinal =
            has(C.access, "public") && !C.generic_signature &&
            independentSourceScope(C) && retainedScope(C, M) &&
            has(M.access, "public") && has(M.access, "final");
        if (!C.external && !has(C.access, "interface") &&
            (FinalOwner || ProtectedFinal || PublicFinal) &&
            !has(M.access, "static") && !has(M.access, "bridge") &&
            !has(M.access, "native") && !has(M.access, "abstract") &&
            !M.instructions.empty()) {
          ClosedCandidates.insert(R);
          if (has(C.access, "final") && has(M.access, "protected"))
            closed_protected.insert(R);
        }
      }
    }
  }
  for (const auto &[Ref, Alias] : Plan.source_method_names)
    Reserved.insert(Alias.name);
  for (const auto &[Ref, Name] : Plan.void_unit_targets)
    Reserved.insert(Name);
  // These conflicts depend on original names, not the family being visited.
  // Index them once so many independent names cannot rescan the whole input.
  std::map<std::string, std::set<MethodRef>> generatedCovariance;
  for (const auto &[Ref, Proof] : Plan.generated_interface_bridges) {
    B.tick();
    if (!Proof.generated_parameter_adaptation &&
        Ref.name == Proof.target.name && Ref.name == Proof.dispatch.name &&
        Ref.parameters == Proof.target.parameters &&
        Ref.parameters == Proof.dispatch.parameters &&
        Ref.returns == Proof.dispatch.returns &&
        isReference(Proof.target.returns) && isReference(Ref.returns)) {
      generatedCovariance[Ref.name].insert(Proof.target);
      generatedCovariance[Ref.name].insert(Proof.dispatch);
    } else {
      MetadataNames.insert(Ref.name);
    }
  }
  for (const auto &[Ref, Proof] : Plan.generated_class_bridges) {
    B.tick();
    MetadataNames.insert(Ref.name);
  }
  std::map<MethodRef, bool> ClosedProofs;
  auto provenClosedRoot = [&](const MethodRef &R) {
    if (const auto I = ClosedProofs.find(R); I != ClosedProofs.end())
      return I->second;
    if (!ClosedCandidates.contains(R) || Plan.bridges.contains(R) ||
        Plan.retained_plain_bridges.contains(R) ||
        MetadataReferences.contains(R))
      return false;
    std::map<std::string, unsigned> States;
    std::function<bool(const std::string &, unsigned)> known_ancestry =
        [&](const std::string &Name, unsigned Depth) {
          B.tick();
          if (Depth > 64 || States.size() > 128)
            return false;
          const auto Found = Classes.find(Name);
          if (Found == Classes.end())
            return Name == "Ljava/lang/Object;";
          auto &State = States[Name];
          if (State)
            return State == 2;
          State = 1;
          const auto &C = Found->second;
          // A verified carrier bridge already owns its target's complete
          // scoped contract, including a disclosed raw interface projection.
          // Its final local owner may receive a spelling-only source name;
          // every ancestor still needs the ordinary independent-scope proof.
          if (!independentSourceScope(C) &&
              !(Name == R.owner &&
                (CarrierTargets.contains(R) || LocalNames.contains(Name))))
            return false;
          if (Name != R.owner)
            for (const auto &M : C.methods) {
              B.tick();
              if (M.reference.name == R.name)
                return false; // Every inherited overload needs its own proof.
            }
          if (Name == "Ljava/lang/Object;") {
            if (C.superclass || !C.interfaces.empty() ||
                has(C.access, "interface"))
              return false;
          } else {
            if (!C.superclass || !known_ancestry(*C.superclass, Depth + 1))
              return false;
            for (const auto &Ancestor : C.interfaces)
              if (!known_ancestry(Ancestor, Depth + 1))
                return false;
          }
          State = 2;
          return true;
        };
    const bool Proven = known_ancestry(R.owner, 0);
    ClosedProofs.emplace(R, Proven);
    return Proven;
  };
  bool Changed = false;
  size_t Suffix = 0;
  auto planFamily = [&](const std::string &Name,
                        const std::vector<const Method *> &Methods) {
    B.tick();
    if (Methods.size() > 256)
      return false;
    std::set<MethodRef> Members, Roots, ClassRoots, PackageSyntheticRoots,
        ProtectedAbstractRoots, SyntheticInterfaceMembers;
    Strings SyntheticPackages;
    bool Exact = true;
    for (const auto *M : Methods) {
      const auto &R = M->reference;
      const auto &C = Classes.at(R.owner);
      const auto Scope = Signatures.classes.find(C.name);
      // An independent owner may retain its complete class-formal scope.
      // That unchanged header grants no method Signature or source projection;
      // the package family and every original call still need their proofs.
      // A captured member already has a canonical source-scope receipt.
      // Erasing only its outer Object-bound formals changes no virtual slot;
      // keep that projection separate from this spelling-only package proof.
      // Own formals, method Signatures and other header projections still need
      // their own source contract. The complete family and call checks below
      // apply unchanged to this member.
      const auto *Contract = sourceMethodSignature(R, Signatures);
      const bool CapturedOwner =
          Signatures.captured_inner_classes.contains(C.name) &&
          !Signatures.inheritance_contracts.contains(C.name) &&
          !M->generic_signature &&
          (!Contract || Contract->source_projections.empty()) &&
          Scope != Signatures.classes.end() &&
          Scope->second.type_parameters.empty() &&
          std::all_of(Scope->second.source_projections.begin(),
                      Scope->second.source_projections.end(),
                      [](const auto &Change) {
                        return Change.kind == "static-inner-type-variable";
                      });
      const bool PackageOwner =
          CapturedOwner ||
          (retainedScope(C, *M) && (Scope == Signatures.classes.end() ||
                                    Scope->second.type_parameters.empty() ||
                                    independentSourceScope(C)));
      const bool PackageSynthetic =
          has(M->access, "synthetic") && !has(M->access, "public") &&
          !has(M->access, "protected") && !has(M->access, "private") &&
          !has(M->access, "abstract") && !has(C.access, "interface") &&
          PackageOwner && !M->generic_signature;
      const auto retained = Plan.retained_plain_bridges.find(R);
      const bool CheckedReference =
          retained != Plan.retained_plain_bridges.end() &&
          retained->second.checked_reference_return;
      const bool InterfaceSuper =
          retained != Plan.retained_plain_bridges.end() &&
          retained->second.interface_super_forwarder && retainedScope(C, *M);
      const bool Covariant = CovariantNames.contains(R);
      const bool retainedImplementation =
          retained != Plan.retained_plain_bridges.end() &&
          ((retained->second.contract_implementation &&
            provenStaticReceiverImplementation(C, *M, Classes, Signatures,
                                               B)) ||
           (retained->second.nominal_boxed_result &&
            Signatures.nominal_boxed_results.contains(R)));
      // A compiler-generated interface declaration/default or concrete
      // implementation retains the ordinary full virtual contract. Its
      // synthetic flag is a disclosed metadata loss. An owned interface root
      // still needs the complete family/ancestry proof below; no bridge,
      // external declaration or changed source scope gains a spelling.
      const bool InterfaceDeclaration =
          has(C.access, "interface") && !has(C.access, "annotation") &&
          independentSourceScope(C) &&
          (has(M->access, "abstract") == M->instructions.empty());
      const bool SyntheticInterface =
          has(M->access, "public") && has(M->access, "synthetic") &&
          !has(M->access, "bridge") && retainedScope(C, *M) &&
          (InterfaceDeclaration ||
           (!has(C.access, "interface") && !has(M->access, "abstract")));
      const bool ProtectedMember = has(M->access, "protected") &&
                                   !has(C.access, "interface") &&
                                   protectedScope(C, *M);
      // Public abstract declarations use the same validated full source
      // contracts as concrete class roots. A retained generic Signature does
      // not change virtual dispatch or grant any source type projection.
      const bool AbstractClassRoot =
          has(C.access, "abstract") && !has(C.access, "final") &&
          !has(C.access, "interface") && has(M->access, "abstract") &&
          !has(M->access, "final") && independentSourceScope(C) &&
          retainedScope(C, *M);
      if (ProtectedMember && AbstractClassRoot)
        ProtectedAbstractRoots.insert(R);
      if (C.external ||
          (!has(M->access, "public") && !PackageSynthetic &&
           !ProtectedMember) ||
          has(M->access, "static") || has(M->access, "native") ||
          (has(M->access, "bridge") && !CheckedReference && !InterfaceSuper &&
           !Covariant && !retainedImplementation) ||
          (has(M->access, "synthetic") && !PackageSynthetic &&
           !CheckedReference && !InterfaceSuper && !Covariant &&
           !SyntheticInterface && !retainedImplementation) ||
          (!has(M->access, "abstract") && M->instructions.empty()) ||
          Plan.source_method_names.contains(R) ||
          (Plan.bridges.contains(R) && !Covariant) ||
          (Plan.retained_plain_bridges.contains(R) && !CheckedReference &&
           !InterfaceSuper && !retainedImplementation)) {
        Exact = false;
        break;
      }
      Members.insert(R);
      if (SyntheticInterface)
        SyntheticInterfaceMembers.insert(R);
      if (PackageSynthetic) {
        PackageSyntheticRoots.insert(R);
        SyntheticPackages.insert(std::string(packageOf(R.owner)));
      }
      // A concrete owned interface default is also an original dispatch root.
      // Its unchanged full source contract and body remain validated by the
      // common inheritance/Body pipeline before source is published.
      if (has(C.access, "interface") &&
          (has(M->access, "abstract") || retainedScope(C, *M)))
        Roots.insert(R);
      else if (!has(C.access, "interface") && !PackageSynthetic &&
               !SyntheticInterface &&
               (!has(M->access, "abstract") || AbstractClassRoot) &&
               retainedScope(C, *M))
        ClassRoots.insert(R);
    }
    if (!Exact ||
        (Roots.empty() && ClassRoots.empty() && PackageSyntheticRoots.empty()))
      return false;
    const bool ProtectedFamily =
        std::any_of(Methods.begin(), Methods.end(), [](const Method *M) {
          return has(M->access, "protected");
        });
    if (ProtectedFamily) {
      if (!Roots.empty() || !PackageSyntheticRoots.empty())
        return false;
      // Abstract declarations retain their concrete overloads. An independent
      // concrete entry can also own a root on a public abstract class: it has
      // its own original body and complete fixed source scope, rather than
      // borrowing a same-descriptor declaration from another component.
      Strings AbstractOwners;
      for (const auto &Root : ProtectedAbstractRoots)
        AbstractOwners.insert(Root.owner);
      auto ProtectedRoots = ProtectedAbstractRoots;
      for (const auto &Root : ClassRoots) {
        B.tick();
        const auto &C = Classes.at(Root.owner);
        const auto M =
            std::find_if(C.methods.begin(), C.methods.end(),
                         [&](const Method &M) { return M.reference == Root; });
        const bool Concrete =
            has(C.access, "public") && has(C.access, "abstract") &&
            !has(C.access, "final") && M != C.methods.end() &&
            !has(M->access, "abstract") && !has(M->access, "bridge") &&
            !has(M->access, "synthetic") && !M->instructions.empty() &&
            protectedScope(C, *M);
        if (AbstractOwners.contains(Root.owner) || Concrete)
          ProtectedRoots.insert(Root);
      }
      if (ProtectedRoots.empty())
        return false;
      for (const auto *M : Methods) {
        const auto &C = Classes.at(M->reference.owner);
        if (!protectedScope(C, *M) ||
            std::none_of(ProtectedRoots.begin(), ProtectedRoots.end(),
                         [&](const MethodRef &Root) {
                           return M->reference.parameters == Root.parameters &&
                                  M->reference.returns == Root.returns;
                         }))
          return false;
      }
      ClassRoots = std::move(ProtectedRoots);
    }
    // Package-synthetic bodies form a separate complete family. Preserve
    // their original package access; no public/mixed family or method Signature
    // gains admission from a compiler flag. Invocation validation additionally
    // requires exact declaring references and callers in this runtime package.
    if (!PackageSyntheticRoots.empty() &&
        (PackageSyntheticRoots.size() != Members.size() ||
         SyntheticPackages.size() != 1))
      return false;
    // An independent private/static alias cannot satisfy an abstract interface
    // slot. If its concrete owner belongs to this family, require an original
    // concrete family member in its known ancestry; otherwise Java cannot
    // preserve that class's unresolved runtime interface entry.
    if (const auto I = IndependentDeclarations.find(Name);
        I != IndependentDeclarations.end())
      for (const auto *M : I->second) {
        const auto &C = Classes.at(M->reference.owner);
        if (has(C.access, "abstract") || has(C.access, "interface"))
          continue;
        std::set<MethodRef> Required;
        for (const auto &R : Roots) {
          B.tick();
          if (R.parameters == M->reference.parameters &&
              R.returns == M->reference.returns)
            Required.insert(R);
        }
        if (Required.empty())
          continue;
        bool Related = false, Concrete = false;
        Strings Seen;
        std::vector<std::string> Pending{C.name};
        for (size_t At = 0; At < Pending.size(); ++At) {
          B.tick();
          const auto &Owner = Pending[At];
          if (Owner == "Ljava/lang/Object;" || !Seen.insert(Owner).second)
            continue;
          const auto Found = Classes.find(Owner);
          if (Found == Classes.end() || Seen.size() > 128)
            return false;
          auto Slot = M->reference;
          Slot.owner = Owner;
          Related |= Required.contains(Slot);
          if (Members.contains(Slot))
            for (const auto &Declaration : Found->second.methods) {
              B.tick();
              if (Declaration.reference == Slot &&
                  !has(Declaration.access, "abstract"))
                Concrete = true;
            }
          if (Found->second.superclass)
            Pending.push_back(*Found->second.superclass);
          Pending.insert(Pending.end(), Found->second.interfaces.begin(),
                         Found->second.interfaces.end());
        }
        if (Related && !Concrete)
          return false;
      }
    // A generated adaptation or enclosing-method metadata change needs its
    // own source receipt. A spelling-only projection cannot authorize it.
    if (MetadataNames.contains(Name))
      return false;
    if (const auto generated = generatedCovariance.find(Name);
        generated != generatedCovariance.end() &&
        !std::includes(Members.begin(), Members.end(),
                       generated->second.begin(), generated->second.end()))
      return false;
    std::map<MethodRef, MethodRef> Declarations;
    for (const auto &R : Members) {
      if (!Exact)
        break;
      std::set<std::string> Seen;
      std::set<MethodRef> ReachableRoots;
      std::optional<MethodRef> ClassRoot;
      std::vector<std::string> Pending{R.owner};
      for (size_t At = 0; At < Pending.size(); ++At) {
        B.tick();
        const auto Owner = Pending[At];
        if (Owner == "Ljava/lang/Object;" || !Seen.insert(Owner).second)
          continue;
        const auto Found = Classes.find(Owner);
        if (Found == Classes.end() || Seen.size() > 128) {
          Exact = false;
          break;
        }
        auto Required = R;
        Required.owner = Owner;
        if (Roots.contains(Required) ||
            PackageSyntheticRoots.contains(Required))
          ReachableRoots.insert(Required);
        if (ClassRoots.contains(Required))
          ClassRoot = Required;
        const auto &C = Found->second;
        if (C.superclass)
          Pending.push_back(*C.superclass);
        Pending.insert(Pending.end(), C.interfaces.begin(), C.interfaces.end());
      }
      // A public concrete source superclass can root a complete known
      // override family. The class-chain traversal retains its oldest exact
      // declaration, including an abstract class with a concrete method.
      // Interface roots retain precedence. Unknown or unchanged SDK members
      // still invalidate this spelling receipt; it grants no call semantics.
      if (SyntheticInterfaceMembers.contains(R) && ReachableRoots.empty())
        Exact = false;
      if (ReachableRoots.empty() && ClassRoot)
        ReachableRoots.insert(*ClassRoot);
      Exact &= !ReachableRoots.empty();
      if (Exact)
        Declarations.emplace(R, *ReachableRoots.begin());
    }
    if (!Exact)
      return false;
    // All overloads bearing this original JVM name share one globally fresh
    // spelling. Ordinary inheritance validation has already checked full
    // signatures and dispatch; original identities remain its authority.
    std::string Spelling;
    do {
      B.tick();
      Spelling = "__neverdVirtualMethod" + std::to_string(Suffix++);
    } while (!Reserved.insert(Spelling).second);
    // A raw implemented interface changes Java's most-specific overload
    // test. Closed methods of one final class have no override edges to share,
    // so keep distinct descriptor spellings when that change affects a sibling.
    const bool closedOverloads = Members.size() > 1 &&
        std::all_of(Members.begin(), Members.end(), [&](const MethodRef &ref) {
          return ref.owner == Members.begin()->owner &&
                 has(Classes.at(ref.owner).access, "final") &&
                 Declarations.at(ref) == ref && !Plan.bridges.contains(ref) &&
                 !Plan.retained_plain_bridges.contains(ref);
        });
    bool rawOverlap = false;
    if (closedOverloads)
      for (const auto &ref : Members)
        for (size_t i = 0; i < ref.parameters.size(); ++i) {
          const auto cls = Signatures.classes.find(ref.parameters[i]);
          if (cls == Signatures.classes.end() || cls->second.source_projections.empty())
            continue;
          for (const auto id : cls->second.interfaces) {
            const auto &edge = cls->second.types.at(id);
            if (edge.segments.empty() || !edge.segments.back().arguments.empty())
              continue;
            rawOverlap |= std::any_of(Members.begin(), Members.end(), [&](const MethodRef &sibling) {
              B.tick();
              return ref.parameters.size() == sibling.parameters.size() &&
                     sibling.parameters[i] == edge.erasure;
            });
          }
        }
    for (const auto &R : Members) {
      if (rawOverlap && R != *Members.begin())
        do {
          B.tick();
          Spelling = "__neverdVirtualMethod" + std::to_string(Suffix++);
        } while (!Reserved.insert(Spelling).second);
      GenericInheritancePlan::SourceMethodName Alias{
          Spelling, Declarations.at(R), {}};
      Alias.identifier_only = true;
      Alias.identifier_family = rawOverlap ? std::set<MethodRef>{R} : Members;
      Alias.interface_identifier_family =
          has(Classes.at(Alias.declaration.owner).access, "interface");
      Alias.protected_identifier_family = ProtectedFamily;
      Plan.source_method_names.emplace(R, std::move(Alias));
    }
    Changed = true;
    return true;
  };
  for (const auto &[Name, Methods] : Families) {
    if (Methods.size() > 256 || planFamily(Name, Methods))
      continue;
    // A failed family may contain unrelated final entries whose complete
    // existing ancestry/call receipt proves an independent source name. Remove
    // only those certified roots, then recheck every remaining family member
    // with the same interface/class contract proof. Successful complete
    // families retain their existing shared spelling.
    std::vector<const Method *> Remaining;
    for (const auto *M : Methods) {
      B.tick();
      if (!provenClosedRoot(M->reference))
        Remaining.push_back(M);
    }
    if (!Remaining.empty() && Remaining.size() != Methods.size())
      planFamily(Name, Remaining);
  }
  // Independent final entries have no overriding descendants. An unrelated
  // dependency with the same JVM name cannot supply or invalidate their scope.
  // Existing complete interface/package families retain their shared spelling.
  for (const auto &R : ClosedCandidates) {
    B.tick();
    if (Plan.source_method_names.contains(R) || Plan.bridges.contains(R) ||
        Plan.retained_plain_bridges.contains(R) ||
        MetadataReferences.contains(R))
      continue;
    if (!provenClosedRoot(R))
      continue;
    std::string Spelling;
    do {
      B.tick();
      Spelling = "__neverdVirtualMethod" + std::to_string(Suffix++);
    } while (!Reserved.insert(Spelling).second);
    GenericInheritancePlan::SourceMethodName Alias{Spelling, R, {}};
    Alias.identifier_only = true;
    Alias.identifier_family.insert(R);
    // An instance method on a final class has no overriding descendants.
    // Retain protected receiver checks when the method itself is not final.
    Alias.protected_identifier_family = closed_protected.contains(R);
    Plan.source_method_names.emplace(R, std::move(Alias));
    Changed = true;
  }
  return Changed;
}

// InnerClasses describes lexical access. The class definition itself can be
// package-accessible, which allows another original class in that package
// to use the type in its own field or method descriptor.
struct MemberPackageAccess {
  std::set<MethodRef> callers;
  Strings descriptor_users;
  Strings instruction_users;
  bool interface_member = false;
};

// A DEX field reference can link to a private member across runtime packages.
// Keep the actual field instruction so the VM performs access checking at its
// original location. Javac needs a visible source declaration; the rebuild
// manifest restores the field's exact original visibility before loading.
std::set<FieldRef> privateFieldReadsAcrossPackages(const ClassMap &classes,
                                                   Budget &budget) {
  std::set<FieldRef> candidates, reads, writes;
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (cls.external || !cls.enclosing || !cls.inner_name ||
        !has(cls.inner_access, "static") || !has(cls.inner_access, "public"))
      continue;
    for (const auto &field : cls.fields) {
      budget.tick();
      if ((field.access == Access{"private"} ||
           field.access == Access{"private", "final"}) &&
          isJavaIdentifier(field.reference.name))
        candidates.insert(field.reference);
    }
  }
  if (candidates.empty())
    return {};
  for (const auto &[name, cls] : classes) {
    if (cls.external)
      continue;
    for (const auto &method : cls.methods)
      for (const auto &op : method.instructions) {
        budget.tick();
        const auto *field = std::get_if<FieldRef>(&op.reference);
        if (!field || !candidates.contains(*field) ||
            packageOf(name) == packageOf(field->owner))
          continue;
        if (starts(op.opcode, "iget"))
          reads.insert(*field);
        else if (starts(op.opcode, "iput"))
          writes.insert(*field);
      }
  }
  for (const auto &field : writes)
    reads.erase(field);
  return reads;
}

std::map<std::string, MemberPackageAccess>
planMemberPackageAccess(const ClassMap &Classes,
                        const GenericSignaturePlans &Signatures, Budget &B) {
  std::set<MethodRef> CalledMembers;
  Strings DescriptorTypes;
  std::map<std::string, MemberPackageAccess> Result;
  for (const auto &[Name, C] : Classes) {
    B.tick();
    if (C.external || !C.enclosing || !C.inner_name)
      continue;
    const auto enclosing = Classes.find(*C.enclosing);
    if (enclosing == Classes.end())
      javaError("missing source enclosing class for " + Name);
    if (has(enclosing->second.access, "interface") &&
        (has(C.inner_access, "private") || has(C.inner_access, "protected"))) {
      Result[Name].interface_member = true;
      continue;
    }
    if (!has(C.inner_access, "private") || has(C.access, "public") ||
        has(C.access, "protected") || has(C.access, "private"))
      continue;
    if (Signatures.captured_inner_classes.contains(Name)) {
      for (const auto &M : C.methods) {
        B.tick();
        if (!has(M.access, "private"))
          CalledMembers.insert(M.reference);
      }
    } else if (has(C.inner_access, "static") &&
               !has(Classes.at(*C.enclosing).access, "interface")) {
      DescriptorTypes.insert(Name);
    }
  }
  if (CalledMembers.empty() && DescriptorTypes.empty())
    return Result;
  std::map<std::string, std::vector<std::string>> visibleOwners;
  for (const auto &[name, cls] : Classes) {
    if (cls.external)
      continue;
    // Naming a nested member also needs access to every enclosing type.
    for (const Class *type = &cls; type->enclosing;
         type = &Classes.at(*type->enclosing)) {
      B.tick();
      if (DescriptorTypes.contains(type->name))
        visibleOwners[name].push_back(type->name);
    }
  }
  auto sourceRoot = [&](const Class &C) {
    const Class *Root = &C;
    while (Root->enclosing || Root->enclosing_method) {
      B.tick();
      Root = &Classes.at(Root->enclosing_method ? Root->enclosing_method->owner
                                                : *Root->enclosing);
    }
    return Root->name;
  };
  for (const auto &[Name, C] : Classes) {
    if (C.external || !independentSourceScope(C))
      continue;
    auto separatePackageUser = [&](const std::string &type) {
      return packageOf(Name) == packageOf(type) &&
             sourceRoot(C) != sourceRoot(Classes.at(type));
    };
    auto descriptor = [&](const std::string &type, const auto &identity,
                          std::optional<uint32_t> pc = {}) {
      B.tick();
      const auto start = type.find_first_not_of('[');
      if (start == type.npos || type[start] != 'L')
        return;
      const auto found = start ? visibleOwners.find(type.substr(start))
                               : visibleOwners.find(type);
      if (found == visibleOwners.end())
        return;
      for (const auto &candidate : found->second)
        if (separatePackageUser(candidate)) {
          if (pc)
            Result[candidate].instruction_users.insert(identity() + "@" + std::to_string(*pc));
          else
            Result[candidate].descriptor_users.insert(identity());
        }
    };
    if (C.superclass)
      descriptor(*C.superclass, [&] { return Name; });
    for (const auto &interface : C.interfaces)
      descriptor(interface, [&] { return Name; });
    for (const auto &F : C.fields)
      descriptor(F.reference.type, [&] {
        return Name + "->" + F.reference.name + ":" + F.reference.type;
      });
    for (const auto &M : C.methods) {
      auto id = [&] { return M.reference.identity(); };
      descriptor(M.reference.returns, id);
      for (const auto &parameter : M.reference.parameters)
        descriptor(parameter, id);
      for (const auto &Op : M.instructions) {
        B.tick();
        const auto *Ref = std::get_if<MethodRef>(&Op.reference);
        if (Ref)
          descriptor(Ref->owner, id, Op.pc);
        else if (const auto *field = std::get_if<FieldRef>(&Op.reference))
          descriptor(field->owner, id, Op.pc);
        else if (const auto *type = std::get_if<std::string>(&Op.reference);
                 type && (Op.opcode == "check-cast" || Op.opcode == "instance-of" ||
                          Op.opcode == "const-class" || Op.opcode == "new-instance" ||
                          Op.opcode == "new-array" || baseOpcode(Op.opcode) == "filled-new-array"))
          descriptor(*type, id, Op.pc);
        if (!Ref || !CalledMembers.contains(*Ref) ||
            (baseOpcode(Op.opcode) != "invoke-virtual" &&
             baseOpcode(Op.opcode) != "invoke-super" &&
             baseOpcode(Op.opcode) != "invoke-direct" &&
             baseOpcode(Op.opcode) != "invoke-static") ||
            !separatePackageUser(Ref->owner))
          continue;
        Result[Ref->owner].callers.insert(M.reference);
      }
    }
  }
  return Result;
}

bool planCapturedFieldIdentifiers(const ClassMap &Classes,
                                  const GenericSignaturePlans &Generics,
                                  GenericInheritancePlan &Plan, Budget &B) {
  Strings Reserved;
  std::set<FieldRef> Captures;
  for (const auto &[Name, C] : Classes) {
    for (const auto &F : C.fields) {
      B.tick();
      Reserved.insert(F.reference.name);
    }
    for (const auto &M : C.methods) {
      B.tick();
      Reserved.insert(M.reference.name);
    }
  }
  for (const auto &[Ref, Alias] : Plan.source_method_names)
    Reserved.insert(Alias.name);
  for (const auto &[Ctor, Fields] : Generics.constructor_captures)
    for (const auto &[Parameter, Ref] : Fields) {
      B.tick();
      const auto Owner = Classes.find(Ref.owner);
      if (Owner == Classes.end() || Owner->second.external ||
          Ctor.owner != Ref.owner || isJavaIdentifier(Ref.name))
        continue;
      const auto &C = Owner->second;
      const auto Field =
          std::find_if(C.fields.begin(), C.fields.end(),
                       [&](const auto &F) { return F.reference == Ref; });
      if (Field == C.fields.end() || !has(Field->access, "final") ||
          !has(Field->access, "synthetic") || has(Field->access, "static") ||
          !std::holds_alternative<std::monostate>(Field->value) ||
          Parameter >= Ctor.parameters.size() ||
          Ctor.parameters[Parameter] != Ref.type)
        continue;
      Captures.insert(Ref);
    }
  size_t Suffix = 0;
  for (const auto &Ref : Captures) {
    std::string Name;
    do {
      B.tick();
      Name = "__neverdCapturedField" + std::to_string(Suffix++);
    } while (!Reserved.insert(Name).second);
    Plan.source_field_names.emplace(Ref, std::move(Name));
  }
  return !Captures.empty();
}

// Reserve helper names independently of semantic admission. Body owns the
// exact forwarding proof; this pass only makes spelling deterministic across
// input order and avoids conflicts with inherited methods and source aliases.
std::map<MethodRef, std::string>
planStaticSuperHelperNames(const ClassMap &Classes,
                           GenericInheritancePlan &Plan, Budget &B) {
  Strings Reserved;
  std::set<MethodRef> Candidates;
  for (const auto &[Name, C] : Classes)
    for (const auto &M : C.methods) {
      B.tick();
      Reserved.insert(M.reference.name);
      if (!C.external && has(M.access, "static"))
        for (const auto &Op : M.instructions) {
          B.tick();
          if (baseOpcode(Op.opcode) == "invoke-super")
            Candidates.insert(M.reference);
        }
    }
  for (const auto &[Ref, Alias] : Plan.source_method_names)
    Reserved.insert(Alias.name);
  for (const auto &[Ref, Name] : Plan.void_unit_targets)
    Reserved.insert(Name);
  std::map<MethodRef, std::string> Result;
  size_t Suffix = 0;
  for (const auto &Ref : Candidates) {
    std::string Name;
    do {
      B.tick();
      Name = "__neverdSuper" + std::to_string(Suffix++);
    } while (!Reserved.insert(Name).second);
    // Carry the shared original-call receipt into the derived instance body.
    // Only spelling changes: the helper keeps every incoming register word,
    // while its lexical qualifier is the original owner's direct interface.
    const auto &C = Classes.at(Ref.owner);
    const auto Qualifier = Plan.static_interface_super_qualifiers.find(Ref);
    if (Qualifier != Plan.static_interface_super_qualifiers.end()) {
      const auto M = std::find_if(
          C.methods.begin(), C.methods.end(),
          [&](const Method &Method) { return Method.reference == Ref; });
      const auto *Dispatch =
          M != C.methods.end() && !M->instructions.empty()
              ? std::get_if<MethodRef>(&M->instructions[0].reference)
              : nullptr;
      if (Dispatch) {
        const auto Proof =
            Plan.scoped_interface_super_calls.find({Ref, *Dispatch});
        if (Proof != Plan.scoped_interface_super_calls.end()) {
          MethodRef Helper{Ref.owner, Name,
                           std::vector<std::string>(Ref.parameters.begin() + 1,
                                                    Ref.parameters.end()),
                           Ref.returns};
          auto Qualified = *Dispatch;
          Qualified.owner = Qualifier->second;
          Plan.scoped_interface_super_calls.emplace(
              std::pair{std::move(Helper), std::move(Qualified)},
              Proof->second);
        }
      }
    }
    Result.emplace(Ref, std::move(Name));
  }
  return Result;
}

std::string sourceMethodName(const MethodRef &Ref,
                             const GenericInheritancePlan &Plan) {
  if (const auto I = Plan.source_method_names.find(Ref);
      I != Plan.source_method_names.end())
    return I->second.name;
  if (const auto I = Plan.void_unit_targets.find(Ref);
      I != Plan.void_unit_targets.end())
    return I->second;
  if (Ref.name == "<init>" || Ref.name == "<clinit>")
    return Ref.name;
  return javaIdentifier(Ref.name);
}

bool changesMethodContract(const MethodRef &Ref,
                           const GenericInheritancePlan &Plan) {
  const auto I = Plan.source_method_names.find(Ref);
  return I != Plan.source_method_names.end() && !I->second.identifier_only;
}

std::string hexLiteral(uint64_t Value, unsigned Bits) {
  if (Bits == 32)
    Value &= UINT64_C(0xffffffff);
  std::ostringstream Out;
  Out << "0x" << std::hex << std::setfill('0') << std::setw(Bits / 4) << Value;
  if (Bits == 64)
    Out << 'L';
  return Out.str();
}

// The readers retain lone UTF-16 surrogates as WTF-8. Java source must retain
// those exact code units rather than replacing them through a UTF-8 serializer.
std::string javaString(const std::string &Value) {
  std::string Out = "\"";
  auto Unit = [&](uint32_t U) {
    constexpr char Digits[] = "0123456789abcdef";
    Out += "\\u";
    for (int Shift = 12; Shift >= 0; Shift -= 4)
      Out += Digits[(U >> Shift) & 15];
  };
  for (size_t I = 0; I < Value.size();) {
    uint8_t Byte = static_cast<uint8_t>(Value[I++]);
    uint32_t Code = Byte;
    unsigned Extra = 0;
    uint32_t Minimum = 0;
    if (Byte >= 0xc2 && Byte <= 0xdf) {
      Code = Byte & 31;
      Extra = 1;
      Minimum = 0x80;
    } else if (Byte >= 0xe0 && Byte <= 0xef) {
      Code = Byte & 15;
      Extra = 2;
      Minimum = 0x800;
    } else if (Byte >= 0xf0 && Byte <= 0xf4) {
      Code = Byte & 7;
      Extra = 3;
      Minimum = 0x10000;
    } else if (Byte >= 0x80)
      javaError("invalid UTF-8/WTF-8 Java string");
    if (Extra > Value.size() - I)
      javaError("truncated UTF-8/WTF-8 Java string");
    for (unsigned J = 0; J < Extra; ++J) {
      uint8_t Continuation = static_cast<uint8_t>(Value[I++]);
      if ((Continuation & 0xc0) != 0x80)
        javaError("invalid UTF-8/WTF-8 Java string continuation");
      Code = (Code << 6) | (Continuation & 63);
    }
    if (Code < Minimum || Code > 0x10ffff)
      javaError("invalid UTF-8/WTF-8 Java string value");
    switch (Code) {
    case '"':
      Out += "\\\"";
      break;
    case '\\':
      Out += "\\\\";
      break;
    case '\b':
      Out += "\\b";
      break;
    case '\f':
      Out += "\\f";
      break;
    case '\n':
      Out += "\\n";
      break;
    case '\r':
      Out += "\\r";
      break;
    case '\t':
      Out += "\\t";
      break;
    default:
      if (Code >= 0x20 && Code <= 0x7e)
        Out += static_cast<char>(Code);
      else if (Code <= 0xffff)
        Unit(Code);
      else {
        Code -= 0x10000;
        Unit(0xd800 + (Code >> 10));
        Unit(0xdc00 + (Code & 1023));
      }
    }
  }
  return Out + '"';
}

bool isReference(std::string_view Type) {
  return anyPrefix(Type, {"L", "[", "self:"});
}
// Reference array stores accept an arbitrary reference. The actual array
// performs its component check after the null/index checks, preserving an
// ArrayStoreException instead of introducing a cast to the static element.
// Statements and constructor argument graphs must use the same source views.
std::pair<std::string, std::string> arrayStoreTypes(const std::string &Array) {
  const auto Element = Array.substr(1);
  return isReference(Element)
             ? std::pair<std::string, std::string>{"[Ljava/lang/Object;",
                                                   "Ljava/lang/Object;"}
             : std::pair{Array, Element};
}

bool isWide(std::string_view Type) {
  return Type == "J" || Type == "D" || Type == "bits64";
}
unsigned wordWidth(std::string_view Type) {
  return Type == "J" || Type == "D" ? 2 : 1;
}

class Lines {
  Budget &budget;
  bool cumulative;
  uint64_t bytes = 0;
  std::vector<std::string> values;

public:
  explicit Lines(Budget &B, bool Cumulative = false)
      : budget(B), cumulative(Cumulative) {}
  void append(std::string Value) {
    uint64_t Size = Value.size() + 1;
    if (Size > budget.limits.max_bytes ||
        bytes > budget.limits.max_bytes - Size)
      javaError("Android generated source exceeded its byte budget");
    bytes += Size;
    if (cumulative)
      budget.output(Size);
    values.push_back(std::move(Value));
  }
  void extend(std::initializer_list<std::string> Values) {
    for (const auto &Value : Values)
      append(Value);
  }
  const std::vector<std::string> &get() const { return values; }
  std::string text() const { return join(values, "\n"); }
};

struct JavaScope {
  enum class Position { Declaration, ClassModifiers };
  const Class &owner;
  const MethodRef *method = nullptr;
  Position position = Position::Declaration;
};

std::string qualifiedTypeName(const std::string &Type,
                              const ClassMap &Classes) {
  if (starts(Type, "["))
    return qualifiedTypeName(Type.substr(1), Classes) + "[]";
  if (auto I = Primitives.find(Type); I != Primitives.end())
    return I->second;
  if (!starts(Type, "L") || Type.size() < 3 || Type.back() != ';')
    javaError("invalid Java projection type");
  if (auto I = Classes.find(Type); I != Classes.end() && isAnonymous(I->second))
    javaError("anonymous type has no qualified Java source name: " + Type);
  if (auto I = Classes.find(Type);
      I != Classes.end() && I->second.enclosing_method)
    javaError("local-class reference has no qualified source name: " + Type);
  if (auto I = Classes.find(Type); I != Classes.end() && I->second.enclosing) {
    const auto &C = I->second;
    if (!C.inner_name || C.inner_name->empty() ||
        !Classes.contains(*C.enclosing))
      javaError("nested Android type has no available enclosing declaration");
    return qualifiedTypeName(*C.enclosing, Classes) + "." +
           javaTypeIdentifier(*C.inner_name);
  }
  auto Parts = split(std::string_view(Type).substr(1, Type.size() - 2), '/');
  for (size_t I = 0; I < Parts.size(); ++I)
    Parts[I] = I + 1 == Parts.size() ? javaTypeIdentifier(Parts[I])
                                     : javaIdentifier(Parts[I]);
  return join(Parts, ".");
}

// A fully qualified Java type can still be obscured by a type whose simple
// name matches the first package component. Resolve names from descriptors
// and lexical type scopes before producing source, never by rewriting text.
bool aliasedEnclosure(const Class &C, const ClassMap &Classes,
                      const std::map<std::string, std::string> &Names,
                      Budget &B) {
  auto Parent = C.enclosing;
  Strings Seen;
  while (Parent) {
    B.tick();
    if (!Seen.insert(*Parent).second)
      javaError("recursive nested-class ownership");
    if (Seen.size() > 128)
      javaError("nested-class ownership exceeds the source depth limit");
    if (Names.contains(*Parent))
      return true;
    auto I = Classes.find(*Parent);
    Parent = I == Classes.end() ? std::nullopt : I->second.enclosing;
  }
  return false;
}

class JavaTypeNames {
  const ClassMap &classes;
  const GenericSignaturePlans &generics;
  const std::map<std::string, std::string> &local_names;
  const SharedAnonymousClasses &shared_anonymous;
  const NamedAnonymousMembers &named_anonymous;
  const EnumPlans &enums;
  Budget &budget;
  std::map<std::string, std::map<std::string, const Class *>> members;
  std::map<MethodRef, std::map<std::string, const Class *>> locals;
  std::map<std::string, std::map<std::string, std::string>> packages;
  Strings missing_member_types;
  Strings final_guard_name_conflicts;
  std::map<std::pair<std::string, std::string>, std::string> formal_names;
  std::string formalName(const std::string &Owner,
                         const std::string &Name) const {
    const auto I = formal_names.find({Owner, Name});
    return I == formal_names.end() ? Name : I->second;
  }
  struct Lookup {
    Strings types;
    bool unknown_inheritance = false;
  };
  mutable std::map<std::tuple<std::string, std::string, std::string, bool,
                              JavaScope::Position>,
                   Lookup>
      cache;

  static std::string package(const std::string &Type) {
    auto End = Type.rfind('/');
    return End == Type.npos ? "" : Type.substr(1, End - 1);
  }
  std::string simple(const Class &C) const {
    if (auto I = local_names.find(C.name); I != local_names.end())
      return I->second;
    if ((C.enclosing || C.enclosing_method) && C.inner_name)
      return *C.inner_name;
    auto Start = C.name.rfind('/');
    Start = Start == C.name.npos ? 1 : Start + 1;
    return C.name.substr(Start, C.name.size() - Start - 1);
  }
  Lookup memberTypes(const Class &Scope, const std::string &Name) const {
    Lookup Result;
    std::deque<std::string> Pending{Scope.name};
    Strings Seen;
    while (!Pending.empty()) {
      budget.tick();
      auto Owner = std::move(Pending.front());
      Pending.pop_front();
      if (!Seen.insert(Owner).second)
        continue;
      auto C = classes.find(Owner);
      if (C == classes.end()) {
        // Object contributes no member types; other missing declarations
        // cannot prove that a source name has no inherited namesake.
        Result.unknown_inheritance |=
            Owner != "Ljava/lang/Object;" &&
            Owner != "Ljava/lang/annotation/Annotation;" &&
            // Enum has no member types in Java 8; Java 12+ adds EnumDesc.
            // That spelling still needs explicit dependency declarations.
            (Owner != "Ljava/lang/Enum;" ||
             (!enums.classes.contains(Scope.name) &&
              !enums.constant_classes.contains(Scope.name)) ||
             Name == "EnumDesc");
        continue;
      }
      auto M = members.find(Owner);
      if (M != members.end()) {
        auto Match = M->second.find(Name);
        if (Match != M->second.end()) {
          const auto &Child = *Match->second;
          const auto &Flags = Child.inner_access;
          bool Inherited = Owner != Scope.name;
          bool Accessible =
              !Inherited ||
              (!Flags.contains("private") &&
               (Flags.contains("public") || Flags.contains("protected") ||
                package(Owner) == package(Scope.name)));
          if (Accessible)
            Result.types.insert(Child.name);
          // A declaration in this class hides any inherited namesake.
          continue;
        }
      }
      if (auto Declared = C->second.declared_member_types.find(Name);
          Declared != C->second.declared_member_types.end() &&
          !classes.contains(Declared->second)) {
        // An incomplete dependency JAR cannot prove accessibility or the
        // inherited type binding by omitting a nested classfile.
        Result.unknown_inheritance = true;
        continue;
      }
      if (C->second.superclass)
        Pending.push_back(*C->second.superclass);
      Pending.insert(Pending.end(), C->second.interfaces.begin(),
                     C->second.interfaces.end());
    }
    return Result;
  }
  const Lookup &lookup(const JavaScope &Context, const std::string &Name,
                       bool InSupertypeHeader = false) const {
    auto Key =
        std::tuple{Context.owner.name,
                   Context.method ? Context.method->identity() : std::string(),
                   Name, InSupertypeHeader, Context.position};
    if (auto I = cache.find(Key); I != cache.end())
      return I->second;
    Lookup Result;
    auto Local = [&](const MethodRef *Method) {
      if (!Method)
        return false;
      if (auto I = locals.find(*Method); I != locals.end())
        if (auto L = I->second.find(Name); L != I->second.end()) {
          Result.types.insert(L->second->name);
          return true;
        }
      return false;
    };
    if (Local(Context.method))
      return cache.emplace(std::move(Key), std::move(Result)).first->second;
    auto Variable = [&](const auto &Plans, const auto &Owner) {
      auto I = Plans.find(Owner);
      if (I == Plans.end())
        return false;
      for (const auto &P : I->second.type_parameters) {
        budget.tick();
        const auto Spelling = [&] {
          if constexpr (std::is_same_v<std::decay_t<decltype(Owner)>,
                                       MethodRef>)
            return P.name;
          else
            return formalName(Owner, P.name);
        }();
        if (Spelling == Name) {
          std::string Identity;
          if constexpr (std::is_same_v<std::decay_t<decltype(Owner)>,
                                       MethodRef>)
            Identity = Owner.identity();
          else
            Identity = Owner;
          Result.types.insert("type-variable:" + Identity + ":" + P.name);
          return true;
        }
      }
      return false;
    };
    if (Context.method && Variable(generics.methods, *Context.method))
      return cache.emplace(std::move(Key), std::move(Result)).first->second;
    const Class *Scope = &Context.owner;
    while (Scope) {
      budget.tick();
      bool ClassModifiers =
          Scope == &Context.owner &&
          Context.position == JavaScope::Position::ClassModifiers;
      if (!ClassModifiers && Variable(generics.classes, Scope->name)) {
        // A declared member shadows a class formal in its body. A formal
        // actually used there has a separate source spelling; an unused
        // namesake may retain its original metadata name in the header.
        if ((!InSupertypeHeader || Scope != &Context.owner))
          if (auto I = members.find(Scope->name);
              I != members.end() && I->second.contains(Name))
            Result.types = Strings{I->second.at(Name)->name};
        return cache.emplace(std::move(Key), std::move(Result)).first->second;
      }
      // Declared and inherited members are in scope within the class body,
      // not within its own modifiers or extends/implements clause. Class
      // type parameters also exclude the modifiers. Enclosing declarations
      // still contribute their complete lexical scopes to a nested header.
      auto Members =
          (InSupertypeHeader || ClassModifiers) && Scope == &Context.owner
              ? Lookup{}
              : memberTypes(*Scope, Name);
      Result.unknown_inheritance |= Members.unknown_inheritance;
      if (simple(*Scope) == Name)
        Members.types.insert(Scope->name);
      if (!Members.types.empty()) {
        Result.types = std::move(Members.types);
        return cache.emplace(std::move(Key), std::move(Result)).first->second;
      }
      if (shared_anonymous.contains(Scope->name))
        Scope = &classes.at(shared_anonymous.at(Scope->name).source_owner);
      else if (Scope->enclosing)
        Scope = &classes.at(*Scope->enclosing);
      else if (Scope->enclosing_method) {
        if (Local(&*Scope->enclosing_method))
          return cache.emplace(std::move(Key), std::move(Result)).first->second;
        if (Variable(generics.methods, *Scope->enclosing_method))
          return cache.emplace(std::move(Key), std::move(Result)).first->second;
        Scope = &classes.at(Scope->enclosing_method->owner);
      } else
        Scope = nullptr;
    }
    if (auto P = packages.find(package(Context.owner.name));
        P != packages.end())
      if (auto I = P->second.find(Name); I != P->second.end())
        Result.types.insert(I->second);
    return cache.emplace(std::move(Key), std::move(Result)).first->second;
  }

public:
  JavaTypeNames(const ClassMap &Classes, const GenericSignaturePlans &Generics,
                const std::map<std::string, std::string> &LocalNames,
                const SharedAnonymousClasses &Shared,
                const NamedAnonymousMembers &Named, const EnumPlans &Enums,
                Budget &B)
      : classes(Classes), generics(Generics), local_names(LocalNames),
        shared_anonymous(Shared), named_anonymous(Named), enums(Enums),
        budget(B) {
    for (const auto &[Name, C] : classes) {
      budget.tick();
      auto reserveGuard = [&](const std::string &Value) {
        if (starts(Value, "__neverdFinalGuard"))
          final_guard_name_conflicts.insert(Value);
      };
      reserveGuard(Name.substr(1, Name.find_first_of("/;", 1) - 1));
      reserveGuard(simple(C));
      for (const auto &F : C.fields) {
        budget.tick();
        reserveGuard(F.reference.name);
      }
      for (const auto &[Simple, Type] : C.declared_member_types)
        if (!classes.contains(Type))
          missing_member_types.insert(Type);
      if (enums.constant_classes.contains(Name))
        continue;
      if (isAnonymous(C) && !isAnonymousLocal(C) && !namedAnonymous(C))
        continue;
      if (C.external &&
          (C.enclosing_method || (C.inner_class_present && !C.inner_name)))
        continue;
      if (shared_anonymous.contains(Name)) {
        if (!members[shared_anonymous.at(Name).source_owner]
                 .emplace(simple(C), &C)
                 .second)
          javaError(
              "ambiguous Java type: duplicate shared anonymous source name");
      } else if (C.enclosing_method) {
        if (!locals[*C.enclosing_method].emplace(simple(C), &C).second)
          javaError("ambiguous Java type: duplicate local source name");
      } else if (C.enclosing) {
        if (!members[*C.enclosing].emplace(simple(C), &C).second)
          javaError("ambiguous Java type: duplicate nested source name");
      } else
        packages[package(Name)].emplace(simple(C), Name);
    }
    std::set<std::pair<std::string, std::string>> BodyVariables;
    Strings Reserved;
    for (const auto &[Name, C] : classes) {
      Reserved.insert(simple(C));
      Reserved.insert(Name.substr(1, Name.find_first_of("/;", 1) - 1));
      for (const auto &[MemberName, Type] : C.declared_member_types)
        Reserved.insert(MemberName);
    }
    auto collect = [&](const GenericSignature &Signature,
                       const std::string &HeaderOwner = std::string()) {
      for (const auto &P : Signature.type_parameters)
        Reserved.insert(P.name);
      for (const auto &Type : Signature.types) {
        budget.tick();
        if (Type.kind == GenericType::Kind::TypeVariable &&
            Type.variable_owner && *Type.variable_owner != HeaderOwner)
          BodyVariables.emplace(*Type.variable_owner, Type.variable_name);
      }
    };
    for (const auto &[Name, Signature] : generics.classes)
      collect(Signature, Name);
    for (const auto &[Ref, Signature] : generics.fields)
      collect(Signature);
    for (const auto &[Ref, Signature] : generics.methods) {
      for (const auto &P : Signature.type_parameters)
        Reserved.insert(P.name);
      const auto *Source = sourceMethodSignature(Ref, generics);
      collect(Source ? *Source : Signature);
    }
    for (const auto &[Owner, Signature] : generics.classes) {
      if (classes.at(Owner).external)
        continue;
      const auto Members = members.find(Owner);
      if (Members == members.end())
        continue;
      for (size_t I = 0; I < Signature.type_parameters.size(); ++I) {
        budget.tick();
        const auto &Name = Signature.type_parameters[I].name;
        if (!Members->second.contains(Name) ||
            !BodyVariables.contains({Owner, Name}))
          continue;
        auto Fresh = "__neverdType" + std::to_string(I);
        while (Reserved.contains(Fresh)) {
          budget.tick();
          Fresh += '_';
        }
        Reserved.insert(Fresh);
        formal_names.emplace(std::pair{Owner, Name}, std::move(Fresh));
      }
    }
  }
  std::vector<GenericSourceProjection>
  formalNameProjections(const GenericSignature &Signature,
                        const std::string &Identity) const {
    std::vector<GenericSourceProjection> Result;
    if (Signature.kind == GenericSignatureKind::Class)
      for (const auto &P : Signature.type_parameters)
        if (auto I = formal_names.find({Identity, P.name});
            I != formal_names.end())
          Result.push_back({0, "type-variable-declaration-source-name", P.name,
                            I->second, Identity});
    for (const auto &Type : Signature.types) {
      budget.tick();
      if (Type.kind != GenericType::Kind::TypeVariable || !Type.variable_owner)
        continue;
      if (auto I =
              formal_names.find({*Type.variable_owner, Type.variable_name});
          I != formal_names.end())
        Result.push_back({Type.offset, "type-variable-reference-source-name",
                          "T" + Type.variable_name + ";", "T" + I->second + ";",
                          Type.variable_owner});
    }
    return Result;
  }
  const std::string *enumConstantBase(const std::string &Type) const {
    const auto I = enums.constant_classes.find(Type);
    return I == enums.constant_classes.end() ? nullptr : &I->second;
  }
  const std::string *enumConstantReader(const FieldRef &Ref) const {
    const auto I = enums.encoded_constants.find(Ref);
    return I == enums.encoded_constants.end() ? nullptr : &I->second;
  }
  std::string finalGuardName(uint32_t PC, std::string_view Kind = "") const {
    std::string Name =
        "__neverdFinalGuard" + std::string(Kind) + std::to_string(PC);
    while (final_guard_name_conflicts.contains(Name)) {
      budget.tick();
      Name += '_';
    }
    return Name;
  }
  bool namedAnonymous(const Class &C) const {
    return isNamedAnonymousMember(C, named_anonymous);
  }
  std::string memberBinaryName(const Class &C) const {
    budget.tick();
    if (C.enclosing_method || shared_anonymous.contains(C.name))
      javaError("field metadata restoration requires a named member owner: " +
                C.name);
    const auto Name = javaTypeIdentifier(simple(C));
    if (C.enclosing)
      return memberBinaryName(classes.at(*C.enclosing)) + "$" + Name;
    if (package(C.name).empty())
      return Name;
    auto Parts = split(package(C.name), '/');
    for (auto &Part : Parts)
      Part = javaIdentifier(Part);
    return (Parts.empty() ? "" : join(Parts, "/") + "/") + Name;
  }
  std::string render(const std::string &Type, const JavaScope &Context,
                     bool InSupertypeHeader = false) const {
    budget.tick();
    if (starts(Type, "["))
      return render(Type.substr(1), Context, InSupertypeHeader) + "[]";
    if (enums.constant_classes.contains(Type))
      javaError("enum constant subclass has no source type name: " + Type);
    if (missing_member_types.contains(Type))
      javaError("dependency member type has no available classfile: " + Type);
    const auto nestedName = [&](const std::string &parent,
                                const std::string &name) {
      const auto enclosing = classes.find(parent);
      if (enclosing != classes.end() && enclosing->second.enclosing_method &&
          (Context.owner.name == parent || Context.owner.name == Type)) {
        const auto &method = *enclosing->second.enclosing_method;
        const auto outer = generics.classes.find(method.owner);
        const auto scope = generics.methods.find(method);
        if ((outer != generics.classes.end() &&
             !outer->second.type_parameters.empty()) ||
            (scope != generics.methods.end() &&
             !scope->second.type_parameters.empty())) {
          // The local qualifier implicitly captures its enclosing formals.
          // A static member can use its simple name in its lexical body.
          const auto &binding = lookup(Context, name, InSupertypeHeader);
          if (!binding.unknown_inheritance && binding.types == Strings{Type})
            return javaTypeIdentifier(name);
        }
      }
      return render(parent, Context, InSupertypeHeader) + "." +
             javaTypeIdentifier(name);
    };
    if (shared_anonymous.contains(Type))
      return nestedName(shared_anonymous.at(Type).reference_owner,
                        local_names.at(Type));
    if (auto I = classes.find(Type);
        I != classes.end() && I->second.enclosing &&
        (I->second.inner_name || namedAnonymous(I->second)) &&
        (namedAnonymous(I->second) ||
         aliasedEnclosure(I->second, classes, local_names, budget)))
      return nestedName(*I->second.enclosing, simple(I->second));
    if (auto I = classes.find(Type);
        I != classes.end() && I->second.enclosing_method) {
      const auto &C = I->second;
      if (!localClassVisible(C, Context.owner, Context.method, classes,
                             budget) ||
          !local_names.contains(Type))
        javaError("local-class reference is outside its exact method scope: " +
                  Type + " in " + Context.owner.name);
      const auto &Name = local_names.at(Type);
      const auto &Binding = lookup(Context, Name, InSupertypeHeader);
      if (Binding.unknown_inheritance || Binding.types != Strings{Type})
        javaError("local-class reference has no unique source binding: " +
                  Type);
      return javaTypeIdentifier(Name);
    }
    auto Full = qualifiedTypeName(Type, classes);
    if (Primitives.contains(Type))
      return Full;
    auto Top = Type;
    while (classes.contains(Top) && classes.at(Top).enclosing) {
      budget.tick();
      Top = *classes.at(Top).enclosing;
    }
    auto Package = package(Top);
    auto Root = Full.substr(0, Full.find('.'));
    const auto &QualifiedBinding = lookup(Context, Root, InSupertypeHeader);
    if (!QualifiedBinding.unknown_inheritance &&
        (QualifiedBinding.types.empty() ||
         (Package.empty() && QualifiedBinding.types == Strings{Top})))
      return Full;
    if (Package == package(Context.owner.name) && classes.contains(Top)) {
      const auto &Binding =
          lookup(Context, simple(classes.at(Top)), InSupertypeHeader);
      if (!Binding.unknown_inheritance && Binding.types == Strings{Top})
        return Package.empty() ? Full : Full.substr(Package.size() + 1);
    }
    if (QualifiedBinding.unknown_inheritance)
      javaError("unresolved inherited Java type: " + Type + " in " +
                Context.owner.name +
                " (external declarations are required to prove source "
                "name binding)");
    javaError("ambiguous Java type: " + Type + " in " + Context.owner.name +
              " (package qualifier is shadowed and no unique source name "
              "is proven)");
  }
  std::string render(const std::string &Type, const Class &Context,
                     bool InSupertypeHeader = false) const {
    return render(Type, JavaScope{Context}, InSupertypeHeader);
  }
  // A private member from another package may occur inside DEX Signature
  // metadata even though Java cannot name it. Only the enclosing member type
  // is erased in source; all runtime descriptors and original metadata survive.
  std::optional<std::string> hiddenArgument(const GenericSignature &signature,
                                            const GenericType &node,
                                            const Class &scope) const {
    if (signature.kind == GenericSignatureKind::Class ||
        node.kind != GenericType::Kind::Class)
      return {};
    std::vector<GenericTypeId> pending;
    auto append = [&](const GenericType &type) {
      for (const auto &segment : type.segments)
        for (const auto &arg : segment.arguments)
          if (arg.type)
            pending.push_back(*arg.type);
      if (type.element)
        pending.push_back(*type.element);
    };
    append(node);
    while (!pending.empty()) {
      budget.tick();
      const auto &arg = signature.types.at(pending.back());
      pending.pop_back();
      if (arg.kind == GenericType::Kind::Class && package(scope.name) != package(arg.erasure)) {
        auto decl = classes.find(arg.erasure);
        while (decl != classes.end() && decl->second.enclosing) {
          budget.tick();
          if (has(decl->second.inner_access, "private"))
            return decl->first;
          decl = classes.find(*decl->second.enclosing);
        }
      }
      append(arg);
    }
    return {};
  }
  void recordAccessProjections(const Class &scope, const GenericSignature &signature,
                               const std::string &original,
                               std::vector<GenericSourceProjection> &changes) const {
    auto memberType = [&](GenericTypeId id) {
      const auto *type = &signature.types.at(id);
      while (type->element)
        type = &signature.types.at(*type->element);
      if (const auto hidden = hiddenArgument(signature, *type, scope))
        changes.push_back({type->offset, "inaccessible-member-type-argument", original,
                           type->erasure, *hidden});
    };
    if (signature.field_type)
      memberType(*signature.field_type);
    for (auto id : signature.parameters)
      memberType(id);
    if (signature.result)
      memberType(*signature.result);
  }
  std::optional<std::string> visibleFormal(const GenericType &type,
                                           const JavaScope &scope,
                                           bool inHeader = false) const {
    if (type.kind != GenericType::Kind::TypeVariable || !type.variable_owner)
      return {};
    const auto name = formalName(*type.variable_owner, type.variable_name);
    if (lookup(scope, name, inHeader).types !=
        Strings{"type-variable:" + *type.variable_owner + ":" + type.variable_name})
      return {};
    return javaTypeIdentifier(name);
  }
  std::string render(const GenericSignature &Signature, GenericTypeId Id,
                     const JavaScope &Context,
                     bool InSupertypeHeader = false) const {
    budget.tick();
    const auto &Type = Signature.types.at(Id);
    if (Type.kind == GenericType::Kind::TypeVariable) {
      const auto name = visibleFormal(Type, Context, InSupertypeHeader);
      if (!name)
        javaError("generic type variable has no unique source binding: " + Type.variable_name);
      return *name;
    }
    if (Type.kind == GenericType::Kind::Array)
      return render(Signature, *Type.element, Context, InSupertypeHeader) +
             "[]";
    if (Type.kind != GenericType::Kind::Class)
      return render(Type.erasure, Context, InSupertypeHeader);
    if (hiddenArgument(Signature, Type, Context.owner))
      return render(Type.erasure, Context, InSupertypeHeader);
    // A parameterized enclosing instance changes the member's Java view.
    // Keep that boundary explicit until its owner substitution is proved.
    for (size_t I = 0; I + 1 < Type.segments.size(); ++I)
      if (!Type.segments[I].arguments.empty())
        javaError("parameterized enclosing type requires an owner "
                  "substitution proof");
    if (Type.segments.size() > 1) {
      std::string Owner = "L" + Type.package;
      if (!Type.package.empty())
        Owner += '/';
      Owner += Type.segments.front().name + ';';
      for (size_t I = 1; I < Type.segments.size(); ++I) {
        budget.tick();
        auto Child = Owner.substr(0, Owner.size() - 1) + "$" +
                     Type.segments[I].name + ";";
        auto C = classes.find(Child);
        if (C == classes.end() || C->second.enclosing != Owner ||
            C->second.inner_name != Type.segments[I].name ||
            (!C->second.inner_access.contains("static") &&
             !generics.captured_inner_classes.contains(Child)))
          javaError("generic inner suffix has no proven static source owner");
        Owner = std::move(Child);
      }
    }
    std::string Result = render(Type.erasure, Context, InSupertypeHeader);
    std::vector<std::string> Arguments;
    for (const auto &Argument : Type.segments.back().arguments) {
      budget.tick();
      using Variance = GenericTypeArgument::Variance;
      if (Argument.variance == Variance::Any) {
        Arguments.push_back("?");
        continue;
      }
      auto Value =
          render(Signature, *Argument.type, Context, InSupertypeHeader);
      if (Argument.variance == Variance::Extends)
        Value = "? extends " + Value;
      else if (Argument.variance == Variance::Super)
        Value = "? super " + Value;
      Arguments.push_back(std::move(Value));
    }
    if (!Arguments.empty())
      Result += "<" + join(Arguments, ", ") + ">";
    return Result;
  }
  std::string formals(const GenericSignature &Signature,
                      const JavaScope &Context,
                      bool InSupertypeHeader = false) const {
    std::vector<std::string> Parameters;
    for (const auto &Parameter : Signature.type_parameters) {
      budget.tick();
      std::string Name = javaTypeIdentifier(
          Context.method ? Parameter.name
                         : formalName(Context.owner.name, Parameter.name));
      std::vector<std::string> Bounds;
      if (Parameter.class_bound)
        Bounds.push_back(render(Signature, *Parameter.class_bound, Context,
                                InSupertypeHeader));
      for (auto Id : Parameter.interface_bounds)
        Bounds.push_back(render(Signature, Id, Context, InSupertypeHeader));
      if (!Bounds.empty())
        Name += " extends " + join(Bounds, " & ");
      Parameters.push_back(std::move(Name));
    }
    return Parameters.empty() ? "" : "<" + join(Parameters, ", ") + ">";
  }
  void requireRuntimePackage(const JavaScope &Context) const {
    const auto &Binding = lookup(Context, "java");
    if (Binding.unknown_inheritance)
      javaError("unresolved inherited Java type: java in " +
                Context.owner.name +
                " (external declarations are required to prove runtime "
                "helper name binding)");
    if (!Binding.types.empty())
      javaError("ambiguous Java type: required Java runtime package is "
                "shadowed in " +
                Context.owner.name);
  }
  void requireRuntimePackage(const Class &Context) const {
    requireRuntimePackage(JavaScope{Context});
  }
  void requireAnnotationEnumQualifier(const Class &Context) const {
    // An enum constant is an expression name. A field named java can shadow
    // its package qualifier even when type-name lookup is unambiguous.
    Strings Visited;
    std::vector<std::string> Pending;
    const Class *Scope = &Context;
    while (Scope) {
      budget.tick();
      Pending.push_back(Scope->name);
      Scope = Scope->enclosing ? &classes.at(*Scope->enclosing) : nullptr;
    }
    while (!Pending.empty()) {
      budget.tick();
      auto Name = std::move(Pending.back());
      Pending.pop_back();
      if (!Visited.insert(Name).second)
        continue;
      auto I = classes.find(Name);
      if (I == classes.end()) {
        if (Name != "Ljava/lang/Object;" &&
            Name != "Ljava/lang/annotation/Annotation;")
          javaError("unresolved inherited annotation enum value scope: " +
                    Name);
        continue;
      }
      for (const auto &Field : I->second.fields) {
        budget.tick();
        if (Field.reference.name == "java")
          javaError("annotation enum package qualifier is shadowed by a "
                    "field in " +
                    Name);
      }
      if (I->second.superclass)
        Pending.push_back(*I->second.superclass);
      for (const auto &Interface : I->second.interfaces)
        Pending.push_back(Interface);
    }
  }
};

const Field *member(const FieldRef &Reference, const ClassMap &Classes,
                    Budget &B) {
  const auto Lookup = lookupField(Reference, Classes, B);
  if (Lookup.status == FieldLookupStatus::UnknownOwner)
    return nullptr;
  if (Lookup.status == FieldLookupStatus::Found)
    return Lookup.declaration;
  if (Lookup.status == FieldLookupStatus::TypeMismatch)
    javaError(Reference.owner +
              ": member type differs from the exact referenced declaration");
  javaError(Reference.owner +
            ": referenced member has no proven local declaration");
}

const Method *member(const MethodRef &ref, const ClassMap &classes,
                     Budget &budget,
                     const GenericSignaturePlans *signatures = nullptr) {
  return lookupMethod(ref, classes, budget, signatures);
}

bool planPrivateFieldIdentifiers(const ClassMap &Classes,
                                 GenericInheritancePlan &Plan, Budget &B) {
  Strings Reserved, CandidateNames;
  std::map<FieldRef, bool> Candidates;
  for (const auto &[Name, C] : Classes) {
    for (const auto &F : C.fields) {
      B.tick();
      const auto &Ref = F.reference;
      Reserved.insert(Ref.name);
      if (!C.external && independentSourceScope(C) &&
          !has(C.access, "interface") && has(F.access, "private") &&
          !has(F.access, "synthetic") && !isJavaIdentifier(Ref.name) &&
          !Plan.source_field_names.contains(Ref)) {
        Candidates.emplace(Ref, has(F.access, "static"));
        CandidateNames.insert(Ref.name);
      }
    }
    for (const auto &M : C.methods)
      Reserved.insert(M.reference.name);
  }
  for (const auto &[Ref, Name] : Plan.source_field_names)
    Reserved.insert(Name);
  for (const auto &[Ref, Alias] : Plan.source_method_names)
    Reserved.insert(Alias.name);
  // A private slot is not inherited. Reuse actual member resolution, then
  // require every encoded access to name and execute in its declaring class.
  // This spelling proof grants no new field access or initialization behavior.
  for (const auto &[Name, C] : Classes)
    if (!C.external)
      for (const auto &M : C.methods)
        for (const auto &Op : M.instructions) {
          B.tick();
          const auto *Ref = std::get_if<FieldRef>(&Op.reference);
          if (!Ref || !CandidateNames.contains(Ref->name))
            continue;
          const auto *Field = member(*Ref, Classes, B);
          if (Field && Candidates.contains(Field->reference) &&
              (*Ref != Field->reference || Name != Field->reference.owner))
            Candidates.erase(Field->reference);
        }
  size_t StaticSuffix = 0, InstanceSuffix = 0;
  for (const auto &[Ref, Static] : Candidates) {
    auto &Suffix = Static ? StaticSuffix : InstanceSuffix;
    std::string Name;
    do {
      B.tick();
      Name = (Static ? "__neverdStaticField" : "__neverdInstanceField") +
             std::to_string(Suffix++);
    } while (!Reserved.insert(Name).second);
    Plan.source_field_names.emplace(Ref, std::move(Name));
  }
  return !Candidates.empty();
}

std::string sourceFieldName(const FieldRef &Ref, const ClassMap &Classes,
                            const GenericInheritancePlan &Plan, Budget &B) {
  if (isJavaIdentifier(Ref.name))
    return javaIdentifier(Ref.name);
  // Resolve the complete runtime field descriptor, including inherited owners.
  const auto *Declaration = member(Ref, Classes, B);
  if (Declaration)
    if (const auto I = Plan.source_field_names.find(Declaration->reference);
        I != Plan.source_field_names.end())
      return I->second;
  return javaIdentifier(Ref.name);
}

std::string helperName(const Class &C, std::string Base) {
  Strings Names;
  for (const auto &M : C.methods)
    Names.insert(M.reference.name);
  while (Names.contains(Base))
    Base += '_';
  return Base;
}
bool constructorPrefixThrowsSupported(const Method &callee,
                                      const Method &constructor,
                                      const GenericSignaturePlans &generics) {
  // Before delegation the call propagates through the constructor's own
  // Exceptions declaration. A projected constructor may add source throws.
  const auto plan = generics.constructor_exception_contracts.find(callee.reference);
  if (plan != generics.constructor_exception_contracts.end() &&
      plan->second.source_declared_throws !=
          callee.declared_throws.value_or(std::vector<std::string>{}))
    return false;
  return !callee.declared_throws || callee.declared_throws->empty() ||
         validatedUncheckedDeclaredThrows(callee, generics) ||
         compatibleClosedInheritedThrows(callee, constructor, generics);
}

bool constructorThrows(const Method &Input, const ClassMap &Classes,
                       const GenericSignaturePlans &Generics, Budget &B,
                       bool PreserveDeclaration = false) {
  const Method *M = &Input;
  std::set<MethodRef> Seen;
  for (;;) {
    B.tick();
    if (!Seen.insert(M->reference).second)
      javaError("recursive constructor delegation");
    if (const auto I =
            Generics.constructor_exception_contracts.find(M->reference);
        I != Generics.constructor_exception_contracts.end()) {
      // A this(...) chain may stop at a proven empty source contract. A
      // projected checked declaration still needs coverage at this caller.
      if (I->second.source_declared_throws.empty())
        return false;
      javaError("constructor exception contract requires a proven "
                "delegation substitution");
    }
    // Argument allocations are not this/super delegation. Use the same
    // original prefix selection as the constructor body and generic planner.
    const auto Prefix = selectConstructorPrefixDelegation(*M, B);
    if (!Prefix)
      return false;
    const auto *R =
        std::get_if<MethodRef>(&M->instructions[Prefix->call].reference);
    if (!R || R->name != "<init>")
      return false;
    const auto *Target = member(*R, Classes, B);
    // Constructors are never inherited from a different declaration owner.
    if (Target && Target->reference != *R)
      Target = nullptr;
    if (!Target) {
      if (PreserveDeclaration &&
          *R != MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"})
        javaError("constructor exception contract requires a proven "
                  "delegation declaration");
      return *R != MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"};
    }
    // Known exception declarations always require the shared contract, even
    // when all Signature/Throws metadata comes from SDK declarations.
    if (Target->declared_throws && !Target->declared_throws->empty())
      javaError("constructor exception contract requires a proven "
                "delegation substitution");
    if (Classes.at(Target->reference.owner).external)
      return Target->declared_throws && !Target->declared_throws->empty();
    M = Target;
  }
}

bool sameHandlers(const std::vector<Handler> &A,
                  const std::vector<Handler> &B) {
  if (A.size() != B.size())
    return false;
  for (size_t I = 0; I < A.size(); ++I)
    if (A[I].type != B[I].type || A[I].target != B[I].target)
      return false;
  return true;
}

// The shared model validates source shape and reference scope. This index adds
// only lexical Java naming and emission/accounting facts.
struct LocalProjection {
  std::map<MethodRef, std::vector<const Class *>> by_method;
  std::map<std::string, const Class *> by_class;
  std::set<MethodRef> projected_methods;
  std::set<MethodRef> scalar_closures;
  std::set<MethodRef> anonymous_methods;
  std::set<MethodRef> shared_methods;
  std::set<MethodRef> member_methods;
  std::set<std::string> member_classes, ordinary_static_only_members;
  std::set<MethodRef> anonymous_member_methods;
  std::set<std::string> anonymous_member_classes;

  [[noreturn]] static void fail(const Class &C, const std::string &Reason) {
    javaError(
        C.name + " [" + C.source_id + "] enclosing " +
        (C.enclosing_method ? C.enclosing_method->identity() : "unknown") +
        ": local-class source shape: " + Reason);
  }
  static bool scalar(const std::string &Type, bool Void = false) {
    return Primitives.contains(Type) && (Void || Type != "V");
  }
  LocalProjection(const ClassMap &Classes,
                  const std::map<std::string, std::string> &Names,
                  const SharedAnonymousClasses &Shared,
                  const NamedAnonymousMembers &Named, Budget &B) {
    for (const auto &[Name, C] : Classes) {
      B.tick();
      if (C.enclosing_method && !C.external)
        by_class.emplace(Name, &C);
      if (isNamedAnonymousMember(C, Named)) {
        anonymous_member_classes.insert(Name);
        for (const auto &M : C.methods) {
          if (!M.instructions.empty()) {
            projected_methods.insert(M.reference);
            anonymous_member_methods.insert(M.reference);
          }
        }
      }
      if (!C.external && C.enclosing && C.inner_name &&
          aliasedEnclosure(C, Classes, Names, B)) {
        member_classes.insert(Name);
        for (const auto &M : C.methods) {
          if (!M.instructions.empty()) {
            projected_methods.insert(M.reference);
            member_methods.insert(M.reference);
          }
        }
      } else if (!C.external && isStaticOnlyInnerMember(C) &&
                 C.name == C.enclosing->substr(0, C.enclosing->size() - 1) +
                               "$" + *C.inner_name + ";") {
        ordinary_static_only_members.insert(Name);
      }
    }
    auto AnonymousMemberType = [&](const std::string &Type) {
      auto Start = Type.find_first_not_of('[');
      return Start != Type.npos &&
             anonymous_member_classes.contains(Type.substr(Start));
    };
    for (const auto &[Name, C] : Classes) {
      B.tick();
      if (C.external || anonymous_member_classes.empty())
        continue;
      for (const auto &M : C.methods) {
        if (M.instructions.empty())
          continue;
        bool Mentions = AnonymousMemberType(M.reference.returns);
        for (const auto &Type : M.reference.parameters)
          Mentions |= AnonymousMemberType(Type);
        for (const auto &Op : M.instructions) {
          B.tick();
          if (const auto *Type = std::get_if<std::string>(&Op.reference))
            Mentions |= AnonymousMemberType(*Type);
          else if (const auto *Field = std::get_if<FieldRef>(&Op.reference))
            Mentions |= AnonymousMemberType(Field->owner) ||
                        AnonymousMemberType(Field->type);
          else if (const auto *Ref = std::get_if<MethodRef>(&Op.reference)) {
            Mentions |= AnonymousMemberType(Ref->owner) ||
                        AnonymousMemberType(Ref->returns);
            for (const auto &Type : Ref->parameters)
              Mentions |= AnonymousMemberType(Type);
          }
        }
        if (Mentions) {
          projected_methods.insert(M.reference);
          anonymous_member_methods.insert(M.reference);
        }
      }
    }
    if (by_class.empty())
      return;
    for (const auto &[Name, CP] : by_class) {
      B.tick();
      const auto &C = *CP;
      try {
        javaTypeIdentifier(Names.at(Name));
      } catch (const std::runtime_error &Error) {
        fail(C, Error.what());
      }
      if (Names.at(Name) == "java")
        fail(C, "local name shadows the required Java runtime package");
      auto Owner = Classes.find(C.enclosing_method->owner);
      // A local class cannot have the same source name as an enclosing class.
      const Class *Ancestor = &Owner->second;
      Strings Seen;
      while (Ancestor) {
        B.tick();
        if (!Seen.insert(Ancestor->name).second || Seen.size() > 128)
          fail(C, "invalid enclosing class chain");
        auto Start = Ancestor->name.rfind('/');
        Start = Start == std::string::npos ? 1 : Start + 1;
        auto Simple = Names.contains(Ancestor->name)
                          ? Names.at(Ancestor->name)
                          : Ancestor->inner_name.value_or(Ancestor->name.substr(
                                Start, Ancestor->name.size() - Start - 1));
        if (Names.at(Name) == Simple)
          fail(C, "local source name repeats an enclosing class name");
        auto Parent =
            Ancestor->enclosing_method
                ? std::optional<std::string>(Ancestor->enclosing_method->owner)
                : Ancestor->enclosing;
        if (!Parent)
          break;
        auto Next = Classes.find(*Parent);
        if (Next == Classes.end())
          fail(C, "unresolved enclosing class");
        Ancestor = &Next->second;
      }
      for (const auto &Body : C.methods) {
        B.tick();
        projected_methods.insert(Body.reference);
        if (isAnonymousLocal(C))
          anonymous_methods.insert(Body.reference);
        if (Shared.contains(Name))
          shared_methods.insert(Body.reference);
      }
      if (auto I = Shared.find(Name); I != Shared.end()) {
        shared_methods.insert(*C.enclosing_method);
        shared_methods.insert(I->second.methods.begin(),
                              I->second.methods.end());
        projected_methods.insert(I->second.methods.begin(),
                                 I->second.methods.end());
      } else
        by_method[*C.enclosing_method].push_back(&C);
      projected_methods.insert(*C.enclosing_method);
      if (isAnonymousLocal(C))
        anonymous_methods.insert(*C.enclosing_method);
      else
        scalar_closures.insert(*C.enclosing_method);
    }
  }
};

std::string floatingArithmeticExpression(const FloatingArithmetic &Operation,
                                         const std::string &Left,
                                         const std::string &Right) {
  return "((" + Left + ") " + Operation.symbol + " (" + Right + "))";
}

std::string scalarConversionExpression(const std::string &Destination,
                                       const std::string &Value) {
  return "((" + Primitives.at(Destination) + ") (" + Value + "))";
}

std::optional<std::string> allocationDefaultLiteral(const std::string &Type) {
  if (isReference(Type))
    return "null";
  if (Type == "Z")
    return "false";
  if (Type == "B")
    return "(byte) 0";
  if (Type == "C")
    return "(char) 0";
  if (Type == "S")
    return "(short) 0";
  if (Type == "I")
    return "0";
  if (Type == "J")
    return "0L";
  if (Type == "F")
    return "0.0f";
  if (Type == "D")
    return "0.0d";
  return std::nullopt;
}

struct FieldWritePlan {
  std::set<FieldRef> fields;
  std::set<std::pair<std::string, std::string>> unresolved;
};

FieldWritePlan planFieldWrites(const ClassMap &Classes, Budget &B,
                               bool Static) {
  FieldWritePlan Plan;
  std::set<std::pair<std::string, std::string>> Candidates;
  for (const auto &[Name, C] : Classes) {
    B.tick();
    if (C.external)
      continue;
    for (const auto &F : C.fields)
      if (has(F.access, "static") == Static && has(F.access, "final") &&
          defaultFieldValue(F))
        Candidates.emplace(F.reference.name, F.reference.type);
  }
  if (Candidates.empty())
    return Plan;
  std::set<FieldRef> References;
  for (const auto &[Name, C] : Classes)
    if (!C.external)
      for (const auto &M : C.methods)
        for (const auto &Op : M.instructions) {
          B.tick();
          if (const auto *Ref = std::get_if<FieldRef>(&Op.reference);
              Ref && starts(Op.opcode, Static ? "sput" : "iput") &&
              Candidates.contains({Ref->name, Ref->type}))
            References.insert(*Ref);
        }
  for (const auto &Ref : References) {
    B.tick();
    Plan.fields.insert(Ref);
    // Use the same inherited-member resolution as the body verifier. A
    // symbolic child owner must not conceal a write to its declaring parent.
    if (const auto *Declaration = member(Ref, Classes, B))
      Plan.fields.insert(Declaration->reference);
    else
      Plan.unresolved.emplace(Ref.name, Ref.type);
  }
  return Plan;
}

struct InitializationPlan {
  std::set<FieldRef> fields;
  std::set<FieldRef> default_statics;
  std::set<FieldRef> default_instances;
  std::set<FieldRef> pre_super_defaults;
  // Java cannot express a final store after a this(...) constructor that
  // leaves this slot at its allocation default. Original boundaries and stores
  // stay intact; the required build utility restores the class-file flags.
  std::set<FieldRef> runtime_final_fields;
  std::map<MethodRef, std::set<FieldRef>> writes;
};

// Keep blank-final writes as source statements outside dispatch loops. Each
// intervening CFG region may exit normally only at the next write (or the
// final return), so writes cannot be skipped or revisited. The body verifier
// still owns reachability, operand types and instruction semantics.
struct FinalWritePlan {
  struct OrderedStage {
    std::optional<FieldRef> field;
    std::set<size_t> members, writes;
  };
  // Alternative protected stores may share one next field event even when
  // their handlers merge across physical instruction ranges.
  std::vector<OrderedStage> ordered_stages;
  struct BranchStage {
    OrderedStage flow;
    std::map<FieldRef, std::set<size_t>> stores;
    std::map<FieldRef, size_t> successors;
  };
  std::vector<BranchStage> branch_stages;
  struct Diamond {
    size_t branch, taken, join;
    // Additional forward guards share the same taken arm. The instructions
    // between them have no required final write or alternative control edge.
    std::vector<size_t> guards;
    // An inner arm may throw before its sibling's physical entry while the
    // normal arm jumps to the enclosing join beyond that physical boundary.
    std::optional<size_t> taken_end;
  };
  std::vector<size_t> boundaries;
  std::map<size_t, Diamond> diamonds;
  std::set<size_t> structured_jumps;
  std::set<size_t> inverted_guards;
  // Conditional edges may leave a structured assignment region only through
  // an exact linear throw-only tail. Emit that original tail inside its guard.
  std::map<size_t, std::pair<size_t, size_t>> abrupt_guards;
  std::optional<size_t> structured_end;
  // Physically trailing linear exception blocks each end in throw and never
  // reach another write or normal return. Only exact block entries are legal.
  std::optional<size_t> terminal_tail;
  std::set<size_t> terminal_tail_entries;
  std::set<size_t> terminal_tail_regions;
  // A complete unprotected diamond prefix can precede later single-store
  // stages. Exception dispatch never re-enters that assigned prefix.
  std::optional<size_t> structured_prefix_end;
  // A protected preparation region can converge on an unprotected diamond
  // suffix. All original normal and exceptional entries reach its first guard.
  std::optional<size_t> structured_suffix_begin;
  // The closed middle owns its assignments; ordinary stages before and after
  // it retain their original loops, switches and abrupt exits.
  std::optional<std::pair<size_t, size_t>> structured_middle;
};

std::optional<FinalWritePlan>
orderedFinalWriteStages(const Method &M, const std::set<FieldRef> &Required,
                        Budget &B, bool computedReferences = false) {
  const bool Static = M.reference.name == "<clinit>";
  if ((Static ? !has(M.access, "static")
              : M.reference.name != "<init>" || has(M.access, "static")) ||
      Required.empty() || M.instructions.empty())
    return {};
  const auto &Code = M.instructions;
  std::map<uint32_t, size_t> Positions;
  std::map<FieldRef, unsigned> Counts;
  std::set<FieldRef> Protected;
  std::set<size_t> OriginalWrites;
  for (size_t I = 0; I < Code.size(); ++I) {
    B.tick();
    const auto &Op = Code[I];
    if (!Positions.emplace(Op.pc, I).second || (I && Code[I - 1].pc >= Op.pc) ||
        starts(Op.opcode, "monitor-"))
      return {};
    if (!starts(Op.opcode, Static ? "sput" : "iput"))
      continue;
    const auto *Field = std::get_if<FieldRef>(&Op.reference);
    if (!Field || !Required.contains(*Field))
      continue;
    if (Field->owner != M.reference.owner ||
        Op.registers.size() != (Static ? 1u : 2u))
      return {};
    ++Counts[*Field];
    OriginalWrites.insert(I);
    for (const auto &Try : M.tries) {
      B.tick();
      if (Try.start <= Op.pc && Op.pc < Try.end)
        Protected.insert(*Field);
    }
  }
  // A class initializer may close resources and attach suppressed failures
  // around its own protected stores. Reuse the complete normal/exceptional
  // event graph below: every required field has one original store, and no
  // return or later stage may skip or revisit it. Other static write shapes
  // retain their existing proofs.
  const bool protectedProducers = Static && !M.tries.empty() &&
      !OriginalWrites.empty() &&
      std::all_of(M.tries.begin(), M.tries.end(), [&](const auto &region) {
        B.tick();
        return region.end <= Code[*OriginalWrites.begin()].pc &&
               std::all_of(region.handlers.begin(), region.handlers.end(),
                           [&](const auto &handler) {
                             return handler.target > Code[*OriginalWrites.rbegin()].pc;
                           });
      });
  if (Static &&
      ((Protected.empty() && !protectedProducers) ||
       !std::all_of(Required.begin(), Required.end(),
                    [&](const FieldRef &Field) { return Counts[Field] == 1; })))
    return {};
  const bool ProtectedAlternatives =
      std::any_of(Protected.begin(), Protected.end(),
                  [&](const FieldRef &Field) { return Counts[Field] > 1; });
  // One protected store may share the ordered proof when its handlers form
  // one independent linear throw-only tail. Keep multiple single-store
  // cleanup regions and independent abrupt tails in their existing proofs.
  bool SingleProtectedStore =
      !Static && Protected.size() == 1 &&
      std::all_of(Required.begin(), Required.end(),
                  [&](const FieldRef &Field) { return Counts[Field] == 1; });
  std::optional<uint32_t> HandlerEntry;
  if (SingleProtectedStore) {
    for (const auto &Try : M.tries) {
      B.tick();
      for (const auto &Handler : Try.handlers) {
        if (Try.end > Handler.target ||
            (HandlerEntry && *HandlerEntry != Handler.target))
          SingleProtectedStore = false;
        HandlerEntry = Handler.target;
      }
    }
    if (!HandlerEntry || !Positions.contains(*HandlerEntry) ||
        Code[Positions.at(*HandlerEntry)].opcode != "move-exception" ||
        Code.back().opcode != "throw")
      SingleProtectedStore = false;
    for (size_t I = 0; SingleProtectedStore && I < Code.size(); ++I) {
      B.tick();
      const auto &Op = Code[I];
      if ((Op.opcode == "throw" && I + 1 != Code.size()) ||
          (Op.pc >= *HandlerEntry &&
           (branchTarget(Op) || !Op.targets.empty() ||
            starts(Op.opcode, "return"))))
        SingleProtectedStore = false;
    }
  }
  const bool UnprotectedAlternatives =
      M.tries.empty() &&
      std::any_of(Counts.begin(), Counts.end(),
                  [](const auto &Entry) { return Entry.second > 1; });
  bool CyclicAlternatives = false;
  if (UnprotectedAlternatives) {
    for (const auto &Op : Code) {
      B.tick();
      const auto Target = branchTarget(Op);
      CyclicAlternatives |= Target && *Target <= Op.pc;
      for (const auto PC : Op.targets)
        CyclicAlternatives |= PC <= Op.pc;
    }
  }
  // A complete unprotected write prefix can precede split catch/finally
  // regions. Reuse the ordered event proof for the original stores and tail;
  // handler entries still cannot skip or revisit any required assignment.
  const bool UnprotectedPrefix =
      !M.tries.empty() && Protected.empty() &&
      std::all_of(Required.begin(), Required.end(),
                  [&](const FieldRef &Field) { return Counts[Field] == 1; }) &&
      std::all_of(OriginalWrites.begin(), OriginalWrites.end(), [&](size_t I) {
        return std::all_of(M.tries.begin(), M.tries.end(),
                           [&](const auto &Try) {
                             B.tick();
                             return Code[I].pc < Try.start;
                           });
      });
  // When a protected initializer translates failure into a fresh exception,
  // every handler ends independently. Other throws in the normal body and
  // other stores inside the same try do not change definite assignment.
  const auto wrappedInitializerFailure = [&] {
    if (Static || Protected.empty() || M.tries.size() != 1 ||
        !std::all_of(Required.begin(), Required.end(),
                     [&](const FieldRef &field) { return Counts[field] == 1; }))
      return false;
    const auto &region = M.tries.front();
    if (region.handlers.empty())
      return false;
    for (const auto &handler : region.handlers) {
      B.tick();
      const auto at = Positions.find(handler.target);
      if (at == Positions.end() || handler.target < region.end ||
          Code[at->second].opcode != "move-exception" ||
          at->second + 1 == Code.size())
        return false;
      const auto &allocation = Code[at->second + 1];
      const auto *type = std::get_if<std::string>(&allocation.reference);
      if (allocation.opcode != "new-instance" || !type ||
          allocation.registers.size() != 1)
        return false;
      bool initialized = false;
      size_t i = at->second + 2;
      for (; i < Code.size() && Code[i].opcode != "throw"; ++i) {
        B.tick();
        const auto &op = Code[i];
        if (branchTarget(op) || !op.targets.empty() ||
            starts(op.opcode, "return") || op.opcode == "move-exception" ||
            OriginalWrites.contains(i))
          return false;
        if (const auto *call = std::get_if<MethodRef>(&op.reference);
            call && baseOpcode(op.opcode) == "invoke-direct" &&
            call->owner == *type && call->name == "<init>" &&
            !op.registers.empty() &&
            op.registers.front() == allocation.registers.front())
          initialized = true;
      }
      if (!initialized || i == Code.size() ||
          Code[i].registers != allocation.registers)
        return false;
    }
    return true;
  };
  // D8 can share one physical string store by jumping backward from the
  // second literal. The event graph still proves that no path revisits it.
  const auto sharedLiteralStore = [&] {
    if (Static || !M.tries.empty() || Required.size() != 1 ||
        OriginalWrites.size() != 1)
      return false;
    const auto store = *OriginalWrites.begin();
    const auto literal = [&](size_t i) {
      return i && baseOpcode(Code[i - 1].opcode) == "const-string" &&
             Code[i - 1].registers ==
                 std::vector<unsigned>{Code[store].registers.front()};
    };
    if (Code[store].opcode != "iput-object" ||
        Required.begin()->type != "Ljava/lang/String;" || !literal(store))
      return false;
    for (size_t i = store + 1; i < Code.size(); ++i) {
      B.tick();
      if (starts(Code[i].opcode, "goto") &&
          branchTarget(Code[i]) == Code[store].pc && literal(i))
        return true;
    }
    return false;
  };
  if (computedReferences && !wrappedInitializerFailure() &&
      !sharedLiteralStore()) {
    // A constructor can select a factory result before storing a blank
    // reference field. Its call may throw before the field is assigned.
    // Keep protected stores and handlers in their independently proved plans.
    if (!M.tries.empty()) {
      if (Static || !Protected.empty() || OriginalWrites.empty() ||
          std::none_of(Counts.begin(), Counts.end(),
                       [](const auto &entry) { return entry.second > 1; }))
        return {};
      for (const auto &region : M.tries) {
        if (region.start <= Code[*OriginalWrites.begin()].pc ||
            region.handlers.empty())
          return {};
        for (const auto &handler : region.handlers) {
          const auto entry = Positions.find(handler.target);
          if (entry == Positions.end() || handler.target < region.end ||
              Code[entry->second].opcode != "move-exception")
            return {};
          auto end = entry->second;
          while (end < Code.size() && Code[end].opcode != "throw") {
            B.tick();
            if (branchTarget(Code[end]) || !Code[end].targets.empty() ||
                starts(Code[end].opcode, "return") ||
                OriginalWrites.contains(end))
              return {};
            ++end;
          }
          if (end == Code.size())
            return {};
        }
      }
    }
    if (Static ||
        std::none_of(OriginalWrites.begin(), OriginalWrites.end(),
                     [&](size_t I) {
                       B.tick();
                       return I >= 2 && Code[I].opcode == "iput-object" &&
                              Code[I - 1].opcode == "move-result-object" &&
                              Code[I - 1].registers ==
                                  std::vector<unsigned>{Code[I].registers[0]} &&
                              starts(Code[I - 2].opcode, "invoke-");
                     }))
      return {};
  }
  if ((!Static && !computedReferences && !ProtectedAlternatives &&
       !SingleProtectedStore && !CyclicAlternatives && !UnprotectedPrefix) ||
      std::any_of(Required.begin(), Required.end(),
                  [&](const FieldRef &Field) { return Counts[Field] == 0; }))
    return {};
  FinalWritePlan Plan;
  std::set<FieldRef> Written;
  std::set<size_t> Entries{0}, CoveredWrites;
  for (size_t Number = 0; Number <= Required.size(); ++Number) {
    FinalWritePlan::OrderedStage Stage;
    std::deque<size_t> Queue(Entries.begin(), Entries.end());
    std::set<size_t> Next;
    bool Returns = false;
    auto edge = [&](uint32_t PC) {
      B.tick();
      const auto At = Positions.find(PC);
      if (At == Positions.end())
        javaError("final initialization edge has no executable destination");
      Queue.push_back(At->second);
    };
    while (!Queue.empty()) {
      const auto I = Queue.front();
      Queue.pop_front();
      B.tick();
      if (!Stage.members.insert(I).second)
        continue;
      const auto &Op = Code[I];
      if (OriginalWrites.contains(I)) {
        const auto &Field = std::get<FieldRef>(Op.reference);
        if (Written.contains(Field) || (Stage.field && *Stage.field != Field) ||
            I + 1 == Code.size())
          return {};
        Stage.field = Field;
        Stage.writes.insert(I);
        CoveredWrites.insert(I);
        Next.insert(I + 1);
        // Body independently verifies this or the initializing class, member
        // access, value type, opcode and metadata. That own-field assignment
        // cannot throw; producers keep all original exceptional edges.
        continue;
      }
      if (starts(Op.opcode, "return")) {
        if (Op.opcode != "return-void" || Written != Required)
          return {};
        Returns = true;
      }
      for (auto PC : normalSuccessors(Code, I))
        edge(PC);
      if (canThrow(Op))
        for (const auto &Try : M.tries) {
          B.tick();
          if (Try.start <= Op.pc && Op.pc < Try.end)
            for (const auto &Handler : Try.handlers)
              edge(Handler.target);
        }
    }
    if (!Stage.field) {
      if (!Returns || Written != Required || CoveredWrites != OriginalWrites)
        return {};
      Plan.ordered_stages.push_back(std::move(Stage));
      return Plan;
    }
    if (Returns)
      return {};
    Written.insert(*Stage.field);
    Plan.ordered_stages.push_back(std::move(Stage));
    Entries = std::move(Next);
  }
  return {};
}

// A branch can change the order of distinct final stores. Split at the
// original write PCs so each Java arm performs its own field assignment
// before any subsequent instruction or observable field read.
std::optional<FinalWritePlan> branchedFinalWriteStages(
    const Method &method, const std::set<FieldRef> &required, Budget &budget) {
  if (method.reference.name != "<init>" || !method.tries.empty() ||
      required.size() < 2 || required.size() > 64 || method.instructions.empty())
    return {};
  const auto &code = method.instructions;
  std::map<uint32_t, size_t> positions;
  std::map<size_t, FieldRef> stores;
  std::set<size_t> covered;
  for (size_t i = 0; i < code.size(); ++i) {
    budget.tick();
    const auto &op = code[i];
    if (!positions.emplace(op.pc, i).second || starts(op.opcode, "monitor-"))
      return {};
    const auto *field = std::get_if<FieldRef>(&op.reference);
    if (starts(op.opcode, "iput") && field && required.contains(*field)) {
      if (field->owner != method.reference.owner || op.registers.size() != 2)
        return {};
      stores.emplace(i, *field);
    }
  }
  struct Entry {
    std::set<size_t> starts;
    std::set<FieldRef> assigned;
  };
  std::vector<Entry> entries{{{0}, {}}};
  FinalWritePlan plan;
  bool split = false, returns = false;
  for (size_t n = 0; n < entries.size(); ++n) {
    const auto entry = entries[n];
    auto &stage = plan.branch_stages.emplace_back();
    std::deque<size_t> visit(entry.starts.begin(), entry.starts.end());
    while (!visit.empty()) {
      budget.tick();
      const auto i = visit.front();
      visit.pop_front();
      if (!stage.flow.members.insert(i).second)
        continue;
      if (const auto store = stores.find(i); store != stores.end()) {
        if (entry.assigned.contains(store->second) || i + 1 == code.size())
          return {};
        stage.flow.writes.insert(i);
        stage.stores[store->second].insert(i);
        covered.insert(i);
        continue;
      }
      if (starts(code[i].opcode, "return")) {
        if (code[i].opcode != "return-void" || entry.assigned != required)
          return {};
        returns = true;
      }
      // Let the ordinary constructor validator diagnose an incomplete CFG.
      // Such an edge cannot establish a branch-specific final write.
      const auto &op = code[i];
      if ((anyPrefix(op.opcode, {"if-", "goto"}) && !branchTarget(op)) ||
          (i + 1 == code.size() &&
           !anyPrefix(op.opcode, {"return", "throw", "goto"})))
        return {};
      for (auto pc : normalSuccessors(code, i)) {
        const auto next = positions.find(pc);
        if (next == positions.end() ||
            (entry.assigned != required && next->second <= i))
          return {};
        visit.push_back(next->second);
      }
    }
    if (stage.stores.size() > 1) {
      // A single fork needs at most two copies of the remaining code.
      if (split || stage.stores.size() != 2)
        return {};
      split = true;
    }
    for (const auto &[field, writes] : stage.stores) {
      Entry next{{}, entry.assigned};
      next.assigned.insert(field);
      for (size_t i : writes)
        next.starts.insert(i + 1);
      stage.successors.emplace(field, entries.size());
      entries.push_back(std::move(next));
    }
  }
  return split && returns && covered.size() == stores.size()
             ? std::optional<FinalWritePlan>(std::move(plan))
             : std::nullopt;
}

struct UnprovedFinalRegions {};

FinalWritePlan structuredFinalWriteRegions(const Method &M,
                                           const std::set<FieldRef> &Required,
                                           Budget &B,
                                           bool AllowStagedMiddle = true,
                                           bool StructureAll = false) {
  if (auto Ordered = orderedFinalWriteStages(M, Required, B))
    return std::move(*Ordered);
  auto fail = []() -> void { throw UnprovedFinalRegions{}; };
  const auto &Code = M.instructions;
  if (Code.empty())
    fail();
  size_t ExecutableEnd = Code.size();
  while (ExecutableEnd && emptyNop(Code[ExecutableEnd - 1])) {
    B.tick();
    --ExecutableEnd;
  }
  if (ExecutableEnd && ExecutableEnd < Code.size() &&
      Code[ExecutableEnd - 1].opcode == "throw") {
    // DEX payload alignment can leave exact empty instructions after the
    // terminal cleanup rethrow. They have no fallthrough predecessor, but
    // every explicit normal and exceptional entry must also stay outside.
    // Trim only this proof copy: Body and the recovery inventory keep the
    // original instructions, PCs, handlers and terminal throw.
    const auto PaddingPC = Code[ExecutableEnd].pc;
    for (size_t I = 0; I < Code.size(); ++I) {
      B.tick();
      const auto &Op = Code[I];
      if (Op.pc >= M.code_end || (I && Code[I - 1].pc >= Op.pc) ||
          (I >= ExecutableEnd && Op.pc != Code[I - 1].pc + 1) ||
          (branchTarget(Op) && *branchTarget(Op) >= PaddingPC) ||
          std::any_of(Op.targets.begin(), Op.targets.end(),
                      [&](uint32_t PC) { return PC >= PaddingPC; }))
        fail();
    }
    for (const auto &Try : M.tries) {
      B.tick();
      if (Try.start >= PaddingPC ||
          std::any_of(
              Try.handlers.begin(), Try.handlers.end(),
              [&](const auto &Handler) { return Handler.target >= PaddingPC; }))
        fail();
    }
    Method Executable = M;
    Executable.instructions.resize(ExecutableEnd);
    return structuredFinalWriteRegions(Executable, Required, B, AllowStagedMiddle,
                             StructureAll);
  }
  std::optional<size_t> Return, Tail;
  std::set<size_t> TailEntries;
  bool HasThrow = false;
  for (size_t I = 0; I < Code.size(); ++I) {
    B.tick();
    if (starts(Code[I].opcode, "return")) {
      if (Return || Code[I].opcode != "return-void")
        fail();
      Return = I;
    }
    HasThrow |= Code[I].opcode == "throw";
  }
  const bool AbruptConstructor = !Return;
  if (AbruptConstructor) {
    // A constructor need not assign every blank final if it always throws.
    // Keep the original throw as its last stage boundary; never invent a
    // normal return or a missing store. This profile has no branch, handler
    // or earlier abrupt exit, and each original store still occurs once.
    if (M.reference.name != "<init>" || has(M.access, "static") ||
        !M.tries.empty() || Code.back().opcode != "throw")
      fail();
    for (size_t I = 0; I < Code.size(); ++I) {
      B.tick();
      const auto &Op = Code[I];
      if (branchTarget(Op) || !Op.targets.empty() ||
          anyPrefix(Op.opcode, {"if-", "goto", "return"}) ||
          Op.opcode.find("switch") != std::string::npos ||
          (Op.opcode == "throw" && I + 1 != Code.size()))
        fail();
    }
    Return = Code.size() - 1;
  }
  if (*Return + 1 < Code.size()) {
    const bool Padding =
        std::all_of(Code.begin() + *Return + 1, Code.end(), emptyNop);
    if (!Padding) {
      Tail = *Return + 1;
      if (Code.back().opcode != "throw")
        fail();
      TailEntries.insert(*Tail);
    }
    for (size_t I = Tail.value_or(Code.size()); I < Code.size(); ++I) {
      const auto &Op = Code[I];
      B.tick();
      if (branchTarget(Op) || !Op.targets.empty() ||
          anyPrefix(Op.opcode, {"if-", "goto", "return"}) ||
          Op.opcode.find("switch") != std::string::npos)
        fail();
      if (Op.opcode == "throw" && I + 1 < Code.size())
        TailEntries.insert(I + 1);
    }
    // Protected preparation may leave through independent throw-only tails.
    // No tail instruction is protected, and every exceptional entry must be
    // its original capture at a complete tail boundary. The stage proof still
    // rejects skipped stores and normal/exceptional re-entry between stages.
    if (TailEntries.size() > 1 && !M.tries.empty())
      for (const auto &Try : M.tries) {
        B.tick();
        if (Try.end > Code[*Tail].pc)
          fail();
        for (const auto &Handler : Try.handlers)
          if (std::none_of(TailEntries.begin(), TailEntries.end(),
                           [&](size_t At) {
                             B.tick();
                             return Code[At].pc == Handler.target &&
                                    Code[At].opcode == "move-exception" &&
                                    Code[At].registers.size() == 1;
                           }))
            fail();
      }
  }
  std::map<uint32_t, size_t> Positions;
  std::map<FieldRef, unsigned> Counts;
  std::vector<size_t> Boundaries;
  bool ProtectedSelfStores = false;
  const bool Static = M.reference.name == "<clinit>";
  for (size_t I = 0; I < Code.size(); ++I) {
    B.tick();
    const auto &Op = Code[I];
    if (!Positions.emplace(Op.pc, I).second || (I && Code[I - 1].pc >= Op.pc))
      fail();
    if (starts(Op.opcode, Static ? "sput" : "iput"))
      if (const auto *Ref = std::get_if<FieldRef>(&Op.reference);
          Ref && Required.contains(*Ref)) {
        if (I >= *Return)
          fail();
        ++Counts[*Ref];
        Boundaries.push_back(I);
        for (const auto &Try : M.tries) {
          B.tick();
          if (Op.pc >= Try.start && Op.pc < Try.end) {
            if ((!Static && M.reference.name != "<init>") ||
                Ref->owner != M.reference.owner ||
                Op.registers.size() != (Static ? 1u : 2u) || Op.target ||
                !Op.targets.empty() || !Op.keys.empty() || !Op.data.empty() ||
                Op.element_width ||
                !std::holds_alternative<std::monostate>(Op.literal))
              fail();
            // Own static stores retain the same ordered source assignment.
            // The stage proof keeps their original typed throw-only tails;
            // instance self stores retain their existing cleanup profile.
            ProtectedSelfStores |= !Static;
          }
        }
      }
  }
  if (ProtectedSelfStores) {
    // Body separately requires exact this and a verified register value for
    // every final store. That source assignment cannot throw; all throwing
    // producers keep their original handler dispatch in the preceding stage.
    // A physically trailing catch-all captures the exception, calls one
    // original void cleanup and rethrows that same untouched register.
    if (!Tail || TailEntries.size() != 1 || Code.size() - *Tail != 3 ||
        std::any_of(Required.begin(), Required.end(),
                    [&](const FieldRef &Field) { return Counts[Field] != 1; }))
      fail();
    const auto &Capture = Code[*Tail], &Cleanup = Code[*Tail + 1];
    const auto *Call = std::get_if<MethodRef>(&Cleanup.reference);
    if (Capture.opcode != "move-exception" || Capture.registers.size() != 1 ||
        !starts(Cleanup.opcode, "invoke-") || !Call || Call->returns != "V" ||
        Call->name.starts_with('<') ||
        Code[*Tail + 2].registers != Capture.registers)
      fail();
    for (const auto &Try : M.tries) {
      B.tick();
      if (Try.start <= Code.front().pc || Try.end > Code[*Return].pc ||
          Try.handlers.size() != 1 || Try.handlers[0].type ||
          Try.handlers[0].target != Code[*Tail].pc)
        fail();
    }
  }
  auto stages = [&](FinalWritePlan &Plan, size_t Begin,
                    bool IncludeBoundary = true) {
    for (auto End : Plan.boundaries) {
      const bool Middle =
          Plan.structured_middle && Plan.structured_middle->first == End;
      for (size_t I = Begin; I < End + (IncludeBoundary && !Middle); ++I) {
        B.tick();
        const auto &Op = Code[I];
        auto edge = [&](uint32_t Target) {
          const auto P = Positions.find(Target);
          if (P != Positions.end() &&
              Plan.terminal_tail_entries.contains(P->second)) {
            Plan.terminal_tail_regions.insert(End);
            return;
          }
          if (P == Positions.end() || P->second < Begin || P->second > End)
            fail();
        };
        if (const auto Target = branchTarget(Op))
          edge(*Target);
        for (auto Target : Op.targets)
          edge(Target);
        if (canThrow(Op))
          for (const auto &Try : M.tries) {
            B.tick();
            if (Op.pc >= Try.start && Op.pc < Try.end)
              for (const auto &Handler : Try.handlers)
                edge(Handler.target);
          }
      }
      Begin = Middle ? Plan.structured_middle->second : End + 1;
    }
  };
  if (StructureAll ||
      std::any_of(Required.begin(), Required.end(), [&](const auto &Field) {
        return Counts[Field] > 1 || (!AbruptConstructor && Counts[Field] == 0);
      })) {
    // Older proof copies describe a completely structured prefix/suffix.
    // Only a full unprotected method may compose an independently staged
    // middle; those copies must not silently discard its surrounding stages.
    if (AllowStagedMiddle && M.tries.empty() &&
        std::all_of(
            Required.begin(), Required.end(),
            [&](const FieldRef &Field) { return Counts[Field] != 0; })) {
      std::set<FieldRef> Repeated;
      size_t First = Code.size(), Last = 0, Lower = 0;
      for (const auto &Field : Required)
        if (Counts[Field] > 1)
          Repeated.insert(Field);
      for (auto At : Boundaries)
        if (Repeated.contains(std::get<FieldRef>(Code[At].reference))) {
          First = std::min(First, At);
          Last = std::max(Last, At);
        }
      for (auto At : Boundaries)
        if (At < First &&
            !Repeated.contains(std::get<FieldRef>(Code[At].reference)))
          Lower = std::max(Lower, At + 1);
      std::optional<size_t> Begin;
      // The first guard physically separating repeated stores owns their
      // common split. Later guards sharing its taken arm stay in that proof.
      for (size_t I = Lower; !Repeated.empty() && I < First; ++I) {
        B.tick();
        const auto Target = branchTarget(Code[I]);
        const auto At = Target ? Positions.find(*Target) : Positions.end();
        if (starts(Code[I].opcode, "if-") && At != Positions.end() &&
            At->second > First && At->second <= Last) {
          Begin = I;
          break;
        }
      }
      size_t End = Last + 1;
      bool Closed = Begin.has_value();
      for (size_t I = Begin.value_or(End); Closed && I < End; ++I) {
        B.tick();
        const auto &Op = Code[I];
        if (!Op.targets.empty() ||
            Op.opcode.find("switch") != std::string::npos ||
            anyPrefix(Op.opcode, {"return", "throw"})) {
          Closed = false;
          break;
        }
        if (const auto Target = branchTarget(Op)) {
          const auto At = Positions.find(*Target);
          if (At == Positions.end() || At->second <= I ||
              At->second > *Return) {
            Closed = false;
            break;
          }
          End = std::max(End, At->second);
        }
      }
      for (auto At : Boundaries)
        if (Closed && At >= *Begin && At < End &&
            !Repeated.contains(std::get<FieldRef>(Code[At].reference)))
          Closed = false;
      if (Closed && !HasThrow) {
        // Keep an already complete structured continuation in its original
        // form. Only a later loop or switch needs this staged alternative.
        bool DispatchContinuation = false;
        for (size_t At = End; At < Code.size(); ++At) {
          B.tick();
          const auto &Op = Code[At];
          const auto Target = branchTarget(Op);
          DispatchContinuation |=
              !Op.targets.empty() ||
              Op.opcode.find("switch") != std::string::npos ||
              (Target && *Target <= Op.pc);
        }
        Closed = DispatchContinuation;
      }
      if (Closed) {
        Method Middle = M;
        Middle.instructions.assign(Code.begin() + *Begin, Code.begin() + End);
        Instruction Exit;
        Exit.pc = Code[End].pc;
        Exit.opcode = "return-void";
        Middle.instructions.push_back(std::move(Exit));
        Middle.code_end = Code[End].pc + 1;
        const auto Inner = structuredFinalWriteRegions(Middle, Repeated, B, false);
        if (Inner.diamonds.empty())
          fail();
        FinalWritePlan Plan;
        for (auto [At, Diamond] : Inner.diamonds) {
          Diamond.branch += *Begin;
          Diamond.taken += *Begin;
          Diamond.join += *Begin;
          if (Diamond.taken_end)
            *Diamond.taken_end += *Begin;
          for (auto &Guard : Diamond.guards)
            Guard += *Begin;
          Plan.diamonds.emplace(At + *Begin, std::move(Diamond));
        }
        for (auto At : Inner.structured_jumps)
          Plan.structured_jumps.insert(At + *Begin);
        for (auto At : Inner.inverted_guards)
          Plan.inverted_guards.insert(At + *Begin);
        for (const auto &[At, Range] : Inner.abrupt_guards)
          Plan.abrupt_guards.emplace(
              At + *Begin,
              std::pair{Range.first + *Begin, Range.second + *Begin});
        for (auto At : Boundaries)
          if (!Repeated.contains(std::get<FieldRef>(Code[At].reference)))
            Plan.boundaries.push_back(At);
        Plan.boundaries.push_back(*Begin);
        Plan.boundaries.push_back(*Return);
        std::sort(Plan.boundaries.begin(), Plan.boundaries.end());
        Plan.structured_middle = std::pair{*Begin, End};
        Plan.structured_end = End;
        Plan.terminal_tail = Tail;
        Plan.terminal_tail_entries = TailEntries;
        stages(Plan, 0);
        return Plan;
      }
    }
    if (M.tries.empty() && !HasThrow) {
      size_t FirstMultiple = Code.size();
      for (auto At : Boundaries)
        if (Counts.at(std::get<FieldRef>(Code[At].reference)) > 1)
          FirstMultiple = std::min(FirstMultiple, At);
      std::set<FieldRef> PrefixFields, Remaining;
      size_t Split = 0;
      for (const auto &Field : Required) {
        size_t At = Code.size();
        if (Counts[Field] == 1)
          for (auto Index : Boundaries)
            if (std::get<FieldRef>(Code[Index].reference) == Field)
              At = Index;
        if (At < FirstMultiple) {
          PrefixFields.insert(Field);
          Split = std::max(Split, At + 1);
        } else
          Remaining.insert(Field);
      }
      if (FirstMultiple < Code.size() && !PrefixFields.empty() &&
          !Remaining.empty() && Split <= FirstMultiple) {
        // Common single writes keep their closed CFG stages, including a
        // switch before the next write. The remaining original suffix uses
        // the complete structured branch count proof. No edge may cross the
        // cut except through its first original instruction.
        Method Prefix = M;
        Prefix.instructions.resize(Split);
        Instruction Exit;
        Exit.pc = Code[Split].pc;
        Exit.opcode = "return-void";
        Prefix.instructions.push_back(std::move(Exit));
        Prefix.code_end = Code[Split].pc + 1;
        auto Plan = structuredFinalWriteRegions(Prefix, PrefixFields, B, false);
        Method Suffix = M;
        Suffix.instructions.erase(Suffix.instructions.begin(),
                                  Suffix.instructions.begin() + Split);
        auto TailPlan = structuredFinalWriteRegions(Suffix, Remaining, B, false);
        if (TailPlan.diamonds.empty())
          fail();
        for (auto &[At, Diamond] : TailPlan.diamonds) {
          Diamond.branch += Split;
          Diamond.taken += Split;
          Diamond.join += Split;
          if (Diamond.taken_end)
            *Diamond.taken_end += Split;
          for (auto &Guard : Diamond.guards)
            Guard += Split;
          Plan.diamonds.emplace(At + Split, std::move(Diamond));
        }
        for (auto At : TailPlan.structured_jumps)
          Plan.structured_jumps.insert(At + Split);
        for (auto At : TailPlan.inverted_guards)
          Plan.inverted_guards.insert(At + Split);
        for (const auto &[At, Range] : TailPlan.abrupt_guards)
          Plan.abrupt_guards.emplace(
              At + Split, std::pair{Range.first + Split, Range.second + Split});
        if (TailPlan.structured_end)
          Plan.structured_end = *TailPlan.structured_end + Split;
        Plan.structured_suffix_begin = Split;
        return Plan;
      }
    }
    if (!M.tries.empty()) {
      // The opposite composition keeps all protected preparation before the
      // first final store. Reuse the complete unprotected diamond proof for
      // its suffix, and the stage edge proof for every prefix entry and
      // handler.
      size_t Closed = 0;
      uint32_t LastTryEnd = 0;
      for (const auto &Try : M.tries) {
        B.tick();
        LastTryEnd = std::max(LastTryEnd, Try.end);
        for (const auto &Handler : Try.handlers) {
          const auto At = Positions.find(Handler.target);
          if (At == Positions.end())
            fail();
          Closed = std::max(Closed, At->second + 1);
        }
      }
      for (size_t Split = Closed;
           !Boundaries.empty() && Split < Boundaries.front(); ++Split) {
        B.tick();
        if (Code[Split].pc < LastTryEnd || !starts(Code[Split].opcode, "if-"))
          continue;
        Method Suffix = M;
        Suffix.tries.clear();
        Suffix.instructions.erase(Suffix.instructions.begin(),
                                  Suffix.instructions.begin() + Split);
        auto SuffixPlan = structuredFinalWriteRegions(Suffix, Required, B, false);
        FinalWritePlan Plan;
        for (auto &[At, Diamond] : SuffixPlan.diamonds) {
          Diamond.branch += Split;
          Diamond.taken += Split;
          Diamond.join += Split;
          if (Diamond.taken_end)
            *Diamond.taken_end += Split;
          for (auto &Guard : Diamond.guards)
            Guard += Split;
          Plan.diamonds.emplace(At + Split, std::move(Diamond));
        }
        for (auto At : SuffixPlan.structured_jumps)
          Plan.structured_jumps.insert(At + Split);
        for (auto At : SuffixPlan.inverted_guards)
          Plan.inverted_guards.insert(At + Split);
        for (const auto &[At, Range] : SuffixPlan.abrupt_guards)
          Plan.abrupt_guards.emplace(
              At + Split, std::pair{Range.first + Split, Range.second + Split});
        if (SuffixPlan.structured_end)
          Plan.structured_end = *SuffixPlan.structured_end + Split;
        if (Plan.diamonds.empty())
          fail();
        Plan.boundaries = {Split};
        Plan.structured_suffix_begin = Split;
        // The guard belongs to the structured suffix; prefix edges can reach
        // that boundary, but cannot skip it or enter either assigned arm.
        stages(Plan, 0, false);
        return Plan;
      }
      const auto FirstTry = std::min_element(M.tries.begin(), M.tries.end(),
                                             [](const auto &A, const auto &B) {
                                               return A.start < B.start;
                                             })
                                ->start;
      std::set<FieldRef> PrefixFields, Remaining;
      size_t Split = 0;
      for (const auto &Field : Required) {
        bool Before = Counts[Field] != 0;
        size_t Last = 0;
        for (auto At : Boundaries) {
          B.tick();
          if (std::get<FieldRef>(Code[At].reference) != Field)
            continue;
          Before &= Code[At].pc < FirstTry;
          Last = At + 1;
        }
        if (Before) {
          PrefixFields.insert(Field);
          Split = std::max(Split, Last);
        } else {
          if (Counts[Field] != 1)
            fail();
          Remaining.insert(Field);
        }
      }
      if (PrefixFields.empty() || Split >= Code.size())
        fail();
      // Include the unchanged joins after the last prefix store. The fake
      // return exists only in the proof copy; no original operation is lost.
      for (size_t I = 0; I < Split; ++I) {
        auto extend = [&](uint32_t PC) {
          B.tick();
          const auto At = Positions.find(PC);
          if (At == Positions.end())
            fail();
          Split = std::max(Split, At->second);
        };
        if (const auto PC = branchTarget(Code[I]))
          extend(*PC);
        for (auto PC : Code[I].targets)
          extend(PC);
        if (Split >= Code.size() || Code[Split].pc > FirstTry)
          fail();
      }
      Method Prefix = M;
      Prefix.tries.clear();
      Prefix.instructions.resize(Split);
      Instruction Exit;
      Exit.pc = Code[Split].pc;
      Exit.opcode = "return-void";
      Prefix.instructions.push_back(std::move(Exit));
      Prefix.code_end = Code[Split].pc + 1;
      auto PrefixPlan = structuredFinalWriteRegions(Prefix, PrefixFields, B, false);
      Method Unprotected = M;
      Unprotected.tries.clear();
      FinalWritePlan Plan;
      if (Remaining.empty()) {
        Plan.boundaries = {*Return};
        Plan.terminal_tail = Tail;
        Plan.terminal_tail_entries = TailEntries;
      } else {
        Plan = structuredFinalWriteRegions(Unprotected, Remaining, B, false);
      }
      stages(Plan, Split);
      Plan.diamonds = std::move(PrefixPlan.diamonds);
      Plan.structured_jumps = std::move(PrefixPlan.structured_jumps);
      Plan.inverted_guards = std::move(PrefixPlan.inverted_guards);
      Plan.abrupt_guards = std::move(PrefixPlan.abrupt_guards);
      Plan.structured_prefix_end = Split;
      return Plan;
    }
    // Carry the complete incoming assignment counts into each arm, including
    // arms which throw. Every original store must be unique even on an abrupt
    // path; only normal exits have to agree and initialize every required
    // field.
    FinalWritePlan Plan;
    using WriteCounts = std::map<FieldRef, unsigned>;
    using NormalWrites = std::optional<WriteCounts>;
    auto destination = [&](const Instruction &Op) {
      if (!Op.target || !Positions.contains(*Op.target))
        fail();
      return Positions.at(*Op.target);
    };
    std::function<NormalWrites(size_t, size_t, size_t, unsigned,
                               const WriteCounts &)>
        region;
    region = [&](size_t Begin, size_t End, size_t Exit, unsigned Depth,
                 const WriteCounts &Incoming) -> NormalWrites {
      if (Depth > 64)
        fail();
      WriteCounts Count = Incoming;
      for (size_t I = Begin; I < End; ++I) {
        B.tick();
        const auto &Op = Code[I];
        if (!Op.targets.empty() ||
            Op.opcode.find("switch") != std::string::npos)
          fail();
        if (starts(Op.opcode, "if-")) {
          const size_t Taken = destination(Op);
          if (TailEntries.contains(Taken)) {
            const auto Next = TailEntries.upper_bound(Taken);
            const auto End = Next == TailEntries.end() ? Code.size() : *Next;
            Plan.abrupt_guards.emplace(I, std::pair{Taken, End});
            continue;
          }
          if (Taken <= I + 1 || Taken > End)
            fail();
          size_t Join = Taken;
          if (starts(Code[Taken - 1].opcode, "goto")) {
            Join = destination(Code[Taken - 1]);
            if (Join <= Taken)
              fail();
          } else if (Code[Taken - 1].opcode == "throw") {
            // D8 can place an abrupt inner arm after the normal arm's jump,
            // directly before an enclosing sibling. All surviving forward
            // exits must still name one exact enclosing join; recursion proves
            // the whole arm, including the intervening throwing instructions.
            for (size_t Scan = I + 1; Scan < Taken; ++Scan) {
              B.tick();
              if (!starts(Code[Scan].opcode, "goto"))
                continue;
              const auto Exit = destination(Code[Scan]);
              if (Exit > Taken) {
                if (Join != Taken && Join != Exit)
                  fail();
                Join = Exit;
              }
            }
          }
          if (Join > End && Join != Exit)
            fail();
          size_t Fallthrough = I + 1;
          std::vector<size_t> Guards;
          for (size_t Scan = I + 1; Scan < Taken; ++Scan) {
            B.tick();
            const auto &Candidate = Code[Scan];
            if (starts(Candidate.opcode, "if-") && Candidate.targets.empty() &&
                destination(Candidate) == Taken) {
              if (Guards.size() == 63)
                fail();
              Guards.push_back(Scan);
              Fallthrough = Scan + 1;
              continue;
            }
            if (starts(Candidate.opcode, "if-") && Candidate.targets.empty() &&
                Scan + 2 < Taken && destination(Candidate) == Scan + 2 &&
                starts(Code[Scan + 1].opcode, "goto") &&
                destination(Code[Scan + 1]) == Taken) {
              if (Guards.size() == 63)
                fail();
              Guards.push_back(Scan);
              Plan.inverted_guards.insert(Scan);
              Plan.structured_jumps.insert(Scan + 1);
              Fallthrough = Scan + 2;
              ++Scan;
              continue;
            }
            if (branchTarget(Candidate) || !Candidate.targets.empty() ||
                Candidate.opcode.find("switch") != std::string::npos ||
                anyPrefix(Candidate.opcode, {"return", "throw"}))
              break;
            if (starts(Candidate.opcode, Static ? "sput" : "iput"))
              if (const auto *Field =
                      std::get_if<FieldRef>(&Candidate.reference);
                  Field && Required.contains(*Field))
                break;
          }
          const auto FalseArm =
              region(Fallthrough, Taken, Join, Depth + 1, Count);
          const auto TakenEnd = std::min(Join, End);
          const auto TrueArm = region(Taken, TakenEnd, Join, Depth + 1, Count);
          if ((!FalseArm && !TrueArm) ||
              (FalseArm && TrueArm && *FalseArm != *TrueArm))
            fail();
          Count = FalseArm ? *FalseArm : *TrueArm;
          Plan.diamonds.emplace(I, FinalWritePlan::Diamond{I, Taken, Join,
                                                           std::move(Guards),
                                                           TakenEnd});
          if (Join > End)
            return Count; // The enclosing region owns this proved normal exit.
          I = Join - 1;
          continue;
        }
        if (starts(Op.opcode, "goto")) {
          if (I + 1 != End || destination(Op) != Exit || Exit <= I)
            fail();
          Plan.structured_jumps.insert(I);
          return Count;
        }
        if (branchTarget(Op))
          fail();
        if (Op.opcode == "throw") {
          if (I + 1 != End)
            fail();
          return std::nullopt;
        }
        if (starts(Op.opcode, "return") && (I != *Return || I + 1 != End))
          fail();
        if (starts(Op.opcode, Static ? "sput" : "iput"))
          if (const auto *Field = std::get_if<FieldRef>(&Op.reference);
              Field && Required.contains(*Field) && ++Count[*Field] != 1)
            fail();
      }
      if (End != Exit)
        fail(); // An else arm cannot be reached by falling through its sibling.
      return Count;
    };
    const auto End = *Return + 1;
    const auto Complete = region(0, End, End, 0, {});
    if (!Complete)
      fail();
    for (const auto &Field : Required)
      if (!Complete->contains(Field) || Complete->at(Field) != 1)
        fail();
    Plan.structured_end = End;
    return Plan;
  }
  Boundaries.push_back(*Return);
  FinalWritePlan Plan;
  Plan.boundaries = std::move(Boundaries);
  Plan.terminal_tail = Tail;
  Plan.terminal_tail_entries = TailEntries;
  stages(Plan, 0);
  return Plan;
}

FinalWritePlan finalWriteRegions(const Method &method,
                                 const std::set<FieldRef> &required,
                                 Budget &budget, bool allowStagedMiddle = true,
                                 bool structureAll = false) {
  try {
    return structuredFinalWriteRegions(method, required, budget,
                                       allowStagedMiddle, structureAll);
  } catch (const UnprovedFinalRegions &) {
    // Try the whole-method event graph only when the structured entry failed.
    // A recursive prefix or suffix is never used as evidence for this proof.
    // Every normal return must follow one store of every required field in
    // one order. Exceptional edges keep the state before their producer.
    if (method.reference.name == "<init>" && allowStagedMiddle && !structureAll)
      if (auto ordered =
              orderedFinalWriteStages(method, required, budget, true))
        return std::move(*ordered);
    if (allowStagedMiddle && !structureAll)
      if (auto branched = branchedFinalWriteStages(method, required, budget))
        return std::move(*branched);
    const auto &field = *required.begin();
    javaError("final field " + field.owner + "->" + field.name +
              " needs one verified initialization on every return of " +
              method.reference.identity());
  }
}

// Allocation defaults can be materialized after Object's empty preparation
// only while this remains unobserved. Register-only copies do not escape or
// inspect it. Body still verifies the constructor, every later edge and use.
bool trivialObjectPreparation(const Method &M, Budget &B) {
  if (M.reference.name != "<init>" || has(M.access, "static") ||
      M.registers < M.incomingWords())
    return false;
  const unsigned This = M.registers - M.incomingWords();
  std::set<unsigned> Aliases{This};
  for (const auto &Op : M.instructions) {
    B.tick();
    if (emptyNop(Op))
      continue;
    if (Op.target || !Op.targets.empty() ||
        !std::holds_alternative<std::monostate>(Op.literal))
      return false;
    if (Op.opcode == "move-object" || Op.opcode == "move-object/from16" ||
        Op.opcode == "move-object/16") {
      if (Op.registers.size() != 2 || Op.registers[0] >= This ||
          !Aliases.contains(Op.registers[1]) ||
          !std::holds_alternative<std::monostate>(Op.reference))
        return false;
      Aliases.insert(Op.registers[0]);
      continue;
    }
    const auto *Call = std::get_if<MethodRef>(&Op.reference);
    return Call &&
           *Call == MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"} &&
           (Op.opcode == "invoke-direct" ||
            Op.opcode == "invoke-direct/range") &&
           Op.registers.size() == 1 && Aliases.contains(Op.registers[0]);
  }
  return false;
}

InitializationPlan planInitializations(
    const Class &C, const ClassMap &Classes, Budget &B, const EnumPlans &Enums,
    const GenericSignaturePlans &Generics, const FieldWritePlan &StaticWrites,
    const FieldWritePlan &InstanceWrites,
    const NamedAnonymousMembers &NamedAnonymous) {
  InitializationPlan Plan;
  const auto &Delegations = Generics.constructor_delegations;
  if (Generics.independent_inner_classes.contains(C.name)) {
    for (const auto &Field : C.fields) {
      B.tick();
      const auto &F = Field.reference;
      if (!has(Field.access, "final") || has(Field.access, "static") ||
          has(Field.access, "volatile") ||
          !std::holds_alternative<std::monostate>(Field.value))
        continue;
      const auto Base = F.type.find_first_not_of('[');
      const auto Type =
          Base == F.type.npos ? std::string() : F.type.substr(Base);
      const auto Known = Classes.find(Type);
      // The rebuild manifest must retain the exact compiled field descriptor.
      const bool FixedType =
          Primitives.contains(Type) || Type == "Ljava/lang/Object;" ||
          (Known != Classes.end() &&
           (Known->second.external ||
            (!Known->second.enclosing && !Known->second.enclosing_method &&
             !Known->second.inner_class_present)));
      if (!FixedType)
        continue;
      bool MissingPrimary = false, WrittenAfterDelegation = false;
      std::vector<const Method *> Writers;
      for (const auto &M : C.methods) {
        if (M.reference.name != "<init>")
          continue;
        const bool Written = std::any_of(
            M.instructions.begin(), M.instructions.end(),
            [&](const Instruction &Op) {
              B.tick();
              const auto *Target = std::get_if<FieldRef>(&Op.reference);
              return starts(Op.opcode, "iput") && Target && *Target == F;
            });
        MissingPrimary |= !Written && !Delegations.contains(M.reference);
        WrittenAfterDelegation |= Written && Delegations.contains(M.reference);
        if (Written)
          Writers.push_back(&M);
      }
      if (!MissingPrimary || !WrittenAfterDelegation)
        continue;
      // Reuse the original CFG proof, including exceptional exits and stores
      // on exact this. No source inlining or allocation-default store is added.
      for (const auto *M : Writers) {
        finalWriteRegions(*M, {F}, B);
        Plan.writes[M->reference].insert(F);
      }
      Plan.runtime_final_fields.insert(F);
      Plan.fields.insert(F);
    }
  }

  for (bool Static : {false, true}) {
    std::set<FieldRef> Required;
    for (const auto &F : C.fields) {
      B.tick();
      if (Enums.managed_fields.contains(F.reference))
        continue;
      if (F.access.contains("final") &&
          !Plan.runtime_final_fields.contains(F.reference) &&
          F.access.contains("static") == Static &&
          (std::holds_alternative<std::monostate>(F.value) ||
           (Static && defaultFieldValue(F) &&
            (StaticWrites.fields.contains(F.reference) ||
             StaticWrites.unresolved.contains(
                 {F.reference.name, F.reference.type})))))
        Required.insert(F.reference);
    }
    if (!Static && !has(C.access, "interface") && !has(C.access, "enum") &&
        !has(C.access, "annotation") && C.superclass == "Ljava/lang/Object;" &&
        (independentSourceScope(C) ||
         isNamedAnonymousMember(C, NamedAnonymous))) {
      bool HasConstructor = false, ObjectPreparation = true;
      for (const auto &M : C.methods) {
        if (M.reference.name != "<init>")
          continue;
        HasConstructor = true;
        if (Delegations.contains(M.reference))
          continue;
        const auto Prologue =
            Generics.constructor_super_prologues.find(M.reference);
        // The shared origin proof owns prefix effects and uninitialized this.
        // Body still validates every original operation and exception edge.
        const bool LinearObjectPrefix =
            Prologue != Generics.constructor_super_prologues.end() &&
            Prologue->second.target ==
                MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"} &&
            !Prologue->second.requires_dispatch;
        ObjectPreparation &=
            trivialObjectPreparation(M, B) || LinearObjectPrefix;
      }
      if (HasConstructor && ObjectPreparation)
        for (auto I = Required.begin(); I != Required.end();) {
          const auto Found =
              std::find_if(C.fields.begin(), C.fields.end(),
                           [&](const Field &F) { return F.reference == *I; });
          const auto *Field = Found == C.fields.end() ? nullptr : &*Found;
          if (allocationDefaultLiteral(I->type) && Field &&
              std::holds_alternative<std::monostate>(Field->value) &&
              !InstanceWrites.fields.contains(*I) &&
              !InstanceWrites.unresolved.contains({I->name, I->type})) {
            // The prefix cannot observe this and Object preparation has no
            // user callback. An effect-free initializer preserves allocation's
            // typed default before the original body, without a ConstantValue.
            Plan.default_instances.insert(*I);
            Plan.fields.insert(*I);
            I = Required.erase(I);
          } else
            ++I;
        }
    }
    if (!Static && !has(C.access, "interface") && !has(C.access, "enum") &&
        !has(C.access, "annotation") && independentSourceScope(C) &&
        C.superclass && *C.superclass != "Ljava/lang/Object;") {
      // Before the first super call, this has not escaped. A Java 25 prologue
      // may explicitly retain allocation's typed default there. In contrast,
      // an initializer after super would overwrite a callback's reflective
      // mutation; even reading and writing it back would introduce a race.
      bool HasPrimary = false, ProvenSuper = true;
      for (const auto &M : C.methods) {
        if (M.reference.name != "<init>")
          continue;
        if (Delegations.contains(M.reference))
          continue;
        HasPrimary = true;
        const auto *Call =
            M.instructions.empty()
                ? nullptr
                : std::get_if<MethodRef>(&M.instructions.front().reference);
        const auto *Target = Call ? member(*Call, Classes, B) : nullptr;
        const bool ImmediateSuper =
            Call && Call->owner == *C.superclass && Call->name == "<init>" &&
            Call->parameters.empty() && Call->returns == "V" && Target &&
            Target->reference == *Call && !has(Target->access, "static") &&
            (M.instructions.front().opcode == "invoke-direct" ||
             M.instructions.front().opcode == "invoke-direct/range") &&
            M.registers >= M.incomingWords() &&
            M.instructions.front().registers ==
                std::vector<unsigned>{M.registers - M.incomingWords()};
        // A typed superclass receipt owns the complete argument/origin
        // relation. Compose it with Body's existing uninitialized-receiver
        // and prefix-effect checks instead of repeating that prefix analysis.
        const bool TypedSuper = std::any_of(
            M.instructions.begin(), M.instructions.end(), [&](const auto &Op) {
              const auto *Ref = std::get_if<MethodRef>(&Op.reference);
              return Ref && Ref->owner == *C.superclass &&
                     Generics.superclass_calls.contains({M.reference, *Ref});
            });
        // The shared guarded-prefix receipt proves that its original
        // preparation cannot observe or escape uninitialized this. Generic
        // argument binding remains a separate mandatory Body check; a plain
        // descriptor target does not need a generic superclass-call receipt.
        const auto Guard =
            Generics.constructor_super_prologues.find(M.reference);
        const auto *GuardTarget =
            Guard == Generics.constructor_super_prologues.end()
                ? nullptr
                : member(Guard->second.target, Classes, B);
        const bool GuardedSuper =
            Guard != Generics.constructor_super_prologues.end() &&
            Guard->second.target.owner == *C.superclass && GuardTarget &&
            GuardTarget->reference == Guard->second.target &&
            !has(GuardTarget->access, "static");
        ProvenSuper &= ImmediateSuper || TypedSuper || GuardedSuper;
      }
      if (HasPrimary && ProvenSuper)
        for (auto I = Required.begin(); I != Required.end();) {
          if (allocationDefaultLiteral(I->type) &&
              !InstanceWrites.fields.contains(*I) &&
              !InstanceWrites.unresolved.contains({I->name, I->type})) {
            Plan.pre_super_defaults.insert(*I);
            Plan.fields.insert(*I);
            I = Required.erase(I);
          } else
            ++I;
        }
    }
    if (Static &&
        (std::none_of(
             C.methods.begin(), C.methods.end(),
             [](const Method &M) { return M.reference.name == "<clinit>"; }) ||
         std::count_if(C.methods.begin(), C.methods.end(), [](const Method &M) {
           return M.reference.name == "<clinit>" &&
                  M.reference.parameters.empty() &&
                  M.reference.returns == "V" && has(M.access, "static") &&
                  !M.instructions.empty();
         }) == 1)) {
      for (auto I = Required.begin(); I != Required.end();) {
        B.tick();
        if (!StaticWrites.fields.contains(*I) &&
            !StaticWrites.unresolved.contains({I->name, I->type})) {
          // Missing DEX static values denote the type's zero/null default.
          // No known or unresolved store may target this field. Without an
          // original clinit, the same nonconstant default expression retains
          // runtime reads; its generated initializer is disclosed separately.
          Plan.default_statics.insert(*I);
          Plan.fields.insert(*I);
          I = Required.erase(I);
        } else
          ++I;
      }
    }
    if (Required.empty())
      continue;
    bool Found = false;
    for (const auto &M : C.methods) {
      if (M.reference.name != (Static ? "<clinit>" : "<init>"))
        continue;
      if (auto I = Enums.methods.find(M.reference);
          I != Enums.methods.end() && I->second.compiler_managed)
        continue;
      Found = true;
      if (!Static && Delegations.contains(M.reference)) {
        // The shared delegation proof reaches a primary constructor without
        // touching uninitialized this. Check that constructor for every final.
        for (const auto &Op : M.instructions)
          if (const auto *Field = std::get_if<FieldRef>(&Op.reference);
              Field && starts(Op.opcode, "iput") && Required.contains(*Field))
            javaError(
                "delegating constructor rewrites an initialized final field");
        continue;
      }
      if (auto I = Enums.methods.find(M.reference);
          I != Enums.methods.end() && I->second.kind == "enum-constructor" &&
          I->second.target && I->second.target->owner == C.name)
        continue; // The proven pure this-chain reaches a checked primary
                  // constructor.
      finalWriteRegions(M, Required, B);
      Plan.writes[M.reference].insert(Required.begin(), Required.end());
    }
    if (!Found) {
      const auto &Field = *Required.begin();
      javaError("final field " + Field.owner + "->" + Field.name +
                " has no verified initialization expression");
    }
    Plan.fields.insert(Required.begin(), Required.end());
  }
  return Plan;
}

const MethodRef &
sourceBridgeReference(const MethodRef &original,
                      const GenericSignaturePlans &generics,
                      const GenericInheritancePlan &inheritance) {
  if (const auto found =
          generics.boxed_superclass_source_results.find(original);
      found != generics.boxed_superclass_source_results.end())
    return found->second;
  if (const auto found = inheritance.retained_plain_bridges.find(original);
      found != inheritance.retained_plain_bridges.end() &&
      found->second.source_reference)
    return *found->second.source_reference;
  return original;
}

class Body {
  const Method &method;
  const ClassMap &classes;
  Budget &budget;
  const JavaTypeNames &type_names;
  const GenericSignaturePlans &generics;
  const LocalProjection &local_projection;
  const GenericInheritancePlan &inheritance;
  const std::map<std::string, std::string> &anonymous_sources;
  const std::set<FieldRef> final_writes;
  const std::set<FieldRef> pre_super_defaults;
  const EnumMethodProjection *enum_projection;
  const MethodRef *local_closure = nullptr;
  const std::vector<Instruction> &code;
  std::map<uint32_t, size_t> by_pc;
  std::map<uint32_t, State> states;
  std::map<MethodRef, std::string> invocation_witnesses;
  std::map<MethodRef, std::string> raw_declaring_receivers;
  std::string prefix;
  unsigned minimum_java_release = 8;
  bool has_constructor_prologue = false;
  bool verifying_constructor_arguments = false;
  // Active only while verifying a constructor prefix. Record actual register
  // writes through the ordinary operation engine, including wide invalidation.
  std::set<unsigned> *constructor_prefix_writes = nullptr;
  struct RegisterWriteObservation {
    const State *state = nullptr;
    std::set<unsigned> *words = nullptr;
  };
  RegisterWriteObservation register_write_observation;
  bool prefix_local_declarations = false;
  std::map<unsigned, std::pair<std::string, std::string>> constructor_values;
  std::set<uint32_t> nonlinear_entries;
  std::optional<std::pair<uint32_t, std::string>> static_super_helper;
  std::optional<MethodRef> erased_super_helper_target;
  size_t first = 0;
  std::map<uint32_t, std::string> pending_results;
  std::map<uint32_t, uint32_t> constructors;
  std::map<uint32_t, std::string> abrupt_argument_allocations;
  std::map<uint32_t, std::map<unsigned, uint32_t>> allocation_reads;
  std::set<uint32_t> deferred_allocation_reads;
  struct AllocationValue {
    std::string kind, type;
    size_t instruction = 0;
    // Original incoming or result register, independently of the call's
    // receiver/argument words and the producer's instruction position.
    unsigned register_index = 0;
    std::vector<size_t> arguments;
    std::optional<size_t> initializer;
  };
  struct AllocationRegion {
    uint32_t start = 0, end = 0;
    std::vector<AllocationValue> values;
    std::map<unsigned, size_t> live;
    std::set<unsigned> written;
    std::vector<uint32_t> effects;
    // Each differing exception edge retains its pre-instruction value graph.
    // The converged verifier must prove its copy adapter reaches the same
    // handler tail with every observed word equal to the allocation entry.
    std::map<uint32_t, std::map<unsigned, size_t>> handler_inputs;
  };
  std::map<uint32_t, AllocationRegion> allocation_regions;
  std::map<uint32_t, uint32_t> allocation_region_owners;
  struct ForkedAllocation {
    size_t branch;
    AllocationRegion fallthrough, taken;
  };
  std::map<uint32_t, ForkedAllocation> forked_allocations;
  std::map<uint32_t, uint32_t> forked_allocation_owners;
  std::set<uint32_t> forward_allocations;
  struct ConditionalAllocation {
    size_t begin, end;
    std::set<size_t> members;
  };
  std::map<uint32_t, ConditionalAllocation> conditional_allocations;
  std::map<uint32_t, uint32_t> conditional_allocation_owners;
  struct ConstructorChoice {
    size_t branch, first, second, join;
    AllocationRegion cleanup;
  };
  std::map<uint32_t, ConstructorChoice> constructor_choices;
  std::map<uint32_t, uint32_t> constructor_choice_owners;
  bool emitting = false;
  // Emission-only capture of an already verified final store's canonical RHS.
  std::optional<std::string> final_assignment_target;
  bool allocationInitializer(const Instruction &New, const Instruction &Init);
  std::optional<size_t> allocationReceiverTail(const Instruction &New,
                                               size_t End);
  bool planConstructorChoice(size_t Index);
  bool planForwardAllocation(size_t Index);
  bool planConditionalAllocation(size_t Index, unsigned Depth = 0);
  std::string conditionalAllocationArgument(const ConditionalAllocation &Plan,
                                            const std::string &Result);
  struct MonitorRegion {
    size_t enter, cleanup_exit;
    unsigned object;
    std::set<size_t> body, exits, cleanup;
    std::set<size_t> handler_entries;
    bool dispatch_handlers = false;
  };
  std::map<size_t, MonitorRegion> monitors;
  std::map<size_t, size_t> monitor_release_owners;
  bool monitor_identities_verified = false;
  bool method_synchronized = false;
  std::optional<uint32_t> active_instruction;
  void planMonitor();
  void validateStaticFinalMonitors();

  static std::string allocationLocal(uint32_t Root, size_t Value) {
    return "__neverdAllocation" + std::to_string(Root) + "_" +
           std::to_string(Value);
  }
  std::string intersectionArgumentType(
      const Instruction &Op, const State &Incoming,
      const GenericSignaturePlans::IntersectionInvocation &Contract,
      const MethodRef &Declaration);
  std::string intersectionArgument(const Instruction &Op, const State &Incoming,
                                   const MethodRef &Declaration,
                                   size_t Parameter, std::string Value);
  bool planAbruptAllocationArgument(size_t index, const std::string &type);
  void planNestedAllocation(size_t Index);
  bool planForkedAllocation(size_t index);
  AllocationRegion buildAllocationPath(size_t index, size_t limit, size_t branch,
                                       size_t arm);
  static void declareAllocationValues(Lines &out, const AllocationRegion &plan,
                                      bool captureInputs);
  void verifyAllocationHandlers(const AllocationRegion &Plan);
  std::vector<std::string> emitNestedAllocation(const AllocationRegion &Plan);

  // This domain records origins, independently of the verifier's type sets.
  // Only admitted producers introduce a local or exception object. Joins are
  // bitwise unions, and exceptional edges retain the pre-instruction origins.
  enum Origin : uint8_t { Scalar = 1, Local = 2, Exception = 4, Null = 8 };
  using Origins = std::vector<uint8_t>;
  std::map<uint32_t, Origins> origins;

  [[noreturn]] void fail(const std::string &Message) const {
    if (local_closure) {
      auto I = classes.find(method.reference.owner);
      if (I != classes.end())
        javaError(I->second.name + " [" + I->second.source_id + "] enclosing " +
                  local_closure->identity() + ": " +
                  method.reference.identity() + ": " + Message);
    }
    javaError(method.reference.identity() + ": " + Message);
  }
  const Class &ownerClass() const {
    auto I = classes.find(method.reference.owner);
    if (I == classes.end())
      fail("method owner has no local declaration");
    return I->second;
  }
  std::string sourceType(const std::string &Type) const {
    return type_names.render(Type, JavaScope{ownerClass(), &method.reference});
  }
  std::string allocationExpression(const std::string &Type,
                                   const std::vector<std::string> &Args) const {
    if (const auto Anonymous = anonymous_sources.find(Type);
        Anonymous != anonymous_sources.end())
      return Anonymous->second;
    return "new " + sourceType(Type) + "(" + join(Args, ", ") + ")";
  }
  bool concreteAllocationClass(const std::string &Type) const {
    const auto C = classes.find(Type);
    if (C == classes.end() || has(C->second.access, "interface") ||
        has(C->second.access, "abstract"))
      return false;
    // Both expression planners consume the existing source-scope receipts.
    // Captured members retain their explicit outer parameter and field;
    // named anonymous members retain their validated source alias. Neither
    // planner infers or synthesizes an enclosing receiver.
    return (independentSourceScope(C->second) && !isAnonymous(C->second)) ||
           local_projection.by_class.contains(Type) ||
           generics.captured_inner_classes.contains(Type) ||
           type_names.namedAnonymous(C->second) ||
           anonymous_sources.contains(Type);
  }
  const std::string &referenceType(const Instruction &Op) const {
    if (auto *T = std::get_if<std::string>(&Op.reference)) {
      const auto Kind = baseOpcode(Op.opcode);
      if (Kind == "check-cast" || Kind == "instance-of") {
        if (Op.opcode != Kind)
          fail("reference type check has an unsupported encoding");
        if (!isReference(*T))
          fail("reference type check requires a reference descriptor");
      }
      return *T;
    }
    fail("instruction has no type descriptor");
  }
  uint32_t target(const Instruction &Op) const {
    const auto Target = branchTarget(Op);
    if (!Target)
      fail("branch has no destination");
    return *Target;
  }
  const std::vector<Handler> &handlers(uint32_t PC) const {
    for (const auto &R : method.tries)
      if (R.start <= PC && PC < R.end)
        return R.handlers;
    static const std::vector<Handler> Empty;
    return Empty;
  }
  bool provenMonitorRelease(const Instruction &Op) const {
    return monitor_identities_verified && Op.opcode == "monitor-exit" &&
           monitor_release_owners.contains(by_pc.at(Op.pc));
  }
  void validateShape();
  const Method *validateInvocation(const Instruction &Op);
  bool protectedVirtualFamily(const Method &Declaration) const;
  std::string protectedInvocationOwner(const Instruction &Op,
                                       const Method &Declaration,
                                       const State &Incoming, bool Strict);
  bool expressionMemberAccessible(const MethodRef &Ref,
                                  const Method &Declaration) const;
  bool expressionSuperAccessible(const Instruction &Op,
                                 const Method &Declaration) const;
  std::string finalFieldSourceValue(const FieldRef &Ref,
                                    std::string Value) const {
    const auto I = generics.fields.find(Ref);
    if (I == generics.fields.end())
      return Value;
    const auto &Signature = I->second;
    if (!Signature.field_type ||
        Signature.types.at(*Signature.field_type).erasure != Ref.type)
      fail("final field source cast changes its original erasure");
    // Structured writes retain the full field declaration. Scoped rendering
    // rejects unavailable formals; the cast retains the original carrier and
    // adds no narrower runtime check or inspection of generic contents.
    return "((" +
           type_names.render(Signature, *Signature.field_type,
                             JavaScope{ownerClass(), &method.reference}) +
           ") " + Value + ")";
  }
  std::string lexicalMemberArgument(const std::string &Owner,
                                    const GenericSignature &Contract,
                                    GenericTypeId Id,
                                    const std::string &Descriptor,
                                    std::string Value) const {
    const auto &Type = Contract.types.at(Id);
    const auto declaration = classes.find(Owner);
    if (declaration == classes.end() || !isAnonymousLocal(declaration->second) ||
        (generics.classes.contains(Owner) &&
         !generics.classes.at(Owner).type_parameters.empty()) ||
        Type.variable_owner == Owner ||
        !type_names.visibleFormal(Type, JavaScope{ownerClass(), &method.reference}))
      return Value;
    if (Type.erasure != Descriptor)
      fail("local member source argument changes its original erasure");
    // A named local with no own formals is not a raw view of its enclosing
    // class's formals. Fields and calls retain those lexical source types.
    // Scoped rendering proves visibility/shadowing; this cast has exactly the
    // original carrier erasure and introduces no narrower runtime check.
    return "((" +
           type_names.render(Contract, Id,
                             JavaScope{ownerClass(), &method.reference}) +
           ") " + Value + ")";
  }
  std::string constructorArgument(const MethodRef &Ref, size_t Parameter,
                                  std::string Value) {
    if (Ref.name != "<init>")
      return Value;
    const auto *shape = sourceMethodSignature(Ref, generics);
    const auto local = local_projection.by_class.find(Ref.owner);
    if (!shape || !shape->type_parameters.empty() ||
        (local == local_projection.by_class.end() &&
         !generics.constructor_scoped_static_locals.contains(Ref.owner)))
      return Value;
    const auto &cls = classes.at(Ref.owner);
    if (!cls.enclosing_method)
      return Value;
    size_t slot = Parameter;
    if (const auto mapping = generics.parameter_indices.find(Ref);
        mapping != generics.parameter_indices.end()) {
      const auto pos = std::find(mapping->second.begin(), mapping->second.end(), Parameter);
      if (pos == mapping->second.end())
        return Value;
      slot = pos - mapping->second.begin();
    }
    if (slot >= shape->parameters.size())
      return Value;
    const auto Id = shape->parameters[slot];
    const auto &Type = shape->types.at(Id);
    // A nested local can capture a formal several declarations out. Use the
    // same lexical lookup as the constructor header, including shadowing.
    const auto *formal = &Type;
    while (formal->element)
      formal = &shape->types.at(*formal->element);
    if (formal->variable_owner == Ref.owner ||
        !type_names.visibleFormal(*formal, JavaScope{ownerClass(), &method.reference}))
      return Value;
    if (Type.erasure != Ref.parameters.at(Parameter))
      fail("local constructor source argument changes its original erasure");
    if (!generics.constructor_scoped_static_locals.contains(Ref.owner) &&
        std::count_if(cls.methods.begin(), cls.methods.end(),
                      [&](const Method &candidate) {
                        budget.tick();
                        return candidate.reference.name == "<init>" &&
                               (candidate.reference.parameters.size() ==
                                    Ref.parameters.size() ||
                                has(candidate.access, "varargs"));
                      }) != 1)
      return Value;
    // The local declaration keeps its enclosing method formal. Its cast has
    // exactly the already verified runtime erasure; it adds no narrower check
    // and evaluates the original argument once.
    return "((" +
           type_names.render(*shape, Id,
                             JavaScope{ownerClass(), &method.reference}) +
           ") " + Value + ")";
  }
  std::string superclassArgument(const MethodRef &Ref, size_t Parameter,
                                 std::string Value) const {
    const auto Proof = generics.superclass_calls.find({method.reference, Ref});
    if (Proof == generics.superclass_calls.end() ||
        !isReference(Ref.parameters[Parameter]))
      return Value;
    const auto &Contract = Proof->second;
    return "((" +
           type_names.render(Contract, Contract.parameters.at(Parameter),
                             JavaScope{ownerClass(), &method.reference}) +
           ") " + Value + ")";
  }
  bool bindRawDeclaringReceiver(const MethodRef &ref, const Method &declaration);
  std::string virtualReceiver(const MethodRef &ref, const Instruction &op,
                              std::string receiver) const {
    // Arrays expose a public clone() with the array's own runtime result.
    // DEX can qualify the invocation with Object, whose Java method is protected.
    if (ref.owner == "Ljava/lang/Object;" && ref.name == "clone" &&
        ref.parameters.empty() && ref.returns == ref.owner &&
        baseOpcode(op.opcode) == "invoke-virtual") {
      const auto frame = states.find(op.pc);
      if (frame != states.end()) {
        auto types = frame->second.at(op.registers.at(0));
        types.erase("zero");
        types.erase("null");
        if (types.size() == 1 && types.begin()->starts_with('['))
          return "((" + sourceType(*types.begin()) + ") " + receiver + ")";
      }
    }
    const auto owner = raw_declaring_receivers.find(ref);
    if (owner == raw_declaring_receivers.end())
      return receiver;
    // The inner receiver still enforces the original symbolic qualifier. Its
    // upcast supplies javac with the verified raw member descriptor.
    return "((" + sourceType(owner->second) + ") " + receiver + ")";
  }
  std::optional<std::string>
  closedSuperclassMemberType(const std::string &receiver, const std::string &owner,
                             const GenericSignature &signature, GenericTypeId id) const {
    const auto cls = classes.find(receiver);
    const auto child = generics.classes.find(receiver);
    const auto parent = generics.classes.find(owner);
    if (cls == classes.end() || cls->second.superclass != owner ||
        child == generics.classes.end() || !child->second.superclass ||
        parent == generics.classes.end())
      return {};
    const auto &edge = child->second.types.at(*child->second.superclass);
    const auto &formals = parent->second.type_parameters;
    if (edge.segments.size() != 1 || edge.segments[0].arguments.size() != formals.size())
      return {};
    const auto *variable = &signature.types.at(id);
    std::string rank;
    while (variable->element) {
      rank += '[';
      variable = &signature.types.at(*variable->element);
    }
    if (variable->kind != GenericType::Kind::TypeVariable || variable->variable_owner != owner)
      return {};
    for (size_t i = 0; i < formals.size(); ++i) {
      const auto &arg = edge.segments[0].arguments[i];
      if (formals[i].name != variable->variable_name || !arg.type ||
          arg.variance != GenericTypeArgument::Variance::Invariant)
        continue;
      const auto &bound = child->second.types.at(*arg.type);
      if (bound.kind == GenericType::Kind::Class && bound.segments.size() == 1 &&
          bound.segments[0].arguments.empty())
        return rank + bound.erasure;
    }
    return {};
  }
  std::string fieldReceiver(const FieldRef &ref, const Instruction &op,
                            std::string receiver) const {
    if (ref.owner == method.reference.owner ||
        packageOf(ref.owner) == packageOf(method.reference.owner))
      return receiver;
    const auto state = states.find(op.pc);
    if (state == states.end())
      return receiver;
    const auto &types = state->second.at(op.registers.at(1));
    if (types != Strings{"self:" + method.reference.owner} &&
        types != Strings{method.reference.owner})
      return receiver;
    const auto *field = member(ref, classes, budget);
    if (!field || !has(field->access, "protected") ||
        !isAssignableReference(method.reference.owner, field->reference.owner,
                               classes, budget))
      return receiver;
    auto lexical = ref;
    lexical.owner = method.reference.owner;
    // A cross-package protected field is accessible on the lexical subclass.
    // Check for hiding before qualifying the original receiver with that type.
    if (lookupField(lexical, classes, budget).declaration != field)
      return receiver;
    return "((" + sourceType(lexical.owner) + ") " + receiver + ")";
  }
  bool fixedArgumentExcludes(std::string actual, std::string other) const;
  bool bindInvocationFormals(const MethodRef &ref, const Method &declaration,
                           const GenericSignature &signature);
  std::string invocationName(const MethodRef &ref, const Method *declaration) {
    auto name = sourceMethodName(declaration ? declaration->reference : ref,
                                 inheritance);
    const auto witness = invocation_witnesses.find(ref);
    return witness == invocation_witnesses.end() ? name
                                                       : witness->second + name;
  }
  std::string invocationName(const MethodRef &ref) {
    return invocationName(ref, member(ref, classes, budget, &generics));
  }
  std::string
  invocationArguments(const MethodRef &ref,
                      const std::vector<std::string> &values) const {
    if (!invocation_witnesses.contains(ref) && !raw_declaring_receivers.contains(ref))
      return join(values, ", ");
    // Expression programs (including constructor prefixes) can carry a typed
    // value or a null literal. Use the exact descriptor view used in lookup.
    std::vector<std::string> arguments;
    for (size_t i = 0; i < values.size(); ++i)
      arguments.push_back("((" + sourceType(ref.parameters.at(i)) + ") (" +
                          values[i] + "))");
    return join(arguments, ", ");
  }
  const Method &expressionInvocation(const Instruction &Op,
                                     std::string_view Context,
                                     bool AllowVoid = false);
  std::vector<uint32_t> successors(size_t Index) const;
  bool compatible(std::string Kind, const std::string &Wanted) const;
  std::string read(const State &S, unsigned Reg, const std::string &Type,
                   bool Strict, std::string Carrier = {}) const;
  std::string write(State &S, unsigned Reg, const std::string &Type,
                    const std::string &Expression,
                    bool AsExpression = false) const;
  std::string staticOwner(const std::string &Owner, bool Field = false);
  std::string staticFieldRead(const FieldRef &Ref);
  std::string arrayType(const State &S, unsigned Reg, bool Strict) const;
  std::string arrayCreation(const std::string &Type,
                            const std::string &Length) const;
  std::vector<std::string> arrayPayload(const Instruction &Op,
                                        const std::string &Type,
                                        const std::string &Address);
  std::pair<State, std::vector<std::string>>
  operation(const Instruction &Op, const State &Incoming, bool Strict,
            std::set<unsigned> *Written = nullptr);
  std::string
  condition(const Instruction &Op, const State &S, bool Strict,
            const std::function<std::string(unsigned, const std::string &)>
                &Reader = {}) const;
  State initial() const;
  void validateLocalOperations() const;
  Origins localOrigins(const Instruction &Op, const Origins &Incoming,
                       bool Strict) const;
  void verify();
  static std::string resultRead(const std::string &Type);
  static std::string resultWrite(const std::string &Type,
                                 const std::string &Value);
  std::string throwHelper() const {
    return helperName(ownerClass(), "__neverdThrow");
  }

public:
  Body(const Method &M, const ClassMap &C, Budget &B,
       const JavaTypeNames &TypeNames, const GenericSignaturePlans &Generics,
       const LocalProjection &Projection,
       const GenericInheritancePlan &Inheritance,
       const std::map<std::string, std::string> &AnonymousSources,
       std::set<FieldRef> FinalWrites = {},
       const EnumMethodProjection *EnumProjection = nullptr,
       std::set<FieldRef> PreSuperDefaults = {})
      : method(M), classes(C), budget(B), type_names(TypeNames),
        generics(Generics), local_projection(Projection),
        inheritance(Inheritance), anonymous_sources(AnonymousSources),
        final_writes(std::move(FinalWrites)),
        pre_super_defaults(std::move(PreSuperDefaults)),
        enum_projection(EnumProjection), code(M.instructions) {
    type_names.requireRuntimePackage(JavaScope{ownerClass(), &M.reference});
    if (auto I = Projection.by_class.find(M.reference.owner);
        I != Projection.by_class.end() && !isAnonymousLocal(*I->second))
      local_closure = &*I->second->enclosing_method;
    else if (Projection.scalar_closures.contains(M.reference))
      local_closure = &M.reference;
    for (size_t I = 0; I < code.size(); ++I)
      by_pc.emplace(code[I].pc, I);
    validateShape();
    planMonitor();
    if (!monitors.empty() && !final_writes.empty())
      validateStaticFinalMonitors();
    if (local_closure)
      validateLocalOperations();
  }
  std::string emit(std::string_view LocalDeclarations = {});
  Method staticSuperAccessor(const std::string &HelperName);
  std::string emitSuperHelper(const Method &helper) {
    Body emitter(helper, classes, budget, type_names, generics, local_projection,
                 inheritance, anonymous_sources);
    emitter.erased_super_helper_target = erased_super_helper_target;
    return emitter.emit();
  }
  void interfaceDefaultAccessor() {
    if (inheritance.inherited_interface_default_accessors.contains(
            method.reference)) {
      minimum_java_release = std::max(minimum_java_release, 9u);
      return;
    }
    const auto proof =
        inheritance.interface_default_accessors.find(method.reference);
    if (proof == inheritance.interface_default_accessors.end())
      fail("interface default accessor lacks its exact original receipt");
    static_super_helper = {
        code.front().pc,
        inheritance.interface_default_helpers.at(proof->second)};
    minimum_java_release = std::max(minimum_java_release, 9u);
  }
  std::vector<std::pair<FieldRef, std::string>> interfaceInitializers();
  unsigned minimumJavaRelease() const { return minimum_java_release; }
  bool sourceSynchronized() const { return method_synchronized; }
};

void Body::validateShape() {
  if (code.empty() || by_pc.size() != code.size())
    fail("missing or duplicate instruction positions");
  for (size_t I = 1; I < code.size(); ++I)
    if (code[I - 1].pc >= code[I].pc)
      fail("invalid method code boundaries");
  if (method.code_end <= code.back().pc)
    fail("invalid method code boundaries");
  if (method.registers > 1024)
    fail("register frame exceeds the bounded Java projection limit");
  if (method.incomingWords() > method.registers)
    fail("incoming arguments exceed the register frame");
  for (const auto &Op : code) {
    budget.tick();
    const auto Kind = baseOpcode(Op.opcode);
    if ((Kind == "move" || Kind == "move-object" || Kind == "move-wide") &&
        !registerCopyShape(Op))
      fail("move instruction has malformed encoding or metadata");
    for (unsigned R : Op.registers)
      if (R >= method.registers)
        fail("instruction register is outside its frame");
    std::vector<uint32_t> Targets;
    if (Op.opcode.find("switch") != std::string::npos)
      Targets = Op.targets;
    else if (anyPrefix(Op.opcode, {"goto", "if-"}))
      Targets.push_back(target(Op));
    for (uint32_t T : Targets)
      if (!by_pc.contains(T))
        fail("branch does not target an executable instruction");
    nonlinear_entries.insert(Targets.begin(), Targets.end());
    if (Op.opcode.find("switch") != std::string::npos &&
        (Op.keys.size() != Op.targets.size() ||
         std::set<int32_t>(Op.keys.begin(), Op.keys.end()).size() !=
             Op.keys.size()))
      fail("switch keys and destinations disagree");
  }
  uint32_t PreviousEnd = 0;
  for (const auto &R : method.tries) {
    if (!by_pc.contains(R.start) ||
        (!by_pc.contains(R.end) && R.end != method.code_end) ||
        R.start >= R.end || R.start < PreviousEnd || R.handlers.empty())
      fail("invalid or overlapping exception regions");
    PreviousEnd = R.end;
    for (size_t I = 0; I < R.handlers.size(); ++I) {
      const auto &H = R.handlers[I];
      nonlinear_entries.insert(H.target);
      if (!by_pc.contains(H.target) ||
          code[by_pc.at(H.target)].opcode != "move-exception")
        fail("exception handler must start with move-exception");
      if (!H.type && I + 1 != R.handlers.size())
        fail("catch-all must be the final exception handler");
    }
  }
  for (size_t I = 0; I < code.size(); ++I) {
    const auto &Op = code[I];
    if (!starts(Op.opcode, "move-result"))
      continue;
    if (!I || !anyPrefix(code[I - 1].opcode, {"invoke-", "filled-new-array"}))
      fail("move-result does not immediately follow its producer");
    std::string Type;
    if (auto *M = std::get_if<MethodRef>(&code[I - 1].reference))
      Type = M->returns;
    else if (auto *T = std::get_if<std::string>(&code[I - 1].reference))
      Type = *T;
    if (Type.empty() || Type == "V")
      fail("move-result has no value-producing signature");
    std::string Wanted = isReference(Type)      ? "move-result-object"
                         : wordWidth(Type) == 2 ? "move-result-wide"
                                                : "move-result";
    if (Op.opcode != Wanted)
      fail("move-result carrier disagrees with the native prototype");
    pending_results[Op.pc] = Type;
  }
  if (method.reference.name != "<init>")
    return;
  unsigned SelfReg = method.registers - method.incomingWords();
  std::map<unsigned, std::pair<std::string, std::string>> Original;
  Original[SelfReg] = {"self:" + method.reference.owner, "this"};
  unsigned Reg = SelfReg + 1;
  for (size_t I = 0; I < method.reference.parameters.size(); ++I) {
    const auto &T = method.reference.parameters[I];
    Original[Reg] = {T, enum_projection && I < 2
                            ? (I == 0 ? "super.name()" : "super.ordinal()")
                            : "arg" + std::to_string(I)};
    if (wordWidth(T) == 2)
      Original[Reg + 1] = {"wide-high", ""};
    Reg += wordWidth(T);
  }
  auto prefixValue = [&](unsigned Register, const std::string &Wanted) {
    auto Found = Original.find(Register);
    if (Found == Original.end() || starts(Found->second.first, "self:"))
      fail("constructor prefix requires unavailable argument evaluation");
    const auto &[Actual, Expression] = Found->second;
    if (!compatible(Actual, Wanted) ||
        (wordWidth(Wanted) == 2 &&
         (!Original.contains(Register + 1) ||
          Original.at(Register + 1).first != "wide-high")))
      fail("constructor prefix argument types disagree");
    if (isReference(Wanted))
      return Actual == "zero"
                 ? std::string("null")
                 : "((" + sourceType(Wanted) + ") " + Expression + ")";
    if (Actual == Wanted)
      return Expression;
    if (Wanted == "F")
      return "((java.lang.Float) null).intBitsToFloat(" + Expression + ")";
    if (Wanted == "D")
      return "((java.lang.Double) null).longBitsToDouble(" + Expression + ")";
    if (Wanted == "J")
      return Expression;
    auto Integer = Actual == "Z" ? "(" + Expression + " ? 1 : 0)" : Expression;
    if (Wanted == "Z")
      return "(" + Integer + " != 0)";
    if (Wanted == "I")
      return Integer;
    return "((" + sourceType(Wanted) + ") " + Integer + ")";
  };
  auto prefixWrite = [&](unsigned Register, std::string Type,
                         std::string Expression) {
    unsigned Words = isWide(Type) ? 2 : 1;
    if (Words > method.registers - Register)
      fail("constructor prefix wide write exceeds its frame");
    for (unsigned Word = Register; Word < Register + Words; ++Word) {
      if (auto Old = Original.find(Word); Old != Original.end()) {
        if (isWide(Old->second.first))
          Original.erase(Word + 1);
        else if (Old->second.first == "wide-high" && Word)
          Original.erase(Word - 1);
      }
      Original.erase(Word);
    }
    Original[Register] = {std::move(Type), std::move(Expression)};
    if (Words == 2)
      Original[Register + 1] = {"wide-high", ""};
  };
  auto prefixConstant = [&](const Instruction &Op) {
    const auto Constant = constructorPrefixIntegerConstant(method, Op);
    if (!Constant)
      fail("constructor prefix has an invalid integer constant");
    prefixWrite(Constant->destination, Constant->type,
                hexLiteral(uint64_t(Constant->bits),
                           Constant->type == "bits64" ? 64 : 32));
  };
  auto prefixCopy = [&](const Instruction &Op, bool Argument = false) {
    const auto Copy = constructorPrefixCopy(method, Op);
    if (!Copy || !Original.contains(Copy->source))
      fail("constructor prefix has an undefined move source");
    auto [Type, Expression] = Original.at(Copy->source);
    const auto &Kind = Copy->kind;
    if ((Kind == "move-object" && !isReference(Type) && Type != "zero") ||
        (Kind == "move-wide" && !isWide(Type)) ||
        (Kind == "move" &&
         (isReference(Type) || isWide(Type) || Type == "wide-high")) ||
        !Strings{"move", "move-object", "move-wide"}.contains(Kind))
      fail("constructor prefix move kind disagrees with its source");
    if (isWide(Type) && (!Original.contains(Copy->source + 1) ||
                         Original.at(Copy->source + 1).first != "wide-high"))
      fail("constructor prefix has a broken wide source");
    if (Argument && starts(Type, "self:"))
      fail("constructor allocation argument exposes uninitialized this");
    prefixWrite(Copy->destination, std::move(Type), std::move(Expression));
  };
  size_t Delegation = 0;
  std::optional<std::string> ArgumentCall;
  std::vector<std::string> GuardedArguments;
  const auto CapturedDelegation =
      generics.constructor_delegations.find(method.reference);
  const auto SuperPrologue =
      generics.constructor_super_prologues.find(method.reference);
  const bool GuardedSuper =
      SuperPrologue != generics.constructor_super_prologues.end() &&
      (SuperPrologue->second.requires_dispatch || !pre_super_defaults.empty());
  const auto PrefixSelection =
      !GuardedSuper &&
              CapturedDelegation != generics.constructor_delegations.end()
          ? selectConstructorPrefixDelegation(method, budget)
          : std::optional<ConstructorPrefixDelegation>{};
  const auto CapturedCall =
      !GuardedSuper &&
              CapturedDelegation == generics.constructor_delegations.end()
          ? code.end()
          : std::find_if(code.begin(), code.end(), [&](const Instruction &Op) {
              const auto *Call = std::get_if<MethodRef>(&Op.reference);
              const bool Exact =
                  GuardedSuper
                      ? Op.pc == SuperPrologue->second.call_pc && Call &&
                            *Call == SuperPrologue->second.target
                      : Call && *Call == CapturedDelegation->second &&
                            (!PrefixSelection ||
                             Op.pc == code[PrefixSelection->call].pc);
              return Exact && (Op.opcode == "invoke-direct" ||
                               Op.opcode == "invoke-direct/range");
            });
  if (GuardedSuper && CapturedCall == code.end())
    fail("guarded superclass call disagrees with its shared proof");
  const bool GuardedDelegation =
      CapturedCall != code.end() &&
      ((GuardedSuper && SuperPrologue->second.requires_dispatch) ||
       (PrefixSelection ? PrefixSelection->requires_dispatch
                        : std::any_of(code.begin(), CapturedCall,
                                      [](const Instruction &Op) {
                                        return Op.target.has_value();
                                      })));
  if (GuardedDelegation) {
    // The shared constructor planner proves a single this/super call on every
    // normal path and prohibits use of uninitialized this. Use the ordinary
    // operation and type verifier for the prefix, in its own source variable
    // scope.
    Delegation = size_t(CapturedCall - code.begin());
    const auto &Head = code[Delegation];
    const auto *Target = std::get_if<MethodRef>(&Head.reference);
    if (!Target || *Target != (GuardedSuper ? SuperPrologue->second.target
                                            : CapturedDelegation->second))
      fail("guarded constructor delegation disagrees with its shared proof");
    llvm::SaveAndRestore<bool> PrefixArguments(verifying_constructor_arguments,
                                               true);
    std::set<uint32_t> PrefixAllocations;
    for (size_t At = 0; At < Delegation; ++At) {
      if (code[At].opcode != "new-instance")
        continue;
      const auto End = constructorPrefixAllocationEnd(method, At, budget);
      if (!End || *End >= Delegation)
        fail("constructor argument allocation lacks its closed shared proof");
      if (planConditionalAllocation(At)) {
        const auto &Plan = conditional_allocations.at(code[At].pc);
        if (Plan.end != *End)
          fail("conditional constructor argument disagrees with its shared "
               "initializer");
        PrefixAllocations.insert(code[At].pc);
      } else {
        planNestedAllocation(At);
        const auto &Plan = allocation_regions.at(code[At].pc);
        if (Plan.end != code[*End].pc)
          fail("constructor argument allocation disagrees with its exact "
               "initializer");
        PrefixAllocations.insert(Plan.start);
      }
      At = *End;
    }
    std::vector<std::optional<State>> Incoming(Delegation + 1);
    State Entry(method.registers, Strings{"?"});
    for (const auto &[Register, Value] : Original)
      Entry.at(Register) = Strings{Value.first};
    Incoming[0] = Entry;
    std::deque<size_t> Queue{0};
    std::set<size_t> Queued{0};
    std::set<unsigned> PrefixWrites;
    auto merge = [&](size_t At, const State &StateAt) {
      State Next = StateAt;
      if (Incoming[At])
        for (size_t R = 0; R < Next.size(); ++R)
          Next[R].insert((*Incoming[At])[R].begin(), (*Incoming[At])[R].end());
      if (Incoming[At] && *Incoming[At] == Next)
        return;
      Incoming[At] = std::move(Next);
      if (At < Delegation && Queued.insert(At).second)
        Queue.push_back(At);
    };
    auto prefixOperation = [&](const Instruction &Op, const State &Before) {
      if (baseOpcode(Op.opcode) == "move-object" && Op.registers.size() == 2 &&
          Before.at(Op.registers[1]) ==
              Strings{"self:" + method.reference.owner}) {
        State After = Before;
        After.at(Op.registers[0]) = Before.at(Op.registers[1]);
        if (constructor_prefix_writes)
          constructor_prefix_writes->insert(Op.registers[0]);
        // D8 may copy the uninitialized receiver into an invoke-range frame.
        // Retain the origin without evaluating this in a Java prologue.
        return std::pair<State, std::vector<std::string>>{std::move(After), {}};
      }
      return operation(Op, Before, true);
    };
    while (!Queue.empty()) {
      const auto At = Queue.front();
      Queue.pop_front();
      Queued.erase(At);
      budget.tick(method.registers + 1);
      if (!Incoming[At])
        fail("guarded constructor prefix has an unproven entry");
      const auto &Op = code[At];
      if (const auto *Call = std::get_if<MethodRef>(&Op.reference)) {
        const auto *Declaration =
            constructorPrefixValueCall(Op) &&
                    baseOpcode(Op.opcode) == "invoke-direct"
                ? &expressionInvocation(Op, "constructor prefix private call")
                : validateInvocation(Op);
        if (!Declaration ||
            !constructorPrefixThrowsSupported(*Declaration, method, generics))
          fail("guarded constructor getter requires a checked-exception proof");
      }
      State Outgoing;
      {
        llvm::SaveAndRestore<std::set<unsigned> *> Writes(
            constructor_prefix_writes, &PrefixWrites);
        Outgoing = prefixOperation(Op, *Incoming[At]).first;
      }
      if (starts(Op.opcode, "if-"))
        condition(Op, *Incoming[At], true);
      for (auto PC : successors(At)) {
        const auto Next = by_pc.at(PC);
        if (Next > Delegation)
          fail("guarded constructor prefix escapes its shared region");
        merge(Next, Outgoing);
      }
    }
    for (size_t At = 0; At < Delegation; ++At)
      if (!Incoming[At])
        fail("guarded constructor prefix has an unproven entry");
    for (size_t At = 0; At < Delegation; ++At)
      states[code[At].pc] = *Incoming[At];
    if (!Incoming[Delegation])
      fail("guarded constructor call has no verified entry");
    const bool HasContinuation = GuardedSuper || Delegation + 2 != code.size();
    if (HasContinuation)
      for (unsigned Register : PrefixWrites) {
        const auto Old = Original.find(Register);
        if (Old == Original.end() ||
            std::find(Head.registers.begin(), Head.registers.end(), Register) !=
                Head.registers.end())
          continue;
        // An overwritten input is no longer its original parameter. Drop
        // both words of any stale wide binding; proved scalar exports below
        // may establish a new value. Ordinary continuation verification rejects
        // reads of words that have neither an export nor a later definition.
        if (isWide(Old->second.first))
          Original.erase(Register + 1);
        else if (Old->second.first == "wide-high" && Register)
          Original.erase(Register - 1);
        Original.erase(Register);
      }
    std::map<unsigned, std::pair<std::string, std::string>> ScalarExports;
    if (HasContinuation)
      for (unsigned R : PrefixWrites) {
        budget.tick();
        if (std::find(Head.registers.begin(), Head.registers.end(), R) !=
            Head.registers.end())
          continue;
        const auto &Types = Incoming[Delegation]->at(R);
        static const Strings IntegerWords{"zero", "bits32", "I", "Z",
                                          "B",    "C",      "S"};
        if (Types.empty() ||
            !std::all_of(Types.begin(), Types.end(), [&](const auto &Type) {
              return IntegerWords.contains(Type);
            }))
          continue;
        if (Types == Strings{"zero"}) {
          prefixWrite(R, "zero", "0");
          continue;
        }
        const bool Raw =
            std::all_of(Types.begin(), Types.end(), [](const auto &Type) {
              return Type == "zero" || Type == "bits32";
            });
        const auto Name = "__neverdCtorWord" + std::to_string(R);
        ScalarExports.emplace(R, std::make_pair(Raw ? "bits32" : "I", Name));
        prefix += "int " + Name + " = 0;\n";
      }
    for (size_t I = 0; I < Target->parameters.size(); ++I) {
      const auto &Type = Target->parameters[I];
      GuardedArguments.push_back("__neverdCtorArg" + std::to_string(I));
      prefix += sourceType(Type) + " " + GuardedArguments.back() + " = " +
                (isReference(Type) ? "null"
                 : Type == "Z"     ? "false"
                                   : "0") +
                ";\n";
    }
    prefix += "{\n";
    for (unsigned R = 0; R < method.registers; ++R) {
      const auto Number = std::to_string(R);
      prefix += "int v" + Number + " = 0; long w" + Number +
                " = 0L; java.lang.Object o" + Number + " = null;\n";
    }
    for (auto Root : PrefixAllocations) {
      const auto Region = allocation_regions.find(Root);
      if (Region == allocation_regions.end())
        continue;
      for (size_t I = 0; I < Region->second.values.size(); ++I) {
        const auto &Value = allocation_regions.at(Root).values[I];
        const auto Name = allocationLocal(Root, I);
        if (Value.kind == "input")
          prefix += "java.lang.Object " + Name + "Object = null; long " + Name +
                    "Word = 0L; int " + Name + "Bits = 0;\n";
        else if (Value.kind != "constant" && Value.type != "V")
          prefix += isWide(Value.type) ? "long " + Name + " = 0L;\n"
                    : !isReference(Value.type)
                        ? "int " + Name + " = 0;\n"
                        : "java.lang.Object " + Name + " = null;\n";
      }
    }
    State Locals = Entry;
    unsigned Parameter = SelfReg + 1;
    for (size_t I = 0; I < method.reference.parameters.size(); ++I) {
      const auto &Type = method.reference.parameters[I];
      prefix +=
          write(Locals, Parameter, Type, "arg" + std::to_string(I)) + "\n";
      Parameter += wordWidth(Type);
    }
    prefix +=
        "int result32 = 0; long result64 = 0L; java.lang.Object resultObject = "
        "null;\nint pc = 0;\nint __neverdCtorPc = " +
        std::to_string(code.front().pc) +
        ";\n__neverdCtorPrefix: while (true) {\nswitch (__neverdCtorPc) {\n";
    for (size_t At = 0; At < Delegation; ++At) {
      const auto &Op = code[At];
      if (const auto Owner = conditional_allocation_owners.find(Op.pc);
          Owner != conditional_allocation_owners.end()) {
        const auto &Plan = conditional_allocations.at(Owner->second);
        if (At != Plan.begin && At != Plan.end)
          continue;
      }
      prefix += "case " + std::to_string(Op.pc) + ": {\n";
      std::vector<std::string> Statements;
      {
        llvm::SaveAndRestore<bool> Emit(emitting, true);
        Statements = prefixOperation(Op, *Incoming[At]).second;
      }
      if (auto Region = allocation_region_owners.find(Op.pc);
          Region != allocation_region_owners.end()) {
        const auto &Plan = allocation_regions.at(Region->second);
        if (Op.pc != Plan.start && Op.pc != Plan.end)
          Statements.clear();
        // Prefix dispatch uses its own program counter and has no handlers.
        if (Op.pc == Plan.end) {
          const auto Expected = "pc = " + std::to_string(Plan.start) + ";";
          if (Statements.empty() || Statements.front() != Expected)
            fail("constructor allocation lost its original dispatch marker");
          Statements.erase(Statements.begin());
        }
      }
      for (const auto &Statement : Statements)
        prefix += Statement + "\n";
      prefix += "__neverdCtorPc = ";
      const auto Conditional = conditional_allocations.find(Op.pc);
      if (starts(Op.opcode, "if-"))
        prefix += "(" + condition(Op, *Incoming[At], true) + ") ? " +
                  std::to_string(*Op.target) + " : " +
                  std::to_string(code[At + 1].pc);
      else if (Conditional != conditional_allocations.end())
        prefix += std::to_string(code[Conditional->second.end].pc);
      else {
        const auto Edges = successors(At);
        if (Edges.size() != 1)
          fail("guarded constructor prefix lost its unique successor");
        prefix += std::to_string(Edges[0]);
      }
      prefix += ";\ncontinue __neverdCtorPrefix;\n}\n";
    }
    prefix += "case " + std::to_string(Head.pc) +
              ": break __neverdCtorPrefix;\n"
              "default: throw new java.lang.AssertionError(\"Invalid recovered "
              "constructor flow\");\n"
              "}\n}\n";
    size_t Word = 1;
    for (size_t I = 0; I < Target->parameters.size(); ++I) {
      const auto &Type = Target->parameters[I];
      if (Word >= Head.registers.size())
        fail("guarded constructor argument frame is incomplete");
      const auto Register = Head.registers[Word];
      const auto &Value = GuardedArguments[I];
      prefix += Value + " = " +
                read(*Incoming[Delegation], Register, Type, true) + ";\n";
      // A call consumes a typed argument without changing the original word.
      // Retain the converged literal zero for later scalar/null consumers;
      // exporting its reference parameter type would invent a nominal value.
      if (!Original.contains(Register) || PrefixWrites.contains(Register)) {
        const bool Zero = Incoming[Delegation]->at(Register) == Strings{"zero"};
        prefixWrite(Register, Zero ? "zero" : Type, Zero ? "0" : Value);
      }
      // An unchanged incoming word still denotes its original parameter.
      // Passing a child through a parent view consumes that value without
      // widening the register or losing later child-only field access.
      // Actual prefix writes retain the converged export above.
      Word += wordWidth(Type);
    }
    if (Head.registers.empty() || Incoming[Delegation]->at(Head.registers[0]) !=
                                      Strings{"self:" + method.reference.owner})
      fail("guarded constructor lost its exact uninitialized receiver");
    Original[Head.registers[0]] = {"self:" + method.reference.owner, "this"};
    for (const auto &[R, Export] : ScalarExports) {
      prefix += Export.second + " = " +
                read(*Incoming[Delegation], R, "I", true) + ";\n";
      // Keep raw one-word constants usable as either int or floating bits;
      // typed integer values retain their ordinary verifier interpretation.
      prefixWrite(R, Export.first, Export.second);
    }
    prefix += "}\n";
    for (auto Root : PrefixAllocations) {
      if (const auto Conditional = conditional_allocations.find(Root);
          Conditional != conditional_allocations.end()) {
        constructors.erase(code[Conditional->second.end].pc);
        for (size_t At : Conditional->second.members)
          conditional_allocation_owners.erase(code[At].pc);
        conditional_allocations.erase(Conditional);
        continue;
      }
      const auto End = allocation_regions.at(Root).end;
      for (auto I = allocation_region_owners.begin();
           I != allocation_region_owners.end();) {
        if (I->second == Root)
          I = allocation_region_owners.erase(I);
        else
          ++I;
      }
      constructors.erase(End);
      allocation_regions.erase(Root);
    }
    states.clear();
    minimum_java_release = 25;
  }
  for (; Delegation < code.size(); ++Delegation) {
    budget.tick();
    const auto &Op = code[Delegation];
    if (const auto *R = std::get_if<MethodRef>(&Op.reference);
        R && R->name == "<init>")
      break;
    if (Op.target || !Op.targets.empty())
      fail("constructor prefix control flow requires a separate initialization "
           "proof");
    if (Op.opcode == "nop" && Op.registers.empty())
      continue;
    if (anyPrefix(Op.opcode, {"const/", "const-wide"}) ||
        Op.opcode == "const") {
      prefixConstant(Op);
    } else if (Op.opcode == "const-class") {
      const auto Type = constructorPrefixClassLiteral(method, Op);
      if (!Type)
        fail("constructor prefix class literal requires an exact reference "
             "type and a fresh local register");
      const auto Value = "__neverdCtorClass" + std::to_string(Op.pc);
      // Keep resolution at its original position even if the constructor
      // ignores this argument. A class literal does not initialize its target.
      prefix +=
          "java.lang.Class " + Value + " = " + sourceType(*Type) + ".class;\n";
      prefixWrite(Op.registers[0], "Ljava/lang/Class;", Value);
      minimum_java_release = 25;
    } else if (Op.opcode == "const-string" ||
               Op.opcode == "const-string/jumbo") {
      const auto *Text = std::get_if<std::string>(&Op.literal);
      if (Op.registers.size() != 1 || !Text)
        fail("constructor prefix has an invalid string constant");
      prefixWrite(Op.registers[0], "Ljava/lang/String;", javaString(*Text));
    } else if (starts(Op.opcode, "move") && !starts(Op.opcode, "move-result") &&
               !starts(Op.opcode, "move-exception")) {
      prefixCopy(Op);
    } else if (baseOpcode(Op.opcode) == "filled-new-array") {
      const auto Result = constructorPrefixFilledArray(method, Delegation);
      if (!Result)
        fail("constructor prefix filled array requires its exact adjacent "
             "result and a disjoint reference slot");
      const auto &Type = std::get<std::string>(Op.reference);
      std::vector<std::string> Values;
      for (unsigned Register : Op.registers) {
        budget.tick();
        Values.push_back(prefixValue(Register, Type.substr(1)));
      }
      const auto Value = "__neverdCtorArray" + std::to_string(Op.pc);
      // Allocate once at the original position. Copies and post-delegation
      // consumers retain this same object even if the target mutates it.
      prefix += sourceType(Type) + " " + Value + " = new " + sourceType(Type) +
                " {" + join(Values, ", ") + "};\n";
      prefixWrite(*Result, Type, Value);
      ++Delegation;
      minimum_java_release = 25;
    } else if (const auto conversion = scalarConversion(Op.opcode),
                          unary = scalarUnary(Op.opcode);
               conversion || unary) {
      if (Op.registers.size() != 2 || Op.target || !Op.targets.empty() ||
          !Op.keys.empty() || !Op.data.empty() || Op.element_width ||
          !std::holds_alternative<std::monostate>(Op.reference) ||
          !std::holds_alternative<std::monostate>(Op.literal))
        fail("constructor unary operation or conversion has malformed operands");
      const auto &source = conversion ? conversion->first : unary->first;
      const auto &result = conversion ? conversion->second : source;
      const auto input = prefixValue(Op.registers[1], source);
      const auto value = "__neverdCtorConvert" + std::to_string(Op.pc);
      const auto expression = conversion ? scalarConversionExpression(result, input)
                                         : unary->second + "(" + input + ")";
      prefix += sourceType(result) + " " + value + " = " + expression + ";\n";
      prefixWrite(Op.registers[0], result, value);
      minimum_java_release = 25;
    } else if (const auto Floating = floatingArithmetic(Op.opcode)) {
      const auto Arity = Floating->two_address ? 2u : 3u;
      if (Op.registers.size() != Arity || Op.target || !Op.targets.empty() ||
          !Op.keys.empty() || !Op.data.empty() || Op.element_width ||
          !std::holds_alternative<std::monostate>(Op.reference) ||
          !std::holds_alternative<std::monostate>(Op.literal))
        fail("constructor floating arithmetic has malformed operands");
      const auto Left = prefixValue(Op.registers[Floating->two_address ? 0 : 1],
                                    Floating->type);
      const auto Right = prefixValue(
          Op.registers[Floating->two_address ? 1 : 2], Floating->type);
      const auto Value = "__neverdCtorFloat" + std::to_string(Op.pc);
      prefix += sourceType(Floating->type) + " " + Value + " = " +
                floatingArithmeticExpression(*Floating, Left, Right) + ";\n";
      prefixWrite(Op.registers[0], Floating->type, Value);
      minimum_java_release = 25;
    } else if (const auto Binary = longBinary(Op.opcode);
               Binary && !Binary->can_throw) {
      const auto Arity = Binary->two_address ? 2u : 3u;
      if (Op.registers.size() != Arity || Op.target || !Op.targets.empty() ||
          !Op.keys.empty() || !Op.data.empty() || Op.element_width ||
          !std::holds_alternative<std::monostate>(Op.reference) ||
          !std::holds_alternative<std::monostate>(Op.literal))
        fail("constructor long arithmetic has malformed operands");
      const auto Left =
          prefixValue(Op.registers[Binary->two_address ? 0 : 1], "J");
      const auto Right = prefixValue(Op.registers[Binary->two_address ? 1 : 2],
                                     Binary->right_type);
      const auto Value = "__neverdCtorLong" + std::to_string(Op.pc);
      // Snapshot both original inputs before replacing any overlapping wide
      // destination. Long shifts retain their one-word int distance.
      prefix += "long " + Value + " = ((" + Left + ") " + Binary->symbol +
                " (" + Right + "));\n";
      prefixWrite(Op.registers[0], "J", Value);
      minimum_java_release = 25;
    } else if (const auto Comparison = scalarComparison(Op.opcode)) {
      if (Op.registers.size() != 3 || Op.target || !Op.targets.empty() ||
          !Op.keys.empty() || !Op.data.empty() || Op.element_width ||
          !std::holds_alternative<std::monostate>(Op.reference) ||
          !std::holds_alternative<std::monostate>(Op.literal))
        fail("constructor scalar comparison has malformed operands");
      const auto Left = prefixValue(Op.registers[1], Comparison->type);
      const auto Right = prefixValue(Op.registers[2], Comparison->type);
      const auto Value = "__neverdCtorCompare" + std::to_string(Op.pc);
      prefix += "int " + Value + " = " +
                scalarComparisonExpression(*Comparison, Left, Right) + ";\n";
      prefixWrite(Op.registers[0], "I", Value);
      minimum_java_release = 25;
    } else if (const auto Integer = integerPreparation(Op)) {
      const auto Arity = Integer->two_address || Integer->literal ? 2u : 3u;
      if (Op.registers.size() != Arity ||
          !std::holds_alternative<std::monostate>(Op.reference) ||
          (!Integer->literal &&
           !std::holds_alternative<std::monostate>(Op.literal)))
        fail("constructor integer preparation has malformed operands");
      const auto Left =
          prefixValue(Op.registers[Integer->two_address ? 0 : 1], "I");
      std::string Right;
      if (Integer->literal) {
        const auto *Number = std::get_if<int64_t>(&Op.literal);
        if (!Number)
          fail("constructor integer preparation has no signed literal");
        Right = hexLiteral(static_cast<uint64_t>(*Number), 32);
      } else
        Right = prefixValue(Op.registers[Integer->two_address ? 1 : 2], "I");
      const auto Value = "__neverdCtorInt" + std::to_string(Op.pc);
      // Snapshot the typed scalar at its original position. prefixValue uses
      // the same 0/1 interpretation for boolean parameters and getter results.
      prefix += "int " + Value + " = " +
                integerExpression(*Integer, Left, Right) + ";\n";
      prefixWrite(Op.registers[0], "I", Value);
      minimum_java_release = 25;
    } else if (starts(Op.opcode, "iget")) {
      const auto Field =
          constructorPrefixInstanceRead(method, Op, classes, budget);
      if (!Field)
        fail("constructor prefix instance read requires an exact accessible "
             "instance field and a disjoint typed frame");
      const auto Receiver = prefixValue(Op.registers[1], Field->owner);
      const auto Value = "__neverdCtorField" + std::to_string(Op.pc);
      // Snapshot before changing any register, including an overlapping
      // receiver/destination. Retain null/volatile effects at the original PC;
      // later reads and the constructor cannot substitute a newer field value.
      prefix += sourceType(Field->type) + " " + Value + " = " + Receiver + "." +
                sourceFieldName(*Field, classes, inheritance, budget) + ";\n";
      prefixWrite(Op.registers[0], Field->type, Value);
      minimum_java_release = 25;
    } else if (starts(Op.opcode, "sget")) {
      const auto Field =
          constructorPrefixStaticRead(method, Op, classes, budget);
      if (!Field)
        fail("constructor prefix static read requires an exact nonconstant "
             "static field and a typed register disjoint from this");
      const auto Value = "__neverdCtorField" + std::to_string(Op.pc);
      // Read once at the original position even if super mutates the field or
      // the result is unused. This also retains class initialization failures.
      prefix += sourceType(Field->type) + " " + Value + " = " +
                staticFieldRead(*Field) + ";\n";
      prefixWrite(Op.registers[0], Field->type, Value);
      minimum_java_release = 25;
    } else if (Op.opcode == "new-instance") {
      const auto End =
          constructorPrefixAllocationArgumentsEnd(method, Delegation, budget);
      if (!End)
        fail("constructor prefix allocation requires an exact initializer "
             "with verified constant/copy arguments in a reference slot");
      for (size_t At = Delegation + 1; At < *End; ++At) {
        if (emptyNop(code[At]))
          continue;
        if (constructorPrefixIntegerConstant(method, code[At]))
          prefixConstant(code[At]);
        else
          prefixCopy(code[At], true);
      }
      const auto *allocation = std::get_if<MethodRef>(&code[*End].reference);
      if (!allocation)
        fail("constructor allocation receipt lost its initializer");
      const auto allocated = classes.find(allocation->owner);
      const bool prefix_local =
          allocated != classes.end() &&
          local_projection.by_class.contains(allocation->owner) &&
          allocated->second.enclosing_method == method.reference &&
          !allocated->second.enclosing &&
          std::none_of(allocated->second.fields.begin(),
                       allocated->second.fields.end(), [&](const Field &field) {
                         return !has(field.access, "static") &&
                                has(field.access, "synthetic") &&
                                field.reference.type == method.reference.owner;
                       });
      if (allocated == classes.end() ||
          allocated->second.access.contains("abstract") ||
          allocated->second.access.contains("interface") ||
          allocated->second.access.contains("enum") ||
          (localSourceOwner(allocation->owner, classes, budget) &&
           !prefix_local) ||
          (allocated->second.enclosing &&
           !allocated->second.inner_access.contains("static")))
        fail("constructor prefix allocation requires a concrete class with "
             "no implicit enclosing instance");
      Delegation = *End;
      const auto *Declaration = validateInvocation(code[Delegation]);
      // Prefix origins have not entered the ordinary converged state graph.
      // A declaration receipt alone cannot supply its argument witness here.
      if (Declaration &&
          generics.intersection_invocations.contains(Declaration->reference))
        fail("constructor prefix allocation lacks its intersection argument "
             "witness");
      if (!Declaration || Declaration->reference != *allocation ||
          !expressionMemberAccessible(*allocation, *Declaration) ||
          !constructorPrefixThrowsSupported(*Declaration, method, generics))
        fail("constructor prefix allocation requires its exact constructor "
             "declaration and covered exceptions");
      // The shared allocation-pair receipt owns receiver and word layout.
      // Read the original prefix values before storing the allocated local,
      // retaining allocation/class initialization before argument effects.
      std::vector<std::string> Arguments;
      size_t Cursor = 1;
      for (const auto &Parameter : allocation->parameters) {
        Arguments.push_back(
            prefixValue(code[Delegation].registers[Cursor], Parameter));
        Cursor += wordWidth(Parameter);
      }
      prefix_local_declarations |= prefix_local;
      const auto Type = sourceType(allocation->owner);
      const auto Value = "__neverdCtorAllocation" + std::to_string(Op.pc);
      prefix += Type + " " + Value + " = new " + Type + "(" +
                join(Arguments, ", ") + ");\n";
      prefixWrite(Op.registers[0], allocation->owner, Value);
      minimum_java_release = 25;
    } else if (Op.opcode == "check-cast") {
      const auto &Type = referenceType(Op);
      if (Op.registers.size() != 1 || !isReference(Type))
        fail("constructor prefix cast requires a reference type and one "
             "register");
      const auto Input = prefixValue(Op.registers[0], "Ljava/lang/Object;");
      const auto Value = "__neverdCtorCast" + std::to_string(Op.pc);
      // Materialize even an unused cast at its original position: moving it
      // into super's arguments could reorder a failed cast around other
      // effects.
      prefix += sourceType(Type) + " " + Value + " = ((" + sourceType(Type) +
                ") " + Input + ");\n";
      prefixWrite(Op.registers[0], Type, Value);
      minimum_java_release = 25;
    } else if (enum_projection && enum_projection->argument_call_pc == Op.pc) {
      const auto *Call = std::get_if<MethodRef>(&Op.reference);
      if (!Call || !Call->parameters.empty() || Call->returns == "V" ||
          !Op.registers.empty() ||
          (Op.opcode != "invoke-static" &&
           Op.opcode != "invoke-static/range") ||
          Delegation + 1 >= code.size())
        fail("enum constructor argument call disagrees with its proof");
      const auto *Declaration = validateInvocation(Op);
      if (!Declaration || (Declaration->declared_throws &&
                           !Declaration->declared_throws->empty()))
        fail("enum constructor argument call requires a checked-exception "
             "proof");
      const auto &Result = code[++Delegation];
      if (!pending_results.contains(Result.pc) || Result.registers.size() != 1)
        fail("enum constructor argument call has no exact result carrier");
      ArgumentCall =
          staticOwner(Call->owner) + "." + invocationName(*Call) + "()";
      prefixWrite(Result.registers[0], Call->returns, *ArgumentCall);
    } else if (const auto *Call = std::get_if<MethodRef>(&Op.reference);
               Call && constructorPrefixValueCall(Op)) {
      const auto *Declaration =
          baseOpcode(Op.opcode) == "invoke-direct"
              ? &expressionInvocation(Op, "constructor prefix private call")
              : validateInvocation(Op);
      if (!Declaration ||
          !constructorPrefixThrowsSupported(*Declaration, method, generics) ||
          Delegation + 1 >= code.size())
        fail("constructor prefix getter requires a declaration with covered "
             "exceptions and an exact result");
      const auto Alias =
          inheritance.source_method_names.find(Declaration->reference);
      const bool ProtectedIdentifier =
          has(Declaration->access, "protected") &&
          Alias != inheritance.source_method_names.end() &&
          Alias->second.identifier_only;
      if ((!Call->parameters.empty() || ProtectedIdentifier) &&
          (!expressionMemberAccessible(*Call, *Declaration) ||
           inheritance.bridges.contains(Declaration->reference) ||
           changesMethodContract(Declaration->reference, inheritance) ||
           inheritance.void_unit_targets.contains(Declaration->reference)))
        fail("constructor prefix argument call needs an unchanged public "
             "member or exact class/package member");
      const bool Static = Declaration->access.contains("static");
      if (!Static && Op.registers.empty())
        fail("constructor prefix getter receiver count disagrees");
      const auto Receiver = Static ? staticOwner(Call->owner)
                                   : virtualReceiver(*Call, Op, prefixValue(Op.registers[0], Call->owner));
      std::vector<std::string> Arguments;
      size_t Cursor = Static ? 0 : 1;
      for (const auto &Type : Call->parameters) {
        if (Cursor >= Op.registers.size())
          fail("constructor prefix getter is missing argument words");
        Arguments.push_back(prefixValue(Op.registers[Cursor], Type));
        if (wordWidth(Type) == 2 &&
            (Cursor + 1 >= Op.registers.size() ||
             Op.registers[Cursor + 1] != Op.registers[Cursor] + 1))
          fail("constructor prefix getter has a broken wide argument");
        Cursor += wordWidth(Type);
      }
      if (Cursor != Op.registers.size())
        fail("constructor prefix getter receiver count disagrees");
      const auto &Result = code[++Delegation];
      if (!pending_results.contains(Result.pc) || Result.registers.size() != 1)
        fail("constructor prefix getter has no exact result carrier");
      const auto Value = "__neverdCtorResult" + std::to_string(Op.pc);
      prefix += sourceType(Call->returns) + " " + Value + " = " + Receiver +
                "." + invocationName(*Call) + "(" +
                invocationArguments(*Call, Arguments) + ");\n";
      prefixWrite(Result.registers[0], Call->returns, Value);
      minimum_java_release = 25;
    } else if (Op.opcode == "invoke-static" ||
               Op.opcode == "invoke-static/range") {
      const auto *Call = std::get_if<MethodRef>(&Op.reference);
      const auto *Declaration = validateInvocation(Op);
      if (!Call || Call->returns != "V" || !Declaration ||
          !expressionMemberAccessible(*Call, *Declaration) ||
          generics.intersection_invocations.contains(Declaration->reference) ||
          !constructorPrefixThrowsSupported(*Declaration, method, generics))
        fail("constructor prefix call requires an accessible proven void "
             "declaration and covered exceptions");
      std::vector<std::string> Arguments;
      size_t Cursor = 0;
      for (const auto &Type : Call->parameters) {
        if (Cursor >= Op.registers.size())
          fail("constructor prefix call is missing argument words");
        Arguments.push_back(prefixValue(Op.registers[Cursor], Type));
        if (wordWidth(Type) == 2 &&
            (Cursor + 1 >= Op.registers.size() ||
             Op.registers[Cursor + 1] != Op.registers[Cursor] + 1))
          fail("constructor prefix call has a broken wide register pair");
        Cursor += wordWidth(Type);
      }
      if (Cursor != Op.registers.size())
        fail("constructor prefix call argument count disagrees");
      prefix += staticOwner(Call->owner) + "." + invocationName(*Call) + "(" +
                invocationArguments(*Call, Arguments) + ");\n";
      minimum_java_release = 25;
    } else if (starts(Op.opcode, "iput")) {
      const auto *Field = std::get_if<FieldRef>(&Op.reference);
      if (!Field || Field->owner != method.reference.owner ||
          Op.registers.size() != 2 || !Original.contains(Op.registers[1]) ||
          Original.at(Op.registers[1]).first !=
              "self:" + method.reference.owner)
        fail("constructor prefix field write requires its own uninitialized "
             "receiver");
      const auto *Declaration = member(*Field, classes, budget);
      if (!Declaration || Declaration->access.contains("static") ||
          (Declaration->access.contains("final") &&
           !final_writes.contains(*Field)))
        fail("constructor prefix field write has no initialization proof");
      auto Type = Field->type;
      auto Expected = isReference(Type)      ? "iput-object"
                      : wordWidth(Type) == 2 ? "iput-wide"
                      : Type == "Z"          ? "iput-boolean"
                      : Type == "B"          ? "iput-byte"
                      : Type == "C"          ? "iput-char"
                      : Type == "S"          ? "iput-short"
                                             : "iput";
      if (Op.opcode != Expected)
        fail("constructor prefix field opcode disagrees with its declaration");
      auto value = prefixValue(Op.registers[0], Type);
      if (const auto shape = generics.fields.find(*Field);
          shape != generics.fields.end() && shape->second.field_type) {
        const auto *component = &shape->second.types.at(*shape->second.field_type);
        while (component->element)
          component = &shape->second.types.at(*component->element);
        if (type_names.visibleFormal(*component, JavaScope{ownerClass(), &method.reference}))
          value = finalFieldSourceValue(*Field, std::move(value));
      }
      prefix += "this." +
                sourceFieldName(*Field, classes, inheritance, budget) + " = " +
                value + ";\n";
      minimum_java_release = 25;
    } else {
      fail("constructor requires a representable leading super/this call or "
           "verified prologue");
    }
  }
  if (Delegation == code.size())
    fail("constructor requires a representable leading super/this call");
  const auto &Head = code[Delegation];
  const auto *Ref = std::get_if<MethodRef>(&Head.reference);
  if ((Head.opcode != "invoke-direct" &&
       Head.opcode != "invoke-direct/range") ||
      !Ref || Ref->name != "<init>" || Head.registers.empty() ||
      !Original.contains(Head.registers[0]) ||
      Original.at(Head.registers[0]).first != "self:" + method.reference.owner)
    fail("constructor requires a representable leading super/this call");
  const auto &C = ownerClass();
  if (Ref->owner != C.name && (!C.superclass || Ref->owner != *C.superclass))
    fail("constructor receiver does not match its owner or superclass");
  if (Ref->owner == C.name && minimum_java_release > 8 && !GuardedDelegation &&
      (CapturedDelegation == generics.constructor_delegations.end() ||
       CapturedDelegation->second != *Ref))
    fail("constructor prefix before this delegation requires its shared "
         "initialization proof");
  const auto *DelegationDeclaration = validateInvocation(Head);
  const auto ExceptionContract =
      generics.constructor_exception_contracts.find(method.reference);
  const bool ProvenExceptions =
      ExceptionContract != generics.constructor_exception_contracts.end();
  if (ProvenExceptions &&
      (ExceptionContract->second.target != *Ref ||
       ExceptionContract->second.call_pc != Head.pc || !DelegationDeclaration ||
       DelegationDeclaration->reference != *Ref))
    fail("constructor exception contract does not match the original "
         "delegation");
  // Every resolved delegation uses the same access proof, including a
  // first-operation call and any depth of a this/super constructor chain.
  if ((GuardedSuper || DelegationDeclaration) &&
      (!DelegationDeclaration || DelegationDeclaration->reference != *Ref ||
       (!expressionMemberAccessible(*Ref, *DelegationDeclaration) &&
        !(DelegationDeclaration->reference == *Ref &&
          has(DelegationDeclaration->access, "protected")))))
    fail(
        GuardedSuper
            ? "guarded superclass call requires an accessible exact declaration"
            : "constructor delegation requires an accessible exact "
              "declaration");
  const bool FixedDescriptorTarget =
      DelegationDeclaration && !has(DelegationDeclaration->access, "varargs") &&
      !DelegationDeclaration->generic_signature &&
      !sourceMethodSignature(DelegationDeclaration->reference, generics);
  const GenericSignature *ConstructorContract = nullptr;
  if (const auto I = generics.superclass_calls.find({method.reference, *Ref});
      I != generics.superclass_calls.end())
    ConstructorContract = &I->second;
  else if (const auto I =
               generics.constructor_calls.find({method.reference, *Ref});
           I != generics.constructor_calls.end())
    ConstructorContract = &I->second;
  bool erasedDelegationArguments = false;
  if (!ConstructorContract && DelegationDeclaration && !enum_projection &&
      !has(DelegationDeclaration->access, "varargs")) {
    const auto &owner = classes.at(Ref->owner);
    const auto *signature = sourceMethodSignature(*Ref, generics);
    const bool unique = signature && std::none_of(
        owner.methods.begin(), owner.methods.end(), [&](const Method &m) {
          budget.tick();
          const auto &sibling = m.reference;
          if (sibling.name != "<init>" || sibling == *Ref)
            return false;
          if (sibling.parameters.size() != Ref->parameters.size())
            return has(m.access, "varargs");
          const auto *other = sourceMethodSignature(sibling, generics);
          for (size_t i = 0; i < Ref->parameters.size(); ++i) {
            const auto &selected = Ref->parameters[i];
            if (isReference(selected) != isReference(sibling.parameters[i]) &&
                !has(m.access, "varargs"))
              return false; // The selected constructor needs no boxing.
            if (!signature || signature->parameters.size() != Ref->parameters.size())
              continue;
            const auto &type = signature->types.at(signature->parameters[i]);
            if (type.kind != GenericType::Kind::Class || type.erasure != selected)
              continue;
            // The selected nominal stays at this descriptor even when its
            // own type arguments change. Varargs first compete as arrays.
            if ((!other || other->type_parameters.empty()) &&
                fixedArgumentExcludes(selected, sibling.parameters[i]))
              return false;
            // In this class, a nominal carrier of E cannot be passed as E.
            // A lexical call with that exact carrier selects its own overload.
            if (other && other->parameters.size() == sibling.parameters.size() &&
                other->type_parameters.empty() && other->source_projections.empty() &&
                C.name == Ref->owner && !has(m.access, "varargs") &&
                has(DelegationDeclaration->access, "private") &&
                type.segments.size() == 1 && type.segments.front().arguments.size() == 1) {
              const auto &alt = other->types.at(other->parameters[i]);
              const auto &arg = type.segments.front().arguments.front();
              if (alt.kind == GenericType::Kind::TypeVariable &&
                  alt.variable_owner == C.name && arg.type &&
                  arg.variance == GenericTypeArgument::Variance::Invariant) {
                const auto &element = signature->types.at(*arg.type);
                if (element.variable_owner == C.name && element.variable_name == alt.variable_name)
                  return false;
              }
            }
          }
          return true;
        });
    if (unique && generics.methods.contains(*Ref) &&
        signature == &generics.methods.at(*Ref) &&
        signature->type_parameters.empty() && signature->source_projections.empty() &&
        signature->throws_types.empty() &&
        signature->parameters.size() == Ref->parameters.size()) {
      bool exact = true, classVariable = false, fixedComponents = true;
      for (const auto &type : signature->types) {
        budget.tick();
        if (type.kind == GenericType::Kind::TypeVariable) {
          exact &= type.variable_owner == owner.name;
          classVariable = true;
        }
        if (type.kind == GenericType::Kind::Class)
          exact &= classes.contains(type.erasure);
      }
      for (size_t i = 0; i < Ref->parameters.size(); ++i) {
        auto id = signature->parameters[i];
        exact &= signature->types.at(id).erasure == Ref->parameters[i];
        while (signature->types.at(id).kind == GenericType::Kind::Array)
          id = *signature->types.at(id).element;
        fixedComponents &= signature->types.at(id).kind != GenericType::Kind::TypeVariable;
      }
      auto allocatedArguments = [&] {
        // This fallback binds only values constructed at the parameter's own
        // erased type. Forwarded values and subclass substitutions retain
        // their separately validated constructor contracts.
        std::map<unsigned, std::string> allocated;
        for (size_t pc = 0; pc < Delegation; ++pc) {
          budget.tick();
          const auto &op = code[pc];
          const auto kind = baseOpcode(op.opcode);
          if (op.opcode == "new-array" || op.opcode == "new-instance") {
            allocated[op.registers.at(0)] = referenceType(op);
          } else if (const auto result = constructorPrefixFilledArray(method, pc)) {
            allocated[*result] = referenceType(op);
            ++pc;
          } else if (kind == "move-object") {
            if (auto value = allocated.find(op.registers.at(1)); value != allocated.end())
              allocated[op.registers[0]] = value->second;
            else
              allocated.erase(op.registers[0]);
          } else if (kind == "const" || kind == "const-wide") {
            allocated.erase(op.registers.at(0));
            if (kind == "const-wide")
              allocated.erase(op.registers[0] + 1);
          } else if (const auto *call = std::get_if<MethodRef>(&op.reference);
                     !(kind == "invoke-direct" && call && call->name == "<init>") &&
                     kind != "nop") {
            return false;
          }
        }
        if (Head.registers.size() != Ref->parameters.size() + 1)
          return false;
        for (size_t i = 0; i < Ref->parameters.size(); ++i) {
          const auto value = allocated.find(Head.registers[i + 1]);
          if (value == allocated.end() || value->second != Ref->parameters[i])
            return false;
        }
        return true;
      };
      // An ordinary scalar constructor has no incoming generic reference to
      // substitute. Its private target uses these same class formals, so the
      // checked erased array arguments retain their component erasures even
      // when a prior call produces an array or another argument is null.
      const bool scalarArrayFactory =
          has(DelegationDeclaration->access, "private") &&
          !method.reference.parameters.empty() &&
          std::none_of(method.reference.parameters.begin(),
                       method.reference.parameters.end(), isReference) &&
          std::any_of(signature->parameters.begin(),
                      signature->parameters.end(), [&](auto id) {
                        const auto &type = signature->types.at(id);
                        return type.kind == GenericType::Kind::Array &&
                               signature->types.at(*type.element).kind ==
                                   GenericType::Kind::TypeVariable;
                      });
      // A synthetic access constructor can forward its unchanged incoming
      // words to a private constructor and drop just the trailing marker.
      // The mask form instead uses the already verified guarded prefix.
      const auto &entry = method.reference;
      const bool lexicalMarker =
          exact && Ref->owner == C.name && !C.enclosing &&
          !C.enclosing_method && has(method.access, "synthetic") &&
          !has(method.access, "varargs") && !method.generic_signature &&
          !entry.parameters.empty() &&
          entry.parameters.back() ==
              "Lkotlin/jvm/internal/DefaultConstructorMarker;" &&
          (!method.declared_throws || method.declared_throws->empty()) &&
          (!DelegationDeclaration->declared_throws ||
           DelegationDeclaration->declared_throws->empty());
      const bool accessMarker =
          lexicalMarker && classVariable && has(C.access, "public") &&
          has(C.access, "abstract") && fixedComponents && Delegation == 0 &&
          code.size() == 2 && code[1].opcode == "return-void" &&
          has(DelegationDeclaration->access, "private") &&
          entry.parameters.size() == Ref->parameters.size() + 1 &&
          std::equal(Ref->parameters.begin(), Ref->parameters.end(),
                     entry.parameters.begin()) &&
          Head.registers.size() + 1 == method.incomingWords() &&
          Head.registers.front() == SelfReg &&
          std::adjacent_find(Head.registers.begin(), Head.registers.end(),
                             [](unsigned a, unsigned b) {
                               return b != a + 1;
                             }) == Head.registers.end();
      // Only the bit for the last source parameter may replace that incoming
      // reference with null. The other arm forwards every original word.
      const auto nullDefault = [&] {
        if (!lexicalMarker || !GuardedDelegation || Delegation != 3 ||
            !has(C.access, "public") || !C.generic_signature ||
            DelegationDeclaration->access != Access{"public", "constructor"} ||
            !hasDefaultConstructorMask(entry, *Ref) ||
            Ref->parameters.size() > 8 || !fixedComponents ||
            !isReference(Ref->parameters.back()))
          return false;
        const auto &operand = signature->types.at(signature->parameters.back());
        if (operand.kind != GenericType::Kind::Class ||
            !std::any_of(operand.segments.begin(), operand.segments.end(),
                         [](const auto &segment) {
                           return std::any_of(
                               segment.arguments.begin(),
                               segment.arguments.end(), [](const auto &arg) {
                                 return arg.variance ==
                                        GenericTypeArgument::Variance::Super;
                               });
                         }))
          return false;
        const auto &mask = code[0], &branch = code[1], &zero = code[2];
        const auto bit = std::get_if<int64_t>(&mask.literal);
        const auto value = integerConstant(zero, method.registers);
        const auto maskWord = method.registers - 2;
        return mask.opcode == "and-int/lit8" && mask.registers.size() == 2 &&
               mask.registers[1] == maskWord && bit &&
               *bit == (int64_t{1} << (Ref->parameters.size() - 1)) &&
               branch.opcode == "if-eqz" &&
               branch.registers == std::vector<unsigned>{mask.registers[0]} &&
               branch.target == Head.pc && value && value->bits == 0 &&
               value->type != "bits64" && value->destination == maskWord - 1 &&
               Head.registers.back() == value->destination &&
               Head.registers.front() == SelfReg &&
               Head.registers.size() + 2 == method.incomingWords() &&
               std::adjacent_find(Head.registers.begin(), Head.registers.end(),
                                  [](unsigned a, unsigned b) {
                                    return b != a + 1;
                                  }) == Head.registers.end();
      };
      const auto unchangedCallback = [&] {
        if (!lexicalMarker || !GuardedDelegation || !classVariable || !fixedComponents ||
            has(C.access, "public") || !hasDefaultConstructorMask(entry, *Ref) ||
            DelegationDeclaration->access != Access{"public", "constructor"} ||
            Ref->parameters.size() < 3 || Head.registers.size() < 2 ||
            Head.registers[1] != SelfReg + 1)
          return false;
        const auto &callback = signature->types.at(signature->parameters.front());
        if (callback.segments.size() != 1 || callback.segments.front().arguments.empty())
          return false;
        for (size_t i = 1; i < signature->parameters.size(); ++i) {
          const auto &fixed = signature->types.at(signature->parameters[i]);
          if (fixed.kind != GenericType::Kind::Primitive &&
              !(fixed.kind == GenericType::Kind::Class && fixed.segments.size() == 1 &&
                fixed.segments.front().arguments.empty()))
            return false;
        }
        // The checked mask CFG prepares only ground inputs. The incoming
        // callback carrying the class formal is never read or overwritten.
        std::set<int64_t> masks;
        for (size_t i = 0; i < Delegation; ++i) {
          const auto &op = code[i];
          if (std::find(op.registers.begin(), op.registers.end(), SelfReg + 1) !=
                  op.registers.end() ||
              (op.opcode.find("wide") != std::string::npos &&
               !op.registers.empty() && op.registers.front() == SelfReg))
            return false;
          if (op.opcode == "and-int/lit8")
            if (const auto *bit = std::get_if<int64_t>(&op.literal))
              masks.insert(*bit);
        }
        return masks.size() > 1;
      };
      const auto callerClassTokenAndDefaults = [&] {
        const auto *caller = sourceMethodSignature(method.reference, generics);
        if (!caller || !caller->type_parameters.empty() || !caller->source_projections.empty() ||
            caller->parameters.size() != 1 || Ref->parameters.size() != 3 ||
            Head.registers.size() != 4 || Head.registers[1] != SelfReg + 1 ||
            !Ref->parameters.back().starts_with('['))
          return false;
        const auto &given = caller->types.at(caller->parameters.front());
        const auto &wanted = signature->types.at(signature->parameters.front());
        if (given.erasure != wanted.erasure || given.segments.size() != 1 ||
            wanted.segments.size() != 1 || given.segments.front().arguments.size() != 1 ||
            wanted.segments.front().arguments.size() != 1)
          return false;
        const auto &a = given.segments.front().arguments.front();
        const auto &b = wanted.segments.front().arguments.front();
        if (!a.type || !b.type || a.variance != GenericTypeArgument::Variance::Invariant ||
            b.variance != a.variance)
          return false;
        const auto &left = caller->types.at(*a.type);
        const auto &right = signature->types.at(*b.type);
        if (left.variable_owner != C.name || right.variable_owner != C.name ||
            left.variable_name != right.variable_name || Delegation < 2)
          return false;
        const auto zero = constructorPrefixIntegerConstant(method, code[Delegation - 2]);
        const auto &read = code[Delegation - 1];
        const auto *field = std::get_if<FieldRef>(&read.reference);
        return zero && zero->bits == 0 && zero->destination == Head.registers[2] &&
               read.opcode == "sget-object" && field && field->type == Ref->parameters.back() &&
               read.registers == std::vector<unsigned>{Head.registers[3]};
      };
      if (accessMarker || nullDefault() || unchangedCallback()) {
        ConstructorContract = signature;
      } else if (exact && Ref->owner == C.name && classVariable &&
                 !C.enclosing && !C.enclosing_method &&
                 !has(method.access, "synthetic") &&
                 (scalarArrayFactory || allocatedArguments() ||
                  (fixedComponents && !GuardedDelegation && callerClassTokenAndDefaults()))) {
        // The caller can spell the declaring class's exact original formals.
        const auto *caller = sourceMethodSignature(method.reference, generics);
        if (!caller || caller->type_parameters.empty())
          ConstructorContract = signature;
      } else if (exact && !classVariable && GuardedDelegation &&
                 Ref->owner == C.name && !has(method.access, "varargs") &&
                 ((!C.generic_signature && !has(method.access, "synthetic") &&
                   sourceMethodSignature(method.reference, generics)) ||
                  (has(DelegationDeclaration->access, "private") &&
                   has(method.access, "synthetic") && has(C.access, "final") &&
                   !method.generic_signature && C.generic_signature &&
                   generics.classes.contains(C.name) &&
                   generics.classes.at(C.name).type_parameters.empty() &&
                   CapturedDelegation !=
                       generics.constructor_delegations.end() &&
                   CapturedDelegation->second == *Ref &&
                   hasDefaultConstructorMask(method.reference, *Ref))) &&
                 (!method.declared_throws || method.declared_throws->empty()) &&
                 (!DelegationDeclaration->declared_throws ||
                  DelegationDeclaration->declared_throws->empty())) {
        // The guarded prefix has already checked every converged argument
        // against this descriptor. A ground nominal signature on the same
        // class has no substitutable component, including on the null branch.
        erasedDelegationArguments = true;
      } else if (exact && C.superclass == Ref->owner && fixedComponents) {
        // Rich caller-owned generic substitutions belong to the superclass
        // binding proof above. At a raw boundary admit only ground arguments
        // of a local adapter or independently produced call/field operands.
        const auto *caller = sourceMethodSignature(method.reference, generics);
        const auto child = generics.classes.find(C.name);
        const bool groundLocal = C.enclosing_method && !classVariable;
        const bool produced = std::any_of(code.begin(), code.begin() + Delegation,
            [](const Instruction &op) { return op.opcode == "move-result-object"; });
        const bool singleton = std::any_of(code.begin(), code.begin() + Delegation,
            [](const Instruction &op) { return op.opcode == "sget-object"; });
        const bool fresh = std::any_of(code.begin(), code.begin() + Delegation,
            [](const Instruction &op) { return op.opcode == "new-instance"; });
        const bool closedOwner = child == generics.classes.end() ||
                                 child->second.type_parameters.empty();
        const bool forwarded = produced && caller && !closedOwner &&
            std::all_of(signature->types.begin(), signature->types.end(),
                        [&](const GenericType &type) {
              if (type.kind != GenericType::Kind::Class || type.segments.empty())
                return true;
              const auto &args = type.segments.back().arguments;
              return args.empty() || (args.size() == 1 && args.front().type &&
                  args.front().variance == GenericTypeArgument::Variance::Super &&
                  signature->types.at(*args.front().type).variable_owner == Ref->owner);
            });
        // A raw constructor can pass its sole word straight through, or
        // prepare it with its own static factory. Both calls must propagate
        // their original checked exceptions through this constructor.
        const bool checkedOperand = [&] {
          if (caller || !closedOwner || !validatedClosedDeclaredThrows(method, generics) ||
              !constructorPrefixThrowsSupported(*DelegationDeclaration, method, generics) ||
              method.reference.parameters.size() != 1 || Ref->parameters.size() != 1 ||
              !isReference(Ref->parameters.front()) || Head.registers.size() != 2 ||
              Head.registers.front() != SelfReg)
            return false;
          const auto &carrier = signature->types.at(signature->parameters.front());
          if (carrier.kind != GenericType::Kind::Class || carrier.segments.size() != 1)
            return false;
          const auto &arguments = carrier.segments.front().arguments;
          if (arguments.size() != 1 || !arguments.front().type ||
              arguments.front().variance != GenericTypeArgument::Variance::Invariant)
            return false;
          if (Delegation == 0)
            return !classVariable && Head.registers.back() == SelfReg + 1 &&
                   method.reference.parameters == Ref->parameters &&
                   sameClosedInheritedThrows(*DelegationDeclaration, method, generics);
          if (Delegation != 2 || baseOpcode(code[0].opcode) != "invoke-static" ||
              code[0].registers.size() != 1 || code[0].registers.front() != SelfReg + 1 ||
              code[1].opcode != "move-result-object" ||
              code[1].registers.size() != 1 || code[1].registers.front() != Head.registers.back())
            return false;
          const auto *factory = std::get_if<MethodRef>(&code[0].reference);
          if (!factory || factory->owner != C.name || factory->parameters != method.reference.parameters ||
              factory->returns != Ref->parameters.front())
            return false;
          const auto *declaration = member(*factory, classes, budget, &generics);
          return declaration && declaration->reference == *factory &&
                 constructorPrefixThrowsSupported(*declaration, method, generics);
        }();
        // A factory returning its method formal from a Class token needs the
        // original literal-to-result proof before its result can feed super.
        const bool tokenFactory = std::any_of(code.begin(), code.begin() + Delegation,
            [&](const Instruction &op) {
              const auto *call = std::get_if<MethodRef>(&op.reference);
              if (!call || std::find(call->parameters.begin(), call->parameters.end(),
                                     "Ljava/lang/Class;") == call->parameters.end())
                return false;
              const auto *producer = member(*call, classes, budget, &generics);
              const auto *shape = producer ? sourceMethodSignature(producer->reference, generics)
                                           : nullptr;
              return shape && !shape->type_parameters.empty() && shape->result &&
                     shape->types.at(*shape->result).kind == GenericType::Kind::TypeVariable;
            });
        if (checkedOperand)
          erasedDelegationArguments = true;
        else if (!tokenFactory && !owner.external && (!caller || caller->type_parameters.empty()) &&
            (!method.declared_throws || method.declared_throws->empty()) &&
            (!DelegationDeclaration->declared_throws || DelegationDeclaration->declared_throws->empty()) &&
            (groundLocal || forwarded ||
             (closedOwner && !caller &&
              (produced || (singleton && (!classVariable ||
                                          (fresh && method.reference.parameters.empty())))))))
          erasedDelegationArguments = true;
      }
    }
  }
  bool PrimitiveOverloadSeparation = false;
  bool NominalOverloadSeparation = false;
  if (auto Owner = classes.find(Ref->owner);
      Owner != classes.end() && !ConstructorContract && !erasedDelegationArguments) {
    for (const auto &Candidate : Owner->second.methods) {
      budget.tick();
      if (Candidate.reference.name != "<init>" ||
          Candidate.reference.parameters.size() != Ref->parameters.size())
        continue;
      auto I = generics.methods.find(Candidate.reference);
      if (I == generics.methods.end())
        continue;
      // Every argument below has the selected descriptor's primitive view.
      // A descriptor-only fixed-arity target is applicable before boxing;
      // a reference at any corresponding position cannot compete in that
      // phase. This excludes generic copy/formal/array siblings without
      // guessing their bindings, and never relaxes the selected contract.
      bool ReferenceMismatch = false;
      if (FixedDescriptorTarget && Candidate.reference != *Ref)
        for (size_t P = 0; P < Ref->parameters.size(); ++P) {
          budget.tick();
          ReferenceMismatch |= !isReference(Ref->parameters[P]) &&
                               isReference(Candidate.reference.parameters[P]);
        }
      if (ReferenceMismatch) {
        PrimitiveOverloadSeparation = true;
        continue;
      }
      const auto &Signature = I->second;
      // The selected descriptor supplies an exact source reference type. A
      // public independent Object root or plain child with only Object as its
      // original parent cannot convert to an array, unrelated nominal or
      // class-formal upper bound. A resolved array needs no nominal declaration
      // node. The complete hierarchy also disqualifies a parameterized sibling
      // without inferring its arguments or array elements. This static view
      // holds even for null; a missing declaration proves no non-assignability.
      bool NominalMismatch = false;
      if (FixedDescriptorTarget && Candidate.reference != *Ref &&
          !has(Candidate.access, "varargs") &&
          Signature.type_parameters.empty() &&
          Signature.source_projections.empty() &&
          Signature.parameters.size() == Ref->parameters.size())
        for (size_t P = 0; P < Ref->parameters.size(); ++P) {
          budget.tick();
          const auto &Type = Signature.types.at(Signature.parameters[P]);
          const auto Source = classes.find(Ref->parameters[P]);
          const auto Bound = classes.find(Type.erasure);
          const auto Ancestry =
              generics.constructor_argument_ancestors.find(Ref->parameters[P]);
          // Only the shared signature owner can establish a complete original
          // class chain. Generic interface arguments and intermediate parents
          // remain intact; absence of a class in that chain excludes
          // conversion.
          const bool ClosedClass =
              Type.kind == GenericType::Kind::Class && Bound != classes.end() &&
              !has(Bound->second.access, "interface") &&
              Ancestry != generics.constructor_argument_ancestors.end() &&
              !Ancestry->second.contains(Type.erasure);
          NominalMismatch |=
              ((Type.kind == GenericType::Kind::Array && Type.element) ||
               ((Type.kind == GenericType::Kind::Class ||
                 (Type.kind == GenericType::Kind::TypeVariable &&
                  Type.variable_owner == Candidate.reference.owner)) &&
                Bound != classes.end())) &&
              Type.erasure == Candidate.reference.parameters[P] &&
              Type.erasure != "Ljava/lang/Object;" &&
              Type.erasure != Ref->parameters[P] && Source != classes.end() &&
              has(Source->second.access, "public") &&
              independentSourceScope(Source->second) &&
              !generics.inheritance_contracts.contains(Source->first) &&
              ((!Source->second.generic_signature &&
                Source->second.interfaces.empty() &&
                (Source->second.superclass == "Ljava/lang/Object;" ||
                 (Ref->parameters[P] == "Ljava/lang/Object;" &&
                  !Source->second.superclass))) ||
               ClosedClass);
        }
      // A selected descriptor-only array is already a fixed-arity argument,
      // including a varargs declaration. Use the common array assignability
      // rule to exclude a sibling's unrelated nominal erasure; its generic
      // element contributes no applicability and requires no substitution.
      if (DelegationDeclaration && !DelegationDeclaration->generic_signature &&
          !sourceMethodSignature(DelegationDeclaration->reference, generics) &&
          Candidate.reference != *Ref && !has(Candidate.access, "varargs") &&
          Signature.type_parameters.empty() &&
          Signature.source_projections.empty() &&
          Signature.parameters.size() == Ref->parameters.size())
        for (size_t P = 0; P < Ref->parameters.size(); ++P) {
          budget.tick();
          const auto &Type = Signature.types.at(Signature.parameters[P]);
          const auto Bound = classes.find(Type.erasure);
          NominalMismatch |=
              Ref->parameters[P].starts_with('[') &&
              Type.kind == GenericType::Kind::Class &&
              Type.erasure == Candidate.reference.parameters[P] &&
              Bound != classes.end() && has(Bound->second.access, "public") &&
              independentSourceScope(Bound->second) &&
              !generics.inheritance_contracts.contains(Bound->first) &&
              !isAssignableReference(Ref->parameters[P], Type.erasure, classes,
                                     budget);
        }
      if (NominalMismatch) {
        NominalOverloadSeparation = true;
        continue;
      }
      if (!Signature.type_parameters.empty())
        fail("constructor overload requires a proven generic parameter "
             "binding");
      // An erased cast retains ordinary overload selection. Generic parameter
      // substitution and unchecked applicability require a separate proof for
      // every candidate, including constructors other than the selected one.
      auto Pending = Signature.parameters;
      while (!Pending.empty()) {
        budget.tick();
        const auto &Type = Signature.types.at(Pending.back());
        Pending.pop_back();
        if (Type.kind == GenericType::Kind::TypeVariable ||
            (Type.kind == GenericType::Kind::Class &&
             std::any_of(Type.segments.begin(), Type.segments.end(),
                         [](const GenericClassSegment &Segment) {
                           return !Segment.arguments.empty();
                         })))
          fail("constructor overload requires a proven generic parameter "
               "binding");
        if (Type.kind == GenericType::Kind::Array)
          Pending.push_back(*Type.element);
      }
    }
  }
  std::vector<std::string> Args;
  size_t Cursor = 1;
  for (const auto &T : Ref->parameters) {
    if (Cursor >= Head.registers.size() ||
        !Original.contains(Head.registers[Cursor]))
      fail("constructor prefix requires unavailable argument evaluation");
    auto Value = GuardedDelegation ? GuardedArguments.at(Args.size())
                                   : prefixValue(Head.registers[Cursor], T);
    // A literal zero has no overload-selection type in Java. Keep its
    // original reference descriptor when another fixed-arity entry competes.
    const bool overloadedNull = !GuardedDelegation && isReference(T) &&
        Original.at(Head.registers[Cursor]).first == "zero" && DelegationDeclaration &&
        std::any_of(classes.at(Ref->owner).methods.begin(),
                    classes.at(Ref->owner).methods.end(), [&](const Method &other) {
          budget.tick();
          return other.reference.name == "<init>" && other.reference != *Ref &&
                 (other.reference.parameters.size() == Ref->parameters.size() ||
                  has(other.access, "varargs"));
        });
    if (ConstructorContract) {
      const auto &Contract = *ConstructorContract;
      Value = "((" +
              type_names.render(Contract, Contract.parameters.at(Args.size()),
                                JavaScope{ownerClass(), &method.reference}) +
              ") " + Value + ")";
    } else if ((overloadedNull || erasedDelegationArguments || NominalOverloadSeparation) &&
               isReference(T))
      // In particular, a DEX zero still needs the selected reference view;
      // an untyped null would make unrelated reference overloads ambiguous.
      Value = "((" + sourceType(T) + ") " + Value + ")";
    else if (PrimitiveOverloadSeparation && !isReference(T))
      // DEX loses source-only widening casts (for example byte to int).
      // Keep the selected primitive view exact even beside narrower overloads.
      Value = "((" + Primitives.at(T) + ") (" + Value + "))";
    Args.push_back(std::move(Value));
    if (wordWidth(T) == 2 &&
        (Cursor + 1 >= Head.registers.size() ||
         Head.registers[Cursor + 1] != Head.registers[Cursor] + 1))
      fail("constructor prefix has a broken wide register pair");
    Cursor += wordWidth(T);
  }
  if (Cursor != Head.registers.size())
    fail("constructor prefix argument count disagrees");
  if (!pre_super_defaults.empty() && Ref->owner != C.name) {
    const bool ImmediateSuper =
        Delegation == 0 && prefix.empty() && Ref->parameters.empty();
    if (enum_projection ||
        (!ImmediateSuper &&
         !generics.superclass_calls.contains({method.reference, *Ref}) &&
         (!GuardedSuper || SuperPrologue->second.target != *Ref)))
      fail("default final fields require a proven superclass call");
    std::string Defaults;
    for (const auto &Field : pre_super_defaults) {
      const auto Zero = allocationDefaultLiteral(Field.type);
      if (!Zero)
        fail("unsupported allocation default before superclass construction");
      Defaults += "this." +
                  sourceFieldName(Field, classes, inheritance, budget) + " = " +
                  *Zero + ";\n";
    }
    // Allocation defaults precede every original preparation effect. The
    // verified prefix cannot expose uninitialized this; no later assignment
    // may overwrite a superclass callback's reflective mutation.
    prefix = Defaults + prefix;
    minimum_java_release = 25;
  }
  if (enum_projection) {
    if (enum_projection->kind != "enum-constructor" ||
        enum_projection->delegation_pc != Head.pc ||
        enum_projection->target != *Ref || !prefix.empty())
      fail("enum constructor disagrees with its initialization proof");
    if (Ref->owner == C.name) {
      Args.erase(Args.begin(), Args.begin() + 2);
      prefix = "this(" + join(Args, ", ") + ");";
    }
  } else
    prefix += std::string(Ref->owner == C.name ? "this" : "super") + "(" +
              join(Args, ", ") + ");";
  first = Delegation + 1;
  has_constructor_prologue = Delegation != 0;
  if (has_constructor_prologue) {
    // The enum plan admits one call used once by this(...) and a pure return
    // continuation. Its result must not be rematerialized after delegation.
    if (ArgumentCall) {
      std::set<unsigned> Dead;
      for (const auto &[Register, Value] : Original)
        if (Value.second == *ArgumentCall) {
          Dead.insert(Register);
          if (isWide(Value.first))
            Dead.insert(Register + 1);
        }
      for (auto Register : Dead)
        Original.erase(Register);
    }
    constructor_values = std::move(Original);
  }
  if (first == code.size())
    fail("constructor has no return boundary");
  if (!method.tries.empty() && method.tries[0].start <= Head.pc)
    fail("constructor prefix has unrepresentable exception handling");
  for (size_t I = first; I < code.size(); ++I) {
    if ((code[I].target && *code[I].target <= Head.pc) ||
        std::any_of(code[I].targets.begin(), code[I].targets.end(),
                    [&](uint32_t Target) { return Target <= Head.pc; }))
      fail("constructor body branches into its uninitialized prologue");
  }
}

std::vector<uint32_t> Body::successors(size_t Index) const {
  return normalSuccessors(code, Index);
}

bool Body::compatible(std::string Kind, const std::string &Wanted) const {
  if (starts(Kind, "self:"))
    Kind = Kind.substr(5);
  if (Kind == "?")
    return false;
  if (Wanted == "I")
    return Strings{"I", "Z", "B", "S", "C", "bits32", "zero"}.contains(Kind);
  if (Strings{"Z", "B", "S", "C"}.contains(Wanted))
    return compatible(Kind, "I");
  if (Wanted == "F")
    return Kind == "F" || Kind == "bits32" || Kind == "zero";
  if (Wanted == "J" || Wanted == "D")
    return Kind == Wanted || Kind == "bits64";
  if (isReference(Wanted)) {
    if (Kind == "zero" || Kind == "null")
      return true;
    return isAssignableReference(Kind, Wanted, classes, budget);
  }
  return Kind == Wanted;
}

std::string Body::read(const State &S, unsigned Reg, const std::string &Type,
                       bool Strict, std::string Carrier) const {
  if (Reg >= S.size())
    fail("instruction register is outside its frame");
  const auto &Kinds = S[Reg];
  if (Strict &&
      (Kinds.empty() ||
       !std::all_of(Kinds.begin(), Kinds.end(),
                    [&](const auto &K) { return compatible(K, Type); }) ||
       (wordWidth(Type) == 2 &&
        (Reg + 1 >= S.size() || S[Reg + 1] != Strings{"wide-high"}))))
    fail("undefined or incompatible register v" + std::to_string(Reg) +
         " for " + Type);
  auto R = std::to_string(Reg);
  if (Carrier.empty())
    Carrier = (isReference(Type) ? "o" : wordWidth(Type) == 2 ? "w" : "v") + R;
  if (Type == ownerClass().name &&
      ((isAnonymous(ownerClass()) && !isAnonymousLocal(ownerClass()) &&
        !type_names.namedAnonymous(ownerClass())) ||
       isEnumConstantClass(ownerClass(), classes))) {
    if (Kinds != Strings{"self:" + Type})
      fail("anonymous receiver must be exactly this");
    return "this";
  }
  if (isReference(Type))
    return "((" + sourceType(Type) + ") " + Carrier + ")";
  if (Type == "F")
    return "((java.lang.Float) null).intBitsToFloat(" + Carrier + ")";
  if (Type == "D")
    return "((java.lang.Double) null).longBitsToDouble(" + Carrier + ")";
  if (Type == "J")
    return Carrier;
  if (Type == "Z")
    return "(" + Carrier + " != 0)";
  if (Type == "B" || Type == "S" || Type == "C")
    return "((" + Primitives.at(Type) + ") " + Carrier + ")";
  return Carrier;
}

std::string Body::write(State &S, unsigned Reg, const std::string &Type,
                        const std::string &Expression,
                        bool AsExpression) const {
  unsigned Size = wordWidth(Type) == 2 || Type == "bits64" ? 2 : 1;
  if (Reg >= S.size() || Size > S.size() - Reg)
    fail("wide result exceeds its register frame");
  std::vector<unsigned> Pairs;
  for (unsigned Low = Reg ? Reg - 1 : 0;
       Low < std::min<size_t>(S.size() - 1, Reg + Size); ++Low)
    if (std::any_of(S[Low].begin(), S[Low].end(),
                    [](const auto &K) { return isWide(K); }))
      Pairs.push_back(Low);
  for (unsigned Low : Pairs)
    S[Low] = S[Low + 1] = Strings{"?"};
  auto record = [&](std::set<unsigned> *Written) {
    if (!Written)
      return;
    for (unsigned Word = Reg; Word < Reg + Size; ++Word)
      Written->insert(Word);
    for (unsigned Low : Pairs) {
      Written->insert(Low);
      Written->insert(Low + 1);
    }
  };
  record(constructor_prefix_writes);
  if (register_write_observation.state == &S)
    record(register_write_observation.words);
  if (Size == 2)
    S[Reg + 1] = Strings{"wide-high"};
  S[Reg] = Strings{Type};
  auto R = std::to_string(Reg);
  auto assign = [&](const std::string &Prefix, const std::string &Value) {
    auto Assignment = Prefix + R + " = " + Value;
    return AsExpression ? "(" + Assignment + ")" : Assignment + ";";
  };
  if (isReference(Type) || starts(Type, "new:") || Type == "null")
    return assign("o", Expression);
  if (Type == "J" || Type == "D" || Type == "bits64")
    return assign("w", registerStorage(Type, Expression));
  if (Type == "zero") {
    if (AsExpression)
      fail("ambiguous zero cannot be stored in a single source expression");
    return "v" + R + " = 0; o" + R + " = null;";
  }
  return assign("v", registerStorage(Type, Expression));
}

bool Body::bindRawDeclaringReceiver(const MethodRef &ref,
                                    const Method &declaration) {
  const auto &target = declaration.reference;
  const bool packageProtected = has(declaration.access, "protected") &&
                                has(declaration.access, "final") &&
                                packageOf(target.owner) == packageOf(method.reference.owner);
  if (ref.owner == target.owner || (!has(declaration.access, "public") && !packageProtected) ||
      has(declaration.access, "static") || has(declaration.access, "bridge") ||
      has(declaration.access, "synthetic") ||
      inheritance.bridges.contains(target) ||
      inheritance.retained_plain_bridges.contains(target) ||
      inheritance.source_method_names.contains(target) ||
      inheritance.void_unit_targets.contains(target))
    return false;
  if (raw_declaring_receivers.contains(ref))
    return true;
  const auto &owner = classes.at(target.owner);
  const auto *signature = sourceMethodSignature(target, generics);
  // A subclass can specialize an inherited class or interface parameter.
  // The resolved declaring scope restores the DEX descriptor and original
  // virtual dispatch, including a bridge's runtime argument check.
  if (!generics.raw_receiver_invocations.contains(target) ||
      !(has(declaration.access, "final") ||
        (signature && signature->type_parameters.empty())) ||
      !independentSourceScope(owner) || owner.enclosing ||
      (!has(owner.access, "public") &&
       packageOf(target.owner) != packageOf(method.reference.owner)) ||
      !isAssignableReference(ref.owner, target.owner, classes, budget))
    return false;

  // At the raw declaring class, every input has its exact descriptor type.
  // A competing source declaration with the same parameters but a different
  // JVM return descriptor could invalidate that lookup.
  std::vector<std::string> pending{target.owner};
  Strings seen;
  while (!pending.empty()) {
    budget.tick();
    auto name = std::move(pending.back());
    pending.pop_back();
    if (!seen.insert(name).second)
      continue;
    const auto cls = classes.find(name);
    if (cls == classes.end())
      return false;
    for (const auto &m : cls->second.methods) {
      budget.tick();
      if (m.reference == target)
        continue;
      const auto rename = inheritance.source_method_names.find(m.reference);
      const auto unit = inheritance.void_unit_targets.find(m.reference);
      const auto &spelling = rename != inheritance.source_method_names.end()
                                 ? rename->second.name
                             : unit != inheritance.void_unit_targets.end()
                                 ? unit->second
                                 : m.reference.name;
      if (spelling == target.name &&
          (changesMethodContract(m.reference, inheritance) ||
           sourceBridgeReference(m.reference, generics, inheritance) != m.reference ||
           (m.reference.parameters == target.parameters &&
            m.reference.returns != target.returns)))
        return false;
    }
    if (cls->second.superclass)
      pending.push_back(*cls->second.superclass);
    pending.insert(pending.end(), cls->second.interfaces.begin(),
                   cls->second.interfaces.end());
  }
  raw_declaring_receivers.emplace(ref, target.owner);
  return true;
}

// A negative subtype answer needs complete declarations. Arrays add only
// Object, Cloneable and Serializable to their component-derived supertypes.
bool Body::fixedArgumentExcludes(std::string actual, std::string other) const {
  if (isReference(actual) != isReference(other))
    return true; // The selected fixed-arity method needs no boxing.
  if (!isReference(actual) ||
      isAssignableReference(actual, other, classes, budget))
    return false;
  while (actual.starts_with('[') && other.starts_with('[')) {
    actual.erase(0, 1);
    other.erase(0, 1);
  }
  if (actual.starts_with('[') || other.starts_with('[') ||
      !isReference(actual) || !isReference(other))
    return true;
  std::vector<std::string> pending{actual};
  Strings seen;
  while (!pending.empty()) {
    budget.tick();
    auto name = std::move(pending.back());
    pending.pop_back();
    if (!seen.insert(name).second)
      continue;
    const auto cls = classes.find(name);
    if (cls == classes.end())
      return false;
    if (cls->second.superclass)
      pending.push_back(*cls->second.superclass);
    pending.insert(pending.end(), cls->second.interfaces.begin(),
                   cls->second.interfaces.end());
  }
  return true;
}

bool Body::bindInvocationFormals(const MethodRef &ref,
                                 const Method &declaration,
                                 const GenericSignature &signature) {
  const auto &target = declaration.reference;
  // A static helper and caller can declare identical independent type
  // parameters. Their original Signature and descriptor then agree on every
  // input and result, including a nested consumer of another method formal.
  // A subtype qualifier still resolves to the original declaring slot; its
  // own visible overloads participate in the source selection below.
  const bool matchingStaticScope =
      (target.owner == method.reference.owner ||
       (!classes.at(target.owner).external &&
        has(declaration.access, "public"))) &&
      (ref.owner == target.owner ||
       isAssignableReference(ref.owner, target.owner, classes, budget)) &&
      ref.parameters == method.reference.parameters &&
      ref.returns == method.reference.returns &&
      has(declaration.access, "static") && has(method.access, "static") &&
      !has(declaration.access, "varargs") &&
      !signature.type_parameters.empty() && declaration.generic_signature &&
      declaration.generic_signature == method.generic_signature;
  // A lexical call to a private implementation cannot acquire an override.
  // A package-visible synthetic helper can also be named from its own nested
  // source class. SDK synthetic members remain invisible to Java lookup.
  const auto &provider = classes.at(target.owner);
  const bool lexicalPrivate =
      ref == target && method.reference.owner == target.owner &&
      !provider.external && !provider.generic_signature &&
      has(provider.access, "final") && has(declaration.access, "private") &&
      !has(declaration.access, "synthetic") &&
      !has(declaration.access, "bridge") &&
      !has(declaration.access, "native") &&
      (has(declaration.access, "final") ||
       (has(declaration.access, "static") &&
        has(declaration.access, "varargs") && !ref.parameters.empty() &&
        ref.parameters.back().starts_with('[')));
  const bool nestedPackageCall =
      !provider.external && ref == target &&
      has(declaration.access, "static") &&
      has(declaration.access, "synthetic") &&
      (ownerClass().enclosing == target.owner ||
       (ownerClass().enclosing_method &&
        ownerClass().enclosing_method->owner == target.owner)) &&
      packageOf(method.reference.owner) == packageOf(target.owner);
  if (!expressionMemberAccessible(ref, declaration) ||
      (has(declaration.access, "synthetic") &&
       method.reference.owner != target.owner && !matchingStaticScope &&
       !nestedPackageCall) ||
      !(matchingStaticScope || lexicalPrivate ||
        staticWitnessCandidate(ref, declaration, classes.at(target.owner),
                               signature, classes, budget) ||
        virtualWitnessCandidate(ref, declaration, classes.at(target.owner),
                                 signature, budget) ||
        enumWitnessCandidate(ref, declaration, classes.at(target.owner))) ||
      !signature.result || !onlyBoundPositionProjections(signature) ||
      !signature.throws_types.empty() ||
      (declaration.declared_throws && !declaration.declared_throws->empty() &&
       !validatedClosedDeclaredThrows(declaration, generics)) ||
      changesMethodContract(target, inheritance) ||
      !generics.methods.contains(target) ||
      &generics.methods.at(target) != &signature ||
      signature.parameters.size() != ref.parameters.size() ||
      signature.types.at(*signature.result).erasure != ref.returns)
    return false;
  if (invocation_witnesses.contains(ref))
    return true;

  // Bind each method formal to its single bound's raw nominal type. After
  // substitution every argument has the original descriptor, possibly with
  // unchecked type arguments. No secondary bound may introduce a new cast.
  std::vector<std::string> witnesses;
  bool allPlainObject = true;
  for (const auto &formal : signature.type_parameters) {
    budget.tick();
    if (unsigned(formal.class_bound.has_value()) +
            formal.interface_bounds.size() !=
        1)
      return false;
    const auto id = formal.class_bound ? *formal.class_bound
                                       : formal.interface_bounds.front();
    const auto &bound = signature.types.at(id);
    if (bound.erasure != formal.erasure ||
        (bound.kind != GenericType::Kind::Class &&
         !(bound.kind == GenericType::Kind::TypeVariable &&
           bound.variable_owner == target.identity())))
      return false;
    allPlainObject &= formal.class_bound && bound.kind == GenericType::Kind::Class &&
                      formal.erasure == "Ljava/lang/Object;";
    auto type = classes.find(formal.erasure);
    if (type == classes.end())
      return false;
    for (;;) {
      budget.tick();
      const auto &entry = type->second;
      if (!independentSourceScope(entry) ||
          (!has(entry.access, "public") &&
           packageOf(entry.name) != packageOf(method.reference.owner)))
        return false;
      if (!entry.enclosing)
        break;
      if (!entry.inner_class_present || !has(entry.inner_access, "public"))
        return false;
      type = classes.find(*entry.enclosing);
      if (type == classes.end())
        return false;
    }
    witnesses.push_back(formal.erasure);
  }
  for (size_t i = 0; i < ref.parameters.size(); ++i)
    if (signature.types.at(signature.parameters[i]).erasure !=
        ref.parameters[i])
      return false;
  for (const auto &type : signature.types)
    if (type.kind == GenericType::Kind::TypeVariable &&
        type.variable_owner != target.identity())
      return false;

  const auto name = sourceMethodName(target, inheritance);
  std::vector<std::string> pending{ref.owner};
  Strings seen;
  bool found = false;
  std::vector<const Method *> competitors;
  while (!pending.empty()) {
    budget.tick();
    auto owner = std::move(pending.back());
    pending.pop_back();
    if (!seen.insert(owner).second)
      continue;
    const auto cls = classes.find(owner);
    if (cls == classes.end())
      return false;
    for (const auto &candidate : cls->second.methods) {
      budget.tick();
      const auto &slot = candidate.reference;
      if (slot == target) {
        found = !has(declaration.access, "static") || !has(cls->second.access, "interface");
        continue;
      }
      const auto alias = inheritance.source_method_names.find(slot);
      const auto unit = inheritance.void_unit_targets.find(slot);
      const auto &spelling =
          alias != inheritance.source_method_names.end() ? alias->second.name
          : unit != inheritance.void_unit_targets.end()  ? unit->second
                                                         : slot.name;
      if (spelling != name)
        continue;
      if (changesMethodContract(slot, inheritance) ||
          sourceBridgeReference(slot, generics, inheritance) != slot)
        return false;
      if (slot.parameters.size() != ref.parameters.size())
        continue;
      const auto *other = sourceMethodSignature(slot, generics);
      if (other && !other->source_projections.empty())
        return false;
      if (other && !other->type_parameters.empty() &&
          other->type_parameters.size() != witnesses.size())
        continue; // The invocation supplies explicit type arguments.
      competitors.push_back(&candidate);
    }
    if (cls->second.superclass)
      pending.push_back(*cls->second.superclass);
    pending.insert(pending.end(), cls->second.interfaces.begin(),
                   cls->second.interfaces.end());
  }
  if (!found)
    return false;
  // Package helpers in a class and its parent may differ only in the first
  // (explicit receiver) parameter. The direct superclass must forward its
  // type argument unchanged for the child parameter to be strictly narrower.
  auto narrowerStaticReceiver = [&](const Method &other) {
    const auto &slot = other.reference;
    const auto child = generics.classes.find(target.owner);
    if (ref != target || target.parameters.empty() || slot.parameters.empty() ||
        target.parameters.front() != target.owner ||
        slot.parameters.front() != slot.owner ||
        declaration.access != Access{"static", "synthetic"} ||
        other.access != declaration.access || provider.external ||
        provider.superclass != slot.owner || classes.at(slot.owner).external ||
        child == generics.classes.end() || !child->second.superclass ||
        !child->second.source_projections.empty() ||
        child->second.type_parameters.size() != 1 ||
        !declaration.generic_signature || !other.generic_signature)
      return false;
    const auto &edge = child->second.types.at(*child->second.superclass);
    if (edge.erasure != slot.owner || edge.segments.size() != 1 ||
        edge.segments.front().arguments.size() != 1)
      return false;
    const auto &argument = edge.segments.front().arguments.front();
    if (!argument.type ||
        argument.variance != GenericTypeArgument::Variance::Invariant)
      return false;
    const auto &variable = child->second.types.at(*argument.type);
    if (variable.variable_owner != target.owner ||
        variable.variable_name != child->second.type_parameters.front().name)
      return false;
    const auto &receiver = signature.types.at(signature.parameters.front());
    auto normalized = *declaration.generic_signature;
    const auto prefix = target.owner.substr(0, target.owner.size() - 1);
    if (receiver.segments.size() != 1 ||
        receiver.segments.front().arguments.size() != 1 ||
        normalized.compare(receiver.offset, prefix.size(), prefix) != 0)
      return false;
    normalized.replace(receiver.offset, prefix.size(), slot.owner, 0,
                       slot.owner.size() - 1);
    return normalized == *other.generic_signature;
  };
  auto selectsTarget = [&](const std::vector<std::string> &arguments) {
    for (const auto *candidate : competitors) {
      const auto &slot = candidate->reference;
      const auto *other = sourceMethodSignature(slot, generics);
      bool excluded = false;
      for (size_t i = 0; !excluded && i < ref.parameters.size(); ++i) {
        auto parameter = slot.parameters[i];
        if (other && other->parameters.size() == slot.parameters.size())
          if (auto index =
                  objectParameterIndex(*other, other->parameters[i], budget)) {
            const auto rank = parameter.find_first_not_of('[');
            if (rank != parameter.npos &&
                parameter.substr(rank) == "Ljava/lang/Object;")
              parameter.replace(rank, parameter.npos, arguments.at(*index));
          }
        excluded = fixedArgumentExcludes(ref.parameters[i], parameter);
      }
      if (excluded || narrowerStaticReceiver(*candidate))
        continue;
      // Otherwise the selected method must be strictly more specific for all
      // instantiations, using the sibling's reifiable Object substitution.
      if (ref.parameters == slot.parameters ||
          (other ? !reifiableObjectParameters(slot, *other, budget)
                 : candidate->generic_signature.has_value()))
        return false;
      for (size_t i = 0; i < ref.parameters.size(); ++i)
        if (ref.parameters[i] != slot.parameters[i] &&
            !isAssignableReference(ref.parameters[i], slot.parameters[i],
                                   classes, budget))
          return false;
    }
    return true;
  };
  if (!selectsTarget(witnesses)) {
    // A formal used only inside nominal type arguments changes no JVM input
    // or result type. Void can exclude a competing direct Object formal while
    // the chosen overload still receives exactly its original runtime values.
    const auto marker = classes.find("Ljava/lang/Void;");
    if (marker == classes.end() || !has(marker->second.access, "public") ||
        !has(marker->second.access, "final") ||
        marker->second.superclass != "Ljava/lang/Object;" ||
        !allPlainObject)
      return false;
    auto alternate = witnesses;
    std::fill(alternate.begin(), alternate.end(), marker->first);
    auto preserve = [&](GenericTypeId id) {
      if (auto index = objectParameterIndex(signature, id, budget))
        alternate[*index] = witnesses[*index];
    };
    for (auto id : signature.parameters)
      preserve(id);
    preserve(*signature.result);
    if (alternate == witnesses || !selectsTarget(alternate))
      return false;
    witnesses = std::move(alternate);
  }
  for (auto &type : witnesses)
    type = sourceType(type);
  invocation_witnesses.emplace(ref, "<" + join(witnesses, ", ") + ">");
  return true;
}

const Method *Body::validateInvocation(const Instruction &Op) {
  const auto *Ref = std::get_if<MethodRef>(&Op.reference);
  if (!Ref)
    fail("invocation has no method identity");
  std::string Style = baseOpcode(Op.opcode);
  if (!Strings{"invoke-static", "invoke-virtual", "invoke-interface",
               "invoke-direct", "invoke-super"}
           .contains(Style))
    fail("unsupported invocation dispatch");
  if (Ref->name == "<init>" &&
      (Style != "invoke-direct" || Ref->returns != "V"))
    fail("constructor has invalid dispatch or return type");
  const auto ScopedSuper =
      inheritance.scoped_interface_super_calls.find({method.reference, *Ref});
  const Method *Declaration = member(
      Style == "invoke-super" &&
              ScopedSuper != inheritance.scoped_interface_super_calls.end()
          ? ScopedSuper->second
          : *Ref,
      classes, budget, &generics);
  if (Declaration) {
    // A same-slot intersection override cannot silently inherit a weaker
    // generic declaration through an unrelated erased-call receipt. The
    // caller and target must both enter the shared ordered-bound authority.
    if (Style == "invoke-super" && Ref->name == method.reference.name &&
        Ref->parameters == method.reference.parameters &&
        Ref->returns == method.reference.returns &&
        generics.intersection_invocations.contains(method.reference) &&
        !generics.intersection_invocations.contains(Declaration->reference))
      fail("intersection super invocation lacks its complete target bounds");
    if (const auto Name =
            inheritance.source_method_names.find(Declaration->reference);
        Name != inheritance.source_method_names.end() &&
        Name->second.identifier_only && has(Declaration->access, "private") &&
        (Ref->owner != Declaration->reference.owner ||
         method.reference.owner != Declaration->reference.owner))
      fail(has(Declaration->access, "static")
               ? "private source static invocation requires its exact "
                 "declaring-class scope"
               : "private source instance invocation requires its exact "
                 "declaring-class scope");
    if (const auto Name =
            inheritance.source_method_names.find(Declaration->reference);
        Name != inheritance.source_method_names.end() &&
        Name->second.identifier_only && has(Declaration->access, "static") &&
        !has(Declaration->access, "public") &&
        !has(Declaration->access, "protected") &&
        !has(Declaration->access, "private") &&
        (Ref->owner != Declaration->reference.owner ||
         packageOf(method.reference.owner) !=
             packageOf(Declaration->reference.owner)))
      fail("package source static invocation requires its exact declaring "
           "class and runtime package");
    if (const auto Name =
            inheritance.source_method_names.find(Declaration->reference);
        Name != inheritance.source_method_names.end() &&
        Name->second.identifier_only &&
        !Name->second.identifier_family.empty() &&
        !Name->second.interface_identifier_family &&
        (has(Declaration->access, "synthetic") ||
         has(classes.at(Declaration->reference.owner).access, "final")) &&
        (Ref->owner != Declaration->reference.owner ||
         (has(Declaration->access, "synthetic") &&
          !has(Declaration->access, "public") &&
          packageOf(method.reference.owner) !=
              packageOf(Declaration->reference.owner))))
      fail(has(Declaration->access, "public")
               ? "closed final source invocation requires its exact declaration"
               : "package-synthetic source invocation requires its exact "
                 "declaration and runtime package");
    if (const auto Alias =
            inheritance.source_method_names.find(Declaration->reference);
        Alias != inheritance.source_method_names.end() &&
        Alias->second.identifier_only &&
        has(Declaration->access, "protected")) {
      const bool Family = protectedVirtualFamily(*Declaration);
      if ((!has(Declaration->access, "final") && !Family) ||
          (Style != "invoke-virtual" && !(Family && Style == "invoke-super")))
        fail("protected source identifier requires its closed virtual target");
      // A constant-pool reference may name an inheriting child. Prove that
      // spelling's complete lookup independently of the caller's access.
      std::map<std::string, unsigned> States;
      std::function<bool(const std::string &, unsigned)> Closed =
          [&](const std::string &Name, unsigned Depth) {
            budget.tick();
            if (Depth > 64 || States.size() > 128)
              return false;
            const auto C = classes.find(Name);
            if (C == classes.end())
              return Name == "Ljava/lang/Object;";
            auto &State = States[Name];
            if (State)
              return State == 2;
            State = 1;
            if (!independentSourceScope(C->second))
              return false;
            for (const auto &M : C->second.methods) {
              budget.tick();
              if (M.reference.name == Ref->name &&
                  M.reference != Declaration->reference &&
                  !(Family &&
                    Alias->second.identifier_family.contains(M.reference)))
                return false;
            }
            if (C->second.superclass &&
                !Closed(*C->second.superclass, Depth + 1))
              return false;
            for (const auto &I : C->second.interfaces)
              if (!Closed(I, Depth + 1))
                return false;
            State = 2;
            return true;
          };
      if (!Closed(Ref->owner, 0) ||
          !isAssignableReference(Ref->owner, Declaration->reference.owner,
                                 classes, budget) ||
          (packageOf(method.reference.owner) !=
               packageOf(Declaration->reference.owner) &&
           !isAssignableReference(method.reference.owner,
                                  Declaration->reference.owner, classes,
                                  budget)))
        fail("protected source identifier lacks its closed lookup and caller "
             "access proof");
    }
    const bool RawDeclaringReceiver =
        (Style == "invoke-virtual" || Style == "invoke-interface") &&
        bindRawDeclaringReceiver(*Ref, *Declaration);
    if (const auto *Signature =
            sourceMethodSignature(Declaration->reference, generics);
        Signature && !Signature->type_parameters.empty()) {
      const bool RawReceiver =
          generics.raw_receiver_invocations.contains(Declaration->reference) &&
          Ref->owner == Declaration->reference.owner &&
          (has(classes.at(Ref->owner).access, "public") ||
           packageOf(method.reference.owner) == packageOf(Ref->owner)) &&
          ((has(Declaration->access, "public") &&
            (Style == "invoke-virtual" || Style == "invoke-interface")) ||
           (method.reference.owner == Ref->owner &&
            ((Style == "invoke-direct" && has(Declaration->access, "private")) ||
             (Style == "invoke-virtual" && has(Declaration->access, "protected")))));
      const bool Intersection =
          generics.intersection_invocations.contains(Declaration->reference) &&
          (Style == "invoke-static" || Style == "invoke-virtual" ||
           Style == "invoke-interface" || Style == "invoke-super" ||
           (Style == "invoke-direct" && Ref->name == "<init>" &&
            *Ref == Declaration->reference));
      const auto Constructor =
          generics.superclass_call_declarations.find({method.reference, *Ref});
      const bool BoundConstructor =
          Style == "invoke-direct" && Ref->name == "<init>" &&
          method.reference.name == "<init>" && !method.instructions.empty() &&
          Op.pc == method.instructions.front().pc &&
          Constructor != generics.superclass_call_declarations.end() &&
          Constructor->second == Declaration->reference &&
          *Ref == Declaration->reference &&
          generics.superclass_calls.contains({method.reference, *Ref});
      // A constructor method formal has an operation-local argument receipt.
      // Consume it only at that original first superclass call; another
      // allocation of the same declaration receives no erased binding.
      if (!generics.erased_generic_invocations.contains(
              Declaration->reference) &&
          !Intersection && !RawReceiver && !RawDeclaringReceiver && !BoundConstructor &&
          !((Style == "invoke-static" || Style == "invoke-interface" ||
             Style == "invoke-virtual" ||
             (Style == "invoke-direct" && has(Declaration->access, "private") &&
              !Ref->name.starts_with('<'))) &&
            bindInvocationFormals(*Ref, *Declaration, *Signature)))
        fail("generic invocation requires a proven erased member binding: " +
             Declaration->reference.identity() + " at pc " +
             std::to_string(Op.pc));
      // A declaration's erased type proof does not make a private entry
      // inheritable. Check the actual call site after member resolution, so
      // adding a valid local caller cannot enable a foreign or aliased call.
      if (has(Declaration->access, "private") && Ref->name != "<init>" &&
          (Ref->owner != Declaration->reference.owner ||
           method.reference.owner != Declaration->reference.owner))
        fail("private generic invocation requires its exact declaring-class "
             "scope");
      if (has(Declaration->access, "static") &&
          !has(Declaration->access, "public") &&
          !has(Declaration->access, "protected") &&
          !has(Declaration->access, "private") &&
          (Ref->owner != Declaration->reference.owner ||
           packageOf(method.reference.owner) !=
               packageOf(Declaration->reference.owner)))
        fail("package generic static invocation requires its exact declaration "
             "and runtime package");
      if (has(Declaration->access, "static") &&
          has(Declaration->access, "protected") &&
          !expressionMemberAccessible(*Ref, *Declaration))
        fail("protected generic static invocation requires its exact "
             "declaration and an accessible caller");
    }
    if (Style == "invoke-super" && has(Declaration->access, "abstract"))
      fail("super invocation requires a concrete declared target");
    bool Static = Declaration->access.contains("static");
    if (Static != (Style == "invoke-static"))
      fail("invocation dispatch disagrees with the declared static/instance "
           "kind");
    bool Private = Declaration->access.contains("private");
    if (Private && Ref->name == "<init>" &&
        independentSourceScope(ownerClass()) &&
        independentSourceScope(classes.at(Declaration->reference.owner)) &&
        !expressionMemberAccessible(*Ref, *Declaration))
      fail("private constructor belongs to a different top-level class");
    if (Style == "invoke-direct" && Ref->name != "<init>" && !Private)
      fail("direct invocation requires a private method or constructor");
    if (Private && Style != "invoke-direct" && Style != "invoke-static")
      fail("virtual invocation cannot dispatch to a private method");
    bool Interface = classes.at(Ref->owner).access.contains("interface");
    if ((Style == "invoke-interface" || Style == "invoke-virtual") &&
        Interface != (Style == "invoke-interface"))
      fail("invocation dispatch disagrees with the declared class/interface "
           "kind");
  }
  if (Style == "invoke-super" &&
      (!ownerClass().superclass || Ref->owner != *ownerClass().superclass)) {
    const auto access =
        inheritance.interface_default_accessors.find(method.reference);
    const auto inherited_access =
        inheritance.inherited_interface_default_accessors.find(
            method.reference);
    const bool default_access =
        (access != inheritance.interface_default_accessors.end() &&
         access->second == *Ref) ||
        (inherited_access !=
             inheritance.inherited_interface_default_accessors.end() &&
         inherited_access->second.first == *Ref);
    const auto proof =
        inheritance.retained_plain_bridges.find(method.reference);
    const bool scoped_default =
        ScopedSuper != inheritance.scoped_interface_super_calls.end() &&
        Declaration && Declaration->reference == ScopedSuper->second;
    const auto key = std::pair{method.reference, *Ref};
    const auto superclass = generics.superclass_call_declarations.find(key);
    const bool inherited_super =
        Declaration && generics.superclass_calls.contains(key) &&
        superclass != generics.superclass_call_declarations.end() &&
        superclass->second == Declaration->reference;
    if (!default_access && !scoped_default && !inherited_super &&
        (proof == inheritance.retained_plain_bridges.end() ||
         !proof->second.interface_super_forwarder ||
         proof->second.target != *Ref))
      fail("super invocation lacks a proven immediate superclass or direct "
           "interface default binding");
  }
  return Declaration;
}

bool Body::protectedVirtualFamily(const Method &Declaration) const {
  const auto Alias =
      inheritance.source_method_names.find(Declaration.reference);
  // Naming owns the complete declaration-family proof. Rechecking an abstract
  // flag here would discard the same root's certified concrete overloads.
  return Alias != inheritance.source_method_names.end() &&
         Alias->second.identifier_only &&
         Alias->second.protected_identifier_family &&
         Alias->second.identifier_family.contains(Declaration.reference) &&
         Alias->second.identifier_family.contains(Alias->second.declaration);
}

std::string Body::protectedInvocationOwner(const Instruction &Op,
                                           const Method &Declaration,
                                           const State &Incoming, bool Strict) {
  // A declaration's type or spelling proof grants no access permission.
  // A qualified receiver whose proven type is the lexical caller is legal
  // in both static and instance methods. Keep that original value; casting
  // it to the declaring parent would lose cross-package protected access.
  const auto &Ref = std::get<MethodRef>(Op.reference);
  // Same-package access preserves the original receiver type. Both access
  // routes still need the complete target and source-overload proof below.
  const bool PackageAccess = packageOf(method.reference.owner) ==
                                 packageOf(Declaration.reference.owner) &&
                             expressionMemberAccessible(Ref, Declaration);
  const auto &LookupOwner = PackageAccess ? Ref.owner : method.reference.owner;
  auto Lexical = Ref;
  Lexical.owner = LookupOwner;
  const auto *Selected = member(Lexical, classes, budget, &generics);
  const bool Family = protectedVirtualFamily(Declaration);
  const auto Alias =
      inheritance.source_method_names.find(Declaration.reference);
  const auto SourceName = sourceMethodName(Declaration.reference, inheritance);
  const bool StableSpelling = Alias == inheritance.source_method_names.end() &&
                              SourceName == Declaration.reference.name;
  const auto SameTarget = [&](const MethodRef &Ref) {
    return Ref == Declaration.reference ||
           (Family && Alias->second.identifier_family.contains(Ref));
  };
  const bool Self =
      !has(method.access, "static") &&
      Incoming[Op.registers[0]] == Strings{"self:" + method.reference.owner};
  const bool Qualified =
      Incoming[Op.registers[0]] == Strings{method.reference.owner};
  if (baseOpcode(Op.opcode) != "invoke-virtual" ||
      (!PackageAccess && ((Strict && !Self && !Qualified) ||
                          !isAssignableReference(method.reference.owner,
                                                 Declaration.reference.owner,
                                                 classes, budget))) ||
      !Selected || !SameTarget(Selected->reference) ||
      (!has(Declaration.access, "final") && !Family && !StableSpelling))
    fail("protected virtual invocation requires its proven lexical "
         "receiver and proven target");
  // An unchanged source name retains ordinary virtual overriding, including
  // abstract targets. Renamed entries still need their final/family proof.
  // Explicit descriptor casts and the complete lookup below own overload
  // selection; no runtime implementation or returned subtype is inferred.
  std::vector<std::string> Pending{LookupOwner};
  Strings Seen;
  while (!Pending.empty()) {
    budget.tick();
    auto Name = std::move(Pending.back());
    Pending.pop_back();
    if (!Seen.insert(Name).second)
      continue;
    if (Seen.size() > 128)
      fail("protected virtual source ancestry exceeds its bound");
    const auto Ancestor = classes.find(Name);
    if (Ancestor == classes.end()) {
      if (Name == "Ljava/lang/Object;")
        continue;
      fail("protected virtual invocation has unknown source ancestry");
    }
    for (const auto &Candidate : Ancestor->second.methods) {
      budget.tick();
      // Comparing an unrelated name must not try to render it. Its own
      // declaration stage still validates any unplanned Java identifier.
      const auto Alias =
          inheritance.source_method_names.find(Candidate.reference);
      const auto Unit = inheritance.void_unit_targets.find(Candidate.reference);
      const auto &Name = Alias != inheritance.source_method_names.end()
                             ? Alias->second.name
                         : Unit != inheritance.void_unit_targets.end()
                             ? Unit->second
                             : Candidate.reference.name;
      if (Name == SourceName && !SameTarget(Candidate.reference)) {
        // A fixed-arity overload with a different argument count cannot
        // compete with this original call. Keep same-arity and variadic
        // siblings conservative until their complete applicability is proven.
        if (StableSpelling && !has(Candidate.access, "varargs") &&
            !has(Declaration.access, "varargs") &&
            Candidate.reference.parameters.size() !=
                Declaration.reference.parameters.size())
          continue;
        fail("protected virtual invocation has a competing source binding");
      }
    }
    if (Ancestor->second.superclass)
      Pending.push_back(*Ancestor->second.superclass);
    Pending.insert(Pending.end(), Ancestor->second.interfaces.begin(),
                   Ancestor->second.interfaces.end());
  }
  if (PackageAccess)
    return read(Incoming, Op.registers[0], Ref.owner, Strict);
  return Self ? "this"
              : read(Incoming, Op.registers[0], method.reference.owner, Strict);
}

std::string Body::staticOwner(const std::string &Owner, bool Field) {
  auto I = classes.find(Owner);
  // Check the actual reference spelling, not an inherited member's declaring
  // class. A public facade may legally expose that member, while a direct
  // foreign reference to a package-private top-level type cannot name it.
  if (I != classes.end() && !I->second.enclosing &&
      !I->second.enclosing_method && !has(I->second.access, "public") &&
      packageOf(Owner) != packageOf(method.reference.owner))
    fail("static reference owner is not accessible from the caller");
  std::string Qualified = sourceType(Owner);
  if (Field || (I != classes.end() && !I->second.access.contains("interface")))
    return "((" + Qualified + ") null)";
  std::string Root = Qualified.substr(0, Qualified.find('.'));
  Strings Names{"pc",       "caught",   "failure",
                "result32", "result64", "resultObject"};
  for (size_t N = 0; N < method.reference.parameters.size(); ++N)
    Names.insert("arg" + std::to_string(N));
  for (unsigned N = 0; N < method.registers; ++N)
    for (const auto *P : {"v", "w", "o"})
      Names.insert(P + std::to_string(N));
  std::string Current = method.reference.owner;
  Strings Seen;
  for (;;) {
    auto C = classes.find(Current);
    if (C == classes.end() || !Seen.insert(Current).second)
      break;
    budget.tick();
    for (const auto &F : C->second.fields)
      Names.insert(F.reference.name);
    if (!C->second.superclass)
      break;
    Current = *C->second.superclass;
  }
  if (Names.contains(Root))
    fail("static method type qualifier is shadowed without a proven class "
         "binding");
  return Qualified;
}

std::string Body::staticFieldRead(const FieldRef &Ref) {
  if (const auto *Declaration = member(Ref, classes, budget)) {
    if (const auto *Reader =
            type_names.enumConstantReader(Declaration->reference))
      return staticOwner(Declaration->reference.owner) + "." + *Reader + "()";
    // A constant body can inherit a static field through its unnameable binary
    // subtype. Name the proven enum declaration instead; field resolution and
    // active initialization still belong to that same declaring class.
    if (const auto *Base = type_names.enumConstantBase(Ref.owner);
        Base && Ref.owner == method.reference.owner &&
        Declaration->reference.owner == *Base)
      return staticOwner(*Base, true) + "." +
             sourceFieldName(Declaration->reference, classes, inheritance,
                             budget);
  }
  return staticOwner(Ref.owner, true) + "." +
         sourceFieldName(Ref, classes, inheritance, budget);
}

std::string Body::arrayCreation(const std::string &Type,
                                const std::string &Length) const {
  const size_t Dimensions = Type.find_first_not_of('[');
  if (Dimensions == 0 || Dimensions == std::string::npos ||
      Type.substr(Dimensions) == "V")
    fail("new-array has no array element type");
  std::string Expr =
      "new " + sourceType(Type.substr(Dimensions)) + "[" + Length + "]";
  for (size_t I = 1; I < Dimensions; ++I)
    Expr += "[]";
  return Expr;
}

std::string Body::arrayType(const State &S, unsigned Reg, bool Strict) const {
  Strings Kinds = S.at(Reg);
  for (const auto *K : {"zero", "null", "?"})
    Kinds.erase(K);
  if (Kinds.size() == 1 && starts(*Kinds.begin(), "["))
    return *Kinds.begin();
  // Every reference array supports the same length, aaload and aastore
  // operations through Object[]. Primitive arrays keep their exact types.
  if (Kinds.size() > 1 &&
      std::all_of(Kinds.begin(), Kinds.end(), [](const std::string &kind) {
        return kind.starts_with("[L") || kind.starts_with("[[");
      }))
    return "[Ljava/lang/Object;";
  if (Strict)
    fail("array operation lacks one proven element type");
  return "[I";
}

std::vector<std::string> Body::arrayPayload(const Instruction &Op,
                                            const std::string &Type,
                                            const std::string &Address) {
  Lines Out(budget);
  const auto Element = Type.substr(1);
  static const std::map<std::string, unsigned> Widths{
      {"Z", 1}, {"B", 1}, {"C", 2}, {"S", 2},
      {"I", 4}, {"F", 4}, {"J", 8}, {"D", 8}};
  auto W = Widths.find(Element);
  if (W == Widths.end() || Op.element_width != W->second)
    fail("array payload element width disagrees with its array type");
  Out.append("if (" + Address + ".length < " + std::to_string(Op.data.size()) +
             ") throw new java.lang.ArrayIndexOutOfBoundsException();");
  for (size_t I = 0; I < Op.data.size(); ++I) {
    budget.tick();
    uint64_t V = Op.data[I];
    std::string Expr;
    if (Element == "F")
      Expr =
          "((java.lang.Float) null).intBitsToFloat(" + hexLiteral(V, 32) + ")";
    else if (Element == "D")
      Expr = "((java.lang.Double) null).longBitsToDouble(" + hexLiteral(V, 64) +
             ")";
    else if (Element == "Z") {
      if (V > 1)
        fail("boolean array payload contains a non-boolean value");
      Expr = V ? "true" : "false";
    } else
      Expr = "(" + Primitives.at(Element) + ") " +
             hexLiteral(V, Element == "J" ? 64 : 32);
    Out.append(Address + "[" + std::to_string(I) + "] = " + Expr + ";");
  }
  return Out.get();
}

bool Body::expressionMemberAccessible(const MethodRef &Ref,
                                      const Method &Declaration) const {
  // Constructor prefixes and expression programs share the same access proof.
  // Nonpublic calls keep their declaring class and original runtime package,
  // including virtual references naming an inheriting ordinary class.
  // Private entries require this exact declaring-class body. Invocation
  // validation separately owns static/direct dispatch and rejects virtual
  // access to a private instance method before this access proof is consumed.
  if (has(Declaration.access, "public"))
    return true;
  if (has(Declaration.access, "private"))
    return Declaration.reference == Ref && Ref.owner == method.reference.owner;
  const auto Package = packageOf(Declaration.reference.owner);
  const bool ProtectedCallerQualifier = has(Declaration.access, "protected") &&
                                        !has(Declaration.access, "static") &&
                                        Ref.owner == method.reference.owner;
  const bool ProtectedStaticCaller =
      has(Declaration.access, "protected") &&
      has(Declaration.access, "static") &&
      isAssignableReference(method.reference.owner, Declaration.reference.owner,
                            classes, budget);
  if (Package != packageOf(method.reference.owner) &&
      !ProtectedCallerQualifier && !ProtectedStaticCaller)
    return false;
  if (Declaration.reference == Ref)
    return true;
  const auto *signature = sourceMethodSignature(Declaration.reference, generics);
  const bool closedStaticSignature =
      signature && signature->type_parameters.empty() &&
      signature->source_projections.empty() && signature->throws_types.empty() &&
      std::none_of(signature->types.begin(), signature->types.end(),
                   [](const auto &type) {
                     return type.kind == GenericType::Kind::TypeVariable;
                   });
  const bool qualifierStatic = ProtectedStaticCaller &&
      Ref.owner == method.reference.owner &&
      ((!Declaration.generic_signature && !signature) || closedStaticSignature);
  if ((has(Declaration.access, "static") && !qualifierStatic) ||
      Declaration.reference.name != Ref.name ||
      Declaration.reference.parameters != Ref.parameters ||
      Declaration.reference.returns != Ref.returns)
    return false;
  // Ordinary resolution owns the declaration and receiver contract. A
  // protected instance access through this caller's exact class qualifier
  // may cross packages. Package access still requires its original package;
  // generic edges retain their complete unchanged source proof below.
  const bool qualifierPackage = Ref.owner == method.reference.owner &&
      !has(Declaration.access, "static") && !has(Declaration.access, "protected") &&
      Package == packageOf(method.reference.owner);
  const bool ClosedDescriptorSlot =
      (has(Declaration.access, "final") || ProtectedCallerQualifier ||
       qualifierStatic || qualifierPackage) &&
      !Declaration.generic_signature &&
      !sourceMethodSignature(Declaration.reference, generics) &&
      sourceBridgeReference(Declaration.reference, generics, inheritance) ==
          Declaration.reference;
  std::string Current = Ref.owner;
  Strings Seen;
  for (;;) {
    budget.tick();
    const auto C = classes.find(Current);
    if (C == classes.end() || !Seen.insert(Current).second ||
        Seen.size() > limits::kMaxMobileExpressionMemberAncestryVisits ||
        (packageOf(Current) != Package && !ProtectedCallerQualifier &&
         !qualifierStatic) ||
        (!independentSourceScope(C->second) &&
         !(qualifierStatic && Current == method.reference.owner &&
           generics.captured_inner_classes.contains(Current))) ||
        has(C->second.access, "interface"))
      return false;
    if (qualifierStatic && closedStaticSignature)
      for (const auto &candidate : C->second.methods) {
        budget.tick();
        if (sourceMethodName(candidate.reference, inheritance) ==
                sourceMethodName(Declaration.reference, inheritance) &&
            (has(candidate.access, "varargs") ||
             (candidate.reference.parameters.size() == Ref.parameters.size() &&
              candidate.reference != Declaration.reference)))
          return false;
      }
    const auto Shape = generics.classes.find(Current);
    if (C->second.generic_signature) {
      if (!ClosedDescriptorSlot || Shape == generics.classes.end() ||
          !Shape->second.source_projections.empty() ||
          (generics.inheritance_contracts.contains(Current) &&
           !(qualifierPackage && Shape->second.type_parameters.empty())) ||
          Shape->second.type_parameters.size() >
              limits::kMaxMobileExpressionMemberFormals)
        return false;
      for (const auto &Formal : Shape->second.type_parameters) {
        if (!Formal.class_bound || !Formal.interface_bounds.empty())
          return false;
        const auto &Bound = Shape->second.types.at(*Formal.class_bound);
        if (Bound.kind != GenericType::Kind::Class ||
            Bound.erasure != "Ljava/lang/Object;" ||
            Bound.segments.size() != 1 || !Bound.segments[0].arguments.empty())
          return false;
      }
    }
    if (Current == Declaration.reference.owner)
      return true;
    if (!C->second.superclass)
      return false;
    const auto Parent = classes.find(*C->second.superclass);
    const auto ParentShape = generics.classes.find(*C->second.superclass);
    if (Parent == classes.end() ||
        (packageOf(Current) != packageOf(Parent->first) &&
         !has(Parent->second.access, "public")))
      return false;
    const size_t Arity = ParentShape == generics.classes.end()
                             ? 0
                             : ParentShape->second.type_parameters.size();
    if (Parent->second.generic_signature &&
        ParentShape == generics.classes.end())
      return false;
    if (Arity || C->second.generic_signature) {
      if (!ClosedDescriptorSlot || Shape == generics.classes.end() ||
          !Shape->second.superclass)
        return false;
      const auto &Edge = Shape->second.types.at(*Shape->second.superclass);
      if (Edge.kind != GenericType::Kind::Class ||
          Edge.erasure != *C->second.superclass || Edge.segments.size() != 1 ||
          Edge.segments[0].arguments.size() != Arity ||
          !std::all_of(Edge.segments[0].arguments.begin(),
                       Edge.segments[0].arguments.end(), [](const auto &Arg) {
                         return Arg.type &&
                                Arg.variance ==
                                    GenericTypeArgument::Variance::Invariant;
                       }))
        return false;
      // The common class-signature validator already owns these complete
      // scoped edges. A descriptor-only slot consumes no class formal,
      // so access needs no new parameter/result substitution. Requiring every
      // original edge excludes a raw hop through a generic ancestor.
    }
    Current = *C->second.superclass;
  }
}

bool Body::expressionSuperAccessible(const Instruction &Op,
                                     const Method &Declaration) const {
  const auto *Ref = std::get_if<MethodRef>(&Op.reference);
  const auto &Owner = ownerClass();
  if (!Ref || baseOpcode(Op.opcode) != "invoke-super" ||
      has(method.access, "static") || !Owner.superclass ||
      Ref->owner != *Owner.superclass || !Ref->parameters.empty() ||
      Ref->name != Declaration.reference.name ||
      Ref->parameters != Declaration.reference.parameters ||
      Ref->returns != Declaration.reference.returns ||
      !has(Declaration.access, "protected") ||
      has(Declaration.access, "private") || has(Declaration.access, "static") ||
      has(Declaration.access, "abstract") || Declaration.generic_signature)
    return false;
  // Protected lexical access follows the original class chain, even when the
  // immediate parent merely inherits this declaration. Ordinary operation
  // verification still proves the exact self receiver and result register;
  // this receipt cannot authorize a virtual call or a source substitution.
  std::string Current = Owner.name;
  Strings Seen;
  for (;;) {
    budget.tick();
    const auto C = classes.find(Current);
    if (C == classes.end() || !Seen.insert(Current).second ||
        Seen.size() > 64 || !independentSourceScope(C->second) ||
        has(C->second.access, "interface") || C->second.generic_signature)
      return false;
    if (Current == Declaration.reference.owner)
      return true;
    if (!C->second.superclass)
      return false;
    Current = *C->second.superclass;
  }
}

const Method &Body::expressionInvocation(const Instruction &Op,
                                         std::string_view Context,
                                         bool AllowVoid) {
  const auto *Ref = std::get_if<MethodRef>(&Op.reference);
  const auto *Declaration = validateInvocation(Op);
  const auto Retained =
      Declaration
          ? inheritance.retained_plain_bridges.find(Declaration->reference)
          : inheritance.retained_plain_bridges.end();
  const bool RetainedInteger =
      Retained != inheritance.retained_plain_bridges.end() &&
      Retained->second.primitive_forwarding &&
      Declaration->reference.parameters.empty() &&
      Declaration->reference.returns == "I" &&
      !Retained->second.source_signature &&
      !Retained->second.source_reference &&
      sourceBridgeReference(Declaration->reference, generics, inheritance) ==
          Declaration->reference;
  if (!Ref || !Declaration ||
      (!isReference(Ref->returns) &&
       (!Primitives.contains(Ref->returns) || Ref->returns == "V") &&
       !(AllowVoid && Ref->returns == "V")) ||
      (!expressionMemberAccessible(*Ref, *Declaration) &&
       !expressionSuperAccessible(Op, *Declaration)) ||
      (has(Declaration->access, "bridge") && !RetainedInteger) ||
      inheritance.bridges.contains(Declaration->reference) ||
      generics.intersection_invocations.contains(Declaration->reference) ||
      changesMethodContract(Declaration->reference, inheritance) ||
      inheritance.void_unit_targets.contains(Declaration->reference))
    fail(std::string(Context) +
         " requires an unchanged declared reference or scalar call: " +
         (Ref ? Ref->identity() : "<missing method>"));
  // Void statements consume the same checked receiver, dispatch and argument
  // contract as discarded nonvoid calls; they do not define a result register.
  return *Declaration;
}

std::string Body::intersectionArgumentType(
    const Instruction &Op, const State &Incoming,
    const GenericSignaturePlans::IntersectionInvocation &Contract,
    const MethodRef &Declaration) {
  const auto &Bounds = Contract.bounds;
  const bool Constructor = Declaration.name == "<init>";
  const auto *Ref = std::get_if<MethodRef>(&Op.reference);
  const auto Style = baseOpcode(Op.opcode);
  const bool Static = Style == "invoke-static";
  const bool Super = Style == "invoke-super";
  const size_t ReceiverWords = Static ? 0 : 1;
  size_t Argument = ReceiverWords;
  const auto Tail = generics.intersection_callback_tails.find(Declaration);
  unsigned Words = 0;
  if (Ref)
    for (const auto &Type : Ref->parameters)
      Words += wordWidth(Type);
  if (!Ref ||
      (!Static && !Super && Style != "invoke-virtual" &&
       Style != "invoke-interface" &&
       !(Constructor && Style == "invoke-direct" && *Ref == Declaration)) ||
      (Super &&
       (!ownerClass().superclass || has(ownerClass().access, "interface"))) ||
      Op.registers.size() != ReceiverWords + Words)
    fail("intersection generic invocation requires its original argument "
         "words and dispatch");
  for (size_t i = 0; i < Contract.parameter_index; ++i)
    Argument += wordWidth(Ref->parameters[i]);
  if (Op.registers[Argument] >= Incoming.size())
    fail("intersection generic invocation has an invalid argument word");
  // Narrowing the formal to the original nominal witness must not select
  // another Java overload. Inspect the actual source qualifier, including
  // inherited aliases, rather than only the canonical declaring class.
  // Same-erasure generic contracts must reuse the authoritative ordered-bound
  // proof.
  std::vector<std::string> Pending{Super ? *ownerClass().superclass
                                         : Ref->owner};
  Strings Seen;
  while (!Pending.empty()) {
    budget.tick();
    auto Name = std::move(Pending.back());
    Pending.pop_back();
    if (!Seen.insert(Name).second)
      continue;
    const auto C = classes.find(Name);
    if (C == classes.end()) {
      if (Name == "Ljava/lang/Object;")
        continue;
      fail("intersection generic invocation has unknown source ancestry");
    }
    for (const auto &Candidate : C->second.methods) {
      budget.tick();
      const auto &Slot = Candidate.reference;
      if (Slot.name != Ref->name)
        continue;
      // Constructors are not inherited. A different fixed arity cannot
      // compete, but a variable-arity declaration needs its own proof.
      if (Constructor) {
        if (has(Candidate.access, "varargs"))
          fail(
              "intersection generic constructor has a variable-arity overload");
        if (Slot.parameters.size() != Ref->parameters.size())
          continue;
      }
      if (Slot.parameters != Ref->parameters || Slot.returns != Ref->returns ||
          has(Candidate.access, "static") != Static ||
          changesMethodContract(Slot, inheritance) ||
          inheritance.bridges.contains(Slot))
        fail("intersection generic invocation has a competing source binding");
      // A generic child cannot override a nongeneric ancestor merely because
      // their erasures agree (the reverse override direction is permitted).
      // Requiring the complete receipt also excludes class-formal parameters
      // that would need an unproved owner substitution.
      const auto Proof = generics.intersection_invocations.find(Slot);
      const auto OtherTail = generics.intersection_callback_tails.find(Slot);
      const bool SameTail =
          Tail == generics.intersection_callback_tails.end()
              ? OtherTail == generics.intersection_callback_tails.end()
              : OtherTail != generics.intersection_callback_tails.end() &&
                    OtherTail->second == Tail->second;
      if (Proof == generics.intersection_invocations.end() ||
          Proof->second != Contract || !SameTail)
        fail("intersection generic invocation has incompatible source bounds");
    }
    if (!Constructor) {
      if (C->second.superclass)
        Pending.push_back(*C->second.superclass);
      Pending.insert(Pending.end(), C->second.interfaces.begin(),
                     C->second.interfaces.end());
    }
  }
  if (Super) {
    const auto Caller =
        generics.intersection_invocations.find(method.reference);
    const unsigned Receiver = method.registers - method.incomingWords();
    // At the first non-reentrant instruction these words still denote the
    // original source parameter and this. Use that parameter directly, so an
    // erased caller's later second-bound check stays inside the callee.
    if (Caller == generics.intersection_invocations.end() ||
        Caller->second != Contract || Contract.parameter_index != 0 ||
        Ref->parameters.size() != 1 || Ref->returns != "V" ||
        Ref->name != method.reference.name ||
        Ref->parameters != method.reference.parameters ||
        method.reference.returns != "V" || has(method.access, "static") ||
        first != 0 || Op.pc != code.front().pc ||
        nonlinear_entries.contains(Op.pc) ||
        Op.registers != std::vector<unsigned>{Receiver, Receiver + 1} ||
        Incoming.at(Receiver) != Strings{"self:" + method.reference.owner} ||
        Incoming.at(Receiver + 1) != Strings{Ref->parameters[0]} ||
        Tail != generics.intersection_callback_tails.end())
      fail("intersection super invocation lacks its original scoped parameter");
    return {}; // The unchanged typed parameter needs no nominal cast.
  }
  auto witness = [&](const Strings &Kinds) -> std::string {
    if (Kinds.size() != 1)
      return {};
    auto Type = *Kinds.begin();
    if (starts(Type, "self:"))
      Type = Type.substr(5);
    const auto C = classes.find(Type);
    if (C == classes.end() || isAnonymous(C->second))
      return {};
    // A named captured member retains its exact nominal identity inside its
    // original enclosing class. The existing capture receipt owns the source
    // scope and explicit outer field; ancestry still proves every bound.
    if (!independentSourceScope(C->second) &&
        (C->second.enclosing != method.reference.owner ||
         !generics.captured_inner_classes.contains(Type)))
      return {};
    for (const auto &Bound : Bounds)
      if (!isAssignableReference(Type, Bound, classes, budget))
        return {};
    return Type;
  };
  const auto Register = Op.registers[Argument];
  if (auto Type = witness(Incoming[Register]); !Type.empty())
    return Type;
  const auto Index = by_pc.at(Op.pc);
  if (Index >= 2) {
    const auto &Cast = code[Index - 1], &Move = code[Index - 2];
    const auto Prior = states.find(Move.pc);
    if (Cast.opcode == "check-cast" &&
        Cast.registers == std::vector<unsigned>{Register} &&
        referenceType(Cast) == Bounds.front() &&
        baseOpcode(Move.opcode) == "move-object" &&
        Move.registers.size() == 2 && Move.registers[0] == Register &&
        Prior != states.end() && Move.registers[1] < Prior->second.size() &&
        !nonlinear_entries.contains(Cast.pc) &&
        !nonlinear_entries.contains(Op.pc) && handlers(Move.pc).empty() &&
        handlers(Cast.pc).empty() && handlers(Op.pc).empty() &&
        successors(Index - 2) == std::vector<uint32_t>{Cast.pc} &&
        successors(Index - 1) == std::vector<uint32_t>{Op.pc})
      if (auto Type = witness(Prior->second[Move.registers[1]]); !Type.empty())
        return Type;
  }
  if (Tail != generics.intersection_callback_tails.end()) {
    for (size_t CastIndex = Index; CastIndex > 0 && Index - CastIndex < 64;) {
      --CastIndex;
      const auto &Cast = code[CastIndex];
      if (std::find(Cast.registers.begin(), Cast.registers.end(), Register) ==
          Cast.registers.end())
        continue;
      if (CastIndex == 0 || Cast.opcode != "check-cast" ||
          Cast.registers != std::vector<unsigned>{Register} ||
          referenceType(Cast) != Bounds.front())
        break;
      const auto &Move = code[CastIndex - 1];
      const auto Prior = states.find(Move.pc);
      bool Exact = baseOpcode(Move.opcode) == "move-object" &&
                   Move.registers.size() == 2 &&
                   Move.registers[0] == Register && Prior != states.end() &&
                   Move.registers[1] < Prior->second.size();
      for (size_t I = CastIndex - 1; Exact && I < Index; ++I) {
        budget.tick();
        Exact =
            handlers(code[I].pc).empty() &&
            (I == CastIndex - 1 || !nonlinear_entries.contains(code[I].pc)) &&
            successors(I) == std::vector<uint32_t>{code[I + 1].pc};
        if (I > CastIndex)
          Exact &= std::find(code[I].registers.begin(), code[I].registers.end(),
                             Register) == code[I].registers.end();
      }
      Exact &= handlers(Op.pc).empty() && !nonlinear_entries.contains(Op.pc);
      // The move's incoming state already joins every predecessor. Entering
      // there preserves that witness; entering after it could bypass the copy
      // or its original cast, so all later entries must remain linear.
      if (Exact)
        if (auto Type = witness(Prior->second[Move.registers[1]]);
            !Type.empty())
          return Type;
      break;
    }
  }
  fail("intersection generic invocation lacks an original argument witness for "
       "every bound");
}

std::string Body::intersectionArgument(const Instruction &Op,
                                       const State &Incoming,
                                       const MethodRef &Declaration,
                                       size_t Parameter, std::string Value) {
  const auto Proof = generics.intersection_invocations.find(Declaration);
  if (Proof == generics.intersection_invocations.end() ||
      Parameter != Proof->second.parameter_index)
    return Value;
  // The complete bound receipt and converged original word prove this nominal
  // cast cannot fail. Ordinary calls and immutable allocation expressions use
  // the same view, preserving the callee's checks and class-init order.
  const auto Type =
      intersectionArgumentType(Op, Incoming, Proof->second, Declaration);
  if (Type.empty())
    return "arg" + std::to_string(Parameter);
  return "((" + sourceType(Type) + ") " + Value + ")";
}

Method Body::staticSuperAccessor(const std::string &HelperName) {
  const auto &R = method.reference;
  // A captured named member already has an authoritative constructor/field
  // receipt and is emitted as a static member with that explicit capture.
  // Only the accessor owner may use it; superclass scopes remain independent.
  const auto sourceScope = [&](const Class &C) {
    return independentSourceScope(C) ||
           (C.name == R.owner &&
            generics.captured_inner_classes.contains(C.name));
  };
  // A static implementation can invoke its immediate superclass in just one
  // branch. Move only that dispatch into an instance helper. Its parameters
  // use the original raw nominal carriers, so class arguments in the target
  // Signature introduce no additional checked conversion.
  if (method.access == Access{"static", "synthetic"} && method.generic_signature &&
      !R.parameters.empty() && R.parameters.front() == R.owner &&
      R.returns == "Ljava/lang/Object;" && method.tries.empty() &&
      sourceScope(ownerClass()) && !has(ownerClass().access, "interface") &&
      !isAnonymous(ownerClass()) && ownerClass().superclass) {
    const unsigned receiver = method.registers - method.incomingWords();
    std::vector<unsigned> words;
    for (unsigned word = receiver; word < method.registers; ++word)
      words.push_back(word);
    const Instruction *dispatch = nullptr;
    bool unchanged = true;
    for (const auto &op : code) {
      budget.tick();
      const auto kind = baseOpcode(op.opcode);
      if (kind == "invoke-super") {
        if (dispatch || op.registers != words)
          unchanged = false;
        dispatch = &op;
      }
      if (op.registers.empty() || anyPrefix(kind, {"invoke-", "if-", "return"}))
        continue;
      const auto dest = op.registers.front();
      if (dest == receiver ||
          (dest + 1 == receiver && kind != "move-result-object" &&
           kind != "move-object" && kind != "sget-object" &&
           kind != "iget-object" && kind != "check-cast"))
        unchanged = false;
    }
    const auto *ref = dispatch ? std::get_if<MethodRef>(&dispatch->reference) : nullptr;
    if (unchanged && ref && ref->owner == *ownerClass().superclass &&
        ref->returns == R.returns &&
        std::equal(R.parameters.begin() + 1, R.parameters.end(),
                   ref->parameters.begin(), ref->parameters.end())) {
      const auto *target = validateInvocation(*dispatch);
      const auto *shape = target ? sourceMethodSignature(target->reference, generics) : nullptr;
      const auto overrideRef = MethodRef{R.owner, ref->name, ref->parameters, ref->returns};
      const auto child = std::find_if(ownerClass().methods.begin(), ownerClass().methods.end(),
                                     [&](const Method &m) { return m.reference == overrideRef; });
      bool exact = target && target->reference == *ref &&
                   (has(target->access, "public") || has(target->access, "protected")) &&
                   !has(target->access, "abstract") && !has(target->access, "bridge") &&
                   !changesMethodContract(*ref, inheritance) &&
                   shape && shape->type_parameters.empty() && shape->result &&
                   shape->source_projections.empty() && shape->throws_types.empty() &&
                   shape->types.at(*shape->result).kind == GenericType::Kind::Class &&
                   shape->types.at(*shape->result).erasure == ref->returns &&
                   shape->parameters.size() == ref->parameters.size() &&
                   child != ownerClass().methods.end() && has(child->access, "public") &&
                   !has(child->access, "static") && !has(child->access, "bridge");
      for (size_t i = 0; exact && i < shape->parameters.size(); ++i) {
        const auto &type = shape->types.at(shape->parameters[i]);
        exact = type.kind == GenericType::Kind::Class && type.erasure == ref->parameters[i];
      }
      // Check all reachable overloads. The same descriptor in a parent or
      // interface still resolves to the concrete immediate superclass slot.
      Strings seen;
      std::vector<std::string> pending{ref->owner};
      while (exact && !pending.empty()) {
        budget.tick();
        const auto name = pending.back();
        pending.pop_back();
        if (!seen.insert(name).second)
          continue;
        const auto known = classes.find(name);
        if (known == classes.end()) {
          exact = name == "Ljava/lang/Object;";
          continue;
        }
        for (const auto &m : known->second.methods) {
          budget.tick();
          if (sourceMethodName(m.reference, inheritance) == sourceMethodName(*ref, inheritance) &&
              (has(m.access, "varargs") ||
               (m.reference.parameters.size() == ref->parameters.size() &&
                m.reference.parameters != ref->parameters)))
            exact = false;
        }
        if (known->second.superclass)
          pending.push_back(*known->second.superclass);
        pending.insert(pending.end(), known->second.interfaces.begin(), known->second.interfaces.end());
      }
      if (exact) {
        Method helper;
        helper.reference = {R.owner, HelperName, ref->parameters, ref->returns};
        helper.access = {"private"};
        helper.registers = method.registers;
        helper.instructions = {*dispatch, {}, {}};
        helper.instructions[0].pc = 0;
        helper.instructions[1].pc = 3;
        helper.instructions[1].opcode = "move-result-object";
        helper.instructions[1].registers = {0};
        helper.instructions[2].pc = 4;
        helper.instructions[2].opcode = "return-object";
        helper.instructions[2].registers = {0};
        helper.code_end = 5;
        erased_super_helper_target = *ref;
        static_super_helper = {dispatch->pc, HelperName};
        return helper;
      }
    }
  }
  const bool ReturnsValue = R.returns != "V";
  const size_t TailSize = ReturnsValue ? 3 : 2;
  const bool Checked =
      has(method.access, "private") && code.size() == TailSize + 2;
  const bool Synthetic =
      has(method.access, "synthetic") && !has(method.access, "private") &&
      !has(method.access, "protected") && code.size() == TailSize;
  const bool ReceiverOnly = R.parameters == std::vector<std::string>{R.owner};
  const bool ReceiverFirst =
      Synthetic && !R.parameters.empty() && R.parameters.front() == R.owner;
  if ((!Checked && !Synthetic) || !has(method.access, "static") ||
      has(method.access, "bridge") || has(method.access, "synchronized") ||
      has(method.access, "native") || has(method.access, "abstract") ||
      method.generic_signature || !sourceScope(ownerClass()) ||
      isAnonymous(ownerClass()) || (!ReceiverOnly && !ReceiverFirst) ||
      !method.tries.empty())
    fail("static super accessor requires verified receiver/argument/result "
         "forwarding");
  const auto Receiver = method.registers - method.incomingWords();
  std::vector<unsigned> Words;
  for (unsigned Word = Receiver; Word < method.registers; ++Word)
    Words.push_back(Word);
  const std::vector<std::string> Forwarded(R.parameters.begin() + 1,
                                           R.parameters.end());
  const size_t Dispatch = Checked ? 2 : 0;
  if (baseOpcode(code[Dispatch].opcode) != "invoke-super" ||
      code[Dispatch].registers != Words ||
      std::any_of(code.begin(), code.end(), [](const auto &Op) {
        return Op.target || !Op.targets.empty();
      }))
    fail("static super accessor changed its original forwarding body");
  if (ReturnsValue) {
    const bool Reference = isReference(R.returns);
    const bool Wide = wordWidth(R.returns) == 2;
    const auto &Move = code[Dispatch + 1], &Return = code.back();
    if (Move.opcode != (Reference ? "move-result-object"
                        : Wide    ? "move-result-wide"
                                  : "move-result") ||
        Return.opcode != (Reference ? "return-object"
                          : Wide    ? "return-wide"
                                    : "return") ||
        Move.registers.size() != 1 || Return.registers != Move.registers)
      fail("static super accessor changed its original result carrier");
  } else if (code.back().opcode != "return-void" ||
             !code.back().registers.empty()) {
    fail("static super accessor changed its original void return");
  }
  if (Checked) {
    const auto &Literal = code[0];
    const auto &Check = code[1];
    const auto *Ref = std::get_if<MethodRef>(&Check.reference);
    if ((Literal.opcode != "const-string" &&
         Literal.opcode != "const-string/jumbo") ||
        Literal.registers.size() != 1 || Literal.registers[0] == Receiver ||
        !std::holds_alternative<std::string>(Literal.literal) ||
        baseOpcode(Check.opcode) != "invoke-static" || !Ref ||
        Ref->parameters != std::vector<std::string>{"Ljava/lang/Object;",
                                                    "Ljava/lang/String;"} ||
        Ref->returns != "V" ||
        Check.registers !=
            std::vector<unsigned>{Receiver, Literal.registers[0]})
      fail("static super accessor lacks its unchanged parameter-check prefix");
    expressionInvocation(Check, "static super parameter check", true);
  }
  const auto *Target = validateInvocation(code[Dispatch]);
  const bool Interface = has(ownerClass().access, "interface");
  const auto *Original = std::get_if<MethodRef>(&code[Dispatch].reference);
  const auto Proof =
      Original ? inheritance.scoped_interface_super_calls.find({R, *Original})
               : inheritance.scoped_interface_super_calls.end();
  const bool ScopedInterfaceTarget =
      Interface && Target &&
      Proof != inheritance.scoped_interface_super_calls.end() &&
      Proof->second == Target->reference;
  const auto Qualifier = inheritance.static_interface_super_qualifiers.find(R);
  // The shared name receipt distinguishes spelling from contract adaptation.
  // An identifier-only alias keeps this exact forwarding descriptor and the
  // already selected super declaration; other projections remain forbidden.
  if (!Target || Target->reference.parameters != Forwarded ||
      Target->reference.returns != R.returns ||
      (Target->generic_signature && !ScopedInterfaceTarget) ||
      has(Target->access, "abstract") || has(Target->access, "bridge") ||
      (!has(Target->access, "public") && !has(Target->access, "protected")) ||
      changesMethodContract(Target->reference, inheritance) ||
      inheritance.bridges.contains(Target->reference) ||
      inheritance.retained_plain_bridges.contains(Target->reference) ||
      inheritance.void_unit_targets.contains(Target->reference))
    fail("static super accessor requires an unchanged concrete superclass "
         "declaration");
  if (Interface) {
    if (Checked || !Synthetic ||
        Qualifier == inheritance.static_interface_super_qualifiers.end() ||
        !ScopedInterfaceTarget)
      fail("static interface super accessor lacks its shared default receipt");
    minimum_java_release = std::max(minimum_java_release, 9u);
  }
  // Member resolution may also search interfaces. A class accessor requires
  // the exact declaration on its complete, unchanged class ancestry.
  auto Current = R.owner;
  Strings Seen;
  while (!Interface) {
    budget.tick();
    const auto C = classes.find(Current);
    if (C == classes.end() || !Seen.insert(Current).second ||
        !sourceScope(C->second) || has(C->second.access, "interface") ||
        generics.classes.contains(Current))
      fail("static super accessor requires a complete nongeneric class chain");
    if (Current == Target->reference.owner)
      break;
    if (!C->second.superclass)
      fail("static super accessor target is outside its class ancestry");
    Current = *C->second.superclass;
  }
  // Dropping the explicit owner parameter and making this an instance body
  // leaves every incoming register word in place. The ordinary Body verifier
  // now checks the same invoke-super using the actual self receiver.
  Method Helper;
  Helper.reference = {R.owner, HelperName, Forwarded, R.returns};
  Helper.access = {"private"};
  Helper.registers = method.registers;
  Helper.instructions.assign(code.begin() + Dispatch, code.end());
  if (Interface)
    std::get<MethodRef>(Helper.instructions[0].reference).owner =
        Qualifier->second;
  Helper.code_end = method.code_end;
  static_super_helper = {code[Dispatch].pc, HelperName};
  return Helper;
}

std::vector<std::pair<FieldRef, std::string>> Body::interfaceInitializers() {
  if (method.reference.name != "<clinit>" || code.empty() ||
      final_writes.empty() || !method.tries.empty() ||
      code.back().opcode != "return-void")
    fail("interface initializer requires a final-field body with a normal exit");
  using BooleanSelection =
      std::tuple<size_t, ConstructorPrefixConstant, ConstructorPrefixConstant>;
  std::map<size_t, BooleanSelection> Selections;
  std::set<size_t> SelectionJumps;
  if (std::any_of(code.begin(), code.end(), [](const Instruction &Op) {
        return Op.target || !Op.targets.empty();
      })) {
    if (std::any_of(final_writes.begin(), final_writes.end(),
                    [](const FieldRef &Field) { return Field.type != "Z"; }))
      fail("conditional interface initialization requires boolean finals");
    // The common final-write planner owns complete forward regions and every
    // normal exit. This expression profile consumes only its exact two-literal
    // selections; it does not rediscover or relax control-flow ownership.
    const auto Flow =
        finalWriteRegions(method, final_writes, budget, false, true);
    for (const auto &[At, Diamond] : Flow.diamonds) {
      budget.tick();
      if (!Diamond.guards.empty() || Diamond.taken != At + 3 ||
          Diamond.join != At + 4 || Diamond.join >= code.size() ||
          (Diamond.taken_end && *Diamond.taken_end != Diamond.join) ||
          !Flow.structured_jumps.contains(At + 2))
        fail("interface initializer requires a complete boolean selection");
      const auto Fallthrough = integerConstant(code[At + 1], method.registers);
      const auto Taken = integerConstant(code[Diamond.taken], method.registers);
      const auto Boolean = [](const auto &Value) {
        return Value && Value->type != "bits64" &&
               (Value->bits == 0 || Value->bits == 1);
      };
      if (!Boolean(Fallthrough) || !Boolean(Taken) ||
          Fallthrough->destination != Taken->destination)
        fail("interface initializer selection must define one boolean value");
      Selections.emplace(At,
                         BooleanSelection{Diamond.join, *Fallthrough, *Taken});
      SelectionJumps.insert(At + 2);
    }
  }
  for (size_t I = 0; I < code.size(); ++I) {
    const auto &Op = code[I];
    if (((Op.target || !Op.targets.empty()) && !Selections.contains(I) &&
         !SelectionJumps.contains(I)) ||
        (starts(Op.opcode, "return") && I + 1 != code.size()))
      fail("interface initializer has an unowned branch or early return");
  }
  verify();
  struct Expression {
    std::string text;
    std::vector<uint32_t> effects;
    bool zero = false;
    bool boolean = false;
  };
  std::map<unsigned, Expression> Values;
  std::vector<uint32_t> Original, Emitted;
  std::set<FieldRef> Written;
  std::set<uint32_t> ResolvedStrings;
  std::vector<std::pair<FieldRef, std::string>> Result;
  auto recordEffect = [&](Expression &Value, uint32_t PC) {
    Value.effects.push_back(PC);
    Original.push_back(PC);
  };
  auto prepare = [&](const Instruction &Op) {
    Expression Value;
    const auto Base = baseOpcode(Op.opcode);
    if (Op.opcode == "const" || starts(Op.opcode, "const/") ||
        starts(Op.opcode, "const-wide")) {
      const auto Literal = std::get<int64_t>(Op.literal);
      Value.text = hexLiteral(static_cast<uint64_t>(Literal),
                              starts(Op.opcode, "const-wide") ? 64 : 32);
      Value.zero = Literal == 0 && !starts(Op.opcode, "const-wide");
      Value.boolean =
          !starts(Op.opcode, "const-wide") && (Literal == 0 || Literal == 1);
    } else if (Op.opcode == "const-string" || Op.opcode == "const-string/jumbo") {
      Value.text = javaString(std::get<std::string>(Op.literal));
      recordEffect(Value, Op.pc);
    } else if (Base == "move" || Base == "move-object" || Base == "move-wide") {
      const auto Source = Values.find(Op.registers.at(1));
      if (Source == Values.end())
        fail("interface initializer copies an unavailable expression");
      Value = Source->second;
    } else {
      return false;
    }
    Values[Op.registers.at(0)] = std::move(Value);
    return true;
  };
  for (size_t I = 0; I < code.size(); ++I) {
    budget.tick();
    const auto &Op = code[I];
    const auto Base = baseOpcode(Op.opcode);
    Expression Value;
    auto getAt = [&](uint32_t PC, unsigned Reg, const std::string &Type) {
      auto Source = Values.find(Reg);
      if (Source == Values.end())
        fail("interface initializer reads an unavailable expression");
      const auto &Expr = Source->second;
      budget.tick(Expr.effects.size());
      Value.effects.insert(Value.effects.end(), Expr.effects.begin(),
                           Expr.effects.end());
      if (Value.effects.size() > code.size())
        fail("interface initializer duplicates original effects");
      return read(states.at(PC), Reg, Type, true,
                  Expr.zero && isReference(Type) ? "null" : Expr.text);
    };
    auto get = [&](unsigned Reg, const std::string &Type) {
      return getAt(Op.pc, Reg, Type);
    };
    auto effect = [&] { recordEffect(Value, Op.pc); };
    if (const auto Selection = Selections.find(I);
        Selection != Selections.end()) {
      const auto &[Join, Fallthrough, Taken] = Selection->second;
      const auto Test = condition(Op, states.at(Op.pc), true, get);
      // Keep the merged value in the original integer carrier. The ordinary
      // typed read owns conversion to Z at the final store; comparison inputs
      // and their effects are evaluated exactly once in original order.
      Value.text = "(" + Test + " ? " + hexLiteral(Taken.bits, 32) + " : " +
                   hexLiteral(Fallthrough.bits, 32) + ")";
      Value.boolean = true;
      Values[Taken.destination] = std::move(Value);
      I = Join - 1;
    } else if (prepare(Op)) {
      continue;
    } else if (Op.opcode == "new-instance") {
      const auto Receiver = Op.registers.at(0);
      Values.erase(Receiver);
      effect();
      // A final initializer may construct a collection from an array literal.
      // The array and each static field are evaluated inside the original
      // allocation, in index order. Require the complete terminal expression
      // so no temporary array, index, or field register escapes it.
      const auto literalFactory = [&]() -> std::optional<size_t> {
        if (I + 7 >= code.size() || baseOpcode(code[I + 1].opcode) != "const" ||
            code[I + 2].opcode != "new-array")
          return {};
        const auto &length = code[I + 1], &array = code[I + 2];
        const auto *count = std::get_if<int64_t>(&length.literal);
        const auto *type = std::get_if<std::string>(&array.reference);
        if (!count || *count < 1 || uint64_t(*count) > (code.size() - I - 7) / 3 ||
            !type || !type->starts_with("[L") || length.registers.size() != 1 ||
            array.registers != std::vector<unsigned>{length.registers[0], length.registers[0]} ||
            array.registers[0] == Receiver)
          return {};
        std::vector<uint32_t> effects{array.pc};
        std::vector<std::string> elements;
        size_t at = I + 3;
        for (int64_t n = 0; n < *count; ++n, at += 3) {
          budget.tick();
          const auto &index = code[at], &read = code[at + 1], &store = code[at + 2];
          const auto *field = std::get_if<FieldRef>(&read.reference);
          if (baseOpcode(index.opcode) != "const" || index.literal != Literal{n} ||
              index.registers.size() != 1 || read.opcode != "sget-object" ||
              !field || read.registers.size() != 1 || store.opcode != "aput-object" ||
              store.registers != std::vector<unsigned>{read.registers[0], array.registers[0],
                                                       index.registers[0]} ||
              std::set<unsigned>{Receiver, read.registers[0], array.registers[0],
                                 index.registers[0]}.size() != 4 ||
              !isAssignableReference(field->type, type->substr(1), classes, budget))
            return {};
          elements.push_back(staticFieldRead(*field));
          effects.push_back(read.pc);
          effects.push_back(store.pc);
        }
        if (at + 5 != code.size())
          return {};
        const auto &factory = code[at], &move = code[at + 1], &init = code[at + 2];
        const auto *call = std::get_if<MethodRef>(&factory.reference);
        const auto *ctor = std::get_if<MethodRef>(&init.reference);
        if (baseOpcode(factory.opcode) != "invoke-static" || !call ||
            call->parameters.size() != 1 || !call->parameters[0].starts_with("[L") ||
            !isAssignableReference(*type, call->parameters[0], classes, budget) ||
            factory.registers != std::vector<unsigned>{array.registers[0]} ||
            move.opcode != "move-result-object" || move.registers.size() != 1 ||
            move.registers[0] == Receiver || !ctor || ctor->parameters.size() != 1 ||
            !allocationInitializer(Op, init) ||
            !constructors.contains(init.pc) || constructors.at(init.pc) != Op.pc ||
            init.registers != std::vector<unsigned>{Receiver, move.registers[0]} ||
            !isAssignableReference(call->returns, ctor->parameters[0], classes, budget) ||
            code[at + 3].opcode != "sput-object" ||
            code[at + 3].registers != std::vector<unsigned>{Receiver} ||
            code[at + 4].opcode != "return-void")
          return {};
        const auto &producer = expressionInvocation(factory, "interface array factory");
        const auto &constructor = expressionInvocation(init, "interface array constructor", true);
        if ((producer.declared_throws && !producer.declared_throws->empty()) ||
            (constructor.declared_throws && !constructor.declared_throws->empty()))
          return {};
        const auto literal = "new " + sourceType(*type) + " {" + join(elements, ", ") + "}";
        const auto made = staticOwner(call->owner) + "." + invocationName(*call, &producer) +
                          "((" + sourceType(call->parameters[0]) + ") " + literal + ")";
        Value.text = allocationExpression(ctor->owner, {"((" + sourceType(ctor->parameters[0]) +
                                                       ") " + made + ")"});
        effects.push_back(factory.pc);
        effects.push_back(init.pc);
        for (auto pc : effects)
          recordEffect(Value, pc);
        return at + 2;
      };
      if (const auto end = literalFactory()) {
        Values[Receiver] = std::move(Value);
        I = *end;
        continue;
      }
      size_t InitAt = I + 1;
      while (InitAt < code.size()) {
        budget.tick();
        const auto &Prep = code[InitAt];
        if (Prep.opcode == "nop") {
          ++InitAt;
          continue;
        }
        if (!prepare(Prep))
          break;
        if (Prep.registers.at(0) == Receiver)
          fail("interface allocation preparation overwrites its receiver");
        ++InitAt;
      }
      if (InitAt >= code.size())
        fail("interface allocation lacks its initializer");
      const auto &Init = code[InitAt];
      const auto *Ref = std::get_if<MethodRef>(&Init.reference);
      const auto Bound = constructors.find(Init.pc);
      // The ordinary verifier owns concrete type, uninitialized identity,
      // constructor dispatch, overloads and words. This expression consumes
      // the exact receiver and the original ordered literal preparations.
      if (!Ref || Ref->name != "<init>" ||
          !concreteAllocationClass(Ref->owner) ||
          baseOpcode(Init.opcode) != "invoke-direct" ||
          Init.registers.empty() || Init.registers.front() != Receiver ||
          Bound == constructors.end() || Bound->second != Op.pc)
        fail("interface allocation requires its proven constructor span");
      const auto &Declaration =
          expressionInvocation(Init, "interface allocation", true);
      if (!Ref->parameters.empty()) {
        const auto &Allocated = classes.at(Ref->owner);
        if (!independentSourceScope(Allocated) || isAnonymous(Allocated) ||
            Allocated.generic_signature || Declaration.generic_signature ||
            has(Declaration.access, "static") ||
            has(Declaration.access, "native") ||
            has(Declaration.access, "abstract") ||
            has(Declaration.access, "bridge") ||
            has(Declaration.access, "synthetic") ||
            has(Declaration.access, "varargs") ||
            !std::all_of(Ref->parameters.begin(), Ref->parameters.end(),
                         [](const std::string &Type) {
                           return (Type != "V" && Primitives.contains(Type)) ||
                                  Type == "Ljava/lang/String;";
                         }))
          fail("interface allocation arguments require a descriptor-only "
               "scalar or String constructor");
      }
      if ((Declaration.declared_throws &&
           !Declaration.declared_throws->empty()) ||
          generics.declared_throws.contains(Declaration.reference))
        fail("interface allocation requires no declared checked throws");
      std::vector<std::string> Args;
      size_t Cursor = 1;
      for (const auto &Type : Ref->parameters) {
        budget.tick();
        const auto Register = Init.registers.at(Cursor);
        const auto Source = Values.find(Register);
        if (Type == "Z" && (Source == Values.end() || !Source->second.boolean))
          fail("interface allocation boolean needs complete zero/one origins");
        Args.push_back(getAt(Init.pc, Register, Type));
        Cursor += wordWidth(Type);
      }
      if (Cursor != Init.registers.size())
        fail("interface allocation arguments change original words");
      Value.text = allocationExpression(Ref->owner, Args);
      recordEffect(Value, Init.pc);
      Values[Receiver] = std::move(Value);
      I = InitAt;
    } else if (Base == "invoke-static" || Base == "invoke-virtual" ||
               Base == "invoke-interface") {
      const auto &Declaration =
          expressionInvocation(Op, "interface initializer");
      if ((Declaration.declared_throws &&
           !Declaration.declared_throws->empty()) ||
          generics.declared_throws.contains(Declaration.reference))
        fail("interface initializer call requires no declared checked throws");
      const auto &Ref = std::get<MethodRef>(Op.reference);
      if (!isReference(Ref.returns) && Ref.returns != "I")
        fail(
            "interface initializer call result requires a reference or integer "
            "carrier");
      size_t Cursor = Base == "invoke-static" ? 0 : 1;
      Value.text = (Cursor ? virtualReceiver(Ref, Op, get(Op.registers.at(0), Ref.owner))
                           : staticOwner(Ref.owner)) +
                   "." + invocationName(Ref);
      std::vector<std::string> Args;
      for (const auto &Type : Ref.parameters) {
        Args.push_back(get(Op.registers.at(Cursor), Type));
        Cursor += wordWidth(Type);
      }
      Value.text += "(" + invocationArguments(Ref, Args) + ")";
      if (Value.text.size() > 1024 * 1024)
        fail("interface initializer expression exceeds its size limit");
      effect();
      if (++I >= code.size() ||
          code[I].opcode !=
              (isReference(Ref.returns) ? "move-result-object" : "move-result"))
        fail("interface initializer call lacks its adjacent result");
      Values[code[I].registers.at(0)] = std::move(Value);
    } else if (Op.opcode == "sget-object" || Op.opcode == "sget") {
      const auto &Ref = std::get<FieldRef>(Op.reference);
      if (!isReference(Ref.type) && Ref.type != "I")
        fail("interface initializer field read has an unsupported carrier");
      Value.text = staticFieldRead(Ref);
      effect();
      Values[Op.registers.at(0)] = std::move(Value);
    } else if (Op.opcode == "sput-object" || Op.opcode == "sput" ||
               Op.opcode == "sput-boolean") {
      const auto &Ref = std::get<FieldRef>(Op.reference);
      const auto Generic = generics.fields.find(Ref);
      const bool SupportedGeneric = [&] {
        if (Generic == generics.fields.end())
          return true;
        const auto &Signature = Generic->second;
        if (!Signature.field_type || !Signature.source_projections.empty() ||
            std::any_of(Signature.types.begin(), Signature.types.end(),
                        [&](const GenericType &Type) {
                          budget.tick();
                          return Type.kind == GenericType::Kind::TypeVariable ||
                                 (Type.kind == GenericType::Kind::Class &&
                                  !classes.contains(Type.erasure));
                        }))
          return false;
        const auto &Type = Signature.types.at(*Signature.field_type);
        // Canonical signature validation owns complete declarations, bounds
        // and slots. Require a genuine closed parameterized source type: a
        // redundant raw Signature cannot be reproduced by the Java compiler.
        return Type.kind == GenericType::Kind::Class &&
               Type.segments.size() == 1 &&
               !Type.segments.front().arguments.empty();
      }();
      if ((!isReference(Ref.type) && Ref.type != "I" && Ref.type != "Z") ||
          Ref.owner != method.reference.owner || !final_writes.contains(Ref) ||
          !SupportedGeneric || !Written.insert(Ref).second)
        fail("interface initializer requires one write to its supported blank "
             "final");
      const auto Source = Values.find(Op.registers.at(0));
      if (Ref.type == "Z" &&
          (Source == Values.end() || !Source->second.boolean))
        fail("interface initializer boolean needs complete zero/one origins");
      Value.text =
          finalFieldSourceValue(Ref, get(Op.registers.at(0), Ref.type));
      effect();
      for (uint32_t pc : Value.effects) {
        budget.tick();
        // Reusing a previously resolved constant loads the same interned
        // String. Its first resolution must still occur in the original order.
        if (!starts(code[by_pc.at(pc)].opcode, "const-string") ||
            ResolvedStrings.insert(pc).second)
          Emitted.push_back(pc);
      }
      Result.emplace_back(Ref, std::move(Value.text));
    } else if (Op.opcode != "nop" && Op.opcode != "return-void")
      fail("interface initializer has unsupported preparation: " + Op.opcode);
  }
  if (Result.empty() || Written != final_writes || Emitted != Original)
    fail("interface initializer changes original effects or field writes");
  return Result;
}

void Body::planMonitor() {
  std::set<size_t> Enters, Exits;
  for (size_t I = 0; I < code.size(); ++I) {
    budget.tick();
    if (code[I].opcode == "monitor-enter")
      Enters.insert(I);
    else if (code[I].opcode == "monitor-exit")
      Exits.insert(I);
  }
  if (Enters.empty() && Exits.empty())
    return;
  const bool StaticFinal = method.reference.name == "<clinit>" &&
                           has(method.access, "static") &&
                           !final_writes.empty();
  // One physical release can suffice for a total body or an exception-only
  // lifetime. The held-stack and must-alias proofs still own every path and
  // the released object, including complete exceptional cleanup coverage.
  const bool SingleRelease = Enters.size() == 1 && Exits.size() == 1;
  if (Enters.empty() || Enters.size() > 16 || code.size() < 3 ||
      (!SingleRelease && (Exits.size() < Enters.size() * 2 || code.size() < 6)) ||
      ((method.reference.name.starts_with('<') || !final_writes.empty()) &&
       !StaticFinal))
    fail("monitor region requires acquisitions and proven releases");
  auto Plain = [](const Instruction &Op) {
    return std::holds_alternative<std::monostate>(Op.reference) &&
           std::holds_alternative<std::monostate>(Op.literal) &&
           Op.targets.empty() && Op.keys.empty() && Op.data.empty() &&
           !Op.element_width;
  };
  auto RegisterCopy = registerCopyShape;
  for (const auto I : Enters)
    if (code[I].registers.size() != 1 || code[I].target || !Plain(code[I]))
      fail("monitor acquisition has malformed operands or metadata");
  for (const auto I : Exits)
    if (code[I].registers.size() != 1 || code[I].target || !Plain(code[I]))
      fail("monitor release has malformed operands or metadata");
  auto NonThrowing = [&](const Instruction &Op) {
    const auto Kind = baseOpcode(Op.opcode);
    if (Kind == "move" || Kind == "move-object" || Kind == "move-wide")
      return RegisterCopy(Op);
    // Use the same exception classification as ordinary dataflow and
    // allocation ownership. Integer/long division and type resolution stay
    // throwing; total arithmetic, comparisons and result copies do not.
    return !canThrow(Op);
  };
  auto stackScopes = [&] {
    using Stack = std::vector<size_t>;
    std::map<size_t, Stack> Frames{{0, {}}};
    std::vector<size_t> Pending{0};
    std::map<size_t, MonitorRegion> Regions;
    std::map<size_t, size_t> Releases;
    bool Nested = false, Typed = false;
    auto edge = [&](size_t Index, const Stack &Held, bool Exceptional) {
      if (Index >= code.size() ||
          (!Exceptional && code[Index].opcode == "move-exception"))
        return false;
      const auto [At, Inserted] = Frames.emplace(Index, Held);
      if (!Inserted && At->second != Held)
        return false;
      if (Inserted)
        Pending.push_back(Index);
      return true;
    };
    while (!Pending.empty()) {
      budget.tick();
      const auto Index = Pending.back();
      Pending.pop_back();
      const auto &Op = code[Index];
      const auto Incoming = Frames.at(Index);
      auto Outgoing = Incoming;
      if (!Incoming.empty() && !Exits.contains(Index))
        Regions.at(Incoming.back()).body.insert(Index);
      if (Enters.contains(Index)) {
        if (std::find(Incoming.begin(), Incoming.end(), Index) !=
                Incoming.end() ||
            Incoming.size() >= Enters.size())
          return false;
        Nested |= !Incoming.empty();
        MonitorRegion Region{Index, 0, Op.registers[0], {}, {}, {}};
        Region.dispatch_handlers = true;
        Regions.emplace(Index, std::move(Region));
        Outgoing.push_back(Index);
      } else if (Exits.contains(Index)) {
        if (Incoming.empty() || Index + 1 >= code.size())
          return false;
        const auto Owner = Incoming.back();
        Regions.at(Owner).exits.insert(Index);
        Releases.emplace(Index, Owner);
        Outgoing.pop_back();
      } else if (starts(Op.opcode, "return") && !Incoming.empty())
        return false;
      // Acquisition can fail before adding its lock. A certified release is
      // total only after the existing must-alias pass proves its acquired
      // object; its normal edge alone pops the innermost lifetime here.
      if (!Exits.contains(Index) && !NonThrowing(Op)) {
        const auto &Cleanup = handlers(Op.pc);
        if (Cleanup.empty() && !Incoming.empty())
          return false;
        if (!Cleanup.empty()) {
          // Every potentially throwing held operation still needs its final
          // catch-all. Typed alternatives retain their original order and
          // enter with the identical acquired stack, before any release.
          if (!Incoming.empty() && Cleanup.back().type)
            return false;
          for (const auto &Catch : Cleanup) {
            budget.tick();
            if (!by_pc.contains(Catch.target))
              return false;
            const auto Handler = by_pc.at(Catch.target);
            if (code[Handler].opcode != "move-exception" ||
                code[Handler].registers.size() != 1 || code[Handler].target ||
                !Plain(code[Handler]) || !edge(Handler, Incoming, true))
              return false;
            if (!Incoming.empty()) {
              Typed |= Catch.type.has_value();
              Regions.at(Incoming.back()).handler_entries.insert(Handler);
            }
          }
        }
      }
      for (const auto PC : successors(Index))
        if (!by_pc.contains(PC) || !edge(by_pc.at(PC), Outgoing, false))
          return false;
    }
    if ((!Nested && !Typed && !SingleRelease) ||
        Regions.size() != Enters.size() || Releases.size() != Exits.size())
      return false;
    for (size_t I = 0; I < code.size(); ++I)
      if (!Frames.contains(I) && !emptyNop(code[I]))
        return false;
    for (auto &[Index, Region] : Regions) {
      if (Region.exits.empty())
        return false;
      Region.cleanup_exit = *Region.exits.rbegin();
    }
    // Each PC has exactly one held acquisition stack. Bodies belong to the
    // innermost lifetime; a child acquisition is an operation of its parent,
    // while its normal/exceptional releases resume the parent's dispatcher.
    // These stack receipts do not grant object identity: the shared register
    // alias proof below remains mandatory for every original release.
    monitors = std::move(Regions);
    monitor_release_owners = std::move(Releases);
    return true;
  };
  if (stackScopes())
    return;
  struct CleanupRoute {
    unsigned exception;
    size_t release;
    std::set<size_t> members;
  };
  auto totalCleanupRoute = [&](size_t Enter, size_t Target,
                               bool CopiedException =
                                   false) -> std::optional<CleanupRoute> {
    if (Target <= Enter || Target >= code.size())
      return {};
    const auto &Capture = code[Target];
    if (Capture.opcode != "move-exception" || Capture.registers.size() != 1 ||
        Capture.target || !Plain(Capture))
      return {};
    CleanupRoute Route{Capture.registers[0], 0, {Target}};
    std::set<unsigned> ExceptionAliases{Route.exception};
    size_t At = Target + 1;
    while (At < code.size() && !Exits.contains(At)) {
      budget.tick();
      if (!Route.members.insert(At).second)
        return {};
      const auto &Op = code[At];
      if (RegisterCopy(Op)) {
        const unsigned Width = baseOpcode(Op.opcode) == "move-wide" ? 2 : 1;
        const unsigned Destination = Op.registers[0];
        if (!CopiedException && Destination <= Route.exception &&
            Route.exception - Destination < Width)
          return {};
        const bool Copied = baseOpcode(Op.opcode) == "move-object" &&
                            ExceptionAliases.contains(Op.registers[1]);
        for (unsigned Part = 0; Part < Width; ++Part)
          ExceptionAliases.erase(Destination + Part);
        if (Copied)
          ExceptionAliases.insert(Destination);
        ++At;
      } else if (emptyNop(Op))
        ++At;
      else if (baseOpcode(Op.opcode) == "goto" && Op.registers.empty() &&
               Op.target && Plain(Op) && by_pc.contains(*Op.target))
        At = by_pc.at(*Op.target);
      else
        return {};
    }
    if (At + 1 >= code.size() || code[At].target || !Plain(code[At]) ||
        code[At + 1].opcode != "throw" || code[At + 1].registers.size() != 1 ||
        (CopiedException ? !ExceptionAliases.contains(code[At + 1].registers[0])
                         : code[At + 1].registers != Capture.registers) ||
        code[At + 1].target || !Plain(code[At + 1]))
      return {};
    Route.release = At;
    Route.members.insert(At);
    Route.members.insert(At + 1);
    return Route;
  };
  auto scopedCleanup = [&](size_t Enter) -> std::optional<MonitorRegion> {
    // Follow both normal and exceptional edges while this lock is held.
    // Every exact release hands its next original PC and register state back
    // to the unlocked CFG, including rethrows observed by an outer handler.
    // This proof belongs to one acquisition. Sequential scopes can consume
    // it independently; encountering another acquisition while held still
    // fails below, and the common ownership pass checks their disjointness.
    const auto Lock = code[Enter].registers[0];
    MonitorRegion Plan{Enter, 0, Lock, {}, {}, {}};
    struct Edge {
      size_t index;
      bool capture, exceptional_program;
    };
    std::vector<Edge> Pending{{Enter + 1, false, false}};
    std::map<size_t, unsigned> Seen;
    bool ExceptionalEffects = false, CapturedRegisterCopies = false;
    std::set<unsigned> CapturedRegisters;
    std::set<size_t> ThrowingHandlers, ReleaseHandlers;
    while (!Pending.empty()) {
      budget.tick();
      const auto Edge = Pending.back();
      Pending.pop_back();
      const auto I = Edge.index;
      if (I <= Enter || I >= code.size() || Enters.contains(I) ||
          starts(code[I].opcode, "return"))
        fail("monitor scoped cleanup bypasses its release");
      if (code[I].opcode == "move-exception") {
        if (!Edge.capture || code[I].registers.size() != 1 || code[I].target ||
            !Plain(code[I]))
          fail("monitor scoped cleanup has a normal handler entry");
        Plan.handler_entries.insert(I);
        CapturedRegisters.insert(code[I].registers[0]);
      }
      if (Exits.contains(I)) {
        if (!Plan.exits.insert(I).second)
          continue;
        // A physical release handler belongs here only when the existing
        // total-copy route proves its capture rejoins an original release.
        // A range can also cover the following rethrow: its outer handler
        // retains the unlocked continuation instead of acquiring this scope.
        const auto &Cleanup = handlers(code[I].pc);
        if (Cleanup.size() == 1 && !Cleanup[0].type &&
            by_pc.contains(Cleanup[0].target)) {
          const auto Handler = by_pc.at(Cleanup[0].target);
          if (totalCleanupRoute(Enter, Handler, true)) {
            ReleaseHandlers.insert(Handler);
            Pending.push_back({Handler, true, true});
          }
        }
        continue;
      }
      const unsigned Mode = Edge.exceptional_program ? 2 : 1;
      if (Seen[I] & Mode)
        continue;
      Seen[I] |= Mode;
      Plan.body.insert(I);
      CapturedRegisterCopies |=
          Edge.exceptional_program &&
          baseOpcode(code[I].opcode) == "move-object" &&
          RegisterCopy(code[I]) &&
          CapturedRegisters.contains(code[I].registers[1]);
      ExceptionalEffects |= Edge.exceptional_program &&
                            code[I].opcode != "move-exception" &&
                            code[I].opcode != "throw" && !NonThrowing(code[I]);
      const auto &Cleanup = handlers(code[I].pc);
      // A protected capture/copy/constant does not take its handler merely
      // because it lies inside the range. Match ordinary exceptional dataflow
      // and the unlocked CFG: only actual throwing operations add this edge.
      if (canThrow(code[I]) && !Cleanup.empty()) {
        if (Cleanup.size() != 1 || Cleanup[0].type ||
            !by_pc.contains(Cleanup[0].target) ||
            code[by_pc.at(Cleanup[0].target)].opcode != "move-exception")
          fail("monitor scoped cleanup requires its exact catch-all entries");
        ThrowingHandlers.insert(by_pc.at(Cleanup[0].target));
        Pending.push_back({by_pc.at(Cleanup[0].target), true, true});
      } else if (canThrow(code[I]))
        fail("monitor region has an uncovered exceptional path");
      for (const auto PC : successors(I))
        Pending.push_back({by_pc.at(PC), false, Edge.exceptional_program});
    }
    if (Plan.exits.empty())
      fail("monitor region requires its sole catch-all cleanup");
    bool OuterRelease = false;
    for (const auto Exit : Plan.exits)
      if (Exit + 1 < code.size() && !NonThrowing(code[Exit + 1]))
        for (const auto &H : handlers(code[Exit + 1].pc))
          OuterRelease |= by_pc.contains(H.target) &&
                          !Plan.body.contains(by_pc.at(H.target));
    const bool ReleaseOnly = std::any_of(
        ReleaseHandlers.begin(), ReleaseHandlers.end(),
        [&](size_t Handler) { return !ThrowingHandlers.contains(Handler); });
    // A handler may forward its capture through another physical register
    // before a shared release, including a backward cleanup join. Keep those
    // instructions in the held dispatcher: ordinary dataflow owns their exact
    // values and the shared must-alias pass still owns every released object.
    // Static-final stages retain their separate closed cleanup requirement.
    if (!ExceptionalEffects && !OuterRelease && !ReleaseOnly &&
        handlers(code[Enter].pc).empty() &&
        (!CapturedRegisterCopies || StaticFinal))
      return {};
    Plan.cleanup_exit = *Plan.exits.rbegin();
    Plan.dispatch_handlers = true;
    return Plan;
  };
  std::map<size_t, size_t> Owners, CleanupOwners;
  std::set<size_t> ProvenExits;
  for (size_t Enter : Enters) {
    budget.tick();
    if (code[Enter].registers.size() != 1 || method.tries.empty())
      fail("monitor acquisition must precede its cleanup handler");
    MonitorRegion Plan{Enter, 0, code[Enter].registers[0], {}, {}, {}};
    if (auto Nested = scopedCleanup(Enter)) {
      Plan = std::move(*Nested);
      for (const auto H : Plan.handler_entries)
        if (!CleanupOwners.emplace(H, Enter).second)
          fail("monitor nested cleanup overlaps another acquired region");
    } else {
      if (!handlers(code[Enter].pc).empty())
        fail("monitor acquisition must precede its cleanup handler");
      std::set<size_t> CleanupTargets;
      std::vector<size_t> Pending{Enter + 1};
      while (!Pending.empty()) {
        budget.tick();
        const size_t I = Pending.back();
        Pending.pop_back();
        if (I <= Enter || I >= code.size() || Enters.contains(I) ||
            starts(code[I].opcode, "return") ||
            code[I].opcode == "move-exception")
          fail("monitor region normal flow bypasses its release");
        if (Exits.contains(I)) {
          Plan.exits.insert(I);
          continue;
        }
        if (!Plan.body.insert(I).second)
          continue;
        const auto &Cleanup = handlers(code[I].pc);
        if (canThrow(code[I]) && !Cleanup.empty()) {
          if (Cleanup.size() != 1 || Cleanup[0].type ||
              !by_pc.contains(Cleanup[0].target))
            fail("monitor region requires its sole catch-all cleanup");
          CleanupTargets.insert(by_pc.at(Cleanup[0].target));
        } else if (canThrow(code[I]))
          fail("monitor region has an uncovered exceptional path");
        for (auto PC : successors(I))
          Pending.push_back(by_pc.at(PC));
      }
      if (Plan.exits.empty() || CleanupTargets.empty())
        fail("monitor region requires its sole catch-all cleanup");
      Plan.handler_entries = CleanupTargets;
      std::optional<unsigned> Exception;
      std::optional<size_t> Release;
      for (size_t Target : CleanupTargets) {
        const auto Route = totalCleanupRoute(Enter, Target);
        if (!Route || (Exception && *Exception != Route->exception) ||
            (Release && *Release != Route->release) ||
            !CleanupOwners.emplace(Target, Enter).second)
          fail("monitor cleanup requires only total local copies before "
               "release and an exact captured rethrow");
        Exception = Route->exception;
        Release = Route->release;
        Plan.cleanup.insert(Route->members.begin(), Route->members.end());
      }
      // These adapters write only dead locals and cannot throw. All original
      // handlers converge on the same release and rethrow the captured object;
      // synchronized owns that exceptional release in the emitted source.
      Plan.cleanup_exit = *Release;
    }
    auto own = [&](size_t I) {
      if (!Owners.emplace(I, Enter).second || Enters.contains(I))
        fail("monitor regions overlap or nest without a proven release");
    };
    // Handler blocks may precede later normal blocks in physical order.
    // Ownership is the reachable locked CFG, not a contiguous PC interval;
    // cleanup and unlocked-entry checks below still prove it is disjoint.
    for (size_t I : Plan.body)
      own(I);
    for (size_t I : Plan.exits) {
      if (I + 1 >= code.size())
        fail("monitor region has a changed or misplaced normal release");
      own(I);
      ProvenExits.insert(I);
      const auto &Cleanup = handlers(code[I].pc);
      if (!Plan.dispatch_handlers && !Cleanup.empty() &&
          (Cleanup.size() != 1 || Cleanup[0].type ||
           !by_pc.contains(Cleanup[0].target) ||
           !Plan.handler_entries.contains(by_pc.at(Cleanup[0].target))))
        fail("monitor region requires its sole catch-all cleanup");
    }
    for (size_t I : Plan.cleanup)
      own(I);
    ProvenExits.insert(Plan.cleanup_exit);
    monitors.emplace(Enter, std::move(Plan));
  }
  if (ProvenExits != Exits)
    fail("monitor region has an unreachable normal release");
  for (const auto &[Enter, Plan] : monitors) {
    for (const auto Exit : Plan.exits)
      monitor_release_owners.emplace(Exit, Enter);
    monitor_release_owners.emplace(Plan.cleanup_exit, Enter);
  }
  // Treat each proven synchronized region as one transition in the unlocked
  // CFG. Every entry still passes its acquisition; callbacks between regions
  // execute after one release and before the next acquisition.
  std::set<size_t> Unlocked;
  std::vector<size_t> Pending{0};
  auto unlockedEdge = [&](size_t I, bool Exceptional) {
    if (I >= code.size() ||
        (!Exceptional && code[I].opcode == "move-exception"))
      fail("monitor continuation has a normal handler entry");
    Pending.push_back(I);
  };
  auto unlockedHandlers = [&](size_t I) {
    const auto &Cleanup = handlers(code[I].pc);
    // Outside the acquired region these are ordinary ordered handlers. Their
    // original types and selection stay with the shared exception dispatcher;
    // resource ownership still requires every destination to remain unlocked.
    for (const auto &Handler : Cleanup) {
      budget.tick();
      if (!by_pc.contains(Handler.target))
        fail("monitor continuation has an unknown exception handler entry");
      unlockedEdge(by_pc.at(Handler.target), true);
    }
  };
  while (!Pending.empty()) {
    budget.tick();
    const size_t I = Pending.back();
    Pending.pop_back();
    if (I >= code.size() || Owners.contains(I))
      fail("monitor continuation re-enters the locked body or cleanup");
    if (!Unlocked.insert(I).second)
      continue;
    if (const auto Region = monitors.find(I); Region != monitors.end()) {
      for (size_t Exit : Region->second.exits)
        unlockedEdge(Exit + 1, false);
      unlockedHandlers(I);
      continue;
    }
    if (code[I].opcode != "move-exception" && !NonThrowing(code[I]))
      unlockedHandlers(I);
    for (auto PC : successors(I)) {
      const size_t Target = by_pc.at(PC);
      unlockedEdge(Target, false);
    }
  }
  for (size_t Enter : Enters)
    if (!Unlocked.contains(Enter))
      fail("monitor region has an external entry without acquisition");
  for (const auto &Region : method.tries) {
    for (const auto &Catch : Region.handlers) {
      budget.tick();
      if (!by_pc.contains(Catch.target))
        fail("monitor region has an unknown exception handler entry");
      const size_t Handler = by_pc.at(Catch.target);
      const auto Owner = CleanupOwners.find(Handler);
      if (Owner == CleanupOwners.end()) {
        if (!Unlocked.contains(Handler))
          fail("monitor region has an unproven outer exception handler");
        continue;
      }
      const auto &Plan = monitors.at(Owner->second);
      if (Region.handlers.size() != 1 || Catch.type ||
          (!Plan.dispatch_handlers &&
           (Region.start < code[Plan.enter + 1].pc ||
            Region.end > code[Plan.cleanup_exit + 1].pc)))
        fail("monitor region requires its sole catch-all cleanup");
    }
  }
  // Unlocked cycles retain their original PC transitions in the outer
  // dispatcher. The traversal above verifies every edge, including loop
  // backedges: entry into an owned body or release still requires acquisition.
  size_t Tail = 0;
  for (const auto &[I, Owner] : Owners)
    Tail = std::max(Tail, I + 1);
  for (const auto I : Unlocked)
    Tail = std::max(Tail, I + 1);
  const bool PaddingTail =
      std::all_of(code.begin() + Tail, code.end(), emptyNop);
  for (size_t I = 0; I < code.size(); ++I) {
    budget.tick();
    if (Owners.contains(I) || Unlocked.contains(I))
      continue;
    if (PaddingTail && I >= Tail && handlers(code[I].pc).empty())
      continue;
    fail("monitor region has an unproven outside block");
  }
}

// A synchronized statement may execute between two static final assignments,
// but no assignment can be moved across its acquisition or either release.
// Reuse both authoritative receipts: finalWriteRegions owns assignment stages,
// and planMonitor owns exact entries, cleanup, releases and object identity.
void Body::validateStaticFinalMonitors() {
  const auto Flow = finalWriteRegions(method, final_writes, budget);
  if (method.reference.name != "<clinit>" || !has(method.access, "static") ||
      monitors.size() != 1 || Flow.boundaries.empty() ||
      !Flow.ordered_stages.empty() || !Flow.diamonds.empty() ||
      Flow.structured_end || Flow.structured_prefix_end ||
      Flow.structured_suffix_begin || Flow.structured_middle ||
      !Flow.terminal_tail || Flow.terminal_tail_entries.size() != 1)
    fail("static final monitor requires one complete assignment stage");
  const auto &[Enter, Plan] = *monitors.begin();
  const auto End =
      std::lower_bound(Flow.boundaries.begin(), Flow.boundaries.end(), Enter);
  if (End == Flow.boundaries.end())
    fail("static final monitor has no following assignment boundary");
  const auto Begin = End == Flow.boundaries.begin() ? 0 : *std::prev(End) + 1;
  auto Within = [&](size_t I) {
    budget.tick();
    return Begin <= I && I < *End;
  };
  if (Plan.dispatch_handlers || !Within(Enter) ||
      !std::all_of(Plan.body.begin(), Plan.body.end(), Within) ||
      !std::all_of(Plan.exits.begin(), Plan.exits.end(), Within) ||
      Plan.cleanup.empty() || *Plan.cleanup.begin() != *Flow.terminal_tail ||
      !Plan.cleanup.contains(Plan.cleanup_exit) ||
      !std::all_of(Plan.cleanup.begin(), Plan.cleanup.end(), [&](size_t I) {
        budget.tick();
        return I >= *Flow.terminal_tail;
      }))
    fail("static final monitor crosses an assignment stage");
}

// Keep allocation/class initialization before the complete conditional
// argument program. A forward region, no external entry and one exact
// initializer form a closed region; Body still verifies every original value.
// A switch-expression block evaluates that region inside the first argument,
// without a lambda/helper allocation or caching an original repeated getter.
std::optional<size_t> Body::allocationReceiverTail(const Instruction &New,
                                                   size_t End) {
  if (End >= code.size() ||
      !std::holds_alternative<MethodRef>(code[End].reference) ||
      std::get<MethodRef>(code[End].reference).name != "<init>" ||
      New.registers.size() != 1 || code[End].registers.empty() ||
      code[End].registers[0] == New.registers[0])
    return {};
  auto Alias = New;
  Alias.registers = {code[End].registers[0]};
  if (!allocationInitializerShape(Alias, code[End]))
    return {};
  const auto Begin = by_pc.at(New.pc);
  for (size_t At = End; At > Begin + 1 && End - At < 16;) {
    budget.tick();
    const auto &Copy = code[--At];
    if ((Copy.opcode == "move-object" || Copy.opcode == "move-object/from16" ||
         Copy.opcode == "move-object/16") &&
        Copy.registers ==
            std::vector<unsigned>{Alias.registers[0], New.registers[0]} &&
        std::holds_alternative<std::monostate>(Copy.literal) &&
        std::holds_alternative<std::monostate>(Copy.reference))
      return At;
    if (!pureAllocationPreparation(Copy))
      return {};
  }
  return {};
}

bool Body::allocationInitializer(const Instruction &New,
                                 const Instruction &Init) {
  if (!allocationInitializerShape(New, Init))
    return false;
  const auto *Type = std::get_if<std::string>(&New.reference);
  const auto *Ref = std::get_if<MethodRef>(&Init.reference);
  if (!concreteAllocationClass(*Type))
    return false;
  const auto *Declaration = validateInvocation(Init);
  if (!Declaration || !expressionMemberAccessible(*Ref, *Declaration))
    return false;
  if (!anonymous_sources.contains(*Type))
    sourceType(*Type);
  return true;
}

bool Body::planConstructorChoice(size_t Index) {
  if (verifying_constructor_arguments || code.size() - Index < 6)
    return false;
  const auto &Root = code[Index], &Branch = code[Index + 1];
  const auto *Type = std::get_if<std::string>(&Root.reference);
  const bool paired = Branch.opcode == "if-eq" || Branch.opcode == "if-ne";
  const bool Equality = paired || Branch.opcode == "if-eqz" || Branch.opcode == "if-nez";
  if (Root.opcode != "new-instance" || !Type || Root.registers.size() != 1 ||
      !concreteAllocationClass(*Type) ||
      !independentSourceScope(classes.at(*Type)) ||
      !(Equality || Branch.opcode == "if-ltz" || Branch.opcode == "if-gez" ||
        Branch.opcode == "if-gtz" || Branch.opcode == "if-lez") ||
      Branch.registers.size() != (paired ? 2u : 1u) || !Branch.target ||
      std::find(Branch.registers.begin(), Branch.registers.end(),
                Root.registers.front()) != Branch.registers.end())
    return false;
  // A branch may prepare a reference after allocation. Keep those operations
  // inside the chosen new expression so class initialization precedes reads
  // and checked casts. No preparation can observe the uninitialized object.
  auto initializer = [&](size_t pos) {
    while (pos < code.size()) {
      budget.tick();
      const auto &op = code[pos];
      const auto kind = baseOpcode(op.opcode);
      if (kind == "new-instance") {
        if (pos + 1 >= code.size() || !allocationInitializer(op, code[pos + 1]))
          break;
      } else if (kind == "invoke-direct") {
        if (!pos || code[pos - 1].opcode != "new-instance" ||
            !allocationInitializerShape(code[pos - 1], op))
          break;
      } else if (kind != "move-object" && kind != "check-cast" &&
                 kind != "iget-object" && kind != "sget-object" &&
                 kind != "const" && kind != "invoke-virtual" &&
                 kind != "invoke-static" && kind != "invoke-interface" &&
                 kind != "move-result" && kind != "move-result-object" &&
                 kind != "move-result-wide") {
        break;
      }
      if (kind == "move-result-wide" && !op.registers.empty() &&
          op.registers.front() + 1 == Root.registers.front())
        return code.size();
      if (std::find(op.registers.begin(), op.registers.end(), Root.registers[0]) !=
          op.registers.end())
        return code.size();
      ++pos;
    }
    return pos;
  };
  const size_t first = initializer(Index + 2);
  if (first >= code.size() - 2 || Branch.target != code[first + 2].pc)
    return false;
  const size_t second = initializer(first + 2);
  if (second >= code.size() - 1)
    return false;
  const auto &First = code[first], &Jump = code[first + 1];
  const auto &Second = code[second], &Join = code[second + 1];
  const bool returns = Jump.opcode == "return-object" && !Jump.target &&
                       Jump.registers == Root.registers &&
                       Join.opcode == Jump.opcode && Join.registers == Jump.registers;
  if ((!returns && (!starts(Jump.opcode, "goto") || !Jump.registers.empty() ||
                    Jump.target != Join.pc)) ||
      !allocationInitializer(Root, First) || !allocationInitializer(Root, Second))
    return false;
  const auto &A = std::get<MethodRef>(First.reference);
  const auto &B = std::get<MethodRef>(Second.reference);
  const bool prepared = first != Index + 2 || second != first + 2;
  if (prepared && (A.parameters.empty() || B.parameters.empty()))
    return false;
  for (const auto *call : {&First, &Second})
    if (std::find(call->registers.begin() + 1, call->registers.end(),
                  Root.registers.front()) != call->registers.end())
      return false;
  if (A != B || !prepared) {
    const auto &longer = A.parameters.size() > B.parameters.size() ? First : Second;
    const auto &shorter = &longer == &First ? Second : First;
    const auto &with = std::get<MethodRef>(longer.reference).parameters;
    const auto &without = std::get<MethodRef>(shorter.reference).parameters;
    if (with.size() != without.size() + 1 || longer.registers.size() < 2 ||
        shorter.registers.empty())
      return false;
    const bool leading = !paired &&
        std::equal(with.begin() + 1, with.end(), without.begin()) &&
        longer.registers.size() == shorter.registers.size() + 1 &&
        longer.registers[1] == Branch.registers[0] &&
        (with.front() == "I" || (Equality && isReference(with.front()))) &&
        std::equal(longer.registers.begin() + 2, longer.registers.end(),
                   shorter.registers.begin() + 1);
    // An overload can append a value while retaining every original shared
    // word in place. Both distinct constructor descriptors remain explicit.
    const bool trailing = !without.empty() &&
        std::equal(without.begin(), without.end(), with.begin()) &&
        longer.registers.size() == shorter.registers.size() + wordWidth(with.back()) &&
        std::equal(shorter.registers.begin(), shorter.registers.end(),
                   longer.registers.begin());
    if (!leading && !trailing)
      return false;
    size_t word = 1;
    for (const auto &type : without) {
      budget.tick();
      if (word >= shorter.registers.size() ||
          (wordWidth(type) == 2 &&
           (word + 1 >= shorter.registers.size() ||
            shorter.registers[word + 1] != shorter.registers[word] + 1)))
        return false;
      word += wordWidth(type);
    }
    if (word != shorter.registers.size())
      return false;
  }
  for (size_t At = Index; At <= second; ++At) {
    budget.tick();
    const auto &Op = code[At];
    if ((At != Index && Op.pc <= code[At - 1].pc) || Op.pc >= Join.pc ||
        !Op.targets.empty() || !Op.keys.empty() || !Op.data.empty() ||
        Op.element_width ||
        (baseOpcode(Op.opcode) == "const"
             ? !std::holds_alternative<int64_t>(Op.literal)
             : !std::holds_alternative<std::monostate>(Op.literal)) ||
        (prepared && !handlers(Op.pc).empty()) ||
        ((At == Index + 1 || At == first + 1)
             ? !std::holds_alternative<std::monostate>(Op.reference)
             : Op.target.has_value()))
      return false;
  }
  if (!sameHandlers(handlers(Root.pc), handlers(First.pc)) ||
      !sameHandlers(handlers(Root.pc), handlers(Second.pc)))
    return false;
  for (size_t At : {first, second}) {
    const auto *Declaration = validateInvocation(code[At]);
    if (!Declaration || !has(Declaration->access, "public") ||
        (Declaration->generic_signature &&
         !generics.reference_constructor_inputs.contains(Declaration->reference) &&
         !(prepared && *Type == ownerClass().name &&
           generics.classes.contains(*Type) &&
           !generics.classes.at(*Type).type_parameters.empty() &&
           generics.methods.contains(Declaration->reference) &&
           generics.methods.at(Declaration->reference).type_parameters.empty() &&
           generics.methods.at(Declaration->reference).source_projections.empty() &&
           std::count_if(ownerClass().methods.begin(), ownerClass().methods.end(),
                         [&](const Method &candidate) {
                           budget.tick();
                           return candidate.reference.name == "<init>" &&
                                  (candidate.reference.parameters.size() == A.parameters.size() ||
                                   has(candidate.access, "varargs"));
                         }) == 1)) ||
        has(Declaration->access, "varargs") ||
        (Declaration->declared_throws &&
         !Declaration->declared_throws->empty()))
      return false;
  }
  for (size_t At = 0; At < code.size(); ++At) {
    if (At >= Index && At <= second)
      continue;
    budget.tick();
    auto enters = [&](uint32_t PC) { return PC > Root.pc && PC < Join.pc; };
    if ((branchTarget(code[At]) && enters(*branchTarget(code[At]))) ||
        std::any_of(code[At].targets.begin(), code[At].targets.end(), enters))
      return false;
  }
  for (const auto &Region : method.tries)
    for (const auto &Handler : Region.handlers)
      if (Handler.target > Root.pc && Handler.target < Join.pc)
        return false;
  ConstructorChoice Plan{Index + 1, first, second, second + 1, {}};
  Plan.cleanup.start = Root.pc;
  Plan.cleanup.end = Second.pc;
  Plan.cleanup.values.push_back(
      {"new", *Type, Index, Root.registers[0], {}, {}});
  for (auto PC : {First.pc, Second.pc}) {
    constructors[PC] = Root.pc;
    Plan.cleanup.handler_inputs[PC].emplace(Root.registers[0], 0);
  }
  for (size_t At = Index; At <= second; ++At)
    constructor_choice_owners.emplace(code[At].pc, Root.pc);
  constructor_choices.emplace(Root.pc, std::move(Plan));
  return true;
}

bool Body::planForwardAllocation(size_t Index) {
  if (Index >= code.size() || (method.reference.name == "<init>" && first == 0))
    return false;
  const auto &Root = code[Index];
  const auto *Type = std::get_if<std::string>(&Root.reference);
  if (Root.opcode != "new-instance" || !Type || Root.registers.size() != 1 ||
      Root.target || !Root.targets.empty() || !Root.keys.empty() ||
      !Root.data.empty() || Root.element_width ||
      !std::holds_alternative<std::monostate>(Root.literal))
    return false;
  std::set<size_t> Members{Index};
  bool Jumped = false;
  size_t At = Index + 1;
  for (;;) {
    budget.tick();
    if (At >= code.size() || At <= Index || At - Index > 256 ||
        !Members.insert(At).second ||
        allocation_region_owners.contains(code[At].pc) ||
        conditional_allocation_owners.contains(code[At].pc))
      return false;
    const auto &Op = code[At];
    const auto *Ref = std::get_if<MethodRef>(&Op.reference);
    if (Ref && Ref->name == "<init>") {
      if (!Jumped || !Ref->parameters.empty() ||
          !sameHandlers(handlers(Root.pc), handlers(Op.pc)) ||
          !allocationInitializer(Root, Op))
        return false;
      break;
    }
    // A nullary constructor has no argument evaluation site. Only control
    // transfers with no reads, writes or effects may separate it from new.
    if (!Op.registers.empty() || !Op.targets.empty() || !Op.keys.empty() ||
        !Op.data.empty() || Op.element_width ||
        !std::holds_alternative<std::monostate>(Op.literal) ||
        !std::holds_alternative<std::monostate>(Op.reference))
      return false;
    if (Op.opcode == "goto" || Op.opcode == "goto/16" ||
        Op.opcode == "goto/32") {
      const auto Position = by_pc.find(target(Op));
      if (Position == by_pc.end() || Position->second <= At)
        return false;
      At = Position->second;
      Jumped = true;
    } else if (Op.opcode == "nop" && !Op.target)
      ++At;
    else
      return false;
  }
  auto enters = [&](uint32_t PC) {
    const auto Position = by_pc.find(PC);
    return Position != by_pc.end() && Position->second != Index &&
           Members.contains(Position->second);
  };
  for (size_t I = 0; I < code.size(); ++I) {
    budget.tick();
    if (Members.contains(I))
      continue;
    const auto Edges =
        I + 1 < code.size() ? successors(I) : std::vector<uint32_t>{};
    if (std::any_of(Edges.begin(), Edges.end(), enters))
      return false;
  }
  for (const auto &Region : method.tries)
    for (const auto &Handler : Region.handlers) {
      budget.tick();
      if (enters(Handler.target))
        return false;
    }
  // Reuse the value graph's allocation emission and converged cleanup-origin
  // proof. Unrelated physical sibling blocks acquire no ownership. Allocation
  // and initialization dispatch through the same proven handler at new's PC.
  AllocationRegion Plan;
  Plan.start = Root.pc;
  Plan.end = code[At].pc;
  Plan.values.push_back({"new", *Type, Index, Root.registers[0], {}, At});
  Plan.live.emplace(Root.registers[0], 0);
  Plan.written.insert(Root.registers[0]);
  Plan.effects = {Root.pc, code[At].pc};
  Plan.handler_inputs[code[At].pc] = Plan.live;
  for (size_t Member : Members)
    allocation_region_owners.emplace(code[Member].pc, Root.pc);
  constructors[code[At].pc] = Root.pc;
  allocation_regions.emplace(Root.pc, std::move(Plan));
  forward_allocations.insert(Root.pc);
  return true;
}

bool Body::planConditionalAllocation(size_t Index, unsigned Depth) {
  constexpr size_t MaximumSpan = 512;
  if (Depth > 8 || Index >= code.size())
    return false;
  if (conditional_allocations.contains(code[Index].pc))
    return true;
  if (method.reference.name == "<init>" && first == 0 &&
      !verifying_constructor_arguments)
    return false;
  const auto &Root = code[Index];
  if (Root.opcode != "new-instance" || Root.registers.size() != 1 ||
      Root.target || !Root.targets.empty() || !Root.keys.empty() ||
      !Root.data.empty() || Root.element_width ||
      !std::holds_alternative<std::monostate>(Root.literal))
    return false;
  const unsigned Object = Root.registers.at(0);
  // Physical block order need not match ownership: a sibling switch branch
  // can sit between this new and its argument tail. Discover only reachable
  // forward nodes, stopping at one exact initializer or a terminal throw.
  struct ReceiverOrigins {
    std::set<unsigned> possible, definite;
  };
  std::set<size_t> Members{Index}, Initializers, ReceiverTransfers,
      DiscardsOriginal;
  std::map<size_t, ReceiverOrigins> Origins{
      {Index + 1, ReceiverOrigins{{Object}, {Object}}}};
  std::set<size_t> Pending{Index + 1};
  while (!Pending.empty()) {
    budget.tick();
    // Every edge is forward. Visiting the smallest pending position ensures
    // that all predecessors have contributed before its origin is consumed.
    const size_t At = *Pending.begin();
    Pending.erase(Pending.begin());
    if (At <= Index || At >= code.size() || At - Index > MaximumSpan)
      return false;
    if (!Members.insert(At).second)
      continue;
    const auto &Op = code[At];
    const auto &Incoming = Origins.at(At);
    auto Receiver = Root;
    if (!Op.registers.empty())
      Receiver.registers = {Op.registers.front()};
    if (!Op.registers.empty() && allocationInitializerShape(Receiver, Op) &&
        Incoming.possible.contains(Op.registers.front())) {
      if (!Incoming.definite.contains(Op.registers.front()))
        return false;
      Initializers.insert(At);
      continue;
    }
    auto Outgoing = Incoming;
    const auto Kind = baseOpcode(Op.opcode);
    const bool Plain = !Op.target && Op.targets.empty() && Op.keys.empty() &&
                       Op.data.empty() && !Op.element_width;
    const bool NoReference =
        std::holds_alternative<std::monostate>(Op.reference);
    const bool NoLiteral = std::holds_alternative<std::monostate>(Op.literal);
    const auto *Literal = std::get_if<int64_t>(&Op.literal);
    const bool Copy = Kind == "move-object" && Op.registers.size() == 2 &&
                      Plain && NoReference && NoLiteral;
    const bool Null = Kind == "const" && Op.registers.size() == 1 && Plain &&
                      NoReference && Literal && *Literal == 0;
    const bool Result = Kind == "move-result-object" &&
                        Op.registers.size() == 1 && Plain && NoReference &&
                        NoLiteral;
    const bool Discard = Op.registers.size() == 1 && Plain &&
                         (Kind == "new-instance" || Kind == "const-string");
    // An instance-of definition may reuse a receiver word, but its actual
    // reference operand must never observe any possible uninitialized alias.
    // Ordinary operation verification owns its exact type and boolean result.
    const auto *TestType = std::get_if<std::string>(&Op.reference);
    const bool BooleanWrite = Op.opcode == "instance-of" &&
                              Op.registers.size() == 2 && Plain && NoLiteral &&
                              TestType && isReference(*TestType) &&
                              !Incoming.possible.contains(Op.registers[1]);
    // Exact field loads define a new value without reading their destination.
    // An instance receiver cannot consume any possible uninitialized alias.
    // Reuse ordinary member lookup and field width; operation verification
    // still owns access, opcode/type agreement, volatile effects and failures.
    const auto *Field = std::get_if<FieldRef>(&Op.reference);
    const bool StaticRead = starts(Op.opcode, "sget");
    const bool FieldWrite =
        allocationFieldRead(Op.opcode) && Field && Plain && NoLiteral &&
        Op.registers.size() == (StaticRead ? 1u : 2u) &&
        (StaticRead || !Incoming.possible.contains(Op.registers[1])) &&
        member(*Field, classes, budget) != nullptr;
    if (Copy || Null || Result || Discard || BooleanWrite || FieldWrite) {
      const unsigned Destination = Op.registers.front();
      const bool Possible = Copy && Incoming.possible.contains(Op.registers[1]);
      const bool Definite = Copy && Incoming.definite.contains(Op.registers[1]);
      const unsigned Words = FieldWrite ? wordWidth(Field->type) : 1;
      bool Replaced = false;
      for (unsigned Part = 0; Part < Words; ++Part) {
        budget.tick();
        Replaced |= Incoming.possible.contains(Destination + Part);
        Outgoing.possible.erase(Destination + Part);
        Outgoing.definite.erase(Destination + Part);
      }
      if (Possible)
        Outgoing.possible.insert(Destination);
      if (Definite)
        Outgoing.definite.insert(Destination);
      if (Possible || Replaced)
        ReceiverTransfers.insert(At);
      if (Discard && Replaced)
        DiscardsOriginal.insert(At);
    }
    const auto Edges = successors(At);
    if (Edges.empty()) {
      if (Op.opcode != "throw")
        return false;
      continue;
    }
    for (auto PC : Edges) {
      budget.tick();
      const auto Position = by_pc.find(PC);
      if (Position == by_pc.end() || Position->second <= At ||
          code[Position->second].pc <= Op.pc)
        return false;
      const auto Next = Position->second;
      auto [Entry, Inserted] = Origins.emplace(Next, Outgoing);
      if (!Inserted) {
        for (unsigned R : Outgoing.possible) {
          budget.tick();
          Entry->second.possible.insert(R);
        }
        for (auto I = Entry->second.definite.begin();
             I != Entry->second.definite.end();) {
          budget.tick();
          if (!Outgoing.definite.contains(*I))
            I = Entry->second.definite.erase(I);
          else
            ++I;
        }
      }
      Pending.insert(Next);
    }
  }
  if (Initializers.size() != 1)
    return false;
  const size_t End = *Initializers.begin();
  const unsigned Branches =
      std::count_if(Members.begin(), Members.end(), [&](size_t At) {
        return At != Index && At != End &&
               (starts(code[At].opcode, "if-") ||
                code[At].opcode == "packed-switch" ||
                code[At].opcode == "sparse-switch");
      });
  const bool ForwardJump = std::any_of(
      Members.begin(), Members.end(), [&](size_t At) {
        return baseOpcode(code[At].opcode) == "goto" &&
               At + 1 < code.size() &&
               target(code[At]) != code[At + 1].pc;
      });
  // A noncontiguous forward jump can place a sibling between new and its
  // argument tail without introducing a condition. Reuse this same complete
  // CFG and receiver-origin proof. Contiguous linear roots still belong to
  // their value graph; admitted parents may compose linear child arguments.
  // A mutable field store also needs a statement inside new's argument
  // evaluation, even in a linear block. The member/receiver checks below
  // exclude final stores and any use of the uninitialized receiver.
  if (Branches == 0 && !ForwardJump && Depth == 0 &&
      std::none_of(Members.begin(), Members.end(), [&](size_t at) {
        return starts(code[at].opcode, "iput");
      }))
    return false;
  // All edges are forward. Work backwards to distinguish initializer paths
  // from paths that can only throw. An aborting path may discard the original
  // uninitialized word with another new-instance or a string constant, but
  // must never read or copy that original identity. Physical throw tails may
  // follow the successful initializer; no path through them reaches it. The
  // ordinary verifier still owns replacement effects and the thrown value.
  std::set<size_t> ReachesInitializer{End};
  for (auto I = Members.rbegin(); I != Members.rend(); ++I) {
    budget.tick();
    if (*I == End)
      continue;
    for (auto PC : successors(*I)) {
      budget.tick();
      if (ReachesInitializer.contains(by_pc.at(PC))) {
        ReachesInitializer.insert(*I);
        break;
      }
    }
  }
  // New/string replacements are the existing terminal-abort case. Ordinary
  // reference/null, boolean and field definitions may reuse original words
  // while another alias retains the allocation identity; a successful
  // constructor still needs a definite origin on every path. No initializer,
  // argument or observer may consume a merely possible uninitialized value.
  for (size_t At : DiscardsOriginal)
    if (ReachesInitializer.contains(At))
      return false;
  std::set<size_t> NestedInitializers;
  std::set<size_t> ConditionalMembers;
  for (size_t At : Members) {
    if (At == Index || At == End)
      continue;
    budget.tick();
    const auto &Op = code[At];
    const auto Kind = baseOpcode(Op.opcode);
    const bool Switch = Kind == "packed-switch" || Kind == "sparse-switch";
    const bool ArrayRead = arrayLoad(Op.opcode);
    const auto *field = std::get_if<FieldRef>(&Op.reference);
    const auto *store = field && starts(Kind, "iput")
                            ? member(*field, classes, budget)
                            : nullptr;
    const bool mutableStore = store && !has(store->access, "static") &&
                              !has(store->access, "final");
    const auto Conversion = scalarConversion(Op.opcode);
    const auto Unary = scalarUnary(Op.opcode);
    const auto Floating = floatingArithmetic(Op.opcode);
    const auto Binary = longBinary(Op.opcode);
    const auto Comparison = scalarComparison(Op.opcode);
    if (Binary && Binary->can_throw)
      return false;
    bool ConditionalChild = false;
    if (Kind == "new-instance" && planConditionalAllocation(At, Depth + 1)) {
      const auto &Child = conditional_allocations.at(Op.pc);
      if (Child.begin != At || Child.end <= At || Child.end == End ||
          !Members.contains(Child.end) || Child.end - Index > MaximumSpan)
        return false;
      // Reuse the complete child proof while checking all original child
      // instructions against this outer receiver and closed forward CFG too.
      // The shared emitter runs the child only at its own initializer, before
      // any later outer argument effects, retaining both allocation sites.
      for (size_t I : Child.members) {
        budget.tick();
        if (!Members.contains(I))
          return false;
        ConditionalMembers.insert(I);
      }
      NestedInitializers.insert(Child.end);
      ConditionalChild = true;
    }
    if (((Conversion || Unary) && Op.registers.size() != 2) ||
        (Comparison && Op.registers.size() != 3) ||
        (Floating &&
         Op.registers.size() != (Floating->two_address ? 2u : 3u)) ||
        (Binary && Op.registers.size() != (Binary->two_address ? 2u : 3u)) ||
        (!Switch && (!Op.targets.empty() || !Op.keys.empty())) ||
        !Op.data.empty() || Op.element_width ||
        (Op.target && !starts(Kind, "if-") && Kind != "goto" && !Switch) ||
        allocation_region_owners.contains(Op.pc) ||
        (conditional_allocation_owners.contains(Op.pc) &&
         !ConditionalMembers.contains(At)))
      return false;
    const auto *Call = std::get_if<MethodRef>(&Op.reference);
    bool AllocationStep = NestedInitializers.contains(At);
    if (Call && Call->name == "<init>" && !AllocationStep)
      return false;
    if (Kind == "new-instance") {
      // Pure register preparation may place the original arguments into a
      // contiguous range-call frame. No effect may cross this new; ordinary
      // allocation verification proves every copy and uninitialized word.
      if (!ConditionalChild) {
        size_t Init = At + 1;
        while (Init < code.size() && Members.contains(Init) && Init != End &&
               pureAllocationPreparation(code[Init])) {
          budget.tick();
          if (nonlinear_entries.contains(code[Init].pc))
            return false;
          ++Init;
        }
        if (Init >= code.size() || Init == End || !Members.contains(Init) ||
            nonlinear_entries.contains(code[Init].pc) ||
            !allocationInitializer(Op, code[Init]))
          return false;
        NestedInitializers.insert(Init);
      }
      AllocationStep = true;
    }
    // The ordinary condition verifier owns relation, arity and int/reference
    // compatibility, including two-register equality of nominal references.
    // Array creation, loads, lengths, reference stores and private calls retain
    // ordinary operation semantics, including failures and partial effects
    // inside the selected argument path.
    // Constructor calls still require the separate allocation ownership above.
    if (!starts(Kind, "if-") && !Switch && Kind != "throw" && !AllocationStep &&
        !pureAllocationPreparation(Op) && Kind != "goto" &&
        Kind != "move-result" && Kind != "move-result-object" &&
        Kind != "move-result-wide" && Kind != "filled-new-array" &&
        Kind != "const-string" && Kind != "const-class" && Kind != "check-cast" &&
        Kind != "instance-of" && Op.opcode != "array-length" && !ArrayRead &&
        Op.opcode != "new-array" && Op.opcode != "aput-object" &&
        !starts(Kind, "iget") && !starts(Kind, "sget") && !mutableStore &&
        Kind != "invoke-static" && Kind != "invoke-virtual" &&
        Kind != "invoke-interface" && Kind != "invoke-direct" && !Conversion &&
        !Unary && !Floating && !Binary && !Comparison)
      return false;
    for (unsigned Receiver : Origins.at(At).possible) {
      if (ReceiverTransfers.contains(At))
        continue;
      if (std::find(Op.registers.begin(), Op.registers.end(), Receiver) !=
          Op.registers.end())
        return false;
      // Every operand of a double operation occupies two words, including
      // both read/write operands of /2addr. Neither word may alias the new.
      if (Floating && Floating->type == "D" &&
          std::any_of(Op.registers.begin(), Op.registers.end(),
                      [&](unsigned R) { return R + 1 == Receiver; }))
        return false;
      if (Comparison && wordWidth(Comparison->type) == 2 &&
          (Op.registers[1] + 1 == Receiver || Op.registers[2] + 1 == Receiver))
        return false;
      if (Binary &&
          (Op.registers[0] + 1 == Receiver ||
           Op.registers[Binary->two_address ? 0 : 1] + 1 == Receiver ||
           (Binary->right_type == "J" &&
            Op.registers[Binary->two_address ? 1 : 2] + 1 == Receiver)))
        return false;
      const bool Wide = Kind == "move-wide" || Kind == "const-wide" ||
                        Kind == "move-result-wide" || Kind == "iget-wide" ||
                        Kind == "sget-wide" || (mutableStore && Kind == "iput-wide") ||
                        (ArrayRead && Kind == "aget-wide") ||
                        (Conversion && wordWidth(Conversion->second) == 2) ||
                        (Unary && wordWidth(Unary->first) == 2);
      if (Wide && !Op.registers.empty() && Op.registers[0] + 1 == Receiver)
        return false;
      if (((Kind == "move-wide" && Op.registers.size() == 2) ||
           (Conversion && wordWidth(Conversion->first) == 2) ||
           (Unary && wordWidth(Unary->first) == 2)) &&
          Op.registers[1] + 1 == Receiver)
        return false;
    }
  }
  if (End >= code.size() || End - Index > MaximumSpan)
    return false;
  const auto &Init = code[End];
  const auto *Ref = std::get_if<MethodRef>(&Init.reference);
  if (!Ref || Ref->parameters.empty() || Init.registers.empty() ||
      !Origins.at(End).definite.contains(Init.registers.front()))
    return false;
  auto Receiver = Root;
  Receiver.registers = {Init.registers.front()};
  if (!allocationInitializer(Receiver, Init))
    return false;
  auto enters = [&](uint32_t PC) {
    const auto Position = by_pc.find(PC);
    return Position != by_pc.end() && Position->second != Index &&
           Members.contains(Position->second);
  };
  for (size_t I = 0; I < code.size(); ++I) {
    budget.tick();
    if (Members.contains(I))
      continue;
    // A trailing unreachable alignment nop has no physical successor that
    // could enter this region. Ordinary reachability still rejects any real
    // fallthrough beyond the method boundary; explicit entries are checked too.
    const auto Edges =
        I + 1 < code.size() ? successors(I) : std::vector<uint32_t>{};
    if (std::any_of(Edges.begin(), Edges.end(), enters) ||
        (code[I].target && enters(*code[I].target)) ||
        std::any_of(code[I].targets.begin(), code[I].targets.end(), enters))
      return false;
  }
  for (const auto &Region : method.tries)
    for (const auto &Handler : Region.handlers) {
      budget.tick();
      if (enters(Handler.target))
        return false;
    }
  conditional_allocations.emplace(Root.pc,
                                  ConditionalAllocation{Index, End, Members});
  for (size_t I : Members)
    conditional_allocation_owners[code[I].pc] = Root.pc;
  constructors[Init.pc] = Root.pc;
  minimum_java_release = std::max(minimum_java_release, 14U);
  return true;
}

std::string
Body::conditionalAllocationArgument(const ConditionalAllocation &Plan,
                                    const std::string &Result) {
  Lines Out(budget, true);
  const auto Label = "__neverdArguments" + std::to_string(code[Plan.begin].pc);
  Out.extend({"switch (0) { default -> {",
              "  pc = " + std::to_string(code[Plan.begin + 1].pc) + ";",
              "  " + Label + ": while (true) {", "    switch (pc) {"});
  std::set<size_t> Skipped;
  for (size_t I : Plan.members) {
    if (I == Plan.begin || I == Plan.end || Skipped.contains(I))
      continue;
    const auto &Op = code[I];
    const auto Nested = conditional_allocations.find(Op.pc);
    const bool Child = Nested != conditional_allocations.end() &&
                       Nested->second.begin == I &&
                       Nested->second.end != Plan.end &&
                       Plan.members.contains(Nested->second.end);
    Out.append("    case " + std::to_string(Op.pc) + ": {");
    for (const auto &Line : operation(Op, states.at(Op.pc), true).second)
      Out.append("      " + Line);
    const auto Edges = successors(I);
    if (starts(Op.opcode, "if-"))
      Out.append("      pc = (" + condition(Op, states.at(Op.pc), true) +
                 ") ? " + std::to_string(target(Op)) + " : " +
                 std::to_string(code[I + 1].pc) + ";");
    else if (Op.opcode == "packed-switch" || Op.opcode == "sparse-switch") {
      Out.append("      switch (v" + std::to_string(Op.registers[0]) + ") {");
      for (size_t J = 0; J < Op.keys.size(); ++J)
        Out.append("        case " +
                   hexLiteral(static_cast<uint64_t>(Op.keys[J]), 32) +
                   ": pc = " + std::to_string(Op.targets[J]) + "; break;");
      Out.extend(
          {"        default: pc = " + std::to_string(code[I + 1].pc) + ";",
           "      }"});
    } else if (Op.opcode == "throw") {
      Out.append("    }");
      continue;
    } else {
      if (!Child && Edges.size() != 1)
        fail("conditional allocation lost its closed successor proof");
      Out.append(
          "      pc = " +
          std::to_string(Child ? code[Nested->second.end].pc : Edges[0]) + ";");
    }
    Out.extend({"      continue " + Label + ";", "    }"});
    if (Child)
      for (size_t Member : Nested->second.members)
        if (Member != Nested->second.begin && Member != Nested->second.end)
          Skipped.insert(Member);
  }
  Out.extend({"    case " + std::to_string(code[Plan.end].pc) + ": break " +
                  Label + ";",
              "    default: throw new java.lang.AssertionError(\"Invalid "
              "recovered argument flow\");",
              "    }", "  }", "  yield " + Result + ";", "} }"});
  return Out.text();
}

// A verifier-valid new-instance may be discarded without invoking <init>.
// When the same register is cleared and dereferenced unconditionally, use
// that actual invocation as the Java constructor argument. The JVM performs
// new (including class initialization) before evaluating the argument. Its
// null receiver prevents the constructor from executing on every path.
bool Body::planAbruptAllocationArgument(size_t index, const std::string &type) {
  if (index + 3 >= code.size() || !concreteAllocationClass(type) ||
      !independentSourceScope(classes.at(type)) ||
      !has(classes.at(type).access, "public") || isAnonymous(classes.at(type)))
    return false;
  const auto &allocation = code[index], &zero = code[index + 1],
             &invoke = code[index + 2], &abrupt = code[index + 3];
  const auto *call = std::get_if<MethodRef>(&invoke.reference);
  if (zero.opcode != "const/4" || zero.literal != Literal{int64_t(0)} ||
      zero.registers != allocation.registers ||
      baseOpcode(invoke.opcode) != "invoke-interface" || !call ||
      !call->parameters.empty() || !isReference(call->returns) ||
      invoke.registers != zero.registers || abrupt.opcode != "throw" ||
      abrupt.registers != zero.registers)
    return false;
  for (size_t i = index; i <= index + 3; ++i) {
    budget.tick();
    if (!handlers(code[i].pc).empty() ||
        (i != index && nonlinear_entries.contains(code[i].pc)))
      return false;
  }
  const auto &producer = expressionInvocation(invoke, "abrupt allocation argument");
  if (producer.generic_signature || producer.reference != *call)
    return false;
  const MethodRef ctor{type, "<init>", {call->returns}, "V"};
  const auto &declared = classes.at(type).methods;
  const auto found = std::find_if(declared.begin(), declared.end(),
                                  [&](const Method &m) { return m.reference == ctor; });
  if (found == declared.end() || !has(found->access, "public") ||
      has(found->access, "varargs") || has(found->access, "synthetic") ||
      found->generic_signature ||
      (found->declared_throws && !found->declared_throws->empty()) ||
      changesMethodContract(ctor, inheritance))
    return false;
  abrupt_argument_allocations.emplace(invoke.pc, type);
  return true;
}

void Body::planNestedAllocation(size_t Index) {
  auto plan = buildAllocationPath(Index, code.size(), code.size(), code.size());
  for (size_t i = Index; i <= by_pc.at(plan.end); ++i)
    allocation_region_owners.emplace(code[i].pc, plan.start);
  allocation_regions.emplace(plan.start, std::move(plan));
}

bool Body::planForkedAllocation(size_t index) {
  if (verifying_constructor_arguments || (method.reference.name == "<init>" && first == 0))
    return false;
  const auto &root = code[index];
  // Only the comparison of an unchanged incoming register moves before new.
  // Every allocation, read, and call remains in its original order on each path.
  auto available = [&](size_t pos) {
    const auto pc = code[pos].pc;
    return handlers(pc).empty() && !allocation_region_owners.contains(pc) &&
           !conditional_allocation_owners.contains(pc) &&
           !constructor_choice_owners.contains(pc) &&
           !forked_allocation_owners.contains(pc);
  };
  size_t branch = index + 1;
  for (; branch < code.size() && branch - index <= 256; ++branch) {
    budget.tick();
    const auto &op = code[branch];
    if (op.opcode == "if-eqz" || op.opcode == "if-nez")
      break;
    if (op.target || !op.targets.empty() || anyPrefix(op.opcode, {"return", "throw"}) ||
        allocationInitializerShape(root, op))
      return false;
  }
  if (branch + 1 >= code.size() || branch - index > 256)
    return false;
  const auto &test = code[branch];
  if (!test.target || test.registers.size() != 1 ||
      !std::holds_alternative<std::monostate>(test.literal) ||
      !std::holds_alternative<std::monostate>(test.reference) ||
      !test.targets.empty() || !test.keys.empty() || !test.data.empty() ||
      test.element_width)
    return false;
  const size_t taken = by_pc.at(*test.target);
  if (taken <= branch + 1)
    return false;
  for (size_t i = index; i <= branch; ++i) {
    budget.tick();
    const auto &op = code[i];
    if (!available(i) || (i != index && nonlinear_entries.contains(op.pc)))
      return false;
    if (i != branch &&
        (std::find(op.registers.begin(), op.registers.end(), test.registers[0]) !=
             op.registers.end() || op.opcode.find("wide") != std::string::npos))
      return false;
  }
  auto endOfArm = [&](size_t start, size_t limit) -> std::optional<size_t> {
    for (size_t i = start; i < limit && i - start + branch - index <= 256; ++i) {
      budget.tick();
      const auto &op = code[i];
      if (!available(i) || (i != taken && nonlinear_entries.contains(op.pc)) ||
          op.target || !op.targets.empty() ||
          anyPrefix(op.opcode, {"return", "throw", "move-exception"}))
        break;
      if (allocationInitializerShape(root, op))
        return i;
    }
    return {};
  };
  const auto left = endOfArm(branch + 1, taken);
  const auto right = endOfArm(taken, code.size());
  if (!left || !right || *left + 1 >= taken || *right + 1 >= code.size())
    return false;
  // Adjacent choices with an immediate join belong to planConstructorChoice,
  // which proves the shared captured arguments on both overloads.
  if (branch == index + 1 && anyPrefix(code[*left + 1].opcode, {"goto", "return"}))
    return false;
  // Other arm instructions have only physical fallthrough predecessors.
  // The target itself must have exactly the original comparison as its entry.
  for (size_t i = first; i < code.size(); ++i) {
    budget.tick();
    if (i == branch)
      continue;
    const auto &op = code[i];
    if (op.target == test.target ||
        std::find(op.targets.begin(), op.targets.end(), *test.target) != op.targets.end())
      return false;
  }
  const auto preceding = successors(taken - 1);
  if (std::find(preceding.begin(), preceding.end(), *test.target) != preceding.end())
    return false;
  auto fallthrough = buildAllocationPath(index, *left + 1, branch, branch + 1);
  auto targetPath = buildAllocationPath(index, *right + 1, branch, taken);
  for (size_t i = index; i <= *left; ++i)
    forked_allocation_owners.emplace(code[i].pc, root.pc);
  for (size_t i = taken; i <= *right; ++i)
    forked_allocation_owners.emplace(code[i].pc, root.pc);
  forked_allocations.emplace(root.pc, ForkedAllocation{branch, std::move(fallthrough),
                                                      std::move(targetPath)});
  return true;
}

Body::AllocationRegion Body::buildAllocationPath(size_t Index, size_t Limit,
                                                  size_t Branch, size_t Arm) {
  if (method.reference.name == "<init>" && first == 0 &&
      !verifying_constructor_arguments)
    fail("nested allocation before constructor delegation needs a separate "
         "prefix proof");
  AllocationRegion Plan;
  Plan.start = code[Index].pc;
  const auto &Cleanup = handlers(Plan.start);
  auto input = [&](unsigned Register) {
    if (auto I = Plan.live.find(Register); I != Plan.live.end()) {
      const auto &Value = Plan.values[I->second];
      if (Value.kind == "new" && !Value.initializer)
        fail("nested allocation exposes an uninitialized receiver");
      return I->second;
    }
    size_t Id = Plan.values.size();
    Plan.values.push_back({"input", {}, Index, Register, {}, {}});
    Plan.live.emplace(Register, Id);
    return Id;
  };
  auto define = [&](unsigned Register, AllocationValue Value) {
    size_t Id = Plan.values.size();
    Value.register_index = Register;
    Plan.values.push_back(std::move(Value));
    Plan.live[Register] = Id;
    Plan.written.insert(Register);
    return Id;
  };
  auto arguments = [&](const Instruction &Op, const MethodRef &Ref,
                       size_t Cursor, std::vector<size_t> &Values) {
    for (const auto &Type : Ref.parameters) {
      if (Cursor >= Op.registers.size())
        fail("nested allocation invocation is missing argument words");
      Values.push_back(input(Op.registers[Cursor]));
      if (wordWidth(Type) == 2 &&
          (Cursor + 1 >= Op.registers.size() ||
           Op.registers[Cursor + 1] != Op.registers[Cursor] + 1))
        fail("nested allocation invocation has a broken wide argument");
      Cursor += wordWidth(Type);
    }
    if (Cursor != Op.registers.size())
      fail("nested allocation invocation has extra argument words");
  };
  size_t End = Index;
  size_t length = 0;
  for (; End < Limit; End = End + 1 == Branch ? Arm : End + 1) {
    budget.tick();
    if (++length > 257 || Plan.values.size() > 256)
      fail("nested allocation expression exceeds its bounded domain");
    const auto &Op = code[End];
    // This graph owns a linear region. Only an array payload address is
    // data provenance here; other target metadata needs a control-flow proof.
    if (Op.target && Op.opcode != "fill-array-data")
      fail("nested allocation has an unproved executable target");
    if (End != Index && End != Arm && nonlinear_entries.contains(Op.pc))
      fail("nested allocation crosses an exception or branch entry");
    if (canThrow(Op) && !sameHandlers(Cleanup, handlers(Op.pc))) {
      const auto &Other = handlers(Op.pc);
      if (Cleanup.size() != Other.size() ||
          !std::equal(
              Cleanup.begin(), Cleanup.end(), Other.begin(),
              [](const auto &A, const auto &B) { return A.type == B.type; }))
        fail("nested allocation changes its ordered exception types");
    }
    // Even equal handler targets can observe a changed register value. Retain
    // the original prethrow origins for every later effect, independently of
    // the physical frame that stays committed until expression completion.
    if (canThrow(Op) && End != Index && !Cleanup.empty())
      Plan.handler_inputs.emplace(Op.pc, Plan.live);
    const auto Base = baseOpcode(Op.opcode);
    const auto *Call = std::get_if<MethodRef>(&Op.reference);
    if (Op.opcode == "new-instance") {
      const auto *Type = std::get_if<std::string>(&Op.reference);
      if (!Type || Op.registers.size() != 1 || !concreteAllocationClass(*Type))
        fail("nested allocation requires a known concrete source class");
      // Resolve through the shared scope and capture receipts, including
      // explicit-outer members and reused anonymous declarations.
      // The canonical anonymous planner already owns its exact initializer
      // context, unique allocation and empty constructor. Its source is an
      // expression; a fabricated qualified class name would lose that scope.
      if (!anonymous_sources.contains(*Type))
        sourceType(*Type);
      define(Op.registers[0], {"new", *Type, End, 0, {}, {}});
      Plan.effects.push_back(Op.pc);
    } else if (Base == "invoke-direct" && Call && Call->name == "<init>") {
      const auto *Ref = std::get_if<MethodRef>(&Op.reference);
      if (!Ref || Ref->name != "<init>" || Op.registers.empty())
        fail("nested allocation requires an exact constructor");
      const auto Receiver = Plan.live.find(Op.registers[0]);
      if (Receiver == Plan.live.end())
        fail("nested allocation constructor lacks its original receiver");
      const size_t Id = Receiver->second;
      if (Plan.values[Id].kind != "new" || Plan.values[Id].initializer ||
          Plan.values[Id].type != Ref->owner || Ref->returns != "V")
        fail("nested allocation constructor receiver changed");
      const auto *Declaration = validateInvocation(Op);
      if (!Declaration || !expressionMemberAccessible(*Ref, *Declaration))
        fail("nested allocation constructor declaration is not proven");
      // Input insertion may grow the value vector; keep indices, not
      // references.
      std::vector<size_t> Args;
      arguments(Op, *Ref, 1, Args);
      Plan.values[Id].arguments = std::move(Args);
      Plan.values[Id].initializer = End;
      Plan.effects.push_back(Op.pc);
      if (Id == 0) {
        Plan.end = Op.pc;
        break;
      }
    } else if (Base == "invoke-static" || Base == "invoke-virtual" ||
               Base == "invoke-interface" || Base == "invoke-super" ||
               Base == "invoke-direct") {
      const auto *Ref = std::get_if<MethodRef>(&Op.reference);
      const auto &Declaration =
          expressionInvocation(Op, "nested allocation", true);
      const bool GenericSuper =
          generics.methods.contains(Declaration.reference) &&
          generics.superclass_calls.contains({method.reference, *Ref});
      if (Base == "invoke-super" &&
          ((!Ref->parameters.empty() && !GenericSuper) ||
           has(Declaration.access, "abstract")))
        fail("nested allocation needs a concrete nullary or proven generic "
             "superclass call");
      // A DEX invocation may discard any declared result. Keep it as an
      // ordered call statement, without inventing a result register. An
      // actual move-result still needs the exact carrier and entry proof.
      const bool HasResult = Ref->returns != "V" && End + 1 < Limit &&
                             starts(code[End + 1].opcode, "move-result");
      AllocationValue Value{"call", HasResult ? Ref->returns : "V", End, 0, {},
                            {}};
      size_t Cursor = Base == "invoke-static" ? 0 : 1;
      if (Cursor) {
        if (Op.registers.empty())
          fail("nested allocation call has no receiver");
        Value.arguments.push_back(input(Op.registers[0]));
      }
      arguments(Op, *Ref, Cursor, Value.arguments);
      if (!HasResult) {
        Plan.values.push_back(std::move(Value));
        Plan.effects.push_back(Op.pc);
        continue;
      }
      const auto &Result = code[++End];
      if (Result.opcode != (isReference(Ref->returns) ? "move-result-object"
                            : wordWidth(Ref->returns) == 2 ? "move-result-wide"
                                                           : "move-result") ||
          Result.registers.size() != 1 || nonlinear_entries.contains(Result.pc))
        fail("nested allocation call lacks its adjacent declared result");
      define(Result.registers[0], std::move(Value));
      Plan.effects.push_back(Op.pc);
    } else if (allocationFieldRead(Op.opcode)) {
      const auto *Ref = std::get_if<FieldRef>(&Op.reference);
      const bool Instance = !starts(Op.opcode, "sget");
      // Ordinary operation verification owns the descriptor/opcode agreement,
      // including wide, floating and narrow integer fields.
      if (!Ref || Op.registers.size() != (Instance ? 2u : 1u))
        fail("nested allocation field read has an invalid shape");
      AllocationValue Value{"field", Ref->type, End, 0, {}, {}};
      if (Instance)
        Value.arguments.push_back(input(Op.registers[1]));
      define(Op.registers[0], std::move(Value));
      Plan.effects.push_back(Op.pc);
    } else if (const auto Arithmetic = integerPreparation(Op)) {
      if (Op.registers.size() !=
              (Arithmetic->two_address || Arithmetic->literal ? 2u : 3u) ||
          (Arithmetic->literal && !std::holds_alternative<int64_t>(Op.literal)))
        fail("nested allocation integer operation has an invalid shape");
      std::vector<size_t> Arguments{
          input(Op.registers[Arithmetic->two_address ? 0 : 1])};
      if (!Arithmetic->literal)
        Arguments.push_back(
            input(Op.registers[Arithmetic->two_address ? 1 : 2]));
      define(Op.registers[0],
             {"integer", "I", End, 0, std::move(Arguments), {}});
    } else if (const auto Binary = longBinary(Op.opcode)) {
      if (Op.registers.size() != (Binary->two_address ? 2u : 3u))
        fail("nested allocation long binary operation has an invalid shape");
      const auto Left = input(Op.registers[Binary->two_address ? 0 : 1]);
      const auto Right = input(Op.registers[Binary->two_address ? 1 : 2]);
      define(Op.registers[0], {"long-binary", "J", End, 0, {Left, Right}, {}});
      // Even an unused or overwritten quotient/remainder can fail before
      // later arguments or constructors. Keep that original effect in order.
      if (Binary->can_throw)
        Plan.effects.push_back(Op.pc);
    } else if (const auto Floating = floatingArithmetic(Op.opcode)) {
      if (Op.registers.size() != (Floating->two_address ? 2u : 3u))
        fail("nested allocation floating operation has an invalid shape");
      const auto Left = input(Op.registers[Floating->two_address ? 0 : 1]);
      const auto Right = input(Op.registers[Floating->two_address ? 1 : 2]);
      define(Op.registers[0],
             {"floating", Floating->type, End, 0, {Left, Right}, {}});
    } else if (const auto Unary = scalarUnary(Op.opcode)) {
      if (Op.registers.size() != 2)
        fail("nested allocation scalar unary operation has an invalid shape");
      const auto Source = input(Op.registers[1]);
      define(Op.registers[0], {"unary", Unary->first, End, 0, {Source}, {}});
    } else if (const auto Conversion = scalarConversion(Op.opcode)) {
      if (Op.registers.size() != 2)
        fail("nested allocation scalar conversion has an invalid shape");
      const auto Source = input(Op.registers[1]);
      define(Op.registers[0],
             {"conversion", Conversion->second, End, 0, {Source}, {}});
    } else if (arrayLoad(Op.opcode)) {
      if (Op.registers.size() != 3)
        fail("nested allocation array read has an invalid shape");
      const auto Array = input(Op.registers[1]);
      const auto IndexValue = input(Op.registers[2]);
      const auto Carrier = Op.opcode == "aget-object" ? "Ljava/lang/Object;"
                           : Op.opcode == "aget-wide" ? "J"
                                                      : "I";
      define(Op.registers[0],
             {"array-read", Carrier, End, 0, {Array, IndexValue}, {}});
      Plan.effects.push_back(Op.pc);
    } else if (Op.opcode == "array-length") {
      if (Op.registers.size() != 2)
        fail("nested allocation array length has an invalid shape");
      const auto Source = input(Op.registers[1]);
      define(Op.registers[0], {"array-length", "I", End, 0, {Source}, {}});
      // A null array throws at this exact point. Keep the read after the
      // outer allocation and before later calls or nested allocations, even
      // when its integer result is not a constructor argument.
      Plan.effects.push_back(Op.pc);
    } else if (Op.opcode == "new-array") {
      const auto &Type = referenceType(Op);
      if (Op.registers.size() != 2 || !starts(Type, "["))
        fail("nested allocation array creation has an invalid shape");
      const auto Length = input(Op.registers[1]);
      define(Op.registers[0], {"array", Type, End, 0, {Length}, {}});
      Plan.effects.push_back(Op.pc);
    } else if (Base == "filled-new-array") {
      const auto &Type = referenceType(Op);
      if (!starts(Type, "[") || Type.substr(1) == "J" ||
          Type.substr(1) == "D" || End + 1 >= Limit)
        fail("nested allocation filled array needs single-word elements");
      std::vector<size_t> Values;
      for (const auto Register : Op.registers)
        Values.push_back(input(Register));
      const auto Producer = End;
      const auto &Result = code[++End];
      if (Result.opcode != "move-result-object" ||
          Result.registers.size() != 1 || nonlinear_entries.contains(Result.pc))
        fail("nested allocation filled array lacks its adjacent result");
      define(Result.registers[0],
             {"filled-array", Type, Producer, 0, std::move(Values), {}});
      Plan.effects.push_back(Op.pc);
    } else if (Op.opcode == "fill-array-data") {
      if (Op.registers.size() != 1)
        fail("nested allocation array payload has an invalid shape");
      const auto Array = input(Op.registers[0]);
      Plan.values.push_back({"array-fill", "V", End, 0, {Array}, {}});
      // The payload's target is a data address, never a control-flow edge.
      // Retain the original fill as one ordered effect on the same array.
      Plan.effects.push_back(Op.pc);
    } else if (starts(Base, "aput")) {
      if (Op.registers.size() != 3)
        fail("nested allocation array store has an invalid shape");
      std::vector<size_t> Values;
      for (const auto Register : Op.registers)
        Values.push_back(input(Register));
      Plan.values.push_back(
          {"array-store", "V", End, 0, std::move(Values), {}});
      // Stores are effects, not replacements for an immutable array value.
      // Preserve both fresh-array and incoming-array identities through them.
      Plan.effects.push_back(Op.pc);
    } else if (Base == "move" || Base == "move-wide") {
      if (Op.registers.size() != 2)
        fail("nested allocation scalar copy has an invalid shape");
      const auto Source = input(Op.registers[1]);
      define(Op.registers[0], {"bits-copy",
                               Base == "move-wide" ? "bits64" : "bits32",
                               End,
                               0,
                               {Source},
                               {}});
    } else if (Op.opcode == "check-cast" || Base == "move-object") {
      const bool Cast = Op.opcode == "check-cast";
      if (Op.registers.size() != (Cast ? 1u : 2u))
        fail("nested allocation reference preparation has an invalid shape");
      const auto Type = Cast ? referenceType(Op) : "Ljava/lang/Object;";
      if (!isReference(Type))
        fail("nested allocation cast lacks its reference type");
      if (!Cast) {
        const auto Source = Plan.live.find(Op.registers[1]);
        if (Source != Plan.live.end() &&
            Plan.values[Source->second].kind == "new" &&
            !Plan.values[Source->second].initializer) {
          // D8 can move an uninitialized object into a contiguous range-call
          // receiver word. Keep one allocation identity; every other consumer
          // still passes through input() and rejects use before construction.
          Plan.live[Op.registers[0]] = Source->second;
          Plan.written.insert(Op.registers[0]);
          continue;
        }
      }
      const auto Source = input(Op.registers[Cast ? 0 : 1]);
      define(Op.registers[0],
             {Cast ? "cast" : "move", Type, End, 0, {Source}, {}});
      if (Cast)
        Plan.effects.push_back(Op.pc);
    } else if (Op.opcode == "const-string" ||
               Op.opcode == "const-string/jumbo") {
      if (Op.registers.size() != 1 ||
          !std::holds_alternative<std::string>(Op.literal))
        fail("nested allocation string has an invalid shape");
      define(Op.registers[0], {"string", "Ljava/lang/String;", End, 0, {}, {}});
      Plan.effects.push_back(Op.pc);
    } else if (Op.opcode == "const-class") {
      if (Op.registers.size() != 1 || !isReference(referenceType(Op)))
        fail("nested allocation class literal has an invalid shape");
      define(Op.registers[0], {"class", "Ljava/lang/Class;", End, 0, {}, {}});
      Plan.effects.push_back(Op.pc);
    } else if (Op.opcode == "const" || starts(Op.opcode, "const/") ||
               starts(Op.opcode, "const-wide")) {
      const auto *Literal = std::get_if<int64_t>(&Op.literal);
      if (!Literal || Op.registers.size() != 1)
        fail("nested allocation constant has an invalid shape");
      define(Op.registers[0], {"constant",
                               starts(Op.opcode, "const-wide") ? "bits64"
                               : *Literal == 0                 ? "zero"
                                                               : "bits32",
                               End,
                               0,
                               {},
                               {}});
    } else if (Op.opcode != "nop")
      fail("nested allocation contains an unsupported preparation");
  }
  if (End >= Limit || !Plan.values[0].initializer)
    fail("nested allocation lacks its outer constructor");
  for (const auto &Value : Plan.values)
    if (Value.kind == "new") {
      if (!Value.initializer)
        fail("nested allocation leaves an object uninitialized");
      constructors[code[*Value.initializer].pc] = code[Value.instruction].pc;
    }
  return Plan;
}

void Body::verifyAllocationHandlers(const AllocationRegion &Plan) {
  // Original register copies are committed only after construction succeeds.
  // Once ordinary dataflow has converged, reuse its operation verifier with
  // those words unavailable. An unchanged input or a handler-local definition
  // can then feed calls, writes and a shared tail without a second opcode/type
  // policy, while any observation of a deferred copy still fails closed.
  // Emission dispatches through the allocation entry with its incoming frame.
  // Original input words remain committed even when later argument copies
  // overwrite those same registers. Per-effect origin proofs below establish
  // that every observed cleanup word still denotes the original input.
  const std::set<unsigned> Uncommitted{code[by_pc.at(Plan.start)].registers[0]};
  // Catch bodies can themselves be protected by finally, branch into shared
  // cleanup, or retry the allocation. Prove their complete graph;
  // physical instruction order is not a control-flow or commitment proof.
  std::map<uint32_t, std::vector<uint32_t>> Normal, Exceptional;
  std::set<uint32_t> Visits;
  auto member = [&](uint32_t PC) {
    const auto Region = allocation_region_owners.find(PC);
    const auto Choice = constructor_choice_owners.find(PC);
    return (Region != allocation_region_owners.end() &&
            Region->second == Plan.start) ||
           (Choice != constructor_choice_owners.end() &&
            Choice->second == Plan.start);
  };
  auto visit = [&](uint32_t Entry) {
    // A catch can join the rest of a large method. Bound work with the shared
    // budget and storage with unique validated instruction identities, rather
    // than making continuation length a semantic restriction or stack depth.
    std::deque<uint32_t> Pending;
    auto enqueue = [&](uint32_t PC) {
      budget.tick();
      if (Visits.insert(PC).second)
        Pending.push_back(PC);
    };
    enqueue(Entry);
    while (!Pending.empty()) {
      const auto PC = Pending.front();
      Pending.pop_front();
      const auto At = by_pc.at(PC);
      if (At < first)
        fail("nested allocation handler re-enters its constructor prefix");
      const auto &Op = code[At];
      Normal[PC] = successors(At);
      // The shared monitor receipt proves acquisition, the acquired object's
      // identity and every release. Its cleanup release cannot recursively
      // catch itself; ordinary dataflow may retain that conservative opcode
      // edge for coverage.
      if (canThrow(Op) && !provenMonitorRelease(Op))
        for (const auto &H : handlers(PC)) {
          budget.tick();
          Exceptional[PC].push_back(H.target);
        }
      if (Normal.at(PC).empty() && !starts(Op.opcode, "return") &&
          Op.opcode != "throw")
        fail("nested allocation handler has no independent result");
      for (auto Target : Normal.at(PC)) {
        if (code[by_pc.at(Target)].opcode == "move-exception")
          fail("nested allocation handler normally enters an exception value");
        if (Target != Plan.start && member(Target) &&
            (!member(PC) || PC == Plan.end))
          fail("nested allocation handler enters the middle of its allocation");
        enqueue(Target);
      }
      for (auto Target : Exceptional[PC]) {
        if (member(Target))
          fail("nested allocation handler enters the middle of its allocation");
        enqueue(Target);
      }
    }
  };
  std::map<uint32_t, State> Available;
  auto merge = [&](uint32_t PC, const State &Incoming) {
    budget.tick(method.registers + 1);
    auto [I, Fresh] = Available.emplace(PC, Incoming);
    if (!Fresh)
      for (size_t Reg = 0; Reg < Incoming.size(); ++Reg)
        I->second[Reg].insert(Incoming[Reg].begin(), Incoming[Reg].end());
  };
  for (const auto &H : handlers(Plan.start)) {
    const auto &Entry = code[by_pc.at(H.target)];
    if (Entry.opcode != "move-exception" || Entry.registers.size() != 1)
      fail("nested allocation handler has no independent exception entry");
    State Incoming = states.at(Plan.start);
    for (auto Register : Uncommitted)
      Incoming.at(Register) = Strings{"?"};
    merge(H.target, Incoming);
    visit(H.target);
  }
  // A handler may return to an outer loop and then start a fresh allocation.
  // Reuse ordinary transfer semantics to a fixed point instead of requiring
  // a topological order. Unavailable words remain unavailable at joins until
  // a real definition overwrites them; exceptional edges retain prethrow
  // inputs. The same proof checks both committed words and origin equality.
  auto prove = [&](std::map<uint32_t, State> Inputs, const char *Reason) {
    std::deque<uint32_t> Queue;
    std::set<uint32_t> Queued;
    for (const auto &[PC, Incoming] : Inputs) {
      Queue.push_back(PC);
      Queued.insert(PC);
    }
    while (!Queue.empty()) {
      budget.tick(method.registers + 1);
      const auto PC = Queue.front();
      Queue.pop_front();
      Queued.erase(PC);
      const State Incoming = Inputs.at(PC);
      State Outgoing;
      try {
        Outgoing = operation(code[by_pc.at(PC)], Incoming, true).first;
      } catch (const std::runtime_error &Error) {
        fail(std::string(Reason) + Error.what());
      }
      auto combine = [&](uint32_t Target, const State &Value) {
        budget.tick(method.registers + 1);
        auto [At, Changed] = Inputs.emplace(Target, Value);
        if (!Changed)
          for (unsigned R = 0; R < method.registers; ++R) {
            const auto Before = At->second[R].size();
            At->second[R].insert(Value[R].begin(), Value[R].end());
            Changed |= Before != At->second[R].size();
          }
        if (Changed && Queued.insert(Target).second)
          Queue.push_back(Target);
      };
      for (auto Target : Normal.at(PC))
        combine(Target, Outgoing);
      for (auto Target : Exceptional.at(PC))
        combine(Target, Incoming);
    }
  };
  // Register origins are identities, not verifier types. A copy adapter may
  // change physical words while preserving the values visible to a common
  // cleanup tail. Unknown/new/computed values remain distinct and unavailable.
  using WordOrigins = std::vector<std::string>;
  struct AdapterPoint {
    uint32_t pc;
    WordOrigins words;
  };
  auto entryWords = [&] {
    WordOrigins Words;
    for (unsigned R = 0; R < method.registers; ++R)
      Words.push_back("input:" + std::to_string(R));
    return Words;
  };
  auto adapter = [&](uint32_t Start, WordOrigins Words) {
    std::vector<AdapterPoint> Points;
    std::set<uint32_t> Seen;
    uint32_t PC = Start;
    for (;;) {
      budget.tick(method.registers + 1);
      if (!Seen.insert(PC).second ||
          (PC >= Plan.start && PC <= Plan.end))
        fail("nested allocation exception adapter is cyclic or re-enters its "
             "allocation");
      Points.push_back({PC, Words});
      const auto &Op = code[by_pc.at(PC)];
      const auto Base = baseOpcode(Op.opcode);
      if (PC == Start) {
        if (Op.opcode != "move-exception" || Op.registers.size() != 1)
          fail("nested allocation exception adapter lacks its exception entry");
        Words.at(Op.registers[0]) = "exception";
      } else if (Base == "move-object" || Base == "move" ||
                 Base == "move-wide") {
        if (Op.registers.size() != 2)
          fail("nested allocation exception adapter has a malformed copy");
        const auto Low = Words.at(Op.registers[1]);
        const auto High =
            Base == "move-wide" ? Words.at(Op.registers[1] + 1) : std::string{};
        Words.at(Op.registers[0]) = Low;
        if (Base == "move-wide")
          Words.at(Op.registers[0] + 1) = High;
      } else if (Base == "const" || Base == "const-wide") {
        const auto *Literal = std::get_if<int64_t>(&Op.literal);
        const bool Wide = Base == "const-wide";
        if (!Literal || Op.registers.size() != 1 ||
            Op.registers[0] >= method.registers ||
            (Wide && Op.registers[0] + 1 >= method.registers))
          fail("nested allocation exception adapter has a malformed constant");
        // The ordinary operation verifier still owns these definitions and
        // their later uses. Equal decoded bits have one origin across opcode
        // encodings; narrow values and the two wide halves stay distinct.
        const auto Bits = Wide ? static_cast<uint64_t>(*Literal)
                               : static_cast<uint32_t>(*Literal);
        const auto Origin = std::string(Wide ? "constant64:" : "constant32:") +
                            std::to_string(Bits);
        Words.at(Op.registers[0]) = Origin;
        if (Wide)
          Words.at(Op.registers[0] + 1) = "high:" + Origin;
      } else if (Op.opcode != "nop" && !starts(Op.opcode, "goto"))
        break;
      const auto Next = successors(by_pc.at(PC));
      if (Next.size() != 1)
        fail("nested allocation exception adapter has an ambiguous successor");
      PC = Next[0];
    }
    return Points;
  };
  std::function<std::optional<unsigned>(size_t)> inputOrigin =
      [&](size_t Id) -> std::optional<unsigned> {
    budget.tick();
    const auto &Value = Plan.values.at(Id);
    if (Value.kind == "input")
      return Value.register_index;
    if ((Value.kind == "move" || Value.kind == "bits-copy") &&
        Value.arguments.size() == 1)
      return inputOrigin(Value.arguments[0]);
    return {};
  };
  for (const auto &[Effect, Live] : Plan.handler_inputs) {
    auto Actual = entryWords();
    for (const auto &[Register, Id] : Live) {
      const auto Origin = inputOrigin(Id);
      Actual.at(Register) = Origin ? "input:" + std::to_string(*Origin)
                                   : "value:" + std::to_string(Id);
      if (isWide(Plan.values.at(Id).type))
        Actual.at(Register + 1) = Origin
                                      ? "input:" + std::to_string(*Origin + 1)
                                      : "high:" + std::to_string(Id);
    }
    const auto &Other = handlers(Effect);
    for (size_t H = 0; H < Other.size(); ++H) {
      const auto Root = adapter(handlers(Plan.start)[H].target, entryWords());
      const auto Candidate = adapter(Other[H].target, Actual);
      const AdapterPoint *CommonRoot = nullptr, *CommonOther = nullptr;
      std::map<uint32_t, const AdapterPoint *> CandidateByPC;
      for (const auto &Point : Candidate) {
        budget.tick();
        CandidateByPC.emplace(Point.pc, &Point);
      }
      for (const auto &A : Root) {
        budget.tick();
        if (const auto I = CandidateByPC.find(A.pc); I != CandidateByPC.end()) {
          CommonRoot = &A;
          CommonOther = I->second;
          break;
        }
      }
      if (!CommonRoot)
        fail("nested allocation exception adapters have no common cleanup");
      State Equal = states.at(CommonRoot->pc);
      for (unsigned R = 0; R < method.registers; ++R)
        if (CommonRoot->words[R] != CommonOther->words[R])
          Equal[R] = Strings{"?"};
      // Reuse the complete ordinary verifier and bounded handler graph. A
      // differing word must be independently overwritten before any read;
      // reads of same-typed but different objects therefore cannot pass.
      prove({{CommonRoot->pc, Equal}},
            "nested allocation exception adapters change a cleanup input: ");
    }
  }
  prove(std::move(Available),
        "nested allocation handler cannot prove committed inputs: ");
}

void Body::declareAllocationValues(Lines &Out, const AllocationRegion &Plan,
                                   bool captureInputs) {
    for (size_t I = 0; I < Plan.values.size(); ++I) {
      const auto &Value = Plan.values[I];
      const auto Name = allocationLocal(Plan.start, I);
      if (Value.kind == "input") {
        const auto reg = std::to_string(Value.register_index);
        Out.extend({"java.lang.Object " + Name + "Object = " + (captureInputs ? "o" + reg : "null") + ";",
                    "long " + Name + "Word = " + (captureInputs ? "w" + reg : "0L") + ";",
                    "int " + Name + "Bits = " + (captureInputs ? "v" + reg : "0") + ";"});
      } else if (Value.kind != "constant" && Value.type != "V")
        Out.append(isWide(Value.type) ? "long " + Name + " = 0L;"
                   : !isReference(Value.type)
                       ? "int " + Name + " = 0;"
                       : "java.lang.Object " + Name + " = null;");
    }
}

std::vector<std::string>
Body::emitNestedAllocation(const AllocationRegion &Plan) {
  std::vector<uint32_t> Effects;
  std::set<size_t> Done, Active;
  std::function<std::string(size_t)> evaluate;
  auto value = [&](size_t Id, uint32_t PC, unsigned Register,
                   const std::string &Wanted) {
    const auto &Node = Plan.values.at(Id);
    std::string Carrier;
    if (isReference(Wanted) && states.at(PC).at(Register) == Strings{"zero"})
      Carrier = "null";
    else if (Node.kind == "input")
      Carrier =
          allocationLocal(Plan.start, Id) + (isReference(Wanted)      ? "Object"
                                             : wordWidth(Wanted) == 2 ? "Word"
                                                                      : "Bits");
    else if (Node.kind == "constant") {
      auto Literal = std::get<int64_t>(code[Node.instruction].literal);
      Carrier = isReference(Wanted) && Literal == 0
                    ? "null"
                    : hexLiteral(static_cast<uint64_t>(Literal),
                                 Node.type == "bits64" ? 64 : 32);
    } else
      Carrier = evaluate(Id);
    return read(states.at(PC), Register, Wanted, true, Carrier);
  };
  struct EffectPrefix {
    std::vector<std::string> conditions, steps;
    bool has_void = false;
  };
  auto predecessors = [&](const Instruction &Op) {
    EffectPrefix Prefix;
    auto orderedPrefix = [&] {
      return Effects.size() <= Plan.effects.size() &&
             std::equal(Effects.begin(), Effects.end(), Plan.effects.begin());
    };
    while (orderedPrefix() && Effects.size() < Plan.effects.size() &&
           Plan.effects[Effects.size()] != Op.pc) {
      const auto Next = Plan.effects[Effects.size()];
      auto Owner = std::find_if(
          Plan.values.begin(), Plan.values.end(), [&](const auto &Earlier) {
            return code[Earlier.instruction].pc == Next ||
                   (Earlier.initializer &&
                    code[*Earlier.initializer].pc == Next);
          });
      if (Owner == Plan.values.end() ||
          (!isReference(Owner->type) && !Primitives.contains(Owner->type)))
        fail("nested allocation predecessor has no exact value");
      const auto Before = Effects.size();
      const auto Earlier = evaluate(Owner - Plan.values.begin());
      if (Owner->type == "V") {
        Prefix.has_void = true;
        Prefix.steps.push_back(Earlier + ";");
      } else {
        Prefix.conditions.push_back(
            "(" + Earlier +
            (isReference(Owner->type) ? " == null)" : " == 0)"));
        Prefix.steps.push_back("if (" + Prefix.conditions.back() + ") {}");
      }
      if (Effects.size() == Before)
        fail("nested allocation predecessor was already consumed");
    }
    if (!orderedPrefix() || Effects.size() == Plan.effects.size() ||
        Plan.effects[Effects.size()] != Op.pc)
      fail("nested allocation producer changes original effect order");
    return Prefix;
  };
  auto sequence = [&](const EffectPrefix &Prefix, const std::string &Expr,
                      const std::string &Fallback = "null") {
    if (Prefix.has_void) {
      minimum_java_release = std::max(minimum_java_release, 14u);
      return "(switch (0) { default -> { " + join(Prefix.steps, " ") +
             " yield " + Expr + "; } })";
    }
    return Prefix.conditions.empty()
               ? Expr
               : "((" + join(Prefix.conditions, " | ") + " | true) ? " + Expr +
                     " : " + Fallback + ")";
  };
  evaluate = [&](size_t Id) -> std::string {
    budget.tick();
    const auto &Node = Plan.values.at(Id);
    const auto Name = allocationLocal(Plan.start, Id);
    if (Done.contains(Id))
      return Name;
    if (Active.size() >= 64 || !Active.insert(Id).second)
      fail("nested allocation expression is cyclic or exceeds its depth limit");
    const auto &Op = code[Node.instruction];
    std::string Expr, StorageType = Node.type;
    if (Node.kind == "new" || Node.kind == "call") {
      const auto &Call = Node.kind == "new" ? code[*Node.initializer] : Op;
      const auto &Ref = std::get<MethodRef>(Call.reference);
      size_t Cursor = 0, Argument = 0;
      EffectPrefix Prefix;
      if (Node.kind == "new") {
        // Preserve reads/calls preceding this allocation inside the outer
        // argument expression. Their immutable values may be consumed only
        // by this object's constructor, after its class initialization.
        Prefix = predecessors(Op);
        Effects.push_back(Op.pc);
        Cursor = 1;
      } else if (baseOpcode(Call.opcode) == "invoke-static")
        Expr = staticOwner(Ref.owner) + "." + invocationName(Ref);
      else if (baseOpcode(Call.opcode) == "invoke-super") {
        // Ordinary operation verification proves the immediate parent and
        // actual self receiver before any expression is emitted. Preserve
        // that lexical dispatch; a receiver alias is copied on live writeback.
        Expr = "super." + invocationName(Ref);
        Cursor = 1;
        ++Argument;
      } else {
        Expr = virtualReceiver(Ref, Call, value(Node.arguments[Argument++], Call.pc, Call.registers[0],
                     Ref.owner)) +
               "." + invocationName(Ref);
        Cursor = 1;
      }
      // Prepare the complete argument program after this new/class-init,
      // including original effects not used as a constructor argument. Each
      // producer stays immutable and is consumed once in the original order.
      const auto Preparation =
          Node.kind == "new" ? predecessors(Call) : EffectPrefix{};
      std::vector<std::string> Args;
      for (const auto &Type : Ref.parameters) {
        auto Value = value(Node.arguments[Argument++], Call.pc,
                           Call.registers[Cursor], Type);
        // Strict operation verification already checked the original word
        // against the substituted erasure. Reuse the same scoped source view
        // while this graph retains its immutable origin and evaluation order.
        if (baseOpcode(Call.opcode) == "invoke-super")
          Value = superclassArgument(Ref, Args.size(), std::move(Value));
        if (Node.kind == "new")
          Value = intersectionArgument(Call, states.at(Call.pc), Ref,
                                       Args.size(), std::move(Value));
        Args.push_back(constructorArgument(Ref, Args.size(), std::move(Value)));
        Cursor += wordWidth(Type);
      }
      if (!Preparation.steps.empty()) {
        if (Args.empty())
          fail("nested allocation preparation needs a constructor argument "
               "evaluation site");
        const auto &Type = Ref.parameters[0];
        Args[0] = sequence(Preparation, Args[0],
                           isReference(Type) ? "null"
                           : Type == "Z"     ? "false"
                                             : "0");
      }
      Expr = Node.kind == "new"
                 ? allocationExpression(Node.type, Args)
                 : Expr + "(" + invocationArguments(Ref, Args) + ")";
      Effects.push_back(Call.pc);
      if (Node.kind == "new")
        Expr = sequence(Prefix, Expr);
    } else if (Node.kind == "field") {
      const auto &Ref = std::get<FieldRef>(Op.reference);
      Expr = Node.arguments.empty()
                 ? staticFieldRead(Ref)
                 : fieldReceiver(Ref, Op, value(Node.arguments[0], Op.pc, Op.registers[1], Ref.owner)) +
                       "." + sourceFieldName(Ref, classes, inheritance, budget);
      Effects.push_back(Op.pc);
    } else if (Node.kind == "array" || Node.kind == "filled-array") {
      // Array allocation can fail before later reads or stores. Force earlier
      // effects to finish first, then allocate at its original position.
      const auto Prefix = predecessors(Op);
      if (Node.kind == "array")
        Expr = arrayCreation(
            Node.type, value(Node.arguments[0], Op.pc, Op.registers[1], "I"));
      else {
        std::vector<std::string> Values;
        for (size_t I = 0; I < Node.arguments.size(); ++I)
          Values.push_back(value(Node.arguments[I], Op.pc, Op.registers[I],
                                 Node.type.substr(1)));
        Expr = "new " + sourceType(Node.type) + " {" + join(Values, ", ") + "}";
      }
      Expr = sequence(Prefix, Expr);
      Effects.push_back(Op.pc);
    } else if (Node.kind == "array-fill") {
      const auto Type = arrayType(states.at(Op.pc), Op.registers[0], true);
      const auto Address =
          value(Node.arguments[0], Op.pc, Op.registers[0], Type);
      const auto Local = "__neverdPayload" + std::to_string(Op.pc);
      // Bind the immutable array once, then consume the ordinary range/bit
      // decoder. A short array must fail before any element is overwritten.
      Expr = "{ " + sourceType(Type) + " " + Local + " = " + Address + "; " +
             join(arrayPayload(Op, Type, Local), " ") + " }";
      Effects.push_back(Op.pc);
    } else if (Node.kind == "array-read") {
      const auto Type = arrayType(states.at(Op.pc), Op.registers[1], true);
      Expr = value(Node.arguments[0], Op.pc, Op.registers[1], Type) + "[" +
             value(Node.arguments[1], Op.pc, Op.registers[2], "I") + "]";
      // Graph locals are physical int/long/object carriers. Reuse ordinary
      // floating/boolean storage so aliases copy the original element bits.
      StorageType = Type.substr(1);
      Effects.push_back(Op.pc);
    } else if (Node.kind == "array-store") {
      // The ordinary verifier owns the opcode/element-width and value checks.
      // Use its same array and element views while keeping the store exactly
      // once inside the original constructor argument program.
      const auto Type = arrayType(states.at(Op.pc), Op.registers[1], true);
      const auto [Array, Element] = arrayStoreTypes(Type);
      Expr = value(Node.arguments[1], Op.pc, Op.registers[1], Array) + "[" +
             value(Node.arguments[2], Op.pc, Op.registers[2], "I") +
             "] = " + value(Node.arguments[0], Op.pc, Op.registers[0], Element);
      Effects.push_back(Op.pc);
    } else if (Node.kind == "array-length") {
      const auto Type = arrayType(states.at(Op.pc), Op.registers[1], true);
      Expr = value(Node.arguments[0], Op.pc, Op.registers[1], Type) + ".length";
      Effects.push_back(Op.pc);
    } else if (Node.kind == "string" || Node.kind == "class") {
      // A late literal and a nested allocation share the same ordered
      // predecessor proof; neither may hoist effects before the outer new.
      const auto Prefix = predecessors(Op);
      Expr = Node.kind == "string"
                 ? javaString(std::get<std::string>(Op.literal))
                 : sourceType(referenceType(Op)) + ".class";
      Expr = sequence(Prefix, Expr);
      Effects.push_back(Op.pc);
    } else if (Node.kind == "bits-copy") {
      // DEX move/move-wide copy raw carriers. Converting a float here would
      // change its bits before the eventual argument consumer interprets it.
      const auto Source = Node.arguments.at(0);
      const auto &Original = Plan.values.at(Source);
      if (states.at(Op.pc).at(Op.registers[1]) == Strings{"zero"})
        Expr = "0";
      else if (Original.kind == "input")
        Expr = allocationLocal(Plan.start, Source) +
               (Node.type == "bits64" ? "Word" : "Bits");
      else if (Original.kind == "constant")
        Expr = hexLiteral(static_cast<uint64_t>(std::get<int64_t>(
                              code[Original.instruction].literal)),
                          Node.type == "bits64" ? 64 : 32);
      else
        Expr = evaluate(Source);
    } else if (Node.kind == "integer") {
      const auto Arithmetic = integerPreparation(Op);
      if (!Arithmetic)
        fail("nested allocation lost its integer operation receipt");
      const auto Left =
          value(Node.arguments[0], Op.pc,
                Op.registers[Arithmetic->two_address ? 0 : 1], "I");
      const auto Right =
          Arithmetic->literal
              ? hexLiteral(static_cast<uint64_t>(std::get<int64_t>(Op.literal)),
                           32)
              : value(Node.arguments[1], Op.pc,
                      Op.registers[Arithmetic->two_address ? 1 : 2], "I");
      Expr = integerExpression(*Arithmetic, Left, Right);
    } else if (Node.kind == "long-binary") {
      const auto Binary = longBinary(Op.opcode);
      if (!Binary || Node.type != "J")
        fail("nested allocation lost its long binary operation receipt");
      const auto Prefix = Binary->can_throw ? predecessors(Op) : EffectPrefix{};
      const auto Left = value(Node.arguments[0], Op.pc,
                              Op.registers[Binary->two_address ? 0 : 1], "J");
      const auto Right =
          value(Node.arguments[1], Op.pc,
                Op.registers[Binary->two_address ? 1 : 2], Binary->right_type);
      Expr = "((" + Left + ") " + Binary->symbol + " (" + Right + "))";
      if (Binary->can_throw) {
        Expr = sequence(Prefix, Expr, "0");
        Effects.push_back(Op.pc);
      }
    } else if (Node.kind == "floating") {
      const auto Floating = floatingArithmetic(Op.opcode);
      if (!Floating || Floating->type != Node.type)
        fail("nested allocation lost its floating operation receipt");
      const auto Left =
          value(Node.arguments[0], Op.pc,
                Op.registers[Floating->two_address ? 0 : 1], Floating->type);
      const auto Right =
          value(Node.arguments[1], Op.pc,
                Op.registers[Floating->two_address ? 1 : 2], Floating->type);
      Expr = "((" + Left + ") " + Floating->symbol + " (" + Right + "))";
    } else if (Node.kind == "unary") {
      const auto Unary = scalarUnary(Op.opcode);
      if (!Unary || Unary->first != Node.type)
        fail("nested allocation lost its scalar unary operation receipt");
      Expr = Unary->second + "(" +
             value(Node.arguments[0], Op.pc, Op.registers[1], Unary->first) +
             ")";
    } else if (Node.kind == "conversion") {
      const auto Conversion = scalarConversion(Op.opcode);
      if (!Conversion || Conversion->second != Node.type)
        fail("nested allocation lost its scalar conversion receipt");
      Expr =
          "((" + Primitives.at(Conversion->second) + ") (" +
          value(Node.arguments[0], Op.pc, Op.registers[1], Conversion->first) +
          "))";
    } else if (Node.kind == "cast" || Node.kind == "move") {
      Expr = value(Node.arguments[0], Op.pc,
                   Op.registers[Node.kind == "cast" ? 0 : 1],
                   "Ljava/lang/Object;");
      if (Node.kind == "cast") {
        Expr = "((" + sourceType(Node.type) + ") " + Expr + ")";
        Effects.push_back(Op.pc);
      }
    } else
      fail("nested allocation expression has no value producer");
    Active.erase(Id);
    Done.insert(Id);
    return Node.type == "V"
               ? Expr
               : "(" + Name + " = " + registerStorage(StorageType, Expr) + ")";
  };
  Lines Out(budget);
  Out.append("o" + std::to_string(code[by_pc.at(Plan.start)].registers[0]) +
             " = " + evaluate(0) + ";");
  // Every value is immutable in this expression graph. Restore original DEX
  // registers only after the complete constructor succeeds. The closed region
  // has no alternative entry; retained handlers use proven unchanged inputs or
  // their own definitions and cannot observe the deferred register copies.
  State Scratch = states.at(Plan.end);
  for (unsigned Register : Plan.written) {
    const auto Id = Plan.live.at(Register);
    const auto &Node = Plan.values[Id];
    if (Node.kind == "bits-copy" &&
        states.at(Plan.end).at(Register) == Strings{"zero"})
      Out.append(write(Scratch, Register, "zero", "0"));
    else if (Node.kind == "constant") {
      const auto Literal = std::get<int64_t>(code[Node.instruction].literal);
      Out.append(write(Scratch, Register, Node.type,
                       hexLiteral(static_cast<uint64_t>(Literal),
                                  Node.type == "bits64" ? 64 : 32)));
    } else if (Node.type == "F" || Node.type == "D")
      // evaluate() already stored the original floating result's raw bits.
      // Restore that carrier without interpreting an integer as a float or
      // applying a second float-to-bits conversion.
      Out.append(write(Scratch, Register,
                       Node.type == "D" ? "bits64" : "bits32", evaluate(Id)));
    else if (Node.type == "Z")
      Out.append(
          write(Scratch, Register, Node.type, "(" + evaluate(Id) + " != 0)"));
    else
      Out.append(write(Scratch, Register, Node.type, evaluate(Id)));
  }
  if (Effects != Plan.effects)
    fail("nested allocation expression changes original effect order");
  return Out.get();
}

std::pair<State, std::vector<std::string>>
Body::operation(const Instruction &Op, const State &Incoming, bool Strict,
                std::set<unsigned> *Written) {
  budget.tick();
  llvm::SaveAndRestore<std::optional<uint32_t>> Active(active_instruction,
                                                       Op.pc);
  State S = Incoming;
  // Observe only committed writes to this operation's state. Speculative
  // allocation/constructor scratch states and nested operation calls cannot
  // invent writes in the caller's object-identity transfer.
  llvm::SaveAndRestore<RegisterWriteObservation> Observe(
      register_write_observation, RegisterWriteObservation{&S, Written});
  Lines Out(budget);
  const auto &Name = Op.opcode;
  const auto &Regs = Op.registers;
  const auto Base = baseOpcode(Name);
  auto arity = [&](size_t N) {
    if (Regs.size() != N)
      fail("wrong register count for " + Name);
  };
  auto get = [&](unsigned R, const std::string &T) {
    return read(Incoming, R, T, Strict);
  };
  auto put = [&](unsigned R, const std::string &T, const std::string &V) {
    Out.append(write(S, R, T, V));
  };
  if (Name == "nop") {
    arity(0);
    if (!emptyNop(Op))
      fail("nop has unexpected control, payload or decoded operands");
  } else if (Name == "monitor-enter" || Name == "monitor-exit") {
    arity(1);
    if (!monitors.contains(by_pc.at(Op.pc)) &&
        !monitor_release_owners.contains(by_pc.at(Op.pc)))
      fail("monitor operation has no verified structured region");
    get(Regs[0], "Ljava/lang/Object;");
    // The verified source synchronized region owns acquisition and all
    // releases. They must never be emitted as independent empty statements.
  } else if (Base == "move" || Base == "move-object" || Base == "move-wide") {
    arity(2);
    const auto &Kinds = Incoming[Regs[1]];
    if (Strict && Kinds.empty())
      fail("move reads an undefined carrier");
    if (Base == "move-object") {
      if (Strict && !std::all_of(Kinds.begin(), Kinds.end(), [](const auto &K) {
            return isReference(K) || K == "zero" || K == "null" ||
                   starts(K, "new:");
          }))
        fail("move-object reads an undefined or scalar carrier");
      put(Regs[0], "Ljava/lang/Object;", "o" + std::to_string(Regs[1]));
    } else if (Base == "move-wide") {
      if (Strict && (!std::all_of(Kinds.begin(), Kinds.end(),
                                  [](const auto &K) { return isWide(K); }) ||
                     Regs[1] + 1 >= Incoming.size() ||
                     Incoming[Regs[1] + 1] != Strings{"wide-high"}))
        fail("move-wide reads an undefined or broken wide carrier");
      put(Regs[0], "bits64", "w" + std::to_string(Regs[1]));
    } else {
      static const Strings Allowed{"I", "B", "C",      "S",
                                   "Z", "F", "bits32", "zero"};
      if (Strict &&
          !std::all_of(Kinds.begin(), Kinds.end(),
                       [&](const auto &K) { return Allowed.contains(K); }))
        fail("move reads an undefined or incompatible carrier");
      put(Regs[0], "bits32", "v" + std::to_string(Regs[1]));
      if (Kinds.contains("zero"))
        Out.append("o" + std::to_string(Regs[0]) + " = o" +
                   std::to_string(Regs[1]) + ";");
    }
    S[Regs[0]] = Kinds;
  } else if (starts(Name, "move-result")) {
    arity(1);
    const auto &T = pending_results.at(Op.pc);
    put(Regs[0], T, resultRead(T));
  } else if (Name == "move-exception") {
    arity(1);
    Strings Catches;
    for (const auto &R : method.tries)
      for (const auto &H : R.handlers)
        if (H.target == Op.pc)
          Catches.insert(H.type.value_or("Ljava/lang/Throwable;"));
    if (Catches.empty())
      fail("move-exception is outside a handler entry");
    put(Regs[0], "Ljava/lang/Throwable;", "caught");
    S[Regs[0]] = std::move(Catches);
  } else if (Name == "const-string" || Name == "const-string/jumbo") {
    arity(1);
    const auto *V = std::get_if<std::string>(&Op.literal);
    if (!V)
      fail("const-string lacks its decoded string");
    put(Regs[0], "Ljava/lang/String;", javaString(*V));
  } else if (Name == "const-class") {
    arity(1);
    put(Regs[0], "Ljava/lang/Class;", sourceType(referenceType(Op)) + ".class");
  } else if (starts(Name, "const")) {
    const auto Constant = integerConstant(Op, method.registers);
    if (!Constant)
      fail("constant has an unsupported load name, operands or frame");
    put(Constant->destination, Constant->type,
        hexLiteral(static_cast<uint64_t>(Constant->bits),
                   Constant->type == "bits64" ? 64 : 32));
  } else if (starts(Base, "return")) {
    const auto &T = method.reference.returns;
    if (Name == "return-void") {
      arity(0);
      if (T != "V")
        fail("void return disagrees with method signature");
      Out.append(method.reference.name == "<clinit>" ? "break dispatch;"
                                                     : "return;");
    } else {
      arity(1);
      std::string Expected = isReference(T)      ? "return-object"
                             : wordWidth(T) == 2 ? "return-wide"
                                                 : "return";
      if (T == "V" || Name != Expected)
        fail("return carrier disagrees with method signature");
      // Each source receipt proves this exact result register already has
      // the nominal source type; reading it adds no stronger runtime check.
      auto Value = get(Regs[0], sourceBridgeReference(method.reference,
                                                      generics, inheritance)
                                    .returns);
      if (const auto *Source =
              sourceMethodSignature(method.reference, generics);
          isReference(T) && Source) {
        auto Declared =
            type_names.render(*Source, *Source->result,
                              JavaScope{ownerClass(), &method.reference});
        // The signature validator proved this cast has the same erasure as
        // the existing register read. It adds no stronger runtime check.
        Value = "((" + Declared + ") " + Value + ")";
      }
      Out.append("return " + Value + ";");
    }
  } else if (Name == "array-length") {
    arity(2);
    if (Op.target || !Op.targets.empty() || !Op.keys.empty() ||
        !Op.data.empty() || Op.element_width ||
        !std::holds_alternative<std::monostate>(Op.reference) ||
        !std::holds_alternative<std::monostate>(Op.literal))
      fail("array-length has unexpected control, payload or decoded operands");
    std::string T = arrayType(Incoming, Regs[1], Strict);
    put(Regs[0], "I", get(Regs[1], T) + ".length");
  } else if (Name == "new-array") {
    arity(2);
    if (Op.target || !Op.targets.empty() || !Op.keys.empty() ||
        !Op.data.empty() || Op.element_width ||
        !std::holds_alternative<std::monostate>(Op.literal))
      fail("new-array has unexpected control, payload or literal operands");
    const auto &T = referenceType(Op);
    put(Regs[0], T, arrayCreation(T, get(Regs[1], "I")));
  } else if (Name == "filled-new-array" || Name == "filled-new-array/range") {
    const auto &T = referenceType(Op);
    if (!starts(T, "[") || T.substr(1) == "J" || T.substr(1) == "D")
      fail("filled-new-array requires single-word elements");
    std::vector<std::string> Values;
    for (unsigned R : Regs)
      Values.push_back(get(R, T.substr(1)));
    Out.append("resultObject = new " + sourceType(T) + " {" +
               join(Values, ", ") + "};");
  } else if (Name == "new-instance") {
    arity(1);
    const auto &T = referenceType(Op);
    if (!starts(T, "L"))
      fail("new-instance requires a class descriptor");
    size_t Index = by_pc.at(Op.pc);
    if (Index + 1 >= code.size())
      fail("new-instance has no initializing invocation");
    if (forked_allocation_owners.contains(Op.pc) ||
        planAbruptAllocationArgument(Index, T)) {
      write(S, Regs[0], "new:" + std::to_string(Op.pc) + ":" + T, "null");
      return {std::move(S), Out.get()};
    }
    // The inheritance planner has already proved the complete six-operation
    // terminal body, its full contract, and the unchanged conversion bridge.
    // Defer the field/read/call/result together into new T(...), so allocation
    // and class initialization still precede every argument effect.
    if (auto Alias = inheritance.source_method_names.find(method.reference);
        Alias != inheritance.source_method_names.end() &&
        Alias->second.nonreturning_message_integer_bridge) {
      const auto &Read = code.at(1), &Result = code.at(3), &Init = code.at(4);
      write(S, Regs[0], "new:" + std::to_string(Op.pc) + ":" + T, "null");
      constructors[Init.pc] = Op.pc;
      allocation_reads[Init.pc].emplace(Result.registers.at(0), Read.pc);
      for (size_t I = 1; I < 4; ++I)
        deferred_allocation_reads.insert(code[I].pc);
      return {std::move(S), Out.get()};
    }
    if (constructor_choices.contains(Op.pc) || planConstructorChoice(Index)) {
      const auto &Plan = constructor_choices.at(Op.pc);
      // Class initialization cannot change a scalar or reference already in
      // the tested register. Each arm evaluates its original argument program
      // inside new and keeps the original branch's updated register values.
      if (emitting) {
        const auto test = condition(code[Plan.branch], Incoming, true);
        auto construct = [&](size_t begin, size_t at) {
          const auto &call = code[at];
          const auto &ref = std::get<MethodRef>(call.reference);
          const auto &frame = states.at(call.pc);
          std::vector<std::string> args;
          size_t word = 1;
          for (const auto &type : ref.parameters) {
            args.push_back(read(frame, call.registers.at(word), type, true));
            word += wordWidth(type);
          }
          if (begin < at) {
            Lines program(budget, true);
            program.append("switch (0) { default -> {");
            for (size_t i = begin; i != at; ++i)
              for (const auto &line : operation(code[i], states.at(code[i].pc), true).second)
                program.append(line);
            program.append("yield " + args.front() + "; } }");
            args.front() = program.text();
            minimum_java_release = std::max(minimum_java_release, 14u);
          }
          return "new " + sourceType(T) + "(" + join(args, ", ") + ")";
        };
        auto yes = construct(Plan.first + 2, Plan.second);
        auto no = construct(Plan.branch + 1, Plan.first);
        State scratch = Incoming;
        Out.append(write(scratch, Regs[0], T,
                         "(" + test + " ? " + yes + " : " + no + ")"));
      }
      write(S, Regs[0], "new:" + std::to_string(Op.pc) + ":" + T, "null");
      return {std::move(S), Out.get()};
    }
    if (!allocation_region_owners.contains(Op.pc))
      planForwardAllocation(Index);
    if (!allocation_region_owners.contains(Op.pc) &&
        planConditionalAllocation(Index)) {
      write(S, Regs[0], "new:" + std::to_string(Op.pc) + ":" + T, "null");
      return {std::move(S), Out.get()};
    }
    if (!allocation_region_owners.contains(Op.pc) && planForkedAllocation(Index)) {
      write(S, Regs[0], "new:" + std::to_string(Op.pc) + ":" + T, "null");
      return {std::move(S), Out.get()};
    }
    if (!allocation_region_owners.contains(Op.pc)) {
      // Find the complete nested expression even when D8 prefixes it with
      // reference reads/calls and reuses their registers. This scan only
      // selects the existing value-graph proof; it grants no new semantics.
      // Nonadjacent protected preparation needs that same proof even when
      // it contains only constants/copies: its handler checks distinguish
      // committed inputs from deferred register writes.
      bool DeferredPrefix = false, ReferenceRead = false;
      std::set<unsigned> FieldResults, FieldReceivers;
      for (size_t Probe = Index + 1; Probe < code.size(); ++Probe) {
        budget.tick();
        const auto &Candidate = code[Probe];
        const auto *Call = std::get_if<MethodRef>(&Candidate.reference);
        const auto Kind = baseOpcode(Candidate.opcode);
        const bool FieldRead = allocationFieldRead(Kind);
        const bool StringConstant = Candidate.opcode == "const-string" ||
                                    Candidate.opcode == "const-string/jumbo";
        const bool ChainedField =
            FieldRead &&
            std::any_of(Candidate.registers.begin(), Candidate.registers.end(),
                        [&](unsigned R) { return FieldResults.contains(R); });
        const bool ScalarFieldCopy =
            (Kind == "move" || Kind == "move-wide") &&
            std::any_of(Candidate.registers.begin(), Candidate.registers.end(),
                        [&](unsigned R) { return FieldResults.contains(R); });
        // A later copy may reuse the physical receiver word after its
        // field read. The immutable graph retains the original receiver
        // value across scalar, wide and reference register transfers; the
        // linear deferred-read path cannot prove that lifetime itself.
        const bool ReceiverFieldCopy =
            (Kind == "move" || Kind == "move-wide" || Kind == "move-object") &&
            Candidate.registers.size() == 2 &&
            (FieldReceivers.contains(Candidate.registers[0]) ||
             (Kind == "move-wide" &&
              FieldReceivers.contains(Candidate.registers[0] + 1)));
        const bool ProtectedPreparation = Probe == Index + 1 &&
                                          !handlers(Op.pc).empty() &&
                                          (!Call || Call->name != "<init>");
        bool ReusedCallInput = false;
        if (Kind == "invoke-static" && Call && isReference(Call->returns) &&
            Probe + 1 < code.size()) {
          const auto &Result = code[Probe + 1];
          ReusedCallInput =
              Result.opcode == "move-result-object" &&
              Result.registers.size() == 1 &&
              std::find(Candidate.registers.begin(), Candidate.registers.end(),
                        Result.registers[0]) != Candidate.registers.end();
        }
        bool RetainedValue = false;
        std::optional<unsigned> ProducedRegister;
        size_t FirstUse = Probe + 1;
        if (Kind == "invoke-static" && Call && isReference(Call->returns) &&
            Probe + 1 < code.size() &&
            code[Probe + 1].opcode == "move-result-object" &&
            code[Probe + 1].registers.size() == 1) {
          ProducedRegister = code[Probe + 1].registers[0];
          FirstUse = Probe + 2;
        } else if ((FieldRead || StringConstant) &&
                   !Candidate.registers.empty()) {
          ProducedRegister = Candidate.registers[0];
        }
        if (ProducedRegister) {
          for (size_t End = FirstUse; End < code.size() && End - Index <= 256;
               ++End) {
            budget.tick();
            const auto &Init = code[End];
            const auto *Target = std::get_if<MethodRef>(&Init.reference);
            if (Target && Target->name == "<init>") {
              RetainedValue =
                  !Init.registers.empty() &&
                  std::count(Init.registers.begin() + 1, Init.registers.end(),
                             *ProducedRegister) != 1;
              break;
            }
            if (branchTarget(Init) || !Init.targets.empty() ||
                Init.opcode == "new-instance" ||
                anyPrefix(Init.opcode, {"return", "throw"}))
              break;
          }
        }
        // Shared reads, retained calls and uninitialized receiver copies
        // need the existing immutable value graph. Its producer cache retains
        // each original read once and keeps allocation/class initialization
        // before argument effects. Later string constants can occupy earlier
        // argument slots; the same graph retains their original predecessors,
        // including other strings loaded in a different argument order.
        if (RetainedValue || ProtectedPreparation || Kind == "invoke-virtual" ||
            Kind == "invoke-interface" || Kind == "invoke-super" ||
            (Kind == "invoke-direct" && Call && Call->name != "<init>") ||
            Candidate.opcode == "new-instance" || ChainedField ||
            // A scalar field read can replace its own reference register.
            // Snapshot its input in the value graph before that replacement.
            (starts(Kind, "iget") && Kind != "iget-object" &&
             Candidate.registers.size() == 2 &&
             Candidate.registers[0] == Candidate.registers[1]) ||
            ScalarFieldCopy || ReceiverFieldCopy ||
            // The simple call path can fold only a cast of its own adjacent
            // result. A cast of another word has a separate exception point
            // and belongs to the ordered allocation value graph.
            (Candidate.opcode == "check-cast" &&
             (ReferenceRead || code[Probe - 1].opcode != "move-result-object" ||
              code[Probe - 1].registers != Candidate.registers)) ||
            (Kind == "move-object" &&
             (ReferenceRead || (Candidate.registers.size() == 2 &&
                                Candidate.registers[1] == Regs[0]))) ||
            (StringConstant && DeferredPrefix) ||
            Candidate.opcode == "const-class" ||
            scalarConversion(Candidate.opcode) ||
            scalarUnary(Candidate.opcode) ||
            floatingArithmetic(Candidate.opcode) ||
            longBinary(Candidate.opcode) ||
            (integerPreparation(Candidate) &&
             std::any_of(
                 Candidate.registers.begin(), Candidate.registers.end(),
                 [&](unsigned R) { return FieldResults.contains(R); })) ||
            Candidate.opcode == "array-length" ||
            arrayLoad(Candidate.opcode) ||
            Candidate.opcode == "new-array" || Kind == "filled-new-array" ||
            Candidate.opcode == "fill-array-data" || starts(Kind, "aput") ||
            (Kind == "invoke-static" && Call &&
             ((!isReference(Call->returns) &&
               Primitives.contains(Call->returns)) ||
              (isReference(Call->returns) &&
               (DeferredPrefix || ReusedCallInput || Probe + 1 == code.size() ||
                !starts(code[Probe + 1].opcode, "move-result")))))) {
          planNestedAllocation(Index);
          break;
        }
        if (FieldRead && !Candidate.registers.empty()) {
          FieldResults.insert(Candidate.registers[0]);
          if (starts(Kind, "iget") && Candidate.registers.size() == 2)
            FieldReceivers.insert(Candidate.registers[1]);
          if (Kind == "iget-wide" || Kind == "sget-wide")
            FieldResults.insert(Candidate.registers[0] + 1);
        }
        ReferenceRead |=
            Kind == "iget-object" || Kind == "sget-object" || StringConstant;
        DeferredPrefix |= FieldRead || StringConstant ||
                          Kind == "invoke-static" || Kind == "invoke-virtual" ||
                          Kind == "invoke-interface";
        if (Probe - Index >= 256 ||
            (!FieldRead && !pureAllocationPreparation(Candidate) &&
             Kind != "invoke-static" && Kind != "invoke-virtual" &&
             Kind != "invoke-interface" && Kind != "move-result" &&
             Kind != "move-result-object" && Kind != "check-cast" &&
             !starts(Candidate.opcode, "const")))
          break;
      }
    }
    if (auto Region = allocation_region_owners.find(Op.pc);
        Region != allocation_region_owners.end()) {
      const auto &Plan = allocation_regions.at(Region->second);
      write(S, Regs[0], "new:" + std::to_string(Op.pc) + ":" + T, "null");
      if (emitting && Op.pc == Plan.start)
        for (size_t I = 0; I < Plan.values.size(); ++I) {
          const auto &Value = Plan.values[I];
          if (Value.kind != "input")
            continue;
          const auto Name = allocationLocal(Plan.start, I);
          const auto Register = std::to_string(Value.register_index);
          Out.extend({Name + "Object = o" + Register + ";",
                      Name + "Word = w" + Register + ";",
                      Name + "Bits = v" + Register + ";"});
        }
      return {std::move(S), Out.get()};
    }

    size_t Initialization = Index + 1;
    std::map<unsigned, uint32_t> Deferred;
    std::set<unsigned> DeferredWords;
    std::set<unsigned> DeferredReceivers;
    std::vector<uint32_t> Reads, CallContinuations;
    for (; Initialization < code.size(); ++Initialization) {
      const auto &Prepare = code[Initialization];
      if (const auto *R = std::get_if<MethodRef>(&Prepare.reference);
          R && R->name == "<init>")
        break;
      budget.tick();
      const auto Kind = baseOpcode(Prepare.opcode);
      if (Kind == "invoke-static") {
        const auto *Call = std::get_if<MethodRef>(&Prepare.reference);
        const auto *Declaration = validateInvocation(Prepare);
        if (!Call || !Declaration || !isReference(Call->returns) ||
            !expressionMemberAccessible(*Call, *Declaration) ||
            has(Declaration->access, "bridge") ||
            inheritance.void_unit_targets.contains(Declaration->reference) ||
            changesMethodContract(Declaration->reference, inheritance) ||
            nonlinear_entries.contains(Prepare.pc) ||
            !handlers(Prepare.pc).empty() || Initialization + 1 >= code.size())
          fail("allocation argument call needs a known unchanged static "
               "reference result");
        const auto &Result = code[Initialization + 1];
        if (Result.opcode != "move-result-object" ||
            Result.registers.size() != 1 ||
            nonlinear_entries.contains(Result.pc) ||
            !handlers(Result.pc).empty())
          fail("allocation argument call needs its adjacent result register");
        const auto Register = Result.registers[0];
        if (Register == Regs[0] || DeferredWords.contains(Register) ||
            DeferredReceivers.contains(Register) ||
            std::find(Prepare.registers.begin(), Prepare.registers.end(),
                      Register) != Prepare.registers.end())
          fail("allocation argument result cannot alias its live inputs");
        for (unsigned Input : Prepare.registers) {
          if (Input == Regs[0] || DeferredWords.contains(Input))
            fail("allocation argument call cannot consume an uninitialized or "
                 "deferred input");
          DeferredReceivers.insert(Input);
        }
        Deferred.emplace(Register, Prepare.pc);
        DeferredWords.insert(Register);
        Reads.push_back(Prepare.pc);
        CallContinuations.push_back(Result.pc);
        ++Initialization;
        if (Initialization + 1 < code.size() &&
            code[Initialization + 1].opcode == "check-cast") {
          const auto &Cast = code[++Initialization];
          if (Cast.registers != Result.registers ||
              !std::holds_alternative<std::string>(Cast.reference) ||
              !isReference(std::get<std::string>(Cast.reference)) ||
              nonlinear_entries.contains(Cast.pc) || !handlers(Cast.pc).empty())
            fail("allocation argument cast must check that same result");
          CallContinuations.push_back(Cast.pc);
        }
        continue;
      }
      const bool StaticRead = starts(Prepare.opcode, "sget");
      const bool InstanceRead = starts(Prepare.opcode, "iget");
      const bool FieldRead = StaticRead || InstanceRead;
      const bool StringConstant = Prepare.opcode == "const-string" ||
                                  Prepare.opcode == "const-string/jumbo";
      const bool Wide =
          Kind == "move-wide" || starts(Prepare.opcode, "const-wide") ||
          Prepare.opcode == "sget-wide" || Prepare.opcode == "iget-wide";
      // Total integer arithmetic only touches local words. It cannot resolve,
      // initialize or allocate a class or throw, so it can precede the emitted
      // new without moving an observable effect. Deferred reads, uninitialized
      // receivers, external entries and handlers retain their existing guards.
      if ((!pureAllocationPreparation(Prepare) && !FieldRead &&
           !StringConstant) ||
          nonlinear_entries.contains(Prepare.pc) ||
          !handlers(Prepare.pc).empty())
        fail("allocation requires an adjacent initializer or pure register "
             "preparation");
      for (unsigned Register : Prepare.registers)
        if (Register == Regs[0] || (Wide && Register + 1 == Regs[0]))
          fail("allocation preparation cannot alias or overwrite its "
               "uninitialized receiver");
        else if (DeferredWords.contains(Register) ||
                 (Wide && DeferredWords.contains(Register + 1)))
          fail("allocation field-read argument cannot be overwritten or "
               "aliased");
      if (!Prepare.registers.empty() &&
          (DeferredReceivers.contains(Prepare.registers[0]) ||
           (Wide && DeferredReceivers.contains(Prepare.registers[0] + 1))))
        fail("allocation field-read receiver cannot be overwritten");
      if (FieldRead || StringConstant) {
        if (Prepare.registers.size() != (InstanceRead ? 2u : 1u) ||
            (FieldRead &&
             !std::holds_alternative<FieldRef>(Prepare.reference)) ||
            (StringConstant && !std::get_if<std::string>(&Prepare.literal)))
          fail("allocation field-read argument has an invalid shape");
        const unsigned Register = Prepare.registers[0];
        if (InstanceRead) {
          const unsigned Receiver = Prepare.registers[1];
          if ((Register == Receiver && Prepare.opcode != "iget-object") ||
              (Wide && Register + 1 == Receiver))
            fail("allocation field-read receiver cannot be overwritten");
          DeferredReceivers.insert(Receiver);
        }
        Deferred.emplace(Register, Prepare.pc);
        DeferredWords.insert(Register);
        if (Wide)
          DeferredWords.insert(Register + 1);
        Reads.push_back(Prepare.pc);
      }
    }
    if (Initialization == code.size())
      fail("allocation requires an adjacent initializer or pure register "
           "preparation");
    const auto &Next = code[Initialization];
    const auto *R = std::get_if<MethodRef>(&Next.reference);
    if ((Next.opcode != "invoke-direct" &&
         Next.opcode != "invoke-direct/range") ||
        !R || R->name != "<init>" || Next.registers.empty() ||
        Next.registers[0] != Regs[0] || R->owner != T ||
        !sameHandlers(handlers(Op.pc), handlers(Next.pc)) ||
        nonlinear_entries.contains(Next.pc) ||
        (Initialization != Index + 1 && !handlers(Op.pc).empty()))
      fail("allocation requires an adjacent initializer in the same exception "
           "region");
    write(S, Regs[0], "new:" + std::to_string(Op.pc) + ":" + T, "null");
    constructors[Next.pc] = Op.pc;
    if (!Deferred.empty()) {
      std::vector<uint32_t> ArgumentReads;
      size_t Cursor = 1;
      for (const auto &Type : R->parameters) {
        if (Cursor >= Next.registers.size())
          fail("allocation invocation is missing argument words");
        if (auto I = Deferred.find(Next.registers[Cursor]); I != Deferred.end())
          ArgumentReads.push_back(I->second);
        Cursor += wordWidth(Type);
      }
      if (ArgumentReads != Reads)
        fail("allocation field-read arguments must be consumed exactly once in "
             "their original order");
      allocation_reads[Next.pc] = std::move(Deferred);
      deferred_allocation_reads.insert(Reads.begin(), Reads.end());
      deferred_allocation_reads.insert(CallContinuations.begin(),
                                       CallContinuations.end());
    }
  } else if (Name == "fill-array-data") {
    arity(1);
    const auto T = arrayType(Incoming, Regs[0], Strict);
    for (const auto &Line : arrayPayload(Op, T, get(Regs[0], T)))
      Out.append(Line);
  } else if (anyPrefix(Name, {"aget", "aput"})) {
    arity(3);
    if (Op.target || !Op.targets.empty() || !Op.keys.empty() ||
        !Op.data.empty() || Op.element_width ||
        !std::holds_alternative<std::monostate>(Op.literal) ||
        !std::holds_alternative<std::monostate>(Op.reference))
      fail("array access has unexpected control, payload or decoded operands");
    if (!Strict && starts(Name, "aget")) {
      // A nullable array can reach a join before its nonnull producer. Never
      // seed the monotone fixed point with a guessed primitive component:
      // null/undefined paths provide no normal-load result type. Preserve
      // every actual array component, then let the converged strict pass own
      // the unique array, opcode, index and register-frame checks.
      Strings Elements;
      for (const auto &Kind : Incoming.at(Regs[1]))
        if (starts(Kind, "["))
          Elements.insert(Kind.substr(1));
      put(Regs[0],
          Name == "aget-wide"     ? "bits64"
          : Name == "aget-object" ? "Ljava/lang/Object;"
                                  : "bits32",
          Name == "aget-object" ? "null" : "0");
      S[Regs[0]] = std::move(Elements);
      return {std::move(S), Out.get()};
    }
    const auto T = arrayType(Incoming, Regs[1], Strict);
    const auto Element = T.substr(1);
    std::string Suffix;
    if (isReference(Element))
      Suffix = "-object";
    else if (Element == "J" || Element == "D")
      Suffix = "-wide";
    else if (Element == "Z")
      Suffix = "-boolean";
    else if (Element == "B")
      Suffix = "-byte";
    else if (Element == "C")
      Suffix = "-char";
    else if (Element == "S")
      Suffix = "-short";
    if (Strict && Name != Name.substr(0, 4) + Suffix)
      fail("array opcode disagrees with its element width/type");
    if (starts(Name, "aget")) {
      put(Regs[0], Element, get(Regs[1], T) + "[" + get(Regs[2], "I") + "]");
      if (Name == "aget-object" && T == "[Ljava/lang/Object;") {
        // The Java expression uses the common array view. Keep each possible
        // actual element for later checked reads, including nested arrays.
        Strings elements;
        for (const auto &kind : Incoming[Regs[1]])
          if (kind.starts_with("[L") || kind.starts_with("[["))
            elements.insert(kind.substr(1));
        if (!elements.empty())
          S[Regs[0]] = std::move(elements);
      }
    } else {
      const auto [Array, Value] = arrayStoreTypes(T);
      Out.append(get(Regs[1], Array) + "[" + get(Regs[2], "I") +
                 "] = " + get(Regs[0], Value) + ";");
    }
  } else if (Name == "check-cast") {
    arity(1);
    get(Regs[0], "Ljava/lang/Object;");
    const auto &T = referenceType(Op);
    put(Regs[0], T,
        "((" + sourceType(T) + ") o" + std::to_string(Regs[0]) + ")");
  } else if (Name == "instance-of") {
    arity(2);
    const auto &type = referenceType(Op);
    const auto value = get(Regs[1], "Ljava/lang/Object;");
    auto component = type.find_first_not_of('[');
    const auto local = local_projection.by_class.find(type.substr(component));
    // An otherwise nongeneric local class can capture its enclosing
    // method's type variables. Its class literal still denotes precisely
    // the reified JVM type, including for an array of that local class.
    if (local != local_projection.by_class.end() && local->second->enclosing_method &&
        (generics.methods.contains(*local->second->enclosing_method) ||
         generics.classes.contains(local->second->enclosing_method->owner)))
      put(Regs[0], "Z", sourceType(type) + ".class.isInstance(" + value + ")");
    else
      put(Regs[0], "Z", value + " instanceof " + sourceType(type));
  } else if (anyPrefix(Name, {"iget", "iput", "sget", "sput"})) {
    const auto *R = std::get_if<FieldRef>(&Op.reference);
    if (!R)
      fail("field instruction has no field reference");
    if (Op.target || !Op.targets.empty() || !Op.keys.empty() ||
        !Op.data.empty() || Op.element_width ||
        !std::holds_alternative<std::monostate>(Op.literal))
      fail("field instruction has malformed metadata");
    bool Static = starts(Name, "s");
    arity(Static ? 1 : 2);
    std::string Suffix;
    if (isReference(R->type))
      Suffix = "-object";
    else if (wordWidth(R->type) == 2)
      Suffix = "-wide";
    else if (R->type == "Z")
      Suffix = "-boolean";
    else if (R->type == "B")
      Suffix = "-byte";
    else if (R->type == "C")
      Suffix = "-char";
    else if (R->type == "S")
      Suffix = "-short";
    if (Name.substr(4) != Suffix)
      fail("field opcode disagrees with its type");
    const auto *Declaration = member(*R, classes, budget);
    if (Declaration && Static != Declaration->access.contains("static"))
      fail("field opcode disagrees with the declared static/instance kind");
    if (Declaration && has(Declaration->access, "private") &&
        packageOf(method.reference.owner) != packageOf(Declaration->reference.owner) &&
        (!starts(Name, "iget") || !inheritance.cross_package_private_reads.contains(*R)))
      fail("cross-package private field access lacks a verified rebuild binding");
    // Distinct top-level classes cannot share Java private field access.
    // Nested/local declarations retain their existing lexical scope proof.
    if (Declaration && has(Declaration->access, "private") &&
        Declaration->reference.owner != method.reference.owner &&
        !ownerClass().enclosing && !ownerClass().enclosing_method) {
      const auto &Owner = classes.at(Declaration->reference.owner);
      if (!Owner.enclosing && !Owner.enclosing_method)
        fail("private field belongs to a different top-level class");
    }
    const bool Read = Name.substr(1, 3) == "get";
    auto Address =
        Static && Read
            ? staticFieldRead(*R)
            : (Static ? staticOwner(R->owner, true) : fieldReceiver(*R, Op, get(Regs[1], R->owner))) +
                  "." + sourceFieldName(*R, classes, inheritance, budget);
    if (Read) {
      put(Regs[0], R->type, Address);
    } else {
      if (Declaration && Declaration->access.contains("final")) {
        if (!final_writes.contains(*R) || R->owner != method.reference.owner ||
            (!Static && Incoming[Regs[1]] != Strings{"self:" + R->owner}))
          fail("final field assignment requires a structured initialization "
               "proof: " +
               R->owner + "->" + R->name);
        Address = (Static ? "" : "this.") +
                  sourceFieldName(*R, classes, inheritance, budget);
        if (Static) {
          Strings Locals{"result32", "result64", "resultObject",
                         "caught",   "pc",       "failure"};
          for (unsigned Reg = 0; Reg < method.registers; ++Reg)
            for (const char *Prefix : {"v", "w", "o"})
              Locals.insert(Prefix + std::to_string(Reg));
          if (Locals.contains(R->name))
            fail("static final initializer field shadows a generated local");
        }
      }
      std::string valueType = R->type;
      if (!Static && Declaration && has(Declaration->access, "protected") &&
          ownerClass().superclass == Declaration->reference.owner &&
          (R->owner == ownerClass().name ||
           packageOf(Declaration->reference.owner) != packageOf(ownerClass().name))) {
        // Java resolves an inherited field at its parameterized superclass.
        // A concrete source cast is safe only if every incoming DEX word
        // already has that type (or is null).
        const auto field = generics.fields.find(Declaration->reference);
        if (field != generics.fields.end() && field->second.field_type) {
          const auto bound = closedSuperclassMemberType(ownerClass().name,
              Declaration->reference.owner, field->second, *field->second.field_type);
          const auto &words = Incoming[Regs[0]];
          if (bound && !words.empty() && std::all_of(words.begin(), words.end(),
                  [&](const auto &word) { return compatible(word, *bound); }))
            valueType = *bound;
        }
      }
      auto Value = get(Regs[0], valueType);
      if (const auto I = generics.fields.find(*R);
          I != generics.fields.end() && I->second.field_type)
        Value =
            lexicalMemberArgument(R->owner, I->second, *I->second.field_type,
                                  R->type, std::move(Value));
      if (Declaration && Declaration->access.contains("final"))
        Value = finalFieldSourceValue(*R, std::move(Value));
      if (emitting && final_assignment_target && Declaration &&
          Declaration->access.contains("final"))
        Address = *final_assignment_target;
      Out.append(Address + " = " + Value + ";");
    }
  } else if (starts(Name, "invoke-")) {
    const auto *R = std::get_if<MethodRef>(&Op.reference);
    if (!R || (starts(R->name, "<") && R->name != "<init>"))
      fail("unmodelled constructor or dynamic invocation");
    const auto *Declaration = validateInvocation(Op);
    bool Static = starts(Name, "invoke-static");
    size_t Cursor = Static ? 0 : 1;
    if (!Static && Regs.empty())
      fail("instance invocation lacks its receiver");
    std::vector<std::string> Args;
    const auto SuperContract =
        generics.superclass_calls.find({method.reference, *R});
    const GenericSignature *InvocationContract = nullptr;
    if (starts(Name, "invoke-super") &&
        SuperContract != generics.superclass_calls.end())
      InvocationContract = &SuperContract->second;
    else if (Declaration && starts(Name, "invoke-virtual")) {
      const auto Access = generics.public_superclass_access_bridges.find(
          Declaration->reference);
      if (Access != generics.public_superclass_access_bridges.end() &&
          Access->second.regenerate_public_access)
        InvocationContract = &Access->second.contract;
    }
    // A raw nominal argument can make signed primitive overloads ambiguous
    // even when the DEX word already selects int or long. A fully closed
    // Signature supplies the type arguments at the same runtime erasure.
    const GenericSignature *staticOverload = nullptr;
    if (Static && Declaration) {
      const auto &ref = Declaration->reference;
      const auto *shape = sourceMethodSignature(ref, generics);
      if (shape && shape->parameters.size() == ref.parameters.size() &&
          shape->type_parameters.empty() &&
          std::none_of(shape->types.begin(), shape->types.end(), [](const GenericType &type) {
            return type.kind == GenericType::Kind::TypeVariable;
          })) {
        const auto &owner = classes.at(ref.owner);
        for (const auto &other : owner.methods) {
          budget.tick();
          if (other.reference.name != ref.name || !has(other.access, "static") ||
              other.reference.parameters.size() != ref.parameters.size())
            continue;
          for (size_t i = 0; i < ref.parameters.size(); ++i)
            if (ref.parameters[i] != other.reference.parameters[i] &&
                !isReference(ref.parameters[i]) && !isReference(other.reference.parameters[i]))
              staticOverload = shape;
          if (staticOverload)
            break;
        }
      }
    }
    for (const auto &T : R->parameters) {
      if (Cursor >= Regs.size())
        fail("invocation has too few argument words");
      auto Argument = get(Regs[Cursor], T);
      const auto &ArgumentType =
          InvocationContract
              ? InvocationContract->types
                    .at(InvocationContract->parameters.at(Args.size()))
                    .erasure
              : T;
      if (ArgumentType != T)
        // Both superclass calls and regenerated visibility entries expose the
        // same proven closed source contract. Validate the converged original
        // word before spelling that view: an erased input cannot acquire a
        // narrowing check from Signature metadata alone.
        Argument = get(Regs[Cursor], ArgumentType);
      if (auto Plan = allocation_reads.find(Op.pc);
          Plan != allocation_reads.end())
        if (auto I = Plan->second.find(Regs[Cursor]); I != Plan->second.end()) {
          const auto &Prepare = code[by_pc.at(I->second)];
          std::string Value, ValueType;
          std::optional<std::string> CheckedResult;
          if (Prepare.opcode == "const-string" ||
              Prepare.opcode == "const-string/jumbo") {
            Value = javaString(std::get<std::string>(Prepare.literal));
            ValueType = "Ljava/lang/String;";
          } else if (baseOpcode(Prepare.opcode) == "invoke-static") {
            const auto &Call = std::get<MethodRef>(Prepare.reference);
            std::vector<std::string> Inputs;
            size_t Word = 0;
            for (const auto &Type : Call.parameters) {
              if (Word >= Prepare.registers.size())
                fail("allocation argument call has too few words");
              Inputs.push_back(get(Prepare.registers[Word], Type));
              Word += wordWidth(Type);
            }
            if (Word != Prepare.registers.size())
              fail("allocation argument call has too many words");
            Value = "(resultObject = " + staticOwner(Call.owner) + "." +
                    invocationName(Call) + "(" +
                    invocationArguments(Call, Inputs) + "))";
            ValueType = Call.returns;
            const auto AfterResult = by_pc.at(Prepare.pc) + 2;
            if (AfterResult < code.size() &&
                code[AfterResult].opcode == "check-cast")
              CheckedResult = referenceType(code[AfterResult]);
          } else {
            const auto &Field = std::get<FieldRef>(Prepare.reference);
            if (starts(Prepare.opcode, "sget")) {
              Value = staticFieldRead(Field);
            } else {
              const auto receiver = Prepare.registers[1];
              // An iget-object can consume the old value of its destination.
              // The deferred assignment evaluates its RHS before replacing it.
              const auto &frame = Prepare.registers[0] == receiver
                                      ? states.at(Prepare.pc)
                                      : Incoming;
              Value = read(frame, receiver, Field.owner, Strict) + "." +
                      sourceFieldName(Field, classes, inheritance, budget);
            }
            ValueType = Field.type;
          }
          State Scratch = Incoming;
          auto Assignment =
              write(Scratch, Regs[Cursor], ValueType, Value, true);
          if (CheckedResult)
            Assignment =
                "((" + sourceType(*CheckedResult) + ") " + Assignment + ")";
          if (auto Alias =
                  inheritance.source_method_names.find(method.reference);
              Alias != inheritance.source_method_names.end() &&
              Alias->second.nonreturning_message_integer_bridge) {
            // Keep the original field register, virtual Object.toString call,
            // result carrier and move-result assignment in their exact order.
            const auto Call =
                "((java.lang.Object) " + Assignment + ").toString()";
            Assignment = write(Scratch, Regs[Cursor], ValueType,
                               "(resultObject = " + Call + ")", true);
          }
          Argument =
              read(Incoming, Regs[Cursor], ArgumentType, Strict, Assignment);
        }
      if (!Static && Declaration && has(Declaration->access, "final") &&
          R->owner != Declaration->reference.owner && isReference(T)) {
        // The inherited member is specialized at this symbolic receiver.
        // Keep its fixed-arity array (including null) or nominal parameter
        // when all incoming DEX words already satisfy the source type.
        const auto *shape = sourceMethodSignature(Declaration->reference, generics);
        if (shape && shape->parameters.size() == R->parameters.size()) {
          const auto bound = closedSuperclassMemberType(R->owner, Declaration->reference.owner,
                                                       *shape, shape->parameters[Args.size()]);
          const auto &words = Incoming[Regs[Cursor]];
          if (bound && !words.empty() && std::all_of(words.begin(), words.end(),
                 [&](const auto &word) { return compatible(word, *bound); })) {
            // A narrower source cast must not select a different overload.
            bool unique = true;
            std::vector<std::string> pending{R->owner};
            Strings seen;
            while (!pending.empty() && unique) {
              budget.tick();
              auto owner = std::move(pending.back());
              pending.pop_back();
              if (!seen.insert(owner).second)
                continue;
              const auto decl = classes.find(owner);
              if (decl == classes.end()) {
                unique = owner == "Ljava/lang/Object;";
                continue;
              }
              for (const auto &other : decl->second.methods) {
                budget.tick();
                if (other.reference.name == R->name &&
                    other.reference.parameters != R->parameters &&
                    (other.reference.parameters.size() == R->parameters.size() ||
                     has(other.access, "varargs"))) {
                  unique = false;
                  break;
                }
              }
              if (decl->second.superclass)
                pending.push_back(*decl->second.superclass);
              pending.insert(pending.end(), decl->second.interfaces.begin(),
                             decl->second.interfaces.end());
            }
            if (unique)
              Argument = get(Regs[Cursor], *bound);
          }
        }
      }
      if (staticOverload) {
        const auto id = staticOverload->parameters.at(Args.size());
        const auto &type = staticOverload->types.at(id);
        if (type.erasure == T && !type.segments.empty() &&
            !type.segments.back().arguments.empty())
          Argument = "((" + type_names.render(*staticOverload, id,
                        JavaScope{ownerClass(), &method.reference}) + ") " + Argument + ")";
      }
      if (Declaration && !Static && R->name != "<init>")
        if (const auto *Contract =
                sourceMethodSignature(Declaration->reference, generics);
            Contract && Args.size() < Contract->parameters.size())
          Argument = lexicalMemberArgument(
              Declaration->reference.owner, *Contract,
              Contract->parameters[Args.size()], T, std::move(Argument));
      if (Declaration && Strict)
        Argument = intersectionArgument(Op, Incoming, Declaration->reference,
                                        Args.size(), std::move(Argument));
      Args.push_back(constructorArgument(*R, Args.size(), std::move(Argument)));
      if (wordWidth(T) == 2 &&
          (Cursor + 1 >= Regs.size() || Regs[Cursor + 1] != Regs[Cursor] + 1))
        fail("invocation has a broken wide argument");
      Cursor += wordWidth(T);
    }
    if (Cursor != Regs.size())
      fail("invocation has too many argument words");
    if (R->name == "<init>") {
      auto N = constructors.find(Op.pc);
      if (N == constructors.end())
        fail("initializer is not bound to its uninitialized allocation");
      std::string Expected =
          "new:" + std::to_string(N->second) + ":" + R->owner;
      if (Incoming[Regs[0]] != Strings{Expected} || R->returns != "V")
        fail("initializer is not bound to its uninitialized allocation");
      // Initialization changes the allocation identity in every live alias,
      // independently of which physical word holds the range-call receiver.
      // Exceptional dataflow still uses Incoming and exposes no partial value.
      for (unsigned Register = 0; Register < method.registers; ++Register)
        if (Incoming[Register] == Strings{Expected})
          S[Register] = Strings{R->owner};
      if (auto Region = allocation_region_owners.find(Op.pc);
          emitting && Region != allocation_region_owners.end() &&
          allocation_regions.at(Region->second).end == Op.pc) {
        write(S, Regs[0], R->owner, "null");
        // The expression can throw during allocation, argument evaluation or
        // construction. All admitted edges have proven equivalent cleanup;
        // dispatch them through the original allocation entry, whose physical
        // registers remain committed until the complete expression succeeds.
        Out.append("pc = " + std::to_string(Region->second) + ";");
        for (const auto &Line :
             emitNestedAllocation(allocation_regions.at(Region->second)))
          Out.append(Line);
        return {std::move(S), Out.get()};
      }
      if (const auto Plan = conditional_allocations.find(N->second);
          emitting && Plan != conditional_allocations.end()) {
        if (Args.empty())
          fail("conditional allocation requires its original first argument");
        // The new expression starts at allocation, before its first argument.
        // Its closed argument dispatcher then retains each original PC and
        // register write, reaching the initializer PC before yielding. An
        // exception therefore leaves the switch expression for the ordinary
        // outer handler dispatcher with its exact prethrow register frame;
        // unlike the linear value graph, no argument copies are deferred.
        Out.append("pc = " + std::to_string(N->second) + ";");
        Args[0] = conditionalAllocationArgument(Plan->second, Args[0]);
      }
      put(Regs[0], R->owner, allocationExpression(R->owner, Args));
      for (unsigned Register = 0; Register < method.registers; ++Register)
        if (Register != Regs[0] && Incoming[Register] == Strings{Expected})
          Out.append(
              write(S, Register, R->owner, "o" + std::to_string(Regs[0])));
      return {std::move(S), Out.get()};
    }
    std::string Owner;
    auto Bridge = Declaration ? inheritance.bridges.find(Declaration->reference)
                              : inheritance.bridges.end();
    if (Bridge != inheritance.bridges.end() &&
        Bridge->second.protected_contract &&
        !inheritance.public_protected_bridge_targets.contains(
            Bridge->second.target))
      fail("protected erased bridge dispatch needs a source receiver access "
           "proof");
    const auto SuperDeclaration =
        generics.superclass_call_declarations.find({method.reference, *R});
    const bool PublicAccessSuper =
        Bridge != inheritance.bridges.end() &&
        Bridge->second.public_superclass_access &&
        starts(Name, "invoke-super") &&
        SuperContract != generics.superclass_calls.end() &&
        SuperDeclaration != generics.superclass_call_declarations.end() &&
        Declaration && SuperDeclaration->second == Declaration->reference;
    if (Bridge != inheritance.bridges.end() && starts(Name, "invoke-super") &&
        !PublicAccessSuper)
      fail("super bridge dispatch requires a proven specialized receiver "
           "binding");
    if (starts(Name, "invoke-super"))
      if (const auto Proof =
              inheritance.inherited_interface_default_accessors.find(
                  method.reference);
          Proof != inheritance.inherited_interface_default_accessors.end() &&
          Proof->second.first == *R) {
        const auto &Forward = Proof->second.second;
        std::vector<std::string> Forwarded{
            get(Regs[0], method.reference.owner)};
        Forwarded.insert(Forwarded.end(), Args.begin(), Args.end());
        const auto Call = staticOwner(Forward.owner) + "." +
                          sourceMethodName(Forward, inheritance) + "(" +
                          join(Forwarded, ", ") + ")";
        Out.append(R->returns == "V" ? Call + ";"
                                     : resultWrite(R->returns, Call));
        return {std::move(S), Out.get()};
      }
    if (starts(Name, "invoke-super") && static_super_helper &&
        static_super_helper->first == Op.pc) {
      // The original static prefix has already executed in this same Body.
      // Resolve the private helper only at the original superclass call, so a
      // null receiver cannot bypass an earlier check or class initializer.
      const auto Default =
          inheritance.interface_default_accessors.find(method.reference);
      const bool OriginalDefault =
          Default != inheritance.interface_default_accessors.end() &&
          Default->second == *R;
      if ((R->returns != method.reference.returns && !OriginalDefault) ||
          R->parameters !=
              std::vector<std::string>(method.reference.parameters.begin() + 1,
                                       method.reference.parameters.end()))
        fail("static super accessor lost its exact dispatch receipt");
      const auto Call = get(Regs[0], method.reference.owner) + "." +
                        static_super_helper->second + "(" + join(Args, ", ") +
                        ")";
      Out.append(R->returns == "V" ? Call + ";"
                                   : resultWrite(R->returns, Call));
      return {std::move(S), Out.get()};
    }
    if (starts(Name, "invoke-super")) {
      if (method.access.contains("static") ||
          Incoming[Regs[0]] != Strings{"self:" + method.reference.owner})
        fail("super invocation lacks its actual receiver");
      Owner = "super";
      const auto ScopedSuper =
          inheritance.scoped_interface_super_calls.find({method.reference, *R});
      if (ScopedSuper != inheritance.scoped_interface_super_calls.end())
        Owner = sourceType(R->owner) + ".super";
      if (const auto proof =
              inheritance.retained_plain_bridges.find(method.reference);
          proof != inheritance.retained_plain_bridges.end() &&
          proof->second.interface_super_forwarder && proof->second.target == *R)
        Owner = sourceType(R->owner) + ".super";
      if (Declaration &&
          (generics.methods.contains(Declaration->reference) ||
           PublicAccessSuper) &&
          !R->parameters.empty()) {
        // Intersection arguments have already consumed their exact scoped
        // forwarding proof above; they need no class-formal substitution.
        if (SuperContract == generics.superclass_calls.end() &&
            erased_super_helper_target != *R &&
            ScopedSuper == inheritance.scoped_interface_super_calls.end() &&
            !generics.intersection_invocations.contains(Declaration->reference))
          fail(
              "generic super invocation requires a complete erasure-preserving "
              "superclass substitution");
        if (SuperContract != generics.superclass_calls.end()) {
          for (size_t I = 0; I < Args.size(); ++I)
            Args[I] = superclassArgument(*R, I, std::move(Args[I]));
        }
      }
    } else
      Owner = Static ? staticOwner(R->owner) : virtualReceiver(*R, Op, get(Regs[0], R->owner));
    if (Declaration && !Static && has(Declaration->access, "protected")) {
      const auto Alias =
          inheritance.source_method_names.find(Declaration->reference);
      const bool Renamed = Alias != inheritance.source_method_names.end() &&
                           Alias->second.identifier_only;
      const bool FamilySuper =
          starts(Name, "invoke-super") && protectedVirtualFamily(*Declaration);
      // The super path above already checked its actual self and retains
      // Owner=super. A complete naming receipt must not turn that dispatch
      // into an ordinary virtual call through this.
      if (!FamilySuper &&
          (generics.erased_generic_invocations.contains(
               Declaration->reference) ||
           (Renamed && packageOf(method.reference.owner) !=
                           packageOf(Declaration->reference.owner)))) {
        Owner = protectedInvocationOwner(Op, *Declaration, Incoming, Strict);
      }
    }
    if (Declaration) {
      const auto Alias =
          inheritance.source_method_names.find(Declaration->reference);
      const auto Signature = generics.methods.find(Declaration->reference);
      const auto NonReturning =
          Alias != inheritance.source_method_names.end() &&
                  Alias->second.nonreturning_void_bridge
              ? generics.nonreturning_void_bridges.find(
                    *Alias->second.nonreturning_void_bridge)
              : generics.nonreturning_void_bridges.end();
      const bool ErasedOwnFormals =
          NonReturning != generics.nonreturning_void_bridges.end() &&
          NonReturning->second.erased_method_formals &&
          NonReturning->second.target == Declaration->reference;
      // The complete raw binding uses the already checked descriptor casts.
      // A target's own method formal is not declared in the bridge's scope.
      if (Alias != inheritance.source_method_names.end() &&
          (Alias->second.nonreturning_void_bridge ||
           Alias->second.checked_reference_bridge) &&
          Signature != generics.methods.end() && !ErasedOwnFormals)
        for (size_t I = 0; I < Args.size(); ++I)
          if (isReference(R->parameters[I]))
            Args[I] = "((" +
                      type_names.render(
                          Signature->second, Signature->second.parameters.at(I),
                          JavaScope{ownerClass(), &method.reference}) +
                      ") " + Args[I] + ")";
    }
    if (Bridge != inheritance.bridges.end() && !PublicAccessSuper)
      Owner = "((" +
              sourceType(inheritance.public_protected_bridge_targets.contains(
                             Bridge->second.target)
                             ? Bridge->second.target.owner
                             : Bridge->second.dispatch.owner) +
              ") " + Owner + ")";
    if (Declaration) {
      const auto Projected =
          inheritance.retained_plain_bridges.find(Declaration->reference);
      if (Projected != inheritance.retained_plain_bridges.end() &&
          Projected->second.source_reference) {
        const auto &Source = sourceBridgeReference(Declaration->reference,
                                                   generics, inheritance);
        if (Static || !starts(Name, "invoke-virtual") ||
            Source.parameters.size() != Args.size())
          fail("array carrier source invocation needs its original virtual "
               "receiver and parameters");
        // Source cannot name a compiler-generated erased bridge. Evaluate the
        // original receiver and argument expressions once, then preserve its
        // null-receiver failure before the checked carrier conversion. This
        // also retains any deferred argument-read effects on the null path.
        const auto Stem = "__neverdBridge" + std::to_string(Op.pc);
        const auto Receiver = Stem + "Receiver";
        Out.append(sourceType(R->owner) + " " + Receiver + " = " + Owner + ";");
        for (size_t I = 0; I < Args.size(); ++I) {
          const auto Argument = Stem + "Arg" + std::to_string(I);
          Out.append(sourceType(R->parameters[I]) + " " + Argument + " = " +
                     Args[I] + ";");
          Args[I] = Argument;
        }
        Out.append("if (" + Receiver +
                   " == null) throw new java.lang.NullPointerException();");
        for (size_t I = 0; I < Args.size(); ++I)
          if (Source.parameters[I] != R->parameters[I])
            Args[I] =
                "((" + sourceType(Source.parameters[I]) + ") " + Args[I] + ")";
        Owner = Receiver;
      }
    }
    const auto SourceName = invocationName(*R, Declaration);
    std::string Call =
        Owner + "." + SourceName + "(" + invocationArguments(*R, Args) + ")";
    if (const auto abrupt = abrupt_argument_allocations.find(Op.pc);
        abrupt != abrupt_argument_allocations.end())
      Out.append(allocationExpression(abrupt->second, {Call}) + ";");
    else
      Out.append(R->returns == "V" ? Call + ";" : resultWrite(R->returns, Call));
  } else if (anyPrefix(Name, {"goto", "if-"}) ||
             Name.find("switch") != std::string::npos) {
    if (starts(Name, "goto")) {
      if (Name != "goto" && Name != "goto/16" && Name != "goto/32")
        fail("unsupported unconditional branch encoding");
      arity(0);
      if (!Op.target || !Op.targets.empty() || !Op.keys.empty() ||
          !Op.data.empty() || Op.element_width ||
          !std::holds_alternative<std::monostate>(Op.literal) ||
          !std::holds_alternative<std::monostate>(Op.reference))
        fail("unconditional branch has unexpected control, payload or decoded "
             "operands");
    } else if (starts(Name, "if-"))
      condition(Op, Incoming, Strict);
    else if (Name.find("switch") != std::string::npos) {
      arity(1);
      get(Regs[0], "I");
    }
  } else if (Name == "throw") {
    arity(1);
    std::string Value = get(Regs[0], "Ljava/lang/Object;");
    Out.append("throw " + throwHelper() + "((java.lang.Throwable) " + Value +
               ");");
  } else {
    static const std::regex Unary("(neg|not)-(int|long|float|double)");
    static const std::regex Arithmetic(
        "(add|sub|mul|div|rem|and|or|xor|shl|shr|ushr)-(int|long|float|"
        "double)(/2addr|/lit8|/lit16)?");
    std::smatch Match;
    if (const auto Integer = integerPreparation(Op)) {
      arity(Integer->two_address || Integer->literal ? 2 : 3);
      const auto Left = get(Regs[Integer->two_address ? 0 : 1], "I");
      std::string Right;
      if (Integer->literal) {
        const auto *Literal = std::get_if<int64_t>(&Op.literal);
        if (!Literal)
          fail("literal operation has no signed integer literal");
        Right = hexLiteral(static_cast<uint64_t>(*Literal), 32);
      } else
        Right = get(Regs[Integer->two_address ? 1 : 2], "I");
      put(Regs[0], "I", integerExpression(*Integer, Left, Right));
    } else if (const auto Division = integerDivision(Op)) {
      arity(Division->two_address || Division->literal ? 2 : 3);
      const auto Left = get(Regs[Division->two_address ? 0 : 1], "I");
      const auto Right =
          Division->literal
              ? hexLiteral(static_cast<uint64_t>(std::get<int64_t>(Op.literal)),
                           32)
              : get(Regs[Division->two_address ? 1 : 2], "I");
      put(Regs[0], "I",
          "((" + Left + ") " + Division->symbol + " (" + Right + "))");
    } else if (const auto Binary = longBinary(Name)) {
      arity(Binary->two_address ? 2 : 3);
      if (Op.target || !Op.targets.empty() || !Op.keys.empty() ||
          !Op.data.empty() || Op.element_width ||
          !std::holds_alternative<std::monostate>(Op.reference) ||
          !std::holds_alternative<std::monostate>(Op.literal))
        fail("long binary operation has unexpected control, payload or decoded "
             "operands");
      const auto Left = get(Regs[Binary->two_address ? 0 : 1], "J");
      const auto Right =
          get(Regs[Binary->two_address ? 1 : 2], Binary->right_type);
      put(Regs[0], "J",
          "((" + Left + ") " + Binary->symbol + " (" + Right + "))");
    } else if (const auto Floating = floatingArithmetic(Name)) {
      arity(Floating->two_address ? 2 : 3);
      const auto Left =
          get(Regs[Floating->two_address ? 0 : 1], Floating->type);
      const auto Right =
          get(Regs[Floating->two_address ? 1 : 2], Floating->type);
      put(Regs[0], Floating->type,
          floatingArithmeticExpression(*Floating, Left, Right));
    } else if (const auto Operation = scalarUnary(Name)) {
      arity(2);
      put(Regs[0], Operation->first,
          Operation->second + "(" + get(Regs[1], Operation->first) + ")");
    } else if (std::regex_match(Name, Match, Unary)) {
      fail("invalid floating bitwise negation");
    } else if (const auto Conversion = scalarConversion(Name)) {
      arity(2);
      const auto &[Source, Dest] = *Conversion;
      put(Regs[0], Dest,
          scalarConversionExpression(Dest, get(Regs[1], Source)));
    } else if (const auto Comparison = scalarComparison(Name)) {
      arity(3);
      auto A = get(Regs[1], Comparison->type),
           B = get(Regs[2], Comparison->type);
      put(Regs[0], "I", scalarComparisonExpression(*Comparison, A, B));
    } else {
      if (baseOpcode(Name) == "div-int" || baseOpcode(Name) == "rem-int")
        fail("integer division/remainder has malformed decoded operands");
      if (!std::regex_match(Name, Match, Arithmetic))
        fail("unsupported instruction: " + Name);
      std::string Operator = Match[1].str(),
                  Type = ArithmeticTypes.at(Match[2].str()),
                  Form = Match[3].str();
      if (Strings{"and", "or", "xor", "shl", "shr", "ushr"}.contains(
              Operator) &&
          Type != "I" && Type != "J")
        fail("invalid arithmetic carrier");
      arity(Form.empty() ? 3 : 2);
      std::string Left = get(Form == "/2addr" ? Regs[0] : Regs[1], Type), Right;
      if (Form == "/lit8" || Form == "/lit16") {
        const auto *V = std::get_if<int64_t>(&Op.literal);
        if (Type != "I" || !V)
          fail("literal operation has no signed integer literal");
        Right = hexLiteral(static_cast<uint64_t>(*V), 32);
      } else {
        std::string RightType =
            Operator == "shl" || Operator == "shr" || Operator == "ushr" ? "I"
                                                                         : Type;
        Right = get(Form == "/2addr" ? Regs[1] : Regs[2], RightType);
      }
      static const std::map<std::string, std::string> Symbols = {
          {"add", "+"}, {"sub", "-"},  {"mul", "*"},
          {"div", "/"}, {"rem", "%"},  {"and", "&"},  {"or", "|"},
          {"xor", "^"}, {"shl", "<<"}, {"shr", ">>"}, {"ushr", ">>>"}};
      put(Regs[0], Type,
          "((" + Left + ") " + Symbols.at(Operator) + " (" + Right + "))");
    }
  }
  return {std::move(S), Out.get()};
}

std::string
Body::condition(const Instruction &Op, const State &S, bool Strict,
                const std::function<std::string(unsigned, const std::string &)>
                    &Reader) const {
  const auto Relation = conditionalBranch(Op.opcode);
  if (!Relation)
    fail("unsupported conditional relation");
  if (!Op.target || !Op.targets.empty() || !Op.keys.empty() ||
      !Op.data.empty() || Op.element_width ||
      !std::holds_alternative<std::monostate>(Op.literal) ||
      !std::holds_alternative<std::monostate>(Op.reference))
    fail("conditional branch has unexpected control, payload or decoded "
         "operands");
  const bool Zero = Relation->zero;
  const auto &Regs = Op.registers;
  if (Regs.size() != (Zero ? 1 : 2))
    fail("conditional branch register count disagrees");
  Strings Kinds = S.at(Regs[0]);
  if (!Zero)
    Kinds.insert(S.at(Regs[1]).begin(), S.at(Regs[1]).end());
  bool Reference = std::any_of(Kinds.begin(), Kinds.end(), [](const auto &K) {
    return isReference(K) || K == "null";
  });
  std::string Left, Right;
  auto operand = [&](unsigned Reg, const std::string &Type) {
    return Reader ? Reader(Reg, Type) : read(S, Reg, Type, Strict);
  };
  if (Reference) {
    if (Relation->symbol != "==" && Relation->symbol != "!=")
      fail("ordered comparison of object references");
    Left = operand(Regs[0], "Ljava/lang/Object;");
    Right = Zero ? "null" : operand(Regs[1], "Ljava/lang/Object;");
  } else {
    Left = operand(Regs[0], "I");
    Right = Zero ? "0" : operand(Regs[1], "I");
  }
  return Left + " " + Relation->symbol + " " + Right;
}

State Body::initial() const {
  State S(method.registers, Strings{"?"});
  if (has_constructor_prologue) {
    for (const auto &[Reg, Value] : constructor_values)
      S.at(Reg) = Strings{Value.first};
    return S;
  }
  unsigned Reg = method.registers - method.incomingWords();
  if (!method.access.contains("static"))
    S.at(Reg++) = Strings{"self:" + method.reference.owner};
  for (const auto &T : method.reference.parameters) {
    S.at(Reg) = Strings{T};
    if (wordWidth(T) == 2)
      S.at(Reg + 1) = Strings{"wide-high"};
    Reg += wordWidth(T);
  }
  return S;
}

void Body::validateLocalOperations() const {
  for (size_t I = 0; I < code.size(); ++I) {
    budget.tick();
    const auto &Op = code[I];
    const auto &Name = Op.opcode;
    if (anyPrefix(Name,
                  {"check-cast", "instance-of", "const-string", "const-class",
                   "new-array", "filled-new-array", "fill-array-data",
                   "array-length", "aget", "aput", "iget", "iput", "monitor-",
                   "return-object", "move-result-object"}))
      fail("local-class object operation is outside the bounded projection: " +
           Name);
    if (Name == "new-instance") {
      auto Local = local_projection.by_class.find(referenceType(Op));
      if (Local == local_projection.by_class.end() ||
          *Local->second->enclosing_method != *local_closure)
        fail("local-class object allocation has no proven lexical owner");
    }
    if (anyPrefix(Name, {"sget", "sput"})) {
      const auto *Ref = std::get_if<FieldRef>(&Op.reference);
      if (!Ref || !LocalProjection::scalar(Ref->type))
        fail("local-class object field access is unsupported");
    }
    if (!starts(Name, "invoke-"))
      continue;
    const auto *Ref = std::get_if<MethodRef>(&Op.reference);
    if (!Ref || !LocalProjection::scalar(Ref->returns, true) ||
        !std::all_of(Ref->parameters.begin(), Ref->parameters.end(),
                     [](const auto &T) { return LocalProjection::scalar(T); }))
      fail("local-class object invocation argument or return is unsupported");
    if (baseOpcode(Name) == "invoke-static")
      continue;
    if (I == 0 && first == 1 &&
        *Ref == MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"})
      continue;
    auto Local = local_projection.by_class.find(Ref->owner);
    if (Local == local_projection.by_class.end() ||
        *Local->second->enclosing_method != *local_closure)
      fail("local-class object invocation receiver has no proven local body");
  }
}

Body::Origins Body::localOrigins(const Instruction &Op, const Origins &Incoming,
                                 bool Strict) const {
  Origins Out = Incoming;
  const auto &Name = Op.opcode;
  const auto Base = baseOpcode(Name);
  const auto &Regs = Op.registers;
  auto require = [&](unsigned Reg, uint8_t Allowed, const char *Reason) {
    if (Strict && (Incoming.at(Reg) == 0 || (Incoming.at(Reg) & ~Allowed) != 0))
      fail(std::string("local-class object ") + Reason);
  };
  if (Base == "move-object") {
    require(Regs.at(1), Local | Exception | Null,
            "copy has no proven local, exception, or null origin");
    Out.at(Regs.at(0)) = Incoming.at(Regs.at(1));
  } else if (Name == "move-exception")
    Out.at(Regs.at(0)) = Exception;
  else if (Name == "new-instance")
    Out.at(Regs.at(0)) = Local;
  else if (starts(Name, "invoke-")) {
    if (Base != "invoke-static")
      require(Regs.at(0), Local, "receiver is not a proven local instance");
  } else if (Name == "throw")
    require(Regs.at(0), Exception | Null,
            "throw operand is not a proven exception carrier");
  else if (starts(Name, "if-")) {
    for (unsigned R : Regs)
      require(R, Scalar | Null, "identity comparison is unsupported");
  } else if (Name == "nop" || starts(Name, "goto") ||
             Name.find("switch") != std::string::npos ||
             starts(Name, "return") || starts(Name, "sput")) {
    // No register write. Scalar compatibility and definedness are still
    // checked by operation(), independently of this object-origin domain.
  } else if (!Regs.empty()) {
    unsigned Dest = Regs[0];
    bool Wide = starts(Name, "const-wide") || Base == "move-wide" ||
                Name == "move-result-wide" || Name == "sget-wide" ||
                (!starts(Name, "cmp") &&
                 (Base.ends_with("-long") || Base.ends_with("-double")));
    Out.at(Dest) = Scalar;
    if (Base == "move")
      Out.at(Dest) = Incoming.at(Regs.at(1));
    else if (starts(Name, "const") && !Wide) {
      const auto *Value = std::get_if<int64_t>(&Op.literal);
      if (Value && *Value == 0)
        Out.at(Dest) = Null;
    }
    if (Wide)
      Out.at(Dest + 1) = Scalar;
  }
  return Out;
}

void Body::verify() {
  monitor_identities_verified = false;
  method_synchronized = false;
  uint32_t Start = code[first].pc;
  states[Start] = initial();
  // Tokens identify this acquisition's object, independently of verifier
  // types. Joins intersect must-alias facts; successful reference copies and
  // casts preserve them, actual writes kill them, and exceptional edges retain
  // the pre-instruction facts. The source synchronized statement snapshots
  // the acquired object, so its register may subsequently be reused.
  using MonitorAliases = std::vector<std::set<size_t>>;
  std::map<uint32_t, MonitorAliases> Aliases;
  if (!monitors.empty())
    Aliases[Start] = MonitorAliases(method.registers);
  if (local_closure) {
    Origins Entry(method.registers, Scalar);
    if (!method.access.contains("static"))
      Entry.at(method.registers - method.incomingWords()) = Local;
    origins[Start] = std::move(Entry);
  }
  std::deque<uint32_t> Queue{Start};
  std::set<uint32_t> Queued{Start};
  std::map<uint32_t, std::set<uint32_t>> NormalPredecessors;
  while (!Queue.empty()) {
    budget.tick(
        method.registers * (1 + bool(local_closure) + !monitors.empty()) + 1);
    uint32_t PC = Queue.front();
    Queue.pop_front();
    Queued.erase(PC);
    size_t Index = by_pc.at(PC);
    State Incoming = states.at(PC);
    std::set<unsigned> Written;
    State Outgoing = operation(code[Index], Incoming, false,
                               monitors.empty() ? nullptr : &Written)
                         .first;
    MonitorAliases IncomingAliases, OutgoingAliases;
    if (!monitors.empty()) {
      IncomingAliases = Aliases.at(PC);
      OutgoingAliases = IncomingAliases;
      for (const auto Reg : Written)
        OutgoingAliases.at(Reg).clear();
      const auto &Op = code[Index];
      if (baseOpcode(Op.opcode) == "move-object")
        OutgoingAliases.at(Op.registers[0]) =
            IncomingAliases.at(Op.registers[1]);
      else if (Op.opcode == "check-cast")
        OutgoingAliases.at(Op.registers[0]) =
            IncomingAliases.at(Op.registers[0]);
      else if (monitors.contains(Index)) {
        // Reacquiring this source region starts a new object lifetime.
        for (auto &Tokens : OutgoingAliases)
          Tokens.erase(Index);
        OutgoingAliases.at(Op.registers[0]).insert(Index);
      }
    }
    Origins IncomingOrigins, OutgoingOrigins;
    if (local_closure) {
      IncomingOrigins = origins.at(PC);
      OutgoingOrigins = localOrigins(code[Index], IncomingOrigins, false);
    }
    auto edge = [&](uint32_t Target, const State &S, const Origins &O,
                    const MonitorAliases &A, bool Normal) {
      if (first && Target == code[0].pc)
        fail("control flow re-enters its constructor prefix");
      if (Normal)
        NormalPredecessors[Target].insert(PC);
      auto Old = states.find(Target);
      State Merged = S;
      bool Changed = Old == states.end();
      if (Old != states.end()) {
        for (size_t I = 0; I < Merged.size(); ++I)
          Merged[I].insert(Old->second[I].begin(), Old->second[I].end());
        Changed |= Merged != Old->second;
      }
      if (local_closure) {
        Origins MergedOrigins = O;
        auto Previous = origins.find(Target);
        if (Previous != origins.end()) {
          for (size_t I = 0; I < O.size(); ++I)
            MergedOrigins[I] |= Previous->second[I];
          Changed |= MergedOrigins != Previous->second;
        } else
          Changed = true;
        origins[Target] = std::move(MergedOrigins);
      }
      if (!monitors.empty()) {
        MonitorAliases MergedAliases = A;
        if (const auto Previous = Aliases.find(Target);
            Previous != Aliases.end()) {
          for (size_t I = 0; I < A.size(); ++I)
            std::erase_if(MergedAliases[I], [&](const auto Token) {
              return !Previous->second[I].contains(Token);
            });
          Changed |= MergedAliases != Previous->second;
        } else
          Changed = true;
        Aliases[Target] = std::move(MergedAliases);
      }
      if (!Changed)
        return;
      states[Target] = std::move(Merged);
      if (Queued.insert(Target).second)
        Queue.push_back(Target);
    };
    for (auto T : successors(Index))
      edge(T, Outgoing, OutgoingOrigins, OutgoingAliases, true);
    if (canThrow(code[Index]))
      for (const auto &H : handlers(PC))
        edge(H.target, Incoming, IncomingOrigins, IncomingAliases, false);
  }
  // Certify identity before allocation cleanup consumes the shared release
  // receipt. A structural release owner alone cannot suppress a throwing edge.
  for (const auto &[Index, Owner] : monitor_release_owners) {
    const auto &Op = code[Index];
    const auto Incoming = Aliases.find(Op.pc);
    if (Incoming == Aliases.end() ||
        !Incoming->second.at(Op.registers[0]).contains(Owner))
      fail("monitor region release has no proven acquired object identity");
  }
  monitor_identities_verified = true;
  for (const auto &[PC, Plan] : allocation_regions)
    verifyAllocationHandlers(Plan);
  for (const auto &[PC, Plan] : constructor_choices)
    verifyAllocationHandlers(Plan.cleanup);
  for (const auto &[PC, S] : states) {
    size_t Index = by_pc.at(PC);
    const auto &Op = code[Index];
    const auto &Predecessors = NormalPredecessors[PC];
    if (Op.opcode == "move-exception" && (PC == Start || !Predecessors.empty()))
      fail("normal flow enters an exception-only value");
    if (starts(Op.opcode, "move-result") &&
        Predecessors != std::set<uint32_t>{code[Index - 1].pc})
      fail("branch bypasses an invocation result producer");
    if (auto I = constructors.find(PC);
        I != constructors.end() &&
        !conditional_allocations.contains(I->second) &&
        !constructor_choices.contains(I->second) &&
        !forked_allocation_owners.contains(PC) &&
        !forward_allocations.contains(I->second))
      for (size_t Step = by_pc.at(I->second) + 1; Step <= Index; ++Step) {
        budget.tick();
        if (NormalPredecessors[code[Step].pc] !=
            std::set<uint32_t>{code[Step - 1].pc})
          fail("branch bypasses an uninitialized allocation");
      }
    operation(Op, S, true);
    if (local_closure)
      localOrigins(Op, origins.at(PC), true);
  }
  for (size_t I = first; I < code.size(); ++I)
    if (!states.contains(code[I].pc) && !emptyNop(code[I]))
      fail("unreachable instructions have no verified source projection");
  // The source flag alone grants no implicit lock. Retain it only when the
  // proven region owns the whole instance body through its sole return or
  // terminal throw. The header owns acquisition and release without adding
  // another lock depth or moving an unlocked suffix under the receiver lock.
  if (has(method.access, "declared-synchronized") &&
      !has(method.access, "synchronized") && !has(method.access, "static") &&
      code.size() >= 3 && monitors.size() == 1 &&
      monitors.begin()->first == 0 &&
      monitors.begin()->second.exits == std::set<size_t>{code.size() - 2}) {
    const bool TotalReturn =
        method.tries.empty() && starts(code.back().opcode, "return");
    bool WholeThrow = code.back().opcode == "throw";
    if (WholeThrow)
      for (size_t I = 1; I + 2 < code.size(); ++I) {
        budget.tick();
        if (!emptyNop(code[I]) && !monitors.begin()->second.body.contains(I)) {
          WholeThrow = false;
          break;
        }
      }
    const unsigned Receiver = method.registers - method.incomingWords();
    method_synchronized = (TotalReturn || WholeThrow) &&
                          code[0].registers == std::vector<unsigned>{Receiver};
  }
}

std::string Body::resultRead(const std::string &Type) {
  if (isReference(Type))
    return "resultObject";
  if (Type == "F")
    return "((java.lang.Float) null).intBitsToFloat(result32)";
  if (Type == "D")
    return "((java.lang.Double) null).longBitsToDouble(result64)";
  if (Type == "J")
    return "result64";
  if (Type == "Z")
    return "(result32 != 0)";
  return "result32";
}
std::string Body::resultWrite(const std::string &Type,
                              const std::string &Value) {
  if (isReference(Type))
    return "resultObject = " + Value + ";";
  return std::string(wordWidth(Type) == 2 ? "result64" : "result32") + " = " +
         registerStorage(Type, Value) + ";";
}

std::string Body::emit(std::string_view LocalDeclarations) {
  emitting = false;
  verify();
  // Expression rendering consumes converged register states, never a partial
  // dataflow iteration from the body verifier.
  emitting = true;
  Lines Out(budget, true);
  if (prefix_local_declarations && !LocalDeclarations.empty())
    Out.append(std::string(LocalDeclarations));
  if (!prefix.empty())
    Out.append(prefix);
  if (!prefix_local_declarations && !LocalDeclarations.empty())
    Out.append(std::string(LocalDeclarations));
  for (unsigned Reg = 0; Reg < method.registers; ++Reg) {
    std::string R = std::to_string(Reg);
    Out.extend({"int v" + R + " = 0;", "long w" + R + " = 0L;",
                "java.lang.Object o" + R + " = null;"});
  }
  unsigned Reg = method.registers - method.incomingWords();
  State Initial = initial();
  if (!method.access.contains("static"))
    Out.append("o" + std::to_string(Reg++) + " = this;");
  for (size_t I = 0; I < method.reference.parameters.size(); ++I) {
    const auto &T = method.reference.parameters[I];
    Out.append(write(Initial, Reg, T,
                     enum_projection && I < 2
                         ? (I == 0 ? "super.name()" : "super.ordinal()")
                         : "arg" + std::to_string(I)));
    Reg += wordWidth(T);
  }
  if (has_constructor_prologue)
    for (const auto &[Reg, Value] : constructor_values)
      if (Value.first != "wide-high")
        Out.append(write(Initial, Reg, Value.first, Value.second));
  Out.extend({"int result32 = 0;", "long result64 = 0L;",
              "java.lang.Object resultObject = null;",
              "java.lang.Throwable caught = null;"});
  for (const auto &[Root, Plan] : allocation_regions)
    declareAllocationValues(Out, Plan, false);
  auto statements = [&](const Instruction &Op) {
    auto Result = operation(Op, states.at(Op.pc), true).second;
    // These reads execute inside `new T(...)`, so class initialization for T
    // still precedes argument effects. Assignment expressions retain the
    // original register values for later instructions.
    if (deferred_allocation_reads.contains(Op.pc))
      Result.clear();
    if (auto Region = allocation_region_owners.find(Op.pc);
        Region != allocation_region_owners.end()) {
      const auto &Plan = allocation_regions.at(Region->second);
      if (Op.pc != Plan.start && Op.pc != Plan.end)
        Result.clear();
    }

    return Result;
  };
  auto exceptionDispatch = [&](const std::string &Label,
                               const std::string &Indent,
                               const MonitorRegion *ActiveMonitor = nullptr) {
    for (const auto &R : method.tries) {
      if (ActiveMonitor) {
        // Locked handlers execute before their exact release. A post-release
        // rethrow is an unlocked instruction with its original PC and locals.
        if (!ActiveMonitor->dispatch_handlers ||
            !std::all_of(
                R.handlers.begin(), R.handlers.end(), [&](const auto &Catch) {
                  return ActiveMonitor->body.contains(by_pc.at(Catch.target));
                }))
          continue;
      } else if (!monitors.empty() &&
                 std::any_of(R.handlers.begin(), R.handlers.end(),
                             [&](const auto &H) {
                               const auto I = by_pc.at(H.target);
                               return std::any_of(
                                   monitors.begin(), monitors.end(),
                                   [&](const auto &Entry) {
                                     return Entry.second.body.contains(I) ||
                                            Entry.second.cleanup.contains(I);
                                   });
                             }))
        continue;
      Out.append(Indent + "if (pc >= " + std::to_string(R.start) + " && pc < " +
                 std::to_string(R.end) + ") {");
      for (const auto &H : R.handlers) {
        auto Test =
            H.type ? "failure instanceof " + sourceType(*H.type) : "true";
        Out.append(Indent + "  if (" + Test + ") { caught = failure; pc = " +
                   std::to_string(H.target) + "; continue " + Label + "; }");
      }
      Out.append(Indent + "}");
    }
  };
  // A separate JVM switch entry and backedge for every Dalvik instruction
  // can exhaust Code's 65535-byte limit. In large ordinary dispatchers only
  // control-flow destinations need case labels. Linear instructions retain
  // separate scopes for their temporaries and keep their original order.
  const bool compact_dispatch =
      code.size() >= 1024 && final_writes.empty() && monitors.empty() &&
      forked_allocations.empty() && conditional_allocations.empty() &&
      constructor_choices.empty();
  std::set<uint32_t> dispatch_entries;
  if (compact_dispatch) {
    dispatch_entries.insert(code.at(first).pc);
    for (size_t i = first; i < code.size(); ++i) {
      const auto &op = code[i];
      if (!states.contains(op.pc))
        continue;
      if (branchTarget(op) || op.opcode.find("switch") != std::string::npos)
        for (auto successor : normalSuccessors(code, i))
          dispatch_entries.insert(successor);
    }
    for (const auto &region : method.tries)
      for (const auto &handler : region.handlers)
        dispatch_entries.insert(handler.target);
  }
  const FinalWritePlan::OrderedStage *OrderedStage = nullptr;
  bool selectFinalStore = false;
  std::string OrderedValue;
  std::set<size_t> EmittedOrderedWrites;
  std::function<void(size_t, size_t, const std::string &,
                     const MonitorRegion *)>
      cases;
  cases = [&](size_t Begin, size_t End, const std::string &Label,
              const MonitorRegion *ActiveMonitor) {
    for (size_t Index = Begin; Index < End; ++Index) {
      if (OrderedStage && !OrderedStage->members.contains(Index))
        continue;
      const auto &Op = code[Index];
      if (const auto owner = forked_allocation_owners.find(Op.pc);
          owner != forked_allocation_owners.end() && owner->second != Op.pc)
        continue;
      if (const auto Owner = constructor_choice_owners.find(Op.pc);
          Owner != constructor_choice_owners.end() && Op.pc != Owner->second)
        continue;
      if (const auto Owner = conditional_allocation_owners.find(Op.pc);
          Owner != conditional_allocation_owners.end()) {
        const auto &Plan = conditional_allocations.at(Owner->second);
        if (Index != Plan.begin && Index != Plan.end)
          continue; // Emitted once inside the original new expression.
      }
      const bool Outer = ActiveMonitor == nullptr;
      if (!method_synchronized &&
          ((!Outer && !ActiveMonitor->body.contains(Index)) ||
           (Outer && std::any_of(monitors.begin(), monitors.end(),
                                 [&](const auto &Entry) {
                                   const auto &Plan = Entry.second;
                                   return Plan.body.contains(Index) ||
                                          Plan.exits.contains(Index) ||
                                          Plan.cleanup.contains(Index);
                                 }))))
        continue;
      if (monitors.contains(Index) && !method_synchronized) {
        const auto &Monitor = monitors.at(Index);
        const auto Inner = "__neverdMonitor" + std::to_string(Op.pc);
        Out.extend(
            {"      case " + std::to_string(Op.pc) + ": {",
             "        synchronized (" +
                 read(states.at(Op.pc), Monitor.object, "Ljava/lang/Object;",
                      true) +
                 ") {",
             "          pc = " + std::to_string(code[Index + 1].pc) + ";",
             "          " + Inner + ": while (true) {"});
        if (Monitor.dispatch_handlers)
          Out.append("            try {");
        Out.append("            switch (pc) {");
        cases(Monitor.enter + 1, code.size(), Inner, &Monitor);
        for (auto Exit : Monitor.exits)
          Out.append("            case " + std::to_string(code[Exit].pc) +
                     ": pc = " + std::to_string(code[Exit + 1].pc) +
                     "; break " + Inner + ";");
        Out.extend({"            default: throw new "
                    "java.lang.AssertionError(\"Invalid recovered monitor "
                    "flow\");",
                    "            }"});
        if (Monitor.dispatch_handlers) {
          Out.append("            } catch (java.lang.Throwable failure) {");
          exceptionDispatch(Inner, "              ", &Monitor);
          Out.append("              throw " + throwHelper() + "(failure);");
          Out.append("            }");
        }
        Out.extend({"          }", "        }",
                    "        continue " + Label + ";", "      }"});
        continue;
      }
      auto S = states.find(Op.pc);
      if (S == states.end())
        continue;
      if (const auto fork = forked_allocations.find(Op.pc); fork != forked_allocations.end()) {
        const auto &plan = fork->second;
        auto arm = [&](const AllocationRegion &path) {
          declareAllocationValues(Out, path, true);
          for (const auto &line : emitNestedAllocation(path))
            Out.append("          " + line);
          Out.append("          pc = " + std::to_string(code[by_pc.at(path.end) + 1].pc) + ";");
        };
        Out.extend({"      case " + std::to_string(Op.pc) + ": {",
                    "        if (" + condition(code[plan.branch], S->second, true) + ") {"});
        arm(plan.taken);
        Out.append("        } else {");
        arm(plan.fallthrough);
        Out.extend({"        }", "        continue " + Label + ";", "      }"});
        continue;
      }
      const bool OrderedWrite =
          OrderedStage && OrderedStage->writes.contains(Index);
      if (OrderedWrite && selectFinalStore) {
        EmittedOrderedWrites.insert(Index);
        Out.append("      case " + std::to_string(Op.pc) + ": break " + Label + ";");
        continue;
      }
      if (OrderedWrite)
        final_assignment_target = OrderedValue;
      auto Statements = statements(Op);
      final_assignment_target.reset();
      if (OrderedWrite && Statements.size() != 1)
        fail("ordered final store crosses an allocation expression");
      if (!compact_dispatch || dispatch_entries.contains(Op.pc))
        Out.append("      case " + std::to_string(Op.pc) + ": {");
      else
        Out.append("      {");
      if (compact_dispatch && !method.tries.empty())
        Out.append("        pc = " + std::to_string(Op.pc) + ";");
      for (const auto &Statement : Statements)
        Out.append("        " + Statement);
      if (OrderedWrite) {
        EmittedOrderedWrites.insert(Index);
        Out.extend(
            {"        pc = " + std::to_string(code.at(Index + 1).pc) + ";",
             "        break " + Label + ";", "      }"});
        continue;
      }
      if (starts(Op.opcode, "if-")) {
        auto Condition = condition(Op, S->second, true);
        Out.append("        pc = (" + Condition + ") ? " +
                   std::to_string(target(Op)) + " : " +
                   std::to_string(code.at(Index + 1).pc) + ";");
      } else if (Op.opcode.find("switch") != std::string::npos) {
        Out.append("        switch (v" + std::to_string(Op.registers[0]) +
                   ") {");
        for (size_t I = 0; I < Op.keys.size(); ++I)
          Out.append("          case " +
                     hexLiteral(static_cast<uint64_t>(Op.keys[I]), 32) +
                     ": pc = " + std::to_string(Op.targets[I]) + "; break;");
        Out.extend({"          default: pc = " +
                        std::to_string(code.at(Index + 1).pc) + ";",
                    "        }"});
      } else if (starts(Op.opcode, "goto"))
        Out.append("        pc = " + std::to_string(target(Op)) + ";");
      else if (!anyPrefix(Op.opcode, {"return", "throw"})) {
        if (compact_dispatch) {
          Out.append("      }");
          continue;
        }
        const auto Plan = conditional_allocations.find(Op.pc);
        const auto Choice = constructor_choices.find(Op.pc);
        const auto Next =
            Choice != constructor_choices.end()     ? Choice->second.join
            : Plan == conditional_allocations.end() ? Index + 1
                                                    : Plan->second.end;
        Out.append("        pc = " + std::to_string(code.at(Next).pc) + ";");
      }
      if (!anyPrefix(Op.opcode, {"return", "throw"}))
        Out.append("        continue " + Label + ";");
      Out.append("      }");
    }
  };
  if (!final_writes.empty()) {
    const auto Flow = finalWriteRegions(method, final_writes, budget);
    if (!Flow.branch_stages.empty()) {
      Out.extend({"int pc = " + std::to_string(code[first].pc) + ";",
                  "try {"});
      selectFinalStore = true;
      std::function<void(size_t)> emitStage = [&](size_t index) {
        const auto &stage = Flow.branch_stages.at(index);
        const auto label = "__neverdFinalRegion" + std::to_string(index);
        OrderedStage = &stage.flow;
        EmittedOrderedWrites.clear();
        Out.extend({label + ": while (true) {", "  switch (pc) {"});
        cases(first, code.size(), label, nullptr);
        if (EmittedOrderedWrites != stage.flow.writes)
          fail("branched final store is owned by another source expression");
        Out.extend({"  default: throw new java.lang.AssertionError(\"Invalid recovered final flow\");",
                    "  }", "}"});
        if (stage.stores.empty())
          return;
        const bool fork = stage.stores.size() > 1;
        if (fork)
          Out.append("switch (pc) {");
        for (const auto &[field, writes] : stage.stores) {
          if (fork) {
            for (auto at : writes)
              Out.append("case " + std::to_string(code[at].pc) + ":");
            Out.append("{");
          }
          Out.append("switch (pc) {");
          for (auto at : writes) {
            const auto lines = statements(code[at]);
            if (lines.size() != 1)
              fail("branched final field write crosses an allocation expression");
            Out.extend({"case " + std::to_string(code[at].pc) + ":",
                        lines.front(), "pc = " + std::to_string(code[at + 1].pc) + "; break;"});
          }
          Out.extend({"default: throw new java.lang.AssertionError(\"Invalid recovered final store\");", "}"});
          emitStage(stage.successors.at(field));
          if (fork)
            Out.append("}");
        }
        if (fork)
          Out.extend({"default: throw new java.lang.AssertionError(\"Invalid recovered final order\");", "}"});
      };
      emitStage(0);
      Out.extend({"} catch (java.lang.Throwable failure) {",
                  "  throw " + throwHelper() + "(failure);", "}"});
      return Out.text();
    }
    if (!Flow.ordered_stages.empty()) {
      if (!monitors.empty())
        fail("ordered final assignments cross a monitor region");
      Out.extend({"int pc = " + std::to_string(code[first].pc) + ";",
                  "dispatch: {", "  try {"});
      for (size_t Number = 0; Number < Flow.ordered_stages.size(); ++Number) {
        const auto &Stage = Flow.ordered_stages[Number];
        if (!Stage.writes.empty() && *Stage.writes.rbegin() < first)
          continue; // The verified constructor prologue already assigned it.
        if (!Stage.writes.empty() && *Stage.writes.begin() < first)
          fail("ordered final event crosses the constructor prologue");
        const auto Label = "__neverdRegionOrdered" + std::to_string(Number);
        if (Stage.field) {
          const auto &Field = *Stage.field;
          const auto PC = code[*Stage.writes.begin()].pc;
          OrderedValue = type_names.finalGuardName(PC, "Value");
          auto Type = sourceType(Field.type);
          if (auto I = generics.fields.find(Field); I != generics.fields.end())
            Type =
                type_names.render(I->second, *I->second.field_type,
                                  JavaScope{ownerClass(), &method.reference});
          Out.append("    " + Type + " " + OrderedValue + ";");
        }
        OrderedStage = &Stage;
        EmittedOrderedWrites.clear();
        Out.extend({"    " + Label + ": while (true) {", "      try {",
                    "        switch (pc) {"});
        cases(first, code.size(), Label, nullptr);
        if (EmittedOrderedWrites != Stage.writes)
          fail("ordered final store is owned by another source expression");
        Out.extend(
            {"      default: throw new java.lang.AssertionError(\"Invalid "
             "recovered initialization flow\");",
             "        }", "      } catch (java.lang.Throwable failure) {"});
        exceptionDispatch(Label, "        ");
        Out.extend({"        throw " + throwHelper() + "(failure);", "      }",
                    "    }"});
        if (Stage.field)
          Out.append(
              "    " +
              std::string(has(method.access, "static") ? "" : "this.") +
              sourceFieldName(*Stage.field, classes, inheritance, budget) +
              " = " + OrderedValue + ";");
      }
      OrderedStage = nullptr;
      Out.extend({"  } catch (java.lang.Throwable failure) {",
                  "    throw " + throwHelper() + "(failure);", "  }", "}"});
      return Out.text();
    }
    // Allocation expressions can publish an exception-dispatch PC even when
    // final writes otherwise use straight-line or fully structured emission.
    // Declare the shared carrier before any of those paths can return.
    const bool NeedsPC =
        Flow.structured_prefix_end || !allocation_regions.empty() ||
        !conditional_allocations.empty() || !method.tries.empty() ||
        std::any_of(code.begin() + first, code.end(),
                    [](const Instruction &Op) {
                      return branchTarget(Op) || !Op.targets.empty();
                    });
    if (NeedsPC)
      Out.append("int pc = 0;");
    std::function<void(size_t, size_t, const std::string &)> structured;
    if (!Flow.diamonds.empty()) {
      if (Flow.diamonds.begin()->first < first)
        fail("final assignment branch enters the constructor prologue");
      structured = [&](size_t Begin, size_t End, const std::string &Indent) {
        for (size_t Index = Begin; Index < End; ++Index) {
          const auto &Op = code[Index];
          if (const auto Abrupt = Flow.abrupt_guards.find(Index);
              Abrupt != Flow.abrupt_guards.end()) {
            Out.append(Indent + "if (" + condition(Op, states.at(Op.pc), true) +
                       ") {");
            structured(Abrupt->second.first, Abrupt->second.second,
                       Indent + "  ");
            Out.append(Indent + "}");
            continue;
          }
          if (const auto D = Flow.diamonds.find(Index);
              D != Flow.diamonds.end()) {
            std::string Test = condition(Op, states.at(Op.pc), true);
            size_t Fallthrough = Index + 1;
            if (!D->second.guards.empty()) {
              const auto Flag = type_names.finalGuardName(Op.pc);
              Out.append(Indent + "boolean " + Flag + " = " + Test + ";");
              for (const auto Guard : D->second.guards) {
                Out.append(Indent + "if (!" + Flag + ") {");
                structured(Fallthrough, Guard, Indent + "  ");
                const auto &Next = code[Guard];
                auto Test = condition(Next, states.at(Next.pc), true);
                if (Flow.inverted_guards.contains(Guard))
                  Test = "!(" + Test + ")";
                Out.append(Indent + "  " + Flag + " = " + Test + ";");
                Out.append(Indent + "}");
                Fallthrough = Guard + 1;
              }
              Test = Flag;
            }
            Out.append(Indent + "if (" + Test + ") {");
            structured(D->second.taken,
                       D->second.taken_end.value_or(D->second.join),
                       Indent + "  ");
            Out.append(Indent + "} else {");
            structured(Fallthrough, D->second.taken, Indent + "  ");
            Out.append(Indent + "}");
            Index = D->second.join - 1;
            continue;
          }
          if (Flow.structured_jumps.contains(Index))
            continue;
          for (const auto &Statement : statements(Op))
            Out.append(Indent + Statement);
          if (const auto Allocation = conditional_allocations.find(Op.pc);
              Allocation != conditional_allocations.end()) {
            if (Allocation->second.end >= End ||
                Allocation->second.members.size() !=
                    Allocation->second.end - Allocation->second.begin + 1)
              fail("conditional allocation crosses a final assignment region");
            // Its internal branch executes once inside the new expression.
            Index = Allocation->second.end - 1;
          }
        }
      };
      if (!Flow.structured_middle && (!Flow.structured_suffix_begin ||
                                      *Flow.structured_suffix_begin <= first)) {
        Out.extend({"dispatch: {", "  try {"});
        structured(first,
                   Flow.structured_prefix_end.value_or(
                       Flow.structured_end.value_or(code.size())),
                   "    ");
        if (!Flow.structured_prefix_end) {
          // When every common prefix write belongs to the verified Java 25
          // constructor prologue, continue at the original post-super entry.
          Out.extend({"  } catch (java.lang.Throwable failure) {",
                      "    throw " + throwHelper() + "(failure);", "  }", "}"});
          return Out.text();
        }
      }
    }
    if (!Flow.structured_prefix_end)
      Out.extend({"dispatch: {", "  try {"});
    size_t Begin = Flow.structured_prefix_end.value_or(first);
    for (auto End : Flow.boundaries) {
      if (End < Begin)
        continue; // A verified constructor prologue already emitted this write.
      const bool Region =
          std::any_of(code.begin() + Begin, code.begin() + End,
                      [](const Instruction &Op) {
                        return branchTarget(Op) || !Op.targets.empty();
                      }) ||
          (Begin < End && std::any_of(
                              method.tries.begin(), method.tries.end(),
                              [&](const TryRegion &Try) {
                                return Try.start < code[End].pc &&
                                       Try.end > code[Begin].pc;
                              }));
      if (Region) {
        const auto Label = "__neverdRegion" + std::to_string(End);
        Out.extend({"    pc = " + std::to_string(code[Begin].pc) + ";",
                    "    " + Label + ": while (true) {", "      try {",
                    "        switch (pc) {"});
        cases(Begin, End, Label, nullptr);
        if (Flow.terminal_tail_regions.contains(End))
          cases(*Flow.terminal_tail, code.size(), Label, nullptr);
        Out.extend(
            {"      case " + std::to_string(code[End].pc) + ": break " + Label +
                 ";",
             "      default: throw new java.lang.AssertionError(\"Invalid "
             "recovered initialization flow\");",
             "        }", "      } catch (java.lang.Throwable failure) {"});
        exceptionDispatch(Label, "        ");
        Out.extend({"        throw " + throwHelper() + "(failure);", "      }",
                    "    }"});
      } else {
        for (size_t Index = Begin; Index < End; ++Index)
          for (const auto &Statement : statements(code[Index]))
            Out.append("    " + Statement);
      }
      if (Flow.structured_suffix_begin == End) {
        structured(End, Flow.structured_end.value_or(code.size()), "    ");
        break;
      }
      if (Flow.structured_middle && Flow.structured_middle->first == End) {
        structured(End, Flow.structured_middle->second, "    ");
        Begin = Flow.structured_middle->second;
        continue;
      }
      for (const auto &Statement : statements(code[End]))
        Out.append("    " + Statement);
      Begin = End + 1;
    }
    Out.extend({"  } catch (java.lang.Throwable failure) {",
                "    throw " + throwHelper() + "(failure);", "  }", "}"});
    return Out.text();
  }
  Out.extend({"int pc = " + std::to_string(code[first].pc) + ";",
              "dispatch: while (true) {", "  try {", "    switch (pc) {"});
  cases(first, code.size(), "dispatch", nullptr);
  Out.extend({"      default: throw new java.lang.AssertionError(\"Invalid "
              "recovered control flow\");",
              "    }", "  } catch (java.lang.Throwable failure) {"});
  exceptionDispatch("dispatch", "    ");
  Out.extend({"    throw " + throwHelper() + "(failure);", "  }", "}"});
  return Out.text();
}

std::optional<std::string> fieldValue(const FieldValue &Value,
                                      const std::string &Type) {
  if (std::holds_alternative<std::monostate>(Value))
    return std::nullopt;
  if (const auto *Bits = std::get_if<FloatBits>(&Value)) {
    if (!Bits->wide && Type == "F")
      return "((java.lang.Float) null).intBitsToFloat(" +
             hexLiteral(Bits->bits, 32) + ")";
    if (Bits->wide && Type == "D")
      return "((java.lang.Double) null).longBitsToDouble(" +
             hexLiteral(Bits->bits, 64) + ")";
    javaError("unsupported encoded static field value");
  }
  if (const auto *Text = std::get_if<std::string>(&Value);
      Text && Type == "Ljava/lang/String;")
    return javaString(*Text);
  if (Type == "Z") {
    if (const auto *V = std::get_if<bool>(&Value))
      return *V ? "true" : "false";
    if (const auto *V = std::get_if<int64_t>(&Value))
      return *V ? "true" : "false";
  }
  if (const auto *V = std::get_if<int64_t>(&Value);
      V && Strings{"B", "C", "S", "I", "J"}.contains(Type)) {
    auto Text = hexLiteral(static_cast<uint64_t>(*V), Type == "J" ? 64 : 32);
    return Type == "B" || Type == "C" || Type == "S"
               ? "(" + Primitives.at(Type) + ") " + Text
               : Text;
  }
  javaError("static field value disagrees with its declared type");
}

std::vector<std::string> modifiers(const Access &Flags,
                                   std::initializer_list<const char *> Order) {
  std::vector<std::string> Result;
  for (const auto *Flag : Order)
    if (Flags.contains(Flag))
      Result.emplace_back(Flag);
  return Result;
}

} // namespace

llvm::json::Object recoverJava(const ClassMap &Classes, Budget &B) {
  const auto PackageMetadata = planEmptyPackageMetadata(Classes, B);
  const auto SourceScopes = validateSourceScopes(Classes, B);
  const auto &SharedAnonymous = SourceScopes.shared;
  const auto &Enums = SourceScopes.enums;
  const auto AnnotationPlans = validateAnnotations(Classes, B, &Enums);
  const auto LocalNames = planLocalClassNames(Classes, B);
  const auto NamedAnonymous = planNamedAnonymousMembers(Classes, B);
  const auto Generics =
      validateGenericSignatures(Classes, B, &Enums, &SharedAnonymous);
  auto Inheritance = planGenericInheritance(Classes, Generics, B, &Enums);
  bool HasMethodIdentifierProjection =
      planIndependentMethodIdentifiers(Classes, Inheritance, B);
  HasMethodIdentifierProjection |=
      planVirtualMethodIdentifiers(Classes, LocalNames, Generics, Inheritance, B);
  const auto MemberAccess = planMemberPackageAccess(Classes, Generics, B);
  Inheritance.cross_package_private_reads = privateFieldReadsAcrossPackages(Classes, B);
  bool HasFieldIdentifierProjection =
      planCapturedFieldIdentifiers(Classes, Generics, Inheritance, B);
  HasFieldIdentifierProjection |=
      planPrivateFieldIdentifiers(Classes, Inheritance, B);
  auto SuperHelperNames = planStaticSuperHelperNames(Classes, Inheritance, B);
  for (const auto &[ref, target] : Inheritance.interface_default_accessors)
    SuperHelperNames.erase(ref);
  for (const auto &[ref, target] :
       Inheritance.inherited_interface_default_accessors)
    SuperHelperNames.erase(ref);
  Strings InterfaceDefaultOwners;
  for (const auto &[target, helper] : Inheritance.interface_default_helpers)
    InterfaceDefaultOwners.insert(target.owner);
  const auto Anonymous = planAnonymousClasses(Classes, B);
  const auto AnnotationInitializers =
      planAnnotationFieldInitializers(Classes, B);
  const auto StaticWrites = planFieldWrites(Classes, B, true);
  const auto InstanceWrites = planFieldWrites(Classes, B, false);
  llvm::json::Array DefaultInstanceInitializers;
  llvm::json::Array RuntimeFinalBindings;
  std::string RuntimeFinalManifest;
  std::map<std::string, std::string> AnonymousSources;
  std::set<MethodRef> AnonymousMethods;
  for (const auto &[Name, Site] : Anonymous) {
    AnonymousMethods.insert(Site.method);
    for (const auto &M : Classes.at(Name).methods)
      AnonymousMethods.insert(M.reference);
  }
  bool HasGenericMetadata = false;
  for (const auto &[Name, C] : Classes) {
    if (C.external)
      continue;
    HasGenericMetadata |= C.generic_signature.has_value();
    for (const auto &F : C.fields)
      HasGenericMetadata |= F.generic_signature.has_value();
    for (const auto &M : C.methods)
      HasGenericMetadata |= M.generic_signature || M.declared_throws;
  }
  bool HasDeclarationMetadata =
      HasGenericMetadata || !Generics.erased_generic_invocations.empty() ||
      !Generics.raw_receiver_invocations.empty() ||
      !Generics.intersection_invocations.empty() ||
      !Generics.constructor_delegations.empty() ||
      !Generics.constructor_calls.empty() || HasMethodIdentifierProjection ||
      HasFieldIdentifierProjection || !SuperHelperNames.empty() ||
      !Inheritance.interface_default_helpers.empty() ||
      !Inheritance.inherited_interface_default_accessors.empty();
  llvm::json::Array Units, Methods, Bindings, AnonymousBindings, EnumBindings,
      Helpers, DebugMetadata, GenericProjections, PackageMetadataBindings;
  JavaTypeNames TypeNames(Classes, Generics, LocalNames, SharedAnonymous,
                          NamedAnonymous, Enums, B);
  auto genericProjection = [&](const Class &scope, const std::string &Identity,
                               const std::string &Kind,
                               const std::string &Original,
                               const GenericSignature &Signature) {
    auto NameChanges = TypeNames.formalNameProjections(Signature, Identity);
    TypeNames.recordAccessProjections(scope, Signature, Original, NameChanges);
    if (Signature.source_projections.empty() && NameChanges.empty())
      return;
    llvm::json::Array Changes;
    auto AllChanges = Signature.source_projections;
    AllChanges.insert(AllChanges.end(), NameChanges.begin(), NameChanges.end());
    for (const auto &Change : AllChanges) {
      B.tick();
      llvm::json::Object Row{{"offset", static_cast<int64_t>(Change.offset)},
                             {"kind", Change.kind},
                             {"original_type", Change.original_type},
                             {"source_type", Change.source_type}};
      if (Change.scope_origin)
        Row["scope_origin"] = *Change.scope_origin;
      Changes.push_back(std::move(Row));
    }
    GenericProjections.push_back(llvm::json::Object{
        {"declaration", Identity},
        {"declaration_kind", Kind},
        {"original_signature", Original},
        {"status", "source-projected"},
        {"changes", std::move(Changes)},
        {"metadata_differences",
         llvm::json::Array{
             "source generic metadata changes while runtime descriptor "
             "erasures are retained",
             "the changes array records each type substitution, "
             "bound-layout change or scoped type-variable spelling",
             "generic reflection and original language metadata may disagree "
             "with rebuilt source"}}});
  };
  for (const auto &[Name, C] : Classes) {
    if (C.external)
      continue;
    if (auto I = Generics.classes.find(Name); I != Generics.classes.end())
      genericProjection(C, Name, "class", *C.generic_signature, I->second);
    for (const auto &F : C.fields)
      if (auto I = Generics.fields.find(F.reference);
          I != Generics.fields.end())
        genericProjection(C, Name + "->" + F.reference.name + ":" +
                              F.reference.type,
                          "field", *F.generic_signature, I->second);
    for (const auto &M : C.methods) {
      if (auto I = Generics.methods.find(M.reference);
          I != Generics.methods.end()) {
        auto Source = *sourceMethodSignature(M.reference, Generics);
        if (const auto P = Inheritance.erased_void_parameters.find(M.reference);
            P != Inheritance.erased_void_parameters.end()) {
          const auto &Parameter = Source.types.at(Source.parameters.at(0));
          Source.source_projections.push_back(
              {Parameter.offset, "checked-primitive-void-parameter-erasure",
               "T" + Parameter.variable_name + ";", "Ljava/lang/Object;",
               P->second.identity()});
        }
        genericProjection(C, M.reference.identity(), "method",
                          *M.generic_signature, Source);
      }
    }
  }
  std::map<std::string, std::string> documentedContainers;
  for (const auto &[name, repeated] : Classes) {
    if (repeated.external || !has(repeated.access, "annotation") ||
        !repeated.annotation_metadata.documented)
      continue;
    for (const auto &use : repeated.annotations) {
      if (use.type != "Ljava/lang/annotation/Repeatable;" || use.elements.size() != 1)
        continue;
      const auto arg = use.elements.find("value");
      if (arg == use.elements.end() || arg->second.kind != 0x18)
        continue;
      const auto container = Classes.find(std::get<std::string>(arg->second.value));
      if (container == Classes.end() || container->second.external ||
          !has(container->second.access, "annotation") || container->second.annotation_metadata.documented)
        continue;
      if (std::any_of(container->second.methods.begin(), container->second.methods.end(),
                      [&](const Method &m) {
                        return m.reference.name == "value" && m.reference.parameters.empty() &&
                               m.reference.returns == "[" + name && has(m.access, "abstract");
                      }))
        documentedContainers.emplace(container->first, name);
    }
  }
  std::map<std::string, llvm::json::Array> EnumConstantAnnotations;
  uint64_t Recovered = 0, Declared = 0, Projected = 0;
  std::set<MethodRef> emittedLocalMethods;
  unsigned MinimumJavaRelease = 8;
  LocalProjection Projection(Classes, LocalNames, SharedAnonymous,
                             NamedAnonymous, B);
  std::map<std::string, std::vector<const Class *>> Enclosing;
  for (const auto &[Name, C] : Classes) {
    B.tick();
    if (C.external)
      continue;
    if (C.source_debug_extension)
      DebugMetadata.push_back(llvm::json::Object{
          {"class", C.name},
          {"input", C.source_id},
          {"source_debug_extension", *C.source_debug_extension}});
    HasDeclarationMetadata |=
        C.deprecated || C.access.contains("annotation") ||
        !C.annotation_metadata.empty() || !C.marker_annotations.empty() ||
        C.suppress_lint.has_value() || !C.annotations.empty();
    for (const auto &F : C.fields) {
      B.tick();
      // Final-field reconstruction also needs a complete source-entry receipt:
      // its structured regions can use generated throw/constant helpers.
      HasDeclarationMetadata |= F.access.contains("final") || F.deprecated ||
                                F.suppress_lint.has_value() ||
                                !F.annotations.empty();
    }
    for (const auto &M : C.methods) {
      B.tick();
      HasDeclarationMetadata |= M.deprecated || M.suppress_lint.has_value() ||
                                !M.annotations.empty() ||
                                !M.parameter_annotations.empty();
    }
    if (SharedAnonymous.contains(Name))
      Enclosing[SharedAnonymous.at(Name).source_owner].push_back(&C);
    else if (C.enclosing && !Anonymous.contains(Name) &&
             !Enums.constant_classes.contains(Name)) {
      if (*C.enclosing == Name || !Classes.contains(*C.enclosing) ||
          (!isNamedAnonymousMember(C, NamedAnonymous) &&
           (!C.inner_name || C.inner_name->empty())))
        javaError("unresolved or recursive nested class");
      Enclosing[*C.enclosing].push_back(&C);
    }
  }
  for (const auto &[Name, C] : Classes) {
    if (C.external)
      continue;
    std::optional<std::string> Current = Name;
    Strings Seen;
    while (Current) {
      B.tick();
      if (!Seen.insert(*Current).second)
        javaError("recursive nested-class ownership");
      if (Seen.size() > 128)
        javaError("nested-class ownership exceeds the source depth limit");
      const auto &Parent = Classes.at(*Current);
      Current = Parent.enclosing_method
                    ? std::optional<std::string>(Parent.enclosing_method->owner)
                    : Parent.enclosing;
    }
  }
  Strings Emitting, Emitted;
  enum class DeclarationSite {
    TopLevel,
    Member,
    MethodLocal,
    Anonymous,
    EnumConstant
  };
  auto auxiliary = [&](const Class &C, const std::string &Name,
                       const std::string &Prototype, bool Static,
                       const std::string &Unit, const char *Kind) {
    B.tick();
    Helpers.push_back(llvm::json::Object{{"class", C.name},
                                         {"name", Name},
                                         {"prototype", Prototype},
                                         {"static", Static},
                                         {"source_unit", Unit},
                                         {"kind", Kind}});
  };
  std::function<std::string(const Class &, DeclarationSite,
                            const std::string &)>
      emitClass;
  emitClass = [&](const Class &C, DeclarationSite Site,
                  const std::string &Unit) -> std::string {
    B.tick();
    if (!Emitting.insert(C.name).second)
      javaError("recursive nested-class ownership");
    if (!Emitted.insert(C.name).second)
      javaError("Android class ownership emitted a declaration more than once");
    bool Nested = Site == DeclarationSite::Member;
    bool Local = Site == DeclarationSite::MethodLocal;
    const bool Shared = SharedAnonymous.contains(C.name);
    const bool NamedMember = isNamedAnonymousMember(C, NamedAnonymous);
    const bool CapturedInner = Generics.captured_inner_classes.contains(C.name);
    const bool IndependentInner =
        Generics.independent_inner_classes.contains(C.name);
    const bool OrdinaryStaticOnlyInner =
        Nested && Projection.ordinary_static_only_members.contains(C.name);
    const bool StaticOnlyInner =
        isStaticOnlyInnerMember(C) &&
        (Projection.member_classes.contains(C.name) || OrdinaryStaticOnlyInner);
    const bool Enum = Enums.classes.contains(C.name);
    const bool EnumConstantSite = Site == DeclarationSite::EnumConstant;
    bool AnonymousSite = Site == DeclarationSite::Anonymous || EnumConstantSite;
    const bool LocalStaticMembers =
        Local &&
        (std::any_of(
             C.methods.begin(), C.methods.end(),
             [](const Method &M) { return M.access.contains("static"); }) ||
         std::any_of(
             C.fields.begin(), C.fields.end(),
             [](const Field &F) { return F.access.contains("static"); }) ||
         (Enclosing.contains(C.name) &&
          std::any_of(Enclosing.at(C.name).begin(), Enclosing.at(C.name).end(),
                      [&NamedAnonymous, &Generics](const Class *Child) {
                        return Child->inner_access.contains("static") ||
                               Generics.captured_inner_classes.contains(
                                   Child->name) ||
                               isNamedAnonymousMember(*Child, NamedAnonymous);
                      })));
    unsigned ClassJavaRelease = LocalStaticMembers || StaticOnlyInner ? 16 : 8;
    if (InterfaceDefaultOwners.contains(C.name))
      ClassJavaRelease = std::max(ClassJavaRelease, 9u);
    if (C.annotation_metadata.targets)
      for (const auto &Target : *C.annotation_metadata.targets)
        ClassJavaRelease =
            std::max(ClassJavaRelease, annotationTargetJavaRelease(Target));
    MinimumJavaRelease = std::max(MinimumJavaRelease, ClassJavaRelease);
    const bool StaticHelpers = (!Local && !AnonymousSite) || LocalStaticMembers;
    bool Annotation = C.access.contains("annotation");
    if (C.access.contains("enum") && !Enum && !EnumConstantSite)
      javaError("unsupported Java declaration shape: " + C.name);
    const GenericSignature *ClassSignature = nullptr;
    if (auto I = Generics.classes.find(C.name); I != Generics.classes.end()) {
      ClassSignature = &I->second;
      if (!ClassSignature->type_parameters.empty()) {
        auto Parent = C.superclass;
        Strings Seen;
        while (Parent) {
          B.tick();
          if (!Seen.insert(*Parent).second)
            javaError("generic class has cyclic superclass ancestry: " +
                      C.name);
          if (Strings{"Ljava/lang/Throwable;", "Ljava/lang/Exception;",
                      "Ljava/lang/RuntimeException;", "Ljava/lang/Error;"}
                  .contains(*Parent))
            javaError("generic class cannot extend Throwable: " + C.name);
          auto Base = Classes.find(*Parent);
          Parent =
              Base == Classes.end() ? std::nullopt : Base->second.superclass;
        }
      }
    }
    const auto &Flags =
        Nested && !Shared && !NamedMember ? C.inner_access : C.access;
    if (Nested && !Shared && !NamedMember && !StaticOnlyInner &&
        !CapturedInner && !IndependentInner && !Flags.contains("static"))
      javaError("non-static inner class requires an outer-instance "
                "initialization proof: " +
                C.name);
    auto Slash = C.name.rfind('/');
    std::string Name;
    if (Local || Shared || NamedMember)
      Name = javaTypeIdentifier(LocalNames.at(C.name));
    else if (Nested)
      Name = javaTypeIdentifier(*C.inner_name);
    else
      Name = javaTypeIdentifier(C.name.substr(
          Slash == std::string::npos ? 1 : Slash + 1,
          C.name.size() - (Slash == std::string::npos ? 1 : Slash + 1) - 1));
    if (Name == "java")
      javaError("class name shadows the required Java runtime package");
    auto Mods = modifiers(Flags, {"public", "protected", "private", "static",
                                  "abstract", "final", "strictfp"});
    if (Local)
      Mods = modifiers(Flags, {"final"});
    if (const auto entry = MemberAccess.find(C.name); entry != MemberAccess.end()) {
      std::erase(Mods, "private");
      if (entry->second.interface_member)
        std::erase(Mods, "protected"); // Java interface member types are implicitly public.
    }
    if ((Shared || NamedMember || CapturedInner || IndependentInner) &&
        !Flags.contains("static"))
      Mods.push_back("static");
    if (IndependentInner && C.access.contains("final") &&
        !Flags.contains("final"))
      Mods.push_back("final");
    if (!Nested)
      std::erase(Mods, "static");
    if (Annotation)
      std::erase(Mods, "abstract");
    if (Enum) {
      std::erase(Mods, "abstract");
      std::erase(Mods, "final");
    }
    std::string Kind = Annotation                       ? "@interface"
                       : Enum                           ? "enum"
                       : C.access.contains("interface") ? "interface"
                                                        : "class";
    Mods.push_back(Kind);
    Mods.push_back(Name +
                   (ClassSignature
                        ? TypeNames.formals(*ClassSignature, JavaScope{C}, true)
                        : ""));
    std::string Header = join(Mods, " ");
    if (Kind == "class" && C.superclass &&
        *C.superclass != "Ljava/lang/Object;")
      Header += " extends " +
                (ClassSignature ? TypeNames.render(*ClassSignature,
                                                   *ClassSignature->superclass,
                                                   JavaScope{C}, true)
                                : TypeNames.render(*C.superclass, C, true));
    if (!Annotation && !C.interfaces.empty()) {
      std::vector<std::string> Interfaces;
      for (size_t I = 0; I < C.interfaces.size(); ++I)
        Interfaces.push_back(
            ClassSignature ? TypeNames.render(*ClassSignature,
                                              ClassSignature->interfaces.at(I),
                                              JavaScope{C}, true)
                           : TypeNames.render(C.interfaces[I], C, true));
      Header += (Kind == "interface" ? " extends " : " implements ") +
                join(Interfaces, ", ");
    }
    if (Site == DeclarationSite::Anonymous) {
      if (ClassSignature && !ClassSignature->type_parameters.empty())
        javaError("anonymous class cannot declare source type parameters");
      Header =
          "new " +
          (ClassSignature ? TypeNames.render(*ClassSignature,
                                             ClassSignature->interfaces.at(0),
                                             JavaScope{C}, true)
                          : TypeNames.render(C.interfaces.at(0), C, true)) +
          "()";
    }
    if (EnumConstantSite)
      Header.clear();
    Lines BodyLines(B, true);
    const JavaScope ModifierScope{C, nullptr,
                                  JavaScope::Position::ClassModifiers};
    auto annotationType = [&](const std::string &Type) {
      return "@" + TypeNames.render(Type, ModifierScope);
    };
    auto suppressLint = [&](const std::vector<std::string> &Values,
                            const JavaScope &Scope) {
      std::vector<std::string> Strings;
      for (const auto &Value : Values) {
        B.tick(1 + Value.size() / 16);
        Strings.push_back(javaString(Value));
      }
      return "@" +
             TypeNames.render("Landroid/annotation/SuppressLint;", Scope) +
             "({" + join(Strings, ", ") + "})";
    };
    std::function<std::string(const AnnotationValue &, const JavaScope &)>
        annotationValue;
    auto annotationUse =
        [&](const std::string &Type,
            const std::map<std::string, AnnotationValue> &Elements,
            const JavaScope &Scope) {
          std::vector<std::string> Values;
          for (const auto &[Key, Value] : Elements) {
            B.tick();
            Values.push_back(javaIdentifier(Key) + " = " +
                             annotationValue(Value, Scope));
          }
          return "@" + TypeNames.render(Type, Scope) +
                 (Values.empty() ? "" : "(" + join(Values, ", ") + ")");
        };
    annotationValue = [&](const AnnotationValue &Value,
                          const JavaScope &Scope) -> std::string {
      B.tick();
      if (Value.kind == 0x1c) {
        std::vector<std::string> Values;
        for (const auto &Element : Value.array)
          Values.push_back(annotationValue(Element, Scope));
        return "{" + join(Values, ", ") + "}";
      }
      if (Value.kind == 0x1d)
        return annotationUse(Value.annotation_type, Value.elements, Scope);
      if (Value.kind == 0x17)
        return javaString(std::get<std::string>(Value.value));
      if (Value.kind == 0x18)
        return TypeNames.render(std::get<std::string>(Value.value), Scope) +
               ".class";
      if (Value.kind == 0x1b) {
        auto Ref = fieldRef(std::get<std::string>(Value.value));
        return TypeNames.render(Ref.owner, Scope) + "." +
               sourceFieldName(Ref, Classes, Inheritance, B);
      }
      if (Value.kind == 0x1f)
        return std::get<bool>(Value.value) ? "true" : "false";
      if (Value.kind == 0x10 || Value.kind == 0x11) {
        const auto &Bits = std::get<FloatBits>(Value.value);
        double Number = Bits.wide ? std::bit_cast<double>(Bits.bits)
                                  : std::bit_cast<float>(uint32_t(Bits.bits));
        std::string Suffix = Bits.wide ? "d" : "f";
        if (std::isnan(Number)) {
          if (Bits.bits !=
              (Bits.wide ? UINT64_C(0x7ff8000000000000) : UINT64_C(0x7fc00000)))
            javaError("annotation NaN payload has no Java constant spelling: " +
                      C.name);
          return "(0.0" + Suffix + " / 0.0" + Suffix + ")";
        }
        if (std::isinf(Number))
          return std::string(std::signbit(Number) ? "(-1.0" : "(1.0") + Suffix +
                 " / 0.0" + Suffix + ")";
        std::ostringstream Text;
        Text << std::hexfloat << Number << Suffix;
        return Text.str();
      }
      auto Number = std::get<int64_t>(Value.value);
      auto Text =
          hexLiteral(static_cast<uint64_t>(Number), Value.kind == 6 ? 64 : 32);
      if (Value.kind == 0 || Value.kind == 2 || Value.kind == 3)
        Text = std::string(Value.kind == 0   ? "(byte) "
                           : Value.kind == 2 ? "(short) "
                                             : "(char) ") +
               Text;
      return Text;
    };
    const auto &Metadata = C.annotation_metadata;
    if (Metadata.retention || (Metadata.targets && !Metadata.targets->empty()))
      TypeNames.requireAnnotationEnumQualifier(C);
    if (Metadata.retention)
      BodyLines.append(
          annotationType("Ljava/lang/annotation/Retention;") + "(" +
          TypeNames.render("Ljava/lang/annotation/RetentionPolicy;",
                           ModifierScope) +
          "." + *Metadata.retention + ")");
    if (Metadata.targets) {
      std::vector<std::string> Values;
      for (const auto &Target : *Metadata.targets) {
        B.tick();
        Values.push_back(TypeNames.render("Ljava/lang/annotation/ElementType;",
                                          ModifierScope) +
                         "." + Target);
      }
      BodyLines.append(annotationType("Ljava/lang/annotation/Target;") + "({" +
                       join(Values, ", ") + "})");
    }
    if (Metadata.documented || documentedContainers.contains(C.name))
      BodyLines.append(annotationType("Ljava/lang/annotation/Documented;"));
    if (Metadata.inherited)
      BodyLines.append(annotationType("Ljava/lang/annotation/Inherited;"));
    for (const auto &Use : C.marker_annotations) {
      B.tick();
      BodyLines.append(annotationType(Use.type));
    }
    if (C.deprecated)
      BodyLines.append(
          "@" +
          TypeNames.render(
              "Ljava/lang/Deprecated;",
              JavaScope{C, nullptr, JavaScope::Position::ClassModifiers}));
    if (C.suppress_lint)
      BodyLines.append(suppressLint(*C.suppress_lint, ModifierScope));
    for (const auto &Use : C.annotations) {
      auto Source = annotationUse(Use.type, Use.elements, ModifierScope);
      if (EnumConstantSite)
        EnumConstantAnnotations[C.name].push_back(
            llvm::json::Object{{"type", Use.type},
                               {"visibility", Use.visibility},
                               {"source_declaration", std::move(Source)}});
      else
        BodyLines.append(Source);
    }
    if (((Local || Shared) && isAnonymousLocal(C)) || NamedMember)
      BodyLines.append("// Named source projection of an anonymous class; see "
                       "class_source_bindings.");
    BodyLines.append(Header.empty() ? "{" : Header + " {");
    if (Site == DeclarationSite::Anonymous) {
      const auto &Site = Anonymous.at(C.name);
      AnonymousBindings.push_back(
          llvm::json::Object{{"class", C.name},
                             {"input", C.source_id},
                             {"source_unit", Unit},
                             {"binding_kind", "anonymous-interface"},
                             {"enclosing_class", *C.enclosing},
                             {"allocation_method", Site.method.identity()},
                             {"allocation_pc", Site.allocation_pc},
                             {"binary_name_status", "unverified"}});
    }
    if (Enum) {
      const auto &Plan = Enums.classes.at(C.name);
      llvm::json::Array Constants;
      for (size_t I = 0; I < Plan.constants.size(); ++I) {
        const auto &Constant = Plan.constants[I];
        std::vector<std::string> Args;
        for (size_t N = 0; N < Constant.arguments.size(); ++N) {
          const auto &Type = Constant.constructor.parameters.at(N + 2);
          const auto &Value = Constant.arguments[N];
          std::string Expression;
          if (Constant.argument_helpers.at(N))
            Expression =
                sourceMethodName(Constant.argument_helpers[N]->reference,
                                 Inheritance) +
                "()";
          else if (const auto *String = std::get_if<std::string>(&Value))
            Expression = javaString(*String);
          else if (isReference(Type))
            Expression = "null";
          else {
            const auto Number = std::get<int64_t>(Value);
            FieldValue Bits = Number;
            if (Type == "F" || Type == "D")
              Bits = FloatBits{uint64_t(Number), Type == "D"};
            Expression = *fieldValue(Bits, Type);
          }
          Args.push_back("(" + TypeNames.render(Type, C) + ") (" + Expression +
                         ")");
        }
        const auto &Field =
            *std::find_if(C.fields.begin(), C.fields.end(), [&](const auto &F) {
              return F.reference == Constant.field;
            });
        if (Field.deprecated)
          BodyLines.append("  @" +
                           TypeNames.render("Ljava/lang/Deprecated;", C));
        if (Field.suppress_lint)
          BodyLines.append("  " +
                           suppressLint(*Field.suppress_lint, JavaScope{C}));
        llvm::json::Array ReportAnnotations;
        for (const auto &Use : Field.annotations) {
          auto Declaration =
              annotationUse(Use.type, Use.elements, JavaScope{C});
          auto Projection = AnnotationPlans.find(Field.reference);
          if (Projection != AnnotationPlans.end() &&
              Projection->second.contains(Use.type))
            ReportAnnotations.push_back(llvm::json::Object{
                {"type", Use.type},
                {"visibility", Use.visibility},
                {"source_declaration", std::move(Declaration)}});
          else
            BodyLines.append("  " + Declaration);
        }
        auto Source = sourceFieldName(Constant.field, Classes, Inheritance, B) +
                      "(" + join(Args, ", ") + ")";
        if (Constant.implementation != C.name) {
          const auto &Implementation = Classes.at(Constant.implementation);
          Source += " " + emitClass(Implementation,
                                    DeclarationSite::EnumConstant, Unit);
          llvm::json::Object Binding{
              {"class", Constant.implementation},
              {"input", Implementation.source_id},
              {"source_unit", Unit},
              {"binding_kind", "enum-constant-body"},
              {"enclosing_class", C.name},
              {"constant", Constant.field.name},
              {"ordinal", int64_t(I)},
              {"binary_name_status",
               Implementation.inner_name ? "changed" : "unverified"},
              {"constructor_descriptor_status", "unverified"},
              {"metadata_differences",
               llvm::json::Array{"the compiler supplies the constant subclass "
                                 "and its implicit constructor"}}};
          if (Implementation.inner_name)
            Binding.getArray("metadata_differences")
                ->push_back("the original named member becomes an anonymous "
                            "enum constant class; "
                            "binary and simple names, declaring-class and "
                            "member/anonymous reflection metadata change");
          if (auto Annotations =
                  EnumConstantAnnotations.find(Implementation.name);
              Annotations != EnumConstantAnnotations.end()) {
            Binding["original_annotations"] = std::move(Annotations->second);
            Binding["class_annotation_status"] = "report-only";
            Binding.getArray("metadata_differences")
                ->push_back("class annotations have no declaration site on a "
                            "Java enum constant body; "
                            "original declarations and DEX visibility are "
                            "retained only in this report");
          }
          EnumBindings.push_back(std::move(Binding));
        }
        for (const auto &Line :
             split(Source + (I + 1 == Plan.constants.size() ? ";" : ","), '\n'))
          BodyLines.append("  " + Line);
        llvm::json::Object ConstantBinding{
            {"field", Constant.field.name},
            {"ordinal", int64_t(I)},
            {"implementation", Constant.implementation},
            {"constructor", Constant.constructor.identity()}};
        if (!ReportAnnotations.empty()) {
          ConstantBinding["annotation_status"] = "report-only";
          ConstantBinding["original_annotations"] =
              std::move(ReportAnnotations);
          ConstantBinding["metadata_differences"] = llvm::json::Array{
              "declared Java annotation targets exclude enum constant fields; "
              "these original annotation values and DEX visibility are "
              "retained "
              "only in this report, so reflection metadata changes"};
        }
        Constants.push_back(std::move(ConstantBinding));
      }
      if (Plan.constants.empty())
        BodyLines.append("  ;");
      EnumBindings.push_back(llvm::json::Object{
          {"class", C.name},
          {"source_unit", Unit},
          {"binding_kind", "enum-declaration"},
          {"constants", std::move(Constants)},
          {"values_field", Plan.values_field.name},
          {"binary_name_status", aliasedEnclosure(C, Classes, LocalNames, B)
                                     ? "changed"
                                     : "preserved"},
          {"compiler_generated_members_status", "unverified"},
          {"metadata_differences",
           llvm::json::Array{"compiler selects constant-subclass names, access "
                             "bridges and backing-array details",
                             "verified name and ordinal parameters are "
                             "supplied by enum declarations"}}});
      if (Plan.initializer_tail) {
        auto &Binding = *EnumBindings.back().getAsObject();
        Binding["backing_array_projection"] = "verified-read-only-copy";
        Binding.getArray("metadata_differences")
            ->push_back("an explicit read-only backing copy retains typed "
                        "consumers; private array identity and synthetic flags "
                        "may differ under reflection");
      }
    }
    if (Local || Shared) {
      const auto &R = *C.enclosing_method;
      llvm::json::Array Parameters;
      for (const auto &T : R.parameters) {
        B.tick();
        Parameters.push_back(T);
      }
      llvm::json::Object EnclosingMethod{{"identity", R.identity()},
                                         {"owner", R.owner},
                                         {"name", R.name},
                                         {"prototype", R.signature()},
                                         {"parameters", std::move(Parameters)},
                                         {"returns", R.returns}};
      llvm::json::Object Binding{
          {"class", C.name},
          {"input", C.source_id},
          {"enclosing_method", std::move(EnclosingMethod)},
          {"source_unit", Unit},
          {"source_name", Name},
          {"binding_kind", "named-method-local"},
          {"binary_name_status", "unverified"}};
      if (isAnonymousLocal(C)) {
        Binding["binding_kind"] = "anonymous-to-named-local";
        Binding["binary_name_status"] = "changed";
        Binding["enclosing_kind"] =
            R.name == "<init>" ? "constructor" : "method";
        if (ClassJavaRelease > 8)
          Binding["minimum_java_release"] = ClassJavaRelease;
        Binding["metadata_differences"] = llvm::json::Array{
            "anonymous class becomes a named local class",
            "public/protected/private/static class and InnerClass flags cannot "
            "be retained on a named local declaration",
            "binary and simple names change",
            "isAnonymousClass/isLocalClass reflection changes",
            "compiler-generated enclosing-instance fields and constructor "
            "parameters may differ",
            "original language metadata is retained but names may no longer "
            "match"};
        for (const auto &Field : C.fields)
          if (!has(Field.access, "static") && has(Field.access, "synthetic")) {
            Binding.getArray("metadata_differences")
                ->push_back("explicit captured fields retain their descriptors "
                            "but Java source cannot reproduce their synthetic "
                            "flags");
            break;
          }
        for (const auto &Constructor : C.methods)
          if (Constructor.reference.name == "<init>" &&
              Constructor.generic_signature &&
              Generics.parameter_indices.contains(Constructor.reference)) {
            Binding.getArray("metadata_differences")
                ->push_back(
                    "constructor Signature metadata may change when "
                    "implicit capture parameters become explicit source "
                    "parameters");
            break;
          }
        if (Shared) {
          Binding["binding_kind"] = "anonymous-to-shared-member";
          Binding["source_enclosing_class"] =
              SharedAnonymous.at(C.name).source_owner;
          if (SharedAnonymous.at(C.name).reference_owner !=
              SharedAnonymous.at(C.name).source_owner)
            Binding["source_reference_class"] =
                SharedAnonymous.at(C.name).reference_owner;
          Binding["metadata_differences"] = llvm::json::Array{
              "anonymous class becomes one named static member class",
              "binary and simple names change",
              "getEnclosingClass/getDeclaringClass refer to the source "
              "enclosing class",
              "getEnclosingMethod/getEnclosingConstructor become null",
              "isAnonymousClass/isLocalClass become false; isMemberClass "
              "becomes true",
              "original language metadata is retained but names may no longer "
              "match"};
        }
      }
      const auto Alias = Inheritance.source_method_names.find(R);
      const auto DefaultHelper = Inheritance.interface_default_helpers.find(R);
      const bool MovedDefault =
          !Shared &&
          DefaultHelper != Inheritance.interface_default_helpers.end();
      if (MovedDefault ||
          (Alias != Inheritance.source_method_names.end() &&
           Alias->second.identifier_only && Alias->second.name != R.name)) {
        auto Source = R;
        Source.name = MovedDefault ? DefaultHelper->second : Alias->second.name;
        llvm::json::Array Parameters;
        for (const auto &Type : Source.parameters)
          Parameters.push_back(Type);
        Binding["source_enclosing_method"] =
            llvm::json::Object{{"identity", Source.identity()},
                               {"owner", Source.owner},
                               {"name", Source.name},
                               {"prototype", Source.signature()},
                               {"parameters", std::move(Parameters)},
                               {"returns", Source.returns}};
        if (!Binding.getArray("metadata_differences"))
          Binding["metadata_differences"] = llvm::json::Array{};
        Binding.getArray("metadata_differences")
            ->push_back(Shared
                            ? "the original enclosing method has a disclosed "
                              "source name; this shared member has no source "
                              "EnclosingMethod attribute"
                            : "EnclosingMethod names the disclosed source "
                              "method; its original runtime name cannot be "
                              "regenerated");
      }
      Bindings.push_back(std::move(Binding));
    }
    if (NamedMember) {
      llvm::json::Object Binding{
          {"class", C.name},
          {"input", C.source_id},
          {"source_unit", Unit},
          {"source_name", Name},
          {"source_enclosing_class", *C.enclosing},
          {"binding_kind", "anonymous-to-named-member"},
          {"binary_name_status", "changed"},
          {"metadata_differences",
           llvm::json::Array{"anonymous class becomes a named static member "
                             "with explicit captures",
                             "binary and simple names change",
                             "isAnonymousClass becomes false; isMemberClass "
                             "becomes true; the source member is static",
                             "original language metadata is retained but names "
                             "may no longer match"}}};
      if (Generics.stateless_anonymous_members.contains(C.name))
        Binding.getArray("metadata_differences")
            ->push_back("the original class-only EnclosingMethod attribute is "
                        "omitted on the named static source member");
      if (isConstructorlessAnonymousMember(C))
        Binding.getArray("metadata_differences")
            ->push_back(
                "a private source constructor is added for the constructorless "
                "class and is listed only as a generated helper");
      Bindings.push_back(std::move(Binding));
    }
    if (IndependentInner) {
      Bindings.push_back(llvm::json::Object{
          {"class", C.name},
          {"input", C.source_id},
          {"source_unit", Unit},
          {"source_name", Name},
          {"source_enclosing_class", *C.enclosing},
          {"binding_kind", "independent-synthetic-member"},
          {"binary_name_status", "preserved"},
          {"metadata_differences",
           llvm::json::Array{"the independent synthetic member becomes a "
                             "static source member",
                             "class and InnerClass synthetic flags cannot be "
                             "expressed by Java source",
                             "original class finality, constructors and method "
                             "descriptors are retained",
                             "original language metadata is retained but may "
                             "describe the original member flags"}}});
    }
    if (CapturedInner) {
      const auto &Capture = Generics.captured_inner_classes.at(C.name);
      Bindings.push_back(llvm::json::Object{
          {"class", C.name},
          {"input", C.source_id},
          {"source_unit", Unit},
          {"source_name", Name},
          {"source_enclosing_class", *C.enclosing},
          {"binding_kind", "inner-to-static-member"},
          {"binary_name_status", "unverified"},
          {"outer_capture_field",
           Capture.owner + "->" + Capture.name + ":" + Capture.type},
          {"metadata_differences",
           llvm::json::Array{
               "instance member becomes a static member with the original "
               "explicit outer parameter and field",
               "member modifiers and synthetic capture-field reflection change",
               "constructor generic signatures and implicit parameter metadata "
               "may change",
               "original language metadata is retained but may describe the "
               "original instance member"}}});
      if (const auto Access = MemberAccess.find(C.name);
          Access != MemberAccess.end()) {
        auto &Binding = *Bindings.back().getAsObject();
        Binding["source_visibility"] = "package";
        llvm::json::Array Callers;
        for (const auto &Caller : Access->second.callers)
          Callers.push_back(Caller.identity());
        Binding["package_access_callers"] = std::move(Callers);
        Binding.getArray("metadata_differences")
            ->push_back(
                "the private InnerClasses annotation becomes package "
                "source access for original same-package method callers; "
                "runtime class access and member access remain unchanged");
      }
      if (std::any_of(C.methods.begin(), C.methods.end(), [](const auto &M) {
            return M.reference.name == "<init>" && has(M.access, "synthetic");
          }))
        Bindings.back()
            .getAsObject()
            ->getArray("metadata_differences")
            ->push_back("source constructors cannot retain original synthetic "
                        "flags");
    }
    if (Projection.member_classes.contains(C.name)) {
      llvm::json::Object Binding{
          {"class", C.name},
          {"input", C.source_id},
          {"source_unit", Unit},
          {"source_name", Name},
          {"source_enclosing_class", *C.enclosing},
          {"binding_kind", "member-of-projected-class"},
          {"binary_name_status", "changed"},
          {"metadata_differences",
           llvm::json::Array{
               "member binary name follows its enclosing source projection"}}};
      if (StaticOnlyInner) {
        Binding["minimum_java_release"] = 16;
        Binding.getArray("metadata_differences")
            ->push_back(
                "source compiler supplies a private constructor and may add an "
                "enclosing-instance field; original non-static member flags "
                "are retained");
      }
      Bindings.push_back(std::move(Binding));
    } else if (OrdinaryStaticOnlyInner) {
      // No original instance state, constructor or method needs an outer
      // identity. Retain the non-static declaration and all static effects;
      // disclose the compiler-only instance roles instead of inventing an
      // original outer capture or changing the member to static.
      Bindings.push_back(llvm::json::Object{
          {"class", C.name},
          {"input", C.source_id},
          {"source_unit", Unit},
          {"source_name", Name},
          {"source_enclosing_class", *C.enclosing},
          {"binding_kind", "static-only-inner-member"},
          {"binary_name_status", "preserved"},
          {"minimum_java_release", 16},
          {"metadata_differences",
           llvm::json::Array{
               "source compiler supplies a private constructor with an "
               "implicit enclosing-instance parameter and may add its field",
               "original non-static InnerClass flags are retained; binary "
               "class modifiers follow those lexical declaration flags",
               "source declarations cannot retain original synthetic flags"}}});
    }
    Strings MemberNames, ConstantTypes;
    auto Initializations =
        planInitializations(C, Classes, B, Enums, Generics, StaticWrites,
                            InstanceWrites, NamedAnonymous);
    auto AnnotationInitializer =
        AnnotationInitializers.find(MethodRef{C.name, "<clinit>", {}, "V"});
    std::vector<std::pair<FieldRef, std::string>> InterfaceInitializers;
    if (Kind == "interface")
      for (const auto &M : C.methods)
        if (M.reference.name == "<clinit>") {
          Body Emitter(M, Classes, B, TypeNames, Generics, Projection,
                       Inheritance, AnonymousSources,
                       Initializations.writes[M.reference]);
          InterfaceInitializers = Emitter.interfaceInitializers();
        }

    std::vector<const Field *> Fields;
    for (const auto &F : C.fields)
      Fields.push_back(&F);
    if (AnnotationInitializer != AnnotationInitializers.end())
      // Encoded static values exist before the original class initializer.
      // In particular, its source class may read them during an initialization
      // cycle.
      std::stable_partition(Fields.begin(), Fields.end(), [&](const Field *F) {
        return F->reference != AnnotationInitializer->second.target;
      });
    if (!InterfaceInitializers.empty()) {
      // Encoded values precede the original clinit; blank finals follow its
      // write order, which need not match the DEX field table or source name.
      auto rank = [&](const Field *F) {
        auto I = std::find_if(
            InterfaceInitializers.begin(), InterfaceInitializers.end(),
            [&](const auto &Item) { return Item.first == F->reference; });
        return I == InterfaceInitializers.end()
                   ? size_t(0)
                   : size_t(I - InterfaceInitializers.begin()) + 1;
      };
      std::stable_sort(
          Fields.begin(), Fields.end(),
          [&](const Field *A, const Field *B) { return rank(A) < rank(B); });
    }
    bool HasStaticFieldInitializer = false;
    bool HasClassInitializer = false;
    for (const auto *Field : Fields) {
      const auto &F = *Field;
      B.tick();
      if (!MemberNames.insert(F.reference.name).second)
        javaError("Java cannot represent field names overloaded only by type");
      const bool EnumArrayAlias =
          Enum && Enums.classes.at(C.name).initializer_tail &&
          F.reference == Enums.classes.at(C.name).values_field;
      if (Enums.managed_fields.contains(F.reference) && !EnumArrayAlias)
        continue;
      auto FieldMods =
          modifiers(F.access, {"public", "protected", "private", "static",
                               "final", "volatile", "transient"});
      if (Inheritance.cross_package_private_reads.contains(F.reference)) {
        if (Initializations.runtime_final_fields.contains(F.reference))
          javaError("field requires conflicting visibility and final projections");
        const auto binary = TypeNames.memberBinaryName(C);
        const auto name = sourceFieldName(F.reference, Classes, Inheritance, B);
        std::replace(FieldMods.begin(), FieldMods.end(), std::string("private"),
                     std::string("public"));
        RuntimeFinalManifest += binary + "\t" + name + "\t" + F.reference.type +
                                "\tprivate\n";
        llvm::json::Array original, source;
        for (const auto &flag : F.access) {
          original.push_back(flag);
          source.push_back(flag == "private" ? "public" : flag);
        }
        RuntimeFinalBindings.push_back(llvm::json::Object{
            {"class", C.name}, {"original_name", F.reference.name},
            {"original_identity", C.name + "->" + F.reference.name + ":" + F.reference.type},
            {"source_identity", "L" + binary + ";->" + name + ":" + F.reference.type},
            {"source_name", name}, {"type", F.reference.type},
            {"original_access", std::move(original)}, {"source_access", std::move(source)},
            {"binding_kind", "cross-package-private-field-read"},
            {"required_postcompile_flags", "private"},
            {"metadata_differences", llvm::json::Array{
                "Java source makes the field public to retain its original read instruction",
                "the rebuild utility restores private access before class loading"}}});
      }
      if (Initializations.runtime_final_fields.contains(F.reference)) {
        std::erase(FieldMods, "final");
        const auto SourceName =
            sourceFieldName(F.reference, Classes, Inheritance, B);
        const auto ClassName = C.name.substr(1, C.name.size() - 2);
        const auto Flags =
            has(F.access, "synthetic") ? "final,synthetic" : "final";
        RuntimeFinalManifest += ClassName + "\t" + SourceName + "\t" +
                                F.reference.type + "\t" + Flags + "\n";
        llvm::json::Array OriginalFlags, SourceFlags;
        for (const auto &Flag : F.access) {
          OriginalFlags.push_back(Flag);
          if (Flag != "final" && Flag != "synthetic")
            SourceFlags.push_back(Flag);
        }
        RuntimeFinalBindings.push_back(llvm::json::Object{
            {"class", C.name},
            {"original_identity",
             C.name + "->" + F.reference.name + ":" + F.reference.type},
            {"source_identity",
             C.name + "->" + SourceName + ":" + F.reference.type},
            {"original_access", std::move(OriginalFlags)},
            {"source_access", std::move(SourceFlags)},
            {"original_name", F.reference.name},
            {"source_name", SourceName},
            {"type", F.reference.type},
            {"binding_kind", "constructor-final-field"},
            {"required_postcompile_flags", Flags},
            {"metadata_differences",
             llvm::json::Array{
                 "Java source omits final to retain the original constructor "
                 "chain and stores",
                 "the emitted post-javac utility must restore original final "
                 "and synthetic flags before application loading"}}});
      }
      if (NamedMember && isConstructorlessAnonymousMember(C) &&
          has(F.access, "static") && has(F.access, "final") &&
          has(F.access, "synthetic")) {
        // Keep the original static initializer and final store in Java. Only
        // the unspellable synthetic bit needs restoration after compilation,
        // against the exact named member's compiler binary identity.
        const auto ClassName = TypeNames.memberBinaryName(C);
        const auto SourceName =
            sourceFieldName(F.reference, Classes, Inheritance, B);
        RuntimeFinalManifest += ClassName + "\t" + SourceName + "\t" +
                                F.reference.type + "\tfinal,synthetic\n";
        llvm::json::Array OriginalFlags, SourceFlags;
        for (const auto &Flag : F.access) {
          OriginalFlags.push_back(Flag);
          if (Flag != "synthetic")
            SourceFlags.push_back(Flag);
        }
        RuntimeFinalBindings.push_back(llvm::json::Object{
            {"class", C.name},
            {"original_identity",
             C.name + "->" + F.reference.name + ":" + F.reference.type},
            {"source_identity",
             "L" + ClassName + ";->" + SourceName + ":" + F.reference.type},
            {"original_access", std::move(OriginalFlags)},
            {"source_access", std::move(SourceFlags)},
            {"original_name", F.reference.name},
            {"source_name", SourceName},
            {"type", F.reference.type},
            {"binding_kind", "static-synthetic-field"},
            {"required_postcompile_flags", "final,synthetic"},
            {"metadata_differences",
             llvm::json::Array{
                 "Java source retains the original static final field and "
                 "initializer but cannot express its synthetic flag",
                 "the emitted post-javac utility must restore the synthetic "
                 "flag before application loading"}}});
      }
      if (std::holds_alternative<FloatBits>(F.value))
        TypeNames.requireRuntimePackage(C);
      auto Value = fieldValue(F.value, F.reference.type);
      const bool DefaultStatic =
          Initializations.default_statics.contains(F.reference);
      const bool DefaultInstance =
          Initializations.default_instances.contains(F.reference);
      if (Initializations.fields.contains(F.reference) && !DefaultStatic &&
          !DefaultInstance)
        // The common write proof makes this a blank final. Explicit encoded
        // zero/null has the same preparation value before that original write.
        Value.reset();
      if (DefaultInstance) {
        const auto Zero = allocationDefaultLiteral(F.reference.type);
        if (!Zero)
          javaError("unsupported default instance field type: " +
                    F.reference.type);
        // Retain real getfield reads after reflective mutation. The null
        // comparison is effect-free but is not a Java constant expression.
        Value = "(null == null ? " + *Zero + " : " + *Zero + ")";
        DefaultInstanceInitializers.push_back(llvm::json::Object{
            {"class", C.name},
            {"field", F.reference.name},
            {"type", F.reference.type},
            {"source_unit", Unit},
            {"original_write_count", 0},
            {"source_initializer", *Value},
            {"initialization_point",
             "after-Object-constructor-before-original-body"},
            {"ConstantValue", "absent"}});
      }
      if (Initializations.pre_super_defaults.contains(F.reference)) {
        const auto Zero = allocationDefaultLiteral(F.reference.type);
        if (!Zero)
          javaError("unsupported default instance field type: " +
                    F.reference.type);
        DefaultInstanceInitializers.push_back(llvm::json::Object{
            {"class", C.name},
            {"field", F.reference.name},
            {"type", F.reference.type},
            {"source_unit", Unit},
            {"original_write_count", 0},
            {"source_initializer", *Zero},
            {"initialization_point",
             "before-super-constructor-before-original-body"},
            {"ConstantValue", "absent"}});
      }
      if (DefaultStatic) {
        const auto &Type = F.reference.type;
        const auto Zero = allocationDefaultLiteral(Type);
        if (!Zero)
          javaError("unsupported default static field type: " + Type);
        // A null comparison is effect-free but is not a Java constant
        // expression. Preserve getstatic initialization and ConstantValue
        // absence without adding a helper, allocation or runtime call.
        Value = "(null == null ? " + *Zero + " : " + *Zero + ")";
      }
      const auto InterfaceInitializer = std::find_if(
          InterfaceInitializers.begin(), InterfaceInitializers.end(),
          [&](const auto &Item) { return Item.first == F.reference; });
      const bool InterfaceValue =
          InterfaceInitializer != InterfaceInitializers.end();
      if (InterfaceValue) {
        // A runtime assignment must not become an inlinable ConstantValue.
        const auto Zero = allocationDefaultLiteral(F.reference.type);
        if (!Zero)
          javaError("interface initializer has an unsupported field type");
        Value = "(null == null ? " + InterfaceInitializer->second + " : " +
                *Zero + ")";
      }
      if (EnumArrayAlias)
        // The enum owns this generated zero-argument method. A qualified
        // expression could bind an enclosing field with the same name as a type.
        Value = "values()";
      const bool AnnotationCopy =
          AnnotationInitializer != AnnotationInitializers.end() &&
          AnnotationInitializer->second.target == F.reference;
      if (AnnotationCopy) {
        const auto &Source = AnnotationInitializer->second.source;
        Value = "((" + TypeNames.render(Source.owner, C) + ") null)." +
                sourceFieldName(Source, Classes, Inheritance, B);
      }
      HasStaticFieldInitializer |=
          Value.has_value() && F.access.contains("static");
      if (F.access.contains("final") && !Value &&
          !Initializations.fields.contains(F.reference))
        javaError("final field needs a verified initialization expression: " +
                  F.reference.owner + "->" + F.reference.name);
      if (Value && F.access.contains("static") && F.access.contains("final") &&
          !DefaultStatic && !Enums.encoded_constants.contains(F.reference)) {
        if (C.access.contains("interface")) {
          // A null comparison is not a Java constant expression and has no
          // effects. Interface fields retain initialization without adding
          // a private static helper that Java 8 interfaces cannot declare.
          if (!AnnotationCopy && !InterfaceValue)
            *Value = "(null == null ? " + *Value + " : " + *Value + ")";
        } else {
          ConstantTypes.insert(F.reference.type);
          *Value = helperName(C, "__neverdConstant") + "(" + *Value + ")";
        }
      }
      if (auto I = Generics.fields.find(F.reference);
          I != Generics.fields.end())
        FieldMods.push_back(
            TypeNames.render(I->second, *I->second.field_type, JavaScope{C}));
      else
        FieldMods.push_back(TypeNames.render(F.reference.type, C));
      FieldMods.push_back(
          sourceFieldName(F.reference, Classes, Inheritance, B));
      if (F.deprecated)
        BodyLines.append("  @" + TypeNames.render("Ljava/lang/Deprecated;", C));
      if (F.suppress_lint)
        BodyLines.append("  " + suppressLint(*F.suppress_lint, JavaScope{C}));
      for (const auto &Use : F.annotations)
        BodyLines.append("  " +
                         annotationUse(Use.type, Use.elements, JavaScope{C}));
      BodyLines.append("  " + join(FieldMods, " ") +
                       (Value ? " = " + *Value : "") + ";");
    }
    std::set<std::pair<std::string, std::vector<std::string>>> JavaSignatures;
    for (const auto &M : C.methods) {
      B.tick();
      const auto &R = M.reference;
      const auto BridgePlan = Inheritance.bridges.find(R);
      const auto PlainBridge = Inheritance.retained_plain_bridges.find(R);
      const auto ErasedVoidParameter =
          Inheritance.erased_void_parameters.find(R);
      const bool HasErasedVoidParameter =
          ErasedVoidParameter != Inheritance.erased_void_parameters.end();
      const bool RetainedPlainBridge =
          PlainBridge != Inheritance.retained_plain_bridges.end();
      const auto BoxedSource = Generics.boxed_superclass_source_results.find(R);
      const bool BoxedSourceResult =
          BoxedSource != Generics.boxed_superclass_source_results.end();
      const bool IntegerBridge = BridgePlan != Inheritance.bridges.end() &&
                                 BridgePlan->second.preserve_body;
      const bool UnitBridge = BridgePlan != Inheritance.bridges.end() &&
                              BridgePlan->second.result_field.has_value() &&
                              !IntegerBridge;
      const auto VoidTarget = Inheritance.void_unit_targets.find(R);
      const auto NonReturning = Inheritance.nonreturning_results.find(R);
      const bool NonReturningResult =
          NonReturning != Inheritance.nonreturning_results.end();
      const auto ReturnSlotName = Inheritance.source_method_names.find(R);
      const auto PublicProtectedTarget =
          Inheritance.public_protected_bridge_targets.find(R);
      const bool PromoteProtectedTarget =
          PublicProtectedTarget !=
          Inheritance.public_protected_bridge_targets.end();
      const auto SourceName = sourceMethodName(R, Inheritance);
      const auto &SourceReference =
          sourceBridgeReference(R, Generics, Inheritance);
      const bool ArrayCarrierSource =
          RetainedPlainBridge &&
          PlainBridge->second.source_reference.has_value();
      const auto &SourceParameters = UnitBridge
                                         ? BridgePlan->second.target.parameters
                                         : SourceReference.parameters;
      // Bridge flags need their own exact source receipt. Unrelated owned
      // metadata, or moving that metadata to a dependency, cannot authorize
      // dropping an unmapped runtime bridge flag from a recovered method.
      if (M.access.contains("bridge") && !Inheritance.bridges.contains(R) &&
          !RetainedPlainBridge)
        javaError("generic bridge method requires an exact regeneration "
                  "mapping: " +
                  R.identity());
      const GenericSignature *MethodSignature =
          sourceMethodSignature(R, Generics);
      if (RetainedPlainBridge && PlainBridge->second.source_signature)
        MethodSignature = &*PlainBridge->second.source_signature;
      const JavaScope MethodScope{C, &R};
      HasClassInitializer |= R.name == "<clinit>";
      llvm::json::Object Row{
          {"identity", R.identity()},
          {"class", C.name},
          {"name", R.name},
          {"prototype", R.signature()},
          {"input", C.source_id},
          {"deprecated", M.deprecated},
          {"instruction_count", static_cast<int64_t>(M.instructions.size())}};
      auto recordMethod = [&] {
        if (Projection.projected_methods.contains(R)) {
          const auto status = Row.getString("status");
          if ((status != "source-projected" &&
               !(status == "declaration-only" && M.instructions.empty())) ||
              !emittedLocalMethods.insert(R).second)
            javaError("local method lacks a unique source identity: " + R.identity());
        }
        Methods.push_back(std::move(Row));
      };
      auto RecordReturnSlotName = [&] {
        if (ReturnSlotName == Inheritance.source_method_names.end())
          return;
        if (auto Original = Row.getString("projection_kind"))
          Row["body_projection_kind"] = Original->str();
        Row["status"] = "source-projected";
        const bool PrivateInstanceName =
            has(M.access, "private") && !has(M.access, "static");
        Row["projection_kind"] =
            ReturnSlotName->second.discarded_reference_result
                ? "discarded-reference-result"
            : ReturnSlotName->second.static_return_slot
                ? "incompatible-static-return-slot"
            : !ReturnSlotName->second.identifier_family.empty()
                ? "virtual-method-identifier"
            : ReturnSlotName->second.identifier_only
                ? (PrivateInstanceName
                       ? (has(M.access, "synthetic")
                              ? "synthetic-private-method-identifier"
                              : "private-method-identifier")
                       : (has(M.access, "synthetic")
                              ? "synthetic-static-method-identifier"
                              : "static-method-identifier"))
            : ReturnSlotName->second.checked_reference_bridge
                ? "checked-reference-target-alias"
            : ReturnSlotName->second.nonreturning_void_bridge
                ? "nonreturning-void-target-alias"
            : ReturnSlotName->second.local_reference_return_slot
                ? "incompatible-local-reference-return-slot"
            : ReturnSlotName->second.local_return_slot
                ? "incompatible-local-return-slot"
                : "incompatible-inherited-return-slot";
        Row["source_name"] = SourceName;
        Row["source_identity"] =
            MethodRef{R.owner, SourceName, R.parameters, R.returns}.identity();
        Row["source_name_declaration"] =
            ReturnSlotName->second.declaration.identity();
        if (!ReturnSlotName->second.identifier_family.empty()) {
          llvm::json::Array Family;
          for (const auto &Member : ReturnSlotName->second.identifier_family)
            Family.push_back(Member.identity());
          Row["source_name_family"] = std::move(Family);
        }
        if (ReturnSlotName->second.private_producer)
          Row["private_producer"] =
              ReturnSlotName->second.private_producer->identity();
        if (ReturnSlotName->second.checked_reference_bridge)
          Row["original_bridge"] =
              ReturnSlotName->second.checked_reference_bridge->identity();
        if (ReturnSlotName->second.nonreturning_void_bridge)
          Row["original_bridge"] =
              ReturnSlotName->second.nonreturning_void_bridge->identity();
        llvm::json::Array Conflicts;
        for (const auto &Contract :
             ReturnSlotName->second.conflicting_contracts)
          Conflicts.push_back(Contract.identity());
        const bool inheritedStatic = ReturnSlotName->second.static_return_slot &&
            std::any_of(ReturnSlotName->second.conflicting_contracts.begin(),
                        ReturnSlotName->second.conflicting_contracts.end(),
                        [&](const MethodRef &ref) { return ref.owner != C.name; });
        Row[(ReturnSlotName->second.local_return_slot ||
             ReturnSlotName->second.local_reference_return_slot ||
             ReturnSlotName->second.discarded_reference_result ||
             (ReturnSlotName->second.static_return_slot && !inheritedStatic))
                ? "conflicting_local_contracts"
                : "conflicting_inherited_contracts"] = std::move(Conflicts);
        if (!Row.getArray("metadata_differences"))
          Row["metadata_differences"] = llvm::json::Array{};
        Row.getArray("metadata_differences")
            ->push_back("the original JVM method has a distinct Java source "
                        "name; its runtime name, binary ABI and method-name "
                        "reflection cannot be regenerated");
        Row.getArray("metadata_differences")
            ->push_back("known input declarations, implementations and callers "
                        "share the disclosed name; external contracts and "
                        "their original default bodies retain their names");
        if (ReturnSlotName->second.discarded_reference_result) {
          Row.getArray("metadata_differences")
              ->push_back("Java source cannot reproduce the original synthetic "
                          "method flag; public final access, descriptor, "
                          "Exceptions and original forwarding body remain");
          Row["reason"] =
              "A final class has a synthetic void adapter beside its original "
              "same-parameter reference method. The complete scalar word "
              "frame calls that exact same-class method once and discards its "
              "result. Known ancestry excludes an inherited void slot; source "
              "declarations and callers share the disclosed adapter name.";
        } else if (ReturnSlotName->second.static_return_slot) {
          Row.getArray("metadata_differences")
              ->push_back(
                  has(M.access, "synthetic")
                      ? "remaining method access flags, descriptor, "
                        "Signature, Exceptions and each original body "
                        "remain unchanged; external callers and "
                        "method-name reflection require the disclosed name"
                      : "method access, descriptor, Signature, Exceptions "
                        "and each original body remain unchanged; external "
                        "callers and method-name reflection require the "
                        "disclosed name");
          Row["reason"] = inheritedStatic
              ? "A final superclass static method has the same Java parameters. "
                "The subclass entry and calls resolved to its original descriptor "
                "share a fresh source name."
              : "An independent class has distinct static return "
              "descriptors with the same Java parameter signature. Each "
              "original body remains separate; exact resolved source calls "
              "use its deterministic disclosed name without changing argument "
              "evaluation, class initialization or result checks.";
        } else if (!ReturnSlotName->second.identifier_family.empty()) {
          Row.getArray("metadata_differences")
              ->push_back(
                  BridgePlan != Inheritance.bridges.end() && !IntegerBridge &&
                          !UnitBridge
                      ? "javac regenerates the original covariant bridge under "
                        "the disclosed source name; its bridge and synthetic "
                        "flags retain their original values"
                  : RetainedPlainBridge
                      ? "the original bridge body is emitted under the "
                        "disclosed source name with ordinary Java method flags"
                  : has(M.access, "synthetic")
                      ? "Java source cannot reproduce the original synthetic "
                        "method flag; original visibility, descriptors, "
                        "Signatures "
                        "and "
                        "bodies remain unchanged"
                      : "method access flags, descriptors, Signatures and "
                        "bodies remain unchanged; retained compiler "
                        "annotations still describe original runtime names");
          Row["reason"] =
              "A closed source method family has an unrepresentable "
              "JVM name; its declarations, overrides and resolved callers "
              "share one collision-free Java name while preserving original "
              "virtual and superclass dispatch.";
        } else if (ReturnSlotName->second.local_reference_return_slot) {
          Row["reason"] =
              "A final class has distinct generic and nominal getter return "
              "slots. The checked superclass adapter retains its full original "
              "Signature, lexical superclass call and result cast under a "
              "disclosed local name; the generic interface and erased map "
              "entries retain their original names and bodies.";
        } else if (ReturnSlotName->second.local_return_slot) {
          Row.getArray("metadata_differences")
              ->push_back("method access, descriptor, Signature and original "
                          "field-write body remain unchanged");
          Row["reason"] =
              "A final class has a pure void property writer and a "
              "same-class reference updater with the same Java parameter "
              "signature. Their full original scoped contracts and exact "
              "writer call retain separate bodies; resolved source callers "
              "use the disclosed void name.";
        } else if (ReturnSlotName->second.identifier_only &&
                   PrivateInstanceName) {
          Row.getArray("metadata_differences")
              ->push_back(
                  has(M.access, "synthetic")
                      ? "Java source cannot reproduce the original synthetic "
                        "method flag; private access, descriptor, Signature "
                        "and body remain unchanged"
                      : "private access, descriptor, Signature and body remain "
                        "unchanged; retained compiler annotations describe the "
                        "original runtime name");
          Row["reason"] =
              "A known private instance body has an unrepresentable JVM name; "
              "its declaration and exact same-class direct callers share a "
              "collision-free Java name while retaining the original receiver "
              "and parameter words.";
        } else if (ReturnSlotName->second.identifier_only &&
                   has(M.access, "synthetic")) {
          Row.getArray("metadata_differences")
              ->push_back("Java source cannot reproduce the original synthetic "
                          "method access flag; retained annotations still "
                          "describe the original runtime name");
          Row["reason"] =
              "A known source static synthetic body has a JVM name that Java "
              "source cannot declare; all source calls share one "
              "collision-free "
              "name while preserving its descriptor, Signature and body.";
        } else if (ReturnSlotName->second.identifier_only) {
          Row.getArray("metadata_differences")
              ->push_back("method access flags, descriptors, Signatures and "
                          "bodies remain unchanged; retained annotations still "
                          "describe the original runtime name");
          Row["reason"] =
              "A known source static body has an unrepresentable JVM name; "
              "its declaration and resolved callers share one collision-free "
              "Java name while preserving its access flags, descriptor, "
              "Signature and body.";
        } else if (ReturnSlotName->second.checked_reference_bridge)
          Row["reason"] =
              "The original checked reference bridge and its Void-returning "
              "target have distinct JVM return slots; Java source gives the "
              "target a separate name while retaining both complete bodies "
              "and the original result cast.";
        else
          Row["reason"] = "Java cannot declare this distinct JVM return slot "
                          "beside its incompatible inherited contract; source "
                          "retains its full original Signature and body using "
                          "one shared name.";
      };
      if (M.generic_signature)
        Row["generic_signature"] = *M.generic_signature;
      if (const auto Getter = Generics.nominal_getter_return_erasures.find(R);
          Getter != Generics.nominal_getter_return_erasures.end()) {
        llvm::json::Array Contracts;
        for (const auto &Contract : Getter->second)
          Contracts.push_back(Contract.identity());
        Row["inherited_nominal_getter_contracts"] = std::move(Contracts);
        Row["generic_return_contract"] = "nominal-getter-result-erasure";
        Row["source_compiler_diagnostics"] = "unchecked return conversion";
      }
      if (Generics.self_carrier_return_sources.contains(R)) {
        Row["generic_return_contract"] =
            "invariant-self-carrier-result-erasure";
        Row["source_compiler_diagnostics"] = "unchecked return conversion";
      }
      llvm::json::Array ClosedLowerContracts;
      for (auto Proof = Generics.closed_object_lower_returns.lower_bound(
               {R, MethodRef{}});
           Proof != Generics.closed_object_lower_returns.end() &&
           Proof->first.first == R;
           ++Proof) {
        B.tick();
        ClosedLowerContracts.push_back(Proof->first.second.identity());
      }
      if (!ClosedLowerContracts.empty()) {
        Row["generic_return_contract"] = "closed-object-lower-bound";
        Row["inherited_closed_lower_contracts"] =
            std::move(ClosedLowerContracts);
      }
      if (const auto Raw = Inheritance.raw_result_contracts.find(R);
          Raw != Inheritance.raw_result_contracts.end()) {
        llvm::json::Array Contracts;
        for (const auto &Contract : Raw->second)
          Contracts.push_back(Contract.identity());
        Row["inherited_raw_result_contracts"] = std::move(Contracts);
        Row["generic_return_contract"] = "original-raw-result";
        Row["source_compiler_diagnostics"] = "unchecked return conversion";
      }
      if (HasErasedVoidParameter) {
        Row["projection_kind"] = "checked-primitive-void-parameter-erasure";
        Row["source_identity"] = R.identity();
        Row["primitive_bridge_witness"] =
            ErasedVoidParameter->second.identity();
        Row["reason"] =
            "The original Object runtime parameter and body are "
            "retained; source uses Object to preserve the certified "
            "primitive bridge's broad unboxing contract.";
        Row["metadata_differences"] = llvm::json::Array{
            "only the source parameter's class-formal Signature is erased; "
            "runtime descriptor, class generics and other members remain",
            "generic parameter reflection changes as recorded in "
            "generic_signature_projections"};
      }
      const auto ParameterMapping = Generics.parameter_indices.find(R);
      if (ParameterMapping != Generics.parameter_indices.end()) {
        Row["generic_signature_omitted_parameters"] = static_cast<int64_t>(
            R.parameters.size() - ParameterMapping->second.size());
        llvm::json::Array Indices;
        for (auto Index : ParameterMapping->second)
          Indices.push_back(static_cast<int64_t>(Index));
        Row["generic_signature_parameter_indices"] = std::move(Indices);
      }
      if (M.parameter_metadata) {
        llvm::json::Array Parameters;
        for (const auto &Parameter : *M.parameter_metadata) {
          llvm::json::Object Metadata{{"access_flags", Parameter.access}};
          Metadata["name"] = Parameter.name ? llvm::json::Value(*Parameter.name)
                                            : llvm::json::Value(nullptr);
          Parameters.push_back(std::move(Metadata));
        }
        Row["parameter_metadata"] = std::move(Parameters);
      }
      const EnumMethodProjection *EnumProjection = nullptr;
      if (auto I = Enums.methods.find(R); I != Enums.methods.end()) {
        EnumProjection = &I->second;
        if (EnumProjection->compiler_managed) {
          Row["status"] = "source-projected";
          Row["projection_kind"] = EnumProjection->kind;
          if (EnumProjection->target)
            Row["forwarding_target"] = EnumProjection->target->identity();
          Row["reason"] = "Verified enum initialization or intrinsic "
                          "forwarding is supplied by "
                          "Java enum declarations; compiler-generated binary "
                          "details remain unverified.";
          ++Projected;
          RecordReturnSlotName();
          recordMethod();
          continue;
        }
      }
      if (auto I = AnnotationInitializers.find(R);
          I != AnnotationInitializers.end()) {
        auto Identity = [](const FieldRef &F) {
          return F.owner + "->" + F.name + ":" + F.type;
        };
        Row["status"] = "source-projected";
        Row["projection_kind"] = "annotation-field-initializer";
        Row["initializer_field"] = Identity(I->second.target);
        Row["read_field"] = Identity(I->second.source);
        Row["reason"] =
            "The verified static field copy is emitted as an annotation "
            "field initializer after all encoded static values; the Java "
            "compiler regenerates the class initializer.";
        ++Projected;
        RecordReturnSlotName();
        recordMethod();
        continue;
      }
      if (RetainedPlainBridge) {
        Row["status"] = "source-projected";
        Row["projection_kind"] =
            PlainBridge->second.checked_reference_return
                ? "checked-reference-return-bridge"
            : PlainBridge->second.erased_terminal_consumer
                ? "erased-terminal-consumer-bridge"
            : PlainBridge->second.inherited_wildcard_parameter
                ? "inherited-wildcard-parameter-erasure"
            : PlainBridge->second.raw_interface_method_formals
                ? PlainBridge->second.source_signature->source_projections
                      .front()
                      .kind
            : PlainBridge->second.erased_terminal_member
                ? "erased-terminal-raw-member"
            : PlainBridge->second.unboxed_predicate
                ? "checked-comparable-predicate-bridge"
            : PlainBridge->second.guarded_scalar_parameter
                ? "guarded-scalar-parameter-bridge"
            : PlainBridge->second.named_scalar_getter
                ? "named-scalar-getter-bridge"
            : PlainBridge->second.boxed_superclass_result
                ? "boxed-generic-superclass-result-bridge"
            : PlainBridge->second.nominal_boxed_result
                ? "boxed-nominal-interface-result-bridge"
            : PlainBridge->second.boxed_interface_result
                ? "boxed-generic-interface-result-bridge"
            : PlainBridge->second.boxed_primitive_getter
                ? "boxed-primitive-getter-bridge"
            : PlainBridge->second.nonreturning_void
                ? "nonreturning-void-forwarding-bridge"
            : PlainBridge->second.checked_inlined_interface
                ? "checked-inlined-interface-bridge"
            : PlainBridge->second.public_superclass_access
                ? "public-superclass-access-bridge"
            : PlainBridge->second.contract_implementation
                ? (MethodSignature && !MethodSignature->type_parameters.empty()
                       ? "method-formal-implementation-bridge"
                       : "full-contract-implementation-bridge")
            : PlainBridge->second.aliased_return_forwarding
                ? "aliased-return-forwarding-bridge"
            : PlainBridge->second.guarded_default_result
                ? "guarded-default-result-forwarding-bridge"
            : PlainBridge->second.raw_parameter_forwarding
                ? "raw-parent-parameter-forwarding-bridge"
            : PlainBridge->second.raw_unit_result ? "raw-reference-unit-bridge"
            : PlainBridge->second.raw_integer_result
                ? "raw-indexed-result-bridge"
            : !PlainBridge->second.raw_indexed_parameters.empty()
                ? "raw-indexed-scope-bridge"
            : PlainBridge->second.superclass_reference_adapter
                ? "checked-reference-superclass-forwarding-bridge"
            : PlainBridge->second.superclass_scalar_adapter
                ? "scalar-superclass-forwarding-bridge"
            : PlainBridge->second.superclass_getter
                ? (R.returns == "I" ? "integer-superclass-forwarding-bridge"
                   : isReference(R.returns)
                       ? "generic-superclass-result-forwarding-bridge"
                       : "scalar-superclass-forwarding-bridge")
            : PlainBridge->second.primitive_void
                ? "primitive-void-interface-bridge"
            : PlainBridge->second.generic_dispatch
                ? "named-generic-setter-bridge"
            : PlainBridge->second.value_carrier
                ? "value-carrier-interface-bridge"
            : PlainBridge->second.interface_super_forwarder
                ? (R.returns == "V" ? "interface-super-void-forwarding-bridge"
                   : R.parameters.size() > 1
                       ? "interface-super-mixed-forwarding-bridge"
                   : isReference(R.returns)
                       ? (R.parameters.empty()
                              ? "interface-super-getter-forwarding-bridge"
                              : "interface-super-reference-forwarding-bridge")
                       : "interface-super-scalar-forwarding-bridge")
            : PlainBridge->second.closed_reference_getter
                ? "closed-reference-getter-forwarding-bridge"
            : PlainBridge->second.reference_getter
                ? "generic-reference-interface-forwarding-bridge"
            : PlainBridge->second.terminal_throw
                ? "terminal-throw-interface-bridge"
            : PlainBridge->second.guarded_reference
                ? (PlainBridge->second.inherited_class_contract
                       ? "guarded-class-forwarding-bridge"
                       : "guarded-interface-forwarding-bridge")
                : "plain-interface-forwarding-bridge";
        Row["source_identity"] = R.identity();
        if (!PlainBridge->second.contract_implementation &&
            !PlainBridge->second.erased_terminal_member)
          Row["bridge_target"] = PlainBridge->second.target.identity();
        if (PlainBridge->second.contract_implementation)
          Row["inherited_contract"] =
              PlainBridge->second.inherited_class_contract->identity();
        else if (PlainBridge->second.inherited_class_contract)
          Row["inherited_class_contract"] =
              PlainBridge->second.inherited_class_contract->identity();
        if (PlainBridge->second.boxed_primitive_getter)
          Row["box_method"] = PlainBridge->second.box_method->identity();
        if (PlainBridge->second.raw_unit_result) {
          const auto &Field = *PlainBridge->second.raw_unit_result;
          Row["result_field"] =
              Field.owner + "->" + Field.name + ":" + Field.type;
        }
        if (!PlainBridge->second.raw_indexed_parameters.empty()) {
          llvm::json::Array Parameters;
          for (size_t Index : PlainBridge->second.raw_indexed_parameters)
            Parameters.push_back(static_cast<int64_t>(Index));
          Row["unboxed_parameters"] = std::move(Parameters);
        }
        if (PlainBridge->second.unbox_method &&
            (PlainBridge->second.guarded_scalar_parameter ||
             PlainBridge->second.unboxed_predicate ||
             !PlainBridge->second.raw_indexed_parameters.empty()))
          Row["unbox_method"] = PlainBridge->second.unbox_method->identity();
        if (!PlainBridge->second.raw_parameter_conversions.empty() &&
            !PlainBridge->second.unbox_method) {
          llvm::json::Array conversions;
          for (const auto &[index, call] : PlainBridge->second.raw_parameter_conversions) {
            llvm::json::Object entry;
            entry["parameter"] = static_cast<int64_t>(index);
            entry["method"] = call.identity();
            conversions.push_back(std::move(entry));
          }
          Row["parameter_conversions"] = std::move(conversions);
        }
        if (PlainBridge->second.generic_dispatch)
          Row["bridge_dispatch"] =
              PlainBridge->second.generic_dispatch->identity();
        if (PlainBridge->second.primitive_void) {
          llvm::json::Array Contracts;
          for (const auto &Contract :
               Generics.primitive_void_bridges.at(R).contracts)
            Contracts.push_back(Contract.identity());
          Row["bridge_contracts"] = std::move(Contracts);
        }
        if (PlainBridge->second.value_carrier) {
          const auto &Proof = Generics.value_carrier_bridges.at(R);
          Row["bridge_dispatch"] = Proof.dispatch.identity();
          if (!Proof.inherited_contracts.empty()) {
            llvm::json::Array VoidContracts, ResultContracts;
            for (const auto &[Method, Contract] : Proof.inherited_contracts) {
              llvm::json::Array Types;
              for (const auto &Parameter : Contract.parameters)
                Types.push_back(Parameter);
              auto &Contracts =
                  Contract.result == "V" ? VoidContracts : ResultContracts;
              Contracts.push_back(
                  llvm::json::Object{{"identity", Method.identity()},
                                     {"parameters", std::move(Types)},
                                     {"returns", Contract.result}});
            }
            if (!VoidContracts.empty())
              Row["checked_inherited_void_contracts"] =
                  std::move(VoidContracts);
            if (!ResultContracts.empty())
              Row["checked_inherited_result_contracts"] =
                  std::move(ResultContracts);
          }
          if (Proof.interface_owner)
            Row["source_interface_owner"] = *Proof.interface_owner;
          if (Proof.box)
            Row["box_method"] = Proof.box->identity();
          if (Proof.representation_field) {
            const auto &Field = *Proof.representation_field;
            Row["representation_field"] =
                Field.owner + "->" + Field.name + ":" + Field.type;
            Row["representation_signature"] = Proof.representation_signature;
          }
          if (Proof.result_field)
            Row["result_field"] = Proof.result_field->owner + "->" +
                                  Proof.result_field->name + ":" +
                                  Proof.result_field->type;
          llvm::json::Array Unbox;
          for (const auto &Method : Proof.unbox)
            Unbox.push_back(Method.identity());
          Row["unbox_methods"] = std::move(Unbox);
        }
        if (PlainBridge->second.guarded_reference && !M.instructions.empty())
          if (const auto *Predicate =
                  std::get_if<MethodRef>(&M.instructions[0].reference))
            Row["guard_predicate"] = Predicate->identity();
        Row["metadata_differences"] = llvm::json::Array{
            "the complete original body, runtime descriptor and source "
            "modifiers are retained as an ordinary Java method",
            PlainBridge->second.erased_terminal_member
                ? "original access, bridge and synthetic flags remain unchanged"
                : "bridge and any synthetic reflection flags change"};
        if (ArrayCarrierSource) {
          Row["projection_kind"] = "array-carrier-superclass-bridge";
          Row["source_identity"] = SourceReference.identity();
          Row["regenerated_bridge"] = R.identity();
          Row["unbox_method"] = PlainBridge->second.unbox_method->identity();
          llvm::json::Array Contracts;
          for (const auto &Contract : PlainBridge->second.carrier_contracts)
            Contracts.push_back(Contract.identity());
          Row["checked_inherited_contracts"] = std::move(Contracts);
          Row["metadata_differences"] = llvm::json::Array{
              "all original superclass arguments and target visibility remain "
              "unchanged",
              "the original checked array unbox and delegate body moves into "
              "one public typed entry",
              "javac regenerates the original erased descriptor and bridge "
              "flags; added declarations, forwarding frames and source caller "
              "cast frames differ",
              "known source callers preserve argument evaluation and "
              "null-receiver failure before a checked carrier cast"};
          Helpers.push_back(llvm::json::Object{
              {"class", R.owner},
              {"name", SourceName},
              {"prototype", SourceReference.signature()},
              {"static", false},
              {"source_unit", Unit},
              {"kind", "array-carrier-superclass-entry"},
              {"original_bridge", R.identity()},
              {"access_flags", llvm::json::Array{"public"}}});
        }
        if (BoxedSourceResult) {
          const auto &Typed = BoxedSource->second;
          Row["projection_kind"] = "boxed-bound-superclass-result-bridge";
          Row["source_identity"] = Typed.identity();
          Row["regenerated_bridge"] = R.identity();
          Row["metadata_differences"] = llvm::json::Array{
              "the original full superclass argument and dependent bounds are "
              "retained",
              "the original virtual target and factory body moves into one new "
              "public typed result entry",
              "javac regenerates the original erased bridge descriptor and "
              "flags; declared-method reflection and forwarding stack frames "
              "change"};
          Helpers.push_back(llvm::json::Object{
              {"class", Typed.owner},
              {"name", SourceName},
              {"prototype", Typed.signature()},
              {"static", false},
              {"source_unit", Unit},
              {"kind", "boxed-superclass-result-entry"},
              {"original_bridge", R.identity()},
              {"access_flags", llvm::json::Array{"public"}}});
        }
        if (PlainBridge->second.erased_terminal_member) {
          const auto &Projection =
              PlainBridge->second.source_signature->source_projections.front();
          Row["original_method_signature"] = *M.generic_signature;
          Row["source_generic_signature"] = Projection.source_type;
          Row.getArray("metadata_differences")
              ->push_back("the reported source Signature erases verified "
                          "generic spellings; runtime descriptors and complete "
                          "original member contracts remain unchanged");
        }
        if (PlainBridge->second.closed_reference_getter) {
          const auto *Target = member(PlainBridge->second.target, Classes, B);
          Row["checked_target_generic_signature"] = *Target->generic_signature;
        }
        if (PlainBridge->second.nonreturning_void) {
          const auto &Target = PlainBridge->second.target;
          if (MethodSignature) {
            const auto &Original = Generics.methods.at(Target);
            const auto *Declaration = member(Target, Classes, B);
            Row["source_generic_signature"] =
                Declaration->generic_signature->substr(
                    0, Original.types.at(*Original.result).offset) +
                "V";
            Row.getArray("metadata_differences")
                ->push_back("the source parameter Signature comes from the "
                            "fully checked original typed target; runtime "
                            "parameter descriptors remain unchanged");
          }
        }
        if (PlainBridge->second.superclass_getter && isReference(R.returns))
          Row.getArray("metadata_differences")
              ->push_back(
                  "the original broad result Signature is projected to the "
                  "complete superclass result witnessed by the final getter");
      }
      if (auto Bridge = Inheritance.bridges.find(R);
          Bridge != Inheritance.bridges.end()) {
        Row["status"] = "source-projected";
        Row["projection_kind"] =
            has(Classes.at(Bridge->second.dispatch.owner).access, "interface")
                ? "generic-interface-bridge"
                : "generic-superclass-bridge";
        Row["bridge_target"] = Bridge->second.target.identity();
        Row["bridge_dispatch"] = Bridge->second.dispatch.identity();
        if (Inheritance.public_protected_bridge_targets.contains(
                Bridge->second.target)) {
          Row["metadata_differences"] = llvm::json::Array{
              "javac regenerates the original public erased forwarding "
              "descriptor through its verified closed target",
              "the original protected target becomes public in source as "
              "reported on its own method row"};
        }
        if (IntegerBridge) {
          const auto &Result = *Bridge->second.result_field;
          Row["projection_kind"] = Bridge->second.boolean_parameters
                                       ? "boolean-unit-interface-bridge"
                                       : "integer-unit-interface-bridge";
          Row["source_identity"] = R.identity();
          Row["result_field"] =
              Result.owner + "->" + Result.name + ":" + Result.type;
          Row["metadata_differences"] = llvm::json::Array{
              "the complete original bridge body is an ordinary Java method; "
              "bridge and synthetic reflection flags change",
              "the implemented generic interface is raw in source as recorded "
              "in generic_signature_projections"};
        } else if (UnitBridge) {
          const auto &Result = *Bridge->second.result_field;
          Row["projection_kind"] = "void-unit-interface-bridge";
          Row["source_identity"] =
              MethodRef{C.name, R.name, SourceParameters, Result.type}
                  .identity();
          Row["result_field"] =
              Result.owner + "->" + Result.name + ":" + Result.type;
          Row["metadata_differences"] = llvm::json::Array{
              "a typed Unit wrapper retains the original checked void call "
              "followed by the original singleton field read",
              "the Java compiler adds an erased forwarding bridge; method "
              "reflection and stack frames change"};
          if (Generics.methods.contains(Bridge->second.target))
            Row.getArray("metadata_differences")
                ->push_back("wrapper parameters use their checked runtime "
                            "erasures; the renamed void target retains its "
                            "original generic parameter Signature");
        } else {
          if (Bridge->second.boxed_reference_result) {
            Row["projection_kind"] = "boxed-covariant-interface-result-bridge";
            const auto typed = Generics.boxed_interface_results.find(
                {R, Bridge->second.dispatch});
            if (typed != Generics.boxed_interface_results.end() &&
                typed->second.preserve_typed_interface) {
              Row["box_method"] = typed->second.box.identity();
              Row["original_scalar_target"] = typed->second.target.identity();
            } else {
              const auto &proof = Generics.nominal_boxed_results.at(R);
              Row["box_method"] = proof.box.identity();
              Row["original_scalar_target"] = proof.target.identity();
            }
            Row["metadata_differences"] = llvm::json::Array{
                "both original return entries call the same scalar target and "
                "boxing factory with identical arguments",
                "the broader return entry is regenerated through "
                "the retained original boxed entry; stack frames differ"};
          }
          if (Bridge->second.guarded_reference_result) {
            Row["projection_kind"] = "guarded-erased-reference-return-bridge";
            Row["metadata_differences"] = llvm::json::Array{
                "the original guard, null result and typed virtual invocation "
                "are independently identical to the retained original wrapper",
                "javac regenerates the erased return entry through that "
                "wrapper; "
                "an extra stack frame and the original final flag may differ"};
          }
          if (Bridge->second.terminal_throw_result) {
            Row["projection_kind"] = "terminal-throw-result-bridge";
            Row["metadata_differences"] = llvm::json::Array{
                "the original typed body retains the same exception "
                "allocation, "
                "message, constructor and throw without reading any parameters",
                "the Java compiler regenerates the result-only bridge as a "
                "call into that typed body; exception stack frames change"};
          }
          if (Bridge->second.nonreturning_boolean_result) {
            const bool Shared =
                Bridge->second.target.parameters == R.parameters;
            Row["projection_kind"] =
                Shared ? "nonreturning-shared-boolean-result-bridge"
                       : "nonreturning-boolean-result-bridge";
            if (Shared)
              Row["source_shared_with"] = Bridge->second.target.identity();
            Row["metadata_differences"] = llvm::json::Array{
                "the final owner's original target always throws after its "
                "original static prefix and exception allocation",
                "Java regenerates the original erased boolean descriptor and "
                "parameter cast against the projected typed boolean result; "
                "the original post-call cast and unbox cannot execute"};
            if (Shared)
              Row["metadata_differences"] = llvm::json::Array{
                  "both original same-parameter return entries share the "
                  "projected typed boolean source method and unchanged throw "
                  "body",
                  "the original erased entry acquires that full parameter "
                  "Signature, loses bridge/synthetic flags and forwarding "
                  "frame"};
          }
          if (const auto *Target = member(Bridge->second.target, Classes, B);
              Target && !Target->annotations.empty()) {
            Row["bridge_annotation_source"] = Target->reference.identity();
            if (!Row.getArray("metadata_differences"))
              Row["metadata_differences"] = llvm::json::Array{};
            Row.getArray("metadata_differences")
                ->push_back("the original typed target's method annotations "
                            "are retained");
            Row.getArray("metadata_differences")
                ->push_back("Java source and generated bridges can give the "
                            "originally unannotated erased entry the typed "
                            "target's annotations; reflection may differ");
          }
          if (Bridge->second.source_declared_throws) {
            llvm::json::Array Exceptions;
            for (const auto &Exception : *Bridge->second.source_declared_throws)
              Exceptions.push_back(Exception);
            Row["source_bridge_declared_throws"] = std::move(Exceptions);
            Row["bridge_exception_source"] = Bridge->second.dispatch.identity();
            if (!Row.getArray("metadata_differences"))
              Row["metadata_differences"] = llvm::json::Array{};
            Row.getArray("metadata_differences")
                ->push_back(
                    "javac regenerates the originally exception-free erased "
                    "bridge "
                    "with the verified inherited ordered Exceptions attribute; "
                    "the original typed target still declares no exceptions");
          }
          Row["reason"] =
              Bridge->second.nonreturning_boolean_result &&
                      Bridge->second.target.parameters == R.parameters
                  ? "Both original same-parameter return entries "
                    "share the proven original throw body in one "
                    "typed boolean source entry; generic reflection "
                    "and forwarding frame differences are reported."
              : Bridge->second.nonreturning_boolean_result
                  ? "The complete original parameter contract and "
                    "unconditional throw retain invocation and "
                    "exception order; javac supplies the erased "
                    "boolean bridge."
              : Bridge->second.guarded_reference_result
                  ? "The independently verified identical guard "
                    "and typed virtual call remain in the original "
                    "wrapper; javac supplies the original erased "
                    "return descriptor."
              : Bridge->second.terminal_throw_result
                  ? "The identical parameter-independent throw "
                    "remains in the original typed body; javac "
                    "regenerates the result-only forwarding bridge."
                  : "Verified checked forwarding is regenerated by "
                    "the Java compiler; compiler-generated binary "
                    "details remain unverified.";
          if (Bridge->second.public_superclass_access) {
            Row["projection_kind"] = "generated-public-superclass-access-bridge";
            Row["reason"] = "The complete closed inherited input contract and "
                            "original erased superclass forwarding select "
                            "javac's public visibility bridge.";
            Row["metadata_differences"] = llvm::json::Array{
                "the Java compiler supplies the original erased descriptor, "
                "public bridge and synthetic flags; source debug locations differ"};
            if (!M.parameter_metadata)
              Row.getArray("metadata_differences")->push_back(
                  "javac adds unnamed synthetic MethodParameters for the "
                  "input bridge parameters");
          }
          ++Projected;
          RecordReturnSlotName();
          recordMethod();
          continue;
        }
      }
      if (AnonymousSite && R.name == "<init>") {
        Row["status"] = "source-projected";
        Row["projection_kind"] = "anonymous-interface";
        Row["reason"] =
            "Verified empty Object initialization is supplied by the "
            "anonymous expression's implicit constructor.";
        ++Projected;
        RecordReturnSlotName();
        recordMethod();
        continue;
      }
      if (!JavaSignatures.emplace(SourceName, SourceParameters).second)
        javaError(
            R.identity() +
            ": Java cannot represent methods overloaded only by return type");
      auto MethodMods = modifiers(M.access, {"public", "protected", "private",
                                             "static", "final", "synchronized",
                                             "native", "abstract", "strictfp"});
      if (PromoteProtectedTarget) {
        std::erase(MethodMods, "protected");
        MethodMods.insert(MethodMods.begin(), "public");
      }
      if (Kind == "interface" && R.name != "<clinit>" &&
          !has(M.access, "abstract")) {
        if (!has(M.access, "public") && !has(M.access, "private"))
          javaError("interface body requires a public or private declaration");
        if (has(M.access, "private"))
          MinimumJavaRelease = std::max(MinimumJavaRelease, 9u);
        else if (!has(M.access, "static"))
          MethodMods.push_back("default");
      }
      if (MethodSignature) {
        auto Formals = TypeNames.formals(*MethodSignature, MethodScope);
        if (!Formals.empty())
          MethodMods.push_back(std::move(Formals));
      }
      std::vector<std::string> Params;
      const bool EnumConstructor =
          EnumProjection && EnumProjection->kind == "enum-constructor";
      const size_t ImplicitParameters = EnumConstructor ? 2 : 0;
      std::vector<std::optional<GenericTypeId>> ParameterTypes(
          R.parameters.size());
      if (MethodSignature)
        for (size_t I = 0; I < MethodSignature->parameters.size(); ++I) {
          const auto Index =
              ParameterMapping == Generics.parameter_indices.end()
                  ? I
                  : ParameterMapping->second.at(I);
          ParameterTypes.at(Index) = MethodSignature->parameters[I];
        }
      for (size_t I = ImplicitParameters; I < R.parameters.size(); ++I) {
        auto Type = ParameterTypes[I] && !(HasErasedVoidParameter && I == 0)
                        ? TypeNames.render(*MethodSignature, *ParameterTypes[I],
                                           MethodScope)
                        : TypeNames.render(SourceParameters[I], MethodScope);
        if (M.access.contains("varargs") && I + 1 == R.parameters.size()) {
          if (!Type.ends_with("[]"))
            javaError("varargs source parameter does not retain its array "
                      "type: " +
                      R.identity());
          Type.resize(Type.size() - 2);
          Type += "...";
        }
        std::string Annotations;
        if (!M.parameter_annotations.empty())
          for (const auto &Use : M.parameter_annotations.at(I))
            Annotations +=
                annotationUse(Use.type, Use.elements, MethodScope) + " ";
        Params.push_back(Annotations + Type + " arg" + std::to_string(I));
      }
      std::vector<std::string> Throws;
      if (MethodSignature && !MethodSignature->throws_types.empty()) {
        for (auto Id : MethodSignature->throws_types)
          Throws.push_back(TypeNames.render(*MethodSignature, Id, MethodScope));
      } else if (auto I = Generics.declared_throws.find(R);
                 I != Generics.declared_throws.end())
        for (const auto &Type : I->second)
          Throws.push_back(TypeNames.render(Type, MethodScope));
      const auto ConstructorExceptions =
          Generics.constructor_exception_contracts.find(R);
      const bool ProjectConstructorExceptions =
          ConstructorExceptions !=
              Generics.constructor_exception_contracts.end() &&
          ConstructorExceptions->second.source_declared_throws !=
              M.declared_throws.value_or(std::vector<std::string>{});
      std::string Declaration;
      if (R.name == "<clinit>") {
        if (!R.parameters.empty() || R.returns != "V" ||
            !M.access.contains("static") || MethodSignature ||
            Generics.declared_throws.contains(R) || M.deprecated ||
            M.suppress_lint)
          javaError("invalid class initializer signature");
        Declaration = "static";
      } else if (R.name == "<init>") {
        if (R.returns != "V" || M.access.contains("static"))
          javaError("invalid instance initializer signature");
        MethodMods.push_back(Name);
        Declaration = join(MethodMods, " ") + "(" + join(Params, ", ") + ")";
        if (!EnumProjection) {
          if (ProjectConstructorExceptions) {
            Throws.clear();
            for (const auto &Type :
                 ConstructorExceptions->second.source_declared_throws)
              Throws.push_back(TypeNames.render(Type, MethodScope));
          } else if (ConstructorExceptions ==
                         Generics.constructor_exception_contracts.end() &&
                     constructorThrows(M, Classes, Generics, B,
                                       HasGenericMetadata))
            Throws.push_back("java.lang.Throwable");
        }
      } else {
        MethodMods.push_back(
            NonReturningResult
                ? TypeNames.render(NonReturning->second.source_type,
                                   MethodScope)
            : BoxedSourceResult
                ? TypeNames.render(BoxedSource->second.returns, MethodScope)
            : UnitBridge
                ? TypeNames.render(BridgePlan->second.result_field->type,
                                   MethodScope)
            : MethodSignature
                ? TypeNames.render(*MethodSignature, *MethodSignature->result,
                                   MethodScope)
                : TypeNames.render(SourceReference.returns, MethodScope));
        MethodMods.push_back(SourceName);
        Declaration = join(MethodMods, " ") + "(" + join(Params, ", ") + ")";
      }
      if (!Throws.empty())
        Declaration += " throws " + join(Throws, ", ");
      if (M.deprecated)
        // Method and constructor type parameters exclude their modifiers.
        BodyLines.append("  @" + TypeNames.render("Ljava/lang/Deprecated;", C));
      if (M.suppress_lint)
        BodyLines.append("  " + suppressLint(*M.suppress_lint, JavaScope{C}));
      for (const auto &Use : M.annotations)
        BodyLines.append("  " +
                         annotationUse(Use.type, Use.elements, JavaScope{C}));
      if (M.access.contains("abstract") || M.access.contains("native")) {
        if (M.annotation_default)
          Declaration +=
              " default " + annotationValue(*M.annotation_default, MethodScope);
        BodyLines.append("  " + Declaration + ";");
        llvm::json::Array GeneratedBridges;
        bool GeneratedParameters = false, GeneratedClass = false;
        const auto AddGenerated = [&](const auto &Bridges, bool ClassBridge) {
          for (const auto &[Bridge, Proof] : Bridges) {
            if (Proof.target != R)
              continue;
            B.tick();
            GeneratedClass |= ClassBridge;
            GeneratedParameters |= Proof.generated_parameter_adaptation;
            GeneratedBridges.push_back(Bridge.identity());
            Helpers.push_back(llvm::json::Object{
                {"class", Bridge.owner},
                {"name", SourceName},
                {"prototype", Bridge.signature()},
                {"static", false},
                {"source_unit", Unit},
                {"kind", ClassBridge ? (Proof.generated_parameter_adaptation
                                            ? "compiler-parameter-class-bridge"
                                            : "compiler-covariant-class-bridge")
                         : Proof.generated_parameter_adaptation
                             ? "compiler-parameter-interface-bridge"
                             : "compiler-covariant-interface-bridge"},
                {"bridge_target", Proof.target.identity()},
                {"bridge_dispatch", Proof.dispatch.identity()},
                {"access_flags",
                 llvm::json::Array{ClassBridge && Proof.protected_contract
                                       ? "protected"
                                       : "public",
                                   "bridge", "synthetic"}}});
          }
        };
        AddGenerated(Inheritance.generated_interface_bridges, false);
        AddGenerated(Inheritance.generated_class_bridges, true);
        if (GeneratedBridges.empty() &&
            ReturnSlotName == Inheritance.source_method_names.end() &&
            !HasErasedVoidParameter) {
          Row["status"] = "declaration-only";
          Row["reason"] =
              "original abstract/native declaration has no Dalvik body";
          ++Declared;
        } else if (!GeneratedBridges.empty()) {
          Row["status"] = "source-projected";
          Row["projection_kind"] =
              GeneratedClass
                  ? (GeneratedParameters ? "class-parameter-runtime-bridges"
                                         : "class-covariant-runtime-bridges")
              : GeneratedParameters ? "interface-parameter-runtime-bridges"
                                    : "interface-covariant-runtime-bridges";
          Row["reason"] =
              "The original abstract declaration retains its generic signature. "
              "Java generates bridge methods for the inherited runtime slots.";
          Row["generated_bridges"] = std::move(GeneratedBridges);
          Row["metadata_differences"] = llvm::json::Array{
              "the original abstract declaration and complete generic "
              "Signature are retained",
              GeneratedClass
                  ? "javac adds synthetic class bridges with the original "
                    "declaration visibility for inherited specialized slots"
                  : "javac adds public synthetic bridge declarations for the "
                    "inherited specialized slots",
              "declared-method and default/bridge reflection, rebuilt dispatch "
              "and stack frames may differ"};
          ++Projected;
        } else {
          Row["status"] = "source-projected";
          ++Projected;
        }
      } else {
        if (Kind == "interface" && R.name == "<clinit>") {
          Row["reason"] = "Original effects and final field writes are emitted "
                          "once in ordered interface field initializers.";
          if (Projection.projected_methods.contains(R)) {
            Row["status"] = "source-projected";
            Row["projection_kind"] = "interface-initializer-with-projected-classes";
            Row["source_identity"] = R.identity();
            Row["metadata_differences"] = llvm::json::Array{
                "field initializer expressions reference the disclosed source names of their original classes"};
            ++Projected;
          } else {
            Row["status"] = "recovered";
            ++Recovered;
          }
          recordMethod();
          continue;
        }
        std::optional<Method> UnitSourceMethod;
        if (UnitBridge) {
          UnitSourceMethod = M;
          UnitSourceMethod->reference.parameters = SourceParameters;
          UnitSourceMethod->reference.returns =
              BridgePlan->second.result_field->type;
        }
        const Method &SourceMethod =
            UnitSourceMethod ? *UnitSourceMethod
            : EnumProjection && EnumProjection->kind == "enum-initializer"
                ? *Enums.classes.at(C.name).initializer_tail
                : M;
        Body Emitter(SourceMethod, Classes, B, TypeNames, Generics, Projection,
                     Inheritance, AnonymousSources, Initializations.writes[R],
                     EnumConstructor ? EnumProjection : nullptr,
                     Initializations.pre_super_defaults);
        std::string Source;
        std::optional<Method> SuperHelper;
        const auto DefaultBody = Inheritance.interface_default_helpers.find(R);
        const auto DefaultAccess =
            Inheritance.interface_default_accessors.find(R);
        const auto InheritedDefaultAccess =
            Inheritance.inherited_interface_default_accessors.find(R);
        if (DefaultBody != Inheritance.interface_default_helpers.end()) {
          Lines Locals(B);
          if (const auto I = Projection.by_method.find(R);
              I != Projection.by_method.end())
            for (const auto *Child : I->second)
              Locals.append(
                  emitClass(*Child, DeclarationSite::MethodLocal, Unit));
          // Validate and emit the original body in its original semantic scope
          // once. Only its source declaration moves; the public default entry
          // and exact static accessor both call this private instance body.
          const auto HelperSource = Emitter.emit(Locals.text());
          std::vector<std::string> Parameters, Arguments;
          for (size_t I = 0; I < R.parameters.size(); ++I) {
            Parameters.push_back(
                (ParameterTypes[I]
                     ? TypeNames.render(*MethodSignature, *ParameterTypes[I],
                                        MethodScope)
                     : TypeNames.render(R.parameters[I], MethodScope)) +
                " arg" + std::to_string(I));
            Arguments.push_back("arg" + std::to_string(I));
          }
          const auto Formals =
              MethodSignature ? TypeNames.formals(*MethodSignature, MethodScope)
                              : std::string{};
          const auto ReturnType =
              MethodSignature
                  ? TypeNames.render(*MethodSignature, *MethodSignature->result,
                                     MethodScope)
                  : TypeNames.render(R.returns, MethodScope);
          BodyLines.append("  private " +
                           (Formals.empty() ? "" : Formals + " ") + ReturnType +
                           " " + DefaultBody->second + "(" +
                           join(Parameters, ", ") + ") {");
          for (const auto &Line : split(HelperSource, '\n'))
            BodyLines.append("    " + Line);
          BodyLines.append("  }");
          auxiliary(C, DefaultBody->second, R.signature(), false, Unit,
                    "interface-default-body-helper");
          if (MethodSignature)
            Helpers.back().getAsObject()->insert(
                {"generic_signature", *M.generic_signature});
          Source = (R.returns == "V" ? "" : "return ") + std::string("this.") +
                   DefaultBody->second + "(" + join(Arguments, ", ") + ");";
        } else if (DefaultAccess !=
                       Inheritance.interface_default_accessors.end() ||
                   InheritedDefaultAccess !=
                       Inheritance.inherited_interface_default_accessors
                           .end()) {
          Emitter.interfaceDefaultAccessor();
          Source = Emitter.emit();
        } else if (auto I = SuperHelperNames.find(R);
                   I != SuperHelperNames.end()) {
          SuperHelper = Emitter.staticSuperAccessor(I->second);
          const auto HelperSource = Emitter.emitSuperHelper(*SuperHelper);
          std::vector<std::string> HelperParameters, ForwardedArguments;
          for (size_t I = 0; I < SuperHelper->reference.parameters.size();
               ++I) {
            HelperParameters.push_back(
                TypeNames.render(SuperHelper->reference.parameters[I], C) +
                " arg" + std::to_string(I));
            ForwardedArguments.push_back("arg" + std::to_string(I + 1));
          }
          BodyLines.append("  private " +
                           TypeNames.render(SuperHelper->reference.returns, C) +
                           " " + SuperHelper->reference.name + "(" +
                           join(HelperParameters, ", ") + ") {");
          for (const auto &Line : split(HelperSource, '\n'))
            BodyLines.append("    " + Line);
          BodyLines.append("  }");
          auxiliary(C, SuperHelper->reference.name,
                    SuperHelper->reference.signature(), false, Unit,
                    "super-dispatch-helper");
          Source = M.instructions.size() == 2
                       ? "arg0." + SuperHelper->reference.name + "(" +
                             join(ForwardedArguments, ", ") + ");"
                       : Emitter.emit();
        } else if (auto I = Projection.by_method.find(R);
                   I != Projection.by_method.end()) {
          Lines MethodSource(B);
          for (const auto *Child : I->second)
            MethodSource.append(
                emitClass(*Child, DeclarationSite::MethodLocal, Unit));
          Source = Emitter.emit(MethodSource.text());
        } else
          Source = Emitter.emit();
        MinimumJavaRelease =
            std::max(MinimumJavaRelease, Emitter.minimumJavaRelease());
        const auto MethodJavaRelease =
            std::max(ClassJavaRelease, Emitter.minimumJavaRelease());
        if (MethodJavaRelease > 8)
          Row["minimum_java_release"] = MethodJavaRelease;
        auto SourceLines = split(Source, '\n');
        for (auto &Line : SourceLines)
          Line = "    " + Line;
        if (Emitter.sourceSynchronized())
          Declaration = "synchronized " + Declaration;
        BodyLines.append("  " + Declaration + " {\n" + join(SourceLines, "\n") +
                         "\n  }");
        if (DefaultBody != Inheritance.interface_default_helpers.end() ||
            DefaultAccess != Inheritance.interface_default_accessors.end()) {
          const auto Target =
              DefaultBody != Inheritance.interface_default_helpers.end()
                  ? R
                  : DefaultAccess->second;
          const auto &Name = Inheritance.interface_default_helpers.at(Target);
          const MethodRef Helper{Target.owner, Name, Target.parameters,
                                 Target.returns};
          Row["status"] = "source-projected";
          Row["projection_kind"] =
              DefaultBody != Inheritance.interface_default_helpers.end()
                  ? "interface-default-body"
                  : "static-interface-default-accessor";
          Row["source_identity"] = R.identity();
          Row["default_body_helper"] = Helper.identity();
          Row["original_default_method"] = Target.identity();
          Row["metadata_differences"] = llvm::json::Array{
              "one reported private interface instance helper contains the "
              "original default body; declared-method reflection and stack "
              "frames gain this method",
              "owned method-local declarations move with that body; source "
              "EnclosingMethod names the reported helper"};
          if (has(M.access, "synthetic"))
            Row.getArray("metadata_differences")
                ->push_back("the original descriptor is retained but "
                            "its synthetic access flag cannot be reproduced in "
                            "Java source");
          Row["reason"] = "The exact owned static forwarding receipt preserves "
                          "direct interface default dispatch on the original "
                          "receiver through one private body helper.";
          ++Projected;
        } else if (InheritedDefaultAccess !=
                   Inheritance.inherited_interface_default_accessors.end()) {
          Row["status"] = "source-projected";
          Row["projection_kind"] = "inherited-interface-default-accessor";
          Row["source_identity"] = R.identity();
          Row["original_dispatch"] =
              InheritedDefaultAccess->second.first.identity();
          Row["ancestor_accessor"] =
              InheritedDefaultAccess->second.second.identity();
          Row["metadata_differences"] = llvm::json::Array{
              "the original static descriptor remains; its synthetic flag "
              "cannot be reproduced in Java source",
              "the already verified original ancestor static accessor retains "
              "default dispatch; stack traces include that forwarding entry"};
          Row["reason"] = "The exact static forwarding body and complete "
                          "unchanged single-interface formal chain select one "
                          "original ancestor default accessor.";
          ++Projected;
        } else if (SuperHelper) {
          Row["status"] = "source-projected";
          Row["projection_kind"] = "static-super-accessor";
          Row["source_identity"] = R.identity();
          Row["super_dispatch_helper"] = SuperHelper->reference.identity();
          llvm::json::Array Differences;
          if (has(M.access, "synthetic"))
            Differences.push_back(
                "the original static entry and descriptor remain, but Java "
                "source cannot reproduce its synthetic access flag");
          Differences.push_back(
              "a reported private instance helper retains super dispatch "
              "on the original receiver; reflection and stack frames include "
              "this additional method");
          Row["metadata_differences"] = std::move(Differences);
          Row["reason"] = "The exact forwarding body retains "
                          "its static entry, original parameter checks and "
                          "resolved super invocation through a private helper.";
          ++Projected;
        } else if (PromoteProtectedTarget || IntegerBridge || UnitBridge ||
                   RetainedPlainBridge || NonReturningResult ||
                   HasErasedVoidParameter ||
                   ReturnSlotName != Inheritance.source_method_names.end() ||
                   VoidTarget != Inheritance.void_unit_targets.end()) {
          Row["status"] = "source-projected";
          if (PromoteProtectedTarget) {
            Row["projection_kind"] =
                M.generic_signature &&
                        R.returns != PublicProtectedTarget->second.returns
                    ? "public-erased-protected-generic-result-target"
                : R.parameters.empty() ? "public-erased-protected-getter-target"
                : R.returns.starts_with("[L") &&
                        R.parameters == PublicProtectedTarget->second.parameters
                    ? "public-erased-protected-array-result-target"
                    : "public-erased-protected-parameter-target";
            Row["source_identity"] = R.identity();
            Row["original_bridge"] = PublicProtectedTarget->second.identity();
            Row["metadata_differences"] = llvm::json::Array{
                "the complete original typed body, runtime name, "
                "descriptor and Signature remain unchanged",
                "the original protected target is public in source so javac "
                "retains its original public erased forwarding descriptor; "
                "target access reflection changes"};
            Row["reason"] = "The verified public erased bridge requires a "
                            "public typed source override.";
            if (!C.access.contains("final"))
              Row["metadata_differences"].getAsArray()->push_back(
                  "known source descendants inherit the original typed slot; "
                  "independently compiled overrides keep virtual dispatch, "
                  "but recompiling a protected override against this public "
                  "source target requires matching public access");
          } else if (NonReturningResult) {
            auto SourceReference = R;
            SourceReference.returns = NonReturning->second.source_type;
            Row["source_identity"] = SourceReference.identity();
            Row["projection_kind"] = "nonreturning-boolean-result";
            Row["original_bridge"] = NonReturning->second.bridge.identity();
            if (MethodSignature && M.generic_signature)
              Row["source_generic_signature"] =
                  M.generic_signature->substr(
                      0, MethodSignature->types.at(*MethodSignature->result)
                             .offset) +
                  NonReturning->second.source_type;
            Row["metadata_differences"] = llvm::json::Array{
                "the original final-owner body has no normal return path and "
                "retains the same static prefix, allocation, constructor and "
                "throw",
                "the typed result descriptor and generic result Signature "
                "change from Void to boolean; binary ABI and return reflection "
                "change",
                "known source callers retain the original reference carrier; "
                "implicit result boxing cannot execute after this proven "
                "throw"};
            Row["reason"] = "The proven unconditional original throw supplies "
                            "the inherited boolean source result without "
                            "inventing a return value.";
          } else if (RetainedPlainBridge) {
            Row["reason"] =
                ArrayCarrierSource
                    ? "The complete inherited parameter and result contract "
                      "supplies a typed source entry containing the original "
                      "cast, array unbox and delegate calls; javac regenerates "
                      "the erased bridge without widening source ancestry."
                : BoxedSourceResult
                    ? "The complete original factory body supplies the "
                      "verified boxed source result while retaining the full "
                      "superclass argument and dependent bounds; javac "
                      "regenerates the erased forwarding entry."
                : PlainBridge->second.erased_terminal_consumer
                    ? "The original parameter-independent allocation and throw "
                      "remain in the erased runtime entry without a new typed "
                      "method, cast or scalar read; full original contracts "
                      "remain checked and raw source metadata is reported."
                : PlainBridge->second.erased_terminal_member
                    ? "The raw source interface requires erased parameter and "
                      "method-formal metadata; the complete original member "
                      "contract, runtime descriptor and body remain checked."
                : PlainBridge->second.guarded_scalar_parameter
                    ? "The original Object bridge retains its exact type "
                      "guard, false return, unbox and scalar virtual calls; "
                      "effects and failure order remain unchanged, and bridge "
                      "reflection differences are reported."
                : PlainBridge->second.named_scalar_getter
                    ? "The original bridge retains its exact receiver and "
                      "scalar-width virtual call to the original named getter; "
                      "the target's full body, effects and failure order stay "
                      "unchanged, and bridge reflection differences are "
                      "reported."
                : PlainBridge->second.nominal_boxed_result
                    ? "The original bridge retains the unchanged scalar "
                      "arguments, virtual target and boxing factory; the "
                      "scalar return slot has one disclosed source alias, and "
                      "bridge reflection flags differ."
                : PlainBridge->second.boxed_superclass_result
                    ? "The original bridge retains its exact virtual target "
                      "and static factory calls, including result identity, "
                      "effects and failure order; the source superclass "
                      "argument and bridge reflection differences are reported."
                : PlainBridge->second.boxed_primitive_getter
                    ? "The original bridge retains its exact immutable field "
                      "getter and static boxing call, including primitive "
                      "width, allocation, effects and failure order; bridge "
                      "reflection differences are reported."
                : PlainBridge->second.nonreturning_void
                    ? "The original void delegation discards the unchanged "
                      "Void result of its aliased terminal target; prefix, "
                      "allocation and throw order are retained."
                : PlainBridge->second.public_superclass_access
                    ? "The original public access bridge retains its exact "
                      "receiver, argument order and nearest superclass call; "
                      "bridge and synthetic reflection differences are "
                      "reported."
                : PlainBridge->second.contract_implementation
                    ? "The complete original inheritance contract, runtime "
                      "descriptor and body are emitted unchanged; bridge "
                      "reflection differences are reported."
                : PlainBridge->second.aliased_return_forwarding
                    ? "The original erased return body retains its casts and "
                      "virtual call into the disclosed typed source alias; "
                      "bridge reflection differences are reported."
                : PlainBridge->second.guarded_default_result
                    ? "The original type guard returns the unchanged default "
                      "before any default cast; checked arguments and the "
                      "typed virtual call retain their original order. Raw "
                      "source ancestry and bridge flags are reported."
                : PlainBridge->second.raw_parameter_forwarding
                    ? "The proven original parameter bridge remains an "
                      "explicit method under the reported raw source parent; "
                      "its original casts and virtual call are retained."
                : PlainBridge->second.raw_unit_result
                    ? "The original raw callback interface, both method "
                      "descriptors, "
                      "ordered reference casts, any original Number.intValue "
                      "calls, void delegation and post-call "
                      "Unit "
                      "field read remain unchanged; bridge and synthetic "
                      "reflection "
                      "differences are reported."
                : PlainBridge->second.raw_integer_result
                    ? "The original raw callback, both descriptors, ordered "
                      "Number casts, intValue calls and reference result are "
                      "retained without a generic scope change; bridge and "
                      "synthetic reflection differences are reported."
                : !PlainBridge->second.raw_indexed_parameters.empty()
                    ? "The original raw callback, both descriptors, Number "
                      "casts and numeric conversions retain their original order. "
                      "The target reuses the reported default-method scope; "
                      "bridge and synthetic reflection differences are "
                      "reported."
                : PlainBridge->second.superclass_reference_adapter
                    ? "The original widened superclass argument, invocation, "
                      "checked reference result and complete scoped Signature "
                      "are emitted unchanged; bridge reflection differences "
                      "are reported."
                : PlainBridge->second.superclass_scalar_adapter
                    ? "The original widened superclass argument, invocation "
                      "and scalar return are emitted unchanged; bridge "
                      "reflection "
                      "differences are reported."
                : PlainBridge->second.superclass_getter
                    ? (!isReference(R.returns)
                           ? "The original superclass invocation and scalar "
                             "return are emitted unchanged; bridge reflection "
                             "differences are reported."
                           : "The original superclass invocation and reference "
                             "return are emitted unchanged; the proven scoped "
                             "result and bridge reflection differences are "
                             "reported.")
                : PlainBridge->second.primitive_void
                    ? "The original checked cast, broad primitive unboxing "
                      "and interface default delegation are emitted unchanged; "
                      "Java cannot retain the bridge reflection flag."
                : PlainBridge->second.generic_dispatch
                    ? "The original differently named virtual setter call "
                      "and unchanged argument are emitted with both original "
                      "runtime descriptors and complete target Signature; "
                      "Java cannot retain the bridge reflection flag."
                : PlainBridge->second.value_carrier
                    ? "The original checked casts, virtual unboxing calls, "
                      "primitive delegation and result forwarding, "
                      "including any original static boxing call, are emitted "
                      "unchanged; Java cannot retain the bridge reflection "
                      "flag."
                : PlainBridge->second.interface_super_forwarder
                    ? "The original direct-interface super invocation, exact "
                      "argument words and return retain their default "
                      "provider; "
                      "Java cannot retain the bridge reflection flag."
                : PlainBridge->second.closed_reference_getter
                    ? "The original virtual getter invocation, raw reference "
                      "return and provider stay unchanged; the complete "
                      "closed target Signature proves the inherited result, "
                      "and only bridge reflection flags differ."
                : PlainBridge->second.reference_getter
                    ? "The original virtual getter invocation, reference "
                      "return and declared result type are emitted "
                      "unchanged; Java cannot retain the bridge reflection "
                      "flag."
                : PlainBridge->second.terminal_throw
                    ? "The original exception allocation, constructor and "
                      "throw are emitted unchanged without a parameter cast; "
                      "Java cannot retain the bridge reflection flag."
                : PlainBridge->second.guarded_reference
                    ? "The original guard, negative result, checked cast and "
                      "virtual delegation are emitted unchanged; Java cannot "
                      "retain the bridge reflection flag."
                    : "The original virtual forwarding invocation and its "
                      "primitive return are emitted unchanged; Java cannot "
                      "retain the "
                      "bridge reflection flag.";
          } else if (IntegerBridge) {
            Row["reason"] = "Original casts, primitive unboxing calls, void "
                            "delegation and Unit field read are emitted "
                            "unchanged with both original runtime descriptors.";
          } else if (UnitBridge) {
            Row["reason"] = "The original checked call and Unit field read are "
                            "emitted in a typed wrapper; Java supplies the "
                            "erased forwarding bridge.";
          } else if (VoidTarget != Inheritance.void_unit_targets.end()) {
            Row["projection_kind"] = "void-unit-target";
            auto SourceReference = R;
            SourceReference.name = SourceName;
            Row["source_identity"] = SourceReference.identity();
            Row["bridge_source_name"] = SourceName;
            Row["metadata_differences"] = llvm::json::Array{
                "the original void method has a distinct Java source name; "
                "its original runtime name cannot be regenerated",
                "typed input callers share this name mapping and do not "
                "read Unit.INSTANCE"};
            Row["reason"] = "The complete original void body is emitted with "
                            "one shared source name in its final class.";
          }
          ++Projected;
        } else if (EnumProjection || EnumConstantSite) {
          Row["status"] = "source-projected";
          Row["projection_kind"] =
              EnumProjection ? EnumProjection->kind : "enum-constant-body";
          Row["reason"] =
              "Original body is emitted in its enum declaration; the compiler "
              "supplies verified hidden constructor parameters and "
              "constant-subclass identity.";
          ++Projected;
        } else if (IndependentInner) {
          Row["status"] = "source-projected";
          Row["projection_kind"] = "independent-synthetic-member";
          Row["source_identity"] = R.identity();
          Row["reason"] =
              "Original bodies, explicit constructor parameters and effects "
              "are retained; static and synthetic class metadata differences "
              "are recorded in class_source_bindings.";
          ++Projected;
        } else if (R.name == "<init>" && has(M.access, "synthetic") &&
                   !CapturedInner &&
                   !Projection.projected_methods.contains(R)) {
          Row["status"] = "source-projected";
          Row["projection_kind"] = "synthetic-constructor";
          Row["source_identity"] = R.identity();
          Row["reason"] = Generics.constructor_delegations.contains(R)
                              ? "The shared constructor chain preserves "
                                "original argument effects and primary "
                                "final-field writes."
                              : "The complete verified original constructor "
                                "body and field initialization remain "
                                "unchanged; its synthetic flag cannot be "
                                "expressed by Java source.";
          ++Projected;
        } else if (CapturedInner) {
          Row["status"] = "source-projected";
          Row["projection_kind"] = "inner-to-static-member";
          Row["reason"] =
              "Original body, outer-instance field and explicit constructor "
              "parameters are retained in a static member; reflection "
              "differences are recorded in class_source_bindings.";
          ++Projected;
        } else if (Projection.projected_methods.contains(R)) {
          Row["status"] = "source-projected";
          Row["projection_kind"] = "named-method-local";
          Row["reason"] =
              "Method body and lexical class scope were emitted; recompiled "
              "local-class binary identity and access flags are unverified.";
          if (Projection.anonymous_methods.contains(R)) {
            Row["projection_kind"] = "anonymous-to-named-local";
            Row["reason"] =
                "Body, captures and explicit constructor arguments are emitted "
                "in a named local class; binary names and anonymous/local "
                "reflection metadata change as recorded in "
                "class_source_bindings.";
          }
          if (Projection.shared_methods.contains(R)) {
            Row["projection_kind"] = "anonymous-to-shared-member";
            Row["reason"] = "All callers share one named static member class; "
                            "bodies, captures "
                            "and explicit constructor arguments are retained. "
                            "Source ownership "
                            "and reflection differences are recorded in "
                            "class_source_bindings.";
          }
          if (Projection.member_methods.contains(R)) {
            Row["projection_kind"] = "member-of-projected-class";
            Row["reason"] =
                "Body is preserved in a static member whose enclosing "
                "source projection changes its binary name.";
          }
          if (Projection.anonymous_member_methods.contains(R)) {
            Row["projection_kind"] = "anonymous-to-named-member";
            Row["reason"] =
                "Bodies, capture fields and explicit constructor arguments are "
                "retained in a named static member; binary names and "
                "reflection "
                "differences are recorded in class_source_bindings.";
          }
          ++Projected;
        } else if (AnonymousMethods.contains(R)) {
          Row["status"] = "source-projected";
          Row["projection_kind"] = "anonymous-interface";
          Row["reason"] =
              "Body and unique anonymous allocation are emitted; "
              "compiler-selected anonymous binary identity remains unverified.";
          ++Projected;
        } else {
          Row["status"] = "recovered";
          ++Recovered;
        }
      }
      if (Inheritance.static_return_synthetic_methods.contains(R)) {
        if (const auto Status = Row.getString("status");
            Status && *Status == "recovered") {
          Row["status"] = "source-projected";
          Row["projection_kind"] = "synthetic-static-return-slot";
          Row["source_identity"] = R.identity();
          Row["reason"] =
              "The admitted static return-only group retains this original "
              "name, descriptor, Signature and body; Java source cannot "
              "express its original synthetic flag.";
          --Recovered;
          ++Projected;
        }
        if (!Row.getArray("metadata_differences"))
          Row["metadata_differences"] = llvm::json::Array{};
        Row.getArray("metadata_differences")
            ->push_back("Java source cannot reproduce the original synthetic "
                        "access flag; the complete original descriptor, "
                        "Signature, Exceptions, remaining access flags and "
                        "body are retained");
      }
      RecordReturnSlotName();
      // Ordinary retained methods can lose a synthetic flag independently of
      // naming, forwarding or generic adaptation. Disclose that metadata loss
      // even when the complete original descriptor and body need no projection.
      if (has(M.access, "synthetic") && R.name != "<init>" &&
          Row.getString("status") == "recovered") {
        Row["status"] = "source-projected";
        Row["projection_kind"] = "synthetic-method";
        Row["source_identity"] = R.identity();
        Row["reason"] = "The original method declaration and body remain; "
                        "Java source cannot express its synthetic access flag.";
        if (!Row.getArray("metadata_differences"))
          Row["metadata_differences"] = llvm::json::Array{};
        Row.getArray("metadata_differences")
            ->push_back("Java source retains the original method descriptor "
                        "and body but cannot reproduce its synthetic access "
                        "flag");
        --Recovered;
        ++Projected;
      }
      if (ProjectConstructorExceptions) {
        const auto &Contract = ConstructorExceptions->second;
        llvm::json::Array SourceThrows;
        for (const auto &Type : Contract.source_declared_throws)
          SourceThrows.push_back(Type);
        Row["source_constructor_declared_throws"] = std::move(SourceThrows);
        Row["constructor_exception_source"] = Contract.target.identity();
        Row["constructor_exception_call_pc"] = int64_t(Contract.call_pc);
        if (!Row.getArray("metadata_differences"))
          Row["metadata_differences"] = llvm::json::Array{};
        Row.getArray("metadata_differences")
            ->push_back("the original absent or empty constructor Exceptions "
                        "declaration gains the exact checked superclass "
                        "exceptions required by the named local source "
                        "constructor; original captures, delegation and "
                        "exception objects are retained");
      }
      // Constructor flags are independent of lexical class projection. A
      // shared anonymous body may already own the row's projection kind, but
      // javac still cannot reproduce the original synthetic constructor flag.
      if (R.name == "<init>" && has(M.access, "synthetic")) {
        if (!Row.getArray("metadata_differences"))
          Row["metadata_differences"] = llvm::json::Array{};
        Row.getArray("metadata_differences")
            ->push_back("Java source retains the original constructor "
                        "descriptor and body but cannot reproduce its "
                        "synthetic access flag");
      }
      recordMethod();
    }
    if (HasStaticFieldInitializer && !HasClassInitializer)
      auxiliary(C, "<clinit>", "()V", true, Unit, "field-initializer");
    if (Enum)
      for (const auto &Constant : Enums.classes.at(C.name).constants)
        for (const auto &Argument : Constant.argument_helpers) {
          if (!Argument)
            continue;
          const auto &Helper = *Argument;
          Body Emitter(Helper, Classes, B, TypeNames, Generics, Projection,
                       Inheritance, AnonymousSources);
          auto Source = Emitter.emit();
          BodyLines.append("  private static " +
                           TypeNames.render(Helper.reference.returns, C) + " " +
                           sourceMethodName(Helper.reference, Inheritance) +
                           "() {");
          for (const auto &Line : split(Source, '\n'))
            BodyLines.append("    " + Line);
          BodyLines.append("  }");
          auxiliary(C, Helper.reference.name, Helper.reference.signature(),
                    true, Unit, "enum-argument-evaluation");
        }
    const bool InterfaceBodies =
        Kind == "interface" &&
        std::any_of(C.methods.begin(), C.methods.end(), [](const auto &Method) {
          return Method.reference.name != "<clinit>" &&
                 !has(Method.access, "abstract");
        });
    if (Kind == "class" || Enum || InterfaceBodies) {
      // Every emitted class contains runtime helper references, even when
      // its methods only mention primitive types. Validate their fixed root
      // just like descriptor-derived source types before publication.
      TypeNames.requireRuntimePackage(C);
      bool HasConstructor =
          std::any_of(C.methods.begin(), C.methods.end(), [](const auto &M) {
            return M.reference.name == "<init>";
          });
      if (Kind == "class" && !HasConstructor && !AnonymousSite && !Enum) {
        BodyLines.append("  private " + Name + "() {}");
        const auto Prototype =
            OrdinaryStaticOnlyInner ? "(" + *C.enclosing + ")V" : "()V";
        auxiliary(C, "<init>", Prototype, false, Unit, "default-constructor");
      }
      std::string Helper = helperName(C, "__neverdThrow");
      if (InterfaceBodies)
        MinimumJavaRelease = std::max(MinimumJavaRelease, 9u);
      BodyLines.extend({"  @java.lang.SuppressWarnings(\"unchecked\")",
                        std::string("  private ") +
                            (StaticHelpers ? "static " : "") +
                            "<E extends java.lang.Throwable> "
                            "java.lang.RuntimeException " +
                            Helper + "(java.lang.Throwable failure) throws E {",
                        "    if (failure == null) throw new "
                        "java.lang.NullPointerException();",
                        "    throw (E) failure;", "  }"});
      auxiliary(C, Helper,
                "(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;",
                StaticHelpers, Unit, "throw-helper");
    }
    llvm::json::Array EncodedConstants;
    for (const auto &F : C.fields) {
      const auto Reader = Enums.encoded_constants.find(F.reference);
      if (Reader == Enums.encoded_constants.end())
        continue;
      auto Access =
          join(modifiers(F.access, {"public", "protected", "private"}), " ");
      if (!Access.empty())
        Access += " ";
      BodyLines.append(
          "  " + Access + "static " + TypeNames.render(F.reference.type, C) +
          " " + Reader->second + "() { return " +
          sourceFieldName(F.reference, Classes, Inheritance, B) + "; }");
      auxiliary(C, Reader->second, "()" + F.reference.type, true, Unit,
                "enum-encoded-value-read");
      EncodedConstants.push_back(
          llvm::json::Object{{"field", F.reference.name},
                             {"type", F.reference.type},
                             {"read_helper", Reader->second}});
    }
    if (!EncodedConstants.empty())
      for (auto &Value : EnumBindings) {
        auto &Binding = *Value.getAsObject();
        if (Binding.getString("class") != C.name ||
            Binding.getString("binding_kind") != "enum-declaration")
          continue;
        Binding["encoded_constants"] = std::move(EncodedConstants);
        Binding.getArray("metadata_differences")
            ->push_back(
                "final int/long/String encoded values use Java ConstantValue "
                "before constant allocation; "
                "original static field reads use generated accessors "
                "to retain class initialization");
        break;
      }
    for (const auto &T : ConstantTypes) {
      auto NameType = TypeNames.render(T, C);
      BodyLines.append("  private static " + NameType + " " +
                       helperName(C, "__neverdConstant") + "(" + NameType +
                       " value) { return value; }");
      auxiliary(C, helperName(C, "__neverdConstant"), "(" + T + ")" + T, true,
                Unit, "constant-helper");
    }
    if (auto I = Enclosing.find(C.name); I != Enclosing.end()) {
      auto Children = I->second;
      std::sort(Children.begin(), Children.end(),
                [](const auto *A, const auto *B) { return A->name < B->name; });
      for (const auto *Child : Children)
        for (const auto &Line :
             split(emitClass(*Child, DeclarationSite::Member, Unit), '\n'))
          BodyLines.append("  " + Line);
    }
    BodyLines.append("}");
    Emitting.erase(C.name);
    return BodyLines.text();
  };
  for (const auto &[Name, Site] : Anonymous) {
    auto Root = *Classes.at(Name).enclosing;
    while (Classes.at(Root).enclosing) {
      B.tick();
      Root = *Classes.at(Root).enclosing;
    }
    auto Unit = Root.substr(1, Root.size() - 2) + ".java";
    AnonymousSources.emplace(
        Name, emitClass(Classes.at(Name), DeclarationSite::Anonymous, Unit));
  }
  uint64_t MethodCount = 0, ClassCount = 0;
  for (const auto &[Name, C] : Classes) {
    if (C.external)
      continue;
    ++ClassCount;
    MethodCount += C.methods.size();
    if (C.enclosing || C.enclosing_method)
      continue;
    std::string Path = C.name.substr(1, C.name.size() - 2);
    size_t Slash = Path.rfind('/');
    std::string Prefix;
    if (Slash != std::string::npos) {
      auto Package = split(std::string_view(Path).substr(0, Slash), '/');
      for (auto &Part : Package)
        Part = javaIdentifier(Part);
      Prefix = "package " + join(Package, ".") + ";\n\n";
    }
    std::string Source;
    if (PackageMetadata.contains(Name)) {
      if (Prefix.empty() || !Emitted.insert(Name).second)
        javaError("package metadata lost its named package or unique owner");
      Source = Prefix;
      PackageMetadataBindings.push_back(llvm::json::Object{
          {"class", Name},
          {"input", C.source_id},
          {"source_unit", Path + ".java"},
          {"kind", "empty-package-info"},
          {"compiler_option", "-Xpkginfo:always"},
          {"access_flags",
           llvm::json::Array{"abstract", "interface", "synthetic"}}});
    } else {
      Source = Prefix +
               emitClass(C, DeclarationSite::TopLevel, Path + ".java") + "\n";
    }
    Units.push_back(llvm::json::Object{{"path", Path + ".java"},
                                       {"class", C.name},
                                       {"source", std::move(Source)}});
  }
  if (Methods.size() != MethodCount || Emitted.size() != ClassCount ||
      MethodCount != Recovered + Projected + Declared)
    javaError("Android class ownership omitted declared methods");
  if (Units.empty())
    javaError("Android input contains no source classes");
  llvm::json::Object Report{
      {"schema_version", 1},
      {"status", Projected || HasFieldIdentifierProjection ||
                         !Projection.anonymous_member_classes.empty() ||
                         !GenericProjections.empty()
                     ? "partial"
                     : "recovered"},
      {"class_count", static_cast<int64_t>(ClassCount)},
      {"method_count", static_cast<int64_t>(Methods.size())},
      {"recovered_method_count", static_cast<int64_t>(Recovered)},
      {"declaration_only_method_count", static_cast<int64_t>(Declared)},
      {"unrecovered_method_count", 0},
      {"methods", std::move(Methods)},
      {"source_units", std::move(Units)}};
  if (!PackageMetadata.empty()) {
    if (PackageMetadataBindings.size() != PackageMetadata.size())
      javaError("package metadata omitted an original compiler identity");
    Report["java_compiler_options"] = llvm::json::Array{"-Xpkginfo:always"};
    Report["package_metadata_classes"] = std::move(PackageMetadataBindings);
  }
  if (MinimumJavaRelease > 8)
    Report["minimum_java_release"] = MinimumJavaRelease;
  if (!DefaultInstanceInitializers.empty())
    Report["default_instance_final_initializers"] =
        std::move(DefaultInstanceInitializers);
  // Class projection and bridge generation can describe the same method.
  // Audit original identities at the point where their source rows exist,
  // independently of which proof supplied the row or the class binding.
  if (emittedLocalMethods != Projection.projected_methods)
    javaError("Android local method projection omitted an original identity");
  std::map<std::string, size_t> expectedBindings;
  for (const auto &[name, cls] : Projection.by_class)
    ++expectedBindings[name];
  for (const auto &name : Projection.member_classes)
    ++expectedBindings[name];
  for (const auto &name : Projection.anonymous_member_classes)
    ++expectedBindings[name];
  for (const auto &[name, field] : Generics.captured_inner_classes)
    ++expectedBindings[name];
  for (const auto &name : Generics.independent_inner_classes)
    ++expectedBindings[name];
  for (const auto &name : Projection.ordinary_static_only_members)
    ++expectedBindings[name];
  for (const auto &entry : Bindings) {
    const auto name = entry.getAsObject()->getString("class")->str();
    auto expected = expectedBindings.find(name);
    if (expected == expectedBindings.end())
      javaError("unexpected local or member class binding: " + name);
    if (!--expected->second)
      expectedBindings.erase(expected);
  }
  if (!expectedBindings.empty())
    javaError("missing local or member class binding: " + expectedBindings.begin()->first);
  if (Projected)
    Report["projected_method_count"] = static_cast<int64_t>(Projected);
  if (!Anonymous.empty()) {
    if (AnonymousBindings.size() != Anonymous.size())
      javaError("anonymous projection omitted an original class identity");
    for (auto &Binding : AnonymousBindings)
      Bindings.push_back(std::move(Binding));
  }
  if (EnumBindings.size() !=
      Enums.classes.size() + Enums.constant_classes.size())
    javaError("enum projection omitted an original class identity");
  for (auto &Binding : EnumBindings)
    Bindings.push_back(std::move(Binding));
  if (!Bindings.empty())
    Report["class_source_bindings"] = std::move(Bindings);
  llvm::json::Array Visibility;
  for (const auto &[name, access] : MemberAccess) {
    if (!access.interface_member && access.descriptor_users.empty() && access.instruction_users.empty())
      continue;
    llvm::json::Array users, instructions;
    for (const auto &user : access.descriptor_users)
      users.push_back(user);
    for (const auto &user : access.instruction_users)
      instructions.push_back(user);
    Visibility.push_back(llvm::json::Object{
        {"class", name},
        {"original_inner_visibility", has(Classes.at(name).inner_access, "protected") ? "protected" : "private"},
        {"source_visibility", access.interface_member ? "public" : "package"},
        {"descriptor_users", std::move(users)}, {"instruction_users", std::move(instructions)},
        {"metadata_difference", access.interface_member
             ? "Java requires a public member type in an interface"
             : "InnerClasses visibility follows the original package-accessible class definition"}});
  }
  if (!Visibility.empty())
    Report["member_visibility_projections"] = std::move(Visibility);
  if (!documentedContainers.empty()) {
    llvm::json::Array policies;
    for (const auto &[container, repeated] : documentedContainers)
      policies.push_back(llvm::json::Object{
          {"class", container}, {"repeated_annotation", repeated},
          {"source_annotation", "Ljava/lang/annotation/Documented;"},
          {"metadata_difference", "Java requires the documented repeated annotation's container to be documented"}});
    Report["repeatable_container_projections"] = std::move(policies);
  }
  if (!Inheritance.source_field_names.empty() ||
      !RuntimeFinalBindings.empty()) {
    llvm::json::Array Fields = std::move(RuntimeFinalBindings);
    for (const auto &[Ref, Name] : Inheritance.source_field_names) {
      B.tick();
      const auto *Field = member(Ref, Classes, B);
      llvm::json::Array Constructors, Flags;
      for (const auto &Flag : Field->access)
        Flags.push_back(Flag);
      for (const auto &[Ctor, Captures] : Generics.constructor_captures)
        for (const auto &[Parameter, Capture] : Captures) {
          B.tick();
          if (Capture == Ref)
            Constructors.push_back(llvm::json::Object{
                {"identity", Ctor.identity()},
                {"parameter_index", static_cast<int64_t>(Parameter)}});
        }
      const bool Private =
          has(Field->access, "private") && !has(Field->access, "synthetic");
      const bool Static = has(Field->access, "static");
      const std::string Kind =
          Private ? (Static ? "private static" : "private instance")
                  : "captured";
      llvm::json::Array Differences;
      Differences.push_back(
          "the original " + Kind +
          " field has a distinct Java source name; "
          "field-name reflection and binary ABI cannot be regenerated");
      if (!Private)
        Differences.push_back(
            "Java source cannot reproduce the original synthetic field flag");
      Differences.push_back("original language metadata is retained but field "
                            "names may no longer match");
      Fields.push_back(llvm::json::Object{
          {"class", Ref.owner},
          {"original_identity", Ref.owner + "->" + Ref.name + ":" + Ref.type},
          {"original_name", Ref.name},
          {"type", Ref.type},
          {"original_access", std::move(Flags)},
          {"source_name", Name},
          {"source_identity", Ref.owner + "->" + Name + ":" + Ref.type},
          {"binding_kind", Private
                               ? (Static ? "private-static-field-identifier"
                                         : "private-instance-field-identifier")
                               : "captured-field-identifier"},
          {"capture_constructors", std::move(Constructors)},
          {"metadata_differences", std::move(Differences)}});
    }
    Report["field_source_bindings"] = std::move(Fields);
  }

  if (!Helpers.empty() || !Projection.by_class.empty() ||
      !Projection.anonymous_member_classes.empty() || !Anonymous.empty() ||
      !Enums.classes.empty() || !Generics.captured_inner_classes.empty() ||
      HasDeclarationMetadata ||
      !Inheritance.generated_interface_bridges.empty() ||
      !Inheritance.generated_class_bridges.empty())
    Report["generated_source_helpers"] = std::move(Helpers);
  if (!RuntimeFinalManifest.empty()) {
    Report["requires_java_rebuild_postprocessing"] = true;
    Report["java_rebuild"] = llvm::json::Object{
        {"utility_source", std::string(JavaFieldRestorePath)},
        {"field_manifest", std::string(JavaFieldRestoreManifestPath)},
        {"compile", llvm::json::Array{"javac", "-d", "<helper-classes>",
                                      std::string(JavaFieldRestorePath)}},
        {"run", llvm::json::Array{"java", "-cp", "<helper-classes>",
                                  "RestoreFieldFlags", "<application-classes>",
                                  std::string(JavaFieldRestoreManifestPath)}},
        {"application_code_execution", false},
        {"reason", "The rebuild utility restores original field modifiers "
                   "after javac emits the original field operations."}};
    Report["java_rebuild_artifacts"] = llvm::json::Array{
        llvm::json::Object{{"path", std::string(JavaFieldRestorePath)},
                           {"content", std::string(JavaFieldRestoreSource)},
                           {"role", "metadata-build-utility"}},
        llvm::json::Object{{"path", std::string(JavaFieldRestoreManifestPath)},
                           {"content", RuntimeFinalManifest},
                           {"role", "metadata-build-manifest"}}};
  }
  if (!DebugMetadata.empty())
    Report["class_debug_metadata"] = std::move(DebugMetadata);
  if (!GenericProjections.empty())
    Report["generic_signature_projections"] = std::move(GenericProjections);
  return Report;
}
} // namespace neverd::mobile::dalvik
