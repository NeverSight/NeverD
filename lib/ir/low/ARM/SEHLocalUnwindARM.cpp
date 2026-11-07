//===- SEHLocalUnwindARM.cpp - ARM local unwind convention ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Windows ARM `__jump_unwind(TargetFrame, TargetIp)`, whose target is a Thumb
/// code address.  MSVC computes the TargetFrame from its frame register
/// (`add.w r0, r7, #frame`): the EstablisherFrame the unwinder computes, the
/// stack pointer the function was entered with.  The C-specific handler starts
/// a termination handler with the function's callee-saved registers as they
/// were where the runtime left its body, and the handler reads the frame
/// through them.  A scope matches by the `bl` into the runtime itself: the
/// handler steps an unwound frame's return address back by an instruction.
///
/// The unwind codes fix the allocation.  A GS prologue's
/// `__security_push_cookie` allocates its slot in that callee: where the
/// prologue calls one, the frame the code names sits that much above the
/// entry stack pointer the body's own arithmetic gives.
///
//===----------------------------------------------------------------------===//

#include "../SEHLocalUnwindProtocolDetail.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/ExceptionFunction.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"

#include <limits>
#include <optional>

namespace neverd::seh_local_unwind {

namespace {

/// The routine a Windows ARM local unwind calls, `__jump_unwind`.
constexpr llvm::StringLiteral kRoutine("jump_unwind");

llvm::Error refuse(const llvm::Twine &Why) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "local unwind protocol: " + Why);
}

/// What the unwind codes of \p EH allocate, or nullopt when they fix no
/// constant size.
std::optional<uint64_t> codedAllocation(const ExceptionFunction &EH) {
  using K = UnwindOperationKind;
  if (EH.ParseStatus != ExceptionParseStatus::Complete ||
      EH.Kind != RuntimeFunctionKind::Primary ||
      EH.Encoding != ExceptionEncoding::ARM32Unpacked)
    return std::nullopt;
  const auto End =
      llvm::find_if(EH.UnwindOperations, [](const UnwindOperation &Op) {
        return Op.Kind == K::End;
      });
  if (End == EH.UnwindOperations.end())
    return std::nullopt;
  uint64_t Allocation = 0;
  for (const UnwindOperation &Op :
       llvm::make_range(EH.UnwindOperations.begin(), End)) {
    switch (Op.Kind) {
    case K::AllocateStack:
    case K::SaveRegisterPreIndexed:
    case K::SaveRegisterPairPreIndexed:
      if (Op.StackOffset > std::numeric_limits<uint32_t>::max())
        return std::nullopt;
      Allocation += Op.StackOffset;
      break;
    case K::SetStackPointerFromRegister:
    case K::SaveRegister:
    case K::SaveRegisterPair:
    case K::Nop:
      break;
    default:
      return std::nullopt;
    }
  }
  return Allocation;
}

} // namespace

llvm::Expected<SEHLocalUnwindConvention> armConvention(const LowFunc &Function,
                                                       const TargetRegInfo &TRI,
                                                       size_t &WorkRemaining) {
  if (!WorkRemaining)
    return refuse("work budget exhausted in the convention");
  --WorkRemaining;
  if (!Function.ExceptionMetadata || TRI.PointerSize != 4)
    return refuse("no ARM exception metadata");
  const std::optional<uint64_t> Coded =
      codedAllocation(*Function.ExceptionMetadata);
  if (!Coded || !*Coded)
    return refuse("the unwind codes fix no frame");
  const std::optional<DecodedPrologue> Decoded =
      decodedPrologue(Function, TRI, WorkRemaining);
  if (!Decoded)
    return refuse("the prologue moves the stack pointer another way");
  // The cookie slot, allocated by the one helper the prologue calls.
  const uint64_t Unseen = *Coded - Decoded->Allocation;
  if (*Coded < Decoded->Allocation ||
      (Unseen && (Unseen != TRI.PointerSize || Decoded->Calls != 1)))
    return refuse("the decoded prologue disagrees with the unwind codes");
  SEHLocalUnwindConvention Convention;
  Convention.Routine = kRoutine;
  Convention.BodySP = -static_cast<int64_t>(Decoded->BodyAllocation);
  Convention.TargetFrame = static_cast<int64_t>(Unseen);
  Convention.HandlerFrame = Convention.TargetFrame;
  Convention.GSCookieFrame = Convention.TargetFrame;
  Convention.CookieInCallee = Unseen != 0;
  Convention.CalleeHomeBytes = 0;
  Convention.ControlPcIsReturnAddress = false;
  // `__jump_unwind` resumes through `RtlUnwind` with no return value.
  Convention.ResumeReturnValue = 0;
  Convention.HandlersSeeParentRegisters = true;
  return Convention;
}

std::optional<uint64_t> armLiteralPoolValue(const BinaryImage &Image,
                                            uint64_t Address, uint16_t Size) {
  constexpr uint16_t Pointer = 4;
  const va_t At = static_cast<va_t>(Address);
  if (Image.Arch != Arch::ARM || Size != Pointer ||
      !Image.isCodeRange(At, Pointer))
    return std::nullopt;
  const uint8_t *Bytes = Image.readVA(At, Pointer);
  return Bytes ? std::optional<uint64_t>(readLE<uint32_t>(Bytes))
               : std::nullopt;
}

std::set<va_t> armUnwindTargets(const LowFunc &Function,
                                const BinaryImage &Image,
                                const TargetRegInfo &TRI) {
  return surveyUnwindTargets(Function, Image, TRI, kRoutine);
}

} // namespace neverd::seh_local_unwind
