//===- AArch64ReturnAddressState.cpp - Static RA signing state -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Interpret only the AArch64 return-address signing column of normalized CFI.
//===----------------------------------------------------------------------===//

#include "neverd/loader/DWARF/AArch64ReturnAddressState.h"

#include <limits>
#include <vector>

namespace neverd::dwarf_eh {
namespace {

bool isRARegisterRule(const CFIInstruction &I) {
  if (I.Register != aarch64::kRAStateRegister)
    return false;
  switch (I.Kind) {
  case CFIOpKind::Offset:
  case CFIOpKind::ValOffset:
  case CFIOpKind::Register:
  case CFIOpKind::Expression:
  case CFIOpKind::ValExpression:
  case CFIOpKind::Restore:
  case CFIOpKind::Undefined:
  case CFIOpKind::SameValue:
    return true;
  default:
    return false;
  }
}

std::optional<va_t> signingPC(va_t Location, int64_t Offset) {
  if (Offset >= 0) {
    if (uint64_t(Offset) > std::numeric_limits<va_t>::max() - Location)
      return std::nullopt;
    return Location + uint64_t(Offset);
  }
  const uint64_t Magnitude = uint64_t(0) - uint64_t(Offset);
  if (Magnitude > Location)
    return std::nullopt;
  return Location - Magnitude;
}

} // namespace

AArch64RAQueryResult
evaluateAArch64ReturnAddressState(Arch Architecture, const DwarfCIE &CIE,
                                  const DwarfFDE &FDE, va_t PC,
                                  const AArch64RAQueryLimits &Limits) {
  using namespace aarch64;
  using Status = AArch64RAQueryStatus;
  const auto fail = [](Status S, const char *Message) {
    return AArch64RAQueryResult{S, std::nullopt, Message};
  };
  if (Architecture != Arch::AArch64)
    return fail(Status::Unsupported, kUnsupportedArchitecture);
  if (!CIE.CodeAlignmentFactor || CIE.SectionOffset != FDE.CIESectionOffset ||
      !FDE.AddressRange || FDE.InitialLocation > UINT64_MAX - FDE.AddressRange)
    return fail(Status::Malformed, kInvalidIdentity);
  if (PC < FDE.InitialLocation || PC - FDE.InitialLocation >= FDE.AddressRange)
    return fail(Status::OutOfRange, kOutOfRange);
  if (CIE.ParseStatus != ExceptionParseStatus::Complete ||
      FDE.ParseStatus != ExceptionParseStatus::Complete)
    return fail(CIE.ParseStatus == ExceptionParseStatus::Malformed ||
                        FDE.ParseStatus == ExceptionParseStatus::Malformed
                    ? Status::Malformed
                    : Status::Unsupported,
                kIncomplete);
  if (CIE.InitialInstructions.size() > Limits.MaxInstructions ||
      FDE.Instructions.size() >
          Limits.MaxInstructions - CIE.InitialInstructions.size())
    return fail(Status::ResourceLimit, kBudget);

  bool HasToggle = false, HasRegisterRule = false;
  for (const auto *Program : {&CIE.InitialInstructions, &FDE.Instructions})
    for (const auto &I : *Program) {
      HasToggle |= I.Kind == CFIOpKind::NegateRAState ||
                   I.Kind == CFIOpKind::NegateRAStateWithPC;
      HasRegisterRule |= isRARegisterRule(I);
    }
  if (HasToggle && HasRegisterRule)
    return fail(Status::Malformed, kMixedRules);

  AArch64ReturnAddressState Empty;
  Empty.Key = CIE.HasPointerAuth || CIE.UsesArm64eReturnAddressABI
                  ? AArch64RAKey::B
                  : AArch64RAKey::A;
  Empty.Recipe = CIE.UsesNonHintPAuthLR ? AArch64RARecipe::NonHintPAuthLR
                                        : AArch64RARecipe::HintPACM;
  std::optional<AArch64ReturnAddressState> State = Empty;
  std::optional<AArch64ReturnAddressState> Initial = Empty;
  std::vector<std::optional<AArch64ReturnAddressState>> Remembered;
  std::optional<AArch64ReturnAddressState> Answer;
  bool Captured = false;
  AArch64RAQueryResult Error;
  const uint64_t QueryOffset = PC - FDE.InitialLocation;

  const auto run = [&](const std::vector<CFIInstruction> &Program,
                       bool IsInitial) {
    uint64_t Location = 0;
    for (const auto &I : Program) {
      const auto reject = [&](Status S, const char *Message) {
        Error = fail(S, Message);
        return false;
      };
      if (I.CodeOffset != Location || Location > FDE.AddressRange)
        return reject(Status::Malformed, kLocation);
      if (I.Kind == CFIOpKind::AdvanceLoc || I.Kind == CFIOpKind::SetLoc) {
        if (IsInitial)
          return reject(Status::Unsupported, kInitialLocation);
        uint64_t Next = 0;
        if (I.Kind == CFIOpKind::SetLoc) {
          const va_t Target = static_cast<va_t>(I.Offset);
          if (Target < FDE.InitialLocation)
            return reject(Status::Malformed, kLocation);
          Next = Target - FDE.InitialLocation;
        } else {
          if (I.Offset < 0 || uint64_t(I.Offset) > (UINT64_MAX - Location) /
                                                       CIE.CodeAlignmentFactor)
            return reject(Status::Malformed, kLocation);
          Next = Location + uint64_t(I.Offset) * CIE.CodeAlignmentFactor;
        }
        if (Next < Location || Next > FDE.AddressRange ||
            (I.Kind == CFIOpKind::SetLoc && Next == Location))
          return reject(Status::Malformed, kLocation);
        if (!Captured && QueryOffset < Next) {
          Answer = State;
          Captured = true;
        }
        Location = Next;
        continue;
      }
      switch (I.Kind) {
      case CFIOpKind::RememberState:
        if (Remembered.size() >= Limits.MaxRememberDepth)
          return reject(Status::ResourceLimit, kBudget);
        Remembered.push_back(State);
        break;
      case CFIOpKind::RestoreState:
        if (Remembered.empty())
          return reject(Status::Malformed, kRestore);
        State = Remembered.back();
        Remembered.pop_back();
        break;
      case CFIOpKind::SetRAState:
      case CFIOpKind::NegateRAState:
      case CFIOpKind::NegateRAStateWithPC: {
        uint64_t Value = I.RAState;
        if (I.Kind != CFIOpKind::SetRAState) {
          if (!State)
            break;
          const auto SignedKind = I.Kind == CFIOpKind::NegateRAState
                                      ? AArch64RASigningKind::SP
                                      : AArch64RASigningKind::SPAndPC;
          if (State->Kind == AArch64RASigningKind::Unsigned)
            Value = SignedKind == AArch64RASigningKind::SP ? kSignedSP
                                                           : kSignedSPPC;
          else if (State->Kind == SignedKind)
            Value = kUnsigned;
          else
            return reject(Status::Malformed, kToggleState);
        }
        if (Value > kSignedSPPC)
          return reject(Status::Unsupported, kReservedState);
        State = Empty;
        if (Value == kSignedSP)
          State->Kind = AArch64RASigningKind::SP;
        if (Value == kSignedSPPC) {
          State->Kind = AArch64RASigningKind::SPAndPC;
          State->SigningPC =
              signingPC(FDE.InitialLocation + Location,
                        I.Kind == CFIOpKind::SetRAState ? I.Offset : 0);
          if (!State->SigningPC)
            return reject(Status::Malformed, kSigningPCOverflow);
        }
        break;
      }
      case CFIOpKind::Opaque:
        return reject(Status::Unsupported, kOpaque);
      case CFIOpKind::Offset:
      case CFIOpKind::ValOffset:
      case CFIOpKind::Register:
      case CFIOpKind::Expression:
      case CFIOpKind::ValExpression:
      case CFIOpKind::Restore:
      case CFIOpKind::Undefined:
      case CFIOpKind::SameValue:
        if (isRARegisterRule(I))
          State = I.Kind == CFIOpKind::Restore ? Initial : std::nullopt;
        break;
      case CFIOpKind::Nop:
      case CFIOpKind::DefCFA:
      case CFIOpKind::DefCFARegister:
      case CFIOpKind::DefCFAOffset:
      case CFIOpKind::DefCFAExpression:
      case CFIOpKind::GnuArgsSize:
        break;
      default:
        return reject(Status::Unsupported, kOpaque);
      }
    }
    if (!IsInitial && !Captured)
      Answer = State;
    return true;
  };
  if (!run(CIE.InitialInstructions, true))
    return Error;
  Initial = State;
  if (!run(FDE.Instructions, false))
    return Error;
  return Answer ? AArch64RAQueryResult{Status::Known, Answer, nullptr}
                : fail(Status::Unknown, kDynamicState);
}

} // namespace neverd::dwarf_eh
