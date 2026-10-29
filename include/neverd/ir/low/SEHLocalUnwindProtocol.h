//===- SEHLocalUnwindProtocol.h - Saved-frame local unwinds ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A `return` or `goto` that leaves a Windows `__try` with a `__finally`
/// compiles to a call of the runtime's local unwind with this function's frame
/// and a target in it.  The compiler passes the frame as a reload of a slot
/// its prologue fills with the frame value: the saved-frame protocol.  This
/// certifies the calls that follow it, so LowToMed can replace each by the
/// termination-handler calls and the jump it makes.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_SEHLOCALUNWINDPROTOCOL_H
#define NEVERD_IR_LOW_SEHLOCALUNWINDPROTOCOL_H

#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace neverd {

class BinaryImage;
struct TargetRegInfo;

/// What one architecture's local unwind convention fixes for a function whose
/// frame it can certify, as entry-relative stack addresses.
struct SEHLocalUnwindConvention {
  /// The runtime routine, by its import name without leading underscores.
  llvm::StringRef Routine;
  /// The stack pointer the prologue leaves for the body, as the unwind codes
  /// give it and the decoded prologue confirms.
  int64_t BodySP = 0;
  /// The frame the routine takes as its first argument for this function.
  int64_t TargetFrame = 0;
  /// The EstablisherFrame the runtime passes a filter or termination handler
  /// of this function.
  int64_t HandlerFrame = 0;
  /// Where the GS cookie's slot offset and the frame value its prologue
  /// masks the cookie with are taken from (`__GSHandlerCheckCommon`'s
  /// CookieFrameBase and CookieXorValue), for a frame the GS data does not
  /// realign.
  int64_t GSCookieFrame = 0;
  /// Bytes above a call's stack pointer the callee may write without the
  /// caller storing them first (the Win64 home area).
  uint64_t CalleeHomeBytes = 0;
  /// The runtime matches a scope by the return address of the call into it,
  /// not by the call instruction's own address.
  bool ControlPcIsReturnAddress = false;
  /// What the return register holds where the unwind resumes, when the
  /// routine fixes it.
  std::optional<uint64_t> ResumeReturnValue;
};

/// One local unwind the protocol certifies: the runtime call, and where in
/// the function it resumes.
struct LowSEHLocalUnwind {
  va_t CallAddr = InvalidVA;
  int CallSeq = -1;
  /// The address the runtime matches the function's scopes against.
  va_t ControlPc = InvalidVA;
  va_t Target = InvalidVA;
  /// The EstablisherFrame the handlers receive, relative to the stack
  /// pointer the call runs on.
  int64_t HandlerFrameFromSP = 0;
  /// SEHLocalUnwindConvention::ResumeReturnValue.
  std::optional<uint64_t> ResumeReturnValue;
  /// The frame the call passes, relative to the entry stack pointer.
  int64_t TargetFrame = 0;
  /// No code but the certified calls names the target as a value: none of
  /// the function's other code, and none of its out-of-line filters and
  /// termination handlers or the ones nested in them, every one of which is
  /// in the bodies given (a finally that local-unwinds to the target names
  /// it).  Relocations are the caller's to check.
  bool OnlyExitsName = false;
};

/// Whether \p Function calls a runtime local unwind routine at all, by its
/// authenticated import: the cheap test before any proof.
bool callsLowSEHLocalUnwind(const LowFunc &Function, const BinaryImage &Image);

/// \p Function's local unwind convention, from its architecture's frame rules
/// (per-architecture files), or why its frame does not fit them.
llvm::Expected<SEHLocalUnwindConvention>
sehLocalUnwindConvention(const LowFunc &Function, const TargetRegInfo &TRI,
                         size_t &WorkRemaining);

/// The calls of the authenticated local unwind import in \p Function that the
/// saved-frame protocol certifies.  A call qualifies when its target is an
/// exact code address that starts a block of the function, it runs on the
/// body's stack pointer, and its frame is the convention's TargetFrame
/// itself or an exact reload of one frame slot that:
///  - every store the function makes to it fills with that TargetFrame;
///  - no store of the function at an inexact frame address may reach;
///  - no callee may write as its home area;
///  - no out-of-line filter or termination handler of the function writes
///    through its EstablisherFrame, with every such handler's body in
///    \p Bodies.
/// The frame values the function computes may reach a callee or memory only
/// as pointers to other frame objects: its TargetFrame, its body stack
/// pointer and the slot's address reach only the slot itself, the runtime
/// (the local unwind, and setjmp, which keeps the frame in its jump buffer
/// for longjmp's unwind) and the handlers.  The GS cookie is the one other
/// place a frame value may go: the frame its GS data names, XORed with a value
/// of no frame, stored to the slot that data places, and read back only to be
/// XORed with the same frame, which leaves the cookie.  This trusts that only
/// in-bounds accesses reach a frame object, so a pointer to one never reaches
/// the compiler's saved-frame slot, which only its own prologue protocol
/// writes.  An argument register a direct callee provably never observes
/// (\p Unread, the callee summaries' proof over complete bodies, which never
/// covers a result register) passes it nothing; any other callee may read
/// all four.  Any other shape refuses the
/// function, and its calls keep their runtime form.
llvm::Expected<std::vector<LowSEHLocalUnwind>> certifyLowSEHLocalUnwinds(
    const LowFunc &Function, const BinaryImage &Image, const TargetRegInfo &TRI,
    const std::map<va_t, const LowFunc *> &Bodies, size_t &WorkRemaining,
    const std::map<va_t, GPRFamilyMask> *Unread = nullptr);

} // namespace neverd

#endif // NEVERD_IR_LOW_SEHLOCALUNWINDPROTOCOL_H
