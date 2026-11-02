//===- AArch64Decoder.cpp - AArch64 decoder support ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Native AArch64 decoding and scalar relocation handling for lifting.
///
//===----------------------------------------------------------------------===//

#include "neverd/decode/AArch64/AArch64NativeDecode.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/AArch64/AArch64SVEPrefix.h"
#include "neverd/lift/AArch64/AArch64Lifter.h"
#include "neverd/support/BinaryEncoding.h"

#include <atomic>
#include <cstdint>
#include <limits>

namespace neverd {

uint64_t Decoder::nextAArch64DecodeIdentity() {
  // Allocate once per Decoder, not per instruction: independent decoder
  // threads must not contend on a global counter along the decode hot path.
  static std::atomic<uint64_t> LastIdentity{0};
  uint64_t Identity = LastIdentity.load(std::memory_order_relaxed);
  while (Identity != std::numeric_limits<uint64_t>::max())
    if (LastIdentity.compare_exchange_weak(Identity, Identity + 1,
                                           std::memory_order_relaxed))
      return Identity + 1;
  return 0;
}

bool Decoder::advanceAArch64DecodeGeneration() {
  const auto Exhausted = std::numeric_limits<uint64_t>::max();
  if (AArch64DecodeGeneration != Exhausted)
    ++AArch64DecodeGeneration;
  return AArch64DecodeIdentity && AArch64DecodeGeneration != Exhausted;
}

bool Decoder::beginDecode(DecodedInsn &Out) {
  Out.AArch64Prefix.reset();
  Out.AArch64Occurrence.reset();
  if (!AArch64)
    return true;
  Out.Raw = nullptr;
  return advanceAArch64DecodeGeneration();
}

void Decoder::recordAArch64Decode(const uint8_t *Bytes, size_t Len,
                                  DecodedInsn &Out,
                                  bool HasOperandDetail) const {
  using namespace aarch64_sve;
  Out.AArch64Prefix.reset();
  Out.AArch64Occurrence.reset();
  // Capstone may restore its old detail allocation without populating it.
  // Native decoding, however, supplies complete operands even without text.
  if (!Out.Raw || Out.Size != sizeof(uint32_t) || Len < sizeof(uint32_t))
    return;
  Out.AArch64Occurrence =
      AArch64DecodeOccurrence{Out.Raw,
                              Out.Raw->detail,
                              AArch64DecodeIdentity,
                              AArch64DecodeGeneration,
                              readLE<uint32_t>(Bytes),
                              HasOperandDetail && Out.Raw->detail};
  if (Out.AArch64Occurrence->HasOperandDetail &&
      Out.Id == AARCH64_INS_MOVPRFX && Len >= kPrefixPairBytes)
    Out.AArch64Prefix =
        AArch64PrefixWords{readLE<uint32_t>(Bytes),
                           readLE<uint32_t>(Bytes + kPrefixInstructionBytes)};
}

bool Decoder::isCurrentAArch64Decode(
    const AArch64DecodeOccurrence &Occurrence) const {
  return Occurrence.DecoderIdentity == AArch64DecodeIdentity &&
         Occurrence.Instruction == InsnBuf &&
         Occurrence.Generation == AArch64DecodeGeneration;
}

bool Decoder::hasAArch64DecodeOccurrence(const DecodedInsn &Insn) const {
  if (!AArch64 || !Insn.Raw || !Insn.AArch64Occurrence)
    return false;
  const auto &Occurrence = *Insn.AArch64Occurrence;
  // Check a borrowed instruction before dereferencing it: reset/init can have
  // freed its allocation. An owned cs_insn retains its independent raw bytes.
  if (Insn.Raw == Occurrence.Instruction && !isCurrentAArch64Decode(Occurrence))
    return false;
  return Insn.Raw->id == Insn.Id && Insn.Raw->address == Insn.Addr &&
         Insn.Raw->size == sizeof(uint32_t) && Insn.Size == Insn.Raw->size &&
         readLE<uint32_t>(Insn.Raw->bytes) == Occurrence.Word;
}

bool Decoder::hasAArch64OperandDetail(const DecodedInsn &Insn) const {
  if (!hasAArch64DecodeOccurrence(Insn) ||
      !Insn.AArch64Occurrence->HasOperandDetail || !Insn.Raw->detail)
    return false;
  const auto &Occurrence = *Insn.AArch64Occurrence;
  // Copying cs_insn alone leaves its operand allocation borrowed.
  if (Insn.Raw->detail == Occurrence.Operands &&
      !isCurrentAArch64Decode(Occurrence))
    return false;
  return Insn.Raw != Occurrence.Instruction ||
         Insn.Raw->detail == Occurrence.Operands;
}

int Decoder::decodeAArch64ForLift(const uint8_t *Bytes, size_t Len, va_t Addr,
                                  DecodedInsn &Out) {
  // AArch64 is fixed 4-byte width and 4-byte aligned; the common instruction
  // classes decode with a few mask+shift extractions straight into InsnBuf,
  // bypassing Capstone's DFA (the front-end throughput ceiling).  The native
  // decoder is a strict subset of Capstone's accept set and lifts identically
  // (locked by AArch64_NativeDecodeParityTests); anything it declines falls
  // back to the caller's full Capstone decode.  Detail must be on — the native
  // path fills operand detail the lifter reads — and InsnBuf->detail is
  // allocated whenever detail is on (see init()).
  if (AArch64 && Detail && Len >= 4 && InsnBuf && InsnBuf->detail) {
    uint32_t Word = static_cast<uint32_t>(Bytes[0]) |
                    (static_cast<uint32_t>(Bytes[1]) << 8) |
                    (static_cast<uint32_t>(Bytes[2]) << 16) |
                    (static_cast<uint32_t>(Bytes[3]) << 24);
    if (a64native::tryDecode(Word, Addr, *InsnBuf, *InsnBuf->detail)) {
      Out.Addr = InsnBuf->address;
      Out.Size = static_cast<uint16_t>(InsnBuf->size);
      Out.Id = InsnBuf->id;
      Out.Raw = InsnBuf;
      recordAArch64Decode(Bytes, Len, Out, true);
      return Out.Size;
    }
  }
  return 0;
}

void Decoder::liftAArch64ToLow(
    const DecodedInsn &Insn, std::vector<LowOp> &Ops,
    llvm::ArrayRef<RelocatedScalarOperand> ScalarRelocs) {
  bool ScalarWideImmediate = false;
  if (Insn.Raw && Insn.Raw->size == 4) {
    uint32_t Word = 0;
    for (unsigned Byte = 0; Byte != 4; ++Byte)
      Word |= uint32_t(Insn.Raw->bytes[Byte]) << (Byte * 8);
    if ((Word & 0x1f800000u) == 0x12800000u)
      for (const RelocatedScalarOperand &Scalar : ScalarRelocs)
        ScalarWideImmediate |=
            Scalar.Semantics ==
                RelocatedScalarOperand::Kind::AArch64ELFUnrelocatedWideMove &&
            Scalar.FieldVA == Insn.Raw->address && Scalar.Width == 4 &&
            Scalar.EncodedValue == Word;
  }
  std::optional<uint32_t> FollowingWord;
  if (Insn.Id == AARCH64_INS_MOVPRFX) {
    // InsnBuf is reused. Bind the snapshot to this exact address and word,
    // so copying a DecodedInsn cannot pair a later decode with old evidence.
    if (!Insn.Raw || !Insn.AArch64Prefix || Insn.Raw->id != Insn.Id ||
        Insn.Raw->address != Insn.Addr ||
        Insn.Raw->size != aarch64_sve::kPrefixInstructionBytes ||
        Insn.Size != Insn.Raw->size ||
        readLE<uint32_t>(Insn.Raw->bytes) != Insn.AArch64Prefix->Prefix)
      throw UnliftedInstruction(
          Insn.Addr, intrinsicAsmMnemonic(Intrinsic::A64_SveMovprfxZero),
          nullptr);
    FollowingWord = Insn.AArch64Prefix->Follower;
  }
  AArch64->lift(Insn.Raw, Ops, ScalarWideImmediate, FollowingWord);
}

} // namespace neverd
