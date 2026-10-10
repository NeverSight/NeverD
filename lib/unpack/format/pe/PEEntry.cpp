//===- PEEntry.cpp - Preserve DLL loader notification dispatch ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "PEImage.h"

#include "neverd/emulation/GuestMemory.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace neverd::unpack::pe {
llvm::Expected<uint64_t> rebuildEntry(const Image &In, const Capture &C,
                                      uint64_t MetadataRVA,
                                      std::vector<uint8_t> &Metadata) {
  using namespace llvm::support::endian;
  if (In.domain() == ExecutionDomain::Kernel)
    return C.EntryRVA;
  llvm::object::coff_file_header Header;
  std::memcpy(&Header, In.file().data() + In.headers().FileHeaderOffset,
              sizeof(Header));
  if (!(Header.Characteristics & llvm::COFF::IMAGE_FILE_DLL) ||
      C.EntryRVA == In.entryRVA())
    return C.EntryRVA;

  // The selected transfer witnesses process attach only. The PE's original
  // entry still owns detach and thread notifications, including any wrapper
  // cleanup outside the recovered function. Its live bytes are the bytes
  // the original process would execute at this stopped boundary.
  const uint64_t Original = In.entryRVA();
  if (!Original || !In.regionAt(Original) ||
      Original / unpack::value::PageSize >= C.PageAccess.size() ||
      !(C.PageAccess[Original / unpack::value::PageSize] & emulation::Execute))
    return failure(text::DLLNotificationEntry);
  const bool X64 = In.architecture() == emulation::GuestArchitecture::X64;
  if (!X64 && Original % sizeof(uint32_t))
    return failure(text::DLLNotificationEntry);
  const uint64_t Start = llvm::alignTo(Metadata.size(), uint64_t(16));
  const uint64_t Size = X64 ? 33 : 40;
  if (MetadataRVA > UINT32_MAX || Start > UINT32_MAX - MetadataRVA ||
      Size > UINT32_MAX - MetadataRVA - Start)
    return failure(text::ImageSize);
  Metadata.resize(Start + Size, 0);
  uint8_t *Code = Metadata.data() + Start;
  if (X64) {
    // CMP EDX, DLL_PROCESS_ATTACH; JNE original; JMP [RIP]; selected;
    // original: JMP [RIP]; original. Arguments, stack and return PC survive.
    constexpr uint8_t Dispatch[] = {0x83, 0xfa, 1, 0x75, 14, 0xff,
                                    0x25, 0,    0, 0,    0};
    std::copy(std::begin(Dispatch), std::end(Dispatch), Code);
    write64le(Code + 11, C.Base + C.EntryRVA);
    Code[19] = 0xff;
    Code[20] = 0x25;
    write64le(Code + 25, C.Base + Original);
  } else {
    constexpr uint32_t Dispatch[] = {
        0x7100043f, // cmp w1, #DLL_PROCESS_ATTACH
        0x54000061, // b.ne original
        0x58000090, // ldr x16, selected
        0xd61f0200, // br x16
        0x58000090, // original: ldr x16, original target
        0xd61f0200, // br x16
    };
    for (unsigned I = 0; I < std::size(Dispatch); ++I)
      write32le(Code + I * sizeof(uint32_t), Dispatch[I]);
    write64le(Code + 24, C.Base + C.EntryRVA);
    write64le(Code + 32, C.Base + Original);
  }
  return MetadataRVA + Start;
}
} // namespace neverd::unpack::pe
