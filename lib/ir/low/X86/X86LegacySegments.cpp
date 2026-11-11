//===- X86LegacySegments.cpp - Cached protected-mode operands -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep segment permission, bounds and implementation-defined wrap decisions
/// independent of the paged or image-backed memory provider.
//===----------------------------------------------------------------------===//

#include "neverd/ir/X86/X86LegacySegments.h"

#include "llvm/Support/Errc.h"

#include <cstdint>
#include <utility>

namespace neverd {

llvm::Error validateX86LegacySegmentCache(X86SegmentRegister Segment,
                                          const X86LegacySegmentCache &C,
                                          bool Virtual8086) {
  if (!x86SegmentRegisterName(unsigned(Segment)))
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "invalid legacy segment register");
  if (Virtual8086) {
    if (C.Unusable || C.Base != uint32_t(C.Selector) << 4 ||
        C.Limit != UINT16_MAX || C.Attributes != 0xf3)
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "invalid loaded VM86 segment cache");
    return llvm::Error::success();
  }
  const bool CS = Segment == X86SegmentRegister::CS;
  const bool SS = Segment == X86SegmentRegister::SS;
  if (C.Unusable) {
    if (CS || SS)
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "protected CS/SS requires a usable cache");
    return llvm::Error::success();
  }
  const bool Code = C.Attributes & 8, ReadWrite = C.Attributes & 2;
  if ((C.Attributes & 0x90) != 0x90 || (C.Attributes & 0x2f00) ||
      (CS && !Code) || (SS && (Code || !ReadWrite)) ||
      (!CS && Code && !ReadWrite) ||
      ((C.Attributes & 0x8000) ? (C.Limit & 0xfff) != 0xfff
                               : C.Limit > 0xfffff))
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "invalid loaded protected-mode segment cache");
  return llvm::Error::success();
}

llvm::Expected<X86LegacySegmentResult>
checkX86LegacySegmentAccess(const X86LegacySegmentCache &C,
                            const X86LegacySegmentAccess &A) {
  if (auto Error = validateX86LegacySegmentCache(A.Segment, C, A.Virtual8086))
    return std::move(Error);
  if (!A.Bytes ||
      (A.Kind != X86SegmentAccessKind::Read &&
       A.Kind != X86SegmentAccessKind::Write &&
       A.Kind != X86SegmentAccessKind::Execute) ||
      (A.Kind == X86SegmentAccessKind::Execute &&
       A.Segment != X86SegmentRegister::CS) ||
      (A.FullLimitChoice &&
       *A.FullLimitChoice != X86FullSegmentLimitChoice::Fault &&
       *A.FullLimitChoice != X86FullSegmentLimitChoice::Wrap))
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "invalid legacy segment operand");
  X86LegacySegmentResult Result;
  const auto Fault = [&]() {
    Result.FaultVector = A.Segment == X86SegmentRegister::SS ? 12 : 13;
    return Result;
  };
  const bool Code = C.Attributes & 8, ReadWrite = C.Attributes & 2;
  if (C.Unusable ||
      (A.Kind == X86SegmentAccessKind::Write && (Code || !ReadWrite)) ||
      (A.Kind == X86SegmentAccessKind::Read && Code && !ReadWrite))
    return Fault();
  const uint64_t End = uint64_t(A.Offset) + A.Bytes - 1;
  if (!Code && (C.Attributes & 4)) {
    const uint32_t Upper = C.Attributes & 0x4000 ? UINT32_MAX : UINT16_MAX;
    if (A.Offset <= C.Limit || End > Upper)
      return Fault();
  } else if (End > C.Limit) {
    if (C.Limit != UINT32_MAX)
      return Fault();
    if (!A.FullLimitChoice)
      return llvm::createStringError(
          llvm::errc::invalid_argument,
          "full-limit segment crossing requires an explicit execution choice");
    if (*A.FullLimitChoice == X86FullSegmentLimitChoice::Fault)
      return Fault();
  }
  Result.Linear = C.Base + A.Offset;
  return Result;
}

} // namespace neverd
