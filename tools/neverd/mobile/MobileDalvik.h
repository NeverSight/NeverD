//===- MobileDalvik.h - Typed native Dalvik recovery model
//-----------------===//
#pragma once

#include "MobileCommon.h"

#include <compare>
#include <map>
#include <set>
#include <variant>

namespace neverd::mobile::dalvik {
using Access = std::set<std::string>;
bool has(const Access &access, std::string_view flag);
std::string descriptor(std::string_view value, bool allow_void = false);
std::pair<std::vector<std::string>, std::string>
prototype(std::string_view value);
unsigned width(std::string_view type);
// Exact scalar conversion source/destination descriptors shared by ownership
// planning, ordinary verification and allocation emission.
std::optional<std::pair<std::string, std::string>>
scalarConversion(std::string_view opcode);
// Source operand type and Java operator for total unary arithmetic.
std::optional<std::pair<std::string, std::string>>
scalarUnary(std::string_view opcode);
// Comparisons produce one int word. Only the two inputs may be wide; the
// exact NaN direction belongs to the same opcode receipt for all consumers.
struct ScalarComparison {
  std::string type;
  bool greater_on_nan;
};
std::optional<ScalarComparison> scalarComparison(std::string_view opcode);
// Exact conditional opcode, relation and operand form shared by ownership
// planning and ordinary Body verification.
struct ConditionalBranch {
  std::string symbol;
  bool zero;
};
std::optional<ConditionalBranch> conditionalBranch(std::string_view opcode);
// Exact nonthrowing int arithmetic/bitwise forms. The same receipt owns
// constructor preparation, ordinary statements and allocation expressions.
struct IntegerPreparation {
  std::string symbol;
  bool two_address, literal;
  bool literal_first = false;
};
std::optional<IntegerPreparation> integerPreparation(std::string_view opcode);
struct Instruction;
struct IntegerDivision {
  std::string symbol;
  bool two_address, literal, can_throw;
};
// One decoded authority owns exact int division/remainder forms, signed literal
// widths and possible zero-divisor failure. Preparation remains total-only.
std::optional<IntegerDivision> integerDivision(const Instruction &op);
// Division/remainder also require the original, valid nonzero literal. An
// opcode alone cannot prove those preparations nonthrowing.
std::optional<IntegerPreparation> integerPreparation(const Instruction &op);

Access accessFlags(uint32_t value);

struct MethodRef {
  std::string owner;
  std::string name;
  std::vector<std::string> parameters;
  std::string returns;
  std::string signature() const;
  std::string identity() const;
  auto operator<=>(const MethodRef &) const = default;
};
struct FieldRef {
  std::string owner;
  std::string name;
  std::string type;
  auto operator<=>(const FieldRef &) const = default;
};
MethodRef methodRef(std::string_view value);
FieldRef fieldRef(std::string_view value);

// String literals contain UTF-8, with isolated UTF-16 surrogates represented
// in WTF-8 until the Java string emitter escapes their exact code units.
using Literal = std::variant<std::monostate, int64_t, std::string>;
using Reference =
    std::variant<std::monostate, MethodRef, FieldRef, std::string>;
struct FloatBits {
  uint64_t bits = 0;
  bool wide = false;
};
using FieldValue =
    std::variant<std::monostate, int64_t, std::string, bool, FloatBits>;

// DEX encoded-value tags retain the exact scalar kind until declaration
// linking verifies it against the annotation element's return descriptor.
struct AnnotationValue {
  unsigned kind = 0;
  FieldValue value;
  std::vector<AnnotationValue> array;
  std::string annotation_type;
  std::map<std::string, AnnotationValue> elements;
};
struct AnnotationUse {
  std::string type;
  unsigned visibility = 1;
  std::map<std::string, AnnotationValue> elements;
};

struct Instruction {
  uint32_t pc = 0;
  std::string opcode;
  std::vector<unsigned> registers;
  Literal literal;
  std::optional<uint32_t> target;
  Reference reference;
  std::vector<int32_t> keys;
  std::vector<uint32_t> targets;
  std::vector<uint64_t> data;
  unsigned element_width = 0;
};
struct Handler {
  std::optional<std::string> type;
  uint32_t target = 0;
};
struct TryRegion {
  uint32_t start = 0;
  uint32_t end = 0;
  std::vector<Handler> handlers;
};
struct ParameterMetadata {
  std::optional<std::string> name;
  uint32_t access = 0;
};
struct Method {
  MethodRef reference;
  Access access;
  unsigned registers = 0;
  std::vector<Instruction> instructions;
  std::vector<TryRegion> tries;
  uint32_t code_end = 0;
  unsigned incomingWords() const;
  std::optional<std::string> generic_signature;
  std::optional<std::vector<std::string>> declared_throws;
  bool deprecated = false;
  std::optional<std::vector<std::string>> suppress_lint;
  std::vector<AnnotationUse> annotations;
  std::vector<std::vector<AnnotationUse>> parameter_annotations;
  std::optional<AnnotationValue> annotation_default;
  std::optional<std::vector<ParameterMetadata>> parameter_metadata;
};
struct Field {
  FieldRef reference;
  Access access;
  FieldValue value;
  std::optional<std::string> generic_signature;
  bool deprecated = false;
  std::optional<std::vector<std::string>> suppress_lint;
  std::vector<AnnotationUse> annotations;
};
// Exact VM preparation defaults, including explicit typed encoded zero.
// Negative floating zero and nonzero/non-null values are not defaults.
bool defaultFieldValue(const Field &field);
struct AnnotationMetadata {
  std::optional<std::string> retention;
  std::optional<std::vector<std::string>> targets;
  bool documented = false;
  bool inherited = false;
  bool empty() const {
    return !retention && !targets && !documented && !inherited;
  }
};
struct MarkerAnnotation {
  std::string type;
  unsigned visibility = 1;
};
struct Class {
  std::string name;
  std::optional<std::string> superclass;
  Access access;
  std::string source_id;
  std::vector<std::string> interfaces;
  std::vector<Field> fields;
  std::vector<Method> methods;
  std::optional<std::string> enclosing;
  std::optional<std::string> inner_name;
  Access inner_access;
  bool inner_class_present = false;
  std::optional<MethodRef> enclosing_method;
  std::optional<std::string> generic_signature;
  bool deprecated = false;
  AnnotationMetadata annotation_metadata;
  std::optional<std::string> source_debug_extension;
  std::vector<MarkerAnnotation> marker_annotations;
  std::optional<std::vector<std::string>> suppress_lint;
  std::vector<AnnotationUse> annotations;
  // Dependency classfiles contribute declaration facts only. Their bytecode
  // is never recovered or counted in the input's source coverage.
  bool external = false;
  // InnerClasses facts in a dependency owner expose missing nested entries;
  // absent classfiles must not erase an inherited source-name collision.
  std::map<std::string, std::string> declared_member_types;
};
using ClassMap = std::map<std::string, Class>;
enum class FieldLookupStatus { UnknownOwner, Missing, TypeMismatch, Found };
struct FieldLookup {
  FieldLookupStatus status;
  const Field *declaration;
};
// Shared existing field selection for ordinary operations and constructor
// receipts. Callers retain the symbolic owner and separately prove access.
FieldLookup lookupField(const FieldRef &reference, const ClassMap &classes,
                        Budget &budget);
bool arrayLoad(std::string_view opcode);
bool isAssignableReference(const std::string &actual, const std::string &wanted,
                           const ClassMap &classes, Budget &budget);
// Minimum source release, with NeverD's Java 8 baseline; zero means unknown.
unsigned annotationTargetJavaRelease(std::string_view target);
bool isAnonymous(const Class &cls);
bool isAnonymousLocal(const Class &cls);
bool isConstructorlessAnonymousMember(const Class &cls);
using NamedAnonymousMembers = std::set<std::string>;
NamedAnonymousMembers planNamedAnonymousMembers(const ClassMap &classes,
                                                Budget &budget);
bool isNamedAnonymousMember(const Class &cls,
                            const NamedAnonymousMembers &members);
bool isStaticOnlyInnerMember(const Class &cls);
// Named instance members whose constructors store their first explicit
// parameter into the same synthetic final enclosing-instance field before
// delegation, or forward it through a proven pure this-constructor prefix.
// Source retains that parameter and storage in a static member.
using CapturedInnerClasses = std::map<std::string, FieldRef>;
using ConstructorCaptures = std::map<MethodRef, std::map<size_t, FieldRef>>;
using ConstructorDelegations = std::map<MethodRef, MethodRef>;
// A bounded primary prefix owns its exact superclass call and final stores.
// This is not a this-delegation edge and cannot discharge initialization
// duties.
struct ConstructorSuperPrologue {
  MethodRef target;
  uint32_t call_pc;
  // Linear prefixes may retain their existing expression emission. A client
  // that inserts before-super statements can still consume the complete
  // structural receipt without reimplementing the uninitialized-this proof.
  bool requires_dispatch = true;
};
using ConstructorSuperPrologues = std::map<MethodRef, ConstructorSuperPrologue>;
// Classify value-producing prefix calls; ordinary invocation validation still
// owns declaration/access, receiver, word layout, result and exception proofs.
bool constructorPrefixValueCall(const Instruction &op);
// An original filled int/reference array and its adjacent result share one
// local/reference-parameter slot. Origin/type proofs still own all operands.
std::optional<unsigned> constructorPrefixFilledArray(const Method &method,
                                                     size_t instruction);
// A fresh local reference immediately initialized by its exact no-arg
// constructor. Source emission separately checks the class and declaration.
std::optional<MethodRef> constructorPrefixAllocation(const Method &method,
                                                     size_t instruction);
// The same exact fresh receiver may have a closed forward argument preparation
// before its initializer. The ordinary expression planner proves operation,
// type and effect semantics; no edge may enter or leave the uninitialized gap.
std::optional<size_t> constructorPrefixAllocationEnd(const Method &method,
                                                     size_t instruction,
                                                     Budget &budget);
// One shared selection skips proven argument allocations before identifying the
// actual this/super call. An argument may invoke the same constructor
// descriptor.
struct ConstructorPrefixDelegation {
  size_t call;
  std::map<size_t, size_t> allocations;
  // Closed allocation arguments may need ordinary operation verification even
  // without branches. Every prefix consumer uses this shared dispatch choice.
  bool requires_dispatch = false;
};
std::optional<ConstructorPrefixDelegation>
selectConstructorPrefixDelegation(const Method &method, Budget &budget);
struct LongBinary {
  std::string symbol, right_type;
  bool two_address, can_throw;
};
// Exact long arithmetic/bitwise forms, with int shift distances and the
// division/remainder exception fact. Total-only regions check can_throw.
std::optional<LongBinary> longBinary(std::string_view opcode);

struct FloatingArithmetic {
  std::string type, symbol;
  bool two_address;
};
// Exact total floating arithmetic, including division/remainder at zero.
std::optional<FloatingArithmetic> floatingArithmetic(std::string_view opcode);

struct ConstructorPrefixConstant {
  unsigned destination;
  std::string type;
  int64_t bits;
};
// Exact integer load names, decoded bits, complete operands and physical frame.
// Constructor ownership and prefix boundaries remain separate checks.
std::optional<ConstructorPrefixConstant> integerConstant(const Instruction &op,
                                                         unsigned registers);
// Exact integer bit-pattern loads have no resolution or evaluation effect.
std::optional<ConstructorPrefixConstant>
constructorPrefixIntegerConstant(const Method &method, const Instruction &op);
struct ConstructorPrefixReferenceArrayLoop {
  size_t end;
  unsigned array, length, index, element;
  std::string array_type;
  MethodRef initializer;
};
// One original reference array is filled by a zero-based, unit-step loop of
// exact adjacent nullary allocations. No edge may enter its interior. Generic
// binding and ordinary operation/type/effect validation remain separate.
std::optional<ConstructorPrefixReferenceArrayLoop>
constructorPrefixReferenceArrayLoop(const Method &method, size_t instruction,
                                    Budget &budget);
struct ConstructorPrefixCopy {
  unsigned destination, source, words;
  std::string kind;
};
// Exact copy forms and their complete physical word frame. Consumers retain
// the source type/origin proof, including uninitialized receiver aliases.
std::optional<ConstructorPrefixCopy>
constructorPrefixCopy(const Method &method, const Instruction &op);
// Exact empty instruction shape; reachability and physical PC ordering remain
// separate proofs. Payloads and operands cannot be discarded as padding.
bool emptyNop(const Instruction &op);
// The allocation proof composes nonthrowing literals, copies and exact empty
// instructions. Consumers retain every write and the original initializer.
std::optional<size_t>
constructorPrefixAllocationArgumentsEnd(const Method &method,
                                        size_t instruction, Budget &budget);
// A reference class literal in a fresh local resolves the Class object without
// initializing its target. Every prefix consumer shares the same shape check.
std::optional<std::string> constructorPrefixClassLiteral(const Method &method,
                                                         const Instruction &op);
// A prefix read must retain runtime field access and class initialization;
// compile-time constants need a separate source folding/initialization proof.
std::optional<FieldRef> constructorPrefixStaticRead(const Method &method,
                                                    const Instruction &op,
                                                    const ClassMap &classes,
                                                    Budget &budget);
// Exact instance reads use the same typed field/frame receipt. Consumers
// separately prove an initialized receiver and retain the read's null failure.
std::optional<FieldRef> constructorPrefixInstanceRead(const Method &method,
                                                      const Instruction &op,
                                                      const ClassMap &classes,
                                                      Budget &budget);
ConstructorCaptures
planConstructorCaptures(const ClassMap &classes, Budget &budget,
                        ConstructorDelegations *delegations = nullptr);
// Bounded this-prefixes may compute arguments in loops and copy uninitialized
// this, but cannot observe or overwrite its aliases. A leading this-call may
// instead have a bounded postlude whose edges stay after initialization.
// Every chain reaches a known primary;
// final-field validation rejects rewrites, and Body validates all operations.
// A primary may retain exact own-field stores from the shared capture proof.
ConstructorDelegations
planConstructorDelegations(const ClassMap &classes, Budget &budget,
                           const ConstructorDelegations &captured,
                           ConstructorSuperPrologues *super_prologues = nullptr,
                           const ConstructorCaptures *captures = nullptr);
CapturedInnerClasses
planCapturedInnerClasses(const ClassMap &classes, Budget &budget,
                         const ConstructorCaptures *captures = nullptr);
const Class *localSourceOwner(const std::string &type, const ClassMap &classes,
                              Budget &budget);
bool localClassVisible(const Class &local, const Class &context,
                       const MethodRef *method, const ClassMap &classes,
                       Budget &budget);
// Anonymous bodies reused across methods or same-package declarations need one
// shared source declaration rather than distinct local class copies.
struct SharedAnonymousClass {
  std::string source_owner;
  // A public subclass can qualify an inherited public member of a package
  // host without changing that member's actual source declaration owner.
  std::string reference_owner;
  std::set<MethodRef> methods;
  std::set<std::string> declarations;
};
using SharedAnonymousClasses = std::map<std::string, SharedAnonymousClass>;
SharedAnonymousClasses planSharedAnonymousClasses(const ClassMap &classes,
                                                  Budget &budget);
// Source aliases never replace original descriptors or InnerClass metadata.
std::map<std::string, std::string> planLocalClassNames(const ClassMap &classes,
                                                       Budget &budget);
struct AnonymousClassSite {
  MethodRef method;
  uint32_t allocation_pc = 0;
};
using AnonymousClassPlan = std::map<std::string, AnonymousClassSite>;
AnonymousClassPlan planAnonymousClasses(const ClassMap &classes,
                                        Budget &budget);
// Enum constants are Java enum declarations, not ordinary anonymous classes.
// Only verified initialization and forwarding may be supplied by javac.
bool isEnumConstantClass(const Class &cls, const ClassMap &classes);
struct EnumConstant {
  FieldRef field;
  MethodRef constructor;
  std::string implementation;
  std::vector<Literal> arguments;
  std::vector<std::optional<Method>> argument_helpers;
};
struct EnumClassPlan {
  FieldRef values_field;
  std::vector<EnumConstant> constants;
  std::optional<MethodRef> array_factory;
  // Original code after constant/array publication, with proven register
  // values materialized at entry. A backing alias needs a read-only proof.
  std::optional<Method> initializer_tail;
  uint32_t array_publication_pc = 0;
};
struct EnumMethodProjection {
  std::string kind;
  bool compiler_managed = false;
  std::optional<MethodRef> target;
  std::optional<uint32_t> delegation_pc;
  std::optional<uint32_t> argument_call_pc;
  std::optional<std::vector<std::string>> forwarding_arguments;
};
std::string javaEnumLiteral(const Literal &value, const std::string &type);
struct EnumPlans {
  std::map<std::string, EnumClassPlan> classes;
  std::map<std::string, std::string> constant_classes;
  std::map<MethodRef, EnumMethodProjection> methods;
  std::set<FieldRef> managed_fields;
  // Java ConstantValue supplies these before enum allocation. Original field
  // reads call a generated static accessor to preserve active initialization.
  std::map<FieldRef, std::string> encoded_constants;
};
EnumPlans planEnumClasses(const ClassMap &classes, Budget &budget);
void verifyEnumArrayUses(const ClassMap &classes, const EnumPlans &enums,
                         Budget &budget);
using AnnotationProjections = std::map<FieldRef, std::set<std::string>>;
AnnotationProjections validateAnnotations(const ClassMap &classes,
                                          Budget &budget,
                                          const EnumPlans *enums = nullptr);
struct AnnotationFieldInitializer {
  FieldRef source;
  FieldRef target;
};
using AnnotationFieldInitializers =
    std::map<MethodRef, AnnotationFieldInitializer>;
AnnotationFieldInitializers
planAnnotationFieldInitializers(const ClassMap &classes, Budget &budget);
struct SourceScopePlans {
  SharedAnonymousClasses shared;
  EnumPlans enums;
};
// Return plans from this validated, immutable class graph so later stages in
// this operation reuse the same scope and enum decisions.
SourceScopePlans validateSourceScopes(const ClassMap &classes, Budget &budget);
ClassMap linkClasses(std::vector<Class> classes, Budget &budget);
std::vector<Class> parseDex(std::string_view bytes, std::string_view input_id,
                            Budget &budget);
Class parseSmali(std::string_view text, std::string_view input_id,
                 Budget &budget);
Class parseClassDeclaration(std::string_view bytes, std::string_view input_id,
                            Budget &budget);
llvm::json::Object recoverJava(const ClassMap &classes, Budget &budget);
} // namespace neverd::mobile::dalvik
