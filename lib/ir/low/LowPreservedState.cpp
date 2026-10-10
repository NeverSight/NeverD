//===- LowPreservedState.cpp - Exact native fact identities ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/LowPreservedState.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"

namespace neverd {

bool matchesLowPreservedState(const LowInstructionPreservedState &F,
                              const LowInstructionBoundary &B,
                              llvm::ArrayRef<uint8_t> Bytes,
                              llvm::ArrayRef<LowOp> Ops) {
  if ((F.Audit != LowPreservedStateAudit::LegacyIntegerV1 &&
       F.Audit != LowPreservedStateAudit::CetDisabledReadShadowStackV1) ||
      F.StateSet != LowPreservedStateSet::LegacyIntegerOpaqueV1 ||
      F.SemanticsVersion != 1 || F.Architecture != Arch::X64 ||
      F.Mode != InstructionMode::Default || B.Mode != F.Mode || B.FirstOp ||
      !B.Size || B.Size > 15 || B.Address == InvalidVA ||
      B.Size > InvalidVA - B.Address || F.Address != B.Address ||
      F.Size != B.Size || Bytes.size() != B.Size || B.OpCount != Ops.size() ||
      F.OpCount != Ops.size() || Ops.empty() ||
      F.NativeBytesDigest.size() != 64 || F.OperationDigest.size() != 64)
    return false;
  return F.NativeBytesDigest == llvm::toHex(llvm::SHA256::hash(Bytes), true) &&
         F.OperationDigest == lowUndefinedOperationDigest(Ops);
}

std::string lowPreservedStateDigest(const LowInstructionPreservedState &F) {
  llvm::SHA256 Hash;
  Hash.update("neverd-native-preserved-state-fact-v1");
  const auto Number = [&](uint64_t N) {
    uint8_t Bytes[8];
    for (unsigned I = 0; I != 8; ++I)
      Bytes[I] = static_cast<uint8_t>(N >> (8 * I));
    Hash.update(Bytes);
  };
  Number(static_cast<unsigned>(F.Audit));
  Number(static_cast<unsigned>(F.StateSet));
  Number(F.SemanticsVersion);
  Number(static_cast<unsigned>(F.Architecture));
  Number(static_cast<unsigned>(F.Mode));
  Number(F.Address);
  Number(F.Size);
  Number(F.OpCount);
  Number(F.NativeBytesDigest.size());
  Hash.update(F.NativeBytesDigest);
  Number(F.OperationDigest.size());
  Hash.update(F.OperationDigest);
  return llvm::toHex(Hash.final(), true);
}

} // namespace neverd
