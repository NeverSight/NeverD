//===- Signature.h - FLIRT signature data types ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Core data types for FLIRT-compatible function signature matching.
///
/// A signature pattern consists of:
///   - Leading bytes with a mask (fixed bytes vs wildcards)
///   - CRC16 checksum over trailing bytes for verification
///   - One or more function name associations with offsets
///
/// The current loader accepts the text representation of these records.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_SIGNATURE_H
#define NEVERD_SIGS_SIGNATURE_H

#include <cstdint>
#include <string>
#include <vector>

namespace neverd {
namespace sigs {

struct PatternByte {
  uint8_t Value = 0;
  bool IsWildcard = false;
};

struct FuncRef {
  uint32_t Offset = 0;
  std::string Name;
};

struct PatternModule {
  std::vector<PatternByte> LeadingBytes;

  uint16_t CRC16 = 0;
  uint8_t CRCLen = 0;

  uint32_t TotalLen = 0;

  std::vector<FuncRef> PublicNames;

  std::vector<PatternByte> TailBytes;
};

struct SigMatch {
  uint64_t Address = 0;
  std::string Name;
  std::string LibraryName;
  uint32_t FuncLen = 0;
};

} // namespace sigs
} // namespace neverd

#endif // NEVERD_SIGS_SIGNATURE_H
