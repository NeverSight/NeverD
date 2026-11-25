//===- RISCVIndexedLoadFixture.inc - Linked RISC-V table reads -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Encodings assembled with llvm-mc, including a negative low displacement.
///
//===----------------------------------------------------------------------===//

namespace indexed_load {
constexpr va_t TableVA = 0x20ff0;
constexpr unsigned TableOffset = 0x1f0;
constexpr unsigned TableBytes = 32;
constexpr uint32_t Mask31 = 0x01f57313;       // andi t1,a0,31
constexpr uint32_t Mask63 = 0x03f57313;       // andi t1,a0,63
constexpr uint32_t Shift = 0x01b55313;        // srli t1,a0,27
constexpr uint32_t ShiftWord = 0x01b5531b;    // srliw t1,a0,27
constexpr uint32_t SignedShift = 0x41b55313;  // srai t1,a0,27
constexpr uint32_t Argument = 0x00050313;     // mv t1,a0
constexpr uint32_t BaseFirst = 0x006283b3;    // add t2,t0,t1
constexpr uint32_t BaseLast = 0x005303b3;     // add t2,t1,t0
constexpr uint32_t LoadByte = 0x0003c503;     // lbu a0,0(t2)
constexpr uint32_t LoadHalfword = 0x0003d503; // lhu a0,0(t2)

LinkedImage file(Arch A, uint32_t Index, uint32_t Add, uint32_t Load,
                 bool Sections, bool Writable) {
  LinkedImage F(A);
  F.setWords({0x000212b7, 0xff028293, Index, Add, Load, 0x00754533,
              0x00008067}); // lui/addi t0; ...; xor a0,a0,t2; ret
  F.Bytes.resize(TableOffset + TableBytes);
  for (unsigned I = 0; I < TableBytes; ++I)
    F.Bytes[TableOffset + I] = TableBytes - I;
  const unsigned P = F.Wide ? 8 : 4;
  const unsigned Header = F.Wide ? 64 + 56 : 52 + 32;
  F.put(F.Wide ? 56 : 44, 2, 2);
  F.put(Header, llvm::ELF::PT_LOAD, 4);
  F.put(Header + (F.Wide ? 4 : 24),
        llvm::ELF::PF_R | (Writable ? llvm::ELF::PF_W : 0), 4);
  F.put(Header + (F.Wide ? 8 : 4), TableOffset, P);
  F.put(Header + (F.Wide ? 16 : 8), TableVA, P);
  F.put(Header + (F.Wide ? 24 : 12), TableVA, P);
  F.put(Header + (F.Wide ? 32 : 16), TableBytes, P);
  F.put(Header + (F.Wide ? 40 : 20), TableBytes, P);
  F.put(Header + (F.Wide ? 48 : 28), 0x100, P);
  if (Sections)
    F.addSection(".rodata", llvm::ELF::SHT_PROGBITS, TableOffset, TableBytes,
                 llvm::ELF::SHF_ALLOC, TableVA);
  return F;
}
} // namespace indexed_load
