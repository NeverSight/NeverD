//===- MachORegisterResults.cpp - Native register entries -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Lower and validate register-preserving Mach-O entries at the patch boundary.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/MachO/MachORegisterResults.h"

#include "neverd/analysis/AArch64UnwindRecovery.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/AArch64/LLVMAArch64ReturnProtection.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/DirectBranch.h"
#include "neverd/loader/Limits.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/DwarfEH.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <optional>
#include <string>

namespace neverd {
namespace {
#define MACHO_REGISTER_VALUE(Type, Name, Value) constexpr Type Name = Value;
#define MACHO_REGISTER_TEXT(Name, Value) constexpr char Name[] = Value;
#include "MachORegisterResults.def"

using Entries = std::map<va_t, llvm::Function *>;
struct RegisterBody {
  std::string Assembly;
  std::string Constraints;
  llvm::SmallVector<llvm::Value *> Operands;
  llvm::SmallVector<llvm::Function *> ObservedCallees;
  llvm::SmallVector<llvm::Function *> TailCallees;
  bool HasStackIndex = false;
  bool HasMemoryWriteback = false;
  bool HasMemoryIndex = false;
  bool HasSIMDData = false;
  bool HasCompactFrame = false;
  bool HasLanguageFrame = false;
  bool HasBareReturn = false;
  bool LoadsStackArgument = false;
  bool MaterializesCursor = false;
};

llvm::Error invalidBody() {
  return llvm::createStringError(llvm::errc::not_supported, kInvalidBody);
}

llvm::Expected<Entries>
definedEntries(llvm::Module &M, const BinaryImage &Image,
               const llvm::DenseSet<va_t> &Required = {}) {
  Entries Functions;
  for (llvm::Function &F : M) {
    if (F.isIntrinsic())
      continue;
    auto Entry = rewrite_source::getOriginalVA(F);
    if (!Entry)
      return Entry.takeError();
    if (F.isDeclaration() && (!*Entry || (!Required.contains(**Entry) &&
                                          !Image.findImportStubAt(**Entry))))
      continue;
    if (!*Entry || !Functions.try_emplace(**Entry, &F).second)
      return invalidBody();
  }
  return Functions;
}

struct PCSigningInstruction {
  unsigned Operands;
  bool Returns;
};

std::optional<PCSigningInstruction> pcSigningInstruction(unsigned Id) {
  switch (Id) {
#define MACHO_REGISTER_PC_SIGNING(Name, Operands, Returns)                     \
  case AARCH64_INS_##Name:                                                     \
    return PCSigningInstruction{Operands, Returns};
#include "MachORegisterResults.def"
  default:
    return std::nullopt;
  }
}

// A literal load has an immediate target, whereas indirect and indexed
// memory operands carry the actual base. Render only position-independent
// instruction forms; all PC-relative control transfers get symbolic operands.
bool independent(const DecodedInsn &I) {
  const auto &D = I.Raw->detail->aarch64;
  if (const auto PCSigning = pcSigningInstruction(I.Id))
    return D.op_count == PCSigning->Operands &&
           (!D.op_count || D.operands[0].type == AARCH64_OP_IMM);
  switch (I.Id) {
#define MACHO_REGISTER_ALU(Name) case AARCH64_INS_##Name:
#include "MachORegisterResults.def"
    return llvm::all_of(
        llvm::ArrayRef(D.operands, D.op_count), [](const cs_aarch64_op &Op) {
          return Op.type == AARCH64_OP_IMM || Op.type == AARCH64_OP_REG;
        });
#define MACHO_REGISTER_MEMORY(Name) case AARCH64_INS_##Name:
#include "MachORegisterResults.def"
    return llvm::any_of(
        llvm::ArrayRef(D.operands, D.op_count),
        [](const cs_aarch64_op &Op) { return Op.type == AARCH64_OP_MEM; });
  case AARCH64_INS_HINT:
    switch (I.Raw->alias_id) {
#define MACHO_REGISTER_HINT(Name) case AARCH64_INS_ALIAS_##Name:
#include "MachORegisterResults.def"
      return true;
    default:
      return false;
    }
  case AARCH64_INS_RETAA:
  case AARCH64_INS_RETAB:
  case AARCH64_INS_RET:
    return D.op_count == 0;
  case AARCH64_INS_BRK:
    return D.op_count == 1 && D.operands[0].type == AARCH64_OP_IMM;
  default:
    return false;
  }
}

bool physicalRegister(aarch64_reg R) {
  return (R >= AARCH64_REG_X0 && R <= AARCH64_REG_X28) ||
         (R >= AARCH64_REG_W0 && R <= AARCH64_REG_W30) ||
         (R >= AARCH64_REG_B0 && R <= AARCH64_REG_B31) ||
         (R >= AARCH64_REG_H0 && R <= AARCH64_REG_H31) ||
         (R >= AARCH64_REG_Q0 && R <= AARCH64_REG_Q31) ||
         (R >= AARCH64_REG_D0 && R <= AARCH64_REG_D31) ||
         (R >= AARCH64_REG_S0 && R <= AARCH64_REG_S31) || R == AARCH64_REG_FP ||
         R == AARCH64_REG_LR || R == AARCH64_REG_SP || R == AARCH64_REG_WSP ||
         R == AARCH64_REG_XZR || R == AARCH64_REG_WZR;
}

// Keep the constants and callees as actual LLVM operands. In particular a
// writeback displacement is distinct from the address used for the load.
void emitOperand(RegisterBody &Body, llvm::raw_ostream &OS, llvm::Value *V,
                 llvm::StringRef Constraint) {
  const auto It = llvm::find(Body.Operands, V);
  const size_t Index = It - Body.Operands.begin();
  if (It == Body.Operands.end()) {
    Body.Operands.push_back(V);
    Body.Constraints += Constraint;
  }
  OS << kOperandStart << Index << kOperandEnd;
}

bool emitOperands(RegisterBody &Body, llvm::raw_ostream &OS, Decoder &Decode,
                  const DecodedInsn &I, llvm::LLVMContext &Context) {
  const auto &D = I.Raw->detail->aarch64;
  const auto Immediate = [&](int64_t Value) {
    auto *C =
        llvm::ConstantInt::getSigned(llvm::Type::getInt64Ty(Context), Value);
    emitOperand(Body, OS, C, kImmediateConstraint);
  };
  const auto Reg = [&](aarch64_reg R) {
    if (!physicalRegister(R))
      return false;
    OS << cs_reg_name(Decode.getHandle(), R);
    return true;
  };
  for (const auto &[N, Op] :
       llvm::enumerate(llvm::ArrayRef(D.operands, D.op_count))) {
    if (N)
      OS << kSeparator;
    else
      OS << ' ';
    if (Op.type != AARCH64_OP_MEM && Op.ext != AARCH64_EXT_INVALID)
      return false;
    switch (Op.type) {
    case AARCH64_OP_REG:
      if (Op.is_vreg || !Reg(Op.reg))
        return false;
      break;
    case AARCH64_OP_IMM:
      OS << '#';
      Immediate(Op.imm);
      break;
    case AARCH64_OP_MEM:
      OS << '[';
      if (!Reg(Op.mem.base))
        return false;
      if (Op.mem.index != AARCH64_REG_INVALID) {
        if (Op.mem.disp || D.post_index || I.Raw->detail->writeback)
          return false;
        OS << kSeparator;
        if (!Reg(Op.mem.index))
          return false;
        switch (Op.ext) {
        case AARCH64_EXT_INVALID:
          if (Op.shift.type != AARCH64_SFT_INVALID &&
              Op.shift.type != AARCH64_SFT_LSL)
            return false;
          if (Op.shift.value)
            OS << kShiftLeft;
          break;
#define MACHO_REGISTER_EXTEND(Name, Spelling)                                  \
  case AARCH64_EXT_##Name:                                                     \
    OS << Spelling;                                                            \
    if (Op.shift.value)                                                        \
      OS << kShiftAmount;                                                      \
    break;
#include "MachORegisterResults.def"
        default:
          return false;
        }
        if (Op.shift.value)
          Immediate(Op.shift.value);
        OS << ']';
        continue;
      }
      if (Op.ext != AARCH64_EXT_INVALID || Op.shift.value)
        return false;
      if (D.post_index)
        OS << ']';
      if (Op.mem.disp || D.post_index || I.Raw->detail->writeback) {
        OS << kSeparator << '#';
        Immediate(Op.mem.disp);
      }
      if (!D.post_index) {
        OS << ']';
        if (I.Raw->detail->writeback)
          OS << '!';
      }
      break;
    default:
      return false;
    }
    if (Op.shift.value) {
      if (Op.shift.type != AARCH64_SFT_LSL)
        return false;
      OS << kShiftLeft;
      Immediate(Op.shift.value);
    }
  }
  return true;
}

// A W write defines the entire corresponding X register. Reads precede
// writes within an instruction, including read-modify-write operands.
unsigned scratchMask(llvm::ArrayRef<uint16_t> Registers) {
  unsigned Mask = 0;
  for (const auto R : Registers) {
    if (R == AARCH64_REG_X8 || R == AARCH64_REG_W8)
      Mask |= kScratchX8;
    if (R == AARCH64_REG_X9 || R == AARCH64_REG_W9)
      Mask |= kScratchX9;
  }
  return Mask;
}

// Physical results can stay live across both sides of a conditional branch
// and across a backedge. A later W write kills the corresponding full X value.
// Calls terminate the previous result; their own result is observed whenever
// a successor path reaches a read before a write or another call.
struct ScratchFlow {
  llvm::SmallVector<size_t, 2> Predecessors;
  llvm::Function *Callee = nullptr;
  unsigned Reads = 0;
  unsigned Writes = 0;
  unsigned Live = 0;
  bool Observed = false;
};

void observeScratchResults(llvm::MutableArrayRef<ScratchFlow> Flow,
                           RegisterBody &Body) {
  llvm::SmallVector<size_t> Worklist;
  Worklist.reserve(Flow.size());
  for (auto [Index, State] : llvm::enumerate(Flow)) {
    State.Live = State.Reads;
    if (State.Live)
      Worklist.push_back(Index);
  }
  while (!Worklist.empty()) {
    const auto &State = Flow[Worklist.pop_back_val()];
    for (size_t Index : State.Predecessors) {
      auto &Previous = Flow[Index];
      if (Previous.Callee) {
        Previous.Observed = true;
        continue;
      }
      const unsigned Live = State.Live & ~Previous.Writes;
      if (Live & ~Previous.Live) {
        Previous.Live |= Live;
        Worklist.push_back(Index);
      }
    }
  }
  for (const auto &State : Flow)
    if (State.Observed)
      Body.ObservedCallees.push_back(State.Callee);
}

bool savedFrameRegister(unsigned Register) {
  const unsigned FirstVector =
      aarch64_unwind::kDwarfV0 + NDArm64eFirstPreservedD;
  return (Register >= NDArm64eFirstPreservedGPR &&
          Register - NDArm64eFirstPreservedGPR < NDArm64ePreservedGPRCount) ||
         Register == kDwarfFP || Register == kDwarfLR ||
         (Register >= FirstVector &&
          Register - FirstVector < NDArm64ePreservedDCount);
}

bool emitFrameRow(const CFIInstruction &Row, llvm::raw_ostream &OS) {
  switch (Row.Kind) {
  case CFIOpKind::Nop:
  case CFIOpKind::AdvanceLoc:
    return true;
  case CFIOpKind::DefCFA:
    if (Row.Offset < 0 ||
        (Row.Register != kDwarfFP && Row.Register != kDwarfSP))
      return false;
    OS << kCFA << Row.Register << kSeparator << Row.Offset << '\n';
    return true;
  case CFIOpKind::DefCFARegister:
    if (Row.Register != kDwarfFP && Row.Register != kDwarfSP)
      return false;
    OS << kCFARegister << Row.Register << '\n';
    return true;
  case CFIOpKind::DefCFAOffset:
    if (Row.Offset < 0)
      return false;
    OS << kCFAOffset << Row.Offset << '\n';
    return true;
  case CFIOpKind::Offset:
    if (!savedFrameRegister(Row.Register) ||
        Row.Offset % int64_t(sizeof(uint64_t)))
      return false;
    OS << kSpill << Row.Register << kSeparator << Row.Offset << '\n';
    return true;
  case CFIOpKind::Restore:
  case CFIOpKind::SameValue:
    if (!savedFrameRegister(Row.Register))
      return false;
    OS << (Row.Kind == CFIOpKind::Restore ? kRestore : kSameValue)
       << Row.Register << '\n';
    return true;
  case CFIOpKind::RememberState:
    OS << kRememberState;
    return true;
  case CFIOpKind::RestoreState:
    OS << kRestoreState;
    return true;
  case CFIOpKind::GnuArgsSize: {
    if (Row.Offset < 0)
      return false;
    llvm::SmallVector<char> Bytes;
    llvm::raw_svector_ostream Encoded(Bytes);
    llvm::encodeULEB128(Row.Offset, Encoded);
    OS << kEscape << unsigned(llvm::dwarf::DW_CFA_GNU_args_size);
    for (unsigned char Byte : Bytes)
      OS << kSeparator << unsigned(Byte);
    OS << '\n';
    return true;
  }
  default:
    return false;
  }
}

// MC recognizes the compact frame from a canonical CFA and descending slots.
// Emit its recipe without inventing instruction-specific DWARF rows. The
// linked-image verifier requires the original compact word and machine body.
void emitCompactFrame(const CompactUnwindEntry &Compact,
                      llvm::raw_ostream &OS) {
  constexpr int SlotSize = sizeof(uint64_t);
  constexpr int FrameSize = NDArm64eFrameRecordWords * SlotSize;
  int Offset = 0;
  if (Compact.Kind == CompactUnwindKind::FramePointer) {
    OS << kCFA << kDwarfFP << kSeparator << FrameSize << '\n';
    // The compact encoder requires the return address before the frame pointer.
    OS << kSpill << kDwarfLR << kSeparator << -SlotSize << '\n';
    OS << kSpill << kDwarfFP << kSeparator << -FrameSize << '\n';
    Offset = -FrameSize;
  } else if (Compact.StackSize) {
    OS << kCFAOffset << Compact.StackSize << '\n';
  }
  for (const auto &Slot : Compact.SavedRegisterSlots) {
    const unsigned Register =
        Slot.Register +
        (Slot.RegisterClass == UnwindRegisterClass::FloatingPoint
             ? aarch64_unwind::kDwarfV0
             : 0);
    Offset -= SlotSize;
    OS << kSpill << Register << kSeparator << Offset << '\n';
  }
}

// The function body keeps every instruction offset. Canonicalize LPStart to
// the new function start and rebuild only address-bearing fields. Raw action
// links and specification-list offsets retain their selectors and shared tails.
bool emitLanguageTable(const BinaryImage &Image, const ExceptionFunction &Frame,
                       llvm::Module &Module, RegisterBody &Body,
                       llvm::raw_ostream &OS) {
  const auto &Table = *Frame.Itanium;
  // A zero landing-pad offset means no handler. An entry-point handler needs
  // a base before the function so that its encoded offset remains nonzero.
  const unsigned BaseBias =
      llvm::any_of(Table.CallSites,
                   [&](const auto &Site) {
                     return Site.LandingPadVA == Frame.CodeRange.Begin;
                   })
          ? aarch64::kInsnSize
          : 0;
  const auto Bytes = [&](llvm::ArrayRef<uint8_t> Data) {
    for (uint8_t Byte : Data)
      OS << kByte << unsigned(Byte) << '\n';
  };
  const auto SourceBytes = [&](ExceptionAddressRange Range) {
    if (Range.size())
      Bytes({Image.readVA(Range.Begin, Range.size()), size_t(Range.size())});
  };
  llvm::SmallVector<char> Sites;
  llvm::raw_svector_ostream Encoded(Sites);
  for (const auto &Site : Table.CallSites) {
    llvm::encodeULEB128(Site.GuardedRange.Begin - Frame.CodeRange.Begin,
                        Encoded);
    llvm::encodeULEB128(Site.GuardedRange.size(), Encoded);
    llvm::encodeULEB128(Site.LandingPadVA ? Site.LandingPadVA -
                                                Frame.CodeRange.Begin + BaseBias
                                          : 0,
                        Encoded);
    llvm::encodeULEB128(Site.NativeActionRecord, Encoded);
  }
  OS << kPushSection << section_names::macho::TextSeg << kSeparator
     << section_names::macho::GccExceptTab << '\n';
  OS << kGeneratedLSDA << kEndLabel << kByte
     << unsigned(BaseBias ? dweh::PCRel | dweh::Sdata4 : dweh::Omit) << '\n';
  if (BaseBias)
    OS << kLong << kLocal << 0 << '-' << BaseBias << kRelativeField;
  const bool HasTypes = Table.TypeTableEncoding != dweh::Omit;
  const bool Indirect = Table.TypeTableEncoding & dweh::Indirect;
  OS << kByte
     << unsigned(HasTypes ? dweh::PCRel | dweh::Sdata4 |
                                (Indirect ? dweh::Indirect : 0)
                          : dweh::Omit)
     << '\n';
  if (HasTypes)
    OS << kULEB << kTypeTableEnd << '-' << kTypeTableBase << '\n'
       << kTypeTableBase << kEndLabel;
  OS << kByte << unsigned(dweh::Uleb128) << '\n'
     << kULEB << Sites.size() << '\n';
  Bytes({reinterpret_cast<const uint8_t *>(Sites.data()), Sites.size()});
  SourceBytes(Table.ActionTableRange);
  for (const auto &Type : llvm::reverse(Table.TypeTable)) {
    OS << kLong;
    if (Type.IsCatchAll) {
      OS << 0 << '\n';
      continue;
    }
    auto *Target = Module.getNamedGlobal(
        makeNdDataSymbol(Indirect ? Type.TypeInfoSlotVA : Type.TypeInfoVA));
    if (!Target)
      return false;
    emitOperand(Body, OS, Target, kSymbolConstraint);
    OS << kRelativeField;
  }
  if (HasTypes)
    OS << kTypeTableEnd << kEndLabel;
  SourceBytes(Table.ExceptionSpecRange);
  OS << kPopSection;
  return true;
}

std::optional<RegisterBody> registerBody(Decoder &Decode,
                                         const BinaryImage &Image,
                                         const Entries &Functions,
                                         Entries::const_iterator Owner) {
  const va_t Start = Owner->first;
  const Section *Text = Image.getSectionFor(Start);
  if (!Text || Text->Name != section_names::macho::Text ||
      Text->SegmentName != section_names::macho::TextSeg ||
      !Image.hasAuthenticatedFunctionEntryAt(Start))
    return std::nullopt;
  auto Next = std::next(Owner);
  const va_t End = Next != Functions.end() && Text->contains(Next->first)
                       ? Next->first
                       : Text->VA + Text->Size;
  const size_t Word = aarch64::kInsnSize;
  if (End <= Start || (End - Start) % Word ||
      !rangeInBounds(Text->FileOff + Start - Text->VA, End - Start,
                     Image.Raw.size()))
    return std::nullopt;
  const auto *Frame = Image.ExceptionMetadata.findFunction(Start);
  if (!Frame || !Frame->Compact ||
      (!Frame->describesUnwindOnly() &&
       !canPublishMachOLanguageFrame(Image, *Frame)) ||
      Frame->CodeRange.End < End ||
      (Frame->Itanium &&
       (Frame->CodeRange.Begin != Start || Frame->CodeRange.End != End)))
    return std::nullopt;
  const auto &Compact = *Frame->Compact;
  const auto FrameEncoding =
      Compact.NativeEncoding &
      ~(macho_unwind::kHasLSDA | macho_unwind::kPersonalityMask);
  const bool Indexed = (FrameEncoding & macho_unwind::kModeMask) ==
                       macho_unwind::kARM64ModeDwarf;
  const bool PAuthLR =
      (FrameEncoding & ~(macho_unwind::kARM64RegisterPairsMask |
                         macho_unwind::kARM64PAuthLROffsetMask)) ==
      macho_unwind::kARM64ModeFramePAuthLR;
  const bool Framed =
      PAuthLR || (FrameEncoding & ~macho_unwind::kARM64RegisterPairsMask) ==
                     macho_unwind::kARM64ModeFrame;
  if (PAuthLR &&
      (Frame->CodeRange.Begin != Start || !Compact.ReturnAddressSigningOffset ||
       *Compact.ReturnAddressSigningOffset >= End - Start))
    return std::nullopt;
  const DwarfCIE *CIE = nullptr;
  llvm::ArrayRef<CFIInstruction> Rows;
  if (Indexed) {
    if (!Frame->Dwarf || Frame->Dwarf->InitialLocation != Start ||
        Frame->Dwarf->AddressRange != End - Start ||
        Frame->Dwarf->ParseStatus != ExceptionParseStatus::Complete)
      return std::nullopt;
    CIE = Image.ExceptionMetadata.findCIE(Frame->Dwarf->CIESectionOffset);
    if (!CIE || CIE->ParseStatus != ExceptionParseStatus::Complete)
      return std::nullopt;
    Rows = Frame->Dwarf->Instructions;
  } else if (Frame->Dwarf ||
             (!Framed &&
              (FrameEncoding & ~(macho_unwind::kARM64FramelessStackSizeMask |
                                 macho_unwind::kARM64RegisterPairsMask)) !=
                  macho_unwind::kARM64ModeFrameless)) {
    return std::nullopt;
  }

  RegisterBody Body;
  Body.HasLanguageFrame = Frame->Itanium.has_value();
  Body.HasCompactFrame =
      Framed || (!Indexed && !Compact.SavedRegisterSlots.empty());
  llvm::raw_string_ostream OS(Body.Assembly);
  const bool FramelessSpills =
      !Indexed && !Framed && !Compact.SavedRegisterSlots.empty();
  if (PAuthLR || FramelessSpills || (CIE && CIE->HasPointerAuth))
    OS << kBKeyFrame;
  if (Body.HasLanguageFrame) {
    OS << kPersonality
       << unsigned(CIE ? CIE->PersonalityEncoding
                       : dweh::Indirect | dweh::PCRel | dweh::Sdata4)
       << kSeparator
       << (Frame->Personality == ExceptionPersonality::GxxPersonalityV0
               ? kGxxPersonality
               : kObjCPersonality)
       << '\n';
    OS << kLSDA << unsigned(CIE ? CIE->LSDAPointerEncoding : dweh::PCRel)
       << kSeparator;
    if (Frame->Itanium->LandingPadBaseEncoding != dweh::Omit) {
      OS << kGeneratedLSDA;
    } else {
      auto *LSDA = Owner->second->getParent()->getNamedGlobal(
          makeNdDataSymbol(Frame->Itanium->LSDAVA));
      if (!LSDA)
        return std::nullopt;
      emitOperand(Body, OS, LSDA, kSymbolConstraint);
    }
    OS << '\n';
  }
  if (FramelessSpills) {
    for (const auto &Row : machOFramelessSpillRows(Compact))
      if (!emitFrameRow(Row, OS))
        return std::nullopt;
  } else if (!Indexed)
    emitCompactFrame(Compact, OS);
  const uint8_t *Code = Image.Raw.data() + Text->FileOff + Start - Text->VA;
  bool HasCall = false;
  for (size_t Offset = 0; Offset < End - Start; Offset += Word)
    if (decodeDirectBranch(Image.Arch, InstructionMode::Default, Code + Offset,
                           Word, Start + Offset, {.Calls = true})) {
      HasCall = true;
      break;
    }
  llvm::SmallVector<ScratchFlow> Flow(HasCall ? (End - Start) / Word : 0);
  for (va_t PC = Start; PC < End; PC += Word) {
    while (!Rows.empty() && Rows.front().CodeOffset == PC - Start) {
      if (!emitFrameRow(Rows.front(), OS))
        return std::nullopt;
      Rows = Rows.drop_front();
    }
    DecodedInsn I{};
    if (Decode.decodeOne(Code + PC - Start, Word, PC, I) != int(Word) ||
        !I.Raw || !I.Raw->detail)
      return std::nullopt;
    Body.HasBareReturn |= End - Start == Word && I.Id == AARCH64_INS_RET;
    const auto &D = I.Raw->detail->aarch64;
    const auto Ops = llvm::ArrayRef(D.operands, D.op_count);
    const auto Encoding = readLE<uint32_t>(I.Raw->bytes);
    const bool SIMDData = (Encoding & kSIMDDataMask) == kSIMDDataClass;
    Body.HasSIMDData |= SIMDData;
    if (const auto Memory = aarch64AddressMemoryInstruction(Encoding))
      Body.HasMemoryIndex |= Memory->hasRegisterIndex() && !Memory->stackBase();
    if (const auto Memory = aarch64WritebackMemoryInstruction(Encoding))
      Body.HasMemoryWriteback |= Memory->hasWriteback() && !Memory->stackBase();
    OS << kLocal << (PC - Start) / Word << kEndLabel;

    Body.HasStackIndex |= llvm::any_of(Ops, [](const auto &Op) {
      return Op.type == AARCH64_OP_MEM && Op.mem.base == AARCH64_REG_SP &&
             Op.mem.index != AARCH64_REG_INVALID;
    });
    const size_t Index = (PC - Start) / Word;
    if (!Flow.empty()) {
      cs_regs Reads, Writes;
      uint8_t NumReads = 0, NumWrites = 0;
      if (cs_regs_access(Decode.getHandle(), I.Raw, Reads, &NumReads, Writes,
                         &NumWrites) != CS_ERR_OK)
        return std::nullopt;
      Flow[Index].Reads = scratchMask(llvm::ArrayRef(Reads, NumReads));
      Flow[Index].Writes = scratchMask(llvm::ArrayRef(Writes, NumWrites));
    }
    const auto Fallthrough = [&] {
      if (Index + 1 < Flow.size())
        Flow[Index + 1].Predecessors.push_back(Index);
    };
    if (I.Id == AARCH64_INS_BL) {
      if (Ops.size() != 1 || Ops[0].type != AARCH64_OP_IMM)
        return std::nullopt;
      const auto Target = Functions.find(Ops[0].imm);
      if (Target == Functions.end())
        return std::nullopt;
      Flow[Index].Callee = Target->second;
      Fallthrough();
      OS << kCall;
      emitOperand(Body, OS, Target->second, kSymbolConstraint);
      OS << '\n';
      continue;
    }
    const bool TestBit = I.Id == AARCH64_INS_TBZ || I.Id == AARCH64_INS_TBNZ;
    if (I.Id == AARCH64_INS_B || I.Id == AARCH64_INS_CBZ ||
        I.Id == AARCH64_INS_CBNZ || TestBit) {
      const bool TestReg = I.Id != AARCH64_INS_B;
      if (Ops.size() != (TestBit   ? 3u
                         : TestReg ? 2u
                                   : 1u) ||
          (TestBit && Ops[1].type != AARCH64_OP_IMM) ||
          Ops.back().type != AARCH64_OP_IMM ||
          (TestReg && Ops.front().type != AARCH64_OP_REG))
        return std::nullopt;
      const auto Target = va_t(Ops.back().imm);
      if (Target < Start || Target >= End) {
        const auto Tail = Functions.find(Target);
        if (TestReg || Tail == Functions.end() ||
            !decodeDirectBranch(Image.Arch, InstructionMode::Default,
                                I.Raw->bytes, Word, PC, {.Jumps = true}))
          return std::nullopt;
        Body.TailCallees.push_back(Tail->second);
        OS << kTail;
        emitOperand(Body, OS, Tail->second, kSymbolConstraint);
        OS << '\n';
        continue;
      }
      if ((Target - Start) % Word)
        return std::nullopt;
      if (!Flow.empty())
        Flow[(Target - Start) / Word].Predecessors.push_back(Index);
      if (!decodeDirectBranch(Image.Arch, InstructionMode::Default,
                              I.Raw->bytes, Word, PC, {.Jumps = true}))
        Fallthrough();
      OS << I.Raw->mnemonic << ' ';
      if (TestReg)
        OS << cs_reg_name(Decode.getHandle(), Ops.front().reg) << kSeparator;
      if (TestBit) {
        OS << '#';
        emitOperand(Body, OS,
                    llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(Owner->second->getContext()),
                        Ops[1].imm),
                    kImmediateConstraint);
        OS << kSeparator;
      }
      OS << kLocal << (Target - Start) / Word << '\n';
      continue;
    }
    const auto PCSigning = pcSigningInstruction(I.Id);
    if (PCSigning && PCSigning->Operands) {
      const auto Distance =
          ((Encoding >> kAArch64PAuthLRImmShift) & kAArch64PAuthLRImmMask) *
          kAArch64PAuthLRScale;
      if (Distance > PC - Start)
        return std::nullopt;
    }
    const bool PACM =
        I.Id == AARCH64_INS_HINT && I.Raw->alias_id == AARCH64_INS_ALIAS_PACM;
    // ADR can materialize a signing point within this same body. Its relative
    // distance survives publication, just like an internal branch. External
    // data and page-relative addresses require a different relocation owner.
    const bool LocalAddress =
        I.Id == AARCH64_INS_ADR && Ops.size() == 2 &&
        Ops[0].type == AARCH64_OP_REG && physicalRegister(Ops[0].reg) &&
        Ops[1].type == AARCH64_OP_IMM && va_t(Ops[1].imm) >= Start &&
        va_t(Ops[1].imm) < End && (va_t(Ops[1].imm) - Start) % Word == 0;
    if (I.Id != AARCH64_INS_RET && I.Id != AARCH64_INS_RETAA &&
        I.Id != AARCH64_INS_RETAB && !(PCSigning && PCSigning->Returns) &&
        I.Id != AARCH64_INS_BRK)
      Fallthrough();
    if (!SIMDData && !LocalAddress && !independent(I))
      return std::nullopt;
    // Classic compact mode cannot describe a saved PC-signed return. A
    // frameless body without calls or spills retains a live LR instead; the
    // asynchronous bridge authenticates its recovered recipe and supplies
    // the separate normalized LR used by the system cursor.
    if (!Indexed && !PAuthLR && (PCSigning || PACM) &&
        (Framed || HasCall || !Compact.SavedRegisterSlots.empty()))
      return std::nullopt;
    // Frameless variadic entries can load directly at or above incoming SP
    // without materializing va_list. Count the pre-indexed address, not the
    // post-indexed writeback displacement.
    if (!Indexed && !Framed &&
        (I.Id == AARCH64_INS_LDR || I.Id == AARCH64_INS_LDP))
      for (const auto &Op : Ops)
        if (Op.type == AARCH64_OP_MEM && Op.mem.base == AARCH64_REG_SP &&
            Op.mem.index == AARCH64_REG_INVALID &&
            (D.post_index ? 0 : Op.mem.disp) >= int64_t(Compact.StackSize) &&
            llvm::any_of(Ops, [](const auto &Dst) {
              return Dst.type == AARCH64_OP_REG &&
                     (Dst.reg == AARCH64_REG_X8 || Dst.reg == AARCH64_REG_X9 ||
                      Dst.reg == AARCH64_REG_W8 || Dst.reg == AARCH64_REG_W9);
            }))
          Body.LoadsStackArgument = true;
    if ((I.Id == AARCH64_INS_ADD || I.Id == AARCH64_INS_MOV) &&
        Ops.size() >= 2 && Ops[0].type == AARCH64_OP_REG &&
        (Ops[0].reg == AARCH64_REG_X8 || Ops[0].reg == AARCH64_REG_X9) &&
        Ops[1].type == AARCH64_OP_REG && Ops[1].reg == AARCH64_REG_SP)
      Body.MaterializesCursor = true;

    // Retain scalar FP/NEON words, including lanes, arrangements, register
    // lists and FP controls. PAuth_LR words retain their signing-PC distance
    // because every instruction keeps its offset within the copied body.
    // Both the assembly and final linked words are checked against the image.
    if (SIMDData || PCSigning || PACM || LocalAddress) {
      OS << kInstructionWord << Encoding << '\n';
    } else if ((I.Id == AARCH64_INS_MOV || I.Id == AARCH64_INS_MOVZ) &&
               Ops.size() == 2 && Ops[0].type == AARCH64_OP_REG &&
               Ops[1].type == AARCH64_OP_IMM && Ops[1].imm == 0 &&
               ((Ops[0].reg >= AARCH64_REG_X0 &&
                 Ops[0].reg <= AARCH64_REG_X28) ||
                (Ops[0].reg >= AARCH64_REG_W0 &&
                 Ops[0].reg <= AARCH64_REG_W30))) {
      // MOVZ zero and ORR from zero both preserve NZCV and clear the entire
      // physical destination. Use the register form including for a W result.
      llvm::StringRef Reg = cs_reg_name(Decode.getHandle(), Ops[0].reg);
      OS << kClear << Reg
         << (Ops[0].reg >= AARCH64_REG_W0 && Ops[0].reg <= AARCH64_REG_W30
                 ? kWZero
                 : kXZero)
         << '\n';
    } else {
      OS << I.Raw->mnemonic;
      if (!emitOperands(Body, OS, Decode, I, Owner->second->getContext()))
        return std::nullopt;
      OS << '\n';
    }
    if (PAuthLR && PC - Start == *Compact.ReturnAddressSigningOffset) {
      if (I.Id != AARCH64_INS_PACIBSPPC)
        return std::nullopt;
      OS << kSignWithPC;
    }
  }
  if (!llvm::all_of(Rows,
                    [](const auto &Row) { return Row.Kind == CFIOpKind::Nop; }))
    return std::nullopt;
  if (Body.HasLanguageFrame &&
      Frame->Itanium->LandingPadBaseEncoding != dweh::Omit &&
      !emitLanguageTable(Image, *Frame, *Owner->second->getParent(), Body, OS))
    return std::nullopt;
  observeScratchResults(Flow, Body);
  Body.Constraints += kEffects;
  return Body;
}

llvm::InlineAsm *assembly(llvm::Function &F, const RegisterBody &Body) {
  llvm::SmallVector<llvm::Type *> Parameters;
  for (const auto *Operand : Body.Operands)
    Parameters.push_back(Operand->getType());
  auto *Ty = llvm::FunctionType::get(llvm::Type::getVoidTy(F.getContext()),
                                     Parameters, false);
  return llvm::InlineAsm::get(Ty, Body.Assembly, Body.Constraints, true);
}
} // namespace

std::vector<CFIInstruction>
machOFramelessSpillRows(const CompactUnwindEntry &Compact) {
  // A zero outgoing-argument adjustment forces MC to retain an indexed FDE.
  // The system compact walker reads the first spill at CFA instead of CFA-8
  // and subtracts all pair slots from the restored SP. Preserve the actual
  // frame with explicit CFA-relative locations rather than that encoding.
  std::vector<CFIInstruction> Rows{
      {.Kind = CFIOpKind::GnuArgsSize},
      {.Kind = CFIOpKind::DefCFAOffset, .Offset = Compact.StackSize}};
  int64_t Offset = 0;
  for (const auto &Slot : Compact.SavedRegisterSlots) {
    Offset -= sizeof(uint64_t);
    Rows.push_back(
        {.Kind = CFIOpKind::Offset,
         .Register = Slot.Register +
                     (Slot.RegisterClass == UnwindRegisterClass::FloatingPoint
                          ? aarch64_unwind::kDwarfV0
                          : 0),
         .Offset = Offset});
  }
  return Rows;
}

bool canPublishMachOLanguageFrame(const BinaryImage &Image,
                                  const ExceptionFunction &Frame) {
  if (Frame.ParseStatus != ExceptionParseStatus::Complete || !Frame.Itanium ||
      !Frame.Compact || !Frame.FragmentRanges.empty() ||
      (Frame.Personality != ExceptionPersonality::GxxPersonalityV0 &&
       Frame.Personality != ExceptionPersonality::ObjCPersonalityV0))
    return false;
  const auto &LSDA = *Frame.Itanium;
  if (!LSDA.LSDAVA || !LSDA.IsCallSiteAddressForm)
    return false;
  const auto *Storage = Image.getSectionFor(LSDA.LSDAVA);
  if (!Storage || Storage->isWritable() || !Image.readVA(LSDA.LSDAVA, 1))
    return false;
  for (const auto &Site : LSDA.CallSites)
    if (!Frame.CodeRange.contains(Site.GuardedRange) ||
        (Site.LandingPadVA && !Frame.CodeRange.contains(Site.LandingPadVA)))
      return false;
  // Rebuilding an explicit base retains the address-free bytes referenced by
  // action links and exception specifications.
  if (LSDA.LandingPadBaseEncoding != dweh::Omit) {
    for (auto Range : {LSDA.ActionTableRange, LSDA.ExceptionSpecRange})
      if (Range.End < Range.Begin ||
          Range.size() > limits::loader::kLSDAMaxLSDABytes ||
          (Range.size() && !Image.readVA(Range.Begin, Range.size())))
        return false;
    const bool Indirect = LSDA.TypeTableEncoding & dweh::Indirect;
    for (const auto &Type : LSDA.TypeTable)
      if (!Type.IsCatchAll &&
          !(Indirect ? Type.TypeInfoSlotVA : Type.TypeInfoVA))
        return false;
  }
  if (!Frame.Dwarf)
    return (Frame.Compact->Kind == CompactUnwindKind::FramePointer ||
            Frame.Compact->Kind == CompactUnwindKind::FramelessImmediate) &&
           Frame.Compact->PersonalitySlotVA && Frame.Compact->HasLSDA &&
           Frame.Compact->LSDAVA == LSDA.LSDAVA;
  if (Frame.Compact->Kind != CompactUnwindKind::DwarfFDE ||
      LSDA.LSDAVA != Frame.Dwarf->LSDAVA)
    return false;
  const auto *CIE =
      Image.ExceptionMetadata.findCIE(Frame.Dwarf->CIESectionOffset);
  return CIE && CIE->ParseStatus == ExceptionParseStatus::Complete &&
         CIE->PersonalityEncoding ==
             (dweh::Indirect | dweh::PCRel | dweh::Sdata4) &&
         CIE->PersonalitySlotVA && !CIE->IsSignalFrame &&
         !CIE->UsesNonHintPAuthLR && !CIE->HasMTETaggedFrame &&
         (CIE->LSDAPointerEncoding == dweh::PCRel ||
          CIE->LSDAPointerEncoding == (dweh::PCRel | dweh::Sdata4));
}

bool needsMachONativeStackIndex(const MedFunc &Function) {
  return llvm::any_of(Function.Blocks, [](const MedBlock &Block) {
    return llvm::any_of(Block.Ops, [](const MedOp &Op) {
      if (!aarch64AddressMemoryMedShapeIsValid(Op))
        return false;
      const auto I = *aarch64AddressMemoryInstruction(
          Op.Inputs[kAddressMemoryWordOperand + 1].ConstVal);
      return I.stackBase() && I.hasRegisterIndex();
    });
  });
}

llvm::Error
lowerMachORegisterResults(llvm::Module &M, const BinaryImage &Image,
                          llvm::ArrayRef<va_t> AdmittedEntries,
                          llvm::ArrayRef<va_t> RequiredNativeEntries) {
  llvm::DenseSet<va_t> Required(RequiredNativeEntries.begin(),
                                RequiredNativeEntries.end());
  auto Functions = definedEntries(M, Image, Required);
  if (!Functions) {
    if (!Required.empty())
      return Functions.takeError();
    llvm::consumeError(Functions.takeError());
    return llvm::Error::success();
  }
  Decoder Decode;
  if (!Decode.init(Image))
    return Required.empty() ? llvm::Error::success() : invalidBody();
  llvm::DenseSet<va_t> Admitted(AdmittedEntries.begin(), AdmittedEntries.end());
  for (const auto &[VA, F] : *Functions) {
    const auto *Frame = Image.ExceptionMetadata.findFunction(VA);
    if (!Admitted.contains(VA) || !Frame ||
        !canPublishMachOLanguageFrame(Image, *Frame))
      continue;
    const auto &Table = *Frame->Itanium;
    auto *Byte = llvm::Type::getInt8Ty(M.getContext());
    if (Table.LandingPadBaseEncoding == dweh::Omit) {
      M.getOrInsertGlobal(makeNdDataSymbol(Table.LSDAVA), Byte);
      continue;
    }
    for (const auto &Type : Table.TypeTable)
      if (!Type.IsCatchAll)
        M.getOrInsertGlobal(
            makeNdDataSymbol(Table.TypeTableEncoding & dweh::Indirect
                                 ? Type.TypeInfoSlotVA
                                 : Type.TypeInfoVA),
            Byte);
  }
  std::map<llvm::Function *, RegisterBody> Bodies;
  bool HasVariadicCursor = false;
  for (auto It = Functions->begin(); It != Functions->end(); ++It) {
    auto &F = *It->second;
    if (!Admitted.contains(It->first) ||
        llvm::any_of(F, [](const auto &BB) { return BB.hasAddressTaken(); }))
      continue;
    if (auto Body = registerBody(Decode, Image, *Functions, It)) {
      HasVariadicCursor |= F.isVarArg() && Body->MaterializesCursor;
      Bodies.try_emplace(&F, std::move(*Body));
    }
  }
  llvm::DenseSet<llvm::Function *> Selected;
  llvm::SmallVector<llvm::Function *> Worklist;
  const auto Select = [&](llvm::Function *F) {
    if (Selected.insert(F).second)
      Worklist.push_back(F);
  };
  // A declaration withheld from ordinary LLVM emission must acquire its
  // complete machine implementation here or the whole patch fails.
  for (va_t VA : Required) {
    const auto Entry = Functions->find(VA);
    if (Entry == Functions->end())
      return invalidBody();
    const auto Body = Bodies.find(Entry->second);
    if (Body == Bodies.end() || !Body->second.HasStackIndex)
      return invalidBody();
    Select(Entry->second);
  }
  for (const auto &[F, Body] : Bodies) {
    // Keep physical addressing, vector lanes and FP status effects within
    // their complete native body, including runtime checked-pointer behavior.
    if (Body.HasMemoryWriteback || Body.HasMemoryIndex || Body.HasSIMDData ||
        Body.HasCompactFrame || Body.HasLanguageFrame || Body.HasBareReturn)
      Select(F);
    if (HasVariadicCursor &&
        (F->isVarArg() || Body.MaterializesCursor || Body.LoadsStackArgument))
      Select(F);
    for (auto *Callee : Body.ObservedCallees)
      if (Bodies.contains(Callee))
        Select(Callee);
  }
  if (Selected.empty())
    return llvm::Error::success();
  // Direct callers can pass the complete register result on to their own
  // callers, including through a chain of non-variadic functions.
  llvm::DenseMap<llvm::Function *, llvm::SmallVector<llvm::Function *>> Callers;
  for (const auto &[F, Body] : Bodies)
    for (auto *Operand : Body.Operands)
      if (auto *Callee = llvm::dyn_cast<llvm::Function>(Operand))
        Callers[Callee].push_back(F);
  while (!Worklist.empty()) {
    auto *F = Worklist.pop_back_val();
    for (auto *Target : Bodies.at(F).TailCallees)
      if (Bodies.contains(Target))
        Select(Target);
    for (auto *Caller : Callers.lookup(F))
      Select(Caller);
  }
  for (auto &[F, Body] : Bodies) {
    if (!Selected.contains(F))
      continue;
    for (auto &BB : *F)
      BB.dropAllReferences();
    while (!F->empty())
      F->begin()->eraseFromParent();
    F->addFnAttr(kRegisterBody);
    F->addFnAttr(llvm::Attribute::Naked);
    F->removeFnAttr(llvm::Attribute::AlwaysInline);
    F->addFnAttr(llvm::Attribute::NoInline);
    F->setMemoryEffects(llvm::MemoryEffects::unknown());
    // This proved native body owns its signing instructions and CFI. A
    // compiler prologue policy inferred from one source return cannot also
    // own this frame, especially when a landing pad changes signing recipes.
    for (auto Name : {kReturnSigningScopeAttribute, kReturnSigningKeyAttribute,
                      kReturnSigningArm64eAttribute, kReturnSigningPCAttribute})
      F->removeFnAttr(Name);
    auto Features = F->getFnAttribute(kReturnSigningFeaturesAttribute)
                        .getValueAsString()
                        .str();
    if (!Features.empty())
      Features += ',';
    Features += kReturnSigningFeature;
    F->addFnAttr(kReturnSigningFeaturesAttribute, Features);
    F->setUWTableKind(llvm::UWTableKind::Sync);
    llvm::IRBuilder<> B(
        llvm::BasicBlock::Create(M.getContext(), kEntryBlock, F));
    B.CreateCall(assembly(*F, Body), Body.Operands)->setDoesNotReturn();
    B.CreateUnreachable();
  }
  return llvm::Error::success();
}

llvm::Expected<llvm::DenseSet<va_t>>
validateMachORegisterResults(llvm::Module &M, const BinaryImage &Image) {
  llvm::DenseSet<va_t> Result;
  if (!llvm::any_of(
          M, [](const auto &F) { return F.hasFnAttribute(kRegisterBody); }))
    return Result;
  auto Functions = definedEntries(M, Image);
  if (!Functions)
    return Functions.takeError();
  Decoder Decode;
  if (!Decode.init(Image))
    return invalidBody();
  for (auto It = Functions->begin(); It != Functions->end(); ++It) {
    auto &F = *It->second;
    if (!F.hasFnAttribute(kRegisterBody))
      continue;
    if (!F.hasFnAttribute(llvm::Attribute::Naked) || F.size() != 1 ||
        F.front().size() != 2 ||
        !llvm::isa<llvm::UnreachableInst>(F.front().back()))
      return invalidBody();
    const auto *Call = llvm::dyn_cast<llvm::CallInst>(&F.front().front());
    auto Body = registerBody(Decode, Image, *Functions, It);
    if (!Call || !Body || !Call->doesNotReturn() ||
        Call->getCalledOperand() != assembly(F, *Body) ||
        !llvm::equal(Call->args(), Body->Operands))
      return invalidBody();
    Result.insert(It->first);
  }
  return Result;
}
} // namespace neverd
