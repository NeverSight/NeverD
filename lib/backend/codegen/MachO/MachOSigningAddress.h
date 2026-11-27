//===- MachOSigningAddress.h - Darwin signing PC uses -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOSIGNINGADDRESS_H
#define NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOSIGNINGADDRESS_H

#include "neverd/backend/codegen/MachO/MachORegisterResults.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/lift/AArch64/AArch64Regs.h"

#include <optional>

namespace neverd {
class Decoder;

struct MachOAuthenticationReplacement {
  MachOSigningConsumer Consumer;
  uint32_t Encoding;
  va_t Signer;
};

struct MachOSigningAddress {
  enum { None, Local, Observable } Use = None;
  std::optional<MachOAuthenticationReplacement> Authentication;
  /// Retained instruction identity in a previously published image.
  std::optional<uint32_t> InstalledSigner;
};

/// Classify the address materialized at PC. A nonempty sorted Consumers list
/// restricts the proof to authentications belonging to the current MedFunc.
MachOSigningAddress machOSigningAddress(Decoder &Decode,
                                        const MachOLiftedSource &Source,
                                        va_t Start, va_t PC, va_t Target,
                                        aarch64_reg Register,
                                        llvm::ArrayRef<va_t> Consumers = {});

inline std::optional<AArch64PAuthLROperation>
machOPCSigningInstruction(unsigned Id) {
  switch (Id) {
#define MACHO_REGISTER_PC_SIGNING(Name, Operation)                             \
  case AARCH64_INS_##Name:                                                     \
    return aarch64PAuthLROperation(Intrinsic::Operation);
#include "MachORegisterResults.def"
  default:
    return std::nullopt;
  }
}

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOSIGNINGADDRESS_H
