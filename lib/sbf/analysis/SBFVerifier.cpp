//===- SBFVerifier.cpp - Layered official SBF verification ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/analysis/SBFVerifier.h"

#include "neverd/sbf/image/SBFRelocations.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"

#include <algorithm>

namespace neverd::sbf {
namespace {

llvm::Error verifierError(llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      (llvm::Twine("sbf: verifier: ") + Message).str(),
      llvm::inconvertibleErrorCode());
}

VerificationIssue makeIssue(VerifierRule Rule, const LowInstruction &Call,
                            uint64_t Payload) {
  const VerifierRuleInfo Info = getVerifierRuleInfo(Rule);
  std::string Message = Info.Message.str();
  switch (Info.PayloadKind) {
  case VerifierPayloadKind::SyscallHash:
    Message += (llvm::Twine(" (0x") + llvm::utohexstr(Payload) + ")").str();
    break;
  case VerifierPayloadKind::InstructionSlot:
    Message += (llvm::Twine(" (slot ") + llvm::Twine(Payload) + ")").str();
    break;
  }
  return {Rule, Call.Slot, Call.Address, Payload, std::move(Message)};
}

} // namespace

llvm::Expected<VerificationReport>
verifyLocalPreflight(const LowIR &IR,
                     llvm::ArrayRef<uint32_t> RegisteredSyscallHashes) {
  if (std::any_of(IR.Instructions.begin(), IR.Instructions.end(),
                  [](const LowInstruction &Instruction) {
                    return Instruction.isInvalid();
                  }))
    return verifierError(
        "local-preflight requires requisite-verified instructions");

  VerificationReport Report;
  Report.State = VerificationState::Accepted;
  if (!versionHasFeature(IR.TheVersion, VersionFeature::StaticSyscalls))
    return Report;

  llvm::SmallVector<uint32_t> RegisteredSyscalls(
      RegisteredSyscallHashes.begin(), RegisteredSyscallHashes.end());
  std::sort(RegisteredSyscalls.begin(), RegisteredSyscalls.end());
  RegisteredSyscalls.erase(
      std::unique(RegisteredSyscalls.begin(), RegisteredSyscalls.end()),
      RegisteredSyscalls.end());

  for (const LowInstruction &Instruction : IR.Instructions) {
    if (Instruction.IsContinuation || !Instruction.Info ||
        Instruction.Info->ID != Opcode::CALL_IMM)
      continue;
    if (Instruction.Src == 0) {
      const uint32_t Hash = static_cast<uint32_t>(Instruction.RawImmediate);
      if (!std::binary_search(RegisteredSyscalls.begin(),
                              RegisteredSyscalls.end(), Hash))
        Report.Issues.push_back(
            makeIssue(VerifierRule::InvalidSyscall, Instruction, Hash));
    } else if (Instruction.Src == 1 &&
               Instruction.RawImmediate == kLegacyUnresolvedCallImmediate) {
      Report.Issues.push_back(makeIssue(VerifierRule::InvalidFunction,
                                        Instruction, Instruction.Slot));
    }
    if (!Report.Issues.empty()) {
      Report.State = VerificationState::Rejected;
      return Report;
    }
  }
  return Report;
}

} // namespace neverd::sbf
