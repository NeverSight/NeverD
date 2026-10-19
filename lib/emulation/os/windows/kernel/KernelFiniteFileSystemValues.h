//===- KernelFiniteFileSystemValues.h - Finite filesystem contracts
//-------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Finite filesystem contracts.
///

#ifndef NEVERD_EMULATION_KERNELFINITEFILESYSTEMVALUES_H
#define NEVERD_EMULATION_KERNELFINITEFILESYSTEMVALUES_H
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <vector>
namespace neverd::emulation::finite_file_system {
inline constexpr uint32_t AccessDenied = 0xc0000022, PathNotFound = 0xc000003a,
                          SharingViolation = 0xc0000043,
                          NotDirectory = 0xc0000103, IsDirectory = 0xc00000ba,
                          EndOfFile = 0xc0000011, BufferOverflow = 0x80000005,
                          MediaWriteProtected = 0xc00000a2,
                          DiskFull = 0xc000007f, DirectoryNotEmpty = 0xc0000101;
inline constexpr uint32_t ReadData = 1, WriteData = 2, AppendData = 4,
                          Delete = 0x10000, Synchronize = 0x100000,
                          DeleteOnClose = 0x1000;
inline constexpr uint64_t FileHandleLimit = 0x500ffffc;
inline llvm::Error fileError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "filesystem: " + Message);
}
inline uint32_t accessMask(uint32_t A) {
  uint32_t R = A & 0x0fffffff;
  if (A & 0x80000000)
    R |= 0x120089;
  if (A & 0x40000000)
    R |= 0x120116;
  if (A & 0x20000000)
    R |= 0x1200a0;
  if (A & 0x10000000)
    R |= 0x1f01ff;
  return R;
}
inline uint32_t shareRequired(uint32_t A) {
  return (A & (ReadData | 0x20) ? 1 : 0) |
         (A & (WriteData | AppendData) ? 2 : 0) | (A & Delete ? 4 : 0);
}
} // namespace neverd::emulation::finite_file_system
#endif
