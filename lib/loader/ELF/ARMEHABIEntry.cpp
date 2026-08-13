//===- ARMEHABIEntry.cpp - .ARM.extab entry and personality decoding ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reads one out-of-line `.ARM.extab` entry: the compact or generic header
/// word that names its personality routine, the opcode words the header
/// declares, and where the personality routine's own handler data begins.
///
//===----------------------------------------------------------------------===//

#include "ARMEHABIDetail.h"

#include "neverd/support/BinaryEncoding.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd::arm_ehabi {
namespace detail {

void appendWordOpcodes(std::vector<uint8_t> &Out, uint32_t Word,
                       unsigned Count) {
  for (unsigned Byte = Count; Byte-- > 0;)
    Out.push_back(static_cast<uint8_t>((Word >> (Byte * 8)) & 0xFF));
}

bool decodeTableEntry(const BinaryImage &Img, va_t TableVA, TableEntry &Out,
                      std::string &Diagnostic) {
  const uint8_t *First = Img.readVA(TableVA, kWordSize);
  if (!First) {
    Diagnostic = "ARM EHABI table entry is not mapped readable data";
    return false;
  }
  const uint32_t Word0 = readLE<uint32_t>(First);

  unsigned HeaderWords = 0;
  if ((Word0 & kCompactBit) != 0) {
    if ((Word0 & kCompactVendorMask) != 0) {
      Diagnostic = "ARM EHABI compact entry names an undefined vendor";
      return false;
    }
    const uint8_t Index =
        static_cast<uint8_t>((Word0 >> kCompactIndexShift) & kCompactIndexMask);
    if (Index > 2) {
      Diagnostic = "ARM EHABI compact entry names an undefined personality "
                   "routine index";
      return false;
    }
    Out.Kind = ARMEHABIEntryKind::Compact;
    Out.PersonalityIndex = Index;
    if (Index == 0) {
      // Routine 0 has no word count: its whole descriptor is the three opcode
      // bytes beside the index.
      appendWordOpcodes(Out.Opcodes, Word0, 3);
      HeaderWords = 1;
    } else {
      Out.ExtraWordCount = (Word0 >> kExtraWordShift) & kExtraWordMask;
      appendWordOpcodes(Out.Opcodes, Word0, 2);
      HeaderWords = 1 + Out.ExtraWordCount;
    }
  } else {
    const uint8_t *Second = Img.readVA(TableVA + kWordSize, kWordSize);
    if (!Second) {
      Diagnostic = "ARM EHABI generic entry is truncated";
      return false;
    }
    const uint32_t Word1 = readLE<uint32_t>(Second);
    Out.Kind = ARMEHABIEntryKind::Generic;
    Out.PersonalityVA = resolvePrel31(Word0, TableVA);
    Out.ExtraWordCount = (Word1 >> kGenericExtraWordShift) & kExtraWordMask;
    appendWordOpcodes(Out.Opcodes, Word1, 3);
    HeaderWords = 2 + Out.ExtraWordCount;
  }

  const uint64_t HeaderBytes = uint64_t(HeaderWords) * kWordSize;
  if (HeaderBytes > kMaxEntryBytes || TableVA > InvalidVA - HeaderBytes) {
    Diagnostic = "ARM EHABI entry declares more opcode words than it can hold";
    return false;
  }
  // The words past the first carry four opcode bytes each, most significant
  // first, which is the order the unwinder executes them in.
  for (uint32_t Word = 0; Word < Out.ExtraWordCount; ++Word) {
    const va_t WordVA = static_cast<va_t>(
        TableVA + (HeaderWords - Out.ExtraWordCount + Word) * kWordSize);
    const uint8_t *Bytes = Img.readVA(WordVA, kWordSize);
    if (!Bytes) {
      Diagnostic = "ARM EHABI entry declares opcode words it does not have";
      return false;
    }
    appendWordOpcodes(Out.Opcodes, readLE<uint32_t>(Bytes), 4);
  }
  Out.HandlerDataVA = static_cast<va_t>(TableVA + HeaderBytes);
  return true;
}

bool readsAnItaniumLSDA(ExceptionPersonality Personality) {
  switch (Personality) {
  case ExceptionPersonality::AeabiUnwindCppPr0:
  case ExceptionPersonality::AeabiUnwindCppPr1:
  case ExceptionPersonality::AeabiUnwindCppPr2:
  case ExceptionPersonality::GoRuntimeDispatch:
  case ExceptionPersonality::GoSEHTrampoline:
    return false;
  default:
    return true;
  }
}

} // namespace detail
} // namespace neverd::arm_ehabi
