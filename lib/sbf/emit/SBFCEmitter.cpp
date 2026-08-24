//===- SBFCEmitter.cpp - Solana SBF to portable C backend -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Drives C emission for a recovered SBF program: decides which arithmetic
/// helpers the program needs, writes the runtime interface and the recovered
/// constants, then renders the body either as a structured function or as a
/// program-counter dispatch switch.
///
//===----------------------------------------------------------------------===//

#include "neverd/sbf/emit/SBFCEmitter.h"

#include "SBFCEmitterDetail.h"

#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/analysis/SBFStructuredCFG.h"
#include "neverd/sbf/emit/SBFSourceLimits.h"
#include "neverd/sbf/emit/SBFSourceStatus.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace neverd::sbf {

using namespace c_emitter_detail;

namespace {

static_assert(std::is_unsigned_v<RuntimeFeatureMask>,
              "runtime feature masks must use an unsigned source ABI");

std::string cRuntimeFeatureName(llvm::StringRef Name) {
  std::string Result = "NEVERD_SBF_RUNTIME_FEATURE_";
  for (char Character : Name)
    Result.push_back(Character == '-' ? '_' : llvm::toUpper(Character));
  return Result;
}

template <typename Mask> llvm::StringRef cRuntimeFeatureMaskTypeImpl() {
  constexpr unsigned Bits = std::numeric_limits<Mask>::digits;
  static_assert(Bits == std::numeric_limits<uint32_t>::digits ||
                    Bits == std::numeric_limits<uint64_t>::digits,
                "generated C supports 32-bit and 64-bit feature masks");
  if constexpr (Bits == std::numeric_limits<uint64_t>::digits)
    return "uint64_t";
  return "uint32_t";
}

llvm::StringRef cRuntimeFeatureMaskType() {
  return cRuntimeFeatureMaskTypeImpl<RuntimeFeatureMask>();
}

template <typename Mask>
std::string cRuntimeFeatureMaskLiteralImpl(Mask Value) {
  constexpr unsigned Bits = std::numeric_limits<Mask>::digits;
  const llvm::StringRef Macro =
      Bits == std::numeric_limits<uint64_t>::digits ? "UINT64_C" : "UINT32_C";
  std::string Result = Macro.str();
  Result += "(0x";
  Result += llvm::utohexstr(Value);
  Result += ')';
  return Result;
}

std::string cRuntimeFeatureMaskLiteral(RuntimeFeatureMask Value) {
  return cRuntimeFeatureMaskLiteralImpl(Value);
}

} // namespace

llvm::Expected<std::string> emitC(const SBFProgram &Program,
                                  const CEmitterOptions &Options) {
  if (llvm::Error Error = validateVMConfig(Program.Config))
    return std::move(Error);
  if (Program.Med.Instructions.empty())
    return llvm::make_error<llvm::StringError>(
        "sbf: cannot emit C for an empty MedIR",
        llvm::inconvertibleErrorCode());

  std::map<size_t, const MedInstruction *> BySlot;
  for (const MedInstruction &Instruction : Program.Med.Instructions)
    BySlot[Instruction.Slot] = &Instruction;
  std::optional<StructuredControlFlow> Structured =
      Options.PreferStructuredControlFlow
          ? buildStructuredControlFlow(Program)
          : std::optional<StructuredControlFlow>{};
  if (Structured && Structured->MaximumDepth > kMaximumCStructuredNestingDepth)
    Structured.reset();

  bool NeedsSignExtension = false;
  bool NeedsSignedCompare32 = false;
  bool NeedsSignedCompare64 = false;
  bool NeedsArithmeticShift32 = false;
  bool NeedsArithmeticShift64 = false;
  bool NeedsUnsignedHighMultiply = false;
  bool NeedsSignedHighMultiply = false;
  bool NeedsByteSwap = false;
  bool NeedsSignedDivision = false;
  bool NeedsRuntimeStatus = false;
  bool NeedsSyscall = false;
  for (const MedInstruction &Instruction : Program.Med.Instructions) {
    NeedsSignExtension |=
        Instruction.Semantics.Result == ResultExtension::Sign32;
    const bool SignedCompare =
        Instruction.Op == Operation::SGt || Instruction.Op == Operation::SGe ||
        Instruction.Op == Operation::SLt || Instruction.Op == Operation::SLe;
    NeedsSignedCompare32 |= SignedCompare && Instruction.Width == kWordBitWidth;
    NeedsSignedCompare64 |=
        SignedCompare && Instruction.Width == kDoubleWordBitWidth;
    NeedsArithmeticShift32 |=
        Instruction.Op == Operation::ARSh && Instruction.Width == kWordBitWidth;
    NeedsArithmeticShift64 |= Instruction.Op == Operation::ARSh &&
                              Instruction.Width == kDoubleWordBitWidth;
    NeedsUnsignedHighMultiply |= Instruction.Op == Operation::UHighMul ||
                                 Instruction.Op == Operation::SHighMul;
    NeedsSignedHighMultiply |= Instruction.Op == Operation::SHighMul;
    NeedsByteSwap |= Instruction.Op == Operation::EndianBE;
    NeedsSignedDivision |=
        Instruction.Op == Operation::SDiv || Instruction.Op == Operation::SRem;
    NeedsRuntimeStatus |=
        Instruction.Op == Operation::Load ||
        Instruction.Op == Operation::Store ||
        (Instruction.Op == Operation::Call &&
         (Instruction.Call == CallKind::Syscall ||
          Instruction.Call == CallKind::Unresolved ||
          (Instruction.Call == CallKind::Internal &&
           Instruction.Dispatch ==
               CallDispatchPolicy::LegacyRuntimeThenFunction)));
    NeedsSyscall |= Instruction.Op == Operation::Call &&
                    (Instruction.Call == CallKind::Syscall ||
                     Instruction.Call == CallKind::Unresolved ||
                     (Instruction.Call == CallKind::Internal &&
                      Instruction.Dispatch ==
                          CallDispatchPolicy::LegacyRuntimeThenFunction));
  }

  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  OS << "/* Generated by NeverD from "
     << versionDisplayName(Program.Low.TheVersion)
     << ". */\n"
        "#include <stddef.h>\n#include <stdint.h>\n#include <limits.h>\n\n"
        "typedef enum neverd_sbf_status {\n";
#define SBF_SOURCE_SUCCESS(NAME, FAULT_CODE, C_NAME, VALUE)                    \
  OS << "  " #C_NAME " = " << sourceStatusCode(SourceStatus::NAME) << ",\n";
#define SBF_SOURCE_C_V1_ERROR(NAME)                                            \
  OS << "  " << cSourceStatusName(SourceStatus::NAME) << " = "                 \
     << sourceStatusCode(SourceStatus::NAME) << ",\n";
#include "neverd/sbf/emit/SBFSourceStatuses.def"
  OS << "} neverd_sbf_status;\n\n"
        "/* Version 2 has a fixed-width, append-only status domain. Values "
        "zero through eight are the legacy enum values above. */\n"
        "typedef uint32_t neverd_sbf_status_v2;\n"
        "enum {\n";
#define SBF_SOURCE_C_V1_FALLBACK(NAME, LEGACY_NAME)                            \
  OS << "  " << cSourceStatusName(SourceStatus::NAME) << " = "                 \
     << sourceStatusCode(SourceStatus::NAME) << ",\n";
#include "neverd/sbf/emit/SBFSourceStatuses.def"
  OS << "};\n\n"
        "typedef "
     << cRuntimeFeatureMaskType()
     << " neverd_sbf_runtime_feature_mask;\n"
        "typedef struct neverd_sbf_runtime_features {\n"
        "  neverd_sbf_runtime_feature_mask bits;\n"
        "} neverd_sbf_runtime_features;\n";
  for (const RuntimeFeatureInfo &Info : runtimeFeatureInfos())
    OS << "#define " << cRuntimeFeatureName(Info.Name) << " "
       << cRuntimeFeatureMaskLiteral(runtimeFeatureMask(Info.ID)) << "\n";
  OS << "\n"
        "typedef struct neverd_sbf_syscall_invocation {\n"
        "  uint32_t hash;\n"
        "  uint64_t arguments["
     << kArgumentRegisterCount
     << "];\n"
        "  neverd_sbf_runtime_features runtime_features;\n"
        "} neverd_sbf_syscall_invocation;\n\n"
        "typedef struct neverd_sbf_environment {\n"
        "  void *context;\n"
        "  int (*load)(void *, uint64_t, uint32_t, uint64_t *);\n"
        "  int (*store)(void *, uint64_t, uint32_t, uint64_t);\n"
        "  /* Version 1 callbacks return zero on success and nonzero for the "
        "operation's legacy generic fault. */\n"
        "  int (*syscall)(void *, uint32_t";
  for (unsigned Index = 0; Index < kArgumentRegisterCount; ++Index)
    OS << ", uint64_t";
  OS << ", uint64_t *);\n"
        "} neverd_sbf_environment;\n\n"
        "/* Version 2 is a distinct ABI. The original environment layout and "
        "entrypoint never read this extension. Through the v2 entrypoint, "
        "all callbacks may return any declared status for an exact handled "
        "fault. */\n"
        "typedef struct neverd_sbf_environment_v2 {\n"
        "  neverd_sbf_environment base;\n"
        "  int (*syscall_with_features)(\n"
        "      void *, const neverd_sbf_syscall_invocation *, uint64_t *);\n"
        "  /* Null selects the program's resolved snapshot; a pointer to zero "
        "is an explicit empty snapshot. */\n"
        "  const neverd_sbf_runtime_features *runtime_features;\n"
        "} neverd_sbf_environment_v2;\n\n"
        "typedef enum nd_status_abi {\n"
        "  ND_STATUS_ABI_V1, ND_STATUS_ABI_V2\n"
        "} nd_status_abi;\n\n"
        "typedef struct nd_environment_view {\n"
        "  void *context;\n"
        "  int (*load)(void *, uint64_t, uint32_t, uint64_t *);\n"
        "  int (*store)(void *, uint64_t, uint32_t, uint64_t);\n"
        "  int (*syscall)(void *, uint32_t";
  for (unsigned Index = 0; Index < kArgumentRegisterCount; ++Index)
    OS << ", uint64_t";
  OS << ", uint64_t *);\n"
        "  int (*syscall_with_features)(\n"
        "      void *, const neverd_sbf_syscall_invocation *, uint64_t *);\n"
        "  const neverd_sbf_runtime_features *runtime_features;\n"
        "  nd_status_abi status_abi;\n"
        "} nd_environment_view;\n\n";
  OS << "static neverd_sbf_status nd_legacy_status(\n"
        "    neverd_sbf_status_v2 status) {\n"
        "  switch (status) {\n";
#define SBF_SOURCE_SUCCESS(NAME, FAULT_CODE, C_NAME, VALUE)                    \
  OS << "  case " #C_NAME ": return " #C_NAME ";\n";
#define SBF_SOURCE_C_V1_ERROR(NAME)                                            \
  OS << "  case " << cSourceStatusName(SourceStatus::NAME)                     \
     << ": return (neverd_sbf_status)status;\n";
#define SBF_SOURCE_C_V1_FALLBACK(NAME, LEGACY_NAME)                            \
  OS << "  case " << cSourceStatusName(SourceStatus::NAME) << ": return "      \
     << cSourceStatusName(SourceStatus::LEGACY_NAME) << ";\n";
#include "neverd/sbf/emit/SBFSourceStatuses.def"
  OS << "  default: return "
     << cSourceStatusName(SourceStatus::InvalidInstruction)
     << ";\n"
        "  }\n"
        "}\n\n";
  if (NeedsRuntimeStatus) {
    OS << "static neverd_sbf_status_v2 nd_runtime_status(\n"
          "    const nd_environment_view *env, int status,\n"
          "    neverd_sbf_status_v2 legacy_status,\n"
          "    neverd_sbf_status_v2 invalid_status) {\n"
          "  if (status == "
       << cSourceStatusName(SourceStatus::Ok) << ") return "
       << cSourceStatusName(SourceStatus::Ok)
       << ";\n"
          "  if (env && env->status_abi == ND_STATUS_ABI_V1)\n"
          "    return legacy_status;\n"
          "  switch ((neverd_sbf_status_v2)status) {\n";
#define SBF_SOURCE_SUCCESS(NAME, FAULT_CODE, C_NAME, VALUE)
#define SBF_SOURCE_ERROR(NAME, FAULT_CODE, C_NAME, C_VALUE, RUST_VALUE)        \
  OS << "  case " #C_NAME ": return " #C_NAME ";\n";
#include "neverd/sbf/emit/SBFSourceStatuses.def"
    OS << "  default: return invalid_status;\n"
          "  }\n"
          "}\n\n";
  }
  if (NeedsSyscall) {
    OS << "static const neverd_sbf_runtime_features "
          "NEVERD_SBF_PROGRAM_RUNTIME_FEATURES = { "
       << cRuntimeFeatureMaskLiteral(
              runtimeFeatureMask(Program.ActiveRuntimeFeatures))
       << " };\n\n"
          "static int nd_syscall(nd_environment_view *env, uint32_t hash";
    for (unsigned Index = 0; Index < kArgumentRegisterCount; ++Index)
      OS << ", uint64_t a" << Index;
    OS << ", uint64_t *value) {\n"
          "  neverd_sbf_syscall_invocation invocation = { hash, { ";
    for (unsigned Index = 0; Index < kArgumentRegisterCount; ++Index) {
      if (Index)
        OS << ", ";
      OS << "a" << Index;
    }
    OS << " }, NEVERD_SBF_PROGRAM_RUNTIME_FEATURES };\n"
          "  if (!env) return "
       << cSourceStatusName(SourceStatus::UnknownSyscall)
       << ";\n"
          "  if (env->runtime_features)\n"
          "    invocation.runtime_features = *env->runtime_features;\n"
          "  if (env->syscall_with_features)\n"
          "    return env->syscall_with_features(env->context, &invocation, "
          "value);\n"
          "  if (!env->syscall) return "
       << cSourceStatusName(SourceStatus::UnknownSyscall)
       << ";\n"
          "  return env->syscall(env->context, hash";
    for (unsigned Index = 0; Index < kArgumentRegisterCount; ++Index)
      OS << ", a" << Index;
    OS << ", value);\n"
          "}\n\n";
  }
  OS << "enum { NEVERD_SBF_REGISTER_COUNT = " << kRegisterCount
     << ", NEVERD_SBF_RETURN_REGISTER = " << kReturnRegister
     << ", NEVERD_SBF_INPUT_REGISTER = " << kFirstArgumentRegister
     << ", NEVERD_SBF_INSTRUCTION_DATA_REGISTER = " << kInstructionDataRegister
     << ", NEVERD_SBF_FRAME_POINTER = " << kFramePointerRegister
     << ", NEVERD_SBF_FIRST_SAVED_REGISTER = " << kFirstCalleeSavedRegister
     << ", NEVERD_SBF_SAVED_REGISTERS = " << kCalleeSavedRegisterCount
     << ", NEVERD_SBF_MAX_CALL_DEPTH = " << Program.Config.MaxCallDepth
     << " };\n"
        "#define NEVERD_SBF_TEXT_ADDRESS "
     << word(Program.Low.TextAddress)
     << "\n"
        "#define NEVERD_SBF_INSTRUCTION_SIZE UINT64_C("
     << kInstructionSize
     << ")\n"
        "#define NEVERD_SBF_INSTRUCTION_COUNT UINT32_C("
     << Program.Low.Instructions.size()
     << ")\n"
        "#define NEVERD_SBF_STACK_START "
     << word(kStackStart)
     << "\n"
        "#define NEVERD_SBF_STACK_FRAME_SIZE UINT64_C("
     << Program.Config.StackFrameSize
     << ")\n"
        "#define NEVERD_SBF_STACK_SIZE UINT64_C("
     << stackSize(Program.Config) << ")\n\n";

  if (NeedsSignExtension)
    OS << "static uint64_t nd_sext32(uint32_t v) { return (v & "
          "UINT32_C(0x80000000)) ? (UINT64_C(0xffffffff00000000) | v) : v; }\n";
  if (NeedsSignedCompare32)
    OS << "static int nd_sgt32(uint32_t a, uint32_t b) { return (a ^ "
          "UINT32_C(0x80000000)) > (b ^ UINT32_C(0x80000000)); }\n";
  if (NeedsSignedCompare64)
    OS << "static int nd_sgt64(uint64_t a, uint64_t b) { return (a ^ "
          "UINT64_C(0x8000000000000000)) > (b ^ "
          "UINT64_C(0x8000000000000000)); }\n";
  if (NeedsArithmeticShift32)
    OS << "static uint32_t nd_ashr32(uint32_t v, uint64_t s) { unsigned n = "
          "(unsigned)s & 31u; if (!n) return v; return (v >> n) | ((v & "
          "UINT32_C(0x80000000)) ? (~UINT32_C(0) << (32u - n)) : 0); }\n";
  if (NeedsArithmeticShift64)
    OS << "static uint64_t nd_ashr64(uint64_t v, uint64_t s) { unsigned n = "
          "(unsigned)s & 63u; if (!n) return v; return (v >> n) | ((v & "
          "UINT64_C(0x8000000000000000)) ? (~UINT64_C(0) << (64u - n)) : 0); "
          "}\n";
  if (NeedsUnsignedHighMultiply)
    OS << "static uint64_t nd_umulh64(uint64_t a, uint64_t b) { uint64_t a0 "
          "= (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32; "
          "uint64_t w0 = a0*b0, t = a1*b0 + (w0 >> 32), w1 = (uint32_t)t, "
          "w2 = t >> 32; w1 += a0*b1; return a1*b1 + w2 + (w1 >> 32); }\n";
  if (NeedsSignedHighMultiply)
    OS << "static uint64_t nd_smulh64(uint64_t a, uint64_t b) { return "
          "nd_umulh64(a,b) - ((a >> 63) ? b : 0) - ((b >> 63) ? a : 0); }\n";
  if (NeedsByteSwap)
    OS << "static uint64_t nd_bswap(uint64_t v, unsigned bits) { if (bits == "
          "16) return ((v & 0xffu) << 8) | ((v >> 8) & 0xffu); if (bits == "
          "32) { v = ((v & UINT64_C(0x00ff00ff)) << 8) | ((v >> 8) & "
          "UINT64_C(0x00ff00ff)); return (v << 16) | (v >> 16); } v = ((v "
          "& UINT64_C(0x00ff00ff00ff00ff)) << 8) | ((v >> 8) & "
          "UINT64_C(0x00ff00ff00ff00ff)); v = ((v & "
          "UINT64_C(0x0000ffff0000ffff)) << 16) | ((v >> 16) & "
          "UINT64_C(0x0000ffff0000ffff)); return (v << 32) | (v >> 32); }\n";
  if (NeedsSignedDivision)
    OS << "static uint64_t nd_sdivrem(uint64_t a, uint64_t b, unsigned bits, "
          "int rem, neverd_sbf_status_v2 *fault) { uint64_t mask = bits == "
          "32 ? "
          "UINT32_MAX : "
          "UINT64_MAX, sign = bits == 32 ? (UINT64_C(1)<<31) : "
          "(UINT64_C(1)<<63); a &= mask; b &= mask; if (!b) { *fault = "
       << cSourceStatusName(FaultCode::DivideByZero)
       << "; return 0; } if (a == sign && b == mask) { *fault = "
       << cSourceStatusName(FaultCode::DivideOverflow)
       << "; return 0; } { int na = "
          "(a&sign)!=0, nb=(b&sign)!=0; uint64_t ua=na?((~a+1)&mask):a, "
          "ub=nb?((~b+1)&mask):b, value=rem?(ua%ub):(ua/ub); if ((rem?na:"
          "(na!=nb)) && value) value=(~value+1)&mask; return value; } }\n";
  OS << "\n";

  if (Options.IncludeAnalysisComments) {
    OS << "/* Recovered: " << Program.High.Functions.size() << " function(s), "
       << Program.High.Syscalls.size() << " syscall site(s), "
       << Program.High.Regions.size() << " structured region(s). */\n";
    for (const Region &Region : Program.High.Regions)
      OS << "/* " << (Region.Kind == RegionKind::Loop ? "loop" : "if")
         << " at block_" << Region.HeaderBlock << " */\n";
  }

  // The loader hands the program the input buffer and, on a runtime that has
  // activated it, the instruction data. A callable that only takes the first
  // cannot reproduce a program that reads the second.
  const auto EmitPublicEntrypoints = [&] {
    OS << "neverd_sbf_status " << Options.FunctionName
       << "(neverd_sbf_environment *env, uint64_t input, "
          "uint64_t instruction_data, uint64_t *result) {\n"
          "  nd_environment_view view;\n"
          "  if (!env) return nd_legacy_status(nd_program_impl(\n"
          "      NULL, input, instruction_data, result));\n"
          "  view.context = env->context;\n"
          "  view.load = env->load;\n"
          "  view.store = env->store;\n"
          "  view.syscall = env->syscall;\n"
          "  view.syscall_with_features = NULL;\n"
          "  view.runtime_features = NULL;\n"
          "  view.status_abi = ND_STATUS_ABI_V1;\n"
          "  return nd_legacy_status(nd_program_impl(\n"
          "      &view, input, instruction_data, result));\n"
          "}\n\n"
          "neverd_sbf_status_v2 "
       << Options.FunctionName
       << "_v2(neverd_sbf_environment_v2 *env, uint64_t input, "
          "uint64_t instruction_data, uint64_t *result) {\n"
          "  nd_environment_view view;\n"
          "  if (!env) return nd_program_impl(NULL, input, instruction_data, "
          "result);\n"
          "  view.context = env->base.context;\n"
          "  view.load = env->base.load;\n"
          "  view.store = env->base.store;\n"
          "  view.syscall = env->base.syscall;\n"
          "  view.syscall_with_features = env->syscall_with_features;\n"
          "  view.runtime_features = env->runtime_features;\n"
          "  view.status_abi = ND_STATUS_ABI_V2;\n"
          "  return nd_program_impl(&view, input, instruction_data, result);\n"
          "}\n";
  };

  OS << "static neverd_sbf_status_v2 nd_program_impl("
        "nd_environment_view *env, uint64_t input, "
        "uint64_t instruction_data, uint64_t *result) {\n"
        "  (void)env;\n"
        "  (void)result;\n"
        "  uint64_t r[NEVERD_SBF_REGISTER_COUNT] = {0};\n";
  if (!Structured)
    OS << "  uint64_t "
          "saved[NEVERD_SBF_MAX_CALL_DEPTH][NEVERD_SBF_SAVED_REGISTERS] = "
          "{{0}};\n"
          "  uint64_t saved_fp[NEVERD_SBF_MAX_CALL_DEPTH] = {0};\n"
          "  uint32_t return_pc[NEVERD_SBF_MAX_CALL_DEPTH] = {0};\n"
          "  size_t depth = 0, i = 0; uint32_t pc = "
       << Program.Low.EntrySlot << ";\n";
  OS << "  r[NEVERD_SBF_INPUT_REGISTER] = input; "
        "r[NEVERD_SBF_INSTRUCTION_DATA_REGISTER] = instruction_data; "
        "r[NEVERD_SBF_FRAME_POINTER] = NEVERD_SBF_STACK_START "
        "+ "
     << (versionHasFeature(Program.Low.TheVersion,
                           VersionFeature::ManualStackFrames)
             ? "NEVERD_SBF_STACK_SIZE"
             : "NEVERD_SBF_STACK_FRAME_SIZE")
     << ";\n";
  if (Structured) {
    if (!emitStructuredNodes(OS, Program, BySlot, *Structured, "  "))
      return llvm::make_error<llvm::StringError>(
          "sbf: structured C emission rejected its validated control-flow plan",
          llvm::inconvertibleErrorCode());
    OS << "  return " << cSourceStatusName(FaultCode::ExecutionOverrun)
       << ";\n}\n";
    EmitPublicEntrypoints();
    return Buffer;
  }

  OS << "  for (;;) {\n    switch (pc) {\n";
  for (size_t Slot = 0; Slot < Program.Low.Instructions.size(); ++Slot) {
    if (Program.Low.Instructions[Slot].IsContinuation) {
      OS << "      case " << Slot << ": return "
         << cSourceStatusName(FaultCode::InvalidInstruction) << ";\n";
      continue;
    }
    auto It = BySlot.find(Slot);
    if (It == BySlot.end()) {
      OS << "      case " << Slot << ": return "
         << cSourceStatusName(FaultCode::InvalidInstruction) << ";\n";
      continue;
    }
    emitInstruction(OS, *It->second, Program);
  }
  OS << "      default: return "
     << cSourceStatusName(FaultCode::ExecutionOverrun) << ";\n    }\n  }\n}\n";
  EmitPublicEntrypoints();
  return Buffer;
}

} // namespace neverd::sbf
