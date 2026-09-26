//===- KernelSEH.cpp - Checked x64 catch-all exception transfer -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Plans API-raised C exception handling using decoded loader metadata.
///
//===----------------------------------------------------------------------===//

#include "KernelSEH.h"

#include "llvm/ADT/Twine.h"

#include <set>
#include <utility>

namespace neverd::emulation {
namespace {
llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "x64 SEH: " + Message);
}

bool nonvolatile(uint64_t Register) {
  return Register < seh::RegisterCount &&
         ((seh::NonvolatileRegisterMask >> Register) & 1);
}

llvm::Error validateFrame(const ExceptionFunction &F) {
  if (F.ParseStatus != ExceptionParseStatus::Complete)
    return invalid("encountered incomplete exception metadata");
  if (F.Encoding != ExceptionEncoding::X64UnwindV1 || F.UnwindVersion != 1)
    return invalid("only x64 V1 unwind records are supported");
  if (F.Kind != RuntimeFunctionKind::Primary || F.ChainedPrimaryRange ||
      F.ChainedUnwindInfoRVA || (F.UnwindFlags & seh::ChainFlag))
    return invalid("chained unwind records are unsupported");
  if (F.UnwindFlags & ~(seh::ExceptionHandlerFlag | seh::UnwindHandlerFlag))
    return invalid("unsupported unwind flags");
  if (F.GSCookie || (F.Personality != ExceptionPersonality::None &&
                     F.Personality != ExceptionPersonality::CSpecificHandler))
    return invalid("encountered unsupported language personality");
  if (F.Cxx ||
      ((F.UnwindFlags != 0) !=
       (F.Personality == ExceptionPersonality::CSpecificHandler)) ||
      ((F.Personality == ExceptionPersonality::CSpecificHandler) !=
       F.SEH.has_value()))
    return invalid("inconsistent C exception-handler metadata");
  if (F.FrameRegister && !nonvolatile(F.FrameRegister))
    return invalid("invalid frame register");
  if (F.FrameOffset > seh::MaxFrameOffset ||
      F.FrameOffset % seh::FrameOffsetScale ||
      (!F.FrameRegister && F.FrameOffset))
    return invalid("invalid frame-register offset");
  if (F.UnwindOperations.size() > seh::MaxUnwindOperations)
    return invalid("unwind operation limit exceeded");
  uint32_t PreviousOffset = F.PrologueSize;
  unsigned FrameSetCount = 0;
  for (const auto &Op : F.UnwindOperations) {
    if (!Op.CodeOffset || Op.CodeOffset > PreviousOffset)
      return invalid("invalid unwind operation order");
    PreviousOffset = Op.CodeOffset;
    switch (Op.Kind) {
    case UnwindOperationKind::PushNonVolatile:
    case UnwindOperationKind::SaveNonVolatile:
    case UnwindOperationKind::SaveNonVolatileFar:
      if (!nonvolatile(Op.Register))
        return invalid("invalid saved nonvolatile register");
      if (Op.StackOffset % seh::PointerSize)
        return invalid("unaligned nonvolatile save offset");
      break;
    case UnwindOperationKind::AllocateSmall:
      if (Op.StackOffset > seh::MaxSmallAllocation)
        return invalid("invalid small stack allocation");
      [[fallthrough]];
    case UnwindOperationKind::AllocateLarge:
      if (!Op.StackOffset || Op.StackOffset % seh::PointerSize)
        return invalid("invalid stack allocation");
      break;
    case UnwindOperationKind::SetFramePointer:
      if (!F.FrameRegister || ++FrameSetCount != 1)
        return invalid("invalid frame-pointer operation");
      break;
    case UnwindOperationKind::SaveXMM128:
    case UnwindOperationKind::SaveXMM128Far:
      return invalid("XMM unwind restoration is unsupported");
    default:
      return invalid("encountered unsupported unwind operation");
    }
  }
  if (F.FrameRegister && FrameSetCount != 1)
    return invalid("frame register has no establishing operation");
  return llvm::Error::success();
}
} // namespace

KernelSEH::KernelSEH(const ExceptionInfo &Metadata, uint64_t PreferredBase,
                     uint64_t ActualBase, uint64_t ImageSize,
                     ReadStack64 ReadStack, IsExecutable Executable)
    : Metadata(Metadata), PreferredBase(PreferredBase), ActualBase(ActualBase),
      ImageSize(ImageSize), ReadStack(std::move(ReadStack)),
      Executable(std::move(Executable)) {}

llvm::Expected<std::optional<KernelSEH::Transfer>>
KernelSEH::plan(uint32_t ExceptionCode, const Context &Caller,
                Stack Bounds) const {
  if (!ImageSize || ImageSize > UINT64_MAX - PreferredBase ||
      ImageSize > UINT64_MAX - ActualBase || !Bounds.Size ||
      Bounds.Size > UINT64_MAX - Bounds.Base || !ReadStack || !Executable)
    return invalid("invalid image, stack or memory contract");
  if (Metadata.Functions.size() > seh::MaxFunctions)
    return invalid("runtime-function limit exceeded");
  if (Metadata.StructuralDecode &&
      Metadata.StructuralDecode->ParseStatus != ExceptionParseStatus::Complete)
    return invalid("incomplete exception directory metadata");
  const uint64_t StackEnd = Bounds.Base + Bounds.Size;
  const auto InStack = [&](uint64_t Address, uint64_t Size) {
    return Address >= Bounds.Base && Address <= StackEnd &&
           Size <= StackEnd - Address;
  };
  const auto Read = [&](uint64_t Address) -> llvm::Expected<uint64_t> {
    if (Address % seh::PointerSize || !InStack(Address, seh::PointerSize))
      return invalid("unwind read exceeds the current execution stack");
    return ReadStack(Address);
  };
  const auto ToActual = [&](uint64_t Address) -> llvm::Expected<uint64_t> {
    if (Address < PreferredBase || Address - PreferredBase >= ImageSize)
      return invalid("exception target lies outside its image");
    const uint64_t Actual = ActualBase + (Address - PreferredBase);
    if (!Executable(Actual))
      return invalid("exception target is not executable");
    return Actual;
  };
  Context Current = Caller;
  std::set<std::pair<uint64_t, uint64_t>> Seen;
  for (uint64_t Depth = 0; Depth < seh::MaxFrames; ++Depth) {
    const uint64_t SP = Current.GPR[seh::StackRegister];
    if (SP % seh::PointerSize || !InStack(SP, seh::PointerSize))
      return invalid("invalid exception frame stack pointer");
    if (!Seen.emplace(Current.PC, SP).second)
      return invalid("cyclic exception frame chain");
    // The session's synthetic invocation return is outside the image and
    // ends this execution's search. It must never enter a parent callback.
    if (Current.PC < ActualBase || Current.PC - ActualBase >= ImageSize)
      return std::optional<Transfer>{};
    if (!Executable(Current.PC))
      return invalid("exception control address is not executable");
    const uint64_t OriginalPC = PreferredBase + (Current.PC - ActualBase);
    const ExceptionFunction *Frame = nullptr;
    for (const auto &Candidate : Metadata.Functions) {
      if (!Candidate.CodeRange.contains(OriginalPC))
        continue;
      if (Frame)
        return invalid("ambiguous overlapping runtime functions");
      Frame = &Candidate;
    }
    // Legacy COFF summaries combine directory and language failures. Without
    // structural provenance, a missing frame is not proven to be a leaf.
    if (!Frame && !Metadata.StructuralDecode &&
        Metadata.ParseStatus != ExceptionParseStatus::Complete)
      return invalid("incomplete exception directory cannot establish a leaf");
    uint64_t Cursor = SP;
    if (Frame) {
      if (auto E = validateFrame(*Frame))
        return std::move(E);
      if (Frame->CodeRange.Begin < PreferredBase ||
          Frame->CodeRange.End > PreferredBase + ImageSize ||
          !Frame->CodeRange.isValid() ||
          Frame->PrologueSize > Frame->CodeRange.size())
        return invalid("runtime function lies outside its image");
      if (OriginalPC - Frame->CodeRange.Begin < Frame->PrologueSize)
        return invalid("exception search in a prologue is unsupported");
      uint64_t Establisher = SP;
      if (Frame->FrameRegister) {
        const uint64_t FP = Current.GPR[Frame->FrameRegister];
        if (FP < Frame->FrameOffset)
          return invalid("frame-register offset underflows");
        Establisher = FP - Frame->FrameOffset;
      }
      if (Establisher < SP || Establisher % seh::PointerSize ||
          !InStack(Establisher, seh::PointerSize))
        return invalid("establisher frame exceeds the current stack");
      if (Frame->SEH) {
        if (Frame->SEH->Scopes.size() > seh::MaxScopes)
          return invalid("SEH scope limit exceeded");
        for (const auto &Scope : Frame->SEH->Scopes) {
          if (!Scope.GuardedRange.contains(OriginalPC))
            continue;
          if (Scope.ParseStatus != ExceptionParseStatus::Complete ||
              !Scope.GuardedRange.isValid())
            return invalid("encountered incomplete SEH scope");
          if (Scope.Kind != SEHScopeKind::CatchAll)
            return invalid("encountered unsupported filter or finally");
          if (!(Frame->UnwindFlags & seh::ExceptionHandlerFlag) ||
              Scope.FilterOrFinallyVA || Scope.NormalizedFilterVA ||
              Scope.HandlerVA != Scope.ContinuationVA ||
              !Frame->CodeRange.contains(Scope.HandlerVA) ||
              Scope.GuardedRange.contains(Scope.HandlerVA))
            return invalid("invalid catch-all continuation");
          auto Target = ToActual(Scope.HandlerVA);
          if (!Target)
            return Target.takeError();
          Current.GPR[seh::StackRegister] = Establisher;
          // The x64 C handler continuation receives the exception code in
          // the integer return register; GetExceptionCode uses its low 32 bits.
          Current.GPR[seh::ReturnRegister] = ExceptionCode;
          Current.PC = *Target;
          return std::optional<Transfer>{
              Transfer{Current, Establisher, *Target, ExceptionCode}};
        }
      }
      for (const auto &Op : Frame->UnwindOperations) {
        switch (Op.Kind) {
        case UnwindOperationKind::PushNonVolatile: {
          auto Value = Read(Cursor);
          if (!Value)
            return Value.takeError();
          Current.GPR[Op.Register] = *Value;
          Cursor += seh::PointerSize;
          break;
        }
        case UnwindOperationKind::AllocateSmall:
        case UnwindOperationKind::AllocateLarge:
          if (!InStack(Cursor, Op.StackOffset))
            return invalid("unwind allocation exceeds the current stack");
          Cursor += Op.StackOffset;
          break;
        case UnwindOperationKind::SetFramePointer:
          Cursor = Establisher;
          break;
        case UnwindOperationKind::SaveNonVolatile:
        case UnwindOperationKind::SaveNonVolatileFar: {
          if (Op.StackOffset > UINT64_MAX - Establisher)
            return invalid("saved-register address overflows");
          auto Value = Read(Establisher + Op.StackOffset);
          if (!Value)
            return Value.takeError();
          Current.GPR[Op.Register] = *Value;
          break;
        }
        default:
          llvm_unreachable("unwind operation was validated");
        }
      }
    }
    auto Return = Read(Cursor);
    if (!Return)
      return Return.takeError();
    if (!*Return)
      return invalid("null unwind return address");
    Current.GPR[seh::StackRegister] = Cursor + seh::PointerSize;
    if (Current.GPR[seh::StackRegister] <= SP)
      return invalid("unwind did not advance the stack");
    Current.PC = *Return - 1;
  }
  return invalid("exception frame limit exceeded");
}

} // namespace neverd::emulation
