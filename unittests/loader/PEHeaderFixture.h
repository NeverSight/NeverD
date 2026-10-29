//===- PEHeaderFixture.h - PE header fixtures -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authored linked PE images with original, file-backed headers.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LOADER_PEHEADERFIXTURE_H
#define NEVERD_UNITTESTS_LOADER_PEHEADERFIXTURE_H

#include "../web/NativeFixture.h"

#include "neverd/object/PELayout.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace neverd::test {
using namespace llvm::COFF;
using namespace llvm::object;

inline std::string peHeaderImage(bool Is64, uint16_t Machine,
                                 bool LongStub = false) {
  auto Bytes = LongStub ? web::test::nativePELongStub() : web::test::nativePE();
  auto Headers =
      locatePEHeaders(reinterpret_cast<uint8_t *>(Bytes.data()), Bytes.size());
  Headers.FileHeader->Machine = Machine;
  if (!Is64) {
    // Retain the declared optional-header size and section-table position.
    // Only the format-native fields differ between PE32 and PE32+.
    const auto Old = *getPE32PlusOptionalHeader(Headers);
    auto *PE = reinterpret_cast<pe32_header *>(Headers.OptionalHeader);
    std::memset(Headers.OptionalHeader, 0,
                Headers.FileHeader->SizeOfOptionalHeader);
    PE->Magic = PE32Header::PE32;
    PE->ImageBase = 0x400000;
    PE->AddressOfEntryPoint = Old.AddressOfEntryPoint;
    PE->BaseOfCode = Old.BaseOfCode;
    PE->SectionAlignment = Old.SectionAlignment;
    PE->FileAlignment = Old.FileAlignment;
    PE->SizeOfImage = Old.SizeOfImage;
    PE->SizeOfHeaders = Old.SizeOfHeaders;
    PE->NumberOfRvaAndSize = 16;
  }
  return Bytes;
}

/// A linked image whose entry is `_ValidateImageBase(&__ImageBase)` as a CRT
/// inlines it: the NT headers, `&__ImageBase + e_lfanew`, when `e_magic` is
/// "MZ", else zero.
struct PEImageBaseWalker {
  std::string Bytes;
  uint64_t Base = 0;
  uint64_t Entry = 0;
  /// The target to compile its C for, and the object symbol that C's
  /// `__ImageBase` links as there.
  const char *Triple = "";
  const char *Symbol = "";
};

/// \p Machine is x86, x64 or AArch64.  \p Unmapped adds an unmodeled loader
/// protocol (a GLOBAL_PTR directory) that leaves the headers unmapped, as an
/// MSVC extended load configuration does.  On x86, base relocations in a
/// `.reloc` section name the code's absolute header addresses unless
/// \p Fixed: such an image runs at its link address.
inline PEImageBaseWalker peImageBaseWalker(uint16_t Machine, bool Unmapped,
                                           bool Fixed = false) {
  PEImageBaseWalker W;
  const bool X86 = Machine == IMAGE_FILE_MACHINE_I386;
  W.Bytes = peHeaderImage(!X86, Machine);
  std::string Code;
  if (Machine == IMAGE_FILE_MACHINE_AMD64) {
    W.Triple = "x86_64-pc-windows-msvc";
    W.Symbol = "__ImageBase";
    // lea rax, [rip - 0x1007]; movzx ecx, word ptr [rax]; cmp ecx, 0x5a4d;
    // jne 1f; movsxd rcx, dword ptr [rip - 0xfdd]; add rax, rcx; ret;
    // 1: xor eax, eax; ret
    Code = std::string("\x48\x8d\x05\xf9\xef\xff\xff\x0f\xb7\x08\x81\xf9\x4d"
                       "\x5a\x00\x00\x75\x0b\x48\x63\x0d\x23\xf0\xff\xff\x48"
                       "\x01\xc8\xc3\x31\xc0\xc3",
                       32);
  } else if (Machine == IMAGE_FILE_MACHINE_ARM64) {
    W.Triple = "aarch64-pc-windows-msvc";
    W.Symbol = "__ImageBase";
    // adrp x8, base; add x8, x8, #0; ldrh w9, [x8]; mov w10, #0x5a4d;
    // cmp w9, w10; b.ne 1f; ldrsw x9, [x8, #0x3c]; add x0, x8, x9; ret;
    // 1: mov x0, #0; ret
    Code = std::string("\xe8\xff\xff\xf0\x08\x01\x00\x91\x09\x01\x40\x79"
                       "\xaa\x49\x8b\x52\x3f\x01\x0a\x6b\x81\x00\x00\x54"
                       "\x09\x3d\x80\xb9\x00\x01\x09\x8b\xc0\x03\x5f\xd6"
                       "\x00\x00\x80\xd2\xc0\x03\x5f\xd6",
                       44);
  } else {
    W.Triple = "i686-pc-windows-msvc";
    W.Symbol = "___ImageBase";
    // cmp word ptr [0x400000], 0x5a4d; jne 1f; mov eax, [0x40003c];
    // lea eax, [eax + 0x400000]; ret; 1: xor eax, eax; ret
    Code = std::string("\x66\x81\x3d\x00\x00\x40\x00\x4d\x5a\x75\x0c\xa1\x3c"
                       "\x00\x40\x00\x8d\x80\x00\x00\x40\x00\xc3\x31\xc0\xc3",
                       26);
  }
  const bool Relocated = X86 && !Fixed;
  constexpr uint32_t RelocRVA = 0x2000, RelocFile = 0x400, RelocBlock = 16;
  if (Relocated)
    W.Bytes.resize(RelocFile + 0x200, '\0');
  auto *Data = reinterpret_cast<uint8_t *>(W.Bytes.data());
  auto PE = locatePEHeaders(Data, W.Bytes.size());
  W.Base = getPEImageBase(PE);
  W.Entry = W.Base + getPEAddressOfEntryPoint(PE);
  auto *Text = reinterpret_cast<coff_section *>(PE.SectionTable);
  Text->VirtualSize = 0x40;
  std::fill_n(W.Bytes.begin() + Text->PointerToRawData, Text->SizeOfRawData,
              '\0');
  W.Bytes.replace(Text->PointerToRawData, Code.size(), Code);
  if (Relocated) {
    // HIGHLOW fixups of the base, of e_lfanew and of the base the NT
    // headers' offset is added to.
    PE.FileHeader->NumberOfSections = 2;
    coff_section &Reloc = Text[1];
    std::memcpy(Reloc.Name, ".reloc", 6);
    Reloc.VirtualSize = RelocBlock;
    Reloc.VirtualAddress = RelocRVA;
    Reloc.SizeOfRawData = 0x200;
    Reloc.PointerToRawData = RelocFile;
    Reloc.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA |
                            IMAGE_SCN_MEM_DISCARDABLE | IMAGE_SCN_MEM_READ;
    setPESizeOfImage(PE, RelocRVA + 0x1000);
    setPEDataDirectory(PE, BASE_RELOCATION_TABLE, RelocRVA, RelocBlock);
    writeLE<uint32_t>(Data + RelocFile, Text->VirtualAddress);
    writeLE<uint32_t>(Data + RelocFile + 4, RelocBlock);
    const uint16_t Fields[] = {0x003, 0x00c, 0x012};
    for (size_t I = 0; I < std::size(Fields); ++I)
      writeLE<uint16_t>(Data + RelocFile + 8 + 2 * I,
                        uint16_t(IMAGE_REL_BASED_HIGHLOW << 12) | Fields[I]);
  }
  if (Unmapped)
    setPEDataDirectory(PE, GLOBAL_PTR, Text->VirtualAddress, 4);
  return W;
}

} // namespace neverd::test

#endif
