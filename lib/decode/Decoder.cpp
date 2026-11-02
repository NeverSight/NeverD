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

#include "DecoderDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64/AArch64Lifter.h"
#include "neverd/lift/ARM/ARMLifter.h"
#include "neverd/lift/X86/X86Lifter.h"
#include "neverd/support/Diagnostic.h"
#include "neverd/support/ProloguePatterns.h"

#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <utility>
#include <vector>

namespace neverd {

namespace {
/// Release a decoder handle using the active architecture's ownership rules.
void closeHandle(csh &Handle, Arch A) {
  if (A == Arch::X64 || A == Arch::X86)
    decoder_detail::releaseX86Handle(Handle, A);
  else
    cs_close(&Handle);
  Handle = 0;
}
} // namespace

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

Decoder::Decoder() : AArch64DecodeIdentity(nextAArch64DecodeIdentity()) {}

Decoder::~Decoder() { reset(); }

void Decoder::reset() {
  advanceAArch64DecodeGeneration();
  if (InsnBuf)
    cs_free(InsnBuf, 1);
  if (Handle)
    closeHandle(Handle, TargetArch);
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
    CsMode = decoder_detail::x86HandleMode(TheArch);
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
    CsMode = decoder_detail::armHandleMode(Mode);
    NewARM = std::make_unique<ARMLifter>(TheArch, Mode);
    NewARM->setStrict(Strict);
    break;
  default:
    syncError() << "unsupported arch for decoder\n";
    return false;
  }

  csh NewHandle =
      CsArch == CS_ARCH_X86 ? decoder_detail::takeX86Handle(CsMode) : csh(0);
  if (!NewHandle) {
    if (cs_open(CsArch, CsMode, &NewHandle) != CS_ERR_OK) {
      syncError() << "failed to init capstone\n";
      return false;
    }
    cs_option(NewHandle, CS_OPT_DETAIL, CS_OPT_ON);
  }

  cs_insn *NewInsnBuf = cs_malloc(NewHandle);
  if (!NewInsnBuf) {
    closeHandle(NewHandle, TheArch);
    syncError() << "failed to allocate capstone insn buffer\n";
    return false;
  }

  advanceAArch64DecodeGeneration();
  if (InsnBuf)
    cs_free(InsnBuf, 1);
  if (Handle)
    closeHandle(Handle, TargetArch);

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
  configureX86For(Img);
  if (AArch64)
    AArch64->setPreserveImmediateMemory(
        Img.isArm64e() &&
        (Img.MachOCPUSubtype & ~uint32_t(llvm::MachO::CPU_SUBTYPE_MASK)) ==
            macho_loader::arm64e::kCPUSubtypeX1);
}

bool Decoder::init(const BinaryImage &Img) {
  if (Img.Mode != InstructionMode::MixedARMThumb) {
    if (!init(Img.Arch, Img.Mode))
      return false;
    configureFor(Img);
    return true;
  }
  return initARMImageMode(Img);
}

bool Decoder::selectMode(const BinaryImage &Img, va_t Addr,
                         std::optional<InstructionMode> IncomingMode) {
  const auto Mode = Img.instructionModeAt(Addr, IncomingMode);
  if (!Mode)
    return false;
  if (Img.Arch == Arch::ARM &&
      !decoder_detail::isARMInstructionAligned(Addr, *Mode))
    return false;
  if (Handle && TargetArch == Img.Arch && CurrentMode == *Mode) {
    if (AArch64)
      configureFor(Img);
    return true;
  }
  const bool WasDetail = Detail;
  const bool WasText = Text;
  if (!init(Img.Arch, *Mode))
    return false;
  setDetail(WasDetail);
  setText(WasText);
  if (AArch64)
    configureFor(Img);
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

bool Decoder::decodeArchitectureFallback(const uint8_t *Bytes, size_t Len,
                                         va_t Addr) {
  return X86 && (decodePrefixedX86Fence(Bytes, Len, Addr) ||
                 decodeX86MpxFallback(Bytes, Len, Addr));
}

int Decoder::decodeOne(const uint8_t *Bytes, size_t Len, va_t Addr,
                       DecodedInsn &Out) {
  if (!beginDecode(Out) || !Handle || !InsnBuf)
    return 0;
  const uint8_t *Code = Bytes;
  size_t Sz = Len;
  uint64_t A = Addr;

  // Capstone records soft decode failures on the reused instruction object.
  // Validity belongs to this occurrence, never the previously decoded bytes.
  InsnBuf->illegal = false;
  if (!cs_disasm_iter(Handle, &Code, &Sz, &A, InsnBuf) &&
      !decodeArchitectureFallback(Bytes, Len, Addr))
    return 0;

  fixupDecodedInsn(InsnBuf);

  Out.Addr = InsnBuf->address;
  Out.Size = static_cast<uint16_t>(InsnBuf->size);
  Out.Id = InsnBuf->id;
  Out.Raw = InsnBuf;
  if (AArch64)
    recordAArch64Decode(Bytes, Len, Out, Detail && Text);
  // The lift path and CFG classification read the mnemonic (when they need it
  // at all, e.g. x86 push/pop/VCMP width, ARM push/pop, NEON lane) directly
  // from Out.Raw->mnemonic, which capstone already filled.  Copying it into a
  // fixed buffer on every decode was pure hot-path overhead; the SDK
  // disasm/query helpers that render the text also read Out.Raw->mnemonic.
  return Out.Size;
}

int Decoder::decodeOneForLift(const uint8_t *Bytes, size_t Len, va_t Addr,
                              DecodedInsn &Out) {
  if (!beginDecode(Out))
    return 0;
  if (AArch64)
    if (const int Size = decodeAArch64ForLift(Bytes, Len, Addr, Out))
      return Size;
  return decodeOne(Bytes, Len, Addr, Out);
}

int Decoder::decodeOneLight(const uint8_t *Bytes, size_t Len, va_t Addr,
                            DecodedInsn &Out) {
  if (!beginDecode(Out) || !Handle || !InsnBuf)
    return 0;
  const uint8_t *Code = Bytes;
  size_t Sz = Len;
  uint64_t A = Addr;

  InsnBuf->illegal = false;
  if (!cs_disasm_iter(Handle, &Code, &Sz, &A, InsnBuf) &&
      !decodeArchitectureFallback(Bytes, Len, Addr))
    return 0;

  fixupX86DecodeMetadata(InsnBuf);
  Out.Addr = InsnBuf->address;
  Out.Size = static_cast<uint16_t>(InsnBuf->size);
  Out.Id = InsnBuf->id;
  Out.Raw = InsnBuf;
  if (AArch64)
    recordAArch64Decode(Bytes, Len, Out, Detail && Text);
  return Out.Size;
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
  if ((AArch64 || Insn.AArch64Occurrence) && !hasAArch64OperandDetail(Insn))
    throw UnliftedInstruction(Insn.Addr, aarch64_decode::kInvalidOccurrence,
                              nullptr);
  if (X86) {
    X86->lift(Insn.Raw, Ops, Relocs, ScalarRelocs, UndefinedEffects,
              PreservedState);
  } else if (AArch64) {
    liftAArch64ToLow(Insn, Ops, ScalarRelocs);
  } else if (ARM) {
    liftARMToLow(Insn, Ops, ScalarRelocs);
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

void Decoder::fixupDecodedInsn(cs_insn *I) const {
  if (!I)
    return;
  fixupX86DecodeMetadata(I);
  if (X86)
    X86Lifter::fixupDecodedInsn(I);
  else if (AArch64)
    AArch64Lifter::fixupDecodedInsn(I);
  else if (ARM)
    ARMLifter::fixupDecodedInsn(I);
}

bool Decoder::isFunctionTerminator(const DecodedInsn &Insn) const {
  if (!Insn.Raw || ((AArch64 || Insn.AArch64Occurrence) &&
                    !hasAArch64DecodeOccurrence(Insn)))
    return false;
  if (X86)
    return X86Lifter::isFunctionTerminator(Insn.Raw);
  if (AArch64)
    return AArch64Lifter::isFunctionTerminator(Insn.Raw);
  if (ARM)
    return ARMLifter::isFunctionTerminator(Insn.Raw);
  return false;
}

bool Decoder::isAlignmentNop(const DecodedInsn &Insn) const {
  if (!Insn.Raw || !Insn.Size ||
      ((AArch64 || Insn.AArch64Occurrence) &&
       !hasAArch64DecodeOccurrence(Insn)))
    return false;
  if (X86 && decoder_detail::isX86AlignmentNop(Insn))
    return true;
  return alignmentNopLength(TargetArch, Insn.Raw->bytes, Insn.Size,
                            currentMode()) == Insn.Size;
}

std::optional<bool> Decoder::returnsToCaller(const DecodedInsn &Insn) const {
  if (!Insn.Raw || ((AArch64 || Insn.AArch64Occurrence) &&
                    !hasAArch64DecodeOccurrence(Insn)))
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
  if (!Insn.Raw || ((AArch64 || Insn.AArch64Occurrence) &&
                    !hasAArch64DecodeOccurrence(Insn)))
    return false;
  if (X86)
    return X86Lifter::isResumableTrap(Insn.Raw);
  if (AArch64)
    return AArch64Lifter::isResumableTrap(Insn.Raw);
  return ARM && ARMLifter::isResumableTrap(Insn.Raw);
}

va_t Decoder::directCallTarget(const DecodedInsn &Insn) const {
  if (!Insn.Raw || ((AArch64 || Insn.AArch64Occurrence) &&
                    !hasAArch64DecodeOccurrence(Insn)))
    return InvalidVA;
  if (X86)
    return X86Lifter::directCallTarget(Insn.Raw);
  if (AArch64)
    return AArch64Lifter::directCallTarget(Insn.Raw);
  if (ARM)
    return ARMLifter::directCallTarget(Insn.Raw);
  return InvalidVA;
}

} // namespace neverd
