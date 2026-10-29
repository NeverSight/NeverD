//===- X64ContinuationExport.cpp - Explicit x64 state source -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Export the shared instruction-state semantics without guessing a C ABI.
//===----------------------------------------------------------------------===//

#include "neverd/pipeline/X64ContinuationExport.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

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
                                       "segment_base_msr_address_bits",
                                       "native_timestamp_reads",
                                       "native_write_prefetch",
                                       "native_speculation_control_msrs",
                                       "native_descriptor_queries",
                                       "native_page_table_access",
                                       "max_instructions",
                                       "max_operations"};
  for (const auto &[Key, Value] : *Object)
    if (!Keys.count(Key))
      return invalid("unknown x64 continuation option: " +
                     llvm::StringRef(Key));
  if (Object->getString("profile") != "kernel-events-v6")
    return invalid("an explicit kernel-events-v6 profile is required");
  Request Result;
  auto &Options = Result.Options;
  Options.Profile = X64ContinuationProfile::KernelEventsNoFaultV6;
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
  auto Paging = RequiredInteger("paging_address_bits");
  if (!Paging)
    return Paging.takeError();
  if (*Paging != 48 && *Paging != 57)
    return invalid("paging_address_bits must be 48 or 57");
  Options.PagingAddressBits = unsigned(*Paging);
  if (const auto *Value = Object->get("segment_base_msr_address_bits")) {
    auto Bits = integer(*Value);
    if (!Bits)
      return Bits.takeError();
    if (*Bits != 48 && *Bits != 57)
      return invalid("segment_base_msr_address_bits must be 48 or 57");
    Options.SegmentBaseMSRAddressBits = unsigned(*Bits);
  }
  const std::pair<llvm::StringRef, bool *> Bindings[] = {
      {"native_timestamp_reads", &Options.NativeTimestampReads},
      {"native_write_prefetch", &Options.NativeWritePrefetch},
      {"native_speculation_control_msrs",
       &Options.NativeSpeculationControlMsrs},
      {"native_descriptor_queries", &Options.NativeDescriptorQueries},
      {"native_page_table_access", &Options.NativePageTableAccess}};
  for (const auto &[Key, Destination] : Bindings)
    if (const auto *Value = Object->get(Key)) {
      auto Flag = Value->getAsBoolean();
      if (!Flag)
        return invalid(Key + " must be a boolean");
      *Destination = *Flag;
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
  return Result;
}

// This flattened C view is checked field-by-field against the semantic owner's
// versioned layouts. It adds no storage or initialization assumptions.
llvm::json::Array stateDeclaration(llvm::raw_ostream &OS) {
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
    uint64_t ReservedTailV2, CR2, ReservedV4, STAR;
    neverd_x64_segment CS, SS;
    uint64_t ReservedV5;
    neverd_x64_descriptor_table GDTR, IDTR;
    neverd_x64_segment LDTR, TR, DS, ES;
    neverd_x64_segment_metadata FS, GS;
    uint8_t Unusable, NMIBlocked, ReservedV6[14];
} neverd_x64_state_v6;
)C";
  llvm::json::Array Fields;
  const auto Field = [&](llvm::StringRef Name, size_t Offset, size_t Bytes) {
    OS << "_Static_assert(offsetof(neverd_x64_state_v6, " << Name
       << ") == " << Offset << ", \"x64 state offset\");\n";
    OS << "_Static_assert(sizeof(((neverd_x64_state_v6 *)0)->" << Name
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
#undef FIELD
  OS << "_Static_assert(sizeof(neverd_x64_state_v6) == "
     << sizeof(X64ContinuationStateV6) << ", \"x64 state size\");\n";
  OS << "_Static_assert(_Alignof(neverd_x64_state_v6) == "
     << alignof(X64ContinuationStateV6) << ", \"x64 state alignment\");\n";
  return Fields;
}

void driver(llvm::raw_ostream &OS,
            llvm::ArrayRef<X64ContinuationInstructionSource> Units) {
  OS << R"C(
/* State and budget are accessible and disjoint from each other and guest RAM.
 * Every successful instruction consumes one step, including exiting transfers.
 * Native requests and invalid-state rejection consume none. CALL/RET always
 * stop, even for listed targets; the environment may change all state before
 * resuming. Arbitrary-value slots are caller-selected per dynamic instruction;
 * leaving them unchanged selects a subset of permitted executions.
 * 0..7: instruction exit statuses; 8: outside selection; 9: budget exhausted. */
uint64_t neverd_x64_continue_v6(neverd_x64_state_v6 *state, uint64_t *budget) {
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
     << ") return status;\n        --*budget;\n        if (status != "
     << uint64_t(X64ContinuationExitV1::Continue)
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
    PipelineOptions Options;
    Options.OnlyFunctionEntries.insert(Addresses.begin(), Addresses.end());
    Options.DeferMedIR = true;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Context;
    auto Result = Pipeline().run(Image, Context, Options, Debug);
    if (!Result.Success)
      return invalid("selected CFG recovery failed: " + Result.Error);
    std::set<va_t> Unique;
    std::set<va_t> Found;
    for (const auto &Audit : Result.FunctionAudits) {
      if (!Options.OnlyFunctionEntries.count(Audit.Entry))
        continue;
      if (!Audit.DecodeFailures.empty())
        return invalid("selected entry " + hex(Audit.Entry) +
                       " contains an undecodable instruction at " +
                       hex(Audit.DecodeFailures.front()));
      if (Audit.NativeInstructionAddresses.empty())
        return invalid("selected entry " + hex(Audit.Entry) +
                       " has no decoded region: " +
                       pipelineFunctionDispositionName(Audit.Disposition));
      Found.insert(Audit.Entry);
      llvm::json::Array PCs;
      for (va_t Address : Audit.NativeInstructionAddresses) {
        Unique.insert(Address);
        PCs.push_back(hex(Address));
        if (Unique.size() > Request->Options.MaxInstructions)
          return invalid("selected CFG instruction budget exceeded");
      }
      Regions.push_back(llvm::json::Object{
          {"entry", hex(Audit.Entry)},
          {"instructions", std::move(PCs)},
          {"lowir_disposition",
           pipelineFunctionDispositionName(Audit.Disposition)},
          {"lowir_unproved_returns", uint64_t(Audit.UnprovenReturns.size())},
          {"lowir_unsupported_instructions",
           uint64_t(Audit.UnsupportedInstructions.size())},
          {"truncated_paths", uint64_t(Audit.TruncatedPaths.size())}});
    }
    if (Found != Options.OnlyFunctionEntries || Unique.empty())
      return invalid("selected entries did not produce instruction regions");
    Addresses.assign(Unique.begin(), Unique.end());
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
  OS << "/* Explicit x64 kernel-events-v6 state source. See the paired "
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
  auto Fields = stateDeclaration(OS);
  driver(OS, *Units);
  if (Result.Source.size() > limits::pipeline::kX64ContinuationSourceBytes)
    return invalid("x64 continuation source exceeds the output size limit");
  auto Binding = llvm::json::parse(RequestJSON);
  if (!Binding)
    return Binding.takeError();
  llvm::json::Object Report{
      {"schema_version", 2},
      {"profile", "kernel-events-v6"},
      {"interface", "explicit-machine-state"},
      {"coverage",
       "selected instruction regions; calls and exits require the environment"},
      {"source_route", UseLLVM ? "llvm" : "high"},
      {"source_sha256",
       llvm::toHex(
           llvm::SHA256::hash(llvm::arrayRefFromStringRef(Result.Source)),
           true)},
      {"state_version", 6},
      {"state_bytes", uint64_t(sizeof(X64ContinuationStateV6))},
      {"state_alignment", uint64_t(alignof(X64ContinuationStateV6))},
      {"state_fields", std::move(Fields)},
      {"request", std::move(*Binding)},
      {"total", uint64_t(Units->size())},
      {"modeled", uint64_t(Units->size() - Native)},
      {"native_requests", uint64_t(Native)},
      {"regions", std::move(Regions)},
      {"units", std::move(Manifest)},
      {"domain",
       "CPL0 64-bit entry; valid caches; declared active paging and MXCSR "
       "mask; CET/FRED disabled; successful sequential RAM; no MMIO, "
       "concurrent writes, faults or asynchronous events; exact immutable "
       "instruction bytes; accessible disjoint state and budget; native "
       "bindings require the declared executing environment"},
      {"ordinary_source_abi", false},
      {"ordinary_source_validation", "not-run"},
      {"whole_function_coverage", false}};
  llvm::raw_string_ostream(Result.Manifest)
      << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(Report)));
  return Result;
}
} // namespace neverd
