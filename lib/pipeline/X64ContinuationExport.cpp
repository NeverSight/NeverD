//===- X64ContinuationExport.cpp - Explicit x64 state source -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Export the shared instruction-state semantics without guessing a C ABI.
//===----------------------------------------------------------------------===//

#include "neverd/pipeline/X64ContinuationExport.h"

#include "X64ContinuationEnvironment.h"
#include "X64SpecialRegions.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace neverd {
namespace {
using namespace analysis;

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}

std::string hex(uint64_t Value) { return "0x" + llvm::utohexstr(Value); }

llvm::Expected<uint64_t> integer(const llvm::json::Value &Value) {
  if (const auto Text = Value.getAsString()) {
    uint64_t Result;
    if (!Text->empty() && !Text->starts_with('-') &&
        !Text->getAsInteger(0, Result))
      return Result;
  } else if (auto Number = Value.getAsInteger(); Number && *Number >= 0) {
    return uint64_t(*Number);
  }
  return invalid("expected an unsigned integer or hexadecimal string");
}

struct Request {
  X64ContinuationSourceOptions Options;
  std::vector<va_t> Addresses;
  bool Entries = false;
  bool FollowInternalCalls = false;
  bool FollowDirectJumps = false;
  unsigned CalleeDepth = 0;
  bool DirectDispatch = false;
};

llvm::Expected<Request> readRequest(llvm::StringRef Text) {
  if (Text.empty() ||
      Text.size() > limits::pipeline::kX64ContinuationRequestBytes)
    return invalid(
        "x64 continuation request size is outside its allowed limit");
  auto Parsed = llvm::json::parse(Text);
  if (!Parsed)
    return Parsed.takeError();
  const auto *Object = Parsed->getAsObject();
  if (!Object)
    return invalid("x64 continuation request must be an object");
  const std::set<llvm::StringRef> Keys{"profile",
                                       "instructions",
                                       "entries",
                                       "mxcsr_mask",
                                       "paging_address_bits",
                                       "null_segment_base_policy",
                                       "segment_base_msr_address_bits",
                                       "sysenter_msr_address_bits",
                                       "uiret_restores_uif",
                                       "native_timestamp_reads",
                                       "native_write_prefetch",
                                       "native_speculation_control_msrs",
                                       "native_descriptor_queries",
                                       "native_page_table_access",
                                       "native_kvm_hypercalls",
                                       "follow_internal_calls",
                                       "follow_direct_jumps",
                                       "callee_depth",
                                       "dispatch",
                                       "max_instructions",
                                       "max_operations"};
  for (const auto &[Key, Value] : *Object)
    if (!Keys.count(Key))
      return invalid("unknown x64 continuation option: " +
                     llvm::StringRef(Key));
  const auto Profile = Object->getString("profile");
  if (Profile != "kernel-events-v6" && Profile != "intel-fast-system-v7" &&
      Profile != "intel-user-interrupt-v8" &&
      Profile != "intel-fred-return-v9" && Profile != "amd-svm-v10" &&
      Profile != "intel-mpx-v11")
    return invalid(
        "an explicit kernel-events-v6, intel-fast-system-v7 or "
        "intel-user-interrupt-v8, intel-fred-return-v9, amd-svm-v10 or "
        "intel-mpx-v11 profile "
        "is required");
  Request Result;
  auto &Options = Result.Options;
  Options.Profile =
      Profile == "intel-mpx-v11" ? X64ContinuationProfile::MpxNoFaultV11
      : Profile == "amd-svm-v10" ? X64ContinuationProfile::NativeSvmNoFaultV10
      : Profile == "intel-fred-return-v9"
          ? X64ContinuationProfile::FredReturnNoFaultV9
      : Profile == "intel-user-interrupt-v8"
          ? X64ContinuationProfile::UserInterruptNoFaultV8
      : Profile == "intel-fast-system-v7"
          ? X64ContinuationProfile::KernelFastSystemNoFaultV7
          : X64ContinuationProfile::KernelEventsNoFaultV6;
  const auto RequiredInteger =
      [&](llvm::StringRef Key) -> llvm::Expected<uint64_t> {
    const auto *Value = Object->get(Key);
    if (!Value)
      return invalid("missing x64 continuation option: " + Key);
    return integer(*Value);
  };
  auto Mask = RequiredInteger("mxcsr_mask");
  if (!Mask)
    return Mask.takeError();
  if (*Mask > std::numeric_limits<uint32_t>::max())
    return invalid("mxcsr_mask must fit in 32 bits");
  Options.MXCSRMask = uint32_t(*Mask);
  if ((Profile != "amd-svm-v10" && Profile != "intel-mpx-v11") ||
      Object->get("paging_address_bits")) {
    auto Paging = RequiredInteger("paging_address_bits");
    if (!Paging)
      return Paging.takeError();
    if (*Paging != 48 && *Paging != 57)
      return invalid("paging_address_bits must be 48 or 57");
    Options.PagingAddressBits = unsigned(*Paging);
  }
  if (const auto *Value = Object->get("null_segment_base_policy")) {
    const auto Policy = Value->getAsString();
    if (Policy == "clear")
      Options.NullSegmentBasePolicy = X64NullSegmentBasePolicy::Clear;
    else if (Policy == "preserve")
      Options.NullSegmentBasePolicy = X64NullSegmentBasePolicy::Preserve;
    else
      return invalid("null_segment_base_policy must be clear or preserve");
  }
  if (const auto *Value = Object->get("segment_base_msr_address_bits")) {
    auto Bits = integer(*Value);
    if (!Bits)
      return Bits.takeError();
    if (*Bits != 48 && *Bits != 57)
      return invalid("segment_base_msr_address_bits must be 48 or 57");
    Options.SegmentBaseMSRAddressBits = unsigned(*Bits);
  }
  if (const auto *Value = Object->get("sysenter_msr_address_bits")) {
    auto Bits = integer(*Value);
    if (!Bits)
      return Bits.takeError();
    if (*Bits != 48 && *Bits != 57)
      return invalid("sysenter_msr_address_bits must be 48 or 57");
    Options.SysenterMSRAddressBits = unsigned(*Bits);
  }
  const std::pair<llvm::StringRef, bool *> Bindings[] = {
      {"native_timestamp_reads", &Options.NativeTimestampReads},
      {"native_write_prefetch", &Options.NativeWritePrefetch},
      {"native_speculation_control_msrs",
       &Options.NativeSpeculationControlMsrs},
      {"native_descriptor_queries", &Options.NativeDescriptorQueries},
      {"native_page_table_access", &Options.NativePageTableAccess},
      {"native_kvm_hypercalls", &Options.NativeKvmHypercalls},
      {"follow_internal_calls", &Result.FollowInternalCalls},
      {"follow_direct_jumps", &Result.FollowDirectJumps}};
  if (const auto *Value = Object->get("uiret_restores_uif")) {
    const auto Flag = Value->getAsBoolean();
    if (!Flag)
      return invalid("uiret_restores_uif must be a boolean");
    Options.UiretRestoresUIF = *Flag;
  }
  for (const auto &[Key, Destination] : Bindings)
    if (const auto *Value = Object->get(Key)) {
      auto Flag = Value->getAsBoolean();
      if (!Flag)
        return invalid(Key + " must be a boolean");
      *Destination = *Flag;
    }
  if (const auto *Value = Object->get("dispatch")) {
    const auto Dispatch = Value->getAsString();
    if (Dispatch != "switch" && Dispatch != "direct")
      return invalid("dispatch must be switch or direct");
    Result.DirectDispatch = Dispatch == "direct";
  }
  if (const auto *Value = Object->get("callee_depth")) {
    auto Depth = integer(*Value);
    if (!Depth)
      return Depth.takeError();
    if (*Depth > limits::pipeline::kX64ContinuationCalleeDepth)
      return invalid("callee_depth exceeds the bounded discovery limit");
    if (!Object->get("entries") || !Result.FollowInternalCalls)
      return invalid("callee_depth requires entries and follow_internal_calls");
    Result.CalleeDepth = unsigned(*Depth);
  }
  const std::pair<llvm::StringRef, uint64_t *> Budgets[] = {
      {"max_instructions", &Options.MaxInstructions},
      {"max_operations", &Options.MaxOperations}};
  for (const auto &[Key, Destination] : Budgets)
    if (const auto *Value = Object->get(Key)) {
      auto Budget = integer(*Value);
      if (!Budget)
        return Budget.takeError();
      if (!*Budget || *Budget > *Destination)
        return invalid(Key + " must be between 1 and " +
                       llvm::Twine(*Destination));
      *Destination = *Budget;
    }
  if (bool(Object->get("entries")) == bool(Object->get("instructions")))
    return invalid("select exactly one of entries or instructions");
  Result.Entries = Object->get("entries") != nullptr;
  if (Object->get("follow_direct_jumps") && !Result.Entries)
    return invalid("follow_direct_jumps requires entries");
  const auto *Addresses =
      Object->getArray(Result.Entries ? "entries" : "instructions");
  if (!Addresses || Addresses->empty() ||
      Addresses->size() > Options.MaxInstructions)
    return invalid("address selection must be a nonempty bounded array");
  std::set<va_t> Unique;
  for (const auto &Value : *Addresses) {
    auto Address = integer(Value);
    if (!Address)
      return Address.takeError();
    if (!Unique.insert(*Address).second)
      return invalid("duplicate selected address: " + hex(*Address));
    Result.Addresses.push_back(*Address);
  }
  return Result;
}

llvm::json::Array effects(const X64ContinuationInstructionSource &Unit) {
  llvm::json::Array Result;
  for (size_t I = 0; I < Unit.UndefinedEffects.size(); ++I) {
    const auto &Effect = Unit.UndefinedEffects[I];
    Result.push_back(
        llvm::json::Object{{"slot", uint64_t(I)},
                           {"after_operation", uint64_t(Effect.AfterOp)},
                           {"output_space", int64_t(Effect.Output.Space)},
                           {"output_offset", hex(Effect.Output.Offset)},
                           {"output_bytes", int64_t(Effect.Output.Size)},
                           {"bit_offset", int64_t(Effect.BitOffset)},
                           {"bit_count", int64_t(Effect.BitCount)},
                           {"conditional", bool(Effect.When)}});
  }
  for (const auto &Choice : Unit.CompatibilityChoices)
    Result.push_back(llvm::json::Object{
        {"slot", int64_t(Choice.Slot)},
        {"after_profile_transition", "successful_compatibility_return"},
        {"output_space", int64_t(Choice.Output.Space)},
        {"output_offset", hex(Choice.Output.Offset)},
        {"output_bytes", int64_t(Choice.Output.Size)},
        {"bit_offset", 32},
        {"bit_count", 32},
        {"conditional", true}});
  return Result;
}

// This flattened C view is checked field-by-field against the semantic owner's
// versioned layouts. It adds no storage or initialization assumptions.
llvm::json::Array stateDeclaration(llvm::raw_ostream &OS, unsigned Version,
                                   uint64_t Bytes) {
  const auto Type = "neverd_x64_state_v" + std::to_string(Version);
  OS << R"C(
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef struct { uint64_t Base; uint32_t Limit; uint16_t Selector, Attributes; } neverd_x64_segment;
typedef struct { uint64_t Base; uint16_t Limit, Reserved; uint32_t ReservedTail; } neverd_x64_descriptor_table;
typedef struct { uint32_t Limit; uint16_t Selector, Attributes; } neverd_x64_segment_metadata;
typedef struct {
    _Alignas(16) uint64_t GPR[16];
    uint64_t RFlags, PC, FSBase, GSBase;
    uint8_t ZMM[32][64];
    uint64_t Undefined[8], CR8, KernelGSBase;
    uint32_t MXCSR, ReservedV2;
    uint64_t ReservedTailV2, CR2, ReservedV4;
)C";
  if (Version == 11)
    OS << R"C(
    uint64_t Bounds[4][2], BNDCFGS, BNDSTATUS, XCR0;
    uint8_t MPXSupported, OSXSAVE, InTransaction, ReservedV11[5];
)C";
  else if (Version == 10)
    OS << R"C(
    struct { uint64_t VMCB, GuestXState, HostXState, XStateMask, GPR[14]; } SVM;
)C";
  else
    OS << R"C(
    uint64_t STAR;
    neverd_x64_segment CS, SS;
    uint64_t ReservedV5;
    neverd_x64_descriptor_table GDTR, IDTR;
    neverd_x64_segment LDTR, TR, DS, ES;
    neverd_x64_segment_metadata FS, GS;
    uint8_t Unusable, NMIBlocked, ReservedV6[14];
)C";
  if (Version >= 7 && Version <= 9)
    OS << "    uint64_t SysenterCS, SysenterESP, SysenterEIP, ReservedV7;\n";
  if (Version >= 8 && Version <= 9)
    OS << "    uint8_t UIF, MonitorActive, ReservedV8[14];\n";
  if (Version == 9)
    OS << "    uint64_t FredConfig; uint8_t STIBlocked, PendingDebug, "
          "ReservedV9[6];\n";
  OS << "} " << Type << ";\n";
  llvm::json::Array Fields;
  const auto Field = [&](llvm::StringRef Name, size_t Offset, size_t Bytes) {
    OS << "_Static_assert(offsetof(" << Type << ", " << Name
       << ") == " << Offset << ", \"x64 state offset\");\n";
    OS << "_Static_assert(sizeof(((" << Type << " *)0)->" << Name
       << ") == " << Bytes << ", \"x64 state field size\");\n";
    Fields.push_back(llvm::json::Object{{"name", Name},
                                        {"offset", uint64_t(Offset)},
                                        {"bytes", uint64_t(Bytes)}});
  };
#define FIELD(Type, CName, Member)                                             \
  Field(#CName, offsetof(Type, Member), sizeof(((Type *)nullptr)->Member))
  FIELD(X64ContinuationStateV1, GPR, GPR);
  FIELD(X64ContinuationStateV1, RFlags, RFlags);
  FIELD(X64ContinuationStateV1, PC, PC);
  FIELD(X64ContinuationStateV1, FSBase, FSBase);
  FIELD(X64ContinuationStateV1, GSBase, GSBase);
  FIELD(X64ContinuationStateV1, ZMM, ZMM);
  FIELD(X64ContinuationStateV1, Undefined, Undefined);
  FIELD(X64ContinuationStateV2, CR8, CR8);
  FIELD(X64ContinuationStateV2, KernelGSBase, KernelGSBase);
  FIELD(X64ContinuationStateV2, MXCSR, MXCSR);
  FIELD(X64ContinuationStateV2, ReservedV2, Reserved);
  FIELD(X64ContinuationStateV2, ReservedTailV2, ReservedTail);
  FIELD(X64ContinuationStateV4, CR2, CR2);
  FIELD(X64ContinuationStateV4, ReservedV4, Reserved);
  if (Version == 11) {
    FIELD(X64ContinuationStateV11, Bounds, Bounds);
    FIELD(X64ContinuationStateV11, BNDCFGS, BNDCFGS);
    FIELD(X64ContinuationStateV11, BNDSTATUS, BNDSTATUS);
    FIELD(X64ContinuationStateV11, XCR0, XCR0);
    FIELD(X64ContinuationStateV11, MPXSupported, MPXSupported);
    FIELD(X64ContinuationStateV11, OSXSAVE, OSXSAVE);
    FIELD(X64ContinuationStateV11, InTransaction, InTransaction);
    FIELD(X64ContinuationStateV11, ReservedV11, Reserved);
  } else if (Version == 10) {
    FIELD(X64ContinuationStateV10, SVM, SVM);
  } else {
    FIELD(X64ContinuationStateV5, STAR, STAR);
    FIELD(X64ContinuationStateV5, CS, CS);
    FIELD(X64ContinuationStateV5, SS, SS);
    FIELD(X64ContinuationStateV5, ReservedV5, Reserved);
    FIELD(X64ContinuationStateV6, GDTR, GDTR);
    FIELD(X64ContinuationStateV6, IDTR, IDTR);
    FIELD(X64ContinuationStateV6, LDTR, LDTR);
    FIELD(X64ContinuationStateV6, TR, TR);
    FIELD(X64ContinuationStateV6, DS, DS);
    FIELD(X64ContinuationStateV6, ES, ES);
    FIELD(X64ContinuationStateV6, FS, FS);
    FIELD(X64ContinuationStateV6, GS, GS);
    FIELD(X64ContinuationStateV6, Unusable, Unusable);
    FIELD(X64ContinuationStateV6, NMIBlocked, NMIBlocked);
    FIELD(X64ContinuationStateV6, ReservedV6, Reserved);
  }
  if (Version >= 7 && Version <= 9) {
    FIELD(X64ContinuationStateV7, SysenterCS, SysenterCS);
    FIELD(X64ContinuationStateV7, SysenterESP, SysenterESP);
    FIELD(X64ContinuationStateV7, SysenterEIP, SysenterEIP);
    FIELD(X64ContinuationStateV7, ReservedV7, Reserved);
  }
  if (Version >= 8 && Version <= 9) {
    FIELD(X64ContinuationStateV8, UIF, UIF);
    FIELD(X64ContinuationStateV8, MonitorActive, MonitorActive);
    FIELD(X64ContinuationStateV8, ReservedV8, Reserved);
  }
  if (Version == 9) {
    FIELD(X64ContinuationStateV9, FredConfig, FredConfig);
    FIELD(X64ContinuationStateV9, STIBlocked, STIBlocked);
    FIELD(X64ContinuationStateV9, PendingDebug, PendingDebug);
    FIELD(X64ContinuationStateV9, ReservedV9, Reserved);
  }
#undef FIELD
  OS << "_Static_assert(sizeof(" << Type << ") == " << Bytes
     << ", \"x64 state size\");\n";
  OS << "_Static_assert(_Alignof(" << Type
     << ") == " << alignof(X64ContinuationStateV6)
     << ", \"x64 state alignment\");\n";
  return Fields;
}

void driver(llvm::raw_ostream &OS,
            llvm::ArrayRef<X64ContinuationInstructionSource> Units,
            bool FollowInternalCalls) {
  OS << R"C(
/* State and budget are accessible and disjoint from each other and guest RAM.
 * Every successful instruction consumes one step, including exiting transfers.
 * Native requests and invalid-state rejection consume none. The environment
 * may change all state at a stopping boundary before resuming.
 * Arbitrary-value slots are caller-selected per dynamic instruction;
 * leaving them unchanged selects a subset of permitted executions.
 * 0..7,10..14: instruction exits; 8: outside selection; 9: budget exhausted.
 * MPX #BR counts one attempt and stops at the original faulting PC. */
)C";
  OS << (FollowInternalCalls
             ? "/* CALL/RET continue only when their actual target is "
               "selected.\n"
               " * The physical stack owns return PCs; no host call stack is "
               "used. */\n"
             : "/* CALL/RET stop even when their target is selected. */\n");
  const auto Version = Units.front().StateVersion;
  OS << "uint64_t neverd_x64_continue_v" << Version << "(neverd_x64_state_v"
     << Version << " *state, uint64_t *budget) {\n";
  OS << R"C(
    uint64_t last = 8;
    while (*budget) {
        uint64_t status;
        switch (state->PC) {
)C";
  for (const auto &Unit : Units)
    OS << "        case " << hex(Unit.Address)
       << "ULL: status = " << Unit.Function.Name
       << "((uint8_t *)state); break;\n";
  OS << "        default: return last == "
     << uint64_t(X64ContinuationExitV1::Jump) << " ? "
     << uint64_t(X64ContinuationExitV1::Jump) << " : 8;\n"
     << "        }\n        if (status == "
     << uint64_t(X64ContinuationExitV1::NativeInstruction)
     << " || status == " << uint64_t(X64ContinuationExitV1::InvalidState)
     << ") return status;\n        --*budget;\n";
  if (FollowInternalCalls) {
    // Test the actual post-transfer PC, including a RET through changed guest
    // memory. An external transfer still reports its boundary when it consumes
    // the last budget unit; an internal transfer leaves execution budget-bound.
    OS << "        if (status == " << uint64_t(X64ContinuationExitV1::Call)
       << " || status == " << uint64_t(X64ContinuationExitV1::Return)
       << ") {\n            switch (state->PC) {\n";
    for (const auto &Unit : Units)
      OS << "            case " << hex(Unit.Address) << "ULL:\n";
    OS << "                break;\n            default: return status;\n"
          "            }\n            last = status;\n            continue;\n"
          "        }\n";
  }
  OS << "        if (status != " << uint64_t(X64ContinuationExitV1::Continue)
     << " && status != " << uint64_t(X64ContinuationExitV1::Jump)
     << ") return status;\n        last = status;\n    }\n    return 9;\n}\n";
}
} // namespace

llvm::Expected<X64ContinuationExport>
exportX64Continuation(const BinaryImage &Image, llvm::StringRef RequestJSON,
                      bool UseLLVM, DebugContext *Debug) {
  auto Request = readRequest(RequestJSON);
  if (!Request)
    return Request.takeError();
  if (Image.Arch != Arch::X64)
    return invalid("x64 continuation source requires an x86-64 image");
  llvm::json::Array Regions;
  std::vector<va_t> Addresses = Request->Addresses;
  if (Request->Entries) {
    auto Selection = selectX64SpecialRegions(Image, Addresses, Request->Options,
                                             Request->CalleeDepth,
                                             Request->FollowDirectJumps, Debug);
    if (!Selection)
      return Selection.takeError();
    Addresses = std::move(Selection->Addresses);
    Regions = std::move(Selection->Regions);
  }
  auto Units = buildX64ContinuationSource(Image, Addresses, Request->Options);
  if (!Units)
    return Units.takeError();
  std::vector<MedFunc> Functions;
  llvm::json::Array Manifest;
  size_t Native = 0;
  for (const auto &Unit : *Units) {
    LowToMedConverter Converter;
    std::map<va_t, SourceFunctionTypeHint> Hints{
        {Unit.Address, Unit.SourceABI}};
    Converter.setSourceCallHintsEnabled(true);
    Converter.setSourceEntryTypeHints(&Hints);
    auto Function = Converter.convert(Unit.Function, Arch::X64);
    Function.SourceTypeHint = Unit.SourceABI;
    inferMedTypes(Function, Arch::X64);
    if (!verifyMedFunc(Function, "x64-continuation-export"))
      return invalid("x64 continuation source failed MedIR verification");
    Functions.push_back(std::move(Function));
    Native += Unit.RequiresNativeExecution;
    Manifest.push_back(
        llvm::json::Object{{"pc", hex(Unit.Address)},
                           {"bytes", llvm::toHex(Unit.Bytes)},
                           {"native_boundary", Unit.RequiresNativeExecution},
                           {"diagnostic", Unit.Diagnostic},
                           {"name", Unit.Function.Name},
                           {"arbitrary_effects", effects(Unit)}});
  }
  X64ContinuationExport Result;
  llvm::raw_string_ostream OS(Result.Source);
  const auto Version = Units->front().StateVersion;
  const auto Profile = Version == 11   ? "intel-mpx-v11"
                       : Version == 10 ? "amd-svm-v10"
                       : Version == 9  ? "intel-fred-return-v9"
                       : Version == 8  ? "intel-user-interrupt-v8"
                       : Version == 7  ? "intel-fast-system-v7"
                                       : "kernel-events-v6";
  OS << "/* Explicit x64 " << Profile
     << " state source. See the paired "
        "manifest\n"
        " * for original bytes, native bindings and invocation domain. This "
        "is\n"
        " * a selected instruction region; it does not declare an ordinary "
        "ABI. */\n";
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::ELF;
  Options.UseUnalignedPointers = false;
  if (UseLLVM) {
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(Functions, Context);
    if (!Module || llvm::verifyModule(*Module, &llvm::errs()) ||
        !LLVMCEmitter().emit(*Module, OS, Options))
      return invalid("x64 continuation LLVM C emission failed");
  } else {
    std::vector<HighFunc> High;
    for (const auto &Function : Functions)
      High.push_back(MedToHighConverter().convert(Function, Arch::X64));
    if (!HighCEmitter().emit(High, OS, Options))
      return invalid("x64 continuation HighC emission failed");
  }
  // The emitter owns feature-test macros and system-header ordering. State
  // declarations need not precede byte-pointer steps; inserting their headers
  // first would freeze libc feature selection before the emitter's macros.
  auto Fields = stateDeclaration(OS, Version, Units->front().StateBytes);
  if (Request->DirectDispatch)
    emitX64SpecialRegionDriver(OS, *Units, Request->FollowInternalCalls);
  else
    driver(OS, *Units, Request->FollowInternalCalls);
  emitX64ContinuationEnvironment(OS, Version);
  if (Result.Source.size() > limits::pipeline::kX64ContinuationSourceBytes)
    return invalid("x64 continuation source exceeds the output size limit");
  auto Binding = llvm::json::parse(RequestJSON);
  if (!Binding)
    return Binding.takeError();
  llvm::json::Object Report{
      {"schema_version", 2},
      {"profile", Profile},
      {"interface", "explicit-machine-state"},
      {"environment_driver", "neverd_x64_run_v" + std::to_string(Version)},
      {"environment_binding",
       "explicit full-state callback; separately bounded"},
      {"call_return_policy",
       Request->FollowInternalCalls ? "follow-selected-targets" : "stop"},
      {"coverage",
       "selected instruction regions; external transfers and native or context "
       "boundaries require the environment"},
      {"source_route", UseLLVM ? "llvm" : "high"},
      {"source_sha256",
       llvm::toHex(
           llvm::SHA256::hash(llvm::arrayRefFromStringRef(Result.Source)),
           true)},
      {"state_version", Version},
      {"state_bytes", Units->front().StateBytes},
      {"state_alignment", uint64_t(alignof(X64ContinuationStateV6))},
      {"state_fields", std::move(Fields)},
      {"request", std::move(*Binding)},
      {"total", uint64_t(Units->size())},
      {"modeled", uint64_t(Units->size() - Native)},
      {"native_requests", uint64_t(Native)},
      {"regions", std::move(Regions)},
      {"units", std::move(Manifest)},
      {"domain",
       (Version == 11
            ? "CPL0 64-bit MPX with explicit bounds, BNDCFGS, BNDSTATUS, "
              "CPUID MPX, CR4.OSXSAVE and XCR0; no RTM/HLE; original near "
              "branches own conditional bounds initialization; #BR publishes "
              "BNDSTATUS and stops at the faulting PC with status 14 before "
              "exception delivery; host MPX disabled or BNDPRESERVE set; "
              "no host MPX instruction execution; CET/FRED disabled; "
        : Version == 10
            ? "native CPL0 AMD SVM; unchanged XCR0=7; explicit GPR, x87, "
              "XMM/YMM, MXCSR and CR2; native VMM owns VMCB, VM_HSAVE_PA, GIF, "
              "intercepts, debug/supervisor banks and mappings; FS/GS, "
              "KernelGSBase and CR8 agree with state and are guest-preserved; "
              "guest system changes preserve the compiler ABI; distinct "
              "64-byte-aligned standard 832-byte XSAVE images; code, state, "
              "buffers and compiler stack stay mapped and disjoint from "
              "guest writes and hardware-written VMCB/VM_HSAVE_PA pages; "
              "no nested SVM, SEV-ES, SMM or CET; VMEXIT stops "
              "with status 13, including invalid VMCBs; "
        : Version == 9
            ? "Intel FRED enabled; canonical ERETS/ERETU at CPL0; "
              "64-bit entry; ERETU permits compatibility return with explicit "
              "conditional GPR choices; explicit stack-level and event "
              "outputs; "
              "CET and RTM disabled; outside VMX non-root execution; "
              "no pending debug exception at entry; "
        : Version == 8
            ? "Intel UINTR enabled; CET/FRED disabled; outside enclaves/RTM; "
              "UIRET/CLUI/STUI/TESTUI "
              "accept CPL0..3 with declared UIRET_UIF; live UIF and monitor "
              "validity; "
              "SYSENTER accepts CPL0..3, other steps CPL0; live SYSENTER MSRs; "
              "64-bit entry; pending delivery belongs to the environment; "
        : Version == 7
            ? "Intel SEP; CET/FRED disabled; SYSENTER accepts CPL0..3, other "
              "steps CPL0; "
              "64-bit entry; live SYSENTER MSRs with declared processor width; "
            : "CPL0 64-bit entry; CET/FRED disabled; ") +
           std::string(
               Version >= 9
                   ? ""
                   : "16/32/64-bit IRET/RETF require canonical raw frame RIP; "
                     "compatibility targets own eight conditional "
                     "GPR high-half choices; ") +
           std::string(Version >= 10
                           ? "declared MXCSR mask; "
                           : "valid caches; declared active paging and "
                             "MXCSR mask; ") +
           std::string(Request->Options.NativeKvmHypercalls
                           ? "returning KVM ABI with RAX number/result and "
                             "RBX/RCX/RDX/RSI arguments; service depends only "
                             "on those inputs and mapped guest RAM; other "
                             "represented state and compiler environment "
                             "preserved; service writes exclude state, "
                             "budget, generated code and compiler stack; "
                           : "") +
           std::string(
               "successful sequential RAM; no MMIO, "
               "concurrent writes, memory faults or asynchronous events; exact "
               "immutable "
               "instruction bytes; accessible disjoint state and budget; "
               "native "
               "bindings require the declared executing environment")},
      {"ordinary_source_abi", false},
      {"ordinary_source_validation", "not-run"},
      {"whole_function_coverage", false}};
  llvm::raw_string_ostream(Result.Manifest)
      << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(Report)));
  return Result;
}
} // namespace neverd
