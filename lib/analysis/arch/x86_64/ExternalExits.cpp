//===- ExternalExits.cpp - x64 external exit evidence -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file x64 external exit evidence.

#include "NativeStackControl.h"
#include "X64Recovery.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <map>
#include <tuple>

namespace neverd::analysis::x64 {
std::string externalExitDigest(const SpecializationExternalExit &Exit) {
  llvm::SHA256 Hash;
  Hash.update("neverd-specialization-external-exit-v2");
  const auto Number = [&](uint64_t Value) {
    uint8_t Bytes[8];
    llvm::support::endian::write64le(Bytes, Value);
    Hash.update(llvm::ArrayRef<uint8_t>(Bytes));
  };
  const auto Text = [&](llvm::StringRef Value) {
    Number(Value.size());
    Hash.update(Value);
  };
  const auto Variable = [&](const NdVar &V) {
    Number(static_cast<unsigned>(V.Space));
    Number(V.Offset);
    Number(V.Size);
    Number(static_cast<unsigned>(V.Provenance));
    Number(V.AddressOwnerVA);
  };
  Number(Exit.Version);
  Number(Exit.ResidualAddress);
  Variable(Exit.ExpansionScratch);
  Text(Exit.OriginalOperationDigest);
  Text(Exit.ExpandedOperationDigest);
  Text(Exit.ResidualOperationDigest);
  Number(Exit.MachineExit.has_value());
  if (Exit.MachineExit) {
    Number(Exit.MachineExit->Version);
    Number(Exit.MachineExit->Address);
    Number(static_cast<unsigned>(Exit.MachineExit->Kind));
  }
  const auto &I = Exit.Original;
  const auto &B = I.Origin;
  Number(B.Address);
  Number(B.Size);
  Number(B.FirstOp);
  Number(B.OpCount);
  Number(static_cast<unsigned>(B.Mode));
  Number(static_cast<unsigned>(B.TargetMode));
  Number(static_cast<unsigned>(B.Control));
  Number(static_cast<unsigned>(B.ControlFlags));
  Number(B.Immediate.has_value());
  Number(B.Immediate.value_or(0));
  Number(I.Fallthrough.Address);
  Number(static_cast<unsigned>(I.Fallthrough.Mode));
  Number(I.IsNativeCall);
  Number(static_cast<unsigned>(I.NativeStackControl));
  Number(static_cast<unsigned>(I.ProfileProjection));
  Number(I.NativeBytes.size());
  Hash.update(I.NativeBytes);
  Text(lowUndefinedOperationDigest(I.Ops));
  Text(lowPreservedStateDigest(I.PreservedState));
  Number(static_cast<unsigned>(I.UndefinedEffects.Coverage));
  Number(I.UndefinedEffects.OpCount);
  Text(I.UndefinedEffects.OperationDigest);
  Number(I.UndefinedEffects.Effects.size());
  for (const auto &Effect : I.UndefinedEffects.Effects) {
    Number(Effect.AfterOp);
    Variable(Effect.Output);
    Number(Effect.BitOffset);
    Number(Effect.BitCount);
    Number(Effect.When.has_value());
    if (Effect.When)
      Variable(*Effect.When);
  }
  Number(I.TerminalImport.has_value());
  if (I.TerminalImport) {
    const auto &E = *I.TerminalImport;
    Number(E.Binding.Version);
    Number(E.Binding.Slot);
    Text(E.Binding.Module);
    Text(E.Binding.Symbol);
    Text(E.FixedImageDigest);
  }
  return llvm::toHex(Hash.final());
}

llvm::Error validateExternalExits(const SpecializationResult &Result,
                                  const SpecializationOptions &Options) {
  const auto Invalid = [](const char *Message) {
    return llvm::createStringError(llvm::errc::invalid_argument, "%s", Message);
  };
  if (auto Error = validateLowInstructionBoundaries(
          Result.Residual, LowInstructionBoundaryRequirement::Required))
    return Error;
  if (auto Error = validateX64TerminalImportDomain(Options))
    return Error;
  if (auto Cost = x64::validateRawEnvironmentDeclarations(Options); !Cost)
    return Cost.takeError();
  std::map<va_t, const LowInstructionBoundary *> Origins;
  for (const auto &Origin : Result.Origins)
    if (!Origins.emplace(Origin.ResidualAddress, &Origin.NativeInstruction)
             .second)
      return Invalid("duplicate residual origin");
  std::map<va_t, const SpecializationExternalExit *> Exits;
  for (const auto &Exit : Result.ExternalExits)
    if (Exit.Version != 1 || Exit.IdentityDigest != externalExitDigest(Exit) ||
        !Exits.emplace(Exit.ResidualAddress, &Exit).second)
      return Invalid("duplicate or unsupported external-exit receipt");
  for (const auto &Block : Result.Residual.Blocks) {
    for (const auto &Boundary : Block.InstructionBoundaries) {
      if (Boundary.FirstOp > Block.Ops.size() ||
          Boundary.OpCount > Block.Ops.size() - Boundary.FirstOp)
        return Invalid("external exit has an invalid residual span");
      const auto Span = llvm::ArrayRef<LowOp>(Block.Ops).slice(
          Boundary.FirstOp, Boundary.OpCount);
      const auto Found = Exits.find(Boundary.Address);
      const bool Indirect = llvm::any_of(
          Span, [](const LowOp &Op) { return Op.Opcode == NdOp::INDIR_BR; });
      if (Found == Exits.end()) {
        if (Indirect)
          return Invalid(
              "residual indirect transfer lacks an external-exit receipt");
        continue;
      }
      const auto &Exit = *Found->second;
      const bool TrapExit =
          Exit.MachineExit &&
          Exit.MachineExit->Kind == SpecializationX64MachineExitKind::Trap;
      // A Trap exit ends the region on its retained trap intrinsic and keeps a
      // terminator boundary; every other exit ends on its exact indirect
      // transfer. Both are terminal: last span op, no successor, no immediate.
      const bool ShapeOk =
          !Span.empty() &&
          Boundary.FirstOp + Boundary.OpCount == Block.Ops.size() &&
          Block.Succs.empty() && !Boundary.Immediate &&
          lowUndefinedOperationDigest(Span) == Exit.ResidualOperationDigest &&
          (TrapExit
               ? (Span.back().Opcode == NdOp::INTRINSIC &&
                  Boundary.Control == LowInstructionControl::Terminator &&
                  Boundary.ControlFlags == Exit.Original.Origin.ControlFlags)
               : (Span.back().Opcode == NdOp::INDIR_BR &&
                  Boundary.Control == LowInstructionControl::Branch &&
                  Boundary.ControlFlags ==
                      (LowInstructionControlFlag::Branch |
                       LowInstructionControlFlag::Indirect)));
      if (!ShapeOk)
        return Invalid("external exit differs from its terminal residual span");
      if (!Exit.MachineExit)
        if (auto Error = validateX64TerminalImportCall(Exit.Original))
          return Error;
      const auto Origin = Origins.find(Exit.ResidualAddress);
      const auto BoundaryKey = [](const LowInstructionBoundary &B) {
        return std::tie(B.Address, B.Size, B.FirstOp, B.OpCount, B.Mode,
                        B.Control, B.ControlFlags, B.TargetMode, B.Immediate);
      };
      if (Origin == Origins.end() ||
          BoundaryKey(*Origin->second) != BoundaryKey(Exit.Original.Origin))
        return Invalid("external exit differs from its native origin");
      if (Exit.MachineExit) {
        if (std::count(Options.X64MachineExits.begin(),
                       Options.X64MachineExits.end(), *Exit.MachineExit) != 1)
          return Invalid("machine exit has no unique declared boundary");
      } else {
        const auto &Binding = Exit.Original.TerminalImport->Binding;
        if (std::count(Options.X64TerminalImports.begin(),
                       Options.X64TerminalImports.end(), Binding) != 1)
          return Invalid(
              "external exit has no unique declared terminal domain");
      }
      auto Expanded =
          Exit.MachineExit
              ? expandX64MachineExit(Exit.Original, *Exit.MachineExit,
                                     Exit.ExpansionScratch)
              : expandNativeStackControl(
                    Exit.Original, NdVar::reg(x64::StackPointer, 8),
                    Exit.ExpansionScratch,
                    NativeReturnExpansion::InternalTransfer);
      if (!Expanded)
        return Expanded.takeError();
      if (Expanded->Receipt.OriginalOperationDigest !=
              Exit.OriginalOperationDigest ||
          Expanded->Receipt.ExpandedOperationDigest !=
              Exit.ExpandedOperationDigest)
        return Invalid("external exit has stale physical expansion evidence");
      std::vector<LowOp> NativeSpan(Span.begin(), Span.end());
      for (size_t I = 0; I < NativeSpan.size(); ++I) {
        NativeSpan[I].Addr = Exit.Original.Origin.Address;
        NativeSpan[I].Seq = static_cast<int>(I);
      }
      if (lowUndefinedOperationDigest(NativeSpan) !=
          Exit.ExpandedOperationDigest)
        return Invalid(
            "external exit does not retain its exact physical transfer");
      Exits.erase(Found);
    }
  }
  if (!Exits.empty())
    return Invalid("external-exit receipt has no residual occurrence");
  return llvm::Error::success();
}

} // namespace neverd::analysis::x64
