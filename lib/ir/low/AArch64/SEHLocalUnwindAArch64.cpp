//===- SEHLocalUnwindAArch64.cpp - ARM64 local unwind convention ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Windows ARM64 `_local_unwind(TargetFrame, TargetIp)`.  The unwinder's
/// EstablisherFrame is the stack pointer the function was entered with: MSVC
/// saves it (`add x16, sp, #frame; str x16, [x29, #slot]`) and passes it as
/// TargetFrame, and the GS data places the cookie from it.  The C-specific
/// handler starts a filter or termination handler with the function's
/// callee-saved registers as they were where the runtime left its body, and
/// passes the function's frame pointer, not that EstablisherFrame, in x1.
/// The unwind codes fix the prologue's allocation and where it points the
/// frame pointer.  A scope matches by the `bl` into the runtime itself: the
/// handler steps an unwound frame's return address back by an instruction,
/// so MSVC ends a `__try` right after such a call at /O2.  The routine resumes
/// through `RtlUnwind` with no return value, as on x64.  A GS prologue's
/// `__security_push_cookie` allocates its slot in that callee: where the
/// prologue calls one, the frame the code names sits that much above the entry
/// stack pointer the body's own arithmetic gives.  The runtime routine is
/// reached through an import veneer, `adrp xN, page; ldr xN, [xN, #slot]; br
/// xN`.
///
//===----------------------------------------------------------------------===//

#include "../SEHLocalUnwindProtocolDetail.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/ExceptionFunction.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"

#include <limits>

namespace neverd::seh_local_unwind {

namespace {

/// The routine a Windows ARM64 local unwind calls, `_local_unwind`.
constexpr llvm::StringLiteral kRoutine("local_unwind");

llvm::Error refuse(const llvm::Twine &Why) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "local unwind protocol: " + Why);
}

/// The frame the unwind codes fix, in bytes below the entry stack pointer.
struct CodedFrame {
  /// What the prologue allocates: the stack pointer it leaves.
  uint64_t Allocation = 0;
  /// Where it points the frame pointer.
  uint64_t FramePointer = 0;
};

/// The frame \p EH's unwind codes fix, or nullopt when they fix no constant
/// allocation and frame pointer.  The codes run from the prologue's last
/// instruction back to its first.
std::optional<CodedFrame> codedFrame(const ExceptionFunction &EH) {
  using K = UnwindOperationKind;
  if (EH.ParseStatus != ExceptionParseStatus::Complete ||
      EH.Kind != RuntimeFunctionKind::Primary ||
      (EH.Encoding != ExceptionEncoding::ARM64Unpacked &&
       EH.Encoding != ExceptionEncoding::ARM64Packed))
    return std::nullopt;
  const auto End =
      llvm::find_if(EH.UnwindOperations, [](const UnwindOperation &Op) {
        return Op.Kind == K::End;
      });
  if (End == EH.UnwindOperations.end())
    return std::nullopt;
  CodedFrame Frame;
  std::optional<uint64_t> FramePointer;
  for (const UnwindOperation &Op :
       llvm::reverse(llvm::make_range(EH.UnwindOperations.begin(), End))) {
    switch (Op.Kind) {
    case K::AllocateStack:
    case K::SaveRegisterPreIndexed:
    case K::SaveRegisterPairPreIndexed:
      if (Op.StackOffset > std::numeric_limits<uint32_t>::max())
        return std::nullopt;
      Frame.Allocation += Op.StackOffset;
      break;
    case K::SetFramePointer:
    case K::AddFramePointer:
      if (FramePointer || Op.StackOffset > Frame.Allocation)
        return std::nullopt;
      FramePointer = Frame.Allocation -
                     (Op.Kind == K::AddFramePointer ? Op.StackOffset : 0);
      break;
    case K::SaveRegister:
    case K::SaveRegisterPair:
    case K::SaveNextPair:
    case K::SignReturnAddress:
    case K::Nop:
      break;
    default:
      // A chained scope, an SVE-sized allocation, a stack pointer restored
      // from a register, or a custom frame: the size is not this sum.
      return std::nullopt;
    }
  }
  if (!FramePointer)
    return std::nullopt;
  Frame.FramePointer = *FramePointer;
  return Frame;
}

constexpr size_t InsnBytes = 4;

/// What `__security_push_cookie` allocates.
constexpr uint64_t kCookieSlotBytes = 16;

/// The page an `adrp` at \p At names and the register it writes, or nullopt.
std::optional<std::pair<va_t, uint32_t>> adrpPage(uint32_t Word, va_t At) {
  if ((Word & 0x9f000000u) != 0x90000000u)
    return std::nullopt;
  int64_t Page = static_cast<int64_t>((((Word >> 5) & 0x7ffff) << 2) |
                                      ((Word >> 29) & 0x3))
                 << 12;
  if (Page & (int64_t(1) << 32))
    Page |= ~((int64_t(1) << 33) - 1);
  return std::pair{(At & ~va_t(0xfff)) + static_cast<va_t>(Page), Word & 0x1f};
}

/// The address `adrp xN, page; ldr xT, [xN, #imm]` loads from, with the
/// `adrp` at \p At, or nullopt.
std::optional<va_t> pageLoad(uint32_t Adrp, uint32_t Ldr, va_t At,
                             uint32_t Loaded) {
  const auto Page = adrpPage(Adrp, At);
  if (!Page || (Ldr & 0xffc00000u) != 0xf9400000u ||
      ((Ldr >> 5) & 0x1f) != Page->second || (Ldr & 0x1f) != Loaded)
    return std::nullopt;
  return Page->first + ((Ldr >> 10) & 0xfff) * 8;
}

/// The words at \p Address, when they are code.
std::optional<std::vector<uint32_t>> codeWords(const BinaryImage &Image,
                                               va_t Address, size_t Count) {
  if (Address % InsnBytes || !Image.isCodeRange(Address, Count * InsnBytes))
    return std::nullopt;
  const uint8_t *Bytes = Image.readVA(Address, Count * InsnBytes);
  if (!Bytes)
    return std::nullopt;
  std::vector<uint32_t> Words(Count);
  for (size_t I = 0; I < Count; ++I)
    Words[I] = readLE<uint32_t>(Bytes + I * InsnBytes);
  return Words;
}

} // namespace

llvm::Expected<SEHLocalUnwindConvention>
aarch64Convention(const LowFunc &Function, const TargetRegInfo &TRI,
                  size_t &WorkRemaining) {
  if (!WorkRemaining)
    return refuse("work budget exhausted in the convention");
  --WorkRemaining;
  if (!Function.ExceptionMetadata || TRI.PointerSize != 8)
    return refuse("no ARM64 exception metadata");
  const std::optional<CodedFrame> Frame =
      codedFrame(*Function.ExceptionMetadata);
  if (!Frame || !Frame->Allocation || Frame->Allocation % 16)
    return refuse("the unwind codes fix no frame and frame pointer");
  // The cookie slot, allocated by the one helper the prologue calls, is what
  // the body's own arithmetic does not show.
  const std::optional<DecodedPrologue> Decoded =
      decodedPrologue(Function, TRI, WorkRemaining);
  const uint64_t Unseen =
      Function.ExceptionMetadata->GSCookie && Decoded && Decoded->Calls == 1
          ? kCookieSlotBytes
          : 0;
  SEHLocalUnwindConvention Convention;
  Convention.Routine = kRoutine;
  // Where a frame pointer restores the stack pointer for the unwinder, the
  // codes need not record what the body allocates below it.
  Convention.BodySP = -static_cast<int64_t>(
      Decoded && Decoded->BodyAllocation + Unseen >= Frame->Allocation
          ? Decoded->BodyAllocation
          : Frame->Allocation);
  // The EstablisherFrame the unwinder computes, and so the frame a local
  // unwind names, is the stack pointer the function was entered with; the
  // runtime hands the handlers the frame pointer.
  Convention.TargetFrame = static_cast<int64_t>(Unseen);
  Convention.CookieInCallee = Unseen != 0;
  Convention.HandlerFrame = -static_cast<int64_t>(Frame->FramePointer);
  // The GS data places the cookie from the EstablisherFrame.
  Convention.GSCookieFrame = Convention.TargetFrame;
  Convention.CalleeHomeBytes = 0;
  // The C-specific handler steps an unwound frame's control PC back by one
  // instruction, to the `bl` itself.
  Convention.ControlPcIsReturnAddress = false;
  Convention.ResumeReturnValue = 0;
  Convention.HandlersSeeParentRegisters = true;
  Convention.FramePointerIsHandlerFrame = true;
  return Convention;
}

std::set<va_t> aarch64UnwindTargets(const LowFunc &Function,
                                    const BinaryImage &Image,
                                    const TargetRegInfo &TRI) {
  return surveyUnwindTargets(Function, Image, TRI, kRoutine);
}

const Import *aarch64VeneerImport(const BinaryImage &Image, va_t Address) {
  if (Image.Arch != Arch::AArch64)
    return nullptr;
  // adrp xN, page; ldr xN, [xN, #slot]; br xN, with xN x16 or x17
  const auto W = codeWords(Image, Address, 3);
  if (!W)
    return nullptr;
  const uint32_t Reg = (*W)[0] & 0x1f;
  const std::optional<va_t> Slot = pageLoad((*W)[0], (*W)[1], Address, Reg);
  if (!Slot || (Reg != 16 && Reg != 17) ||
      ((*W)[2] & 0xfffffc1fu) != 0xd61f0000u || (((*W)[2] >> 5) & 0x1f) != Reg)
    return nullptr;
  for (const Import &Imported : Image.Imports)
    if (Imported.IATAddr == *Slot)
      return &Imported;
  return nullptr;
}

} // namespace neverd::seh_local_unwind
