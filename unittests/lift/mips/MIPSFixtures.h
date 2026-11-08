//===- MIPSFixtures.h - Independent MIPS ELF test bytes ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Linked ELF fixtures encoded independently of the loader and host compiler.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_MIPS_MIPSFIXTURES_H
#define NEVERD_UNITTESTS_LIFT_MIPS_MIPSFIXTURES_H

#include "neverd/ArchSupport.h"

#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

namespace neverd::mips_test {

struct ELFBytes {
  bool Is64;
  bool Little;
  std::vector<uint8_t> Bytes = std::vector<uint8_t>(0x120);

  void put(size_t Offset, uint64_t Value, size_t Width) {
    for (size_t I = 0; I < Width; ++I)
      Bytes[Offset + I] = uint8_t(Value >> (8 * (Little ? I : Width - I - 1)));
  }

  explicit ELFBytes(Arch A) : Is64(isMIPS64(A)), Little(archLittleEndian(A)) {
    Bytes[0] = 0x7f;
    Bytes[1] = 'E';
    Bytes[2] = 'L';
    Bytes[3] = 'F';
    Bytes[4] = Is64 ? 2 : 1;
    Bytes[5] = Little ? 1 : 2;
    Bytes[6] = 1;
    put(16, 2, 2); // ET_EXEC
    put(18, 8, 2); // EM_MIPS
    put(20, 1, 4);
    put(24, 0x10100, Is64 ? 8 : 4);
    put(Is64 ? 32 : 28, Is64 ? 64 : 52, Is64 ? 8 : 4);
    put(Is64 ? 48 : 36, Is64 ? 0x80000000 : 0x70001000, 4);
    put(Is64 ? 52 : 40, Is64 ? 64 : 52, 2);
    put(Is64 ? 54 : 42, Is64 ? 56 : 32, 2);
    put(Is64 ? 56 : 44, 1, 2);
    put(Is64 ? 58 : 46, Is64 ? 64 : 40, 2);
    const size_t P = Is64 ? 64 : 52;
    put(P, 1, 4); // PT_LOAD
    if (Is64) {
      put(P + 4, 7, 4);
      put(P + 16, 0x10000, 8);
      put(P + 24, 0x10000, 8);
      put(P + 32, Bytes.size(), 8);
      put(P + 40, Bytes.size(), 8);
      put(P + 48, 0x1000, 8);
    } else {
      put(P + 8, 0x10000, 4);
      put(P + 12, 0x10000, 4);
      put(P + 16, Bytes.size(), 4);
      put(P + 20, Bytes.size(), 4);
      put(P + 24, 7, 4);
      put(P + 28, 0x1000, 4);
    }
    put(0x100, 0x03e00008, 4); // jr ra
    put(0x104, 0, 4);          // nop delay slot
    for (size_t I = 0; I < 8; ++I)
      Bytes[0x110 + I] = uint8_t(0x11 * (I + 1));
  }

  void abiFlags() {
    put(Is64 ? 56 : 44, 2, 2);
    const size_t P = Is64 ? 120 : 84;
    put(P, 0x70000003, 4); // PT_MIPS_ABIFLAGS
    put(P + (Is64 ? 8 : 4), 0xe0, Is64 ? 8 : 4);
    put(P + (Is64 ? 32 : 16), 24, Is64 ? 8 : 4);
    Bytes[0xe2] = Is64 ? 64 : 32;
    Bytes[0xe3] = 2;
    Bytes[0xe4] = Is64 ? 2 : 1;
    Bytes[0xe7] = 3; // soft-float
  }

  void dynamicSymbols(uint8_t Mode) {
    Bytes.resize(0x240);
    const size_t Load = Is64 ? 64 : 52;
    put(Load + (Is64 ? 32 : 16), Bytes.size(), Is64 ? 8 : 4);
    put(Load + (Is64 ? 40 : 20), Bytes.size(), Is64 ? 8 : 4);
    put(Is64 ? 56 : 44, 2, 2);
    const size_t Program = Is64 ? 120 : 84;
    const size_t Width = Is64 ? 8 : 4;
    put(Program, 2, 4); // PT_DYNAMIC
    put(Program + (Is64 ? 8 : 4), 0x180, Width);
    put(Program + (Is64 ? 16 : 8), 0x10180, Width);
    put(Program + (Is64 ? 32 : 16), Width * 8, Width);
    put(Program + (Is64 ? 40 : 20), Width * 8, Width);
    put(0x180, 6, Width); // DT_SYMTAB
    put(0x180 + Width, 0x10200, Width);
    put(0x180 + Width * 2, 11, Width); // DT_SYMENT
    put(0x180 + Width * 3, Is64 ? 24 : 16, Width);
    put(0x180 + Width * 4, 0x70000011, Width); // DT_MIPS_SYMTABNO
    put(0x180 + Width * 5, 2, Width);
    const size_t Symbol = 0x200 + (Is64 ? 24 : 16);
    Bytes[Symbol + (Is64 ? 5 : 13)] = Mode;
  }

  void dynamicGOT() {
    dynamicSymbols(0);
    Bytes.resize(0x400);
    const size_t Width = Is64 ? 8 : 4;
    const size_t Load = Is64 ? 64 : 52;
    put(Load + (Is64 ? 32 : 16), Bytes.size(), Width);
    put(Load + (Is64 ? 40 : 20), Bytes.size(), Width);
    const size_t Program = Is64 ? 120 : 84;
    put(Program + (Is64 ? 32 : 16), Width * 18, Width);
    put(Program + (Is64 ? 40 : 20), Width * 18, Width);
    const std::pair<uint64_t, uint64_t> Tags[] = {
        {6, 0x10300}, {11, Is64 ? 24u : 16u}, {0x70000011, 2}, {5, 0x10340},
        {10, 16},     {3, 0x10380},           {0x7000000a, 2}, {0x70000013, 1},
        {0, 0}};
    for (size_t I = 0; I < std::size(Tags); ++I) {
      put(0x180 + I * Width * 2, Tags[I].first, Width);
      put(0x180 + I * Width * 2 + Width, Tags[I].second, Width);
    }
    const size_t Symbol = 0x300 + (Is64 ? 24 : 16);
    put(Symbol, 1, 4);
    Bytes[Symbol + (Is64 ? 4 : 12)] = 0x12; // global function, undefined
    const char Names[] = "\0fixture_import\0";
    std::copy(std::begin(Names), std::end(Names), Bytes.begin() + 0x340);
    // Pointer bindings live in data, not in an executable import veneer.
    put(Is64 ? 56 : 44, 3, 2);
    put(Load + (Is64 ? 4 : 24), 5, 4);
    put(Load + (Is64 ? 32 : 16), 0x300, Width);
    put(Load + (Is64 ? 40 : 20), 0x300, Width);
    const size_t Data = Is64 ? 176 : 116;
    put(Data, 1, 4);
    put(Data + (Is64 ? 4 : 24), 6, 4);
    put(Data + (Is64 ? 8 : 4), 0x300, Width);
    put(Data + (Is64 ? 16 : 8), 0x10300, Width);
    put(Data + (Is64 ? 24 : 12), 0x10300, Width);
    put(Data + (Is64 ? 32 : 16), 0x100, Width);
    put(Data + (Is64 ? 40 : 20), 0x100, Width);
    put(Data + (Is64 ? 48 : 28), 0x100, Width);
  }

  void runtimeRelocation(unsigned Type, bool Defined) {
    dynamicGOT();
    const unsigned Width = Is64 ? 8 : 4;
    const size_t Dynamic = Is64 ? 120 : 84;
    const std::pair<uint64_t, uint64_t> Tags[] = {
        {17, 0x103a0}, {18, Width * 2}, {19, Width * 2}, {0, 0}};
    for (unsigned I = 0; I < 4; ++I) {
      put(0x180 + (16 + 2 * I) * Width, Tags[I].first, Width);
      put(0x180 + (17 + 2 * I) * Width, Tags[I].second, Width);
    }
    put(Dynamic + (Is64 ? 32 : 16), 24 * Width, Width);
    put(Dynamic + (Is64 ? 40 : 20), 24 * Width, Width);
    put(0x3a0, 0x103c0, Width);
    if (Is64) {
      put(0x3a8, 1, 4);
      Bytes[0x3af] = Type;
    } else
      put(0x3a4, (1u << 8) | Type, 4);
    if (Defined) {
      const size_t Symbol = 0x300 + (Is64 ? 24 : 16);
      put(Symbol + (Is64 ? 6 : 14), 1, 2);
      put(Symbol + (Is64 ? 8 : 4), 0x103c0, Width);
      put(Symbol + (Is64 ? 16 : 8), 4, Width);
      Bytes[Symbol + (Is64 ? 4 : 12)] = 0x11; // STB_GLOBAL/STT_OBJECT
      put(0x180 + 15 * Width, 2, Width);      // no global GOT suffix
    }
  }

  llvm::MemoryBufferRef buffer() const {
    return {llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                            Bytes.size()),
            "owned-mips-fixture.elf"};
  }
};

} // namespace neverd::mips_test

#endif // NEVERD_UNITTESTS_LIFT_MIPS_MIPSFIXTURES_H
