//===- X86TransactionEncoding.h - Transaction byte ownership -----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Own transaction prefixes, controls and signed fallback targets for decode,
/// lifting and explicit native source regions.
/// Operand size selects the displacement width, never the target width.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_X86_X86TRANSACTIONENCODING_H
#define NEVERD_DECODE_X86_X86TRANSACTIONENCODING_H

#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"

#include "capstone/capstone.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace neverd {
enum class X86TransactionKind { Begin, Abort, End, Test };

struct X86TransactionEncoding {
  X86TransactionKind Kind = X86TransactionKind::Begin;
  std::array<uint8_t, 4> Prefix{};
  uint8_t Rex = 0;
  unsigned OpcodeOffset = 0;
  unsigned ImmediateBytes = 0;
  uint64_t Target = 0;
  uint8_t AbortCode = 0;
};

using X86TransactionBeginEncoding = X86TransactionEncoding;

inline std::optional<X86TransactionEncoding>
x86TransactionEncoding(std::span<const uint8_t> Bytes, uint64_t Address,
                       bool Mode64) {
  if (Bytes.size() < 3 || Bytes.size() > x86::kMaxInstructionLen)
    return std::nullopt;
  X86TransactionEncoding E;
  for (; E.OpcodeOffset < Bytes.size(); ++E.OpcodeOffset) {
    const auto Byte = Bytes[E.OpcodeOffset];
    if (Mode64 && Byte >= 0x40 && Byte <= 0x4f) {
      E.Rex = Byte;
      continue;
    }
    if (Byte == 0xf2 || Byte == 0xf3)
      E.Prefix[0] = Byte;
    else if (Byte == 0x66)
      E.Prefix[2] = Byte;
    else if (Byte == 0x67)
      E.Prefix[3] = Byte;
    else if (Byte == 0x26 || Byte == 0x2e || Byte == 0x36 || Byte == 0x3e ||
             Byte == 0x64 || Byte == 0x65)
      E.Prefix[1] = Byte;
    else
      break;
    E.Rex = 0;
  }
  if (E.OpcodeOffset + 3 > Bytes.size())
    return std::nullopt;
  const auto Opcode = Bytes[E.OpcodeOffset];
  const auto ModRM = Bytes[E.OpcodeOffset + 1];
  if (Opcode != 0xc7) {
    if (E.OpcodeOffset + 3 != Bytes.size())
      return std::nullopt;
    if (Opcode == 0xc6 && ModRM == 0xf8) {
      E.Kind = X86TransactionKind::Abort;
      E.AbortCode = Bytes.back();
      E.ImmediateBytes = 1;
    } else if (Opcode == 0x0f && ModRM == 0x01 &&
               (Bytes.back() == 0xd5 || Bytes.back() == 0xd6)) {
      // NP in the Intel opcode table forbids 66/F2/F3 on XEND/XTEST.
      // Capstone still names these forms, but canonicalizing them can turn
      // an architectural transaction abort into a successful commit.
      if (E.Prefix[0] || E.Prefix[2])
        return std::nullopt;
      E.Kind = Bytes.back() == 0xd5 ? X86TransactionKind::End
                                    : X86TransactionKind::Test;
    } else {
      return std::nullopt;
    }
    return E;
  }
  E.ImmediateBytes = E.Prefix[2] && !(E.Rex & 8) ? 2 : 4;
  if (E.OpcodeOffset + 2 + E.ImmediateBytes != Bytes.size() ||
      Bytes[E.OpcodeOffset + 1] != 0xf8)
    return std::nullopt;
  const auto *Immediate = Bytes.data() + E.OpcodeOffset + 2;
  const int64_t Relative =
      E.ImmediateBytes == 2
          ? int64_t(static_cast<int16_t>(readLE<uint16_t>(Immediate)))
          : int64_t(static_cast<int32_t>(readLE<uint32_t>(Immediate)));
  E.Target = (Address + Bytes.size() + uint64_t(Relative)) &
             (Mode64 ? UINT64_MAX : UINT32_MAX);
  return E;
}

inline std::optional<X86TransactionBeginEncoding>
x86TransactionBeginEncoding(std::span<const uint8_t> Bytes, uint64_t Address,
                            bool Mode64) {
  auto E = x86TransactionEncoding(Bytes, Address, Mode64);
  return E && E->Kind == X86TransactionKind::Begin ? E : std::nullopt;
}

inline bool x86TransactionBeginDetailMatches(
    const cs_x86 &X, const X86TransactionBeginEncoding &E, bool Mode64) {
  // Capstone's relative operand carrier is 32 bits for the rel16 and REX.W
  // variants in long mode, and 64 bits for the ordinary rel32 variant. The
  // signed fallback calculation above is independent of that carrier size.
  const unsigned OperandBytes =
      Mode64 ? (E.ImmediateBytes == 2 || (E.Rex & 8) ? 4 : 8)
             : E.ImmediateBytes;
  return X.op_count == 1 && X.operands[0].type == X86_OP_IMM &&
         X.operands[0].size == OperandBytes && !X.operands[0].avx_zero_opmask &&
         X.operands[0].avx_bcast == X86_AVX_BCAST_INVALID && !X.eflags &&
         !X.rex2 && !X.avx_sae && X.avx_rm == X86_AVX_RM_INVALID &&
         (!X.prefix[0] || X.prefix[0] == E.Prefix[0]) &&
         X.prefix[1] == E.Prefix[1] && X.prefix[2] == E.Prefix[2] &&
         X.prefix[3] == E.Prefix[3] && X.rex == E.Rex &&
         X.addr_size ==
             (Mode64 ? (E.Prefix[3] ? 4 : 8) : (E.Prefix[3] ? 2 : 4)) &&
         X.opcode[0] == 0xc7 && !X.opcode[1] && !X.opcode[2] && !X.opcode[3] &&
         X.modrm == 0xf8 && X.encoding.modrm_offset == E.OpcodeOffset + 1 &&
         !X.encoding.disp_size && !X.encoding.disp_offset &&
         X.encoding.imm_offset == E.OpcodeOffset + 2 &&
         X.encoding.imm_size == E.ImmediateBytes;
}
inline bool x86TransactionControlDetailMatches(const cs_x86 &X,
                                               const X86TransactionEncoding &E,
                                               bool Mode64) {
  if (E.Kind == X86TransactionKind::Begin)
    return false;
  // Capstone drops ignored repeat prefixes from its normalized prefix slot.
  if ((X.prefix[0] && X.prefix[0] != E.Prefix[0]) || X.rex != E.Rex ||
      X.prefix[1] != E.Prefix[1] || X.prefix[2] != E.Prefix[2] ||
      X.prefix[3] != E.Prefix[3] || X.opcode[2] || X.opcode[3] ||
      X.encoding.disp_size || X.encoding.disp_offset ||
      X.addr_size != (Mode64 ? (E.Prefix[3] ? 4 : 8) : (E.Prefix[3] ? 2 : 4)))
    return false;
  if (E.Kind == X86TransactionKind::Abort) {
    if (X.op_count != 1 || X.operands[0].type != X86_OP_IMM ||
        (X.operands[0].size != 1 && X.operands[0].size != 2 &&
         X.operands[0].size != 4 && X.operands[0].size != 8) ||
        X.opcode[0] != 0xc6 || X.opcode[1] || X.modrm != 0xf8 ||
        X.encoding.modrm_offset != E.OpcodeOffset + 1 ||
        X.encoding.imm_size != 1 || X.encoding.imm_offset != E.OpcodeOffset + 2)
      return false;
    return X.operands[0].imm == E.AbortCode ||
           X.operands[0].imm == static_cast<int8_t>(E.AbortCode);
  }
  const uint8_t ModRM = E.Kind == X86TransactionKind::End ? 0xd5 : 0xd6;
  return !X.op_count && X.opcode[0] == 0x0f && X.opcode[1] == 0x01 &&
         X.modrm == ModRM && X.encoding.modrm_offset == E.OpcodeOffset + 2 &&
         !X.encoding.imm_size && !X.encoding.imm_offset;
}
} // namespace neverd

#endif // NEVERD_DECODE_X86_X86TRANSACTIONENCODING_H
