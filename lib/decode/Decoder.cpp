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
#include "MipsSingle.h"
#include "X86ScanAdapter.h"

#include "neverd/ArchSupport.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/intrinsics/MIPS/MIPSHistoricalISA.h"
#include "neverd/lift/AArch64/AArch64Lifter.h"
#include "neverd/lift/ARM/ARMLifter.h"
#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/lift/PowerPC/PowerPCLifter.h"
#include "neverd/lift/RISCV/RISCVLifter.h"
#include "neverd/lift/X86/X86Lifter.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/Diagnostic.h"
#include "neverd/support/ProloguePatterns.h"

#include "llvm/Support/Compiler.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <utility>
#include <vector>

namespace neverd {

namespace {
va_t mipsInvalidAddressBits(Arch Target, InstructionMode Mode) {
  if (!isMIPS(Target))
    return 0;
  const va_t AlignmentBits = isMIPSCompressedMode(Mode) ? 1 : 3;
  return AlignmentBits | (isMIPS64(Target) ? 0 : ~va_t(UINT32_MAX));
}

IntegerControl integerControl(Arch A, RISCVProfile RV, const cs_insn &I) {
  if (isRISCV(A))
    return riscv::control(A, RV, I);
  if (isPowerPC(A))
    return ppc::control(A, I);
  return {};
}

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
  if (MicroASEHandle)
    cs_close(&MicroASEHandle);
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
  MIPS.reset();
  GoModule = nullptr;
}

// The parallel phases each build a per-thread Decoder, so a failure here can be
// reported from several worker threads at once — hence the serialized writes.
bool Decoder::init(Arch TheArch, InstructionMode Mode) {
  return initWithMIPSRevision(TheArch, Mode, 2);
}

bool Decoder::initWithMIPSRevision(Arch TheArch, InstructionMode Mode,
                                   uint8_t Revision, uint8_t Historical) {
  if ((Mode == InstructionMode::MIPSClassic || isMIPSCompressedMode(Mode)) &&
      !isMIPS(TheArch)) {
    syncError() << "MIPS instruction mode requires a MIPS target\n";
    return false;
  }
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
  std::unique_ptr<MIPSLifter> NewMIPS;

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
  case Arch::RISCV32:
  case Arch::RISCV64:
    if (Mode != InstructionMode::Default)
      return false;
    CsArch = CS_ARCH_RISCV;
    CsMode =
        cs_mode((TheArch == Arch::RISCV64 ? CS_MODE_RISCV64 : CS_MODE_RISCV32) |
                CS_MODE_RISCV_C | CS_MODE_RISCV_A);
    break;
  case Arch::PPC32:
  case Arch::PPC32LE:
  case Arch::PPC64:
  case Arch::PPC64LE:
  case Arch::PPC64ELFv1:
  case Arch::PPC64LEELFv1:
    if (Mode != InstructionMode::Default)
      return false;
    CsArch = CS_ARCH_PPC;
    CsMode = cs_mode((isPowerPC64(TheArch) ? CS_MODE_64 : CS_MODE_32) |
                     (archLittleEndian(TheArch) ? CS_MODE_LITTLE_ENDIAN
                                                : CS_MODE_BIG_ENDIAN));
    break;
  case Arch::MIPS32:
  case Arch::MIPS32EL:
  case Arch::MIPS64O32:
  case Arch::MIPS64O32EL:
  case Arch::MIPS64:
  case Arch::MIPS64N32:
  case Arch::MIPS64N32EL:
  case Arch::MIPS64EL:
    if ((Mode != InstructionMode::Default &&
         Mode != InstructionMode::MIPSClassic && !isMIPSCompressedMode(Mode)) ||
        (isMIPS64(TheArch) && Mode == InstructionMode::MIPS16) ||
        (isMIPS64O32(TheArch) && Historical != 3) ||
        (Historical ? (Historical > 5 || Revision ||
                       isMIPS64(TheArch) != (Historical >= 3) ||
                       isMIPSCompressedMode(Mode))
                    : (Revision != 1 && Revision != 2 && Revision != 3 &&
                       Revision != 5 && Revision != 6)) ||
        ((Revision == 1 || Revision == 3) && isMIPSCompressedMode(Mode)) ||
        (Revision != 2 && Mode == InstructionMode::MIPS16)) {
      syncError() << "unsupported MIPS instruction mode\n";
      return false;
    }
    CsArch = CS_ARCH_MIPS;
    CsMode = static_cast<cs_mode>(
        (Historical == 1   ? CS_MODE_MIPS1
         : Historical == 2 ? CS_MODE_MIPS2
         : Historical == 3 ? CS_MODE_MIPS3
         : Historical == 4 ? CS_MODE_MIPS4
         : Historical == 5 ? CS_MODE_MIPS5
         : Revision == 6
             ? (isMIPS64(TheArch) ? CS_MODE_MIPS64R6 : CS_MODE_MIPS32R6)
         : Revision == 5
             ? (isMIPS64(TheArch) ? CS_MODE_MIPS64R5 : CS_MODE_MIPS32R5)
         : Revision == 3
             ? (isMIPS64(TheArch) ? CS_MODE_MIPS64R3 : CS_MODE_MIPS32R3)
         : Revision == 1 ? (isMIPS64(TheArch) ? CS_MODE_MIPS64 : CS_MODE_MIPS32)
         : isMIPS64(TheArch) ? CS_MODE_MIPS64R2
                             : CS_MODE_MIPS32R2) |
        (Revision == 6 && isMIPS64(TheArch) && !isMIPSN32(TheArch)
             ? CS_MODE_MIPS_PTR64
             : 0) |
        (Mode == InstructionMode::MicroMIPS ? CS_MODE_MICRO : 0) |
        (Mode == InstructionMode::MIPS16 ? CS_MODE_MIPS16 : 0) |
        (archLittleEndian(TheArch) ? CS_MODE_LITTLE_ENDIAN
                                   : CS_MODE_BIG_ENDIAN));
    NewMIPS = std::make_unique<MIPSLifter>(TheArch, Mode, Revision, Historical);
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
  if (MicroASEHandle)
    cs_close(&MicroASEHandle);
  if (InsnBuf)
    cs_free(InsnBuf, 1);
  if (Handle)
    closeHandle(Handle, TargetArch);

  Handle = NewHandle;
  InsnBuf = NewInsnBuf;
  TargetArch = TheArch;
  CurrentMode = Mode;
  MIPSISARevision = Revision;
  MIPSHistoricalISA = Historical;
  MIPSNeedsDecodeGuard =
      NewMIPS && (Revision == 1 || Revision == 6 || Historical ||
                  (Revision == 5 && Mode == InstructionMode::MicroMIPS));
  MIPSInvalidAddressBits = mipsInvalidAddressBits(TheArch, Mode);
  Detail = true;
  Text = true;
  X86 = std::move(NewX86);
  ARM = std::move(NewARM);
  AArch64 = std::move(NewAArch64);
  MIPS = std::move(NewMIPS);
  RISCV = {};
  GoModule = nullptr;
  return true;
}

void Decoder::configureFor(const BinaryImage &Img) {
  RISCV = Img.RISCV;
  configureX86For(Img);
  if (MIPS) {
    MIPS->setMIPS16Enabled(Img.MIPS && Img.MIPS->HasMIPS16);
    MIPS->setMTEnabled(Img.MIPS && Img.MIPS->HasMT);
    MIPS->setMIPS16E2Enabled(Img.MIPS && Img.MIPS->HasMIPS16 &&
                             Img.MIPS->HasMIPS16E2);
    MIPS->setDSPEnabled(Img.MIPS && Img.MIPS->HasDSP,
                        Img.MIPS && Img.MIPS->HasDSPR2);
    MIPSNeedsDecodeGuard =
        MIPSISARevision == 1 || MIPSISARevision == 6 || MIPSHistoricalISA ||
        MIPS->hasClassicDSP64() ||
        (MIPSISARevision == 5 && CurrentMode == InstructionMode::MicroMIPS);
    MIPS->setMSAEnabled(Img.MIPS && Img.MIPS->HasMSA && MIPSISARevision == 5);
    MIPS->setScalarFPEnabled(Img.MIPS && Img.MIPS->hasScalarFPABI(Img.Arch),
                             Img.MIPS && Img.MIPS->hasScalarFP32ABI(Img.Arch));
  }
  if (AArch64)
    AArch64->setPreserveImmediateMemory(
        Img.isArm64e() &&
        (Img.MachOCPUSubtype & ~uint32_t(llvm::MachO::CPU_SUBTYPE_MASK)) ==
            macho_loader::arm64e::kCPUSubtypeX1);
  GoModule = AArch64 && Img.ExceptionMetadata.GoModule
                 ? &*Img.ExceptionMetadata.GoModule
                 : nullptr;
}

bool Decoder::init(const BinaryImage &Img) {
  if (Img.Mode == InstructionMode::MixedMIPS) {
    if (!isMIPS(Img.Arch) || !Img.MIPS ||
        Img.MIPS->HasMicroMIPS == Img.MIPS->HasMIPS16)
      return false;
    if (Img.instructionModeAt(Img.Entry))
      return selectMode(Img, Img.Entry);
    for (const auto &[Address, Mode] : Img.MIPS->CodeModeEntries)
      if (Img.instructionModeAt(Address) == Mode)
        return selectMode(Img, Address);
    return false;
  }
  if (Img.Mode != InstructionMode::MixedARMThumb) {
    if (!initWithMIPSRevision(Img.Arch, Img.Mode,
                              Img.MIPS ? Img.MIPS->ISARevision : 2,
                              Img.MIPS ? Img.MIPS->HistoricalISA : 0))
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
  if (Addr & mipsInvalidAddressBits(Img.Arch, *Mode))
    return false;
  const uint8_t Revision = Img.MIPS ? Img.MIPS->ISARevision : 2;
  const uint8_t Historical = Img.MIPS ? Img.MIPS->HistoricalISA : 0;
  if (Handle && TargetArch == Img.Arch && CurrentMode == *Mode &&
      (!MIPS ||
       (MIPSISARevision == Revision && MIPSHistoricalISA == Historical))) {
    if (AArch64 || MIPS || isRISCV(Img.Arch))
      configureFor(Img);
    return true;
  }
  const bool WasDetail = Detail;
  const bool WasText = Text;
  if (!initWithMIPSRevision(Img.Arch, *Mode, Revision, Historical))
    return false;
  setDetail(WasDetail);
  setText(WasText);
  if (AArch64 || MIPS || isRISCV(Img.Arch))
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

bool Decoder::beginDecode(DecodedInsn &Out) {
  Out.AArch64Prefix.reset();
  Out.AArch64Occurrence.reset();
  Out.MIPSForbiddenFollower.reset();
  if (!AArch64)
    return true;
  Out.Raw = nullptr;
  return advanceAArch64DecodeGeneration();
}

namespace {
// The historical selector table must not expand every common decode entry.
LLVM_ATTRIBUTE_NOINLINE bool historicalMIPSWordAvailable(const uint8_t *Bytes,
                                                         size_t Len,
                                                         Arch Target,
                                                         unsigned Level) {
  return Len >= 4 &&
         mips::historicalInstructionAvailable(
             readTargetEndian<uint32_t>(Bytes, archLittleEndian(Target)),
             Level);
}
} // namespace

LLVM_ATTRIBUTE_ALWAYS_INLINE bool
Decoder::decodeOtherPhysical(const uint8_t *Bytes, size_t Len, va_t Addr) {
  if (isRISCV(TargetArch)) {
    const unsigned Alignment = RISCV.hasCompressed() ? 2 : 4;
    if ((Addr & (Alignment - 1)) || Len < 2 ||
        (!RISCV.hasCompressed() && (Bytes[0] & 3) != 3) ||
        (TargetArch == Arch::RISCV32 && Addr > UINT32_MAX))
      return false;
  } else if (isPowerPC(TargetArch) &&
             ((Addr & 3) || (!isPowerPC64(TargetArch) && Addr > UINT32_MAX))) {
    return false;
  }
  InsnBuf->illegal = false;
  const uint8_t *Code = Bytes;
  size_t Remaining = Len;
  uint64_t PC = Addr;
  const bool Decoded =
      X86 && Text ? X86_disasmIterText(Handle, &Code, &Remaining, &PC, InsnBuf)
                  : cs_disasm_iter(Handle, &Code, &Remaining, &PC, InsnBuf);
  return Decoded || (X86 && (decodePrefixedX86Fence(Bytes, Len, Addr) ||
                             decodeX86MpxFallback(Bytes, Len, Addr)));
}

LLVM_ATTRIBUTE_NOINLINE bool Decoder::decodeMIPSProfile(const uint8_t *Bytes,
                                                        size_t Len, va_t Addr) {
  if (MIPSISARevision == 1) {
    if (Len < 4)
      return false;
    const auto Physical =
        readTargetEndian<uint32_t>(Bytes, archLittleEndian(TargetArch));
    const auto Word = MIPS->classicReleaseOneEncoding(Physical);
    if (!Word)
      return false;
    if (*Word != Physical)
      return decodeMIPSEquivalent(Bytes, *Word, Addr);
  }
  if (MIPS->hasClassicDSP64() && MIPSISARevision != 6) {
    // Only the admitted classic DSP64 profile reaches this branch. Its
    // legacy packed forms otherwise pay for a failed physical decode
    // before decoding their equivalent operand template.
    if (const auto Decoded = decodeMIPSDSP64(Bytes, Len, Addr))
      return *Decoded;
  } else if (MIPSISARevision == 6) {
    if (CurrentMode == InstructionMode::MicroMIPS)
      return decodeMicroMIPSRelease6(Bytes, Len, Addr);
    if (Len >= 4) {
      const auto Word =
          readTargetEndian<uint32_t>(Bytes, archLittleEndian(TargetArch));
      if ((Word & 0xffc0003c) == 0x4600001c)
        return decodeR6FloatCompare(Bytes, Word, Addr);
      // The physical decoder aliases paired LL/SC to an offset form and
      // swaps the n64 SC operands. Own SPECIAL3 before public decoding.
      if ((Word & 0xfc00002e) == 0x7c000026)
        return decodeMIPSReservation(Bytes, Word, Addr);
    }
  }
  // POOL32S vector forms have no microMIPS entries in Capstone. Avoid a
  // failed physical decode for every vector arithmetic or load/store opcode.
  const bool VectorPool =
      MIPSISARevision == 5 && CurrentMode == InstructionMode::MicroMIPS &&
      Len >= 4 && (Bytes[archLittleEndian(TargetArch) ? 1 : 0] >> 2) == 0x16;
  if (VectorPool && decodeMIPSASE(Bytes, Len, Addr))
    return true;
  return Mips_disasmOne(Handle, Bytes, Len, Addr, InsnBuf) ||
         (!VectorPool && decodeMIPSASE(Bytes, Len, Addr));
}

int Decoder::decodeMIPSOne(const uint8_t *Bytes, size_t Len, va_t Addr,
                           DecodedInsn &Out) {
  Out.AArch64Prefix.reset();
  Out.AArch64Occurrence.reset();
  Out.MIPSForbiddenFollower.reset();
  if (Addr & MIPSInvalidAddressBits)
    return 0;
  InsnBuf->illegal = false;
  bool Decoded = false;
  if (MIPSNeedsDecodeGuard) {
    if (MIPSHistoricalISA) {
      if (!historicalMIPSWordAvailable(Bytes, Len, TargetArch,
                                       MIPSHistoricalISA))
        return 0;
    } else {
      if (!decodeMIPSProfile(Bytes, Len, Addr))
        return 0;
      Decoded = true;
    }
  }
  if (!Decoded && !Mips_disasmOne(Handle, Bytes, Len, Addr, InsnBuf) &&
      !decodeMIPSASE(Bytes, Len, Addr))
    return 0;
  Out.Addr = InsnBuf->address;
  Out.Size = InsnBuf->size;
  Out.Id = InsnBuf->id;
  Out.Raw = InsnBuf;
  if (MIPSISARevision == 6 && CurrentMode != InstructionMode::MicroMIPS)
    Out.MIPSForbiddenFollower = MIPSLifter::r6ForbiddenFollower(
        {Bytes, Len}, archLittleEndian(TargetArch));
  return Out.Size;
}

int Decoder::decodeOne(const uint8_t *Bytes, size_t Len, va_t Addr,
                       DecodedInsn &Out) {
  if (MIPS)
    return decodeMIPSOne(Bytes, Len, Addr, Out);
  if (!beginDecode(Out) || !Handle || !InsnBuf)
    return 0;
  if (!decodeOtherPhysical(Bytes, Len, Addr))
    return 0;

  fixupDecodedInsn(InsnBuf);

  Out.Addr = InsnBuf->address;
  Out.Size = static_cast<uint16_t>(InsnBuf->size);
  Out.Id = InsnBuf->id;
  Out.Raw = InsnBuf;
  if (AArch64)
    recordAArch64Decode(Bytes, Len, Out, Detail && Text, true);
  // The lift path and CFG classification read the mnemonic (when they need it
  // at all, e.g. x86 push/pop/VCMP width, ARM push/pop, NEON lane) directly
  // from Out.Raw->mnemonic, which capstone already filled.  Copying it into a
  // fixed buffer on every decode was pure hot-path overhead; the SDK
  // disasm/query helpers that render the text also read Out.Raw->mnemonic.
  return Out.Size;
}

int Decoder::decodeOneForLift(const uint8_t *Bytes, size_t Len, va_t Addr,
                              DecodedInsn &Out) {
  if (MIPS)
    return decodeMIPSOne(Bytes, Len, Addr, Out);
  return AArch64 ? decodeAArch64ForLift(Bytes, Len, Addr, Out)
                 : decodeOne(Bytes, Len, Addr, Out);
}

int Decoder::decodeOneLight(const uint8_t *Bytes, size_t Len, va_t Addr,
                            DecodedInsn &Out) {
  if (MIPS)
    return decodeMIPSOne(Bytes, Len, Addr, Out);
  if (!beginDecode(Out) || !Handle || !InsnBuf)
    return 0;
  if (!decodeOtherPhysical(Bytes, Len, Addr))
    return 0;

  if (X86)
    fixupX86MpxDetail(InsnBuf);
  Out.Addr = InsnBuf->address;
  Out.Size = static_cast<uint16_t>(InsnBuf->size);
  Out.Id = InsnBuf->id;
  Out.Raw = InsnBuf;
  if (AArch64)
    recordAArch64Decode(Bytes, Len, Out, Detail && Text, true);
  return Out.Size;
}

void Decoder::setDetail(bool On) {
  if (On == Detail || Handle == 0)
    return;
  cs_option(Handle, CS_OPT_DETAIL, On ? CS_OPT_ON : CS_OPT_OFF);
  if (MicroASEHandle)
    cs_option(MicroASEHandle, CS_OPT_DETAIL, On ? CS_OPT_ON : CS_OPT_OFF);
  Detail = On;
}

void Decoder::setText(bool On) {
  if (On == Text || Handle == 0)
    return;
  cs_option(Handle, CS_OPT_TEXT, On ? CS_OPT_ON : CS_OPT_OFF);
  if (MicroASEHandle)
    cs_option(MicroASEHandle, CS_OPT_TEXT, On ? CS_OPT_ON : CS_OPT_OFF);
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
  } else if (MIPS) {
    MIPS->lift(*Insn.Raw, Ops, Insn.MIPSForbiddenFollower);
  } else if (isRISCV(TargetArch) && Insn.Raw) {
    riscv::lift(TargetArch, RISCV, *Insn.Raw, Ops);
  } else if (isPowerPC(TargetArch) && Insn.Raw) {
    ppc::lift(TargetArch, *Insn.Raw, Ops);
  } else {
    throw UnliftedInstruction(Insn.Addr, "no active native lifter", nullptr);
  }
  if (UndefinedEffects && !X86)
    UndefinedEffects->OpCount = Ops.size() - OpsStart;
}

void Decoder::fixupDecodedInsn(cs_insn *I) const {
  if (!I)
    return;
  if (X86) {
    fixupX86MpxDetail(I);
    X86Lifter::fixupDecodedInsn(I);
  } else if (AArch64)
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
  return MIPS ? MIPS->isFunctionTerminator(*Insn.Raw)
              : integerControl(TargetArch, RISCV, *Insn.Raw).terminates();
}

bool Decoder::isAlignmentNop(const DecodedInsn &Insn) const {
  if (!Insn.Raw || !Insn.Size ||
      ((AArch64 || Insn.AArch64Occurrence) &&
       !hasAArch64DecodeOccurrence(Insn)))
    return false;
  if (MIPS)
    return MIPS->isAlignmentNop(*Insn.Raw);
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
  if (MIPS)
    return MIPS->isReturn(*Insn.Raw);
  if (isRISCV(TargetArch) || isPowerPC(TargetArch))
    return integerControl(TargetArch, RISCV, *Insn.Raw).Opcode == NdOp::RETURN;
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
  if (MIPS)
    return MIPS->directCallTarget(*Insn.Raw);
  const auto Flow = integerControl(TargetArch, RISCV, *Insn.Raw);
  return Flow.Opcode == NdOp::CALL ? Flow.Target : InvalidVA;
}

} // namespace neverd
