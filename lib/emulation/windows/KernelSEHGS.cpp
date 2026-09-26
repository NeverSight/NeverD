//===- KernelSEHGS.cpp - Checked x64 stack cookies ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check loader-decoded GS metadata against live stack and image storage.
/// The aligned cookie slot and the frame pointer used to encode its value
/// are distinct. No guest address is read until its complete span is checked.
///
//===----------------------------------------------------------------------===//

#include "KernelSEH.h"

namespace neverd::emulation {
namespace {
llvm::Error invalid(llvm::StringRef Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "x64 SEH: " + Message);
}

llvm::Expected<uint64_t> addOffset(uint64_t Base, int32_t Offset) {
  if (Offset < 0) {
    const uint64_t Magnitude = -int64_t(Offset);
    if (Base < Magnitude)
      return invalid("GS frame offset underflows");
    return Base - Magnitude;
  }
  if (uint64_t(Offset) > UINT64_MAX - Base)
    return invalid("GS frame offset overflows");
  return Base + Offset;
}
} // namespace

llvm::Error KernelSEH::checkGSCookie(const ExceptionFunction &Frame,
                                     uint64_t Establisher, Stack Bounds) const {
  if (!Frame.GSCookie || !Cookie)
    return invalid("GS check requires the image security cookie");
  const auto &GS = *Frame.GSCookie;
  uint64_t SlotBase = Establisher;
  if (GS.HasAlignment) {
    auto AlignedBase = addOffset(Establisher, GS.AlignmentBaseOffset);
    if (!AlignedBase)
      return AlignedBase.takeError();
    SlotBase = *AlignedBase & ~(uint64_t(GS.Alignment) - 1);
  }
  auto Slot = addOffset(SlotBase, GS.CookieOffset);
  if (!Slot)
    return Slot.takeError();
  if (*Slot % seh::PointerSize || *Slot < Bounds.Base ||
      *Slot - Bounds.Base > Bounds.Size ||
      seh::PointerSize > Bounds.Size - (*Slot - Bounds.Base))
    return invalid("GS cookie exceeds the current execution stack");
  auto FramePointer = addOffset(Establisher, Frame.FrameOffset);
  if (!FramePointer)
    return FramePointer.takeError();
  auto Stored = ReadStack(*Slot);
  if (!Stored)
    return Stored.takeError();
  auto Expected = Cookie();
  if (!Expected)
    return Expected.takeError();
  const uint64_t Decoded = *Stored ^ *FramePointer;
  if (Decoded != *Expected || (Decoded >> seh::SecurityCookieBits))
    return invalid("GS security cookie check failed");
  return llvm::Error::success();
}
} // namespace neverd::emulation
