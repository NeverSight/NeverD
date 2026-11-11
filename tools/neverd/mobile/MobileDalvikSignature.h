//===- MobileDalvikSignature.h - Parsed JVM generic source declarations
// -----===//
#pragma once

#include "MobileDalvik.h"

namespace neverd::mobile::dalvik {
enum class GenericSignatureKind { Class, Field, Method };
using GenericTypeId = uint32_t;

struct GenericTypeArgument {
  enum class Variance { Invariant, Extends, Super, Any };
  Variance variance = Variance::Invariant;
  std::optional<GenericTypeId> type;
};
struct GenericClassSegment {
  std::string name;
  std::vector<GenericTypeArgument> arguments;
};
struct GenericType {
  enum class Kind { Primitive, Void, Class, TypeVariable, Array };
  Kind kind = Kind::Void;
  char primitive = 0;
  // Slash-separated package, without a trailing slash. Segments preserve
  // explicitly encoded inner suffixes; a literal '$' is not split.
  std::string package;
  std::vector<GenericClassSegment> segments;
  std::string variable_name;
  std::optional<GenericTypeId> element;
  // Filled by validation, not by the syntax-only parser. A variable owner is
  // its class descriptor or exact MethodRef::identity(), never a guessed scope.
  std::string erasure;
  std::optional<std::string> variable_owner;
  size_t offset = 0;
};
struct GenericTypeParameter {
  std::string name;
  std::optional<GenericTypeId> class_bound;
  std::vector<GenericTypeId> interface_bounds;
  std::string erasure;
};
struct GenericSourceProjection {
  size_t offset = 0;
  std::string kind;
  std::string original_type;
  std::string source_type;
  // Exact declaration witnessing an otherwise unavailable synthetic scope.
  std::optional<std::string> scope_origin;
};
struct GenericSignature {
  GenericSignatureKind kind = GenericSignatureKind::Field;
  std::vector<GenericType> types;
  std::vector<GenericTypeParameter> type_parameters;
  std::optional<GenericTypeId> superclass;
  std::vector<GenericTypeId> interfaces;
  std::optional<GenericTypeId> field_type;
  std::vector<GenericTypeId> parameters;
  std::optional<GenericTypeId> result;
  std::vector<GenericTypeId> throws_types;
  std::set<std::string> visible_type_variables;
  // Original Signature strings stay on the input declarations. These bounded
  // source projections retain runtime descriptors and report metadata changes.
  std::vector<GenericSourceProjection> source_projections;
};
// Moving a sole variable bound to its Java class-bound slot preserves both
// its scoped constraint and its original erasure.
bool onlyBoundPositionProjections(const GenericSignature &signature);
// Whether an explicit source witness may be considered for this static lookup.
bool staticWitnessCandidate(const MethodRef &ref, const Method &declaration,
                            const Class &owner, const GenericSignature &signature,
                            const ClassMap &classes, Budget &budget);
// Abstract variant parameters use the same witness and overload proof as
// static members, while retaining their original virtual/interface receiver.
bool virtualWitnessCandidate(const MethodRef &ref, const Method &declaration,
                              const Class &owner, const GenericSignature &signature,
                              Budget &budget);
bool enumWitnessCandidate(const MethodRef &ref, const Method &declaration,
                          const Class &owner);
// An entry with the original constructor parameters, one int mask per 32
// parameters, and the trailing Kotlin runtime marker.
bool hasDefaultConstructorMask(const MethodRef &entry, const MethodRef &target);
// Return the index of an Object-bounded method variable at the runtime
// component of this type. Nominal type arguments never change that component.
std::optional<size_t> objectParameterIndex(const GenericSignature &signature,
                                           GenericTypeId type, Budget &budget);
// Substituting Object for every method variable yields reifiable parameter
// types at the original descriptors, including arrays and unbounded wildcards.
bool reifiableObjectParameters(const MethodRef &ref,
                               const GenericSignature &signature,
                               Budget &budget);
// ? and ? extends the exact plain Object type have identical bounds. Keep
// original syntax/metadata and use this common spelling only for comparison.
GenericTypeArgument::Variance
canonicalGenericArgumentVariance(GenericTypeArgument::Variance variance,
                                 bool plain_object, Budget &budget);
GenericTypeArgument::Variance
canonicalGenericArgumentVariance(const GenericSignature &signature,
                                 const GenericTypeArgument &argument,
                                 Budget &budget);
// A known plain final class closes an invariant / extends parameter bound.
// These comparisons are proof witnesses, never input metadata rewrites.
bool equivalentClosedFinalParameterBound(const GenericSignature &actual,
                                         const GenericTypeArgument &provided,
                                         const GenericSignature &expected,
                                         const GenericTypeArgument &required,
                                         const ClassMap &classes,
                                         Budget &budget);
// Invariant Object and an Object lower wildcard have the same sole known
// type argument. This permits an explicit source projection, not Java type
// equality or an inferred conversion.
bool equivalentClosedObjectLowerParameterBound(
    const GenericSignature &actual, const GenericTypeArgument &provided,
    const GenericSignature &expected, const GenericTypeArgument &required,
    const ClassMap &classes, Budget &budget);
bool equivalentClosedParameterType(const GenericSignature &actual,
                                   GenericTypeId actual_id,
                                   const GenericSignature &expected,
                                   GenericTypeId expected_id,
                                   const ClassMap &classes, Budget &budget);
struct IntegerUnitBridge {
  MethodRef target, dispatch;
  FieldRef result_field;
  GenericTypeId interface_type;
  std::string original_interface;
  bool boolean_parameters = false;
};
struct InheritedCarrierContract {
  std::vector<std::string> parameters;
  std::string result;
};
struct ValueCarrierBridge {
  MethodRef target, dispatch;
  std::optional<MethodRef> box;
  std::vector<MethodRef> unbox;
  GenericTypeId interface_type;
  std::string original_interface;
  // An original void target can be followed by its exact Unit field read.
  std::optional<FieldRef> result_field;
  // A pure reference box stores this complete fixed backing representation.
  std::optional<FieldRef> representation_field;
  std::string representation_signature;
  // A Signature-free implementation may consume a fixed source interface's
  // original literal contract. Only that source interface edge is projected.
  std::optional<std::string> interface_owner;
  // Full original member substitutions across a known interface graph.
  // Keeping these receipts also keeps every sibling on the original graph.
  std::map<MethodRef, InheritedCarrierContract> inherited_contracts;
};
struct PrimitiveVoidBridge {
  MethodRef target;
  // Complete original class-formal substitutions, before source erasure.
  std::set<MethodRef> contracts;
};
struct ClosedReferenceGetterBridge {
  MethodRef target;
  GenericSignature signature;
};
struct NonReturningVoidBridge {
  MethodRef target;
  // A source-only void result on the target's original validated parameters.
  std::optional<GenericSignature> source_signature;
  // Own Object formal on a complete invariant carrier/seed pair. Preserve the
  // bridge's original raw Signature and invoke the target through its exact
  // ABI.
  bool erased_method_formals = false;
  // Exact original ancestor members whose complete own-formal contract admits
  // this unchanged raw bridge. Consumers must not infer a second erasure rule.
  std::set<MethodRef> erased_contracts;
};
struct CovariantBridgeContract {
  MethodRef target;
  // Operation-local ancestor contract in the original bridge owner's scope.
  // This is a proof witness, never a Signature added to the input method.
  GenericSignature contract;
  // Source default parameter receipts may certify a complete raw subsignature
  // through the exact original checked bridge while retaining scoped edges.
  bool raw_parameter_subsignature = false;
  // A closed typed visibility contract proves javac's original erased
  // public access bridge, independently of explicit source forwarding.
  bool regenerate_public_access = false;
};
struct BoxedInterfaceResultBridge {
  MethodRef target, box;
  // Complete original zero-argument interface result, before source widening.
  GenericSignature contract;
  // An original final typed entry has the same complete five-op boxing proof.
  // Its separate runtime return slot needs a disclosed Java source name.
  std::optional<MethodRef> typed_entry;
  bool preserve_typed_interface = false;
};
struct NominalBoxedResultBridge {
  MethodRef target, box;
  // Exact original plain interface declarations with the same runtime ABI.
  std::set<MethodRef> contracts;
  // A broader return entry has the same original scalar call and factory.
  std::optional<MethodRef> forward_to;
};
struct PrimitiveBridgeConversion {
  std::string boxed, checked;
  MethodRef unbox;
};
struct UnboxedPredicateBridge {
  MethodRef target, unbox;
  GenericSignature contract;
};
struct GuardedDefaultResultBridge {
  MethodRef typed, wrapper, super_call, dispatch;
  // The complete inherited contract in the original consumer scope.
  GenericSignature contract;
};
struct SourceErasedInterfaceMember {
  MethodRef contract;
  GenericSignature signature;
};
struct ConstructorExceptionContract {
  MethodRef target;
  uint32_t call_pc;
  // Original ordered exceptions, plus only proven checked additions required
  // by an anonymous-to-named constructor projection. Input metadata is intact.
  std::vector<std::string> source_declared_throws;
};
struct GenericSignaturePlans {
  std::map<std::string, GenericSignature> classes;
  std::map<FieldRef, GenericSignature> fields;
  std::map<MethodRef, GenericSignature> methods;
  // Complete original class-scope contracts for referenced unary reference
  // constructors. Method formals and source projections grant no raw binding.
  std::set<MethodRef> reference_constructor_inputs;
  // Complete original class ancestry of fixed constructor argument views.
  // A missing declaration or open source scope grants no negative conversion.
  std::map<std::string, std::set<std::string>> constructor_argument_ancestors;
  // A captured sibling superclass uses the exact same outer-owned formals.
  // Recorded before source owner arguments leave scope; both constructor
  // capture receipts and complete original owner arguments are required.
  std::set<std::pair<std::string, std::string>> captured_sibling_superclasses;
  // Source-only raw inheritance projections retain their original complete
  // inheritance contracts for the shared member/bridge proof.
  std::map<std::string, GenericSignature> inheritance_contracts;
  // A raw unary superclass formal used only by marker ancestry may receive
  // the unique complete argument already declared by the consumer's graph.
  // Only source metadata changes; original runtime declarations stay intact.
  std::set<std::string> raw_marker_superclasses;
  // Erasing a callback edge over an inherited raw marker requires an explicit
  // source entry for its original parameter-adapting bridge.
  std::set<MethodRef> raw_marker_callbacks;
  // An original erased interface bridge may contain its entire implementation.
  // The exact incoming cast and complete unary contract certify an Object
  // source interface argument; the ordinary recovery pipeline still validates
  // every body op.
  std::map<std::pair<MethodRef, MethodRef>, CovariantBridgeContract>
      checked_inlined_interface_bridges;
  // Original raw interface edges cannot regenerate a checked parameter
  // bridge. Retain the original body after proving its typed target's complete
  // closed contract; neither the owner edge nor target Signature is projected.
  std::map<std::pair<MethodRef, MethodRef>, CovariantBridgeContract>
      raw_interface_parameter_bridges;
  std::map<std::pair<MethodRef, MethodRef>, BoxedInterfaceResultBridge>
      boxed_interface_results;
  // Complete original class/interface contracts for a scalar or array factory
  // bridge, retained independently of its reported source representation.
  std::map<std::pair<MethodRef, MethodRef>, BoxedInterfaceResultBridge>
      boxed_superclass_results;
  std::map<MethodRef, NominalBoxedResultBridge> nominal_boxed_results;
  std::set<std::string> boxed_superclass_owners;
  // Bound uses, multiple formals, protected contracts and nonmarker ancestry
  // retain the full original superclass arguments. The same complete factory
  // proof supplies typed source entries and javac regenerates erased slots.
  std::map<MethodRef, MethodRef> boxed_superclass_source_results;
  // Original bounded reference predicates retain their exact checked scalar
  // dispatch; source-only raw edges keep complete original member contracts.
  std::map<std::pair<MethodRef, MethodRef>, UnboxedPredicateBridge>
      unboxed_predicate_bridges;
  std::map<MethodRef, MethodRef> terminal_throw_bridges;
  std::map<std::pair<MethodRef, MethodRef>, CovariantBridgeContract>
      erased_terminal_consumers;
  // Emission-only method shapes for the corresponding raw source interface.
  // Original full method contracts remain authoritative in methods.
  std::map<MethodRef, SourceErasedInterfaceMember>
      erased_terminal_member_sources;
  // Original raw interface members retain complete method contracts. Source
  // subsignatures erase alpha-equivalent method formals or closed parameters
  // of a proven constant field-read body that never accesses its inputs.
  std::map<MethodRef, SourceErasedInterfaceMember> raw_interface_method_sources;
  // Exact local raw-interface covariance: Object-bounded enclosing parameters
  // leave the source declaration, while javac regenerates the original bridge.
  // A synthetic default's already-resolved method formal keeps its original
  // shared scope receipt rather than inventing a second parameter projection.
  std::map<MethodRef, std::pair<MethodRef, MethodRef>>
      raw_local_covariant_bridges;
  // Source-only raw parameters after complete wildcard ancestor and bounded
  // superclass-argument agreement; original method contracts stay intact.
  std::map<MethodRef, SourceErasedInterfaceMember>
      inherited_wildcard_parameter_sources;

  // A plain Object-parameter interface default may inherit an unchanged
  // Object-bounded class-formal slot. The common validator witnesses both
  // source and runtime contracts without projecting metadata.
  std::set<std::pair<MethodRef, MethodRef>> inherited_erased_contracts;
  // Exact target/ancestor pairs whose Object parameters erase Object-bounded
  // class formals. Results retain full scoped substitution.
  std::set<std::pair<MethodRef, MethodRef>> erased_parameter_overrides;
  // Complete dependency self-return substitution witnesses, including the
  // unchanged covariant runtime bridge declared by the same interface.
  std::set<std::pair<MethodRef, MethodRef>> external_default_return_overrides;
  // Exact method-owned unary result substitutions. Parameter and bound
  // equality and the original bridge body remain the consumer's checks.
  std::set<std::pair<MethodRef, MethodRef>> bounded_covariant_returns;
  // Source result covariance compares complete parameter substitutions,
  // superclass class-formal bindings and nominal result ancestry.
  std::set<std::pair<MethodRef, MethodRef>> covariant_reference_returns;
  // A plain abstract source class can narrow an abstract ancestor's result
  // without an original broad slot. Preserve its full contract and disclose
  // the bridge added by javac separately from original runtime recovery.
  std::map<std::pair<MethodRef, MethodRef>, GenericSignature>
      abstract_class_covariant_contracts;
  // Interface and concrete class returns use full parameter equality,
  // interface DAG bindings and nominal result ancestry. A source default needs
  // its exact original erased bridge body; no runtime entry is invented.
  std::set<std::pair<MethodRef, MethodRef>> covariant_interface_returns;
  // An unchanged SDK class supplies a concrete covariant interface result.
  // The complete required contract is scoped to that provider; consumers
  // compose their own class edge without emitting or guessing an SDK body.
  std::map<std::pair<MethodRef, MethodRef>, GenericSignature>
      external_class_covariant_contracts;
  // A class-owned formal with one known plain class/interface bound can retain
  // its runtime return while implementing that exact plain interface result.
  // This witnesses only the result; consumers still compare all parameters.
  std::set<std::pair<MethodRef, MethodRef>> class_formal_bound_returns;
  // Original erased bridges retain the full proven ancestor substitution.
  // Inheriting consumers must compose and compare this complete contract.
  std::map<std::pair<MethodRef, MethodRef>, CovariantBridgeContract>
      covariant_bridge_contracts;
  // An original child bridge may call a direct parent's final covariant
  // getter. Keep its declaring target and complete child-scoped contract;
  // never copy the inherited implementation into the child.
  std::map<std::pair<MethodRef, MethodRef>, CovariantBridgeContract>
      inherited_final_covariant_bridges;
  // An original narrow result bridge checks a direct generic superclass call.
  // Omitting only this verified compiler entry preserves the inherited broad
  // runtime slot; emitting a source override would invent another bridge.
  std::map<std::pair<MethodRef, MethodRef>, CovariantBridgeContract>
      inherited_checked_result_bridges;
  // A source interface may implement an ancestor's erased parameter
  // slot through its original bridge and concrete typed default. The receipt
  // retains the complete specialized contract for default-provider selection.
  std::map<std::pair<MethodRef, MethodRef>, CovariantBridgeContract>
      source_default_parameter_bridges;
  // A Signature-free class override may erase its own plain class formal
  // only through its exact original checked bridge and complete superclass or
  // interface-path substitution. This witness adds no input method metadata.
  std::map<std::pair<MethodRef, MethodRef>, CovariantBridgeContract>
      scoped_raw_parameter_bridges;
  // Public children of nonpublic classes retain original pure access bridges.
  // The target is the nearest concrete superclass declaration; its complete
  // contract must equal the bridge's unchanged original runtime declaration.
  std::map<MethodRef, CovariantBridgeContract> public_superclass_access_bridges;
  // Super calls compose the original superclass arguments into the caller's
  // scope. Parameters keep their runtime erasure unless an original checked
  // adapter or Body's converged word proof permits a closed nominal view.
  std::map<std::pair<MethodRef, MethodRef>, GenericSignature> superclass_calls;
  // Closed constructor contracts accept a proven null or exact original
  // erased access delegation without inventing a generic binding. Both this
  // and super calls consume these full parameter contracts.
  std::map<std::pair<MethodRef, MethodRef>, GenericSignature> constructor_calls;
  std::map<std::pair<MethodRef, MethodRef>, MethodRef>
      superclass_call_declarations;
  // Original scalar bridge adapters retain an exact widened super call and
  // result. Their complete body has no cast, guard, extra effect or new slot.
  std::map<MethodRef, MethodRef> superclass_scalar_adapters;
  // Exact original widened calls with an original checked reference result.
  std::map<MethodRef, MethodRef> superclass_reference_adapters;
  // Original two-argument guards return the incoming default before checking
  // its type. Their erased body must remain an explicit source declaration.
  std::map<MethodRef, GuardedDefaultResultBridge> guarded_default_results;
  std::set<std::string> guarded_default_raw_owners;
  // Same-owner distinct return slots call the same original private producer;
  // the final broad entry never acquires virtual dispatch through the narrow
  // one.
  std::map<std::pair<MethodRef, MethodRef>, MethodRef>
      private_producer_return_slots;
  // Original super-result bridges whose broad metadata is projected only
  // after an unchanged final getter and complete parent result agree.
  std::map<MethodRef, MethodRef> superclass_getter_results;
  // Plain scalar getter -> original super call and final virtual wrapper.
  std::map<MethodRef, std::pair<MethodRef, MethodRef>>
      superclass_scalar_getters;
  // A covariant target may itself be an original final generic getter bridge.
  // Its exact same-owner forwarding body and complete result Signature agree.
  std::map<MethodRef, MethodRef> reference_getter_bridges;
  // Original plain static receiver implementations can be covariant bridge
  // targets after the common validator proves the complete inherited result.
  std::set<MethodRef> covariant_static_receiver_implementations;
  // Exact final field getters with a proven closed nominal element subtype
  // can use a disclosed raw source result for an invariant inherited result.
  std::map<MethodRef, std::set<MethodRef>> nominal_getter_return_erasures;
  // Original parameter bridges certified before nested result variance is
  // projected. Access admission may consume only this exact bridge/target.
  std::map<MethodRef, MethodRef> nested_return_bridges;
  // A two-formal carrier with no own member/field or ancestor formal use may
  // hold the same input formal and its original provider self type. Preserve
  // each complete interface contract separately from the raw source result.
  std::map<std::pair<MethodRef, MethodRef>, GenericSignature>
      self_carrier_return_contracts;
  std::map<MethodRef, GenericSignature> self_carrier_return_sources;
  // A closed nested Object argument and its Object lower wildcard have the
  // same full return type. Retain original metadata and the inherited witness.
  std::map<std::pair<MethodRef, MethodRef>, GenericSignature>
      closed_object_lower_returns;
  // Source-interface default results satisfy complete ancestor
  // substitutions; their original erased bridges require the body proof.
  std::set<std::pair<MethodRef, MethodRef>> covariant_interface_default_returns;
  // Original same-slot source defaults retain their full scoped parameter
  // and result contract; consumers compose the original parent bindings.
  std::map<std::pair<MethodRef, MethodRef>, GenericSignature>
      source_default_contracts;
  // Proven recursive Comparable static comparisons and boolean receiver
  // predicates, plus one closed nominal method bound with plain inputs or
  // exact uses of its own formal (including one invariant callback carrier).
  // Same-owner private static calls and void/boolean results retain their
  // original access and descriptors, without new checks or result boxing.
  // Same-owner private instance calls also retain one result-only Object
  // formal with reifiable inputs, including proven captured-member scopes.
  // Public static/virtual/interface calls may share one or two Object-bounded
  // formals between invariant input carriers and an invariant result carrier;
  // the single-formal profile also accepts covariant input carriers.
  // A static factory can share one nominal bound between invariant/covariant
  // unary inputs, its invariant result and optional closed-input producers,
  // alongside plain auxiliary slots.
  // Owned private calls may also bind one scoped Object formal through two
  // invariant/covariant inputs and the same complete invariant unary result
  // carrier. Each call still requires its exact private declaring scope.
  // A public static carrier transfer may also take a binary callback consuming
  // the same sole formal twice and producing a complete fixed nominal type.
  // Its unary input and invariant result retain the same known carrier.
  // A unary key selector may instead produce a distinct own Object-bounded
  // formal while the original input formal remains in that result carrier.
  // A static producer of one nominally bounded method formal may return a
  // separate fixed invariant unary carrier beside closed context/scalar slots.
  // Its complete producer input/result and both declaration bounds are checked.
  // Receipts name the exact declaring member, including inherited calls.
  std::set<MethodRef> erased_generic_invocations;
  // One nominally bounded method formal shared by an invariant binary
  // converter input and unary result. The converter's other slot is a closed
  // nominal type; all declaration bounds and descriptor erasures are proven.
  std::set<MethodRef> erased_bounded_converter_invocations;
  // A direct instance member of a raw generic owner has its entire method
  // type erased, including method formals (JLS 4.8). Consumers must still use
  // that exact raw declaring receiver and virtual/interface dispatch; this
  // receipt does not authorize a typed inherited, static or super call.
  // A proven single type-variable bound position change retains the same
  // constraint and erasure; other source projections remain excluded.
  std::set<MethodRef> raw_receiver_invocations;
  // Static closed runtime slots with one or two unused method formals under
  // Object or known simple nominal bounds, including owned private entries.
  // Complete parameterized trees
  // share the nominal-bound proof; static scope excludes owner formals.
  // Calls, Signatures, original private access and dispatch stay unchanged.
  std::set<MethodRef> erased_unused_formal_invocations;
  // Ordered nominal bounds and their one direct formal's parameter position
  // form one contract. Scalar context contributes no type binding. Body still
  // proves the original nominal argument and unchanged source overloads;
  // this receipt alone grants neither an erased value nor direct/super calls.
  struct IntersectionInvocation {
    size_t parameter_index = 0;
    std::vector<std::string> bounds;
    bool operator==(const IntersectionInvocation &) const = default;
  };
  std::map<MethodRef, IntersectionInvocation> intersection_invocations;
  // One closed callback tail accompanies a class/interface first-parameter
  // intersection. Original erasures are shared with source overload checks.
  std::map<MethodRef, std::pair<std::string, std::string>>
      intersection_callback_tails;
  // Static boolean helpers with one recursive method formal shared by an
  // invariant interface receiver and zero to two unchanged erased arguments.
  std::set<MethodRef> erased_predicate_receiver_invocations;
  // Original generic bounds and checked Number.intValue forwarding must be
  // proven before a source interface is erased to keep both original bodies.
  std::map<MethodRef, IntegerUnitBridge> integer_unit_bridges;
  std::map<MethodRef, ValueCarrierBridge> value_carrier_bridges;
  // Source-owned default setters keep their original broad checked unboxing
  // bodies. Only certified Object-bounded ancestor parameters change spelling.
  std::map<MethodRef, PrimitiveVoidBridge> primitive_void_bridges;
  // Original final-owner Void/void siblings, certified before class ancestry
  // and consumed unchanged by source naming, contract checks and emission.
  std::map<MethodRef, NonReturningVoidBridge> nonreturning_void_bridges;
  // Original raw return forwarders to a differently named closed generic
  // getter. Full inherited results consume this exact source-only contract.
  std::map<MethodRef, ClosedReferenceGetterBridge>
      closed_reference_getter_bridges;
  std::map<MethodRef, MethodRef> erased_void_parameters;
  // Locals inside a known static input method whose complete class/field/
  // member signatures reference no enclosing formal and require no projection.
  std::set<std::string> closed_static_local_classes;
  // Static anonymous locals whose enclosing nominal method formal occurs only
  // in original constructor parameters. Inherited member contracts remain
  // closed; constructor scope and any source projection stay independently
  // validated by the ordinary signature and source ownership planners.
  std::set<std::string> constructor_scoped_static_locals;
  // Anonymous classes whose complete original class/field/member types do
  // not consume an enclosing formal. Capture validation remains authoritative.
  std::set<std::string> closed_anonymous_classes;
  // Named class-initializer anonymous members with no state or enclosing
  // formal and one original empty Object constructor.
  std::set<std::string> stateless_anonymous_members;
  CapturedInnerClasses captured_inner_classes;
  // Complete owned synthetic member declarations with no outer capture or
  // enclosing formal and independently initialized original constructors.
  std::set<std::string> independent_inner_classes;
  // Original parameter-to-field capture receipts shared with source naming.
  ConstructorCaptures constructor_captures;
  ConstructorDelegations constructor_delegations;
  ConstructorSuperPrologues constructor_super_prologues;
  // Exact selected this/super edges whose closed checked exceptions fit the
  // original declaration or the narrow captured anonymous source projection.
  // Body still owns invocation, prefix effects and initialization validation.
  std::map<MethodRef, ConstructorExceptionContract>
      constructor_exception_contracts;
  // Generic parameter index -> descriptor parameter index. Only omitted
  // parameters with a shared capture/enum proof may make this non-identity.
  std::map<MethodRef, std::vector<size_t>> parameter_indices;
  // Includes present-empty Throws annotations. A method signature may legally
  // omit concrete throws; this map preserves that independent declaration.
  std::map<MethodRef, std::vector<std::string>> declared_throws;
  // Sorted compatibility keys after the common hierarchy/duplicate proof.
  // Generic throws stay excluded; original declaration order remains above.
  std::map<MethodRef, std::vector<std::string>> closed_declared_throws;
  // Complete bounded nominal ancestry from the common Throwable validator.
  // Closed override checks consume these receipts without walking loaders or
  // guessing subtype relations from exception names.
  std::map<std::string, std::set<std::string>> throwable_ancestors;
  // At least one original declared exception is checked. Mixed declarations
  // consume nominal ancestry; all-unchecked declarations retain their separate
  // method receipt and must not be reclassified by a consumer.
  std::set<MethodRef> checked_exception_declarations;
  // Every declared exception has a proven RuntimeException or Error ancestry.
  // Only closed original Exceptions declarations enter this receipt.
  std::set<MethodRef> unchecked_declared_throws;
};
// Resolve the original JVM descriptor before consuming its generic declaration
// or Java source name. The symbolic owner remains the invocation qualifier.
const Method *lookupMethod(const MethodRef &reference, const ClassMap &classes,
                           Budget &budget,
                           const GenericSignaturePlans *signatures);
// Rendering uses the checked source view; semantic comparisons keep methods.
const GenericSignature *
sourceMethodSignature(const MethodRef &method,
                      const GenericSignaturePlans &signatures);

// Shared original Exceptions receipt readers. Complete member scopes and bridge
// bodies are compared separately; generic throws and source projections fail.
bool validatedClosedDeclaredThrows(const Method &method,
                                   const GenericSignaturePlans &signatures);
bool validatedUncheckedDeclaredThrows(const Method &method,
                                      const GenericSignaturePlans &signatures);

bool sameClosedInheritedThrows(const Method &actual, const Method &required,
                               const GenericSignaturePlans &signatures);
bool emptyClosedInheritedThrows(const Method &actual, const Method &required,
                                const GenericSignaturePlans &signatures);
bool emptyClosedInheritedBridgeThrows(const Method &bridge,
                                      const Method &target,
                                      const Method &required,
                                      const GenericSignaturePlans &signatures);
bool compatibleClosedInheritedThrows(const Method &actual,
                                     const Method &required,
                                     const GenericSignaturePlans &signatures);

struct GenericBridge {
  MethodRef target;
  // The erased inherited declaration gives Java callers a source spelling
  // that retains virtual bridge dispatch, including subclass overrides.
  MethodRef dispatch;
  // Kotlin's void implementation returns this exact field through its erased
  // bridge. A source Unit wrapper retains the original call/read order.
  std::optional<FieldRef> result_field;
  bool preserve_body = false;
  // A public implementation can promote a protected superclass contract.
  // Erased source calls still need their separate receiver access proof.
  bool protected_contract = false;
  bool boolean_parameters = false;
  // An identical terminal throw with unchanged parameters needs javac to
  // regenerate its result-only bridge. Report the additional stack frame.
  bool terminal_throw_result = false;
  // The erased return entry uses an original wrapper with an independently
  // proven identical guard, typed virtual target and nullable result.
  bool guarded_reference_result = false;
  bool nonreturning_boolean_result = false;
  // Regenerating an originally exception-free bridge copies these validated
  // inherited exceptions. Keep the metadata difference explicit.
  std::optional<std::vector<std::string>> source_declared_throws;
  // A missing interface slot added by javac casts one closed Object argument.
  bool generated_parameter_adaptation = false;
  bool boxed_reference_result = false;
  // The public child keeps the accessible source spelling while javac
  // supplies its original erased invokespecial access entry.
  bool public_superclass_access = false;
};
struct GenericInheritancePlan {
  // Method -> original primitive bridge witnessing source parameter erasure.
  std::map<MethodRef, MethodRef> erased_void_parameters;
  struct SourceMethodName {
    std::string name;
    MethodRef declaration;
    std::set<MethodRef> conflicting_contracts;
    std::optional<MethodRef> private_producer;
    std::optional<MethodRef> nonreturning_void_bridge;
    // A final owner keeps an original checked reference-return bridge.
    std::optional<MethodRef> checked_reference_bridge;
    // Exact six-operation message throw and unchanged integer conversion.
    // Body emission consumes this common proof to preserve allocation order.
    std::optional<MethodRef> nonreturning_message_integer_bridge;
    // An independent body or closed final adapter needs a legal Java name.
    // Its descriptor, access contract and body are unchanged; flag differences
    // remain explicit source metadata projections.
    bool identifier_only = false;
    // A closed public source family, rooted in interfaces or final methods
    // of final classes, shares one name. No method flags are changed.
    std::set<MethodRef> identifier_family;
    // A pure property writer conflicts with an unchanged same-class updater,
    // rather than an inherited Java method contract.
    bool local_return_slot = false;
    // A closed final synthetic void adapter retains its exact same-class
    // reference call, discarding its result under a disclosed source name.
    bool discarded_reference_result = false;
    // A final owner keeps distinct generic and checked-super getter bodies.
    bool local_reference_return_slot = false;
    // Distinct static descriptors in one closed owner retain separate bodies.
    bool static_return_slot = false;
    // The complete owned virtual-name proof selected an interface declaration.
    // Calls may retain a resolved inheriting spelling; independent closed and
    // package-synthetic entries keep their exact declaring-reference rule.
    bool interface_identifier_family = false;
    // Complete protected class family, including anchored concrete overloads.
    bool protected_identifier_family = false;
  };
  // JVM return slots that conflict with an inherited Java contract, and
  // source-only identifier projections, share disclosed declaration names.
  std::map<MethodRef, SourceMethodName> source_method_names;
  // Every synthetic entry of an admitted local static return-only group,
  // including the descriptor retaining its original source name.
  std::set<MethodRef> static_return_synthetic_methods;
  // Only original synthetic final captures with an illegal Java identifier.
  // All declarations and field references consume this exact identity map.
  std::map<FieldRef, std::string> source_field_names;
  // Exact surviving getfield instructions whose original private visibility
  // must be restored after compiling their public source declaration.
  std::set<FieldRef> cross_package_private_reads;
  std::map<MethodRef, GenericBridge> bridges;
  // A final owner's getter or primitive-array target, or an independently
  // scoped getter, int-to-reference-array result or void consumer without
  // source descendant overrides, becomes public in source to preserve its
  // original public erased entry.
  std::map<MethodRef, MethodRef> public_protected_bridge_targets;
  struct NonReturningResult {
    MethodRef bridge;
    std::string source_type;
    std::string target_source_name;
  };
  // Final-owner methods with a proven unconditional original throw can adopt
  // the inherited primitive source result. Input bodies/metadata stay intact.
  std::map<MethodRef, NonReturningResult> nonreturning_results;
  // Javac adds these inherited runtime slots when an original source
  // interface redeclares a proven narrower abstract return or fixed parameter.
  std::map<MethodRef, GenericBridge> generated_interface_bridges;
  // Original abstract class declarations retain their owners and metadata;
  // these absent broader slots are compiler projections, never input bodies.
  std::map<MethodRef, GenericBridge> generated_class_bridges;
  // Original Signature-free raw results implementing closed parameterized
  // results through Java's unchecked return conversion. No metadata is added.
  std::map<MethodRef, std::set<MethodRef>> raw_result_contracts;
  struct RetainedBridge {
    MethodRef target;
    bool guarded_reference = false;
    bool terminal_throw = false;
    bool reference_getter = false;
    bool value_carrier = false;
    // An original void bridge may implement a generic superclass setter by
    // forwarding to a differently named method with the complete same input.
    std::optional<MethodRef> generic_dispatch;
    bool primitive_void = false;
    bool superclass_getter = false;
    std::optional<MethodRef> inherited_class_contract;
    bool superclass_scalar_adapter = false;
    bool superclass_reference_adapter = false;
    bool guarded_default_result = false;
    bool raw_parameter_forwarding = false;
    // A raw callback retains both original overloads and the checked
    // post-delegation singleton read; no interface arguments are invented.
    std::optional<FieldRef> raw_unit_result;
    // Explicit raw indexed callbacks keep Number.intValue and either plain
    // Object slots or resolved default-method parameters without narrowing.
    std::set<size_t> raw_indexed_parameters;
    bool raw_integer_result = false;
    bool aliased_return_forwarding = false;
    bool checked_reference_return = false;
    bool contract_implementation = false;
    bool nonreturning_void = false;
    // Emission-only signature copied from the fully checked typed target.
    std::optional<GenericSignature> source_signature;
    bool boxed_primitive_getter = false;
    std::optional<MethodRef> box_method;
    bool public_superclass_access = false;
    bool checked_inlined_interface = false;
    bool boxed_interface_result = false;
    bool nominal_boxed_result = false;
    bool boxed_superclass_result = false;
    bool named_scalar_getter = false;
    bool guarded_scalar_parameter = false;
    bool unboxed_predicate = false;
    std::optional<MethodRef> unbox_method;
    bool erased_terminal_consumer = false;
    bool erased_terminal_member = false;
    bool raw_interface_method_formals = false;
    bool inherited_wildcard_parameter = false;
    bool closed_reference_getter = false;
    // The complete closed ancestor slot supplies source parameter/result
    // types for an original array unbox/delegate body. Its erased descriptor
    // is regenerated by javac; the new typed entry is separately reported.
    std::optional<MethodRef> source_reference;
    std::set<MethodRef> carrier_contracts;
    // Exact original direct-interface super forwarding, shared with Body
    // dispatch.
    bool interface_super_forwarder = false;
    // The common primitive forwarding verifier proved the original wrapper
    // body and inherited slot; expression callers may consume this receipt.
    bool primitive_forwarding = false;
    // Scoped raw callbacks may use different unboxing methods per argument.
    std::map<size_t, MethodRef> raw_parameter_conversions;
  };
  // Original final scalar/getter and guarded-reference forwarding bodies.
  // Java retains the declaration/body but cannot spell ACC_BRIDGE.
  std::map<MethodRef, RetainedBridge> retained_plain_bridges;
  std::set<MethodRef> required_parameter_bridges;
  // Final, non-overriding void implementations need distinct Java names;
  // original typed calls use this shared mapping without reading Unit.
  std::map<MethodRef, std::string> void_unit_targets;
  // Exact owned static interface accessors dispatch directly to this owner's
  // default implementation. Source keeps one original body in a private
  // instance helper, shared by the original default and static entries.
  std::map<MethodRef, MethodRef> interface_default_accessors;
  std::map<MethodRef, std::string> interface_default_helpers;
  // Inherited defaults use an already proved original ancestor static
  // accessor. Keep the original invoke-super reference separately from that
  // source forwarding entry; no private body helper crosses interface owners.
  std::map<MethodRef, std::pair<MethodRef, MethodRef>>
      inherited_interface_default_accessors;
  // Original instance calls qualify a direct interface's inherited default;
  // exact static descriptor accessors use the same declaration receipt before
  // deriving an instance helper and its direct-parent lexical qualifier.
  // The shared selector and full consumer-scoped contract own this exact
  // declaration; emission retains lexical interface.super dispatch on this.
  std::map<std::pair<MethodRef, MethodRef>, MethodRef>
      scoped_interface_super_calls;
  // Exact static accessors also need one legal, nonredundant direct qualifier.
  // Keep this graph decision with the selected declaration for both consumers
  // that derive the private instance helper.
  std::map<MethodRef, std::string> static_interface_super_qualifiers;
};
// Shared witness for source metadata projection and inherited bridge emission.
// Throws unless the complete body only forwards checked original arguments.
void verifyGenericBridge(
    const Method &bridge, const Method &target, Budget &budget,
    const FieldRef *result_field = nullptr,
    const std::set<size_t> *integer_parameters = nullptr,
    const ClassMap *classes = nullptr,
    const GenericSignaturePlans *signatures = nullptr,
    const Method *inherited_contract = nullptr,
    const PrimitiveBridgeConversion *carrier_conversion = nullptr,
    const MethodRef *result_factory = nullptr);
bool provenStaticReceiverImplementation(const Class &cls, const Method &method,
                                        const ClassMap &classes,
                                        const GenericSignaturePlans &signatures,
                                        Budget &budget);
std::optional<MethodRef>
provenGenericReferenceGetterBridge(const Class &cls, const Method &bridge,
                                   const GenericSignaturePlans &signatures,
                                   Budget &budget);
// The same declaration-checked conversion table serves signature and body
// proof. Unknown boxed/checked/unbox declarations do not establish a contract.
std::optional<PrimitiveBridgeConversion>
primitiveBridgeConversion(const std::string &primitive,
                          const ClassMap &classes);
// Exact original scalar carrier call; no conversion behavior is synthesized.
std::optional<PrimitiveBridgeConversion>
scalarCarrierBridgeConversion(const MethodRef &unbox, const ClassMap &classes,
                              const GenericSignaturePlans &signatures);
std::optional<PrimitiveBridgeConversion>
arrayCarrierBridgeConversion(const MethodRef &unbox, const ClassMap &classes,
                             const GenericSignaturePlans &signatures);
std::optional<std::string>
primitiveBridgeBoxedType(const std::string &primitive, const ClassMap &classes);
bool parameterIndependentTerminalThrow(const Method &method, Budget &budget);
bool identicalTerminalThrowBridge(const Method &bridge, const Method &target,
                                  Budget &budget);
std::map<MethodRef, IntegerUnitBridge>
planIntegerUnitBridges(const ClassMap &classes,
                       const GenericSignaturePlans &signatures, Budget &budget);
std::map<MethodRef, ValueCarrierBridge>
planValueCarrierBridges(const ClassMap &classes,
                        const GenericSignaturePlans &signatures,
                        Budget &budget);
std::map<MethodRef, ClosedReferenceGetterBridge>
planClosedReferenceGetterBridges(const ClassMap &classes,
                                 const GenericSignaturePlans &signatures,
                                 Budget &budget);
std::map<MethodRef, NonReturningVoidBridge>
planNonReturningVoidBridges(const ClassMap &classes,
                            const GenericSignaturePlans &signatures,
                            Budget &budget);
GenericInheritancePlan
planGenericInheritance(const ClassMap &classes,
                       const GenericSignaturePlans &signatures, Budget &budget,
                       const EnumPlans *enums = nullptr);
// Prove original default-result guards against the complete class/interface
// ancestry before making the disclosed raw source edges they require.
bool projectGuardedDefaultResults(const ClassMap &classes,
                                  GenericSignaturePlans &signatures,
                                  Budget &budget);

// A named static member has independent source scope; local and anonymous
// owners do not. Shared by signature projections and inheritance proofs.
bool independentSourceScope(const Class &cls);
// Binary packages decide inherited package visibility in one shared layer.
std::string_view packageOf(const std::string &owner);
bool inaccessiblePackageMethod(const Class &owner, const Method &member,
                               const Class &child);

// Syntax-only parsing is bounded and does not establish source validity.
GenericSignature parseGenericSignature(GenericSignatureKind kind,
                                       std::string_view signature,
                                       Budget &budget);
// An exact Kotlin metadata declaration may witness Object-only formals on a
// synthetic static method whose JVM Signature is absent. No body is inferred.
std::optional<std::vector<std::string>>
kotlinUnconstrainedMethodFormals(const Class &owner, const Method &method,
                                 Budget &budget);
// Shared by linkClasses and direct recoverJava callers. It validates lexical
// scopes and descriptor erasures; it does not prove arbitrary external generic
// assignability or body substitutions. Those remain source-emitter obligations.
// Parameterized external declarations require a known contract: currently only
// Java SE 8 List<E> and Comparable<T>, each an Object-bounded interface. Input
// declarations override these contracts; unknown instantiations are rejected.
GenericSignaturePlans
validateGenericSignatures(const ClassMap &classes, Budget &budget,
                          const EnumPlans *enums = nullptr,
                          const SharedAnonymousClasses *shared = nullptr);
} // namespace neverd::mobile::dalvik
