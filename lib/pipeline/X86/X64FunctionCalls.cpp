//===- X64FunctionCalls.cpp - Checked source call interfaces -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve original entry state while connecting scalar C call views to it.
//===----------------------------------------------------------------------===//

#include "X64FunctionCalls.h"

#include "X64FunctionCallSchedule.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <utility>

namespace neverd {

std::optional<uint64_t>
emitX64FunctionCallParameters(llvm::raw_ostream &OS,
                              const X64FunctionCallInterface &Call,
                              uint64_t StackBase) {
  const auto Layout =
      getTargetRegInfo(Arch::X64).integerArgumentLayout(BinaryFormat::COFF);
  std::optional<uint64_t> LastStackByte;
  for (size_t P = 0; P < Call.ArgumentBytes.size(); ++P)
    if (const auto Bytes = Call.ArgumentBytes[P]) {
      OS << ", uint" << *Bytes * 8 << "_t arg" << P;
      if (P >= Layout.Registers.size())
        LastStackByte = StackBase +
                        (P - Layout.Registers.size()) * Layout.SlotBytes +
                        *Bytes - 1;
    }
  return LastStackByte;
}

llvm::json::Array emitX64FunctionCallArguments(
    llvm::raw_ostream &OS, const X64FunctionCallInterface &Call,
    uint64_t StackBase, llvm::StringRef StackOffsetKey) {
  const auto &TRI = getTargetRegInfo(Arch::X64);
  const auto Layout = TRI.integerArgumentLayout(BinaryFormat::COFF);
  llvm::json::Array Arguments;
  for (size_t P = 0; P < Call.ArgumentBytes.size(); ++P) {
    const auto Bytes = Call.ArgumentBytes[P];
    llvm::json::Object Argument;
    std::string Type;
    if (Bytes) {
      Argument["bytes"] = *Bytes;
      Type = "uint" + std::to_string(*Bytes * 8) + "_t";
    } else {
      Argument["retained_in_state"] = true;
    }
    if (P < Layout.Registers.size()) {
      const auto Index = Layout.Registers[P] / TRI.PointerSize;
      if (Bytes)
        OS << "    if ((" << Type << ")state->GPR[" << Index << "] != arg" << P
           << ") return rejected;\n";
      Argument["gpr_index"] = Index;
    } else {
      const auto Offset =
          StackBase + (P - Layout.Registers.size()) * Layout.SlotBytes;
      if (Bytes)
        OS << "    " << Type << " stack_arg" << P << ";\n"
           << "    __builtin_memcpy(&stack_arg" << P
           << ", (const void *)(uintptr_t)(state->GPR[4] + " << Offset
           << "ULL), sizeof(stack_arg" << P << "));\n"
           << "    if (stack_arg" << P << " != arg" << P
           << ") return rejected;\n";
      Argument[StackOffsetKey] = Offset;
    }
    Arguments.push_back(std::move(Argument));
  }
  return Arguments;
}

llvm::json::Array
emitX64FunctionCalls(llvm::raw_ostream &OS,
                     llvm::ArrayRef<X64FunctionCallInterface> Calls,
                     bool PagedMemory, bool ScheduledEvents) {
  llvm::json::Array Manifest;
  if (Calls.empty())
    return Manifest;
  using Exit = analysis::X64ContinuationExitV1;
  const auto &TRI = getTargetRegInfo(Arch::X64);
  const auto Layout = TRI.integerArgumentLayout(BinaryFormat::COFF);
  OS << R"C(
/* Checked scalar call interfaces. The declared arguments must already match
 * native entry state. No original RAM or register byte is initialized here.
 * Zero-initialize call context; retain it and state across pauses. Resume does
 * not check entry arguments again. Value is valid only when Completed is 1.
 * Context, state, budgets and provider storage must be mutually disjoint and
)C";
  OS << (PagedMemory
             ? " * disjoint from guest RAM. Stack slots stay in paged state; "
               "entry checks read no guest RAM. */\n"
             : " * disjoint from guest RAM. Declared stack arguments require "
               "accessible RAM. */\n");
  OS << R"C(typedef struct neverd_x64_call_context_v1 {
    neverd_x64_function_context_v1 Function;
    uint64_t Entry, Value;
} neverd_x64_call_context_v1;
typedef struct neverd_x64_call_result_v1 {
    uint64_t Status, Completed, Value;
} neverd_x64_call_result_v1;
)C";
  for (const auto &Call : Calls) {
    const auto Entry = llvm::utohexstr(Call.Entry);
    const llvm::StringRef Prefix =
        PagedMemory ? (ScheduledEvents ? "paged_events_" : "paged_")
                    : (ScheduledEvents ? "events_" : "");
    const std::string Start =
        (llvm::Twine("neverd_x64_call_") + Prefix + Entry + "_v6").str();
    const std::string Resume =
        (llvm::Twine("neverd_x64_resume_") + Prefix + Entry + "_v6").str();
    const auto ResultType =
        "uint" + std::to_string(Call.ResultBytes * 8) + "_t";
    const llvm::StringRef Parameters =
        ScheduledEvents ? x64ScheduledCallParameters(PagedMemory, false)
        : PagedMemory
            ? "(neverd_x64_state_v6 *state, uint8_t *context, uint64_t "
              "*budget,\n"
              "    neverd_x64_call_context_v1 *call"
            : "(neverd_x64_state_v6 *state, uint64_t *budget, "
              "uint64_t *environment_budget,\n"
              "    neverd_x64_call_context_v1 *call, neverd_x64_environment_v6 "
              "environment, void *environment_context";
    OS << "\nneverd_x64_call_result_v1 " << Resume << Parameters << ") {\n"
       << "    neverd_x64_call_result_v1 result = {"
       << uint64_t(Exit::InvalidState) << ", 0, 0};\n"
       << "    if (call->Entry != 0x" << Entry
       << "ULL) return result;\n"
          "    if (call->Function.Phase == NEVERD_X64_FUNCTION_RETURNED) {\n"
          "        result.Status = "
       << uint64_t(Exit::Return)
       << "; result.Completed = 1; result.Value = call->Value;\n"
          "        return result;\n"
          "    }\n"
          "    result.Status = ";
    if (ScheduledEvents)
      OS << "neverd_x64_continue_" << (PagedMemory ? "paged_" : "")
         << "events_function_v6(state, "
         << (PagedMemory ? "memory_context, " : "")
         << "budget, shadows, poll_budget, peek, accepted, event_context, "
            "&call->Function);\n";
    else if (PagedMemory)
      OS << "neverd_x64_run_paged_function_v6(state, context, budget, "
            "&call->Function);\n";
    else
      OS << "neverd_x64_run_function_v6(state, budget, environment_budget,\n"
            "        &call->Function, environment, environment_context);\n";
    OS << "    if (call->Function.Phase == NEVERD_X64_FUNCTION_RETURNED) {\n"
          "        call->Value = ("
       << ResultType << ")state->GPR[" << TRI.IntReturnReg / TRI.PointerSize
       << "];\n"
          "        result.Completed = 1; result.Value = call->Value;\n"
          "    }\n"
          "    return result;\n}\n";
    OS << "neverd_x64_call_result_v1 " << Start << Parameters;
    const auto LastStackByte =
        emitX64FunctionCallParameters(OS, Call, Layout.EntryStackBase);
    OS << ") {\n"
          "    neverd_x64_call_result_v1 rejected = {"
       << uint64_t(Exit::InvalidState)
       << ", 0, 0};\n"
          "    if (call->Function.Phase != NEVERD_X64_FUNCTION_UNINITIALIZED "
          "||\n"
          "        state->PC != 0x"
       << Entry << "ULL) return rejected;\n";
    if (LastStackByte)
      OS << "    if (state->GPR[4] > UINT64_MAX - " << *LastStackByte
         << "ULL) return rejected;\n";
    auto Arguments = emitX64FunctionCallArguments(
        OS, Call, Layout.EntryStackBase, "entry_sp_offset");
    // The exact entry above is a validated explicit root, so the shared begin
    // cannot reject. Keep return-cell initialization in its existing owner.
    OS << "    neverd_x64_begin_function_v6(state, &call->Function);\n"
          "    call->Entry = 0x"
       << Entry << "ULL;\n"
       << "    return " << Resume;
    if (ScheduledEvents) {
      emitX64ScheduledCallArguments(OS, PagedMemory, "call");
      OS << ";\n}\n";
    } else
      OS << (PagedMemory
                 ? "(state, context, budget, call);\n}\n"
                 : "(state, budget, environment_budget, call, environment, "
                   "environment_context);\n}\n");
    Manifest.push_back(llvm::json::Object{{"entry", "0x" + Entry},
                                          {"abi", "win64"},
                                          {"arguments", std::move(Arguments)},
                                          {"result_bytes", Call.ResultBytes},
                                          {"call", Start},
                                          {"resume", Resume},
                                          {"source_abi_inferred", false}});
  }
  return Manifest;
}

} // namespace neverd
