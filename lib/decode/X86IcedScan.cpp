//===- X86IcedScan.cpp - Bounded iced scan adapter -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Map a closed set of validated iced encodings to Capstone instruction IDs.
//===----------------------------------------------------------------------===//
#include "X86IcedScan.h"

#include "arch/X86/X86ScanAdapter.h"
#include "iced_capstone.h"

#include <iterator>
namespace neverd::x86iced {
namespace {
#include "X86IcedIds.inc"
static_assert(ICED_CAPSTONE_ABI_VERSION == 1);
static_assert(sizeof(iced_capstone_instruction) == 120);
static_assert(offsetof(iced_capstone_instruction, length) == 108);
} // namespace
Scanner::Scanner(unsigned Bits)
    : Bitness(Bits), State(iced_capstone_create(Bits)) {}
Scanner::~Scanner() { iced_capstone_destroy(State); }
bool Scanner::attach(csh Handle) {
  return (Bitness == 32 || Bitness == 64) && State &&
         X86_setScanAdapter(Handle, Bitness == 64 ? CS_MODE_64 : CS_MODE_32,
                            scanCallback, this);
}
void Scanner::detach(csh Handle) {
  X86_setScanAdapter(Handle, CS_MODE_LITTLE_ENDIAN, nullptr, nullptr);
}
uint32_t Scanner::scanCallback(void *Context, const uint8_t *Bytes,
                               size_t Len) {
  return static_cast<Scanner *>(Context)->scan(Bytes, Len);
}
uint32_t Scanner::scan(const uint8_t *Bytes, size_t Len) {
  const unsigned Bits = Bitness;
  constexpr size_t MaxInstructionBytes = 15; // x86 architectural encoding limit
  if ((Bits != 32 && Bits != 64) || !State || !isCandidate(Bytes, Len))
    return 0;
  if (Len > MaxInstructionBytes)
    Len = MaxInstructionBytes;
  const uint32_t Result = iced_capstone_scan_with_state(State, Bytes, Len);
  const uint32_t Code = Result >> 8;
  const unsigned Size = Result & 0xff;
  if (!Size || Size > Len || Code >= std::size(InstructionIds) ||
      !InstructionIds[Code])
    return 0;
  return (InstructionIds[Code] << 8) | Size;
}
} // namespace neverd::x86iced
