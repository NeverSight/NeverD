//===- X64ContinuationEnvironment.cpp - State boundary handoff -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compose an explicit environment with the existing physical-state driver.
//===----------------------------------------------------------------------===//

#include "X64ContinuationEnvironment.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"

#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <string>

namespace neverd {

void emitX64ContinuationEnvironment(llvm::raw_ostream &OS, unsigned Version) {
  using Exit = analysis::X64ContinuationExitV1;
  const auto Suffix = "_v" + std::to_string(Version);
  const auto State = "neverd_x64_state" + Suffix;
  const auto Provider = "neverd_x64_environment" + Suffix;
  OS << "\n/* Explicit provider: return nonzero to resume from the updated "
        "state.\n"
        " * No call arguments, return PC or preserved registers are "
        "synthesized.\n"
        " * Each provider invocation consumes one environment budget unit, "
        "even\n"
        " * when it declines or leaves PC unchanged. Its execution is not "
        "counted\n"
        " * by the translated-instruction budget. */\n"
     << "typedef int (*" << Provider << ")(" << State
     << " *state, uint64_t boundary, void *context);\n"
     << "uint64_t neverd_x64_run" << Suffix << "(" << State
     << " *state, uint64_t *budget, uint64_t *environment_budget,\n    "
     << Provider << " environment, void *context) {\n"
     << "    for (;;) {\n"
     << "        uint64_t status = neverd_x64_continue" << Suffix
     << "(state, budget);\n"
     << "        switch (status) {\n";
  for (const auto Boundary :
       {Exit::Call, Exit::Jump, Exit::Return, Exit::NativeInstruction,
        Exit::PrivilegeReturn, Exit::Exception, Exit::PrivilegeEnter,
        Exit::UserInterruptReturn, Exit::FredReturn, Exit::SvmExit,
        Exit::BoundsFault})
    OS << "        case " << uint64_t(Boundary) << ":\n";
  // Status 8 belongs to the public driver: PC is outside the selection. Like
  // an external jump, only the explicitly supplied environment can handle it.
  OS << "        case 8:\n"
        "            break;\n"
        "        default:\n"
        "            return status;\n"
        "        }\n"
        "        if (!environment || !*budget || !*environment_budget)\n"
        "            return status;\n"
        "        --*environment_budget;\n"
        "        if (!environment(state, status, context))\n"
        "            return status;\n"
        "    }\n"
        "}\n";
}

} // namespace neverd
