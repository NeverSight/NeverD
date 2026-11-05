//===- X64FunctionCalls.cpp - Checked source call interfaces -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve original entry state while connecting scalar C call views to it.
//===----------------------------------------------------------------------===//

#include "X64FunctionCalls.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <utility>

namespace neverd {

llvm::json::Array
emitX64FunctionCalls(llvm::raw_ostream &OS,
                     llvm::ArrayRef<X64FunctionCallInterface> Calls) {
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
 * disjoint from guest RAM. Declared stack arguments require accessible RAM. */
typedef struct neverd_x64_call_context_v1 {
    neverd_x64_function_context_v1 Function;
    uint64_t Entry, Value;
} neverd_x64_call_context_v1;
typedef struct neverd_x64_call_result_v1 {
    uint64_t Status, Completed, Value;
} neverd_x64_call_result_v1;
)C";
  for (const auto &Call : Calls) {
    const auto Entry = llvm::utohexstr(Call.Entry);
    const std::string Start = "neverd_x64_call_" + Entry + "_v6";
    const std::string Resume = "neverd_x64_resume_" + Entry + "_v6";
    const auto ResultType =
        "uint" + std::to_string(Call.ResultBytes * 8) + "_t";
    const auto Parameters =
        "(neverd_x64_state_v6 *state, uint64_t *budget, "
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
          "    result.Status = neverd_x64_run_function_v6(state, budget, "
          "environment_budget,\n"
          "        &call->Function, environment, environment_context);\n"
          "    if (call->Function.Phase == NEVERD_X64_FUNCTION_RETURNED) {\n"
          "        call->Value = ("
       << ResultType << ")state->GPR[" << TRI.IntReturnReg / TRI.PointerSize
       << "];\n"
          "        result.Completed = 1; result.Value = call->Value;\n"
          "    }\n"
          "    return result;\n}\n";
    OS << "neverd_x64_call_result_v1 " << Start << Parameters;
    for (size_t P = 0; P < Call.ArgumentBytes.size(); ++P)
      OS << ", uint" << Call.ArgumentBytes[P] * 8 << "_t arg" << P;
    OS << ") {\n"
          "    neverd_x64_call_result_v1 rejected = {"
       << uint64_t(Exit::InvalidState)
       << ", 0, 0};\n"
          "    if (call->Function.Phase != NEVERD_X64_FUNCTION_UNINITIALIZED "
          "||\n"
          "        state->PC != 0x"
       << Entry << "ULL) return rejected;\n";
    if (Call.ArgumentBytes.size() > Layout.Registers.size()) {
      const auto Last =
          Layout.EntryStackBase +
          (Call.ArgumentBytes.size() - Layout.Registers.size() - 1) *
              Layout.SlotBytes +
          Call.ArgumentBytes.back() - 1;
      OS << "    if (state->GPR[4] > UINT64_MAX - " << Last
         << "ULL) return rejected;\n";
    }
    llvm::json::Array Arguments;
    for (size_t P = 0; P < Call.ArgumentBytes.size(); ++P) {
      const auto Type =
          "uint" + std::to_string(Call.ArgumentBytes[P] * 8) + "_t";
      llvm::json::Object Argument{{"bytes", Call.ArgumentBytes[P]}};
      if (P < Layout.Registers.size()) {
        const auto Index = Layout.Registers[P] / TRI.PointerSize;
        OS << "    if ((" << Type << ")state->GPR[" << Index << "] != arg" << P
           << ") return rejected;\n";
        Argument["gpr_index"] = Index;
      } else {
        const auto Offset = Layout.EntryStackBase +
                            (P - Layout.Registers.size()) * Layout.SlotBytes;
        OS << "    " << Type << " stack_arg" << P << ";\n"
           << "    __builtin_memcpy(&stack_arg" << P
           << ", (const void *)(uintptr_t)(state->GPR[4] + " << Offset
           << "ULL), sizeof(stack_arg" << P << "));\n"
           << "    if (stack_arg" << P << " != arg" << P
           << ") return rejected;\n";
        Argument["entry_sp_offset"] = Offset;
      }
      Arguments.push_back(std::move(Argument));
    }
    // The exact entry above is a validated explicit root, so the shared begin
    // cannot reject. Keep return-cell initialization in its existing owner.
    OS << "    neverd_x64_begin_function_v6(state, &call->Function);\n"
          "    call->Entry = 0x"
       << Entry << "ULL;\n"
       << "    return " << Resume
       << "(state, budget, environment_budget, "
          "call, environment, environment_context);\n}\n";
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
