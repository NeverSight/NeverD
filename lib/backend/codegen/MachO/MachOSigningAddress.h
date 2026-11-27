//===- MachOSigningAddress.h - Darwin signing PC uses -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Classify signing-PC references and describe their authentication consumers.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOSIGNINGADDRESS_H
#define NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOSIGNINGADDRESS_H

#include "neverd/Common.h"
#include "neverd/backend/codegen/MachO/MachORegisterResults.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"

#include "capstone/aarch64.h"
#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>

namespace neverd {
class Decoder;
struct MachOLiftedSource;

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
  /// A following published address pair restores the authenticated register.
  std::optional<va_t> RestorePC;
};

/// Classify the address materialized at PC. A nonempty sorted Consumers list
/// restricts the proof to authentications belonging to the current MedFunc.
MachOSigningAddress machOSigningAddress(Decoder &Decode,
                                        const MachOLiftedSource &Source,
                                        va_t Start, va_t PC, va_t Target,
                                        aarch64_reg Register,
                                        llvm::ArrayRef<va_t> Consumers = {});

llvm::ArrayRef<aarch64_reg> machOSigningScratchRegisters();
bool machODeadSigningScratch(Decoder &Decode, const MachOLiftedSource &Source,
                             va_t Start, va_t At, aarch64_reg Reg);
std::optional<uint32_t> machOPCRelativeAuthentication(Intrinsic Op, va_t PC,
                                                      va_t Sign);

enum class MachOAuthenticationForm { None, Register, PCRelative };

/// The second modifier of an explicit AUT or authenticated return.
inline MachOAuthenticationForm machOPCAuthenticationForm(uint32_t Bits) {
  enum class Field { PC, X15, PCRel, Rn, Rm };
  const auto Form = [Bits](Field F) {
    if (F == Field::PCRel)
      return MachOAuthenticationForm::PCRelative;
    if (F == Field::Rn || (F == Field::Rm && (Bits & kAArch64PAuthLRRegMask) !=
                                                 kAArch64PAuthLRRegMask))
      return MachOAuthenticationForm::Register;
    return MachOAuthenticationForm::None;
  };
#define NEVERD_A64_PAUTH_LR_RETURN(Mask, Encoding, Id, Mod2)                   \
  if ((Bits & Mask) == Encoding)                                               \
    return Form(Field::Mod2);
#define NEVERD_A64_PAUTH_LR_INSTRUCTION(Mask, Encoding, Id, Pointer, Mod,      \
                                        Mod2)                                  \
  NEVERD_A64_PAUTH_LR_RETURN(Mask, Encoding, Id, Mod2)
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthLR.def"
  return MachOAuthenticationForm::None;
}

/// An ordinary reference to a proved signer can escape independently of the
/// reaching definition used by AUT. Follow its aliases through their uses.
bool machOObservesSigningAddress(Decoder &Decode,
                                 const MachOLiftedSource &Source, va_t Start,
                                 va_t After, aarch64_reg Reg);

/// Check both register operands and the address of a published restore pair.
bool machORestoresSigningAddress(Decoder &Decode, llvm::ArrayRef<uint8_t> Bytes,
                                 va_t PC, unsigned Register, va_t Target);

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
