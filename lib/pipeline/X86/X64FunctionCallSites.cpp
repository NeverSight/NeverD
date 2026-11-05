//===- X64FunctionCallSites.cpp - Original CALL context bridges -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Let the original CALL own stack effects before entering a native context.
//===----------------------------------------------------------------------===//

#include "X64FunctionCallSites.h"

#include "X64FunctionCalls.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace neverd {

llvm::Expected<std::vector<X64FunctionCallSite>> bindX64FunctionCallSites(
    const BinaryImage &Image, llvm::ArrayRef<X64FunctionCallInterface> Calls,
    llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units) {
  std::vector<X64FunctionCallSite> Sites;
  llvm::DenseMap<va_t, size_t> Requested;
  for (const auto &Call : Calls)
    for (va_t Site : Call.CallSites) {
      Requested.try_emplace(Site, Sites.size());
      Sites.push_back({&Call, nullptr});
    }
  if (Sites.empty())
    return Sites;
  // Index only requested sites; selected instructions are inspected once.
  for (const auto &Unit : Units)
    if (const auto At = Requested.find(Unit.Address); At != Requested.end())
      Sites[At->second].Instruction = &Unit;
  Decoder Decode;
  if (!Decode.init(Image))
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "function call-site decoder failed");
  for (const auto &Site : Sites) {
    if (!Site.Instruction)
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "function call site is not selected");
    const auto &Unit = *Site.Instruction;
    DecodedInsn Instruction;
    if (Unit.RequiresNativeExecution ||
        !Decode.selectMode(Image, Unit.Address) ||
        !Decode.decodeOneForLift(Unit.Bytes.data(), Unit.Bytes.size(),
                                 Unit.Address, Instruction) ||
        Decode.directCallTarget(Instruction) != Site.Interface->Entry)
      return llvm::createStringError(
          llvm::errc::invalid_argument,
          "function call site 0x" + llvm::utohexstr(Unit.Address) +
              " must be a modeled original direct CALL to its declared entry");
  }
  return Sites;
}

llvm::json::Array
emitX64FunctionCallSites(llvm::raw_ostream &OS,
                         llvm::ArrayRef<X64FunctionCallSite> Sites) {
  llvm::json::Array Manifest;
  if (Sites.empty())
    return Manifest;
  using Exit = analysis::X64ContinuationExitV1;
  const auto Layout =
      getTargetRegInfo(Arch::X64).integerArgumentLayout(BinaryFormat::COFF);
  OS << R"C(
/* Original CALL bridges. Supply the full pre-CALL native state, including
 * original stack bytes. Checks never initialize arguments or padding.
 * Zero-initialize this context and retain it across pauses. Prepared with an
 * uninitialized Callee.Function means CALL has not yet completed; ReturnSlot
 * then holds the checked pre-CALL SP. No provider runs before that CALL.
 * The selected CALL creates its real return cell; the shared function context
 * owns completion. Context, state, budgets and provider storage are disjoint
 * from each other and guest RAM. Never infer caller state from C parameters. */
typedef struct neverd_x64_call_site_context_v1 {
    neverd_x64_call_context_v1 Callee;
    uint64_t Site, Prepared;
} neverd_x64_call_site_context_v1;
)C";
  for (const auto &Site : Sites) {
    const auto &Call = *Site.Interface;
    const auto &Unit = *Site.Instruction;
    const auto Address = llvm::utohexstr(Unit.Address);
    const auto Entry = llvm::utohexstr(Call.Entry);
    const std::string Start = "neverd_x64_call_site_" + Address + "_v6";
    const std::string Resume = "neverd_x64_resume_site_" + Address + "_v6";
    const char *Parameters =
        "(neverd_x64_state_v6 *state, uint64_t *budget, "
        "uint64_t *environment_budget,\n"
        "    neverd_x64_call_site_context_v1 *site, neverd_x64_environment_v6 "
        "environment, void *environment_context";
    OS << "\nneverd_x64_call_result_v1 " << Resume << Parameters << ") {\n"
       << "    neverd_x64_call_result_v1 rejected = {"
       << uint64_t(Exit::InvalidState) << ", 0, 0};\n"
       << "    if (site->Prepared != 1 || site->Site != 0x" << Address
       << "ULL) return rejected;\n"
          "    if (site->Callee.Function.Phase == "
          "NEVERD_X64_FUNCTION_UNINITIALIZED) {\n"
          "        if (state->PC != 0x"
       << Address
       << "ULL || state->GPR[4] != "
          "site->Callee.Function.ReturnSlot) return rejected;\n"
          "        if (!*budget) { rejected.Status = 9; return rejected; }\n"
          "        rejected.Status = "
       << Unit.Function.Name << "((uint8_t *)state);\n"
       << "        if (rejected.Status != " << uint64_t(Exit::Call)
       << ") return rejected;\n"
          "        --*budget;\n"
          "        neverd_x64_begin_function_v6(state, "
          "&site->Callee.Function);\n"
          "    }\n"
       << "    return neverd_x64_resume_" << Entry
       << "_v6(state, budget, environment_budget, &site->Callee,\n"
          "        environment, environment_context);\n}\n";
    OS << "neverd_x64_call_result_v1 " << Start << Parameters;
    const auto LastStackByte =
        emitX64FunctionCallParameters(OS, Call, Layout.CallStackBase);
    OS << ") {\n    neverd_x64_call_result_v1 rejected = {"
       << uint64_t(Exit::InvalidState)
       << ", 0, 0};\n"
          "    if (site->Prepared || site->Callee.Function.Phase != "
          "NEVERD_X64_FUNCTION_UNINITIALIZED ||\n"
          "        state->PC != 0x"
       << Address << "ULL) return rejected;\n";
    if (LastStackByte)
      OS << "    if (state->GPR[4] > UINT64_MAX - " << *LastStackByte
         << "ULL) return rejected;\n";
    auto Arguments = emitX64FunctionCallArguments(
        OS, Call, Layout.CallStackBase, "call_sp_offset");
    OS << "    site->Callee.Function.ReturnSlot = state->GPR[4];\n"
       << "    site->Callee.Entry = 0x" << Entry << "ULL;\n"
       << "    site->Site = 0x" << Address << "ULL; site->Prepared = 1;\n"
       << "    return " << Resume
       << "(state, budget, environment_budget, site,\n"
          "        environment, environment_context);\n}\n";
    Manifest.push_back(llvm::json::Object{
        {"instruction", "0x" + Address},
        {"entry", "0x" + Entry},
        {"return_pc", "0x" + llvm::utohexstr(Unit.Address + Unit.Bytes.size())},
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
