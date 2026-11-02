//===- RegistrationLocalUnwind.cpp - Authenticated local unwind imports --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Identify the exact SEH3/SEH4 runtime without granting argument or frame
/// facts.

#include "RegistrationABIPrivate.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/RuntimeProviders.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringSwitch.h"

#include <set>

namespace neverd {

std::optional<RegistrationLocalUnwindContract>
getCheckedX86LocalUnwindContract(const BinaryImage &Image, va_t Target,
                                 bool Indirect) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Target > UINT32_MAX)
    return std::nullopt;
  va_t Slot = Target;
  if (!Indirect) {
    const auto Bytes =
        readImmutablePE32CodeBytes(Image, Target, 6, {Target + 2});
    if (!Bytes || (*Bytes)[0] != 0xff || (*Bytes)[1] != 0x25)
      return std::nullopt;
    Slot = readLE<uint32_t>(Bytes->data() + 2);
  }
  const auto *Import = registration_abi::checkedImportSlot(Image, Slot);
  if (!Import)
    return std::nullopt;
  using Kind = RegistrationLocalUnwindContract::Kind;
  const auto Routine = llvm::StringSwitch<std::optional<Kind>>(Import->Name)
#define NEVERD_REGISTRATION_LOCAL_UNWIND(Name, Routine)                        \
  .Case(Name, Kind::Routine)
#include "RegistrationRuntimeImports.def"
                           .Default(std::nullopt);
  if (!Routine ||
      !coff_loader::isX86RegistrationRuntimeProvider(Import->Module))
    return std::nullopt;
  return RegistrationLocalUnwindContract{Target, Indirect, *Routine, Slot};
}

std::optional<std::vector<RegistrationLocalUnwindContract>>
RegistrationCallCalleeIndex::localUnwindContracts(const LowFunc &Function) {
  std::vector<RegistrationLocalUnwindContract> Result;
  std::set<std::pair<va_t, bool>> Seen;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (!registration_abi::chargeCalleeWork(Work, 1))
        return std::nullopt;
      if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
          Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
          Op.Inputs[0].Size != 4)
        continue;
      const auto Identity =
          std::make_pair(Op.Inputs[0].Offset, Op.Opcode == NdOp::INDIR_CALL);
      if (!Seen.insert(Identity).second)
        continue;
      if (Seen.size() >
          ::neverd::limits::ir::
              kRegistrationLocalUnwindRegistrationCallCalleeIndexLocalUnwindContractsSeenSizeLimit)
        return std::nullopt;
      if (const auto Contract = getCheckedX86LocalUnwindContract(
              Image, Identity.first, Identity.second))
        Result.push_back(*Contract);
    }
  return Result;
}

} // namespace neverd
