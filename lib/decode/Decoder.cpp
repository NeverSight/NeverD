//===- Decoder.cpp - Capstone-based instruction decoder -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Binary instruction decoder and function detection.
///
//===----------------------------------------------------------------------===//

#include "neverd/decode/Decoder.h"

#include "neverd/Support/Diagnostic.h"
#include "neverd/decode/AArch64NativeDecode.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"
#include "neverd/lift/ARMLifter.h"
#include "neverd/lift/X86Lifter.h"

#include "llvm/Support/raw_ostream.h"

#include <cstring>

namespace neverd {

static const char *NdOpNames[] = {
#define ND_X_NAME(name) #name,
    ND_OP_LIST(ND_X_NAME)
#undef ND_X_NAME
};

static_assert(sizeof(NdOpNames) / sizeof(NdOpNames[0]) ==
                  static_cast<size_t>(NdOp::_COUNT),
              "NdOpNames must list every NdOp enumerator");

const char *ndOpName(NdOp Op) {
  auto Idx = static_cast<int>(Op);
  if (Idx >= 0 && Idx < static_cast<int>(NdOp::_COUNT))
    return NdOpNames[Idx];
  return "???";
}

Decoder::Decoder() = default;

Decoder::~Decoder() {
  if (InsnBuf)
    cs_free(InsnBuf, 1);
  if (Handle)
    cs_close(&Handle);
}

// The parallel phases each build a per-thread Decoder, so a failure here can be
// reported from several worker threads at once — hence the serialized writes.
bool Decoder::init(Arch TheArch, InstructionMode Mode) {
  cs_arch CsArch;
  cs_mode CsMode;
  std::unique_ptr<X86Lifter> NewX86;
  std::unique_ptr<ARMLifter> NewARM;
  std::unique_ptr<AArch64Lifter> NewAArch64;

  switch (TheArch) {
  case Arch::X64:
  case Arch::X86:
    CsArch = CS_ARCH_X86;
    CsMode = (TheArch == Arch::X64) ? CS_MODE_64 : CS_MODE_32;
    NewX86 = std::make_unique<X86Lifter>(TheArch);
    NewX86->setStrict(Strict);
    break;
  case Arch::AArch64:
    CsArch = CS_ARCH_AARCH64;
    CsMode = CS_MODE_ARM;
    NewAArch64 = std::make_unique<AArch64Lifter>(TheArch);
    NewAArch64->setStrict(Strict);
    break;
  case Arch::ARM:
    CsArch = CS_ARCH_ARM;
    CsMode = static_cast<cs_mode>(
        (Mode == InstructionMode::Thumb ? CS_MODE_THUMB : CS_MODE_ARM) |
        CS_MODE_V8);
    NewARM = std::make_unique<ARMLifter>(TheArch);
    NewARM->setStrict(Strict);
    break;
  default:
    syncError() << "unsupported arch for decoder\n";
    return false;
  }

  csh NewHandle = 0;
  if (cs_open(CsArch, CsMode, &NewHandle) != CS_ERR_OK) {
    syncError() << "failed to init capstone\n";
    return false;
  }

  cs_option(NewHandle, CS_OPT_DETAIL, CS_OPT_ON);
  cs_insn *NewInsnBuf = cs_malloc(NewHandle);
  if (!NewInsnBuf) {
    cs_close(&NewHandle);
    syncError() << "failed to allocate capstone insn buffer\n";
    return false;
  }

  if (InsnBuf)
    cs_free(InsnBuf, 1);
  if (Handle)
    cs_close(&Handle);

  Handle = NewHandle;
  InsnBuf = NewInsnBuf;
  TargetArch = TheArch;
  Detail = true;
  X86 = std::move(NewX86);
  ARM = std::move(NewARM);
  AArch64 = std::move(NewAArch64);
  return true;
}

void Decoder::setStrict(bool S) {
  Strict = S;
  if (X86)
    X86->setStrict(S);
  else if (AArch64)
    AArch64->setStrict(S);
  else if (ARM)
    ARM->setStrict(S);
}

int Decoder::decodeOne(const uint8_t *Bytes, size_t Len, va_t Addr,
                       DecodedInsn &Out) {
  const uint8_t *Code = Bytes;
  size_t Sz = Len;
  uint64_t A = Addr;

  if (!cs_disasm_iter(Handle, &Code, &Sz, &A, InsnBuf))
    return 0;

  fixupDecodedInsn(InsnBuf);

  Out.Addr = InsnBuf->address;
  Out.Size = static_cast<uint16_t>(InsnBuf->size);
  Out.Id = InsnBuf->id;
  Out.Raw = InsnBuf;
  // The lift path and CFG classification read the mnemonic (when they need it
  // at all, e.g. x86 push/pop/VCMP width, ARM push/pop, NEON lane) directly
  // from Out.Raw->mnemonic, which capstone already filled.  Copying it into a
  // fixed buffer on every decode was pure hot-path overhead; the SDK
  // disasm/query helpers that render the text also read Out.Raw->mnemonic.
  return Out.Size;
}

int Decoder::decodeOneForLift(const uint8_t *Bytes, size_t Len, va_t Addr,
                              DecodedInsn &Out) {
  // AArch64 is fixed 4-byte width and 4-byte aligned; the common instruction
  // classes decode with a few mask+shift extractions straight into InsnBuf,
  // bypassing Capstone's DFA (the front-end throughput ceiling).  The native
  // decoder is a strict subset of Capstone's accept set and lifts identically
  // (locked by AArch64_NativeDecodeParityTests); anything it declines falls
  // back to the full Capstone decode below.  Detail must be on — the native
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
      return Out.Size;
    }
  }
  return decodeOne(Bytes, Len, Addr, Out);
}

int Decoder::decodeOneLight(const uint8_t *Bytes, size_t Len, va_t Addr,
                            DecodedInsn &Out) {
  const uint8_t *Code = Bytes;
  size_t Sz = Len;
  uint64_t A = Addr;

  if (!cs_disasm_iter(Handle, &Code, &Sz, &A, InsnBuf))
    return 0;

  // No fixupDecodedInsn here: the id fixups (x86 cmp/vex-cmp) read cs_detail,
  // which is absent when detail is disabled, and they only matter for lifting,
  // never for size stepping or terminator classification.  The mnemonic string
  // is likewise skipped — classification reads Id, not the text.
  Out.Addr = InsnBuf->address;
  Out.Size = static_cast<uint16_t>(InsnBuf->size);
  Out.Id = InsnBuf->id;
  Out.Raw = InsnBuf;
  return Out.Size;
}

void Decoder::setDetail(bool On) {
  if (On == Detail || Handle == 0)
    return;
  cs_option(Handle, CS_OPT_DETAIL, On ? CS_OPT_ON : CS_OPT_OFF);
  Detail = On;
}

void Decoder::liftToLow(const DecodedInsn &Insn, std::vector<LowOp> &Ops) {
  if (X86) {
    X86->lift(Insn.Raw, Ops);
  } else if (AArch64) {
    AArch64->lift(Insn.Raw, Ops);
  } else if (ARM) {
    ARM->lift(Insn.Raw, Ops);
  } else {
    LowOp Nop;
    Nop.Opcode = NdOp::NOP;
    Nop.Addr = Insn.Addr;
    Nop.Seq = 0;
    Ops.push_back(Nop);
  }
}

int Decoder::getX86FpuTop() const { return X86 ? X86->getFpuTop() : 0; }

void Decoder::resetX86FpuState() {
  if (X86)
    X86->resetFpuState();
}

bool Decoder::x86FpuDidReset() const { return X86 && X86->fpuDidReset(); }

int Decoder::getX86RetPopBytes() const {
  return X86 ? X86->getRetPopBytes() : 0;
}

void Decoder::fixupDecodedInsn(cs_insn *I) const {
  if (!I)
    return;
  if (X86)
    X86Lifter::fixupDecodedInsn(I);
  else if (AArch64)
    AArch64Lifter::fixupDecodedInsn(I);
  else if (ARM)
    ARMLifter::fixupDecodedInsn(I);
}

bool Decoder::isFunctionTerminator(const DecodedInsn &Insn) const {
  if (!Insn.Raw)
    return false;
  if (X86)
    return X86Lifter::isFunctionTerminator(Insn.Raw);
  if (AArch64)
    return AArch64Lifter::isFunctionTerminator(Insn.Raw);
  if (ARM)
    return ARMLifter::isFunctionTerminator(Insn.Raw);
  return false;
}

va_t Decoder::directCallTarget(const DecodedInsn &Insn) const {
  if (!Insn.Raw)
    return InvalidVA;
  if (X86)
    return X86Lifter::directCallTarget(Insn.Raw);
  if (AArch64)
    return AArch64Lifter::directCallTarget(Insn.Raw);
  if (ARM)
    return ARMLifter::directCallTarget(Insn.Raw);
  return InvalidVA;
}

va_t Decoder::pcRelCodeRefTarget(const DecodedInsn &Insn) const {
  if (!Insn.Raw)
    return InvalidVA;
  // Only x86/x64 has the relocation-free same-section `lea rip` address-of
  // form; AArch64/ARM materialize code addresses through relocations the loader
  // already records (adrp+add, GOTOFF, literal pool).
  if (X86)
    return X86Lifter::pcRelCodeRefTarget(Insn.Raw);
  return InvalidVA;
}

} // namespace neverd
