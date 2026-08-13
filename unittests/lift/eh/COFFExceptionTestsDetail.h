//===- COFFExceptionTestsDetail.h - Shared Windows EH test harness --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Image builders and readers shared by the COFFException* translation units.
// Everything here is `inline`: the harness is linked into one test binary out
// of a dozen TUs, so a non-inline definition would be a duplicate symbol.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_EH_COFFEXCEPTIONTESTSDETAIL_H
#define NEVERD_UNITTESTS_LIFT_EH_COFFEXCEPTIONTESTSDETAIL_H

#include "gtest/gtest.h"

#include "neverd/Common.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace neverd::coff_eh_test {

inline BinaryImage makeX64ExceptionImage(size_t XDataSize = 0x100) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;

  Segment Text;
  Text.Name = ".text";
  Text.VA = Img.Base + 0x1000;
  Text.Size = 0x200;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size, 0x90);
  Img.Segments.push_back(std::move(Text));

  Segment XData;
  XData.Name = ".xdata";
  XData.VA = Img.Base + 0x3000;
  XData.Size = XDataSize;
  XData.Flags = SegmentFlags::Readable;
  XData.Data.resize(XDataSize);
  Img.Segments.push_back(std::move(XData));
  return Img;
}

inline void addPersonalityImport(BinaryImage &Img, va_t StubVA,
                                 llvm::StringRef Name) {
  Import Imp;
  Imp.Module = "vcruntime-test.dll";
  Imp.Name = Name.str();
  Imp.IATAddr = Img.Base + 0x30f0;
  Img.Imports.push_back(std::move(Imp));
  ASSERT_TRUE(Img.recordImportStub(StubVA, Img.Imports.size() - 1));
}

/// The set of registers an operation names, as a sorted list, so a test can
/// state the whole of what it expects rather than probing a bitmask.
inline std::vector<uint16_t> registersOf(const UnwindOperation &Op) {
  std::vector<uint16_t> Registers;
  for (uint16_t Reg = 0; Reg < 32; ++Reg)
    if (Op.RegisterMask & (uint32_t(1) << Reg))
      Registers.push_back(Reg);
  return Registers;
}

} // namespace neverd::coff_eh_test

#endif // NEVERD_UNITTESTS_LIFT_EH_COFFEXCEPTIONTESTSDETAIL_H
