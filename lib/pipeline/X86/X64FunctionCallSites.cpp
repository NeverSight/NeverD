//===- X64FunctionCallSites.cpp - Original CALL context bridges -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Let the original CALL own stack effects before entering a native context.
//===----------------------------------------------------------------------===//

#include "X64FunctionCallSites.h"

#include "X64FunctionCallHandlers.h"
#include "X64FunctionCallSchedule.h"
#include "X64FunctionCalls.h"
#include "X64FunctionContext.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/analysis/arch/x86_64/X64PagedSource.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace neverd {

X64FunctionCallSitePauses::X64FunctionCallSitePauses(
    llvm::ArrayRef<X64FunctionCallSite> Sites) {
  Addresses.reserve(Sites.size());
  for (const auto &Site : Sites)
    Addresses.insert(Site.Instruction->Address);
}

void X64FunctionCallSitePauses::declare(llvm::raw_ostream &OS) const {
  if (!Addresses.empty())
    OS << "\n/* A bound original CALL has not executed. Retain the caller "
          "activation\n"
          " * and use its call-site bridge with this unchanged native state. "
          "*/\n"
          "enum { NEVERD_X64_CALL_SITE_READY_V1 = "
       << uint64_t(analysis::X64ContinuationExitV1::CallSiteReady) << " };\n";
}

bool X64FunctionCallSitePauses::emit(llvm::raw_ostream &OS, va_t Address,
                                     llvm::StringRef Indent) const {
  if (!Addresses.contains(Address))
    return false;
  OS << Indent << "return NEVERD_X64_CALL_SITE_READY_V1;\n";
  return true;
}

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

llvm::json::Array emitX64FunctionCallSites(
    llvm::raw_ostream &OS, llvm::ArrayRef<X64FunctionCallSite> Sites,
    bool PagedMemory, bool ScheduledEvents, bool PauseCallSites, bool Debug) {
  llvm::json::Array Manifest;
  if (Sites.empty())
    return Manifest;
  using Exit = analysis::X64ContinuationExitV1;
  const auto Names = x64FunctionContextNames(Debug);
  const auto Layout =
      getTargetRegInfo(Arch::X64).integerArgumentLayout(BinaryFormat::COFF);
  if (PagedMemory)
    OS << R"C(
/* Paged CALL: retain the same execution binding and its fault/RAM effects.
 * An attempted fault spends one step but leaves CALL pending. The producer
 * handles the boundary and restores the original CALL PC/SP before retrying;
 * this bridge neither runs the handler nor rolls back fault-entry state.
 * Only attempted CALL success starts the callee. Entry checks read no stack. */
)C";
  OS << R"C(
/* Original CALL bridges. Supply the full pre-CALL native state, including
 * original stack bytes. Checks never initialize arguments or padding.
 * Zero-initialize this context and retain it across pauses. Prepared with an
 * uninitialized Callee.Function means CALL has not yet completed; ReturnSlot
 * then holds the checked pre-CALL SP.)C";
  OS << (ScheduledEvents ? " The scheduler polls before that CALL.\n"
                         : " No provider runs before that CALL.\n");
  OS << R"C( * The selected CALL creates its real return cell; the shared function context
 * owns completion. Context, state, budgets and provider storage are disjoint
 * from each other and guest RAM. Never infer caller state from C parameters. */
)C";
  OS << "typedef struct " << Names.SiteContext << " {\n    "
     << Names.CallContext << " Callee;\n    uint64_t Site, Prepared;\n} "
     << Names.SiteContext << ";\n";
  if (PauseCallSites)
    emitX64FunctionCallHandlerContext(OS, Debug);
  for (const auto &Site : Sites) {
    const auto &Call = *Site.Interface;
    const auto &Unit = *Site.Instruction;
    const auto Address = llvm::utohexstr(Unit.Address);
    const auto Entry = llvm::utohexstr(Call.Entry);
    const std::string Prefix = (llvm::Twine(x64FunctionCallPrefix(
                                    PagedMemory, ScheduledEvents, Debug)) +
                                "site_")
                                   .str();
    const std::string Start =
        (llvm::Twine("neverd_x64_call_") + Prefix + Address + "_v6").str();
    const std::string Resume =
        (llvm::Twine("neverd_x64_resume_") + Prefix + Address + "_v6").str();
    const std::string Parameters =
        ScheduledEvents ? x64ScheduledCallParameters(PagedMemory, true, Debug)
        : PagedMemory
            ? (llvm::Twine(
                   "(neverd_x64_state_v6 *state, uint8_t *context, uint64_t "
                   "*budget,\n    ") +
               Names.SiteContext + " *site")
                  .str()
            : "(neverd_x64_state_v6 *state, uint64_t *budget, "
              "uint64_t *environment_budget,\n"
              "    neverd_x64_call_site_context_v1 *site, "
              "neverd_x64_environment_v6 "
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
          "        if (!*budget) { rejected.Status = 9; return rejected; }\n";
    if (ScheduledEvents && Debug)
      OS << "        const uint64_t call_budget = *budget;\n";
    OS << "        rejected.Status = ";
    if (ScheduledEvents) {
      OS << x64ScheduledCallSiteStepName(Unit.Address);
      emitX64ScheduledCallArguments(OS, PagedMemory, "");
      OS << ";\n";
      if (Debug)
        // The scheduler may reject its own arguments before invoking any
        // runtime bridge. An unchanged budget cannot own an earlier receipt.
        OS << "        if (*budget == call_budget) return rejected;\n";
    } else {
      OS << Unit.Function.Name;
      if (PagedMemory)
        OS << "_paged_step(state, context);\n"
              "        if (!(neverd_x64_paged_progress_v1(context) & "
           << analysis::kX64PagedInstructionAttempted
           << ")) return rejected;\n"
              "        --*budget;\n";
      else
        OS << "((uint8_t *)state);\n";
    }
    if (Debug)
      OS << "        uint64_t instruction = 0, pc = 0, sp = 0;\n"
         << "        if (neverd_x64_paged_transfer_v1("
         << (ScheduledEvents ? "memory_context" : "context")
         << ", &instruction, &pc, &sp, 0) != " << uint64_t(Exit::Call)
         << " || instruction != 0x" << Address << "ULL) return rejected;\n";
    else
      OS << "        if (rejected.Status != " << uint64_t(Exit::Call)
         << ") return rejected;\n";
    if (ScheduledEvents && PagedMemory)
      OS << "        if (!(neverd_x64_paged_progress_v1(memory_context) & "
         << analysis::kX64PagedInstructionAttempted << ")) return rejected;\n";
    if (!PagedMemory && !ScheduledEvents)
      OS << "        --*budget;\n";
    if (Debug)
      OS << "        if (pc != 0x" << Entry << "ULL || !" << Names.BeginAt
         << "(pc, sp, &site->Callee.Function)) {\n"
         << "            rejected.Status = " << uint64_t(Exit::InvalidState)
         << "; return rejected;\n        }\n"
         << "        if (rejected.Status != " << uint64_t(Exit::Call)
         << ") return rejected; /* CALL completed before its debug event. */\n";
    else
      OS << "        " << Names.Begin << "(state, &site->Callee.Function);\n";
    OS << "    }\n"
       << "    return neverd_x64_resume_"
       << x64FunctionCallPrefix(PagedMemory, ScheduledEvents, Debug) << Entry;
    if (ScheduledEvents) {
      OS << "_v6";
      emitX64ScheduledCallArguments(OS, PagedMemory, "&site->Callee");
      OS << ";\n}\n";
    } else
      OS << (PagedMemory
                 ? "_v6(state, context, budget, &site->Callee);\n}\n"
                 : "_v6(state, budget, environment_budget, &site->Callee,\n"
                   "        environment, environment_context);\n}\n");
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
       << "    return " << Resume;
    if (ScheduledEvents) {
      emitX64ScheduledCallArguments(OS, PagedMemory, "site");
      OS << ";\n}\n";
    } else
      OS << (PagedMemory ? "(state, context, budget, site);\n}\n"
                         : "(state, budget, environment_budget, site,\n"
                           "        environment, environment_context);\n}\n");
    llvm::json::Object Interface{
        {"instruction", "0x" + Address},
        {"entry", "0x" + Entry},
        {"return_pc", "0x" + llvm::utohexstr(Unit.Address + Unit.Bytes.size())},
        {"abi", "win64"},
        {"arguments", std::move(Arguments)},
        {"result_bytes", Call.ResultBytes},
        {"call", Start},
        {"resume", Resume},
        {"source_abi_inferred", false}};
    if (PauseCallSites) {
      // Without these pauses a near RET/fallthrough could execute the pending
      // CALL inside the region driver before a privilege return was observed.
      const auto Handler =
          (llvm::Twine("neverd_x64_resume_handler_") + Prefix + Address + "_v6")
              .str();
      emitX64FunctionCallHandlerResume(OS, Unit.Address, Handler, Resume,
                                       Parameters, PagedMemory,
                                       ScheduledEvents);
      Interface["resume_handler"] = Handler;
    }
    Manifest.push_back(std::move(Interface));
  }
  return Manifest;
}

} // namespace neverd
