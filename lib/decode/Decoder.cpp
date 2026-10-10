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

#include "neverd/decode/AArch64NativeDecode.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"
#include "neverd/lift/ARMLifter.h"
#include "neverd/lift/X86Lifter.h"
#include "neverd/support/Diagnostic.h"

#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <iterator>

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

Decoder::~Decoder() { reset(); }

void Decoder::reset() {
  if (InsnBuf)
    cs_free(InsnBuf, 1);
  if (Handle)
    cs_close(&Handle);
  InsnBuf = nullptr;
  Handle = 0;
  TargetArch = Arch::Unknown;
  CurrentMode = InstructionMode::Default;
  X86.reset();
  ARM.reset();
  AArch64.reset();
}

// The parallel phases each build a per-thread Decoder, so a failure here can be
// reported from several worker threads at once — hence the serialized writes.
bool Decoder::init(Arch TheArch, InstructionMode Mode) {
  if (!isSingleInstructionMode(Mode)) {
    syncError() << (Mode == InstructionMode::MixedARMThumb
                        ? "mixed ARM/Thumb decoding unsupported\n"
                        : "unknown instruction mode for decoder\n");
    return false;
  }
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
    NewARM = std::make_unique<ARMLifter>(TheArch, Mode);
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
  CurrentMode = Mode;
  Detail = true;
  Text = true;
  X86 = std::move(NewX86);
  ARM = std::move(NewARM);
  AArch64 = std::move(NewAArch64);
  return true;
}

void Decoder::configureFor(const BinaryImage &Img) {
  // A PE image runs on Windows, whose system service convention an x64
  // SYSCALL follows.
  if (X86)
    X86->setSyscallConvention(Img.abiFormat() == BinaryFormat::COFF
                                  ? X86Lifter::SyscallConvention::WindowsNT
                                  : X86Lifter::SyscallConvention::Linux);
}

bool Decoder::init(const BinaryImage &Img) {
  if (Img.Mode != InstructionMode::MixedARMThumb) {
    if (!init(Img.Arch, Img.Mode))
      return false;
    configureFor(Img);
    return true;
  }
  if (Img.Arch != Arch::ARM)
    return false;
  if (Img.Entry != 0)
    if (const auto EntryMode = Img.instructionModeAt(Img.Entry))
      return init(Img.Arch, *EntryMode);
  for (const auto &[Addr, Mode] : Img.ARMCodeModeEntries)
    if (Img.instructionModeAt(Addr) == Mode)
      return init(Img.Arch, Mode);
  for (const ARMCodeRegion &Region : Img.ARMCodeRegions)
    if (const auto Mode = Img.instructionModeAt(Region.Start))
      return init(Img.Arch, *Mode);
  syncError() << "mixed ARM/Thumb image has no authenticated code mode\n";
  return false;
}

bool Decoder::selectMode(const BinaryImage &Img, va_t Addr,
                         std::optional<InstructionMode> IncomingMode) {
  const auto Mode = Img.instructionModeAt(Addr, IncomingMode);
  if (!Mode)
    return false;
  if (Img.Arch == Arch::ARM &&
      (Addr % (*Mode == InstructionMode::Thumb ? 2 : 4)) != 0)
    return false;
  if (Handle && TargetArch == Img.Arch && CurrentMode == *Mode)
    return true;
  const bool WasDetail = Detail;
  const bool WasText = Text;
  if (!init(Img.Arch, *Mode))
    return false;
  setDetail(WasDetail);
  setText(WasText);
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
  if (!Handle || !InsnBuf)
    return 0;
  const uint8_t *Code = Bytes;
  size_t Sz = Len;
  uint64_t A = Addr;

  if (!cs_disasm_iter(Handle, &Code, &Sz, &A, InsnBuf) &&
      !decodePrefixedX86Fence(Bytes, Len, Addr) &&
      !decodeUnprefixedX86MpxRegisterNop(Bytes, Len, Addr))
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
  if (!Handle || !InsnBuf)
    return 0;
  const uint8_t *Code = Bytes;
  size_t Sz = Len;
  uint64_t A = Addr;

  if (!cs_disasm_iter(Handle, &Code, &Sz, &A, InsnBuf) &&
      !decodePrefixedX86Fence(Bytes, Len, Addr) &&
      !decodeUnprefixedX86MpxRegisterNop(Bytes, Len, Addr))
    return 0;

  fixupDecodedInsnId(InsnBuf);
  fixupX86DisplacementDetail(InsnBuf);
  Out.Addr = InsnBuf->address;
  Out.Size = static_cast<uint16_t>(InsnBuf->size);
  Out.Id = InsnBuf->id;
  Out.Raw = InsnBuf;
  return Out.Size;
}

bool Decoder::decodePrefixedX86Fence(const uint8_t *Bytes, size_t Len,
                                     va_t Addr) {
  if (!X86 || !Bytes || !InsnBuf)
    return false;

  size_t PrefixCount = 0;
  while (PrefixCount < Len && Bytes[PrefixCount] == 0x66)
    ++PrefixCount;
  if (PrefixCount == 0 || PrefixCount >= Len || PrefixCount >= 15)
    return false;

  const uint8_t *Code = Bytes + PrefixCount;
  size_t Remaining = Len - PrefixCount;
  uint64_t InnerAddr = Addr + PrefixCount;
  if (!cs_disasm_iter(Handle, &Code, &Remaining, &InnerAddr, InsnBuf))
    return false;
  if (InsnBuf->id != X86_INS_LFENCE && InsnBuf->id != X86_INS_MFENCE &&
      InsnBuf->id != X86_INS_SFENCE)
    return false;

  const size_t InnerSize = InsnBuf->size;
  const size_t TotalSize = PrefixCount + InnerSize;
  if (TotalSize > 15 || TotalSize > sizeof(InsnBuf->bytes))
    return false;

  std::memmove(InsnBuf->bytes + PrefixCount, InsnBuf->bytes, InnerSize);
  std::memset(InsnBuf->bytes, 0x66, PrefixCount);
  InsnBuf->address = Addr;
  InsnBuf->size = static_cast<uint16_t>(TotalSize);
  return true;
}

bool Decoder::decodeUnprefixedX86MpxRegisterNop(const uint8_t *Bytes,
                                                size_t Len, va_t Addr) {
  if (!X86 || !Bytes || !InsnBuf || Len < 3 || Bytes[0] != 0x0f ||
      (Bytes[1] != 0x1a && Bytes[1] != 0x1b) || (Bytes[2] & 0xc0) != 0xc0)
    return false;

  // cs_malloc owns the detail allocation.  Preserve its pointer while
  // replacing the failed decode result with a complete three-byte NOP.
  cs_detail *SavedDetail = InsnBuf->detail;
  std::memset(InsnBuf, 0, sizeof(*InsnBuf));
  InsnBuf->detail = SavedDetail;
  if (SavedDetail)
    std::memset(SavedDetail, 0, sizeof(*SavedDetail));

  InsnBuf->id = X86_INS_NOP;
  InsnBuf->address = Addr;
  InsnBuf->size = 3;
  std::memcpy(InsnBuf->bytes, Bytes, InsnBuf->size);
  std::memcpy(InsnBuf->mnemonic, "nop", 4);
  if (SavedDetail) {
    SavedDetail->x86.opcode[0] = Bytes[0];
    SavedDetail->x86.opcode[1] = Bytes[1];
    SavedDetail->x86.addr_size = TargetArch == Arch::X64 ? 8 : 4;
    SavedDetail->x86.modrm = Bytes[2];
    SavedDetail->x86.encoding.modrm_offset = 2;
  }
  return true;
}

void Decoder::setDetail(bool On) {
  if (On == Detail || Handle == 0)
    return;
  cs_option(Handle, CS_OPT_DETAIL, On ? CS_OPT_ON : CS_OPT_OFF);
  Detail = On;
}

void Decoder::setText(bool On) {
  if (On == Text || Handle == 0)
    return;
  cs_option(Handle, CS_OPT_TEXT, On ? CS_OPT_ON : CS_OPT_OFF);
  Text = On;
}

void Decoder::liftToLow(const DecodedInsn &Insn, std::vector<LowOp> &Ops,
                        llvm::ArrayRef<RelocatedAddressOperand> Relocs,
                        llvm::ArrayRef<RelocatedScalarOperand> ScalarRelocs,
                        LowInstructionUndefinedEffects *UndefinedEffects,
                        LowInstructionPreservedState *PreservedState) {
  const size_t OpsStart = Ops.size();
  if (PreservedState)
    *PreservedState = {};
  if (UndefinedEffects) {
    *UndefinedEffects = {};
    UndefinedEffects->Diagnostic =
        "architecture has no audited undefined-output contract";
  }
  if (X86) {
    X86->lift(Insn.Raw, Ops, Relocs, ScalarRelocs, UndefinedEffects,
              PreservedState);
  } else if (AArch64) {
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
    AArch64->lift(Insn.Raw, Ops, ScalarWideImmediate);
  } else if (ARM) {
    ARM->lift(Insn.Raw, Ops);
  } else {
    LowOp Nop;
    Nop.Opcode = NdOp::NOP;
    Nop.Addr = Insn.Addr;
    Nop.Seq = 0;
    Ops.push_back(Nop);
  }
  if (UndefinedEffects && !X86)
    UndefinedEffects->OpCount = Ops.size() - OpsStart;
}

bool Decoder::liftX64MemoryCallToLow(
    const DecodedInsn &Insn, std::vector<LowOp> &Ops,
    LowInstructionUndefinedEffects *UndefinedEffects,
    LowInstructionPreservedState *PreservedState) {
  if (PreservedState)
    *PreservedState = {};
  if (UndefinedEffects) {
    *UndefinedEffects = {};
    UndefinedEffects->Diagnostic =
        "instruction has no audited x64 memory-call projection";
  }
  return X86 && X86->liftX64MemoryCall(Insn.Raw, Ops, UndefinedEffects,
                                       PreservedState);
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

std::optional<I386GetPcOccurrence> Decoder::getX86GetPcOccurrence() const {
  return X86 ? X86->getLastGetPcOccurrence() : std::nullopt;
}

std::optional<RelocatedInstructionScalarOperandOccurrence>
Decoder::getX86ScalarOperandOccurrence() const {
  return X86 ? X86->getLastScalarOperandOccurrence() : std::nullopt;
}

void Decoder::fixupDecodedInsn(cs_insn *I) const {
  if (!I)
    return;
  fixupDecodedInsnId(I);
  fixupX86DisplacementDetail(I);
  if (X86)
    X86Lifter::fixupDecodedInsn(I);
  else if (AArch64)
    AArch64Lifter::fixupDecodedInsn(I);
  else if (ARM)
    ARMLifter::fixupDecodedInsn(I);
}

void Decoder::fixupX86DisplacementDetail(cs_insn *I) const {
  // The bundled decoder can initialize long-mode displacementSize from the
  // operand-size override, although ModRM still consumes a complete disp32.
  // Correct that metadata once, before relocation ownership and form audits
  // consume it. Do not reinterpret genuine i386 disp16, moffs, compressed
  // disp8 or an instruction decoded without populated operand detail.
  if (TargetArch != Arch::X64 || !Detail || !Text || !I || !I->detail ||
      !I->size || I->size > 15)
    return;
  auto &X = I->detail->x86;
  auto &E = X.encoding;
  if (E.disp_size != 2 || X.prefix[2] != 0x66 ||
      (X.addr_size != 4 && X.addr_size != 8) || !E.modrm_offset ||
      E.modrm_offset >= I->size || X.op_count > std::size(X.operands))
    return;
  const uint8_t ModRM = I->bytes[E.modrm_offset];
  const unsigned Mod = ModRM >> 6, RM = ModRM & 7;
  if (X.modrm != ModRM || Mod == 3 || Mod == 1)
    return;
  size_t Cursor = E.modrm_offset + 1;
  bool HasDisp32 = Mod == 2 || RM == 5;
  if (RM == 4) {
    if (Cursor >= I->size || X.sib != I->bytes[Cursor])
      return;
    HasDisp32 = Mod == 2 || (I->bytes[Cursor] & 7) == 5;
    ++Cursor;
  }
  if (!HasDisp32 || E.disp_offset != Cursor || Cursor + 4 > I->size)
    return;
  const size_t End = Cursor + 4;
  if (E.imm_size ? E.imm_offset != End || End + E.imm_size != I->size
                 : E.imm_offset != 0 || End != I->size)
    return;
  uint32_t Bits = 0;
  for (unsigned N = 0; N != 4; ++N)
    Bits |= uint32_t{I->bytes[Cursor + N]} << (N * 8);
  const int64_t Displacement = static_cast<int32_t>(Bits);
  if (X.disp != Displacement)
    return;
  bool HasMemory = false;
  for (unsigned N = 0; N != X.op_count; ++N)
    if (X.operands[N].type == X86_OP_MEM) {
      if (X.operands[N].mem.disp != Displacement)
        return;
      HasMemory = true;
    }
  if (HasMemory)
    E.disp_size = 4;
}

void Decoder::fixupDecodedInsnId(cs_insn *I) const {
  if (!X86 || !I || (I->id != X86_INS_BNDLDX && I->id != X86_INS_BNDSTX))
    return;

  // The x86 compatibility profile treats no-mandatory-prefix 0F 1A/1B as the
  // MPX-disabled form.  Retaining Capstone's BND ids would incorrectly
  // introduce a memory effect during lifting.  Mandatory-prefix forms use
  // distinct ids and remain untouched.
  I->id = X86_INS_NOP;
  std::memcpy(I->mnemonic, "nop", 4);
  I->op_str[0] = '\0';
  if (I->detail)
    I->detail->x86.op_count = 0;
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

std::optional<bool> Decoder::returnsToCaller(const DecodedInsn &Insn) const {
  if (!Insn.Raw)
    return std::nullopt;
  if (X86)
    return X86Lifter::isReturn(Insn.Raw);
  if (AArch64)
    return AArch64Lifter::isReturn(Insn.Raw);
  if (ARM)
    return ARMLifter::isReturn(Insn.Raw);
  return std::nullopt;
}

bool Decoder::isResumableTrap(const DecodedInsn &Insn) const {
  if (!Insn.Raw || !X86)
    return false;
  return X86Lifter::isResumableTrap(Insn.Raw);
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

std::optional<uint64_t>
Decoder::returnImmediate(const DecodedInsn &Insn) const {
  if (!Insn.Raw || !X86)
    return std::nullopt;
  return X86Lifter::returnImmediate(Insn.Raw);
}

LowInstructionTargetMode
Decoder::controlTargetMode(const DecodedInsn &Insn,
                           InstructionMode SourceMode) const {
  if (!Insn.Raw || !ARM)
    return LowInstructionTargetMode::Preserve;
  return ARMLifter::controlTargetMode(Insn.Raw, SourceMode);
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
