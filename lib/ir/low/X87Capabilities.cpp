//===- X87Capabilities.cpp - Scoped x87 capability inventory -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One semantic inventory for queries and generated documentation. Structural
/// encoding admission remains in X87Context.h; a report never executes code.
//===----------------------------------------------------------------------===//

#include "neverd/ir/X87Capabilities.h"

#include "neverd/ir/X87Context.h"
#include "neverd/ir/low/X87PortableContext.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <set>

namespace neverd {
namespace {
constexpr X87InstructionForm Forms[] = {
#define NEVERD_X87_FORM(ID, Escape, First, Last, Memory, Name, Operands,       \
                        Typed, Class, Portable)                                \
  {#ID,                                                                        \
   Escape,                                                                     \
   First,                                                                      \
   Last,                                                                       \
   Memory,                                                                     \
   Name,                                                                       \
   Operands,                                                                   \
   Typed,                                                                      \
   X87ValueClass::Class,                                                       \
   X87PortableOperation::Portable},
#include "neverd/ir/X87Instructions.def"
#undef NEVERD_X87_FORM
};

const char *supportName(X87Support Support) {
  switch (Support) {
  case X87Support::Exact:
    return "supported-exact";
  case X87Support::Approximate:
    return "explicitly-approximate";
  case X87Support::Unknown:
    return "conservative-unknown";
  case X87Support::Unsupported:
    return "unsupported";
  }
  return "unsupported";
}

const char *coverageClass(const X87InstructionForm &Form) {
  if (Form.PortableOperation != X87PortableOperation::None) {
    if (Form.ValueClass == X87ValueClass::StateOnly)
      return "portable-state-only";
    return Form.ValueClass == X87ValueClass::ExactValue
               ? "portable-exact-value-projection"
               : "portable-value-projection";
  }
  if (!Form.TypedLift)
    return Form.ValueClass == X87ValueClass::StateOnly ? "owned-only-state"
                                                       : "owned-only";
  switch (Form.ValueClass) {
  case X87ValueClass::ExactValue:
    return "exact-value-projection";
  case X87ValueClass::ProcessorDependent:
    return "processor-dependent";
  case X87ValueClass::ValueProjection:
    return "value-projection";
  case X87ValueClass::StateOnly:
    return "state-only";
  }
  return "unqualified";
}

bool validQuery(const X87CapabilityQuery &Q) {
  return (Q.SourceMode == X87SourceMode::I386 ||
          Q.SourceMode == X87SourceMode::X64) &&
         (Q.ABI == X87ABI::SystemV || Q.ABI == X87ABI::Microsoft ||
          Q.ABI == X87ABI::MinGW) &&
         (Q.StatePath == X87StatePath::TypedStack ||
          Q.StatePath == X87StatePath::OwnedContext) &&
         (Q.Consumer >= X87Consumer::Lift &&
          Q.Consumer <= X87Consumer::Translation) &&
         (Q.Backend == X87Backend::None || Q.Backend == X87Backend::NativeX86 ||
          Q.Backend == X87Backend::Portable ||
          Q.Backend == X87Backend::NativeReference) &&
         (Q.Destination == X87Destination::I386 ||
          Q.Destination == X87Destination::X64 ||
          Q.Destination == X87Destination::ARM64);
}

X87Capability unavailable(const char *Reason) {
  X87Capability Result;
  Result.Reason = Reason;
  return Result;
}

llvm::json::Object capabilityJSON(const X87Capability &C) {
  return llvm::json::Object{
      {"implemented", C.Implemented},
      {"numerical", supportName(C.Numerical)},
      {"architectural_state", supportName(C.ArchitecturalState)},
      {"profile", C.Profile},
      {"reason", C.Reason},
      {"requirements", C.Requirements},
      {"test_sources", C.Evidence}};
}

template <typename T> struct NamedValue {
  T Value;
  const char *Name;
};
constexpr NamedValue<X87SourceMode> Modes[] = {{X87SourceMode::I386, "i386"},
                                               {X87SourceMode::X64, "x86-64"}};
constexpr NamedValue<X87ABI> ABIs[] = {{X87ABI::SystemV, "system-v"},
                                       {X87ABI::Microsoft, "microsoft"},
                                       {X87ABI::MinGW, "mingw"}};
constexpr NamedValue<X87StatePath> Paths[] = {
    {X87StatePath::TypedStack, "typed-stack"},
    {X87StatePath::OwnedContext, "owned-context"}};
constexpr NamedValue<X87Consumer> Consumers[] = {
    {X87Consumer::Lift, "lift"},
    {X87Consumer::IRAnalysis, "ir-analysis"},
    {X87Consumer::LLVM, "llvm"},
    {X87Consumer::HighC, "high-c"},
    {X87Consumer::LLVMC, "llvm-c"},
    {X87Consumer::Concrete, "concrete"},
    {X87Consumer::Symbolic, "symbolic"},
    {X87Consumer::CheckedRefinement, "checked-refinement"},
    {X87Consumer::Translation, "translation"}};
constexpr NamedValue<X87Backend> Backends[] = {
    {X87Backend::None, "none"},
    {X87Backend::NativeX86, "native-x86"},
    {X87Backend::Portable, "portable"},
    {X87Backend::NativeReference, "native-reference"}};
constexpr NamedValue<X87Destination> Destinations[] = {
    {X87Destination::I386, "i386"},
    {X87Destination::X64, "x86-64"},
    {X87Destination::ARM64, "arm64"}};

} // namespace

llvm::ArrayRef<X87InstructionForm> x87InstructionForms() { return Forms; }

const X87InstructionForm *x87InstructionForm(uint32_t Selector) {
  if (!x87ContextInstruction(Selector))
    return nullptr;
  const unsigned Escape = Selector & 0xff;
  const unsigned ModRM = (Selector >> 8) & 0xff;
  const bool Memory = (Selector & X87ContextMemory) != 0;
  for (const auto &Form : Forms)
    if (Form.Escape == Escape && Form.Memory == Memory &&
        ModRM >= Form.FirstModRM && ModRM <= Form.LastModRM)
      return &Form;
  return nullptr;
}

bool x87InstructionIsProcessorDependent(uint32_t Selector) {
  const auto *Form = x87InstructionForm(Selector);
  return Form && Form->ValueClass == X87ValueClass::ProcessorDependent;
}

X87Capability queryX87Capability(const X87CapabilityQuery &Q) {
  if (!validQuery(Q))
    return unavailable("invalid-query-dimension");
  const auto *Form = x87InstructionForm(Q.Selector);
  if (!Form)
    return unavailable("unsupported-or-malformed-encoding");
  const bool Owned = Q.StatePath == X87StatePath::OwnedContext;
  if (Owned && Q.SourceMode != X87SourceMode::X64)
    return unavailable("owned-context-requires-x86-64-source");
  if (!Owned && !Form->TypedLift)
    return unavailable("instruction-refused-by-typed-stack");
  if (Q.Consumer == X87Consumer::Translation)
    return unavailable("x87-cross-architecture-translation-unqualified");

  X87Capability Result;
  Result.Numerical = X87Support::Unknown;
  Result.ArchitecturalState = X87Support::Unknown;
  Result.Profile = Owned ? "owned-x64-context-v1" : "typed-x87-projection-v1";
  Result.Requirements = "explicit incoming state; validated encoding, memory "
                        "and CFG/call contract";
  Result.Evidence =
      Owned ? "unittests/lift/x86_64/X86_64_X87ConvertStateTests.cpp"
            : "unittests/lift/x86_32/X86_32_X87FPUTests.cpp";
  if (Q.Consumer == X87Consumer::Symbolic ||
      Q.Consumer == X87Consumer::CheckedRefinement) {
    if (Q.Backend != X87Backend::None)
      return unavailable("analysis-requires-backend-none");
    Result.Reason = "complete-x87-formal-model-unqualified";
    Result.Evidence = "unittests/symbolic/SymX87Tests.cpp";
    return Result;
  }
  if (Q.Consumer == X87Consumer::Lift ||
      Q.Consumer == X87Consumer::IRAnalysis) {
    if (Q.Backend != X87Backend::None)
      return unavailable("analysis-requires-backend-none");
    Result.Implemented = true;
    Result.Reason = "representation-does-not-qualify-execution";
    return Result;
  }
  if (Q.Consumer == X87Consumer::Concrete) {
    if (!Owned) {
      if (Q.Backend != X87Backend::Portable)
        return unavailable("typed-concrete-uses-value-projection");
      Result.Reason = "typed-concrete-complete-instruction-unqualified";
      return Result;
    }
    if (Q.Backend == X87Backend::Portable) {
      if (Form->PortableOperation == X87PortableOperation::None)
        return unavailable("instruction-outside-portable-profile");
      if (Q.Destination == X87Destination::I386)
        return unavailable("portable-32-bit-host-unqualified");
      Result.Implemented = true;
      Result.Numerical = Form->ValueClass == X87ValueClass::StateOnly
                             ? X87Support::Unknown
                             : X87Support::Exact;
      Result.Profile = kX87PortableProfile;
      Result.Reason =
          "portable-masked-slice; complete-state-qualification-pending";
      Result.Requirements =
          "explicit setX87PortableMode(true); x64 owned state; known CW/SW "
          "and read payloads; authenticated ordinary memory; no pending "
          "waiting exception or stack fault; arithmetic/comparisons/integer "
          "stores require "
          "all masks and canonical finite operands; raw m80/register moves "
          "retain "
          "encodings; undefined C0/C2/C3/FOP/register FDP are not CPU identity";
      Result.Evidence = "unittests/lift/core/X87PortableContextTests.cpp; "
                        "unittests/lift/core/X87PortableEmulatorTests.cpp; "
                        "scripts/check_x87_arithmetic_replay.py";
      return Result;
    }
    if (Q.Backend != X87Backend::NativeX86 &&
        Q.Backend != X87Backend::NativeReference)
      return unavailable("concrete-backend-required");
    if (Q.Destination != X87Destination::X64)
      return unavailable("native-context-requires-x86-64-host");
    if (Form->ValueClass == X87ValueClass::ProcessorDependent &&
        Q.Backend != X87Backend::NativeReference)
      return unavailable(
          "processor-dependent-operation-requires-native-reference");
    Result.Implemented = true;
    Result.Numerical = Form->ValueClass == X87ValueClass::ProcessorDependent
                           ? X87Support::Approximate
                           : X87Support::Exact;
    if (Form->ValueClass == X87ValueClass::StateOnly)
      Result.Numerical = X87Support::Unknown;
    Result.Profile = "native-x87-host-reference-v1";
    Result.Reason = "native-result-is-not-portable-or-formal-evidence";
    Result.Requirements =
        "x86-64 GCC/Clang; FXSAVE/FXRSTOR; SSE3 for FISTTP; known CW/SW, tags "
        "and demanded payloads; valid PC/RC; no pending unmasked exception at "
        "waiting boundaries; authenticated ordinary memory; "
        "processor-dependent "
        "operations require explicit non-strict native-reference mode";
    return Result;
  }
  // These are generated-code capabilities, not the architecture of the host
  // doing analysis. All current full-context routes use x86-64 inline asm.
  if (Q.Backend != X87Backend::NativeX86)
    return unavailable("generated-x87-runtime-unqualified");
  if (Q.Destination == X87Destination::ARM64 ||
      (Owned && Q.Destination != X87Destination::X64))
    return unavailable("destination-lacks-qualified-x87-runtime");
  Result.Implemented = true;
  Result.Reason = "emission-does-not-qualify-complete-state-or-exceptions";
  Result.Requirements =
      "x86 GCC/Clang GNU assembly and target x87; actual source/target ABI and "
      "long-double layout; typed C value operations require 64-bit significand "
      "long double; admitted CFG/tag/control and call preconditions";
  if ((Owned && Form->ValueClass != X87ValueClass::StateOnly) ||
      Form->ValueClass == X87ValueClass::ExactValue)
    Result.Numerical = X87Support::Exact;
  if (Form->ValueClass == X87ValueClass::ProcessorDependent)
    Result.Numerical = X87Support::Unknown;
  return Result;
}

std::string x87CapabilitiesJSON() {
  llvm::json::Array Inventory;
  for (const auto &Form : Forms) {
    llvm::json::Array Variants;
    for (uint32_t Flags : {0U, X87ContextOperand16, X87ContextWaitPrefix,
                           X87ContextOperand16 | X87ContextWaitPrefix}) {
      uint32_t Selector =
          Form.Escape | (unsigned(Form.FirstModRM) << 8) | Flags;
      if (Form.Memory)
        Selector |= X87ContextMemory |
                    (unsigned(Form.FirstModRM) << X87ContextOriginalModRMShift);
      const auto Spec = x87ContextInstruction(Selector);
      if (!Spec)
        continue;
      Variants.push_back(llvm::json::Object{
          {"selector", Selector},
          {"operand16", bool(Flags & X87ContextOperand16)},
          {"wait_prefix", bool(Flags & X87ContextWaitPrefix)},
          {"memory_bytes", Spec->MemoryBytes},
          {"reads_memory", Spec->ReadsMemory},
          {"writes_memory", Spec->WritesMemory},
          {"reads_flags", Spec->ReadsFlags},
          {"writes_flags", Spec->WritesFlags},
          {"writes_ax", Spec->WritesAX},
          {"waiting", Spec->Waiting}});
    }
    Inventory.push_back(
        llvm::json::Object{{"id", Form.ID},
                           {"escape", Form.Escape},
                           {"first_modrm", Form.FirstModRM},
                           {"last_modrm", Form.LastModRM},
                           {"memory", Form.Memory},
                           {"mnemonic", Form.Mnemonic},
                           {"operands", Form.Operands},
                           {"typed_lift", Form.TypedLift},
                           {"portable_profile",
                            Form.PortableOperation == X87PortableOperation::None
                                ? "unqualified"
                                : kX87PortableProfile},
                           {"coverage_class", coverageClass(Form)},
                           {"variants", std::move(Variants)}});
  }

  llvm::json::Array SourcePaths;
  for (auto Mode : Modes)
    for (auto ABI : ABIs)
      for (auto Path : Paths) {
        const bool Owned = Path.Value == X87StatePath::OwnedContext;
        SourcePaths.push_back(llvm::json::Object{
            {"source_mode", Mode.Name},
            {"abi", ABI.Name},
            {"state_path", Path.Name},
            {"available", !Owned || Mode.Value == X87SourceMode::X64},
            {"default_path", (Mode.Value == X87SourceMode::X64 &&
                              ABI.Value != X87ABI::SystemV) == Owned},
            {"long_double", ABI.Value == X87ABI::Microsoft
                                ? "binary64"
                                : "binary80; ABI-specific padding"},
            {"call_contract",
             Owned ? "private stack; external state export/refresh required; "
                     "override cannot qualify an x87 return ABI"
                   : "typed stack; independently prove parameters, ST0 results "
                     "and stack effects; ABI does not authenticate payloads"}});
      }

  llvm::json::Array Coverage;
  std::set<std::string> EmittedClasses;
  for (const auto &Form : Forms) {
    if (!EmittedClasses.insert(coverageClass(Form)).second)
      continue;
    uint32_t Selector = Form.Escape | (unsigned(Form.FirstModRM) << 8);
    if (Form.Memory)
      Selector |= X87ContextMemory |
                  (unsigned(Form.FirstModRM) << X87ContextOriginalModRMShift);
    for (auto Mode : Modes)
      for (auto Path : Paths)
        for (auto Consumer : Consumers)
          for (auto Backend : Backends)
            for (auto Destination : Destinations) {
              auto Cell = capabilityJSON(queryX87Capability(
                  {Selector, Mode.Value, X87ABI::SystemV, Path.Value,
                   Consumer.Value, Backend.Value, Destination.Value}));
              Cell["coverage_class"] = coverageClass(Form);
              Cell["source_mode"] = Mode.Name;
              Cell["state_path"] = Path.Name;
              Cell["consumer"] = Consumer.Name;
              Cell["backend"] = Backend.Name;
              Cell["destination"] = Destination.Name;
              Coverage.push_back(std::move(Cell));
            }
  }

  llvm::json::Array Boundaries;
#define NEVERD_X87_EXTERNAL_STATE(ID)                                          \
  Boundaries.push_back(llvm::json::Object{                                     \
      {"intrinsic", #ID},                                                      \
      {"context_action", "export-before; refresh-after"},                      \
      {"architectural_state", "conservative-unknown"},                         \
      {"reason",                                                               \
       "synchronization-is-not-complete-instruction-qualification"}});
#include "neverd/ir/X87StateBoundaries.def"
#undef NEVERD_X87_EXTERNAL_STATE
  for (const char *Name :
       {"X87ContextBegin", "X87ContextRefresh", "X87ContextExport",
        "X87ContextMMXRead", "X87ContextMMXWrite", "X87ContextMMXEnter", "call",
        "return", "independent-cfg-root"})
    Boundaries.push_back(llvm::json::Object{
        {"intrinsic", Name},
        {"context_action", "preserve physical aliases and authenticated state"},
        {"architectural_state", "conservative-unknown"},
        {"reason",
         "transport-contract-does-not-qualify-an-arbitrary-call-ABI"}});

  llvm::json::Object Report{
      {"schema_version", kX87CapabilitiesSchemaVersion},
      {"scope", "static-inventory; no host probe, execution or proof"},
      {"matrix_contract",
       "join forms.coverage_class with coverage; source_paths records actual "
       "ABI contracts; explicit state-path coverage is ABI-independent; all "
       "unlisted dimensions/encodings are unsupported"},
      {"precision", "exact-only by default; native-reference approximation "
                    "never supplies exact or checked-equivalence evidence"},
      {"evidence", "test_sources are regression source references, not passing "
                   "results, CPU qualifications or reusable proof receipts"},
      {"unqualified_interfaces",
       llvm::json::Array{
           "OS exception delivery", "memory-fault partial completion",
           "FXSAVE/FXRSTOR and XSAVE/XRSTOR instruction completion",
           "unknown incoming state", "termination proof"}},
      {"source_paths", std::move(SourcePaths)},
      {"state_boundaries", std::move(Boundaries)},
      {"forms", std::move(Inventory)},
      {"coverage", std::move(Coverage)}};
  std::string Text;
  llvm::raw_string_ostream(Text) << llvm::json::Value(std::move(Report));
  return Text;
}
} // namespace neverd
