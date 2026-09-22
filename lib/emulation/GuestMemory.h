//===- GuestMemory.h - Private emulator memory boundary -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private emulator memory boundary.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_GUESTMEMORY_H
#define NEVERD_EMULATION_GUESTMEMORY_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <vector>

namespace neverd::emulation {
enum GuestPermission : unsigned { Read = 1, Write = 2, Execute = 4 };
class GuestMemory {
public:
  virtual ~GuestMemory() = default;
  virtual llvm::Error map(uint64_t Address, uint64_t Size,
                          unsigned Permissions) = 0;
  virtual llvm::Error protect(uint64_t Address, uint64_t Size,
                              unsigned Permissions) = 0;
  virtual llvm::Error read(uint64_t Address,
                           llvm::MutableArrayRef<uint8_t> Bytes) = 0;
  virtual llvm::Error write(uint64_t Address,
                            llvm::ArrayRef<uint8_t> Bytes) = 0;
  llvm::Expected<uint64_t> readInteger(uint64_t Address, unsigned Size);
  llvm::Error writeInteger(uint64_t Address, uint64_t Value, unsigned Size);
};
} // namespace neverd::emulation
#endif
