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

#include "../CodeGenSymbolize.h"
#include "../rewrite/BlockAddressUse.h"
#include "MachOEmbeddedDispatch.h"
#include "MachOPageAddress.h"

#include "neverd/analysis/AArch64UnwindRecovery.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/AArch64/LLVMAArch64ReturnProtection.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/DirectBranch.h"
#include "neverd/loader/Limits.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/loader/RelocationProvenance.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/AArch64Literal.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/DwarfEH.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
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
#include <utility>

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
  std::vector<MachODataInCodeRange> RecoveredData;
  llvm::SmallVector<MachOLiftedSourceAddress, 0> SourceAddresses;
  llvm::SmallVector<va_t, 0> SourcePointerSlots;
  bool HasDataPage = false;
  bool HasEmbeddedData = false;
  bool HasCompactFrame = false;
  bool HasLanguageFrame = false;
  bool HasBareReturn = false;
  bool IsForwarder = false;
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

// A source entry can escape in X0 or in a caller-supplied output slot. Follow
// the value through full-width copies and offsets until the straight-line
// return.
bool exportsAddress(Decoder &Decode, llvm::ArrayRef<uint8_t> Bytes, va_t PC,
                    aarch64_reg AddressReg) {
  constexpr auto Word = aarch64::kInsnSize;
  bool Stored = false;
  while (Bytes.size() >= Word) {
    DecodedInsn I{};
    if (Decode.decodeOne(Bytes.data(), Word, PC, I) != int(Word) || !I.Raw ||
        !I.Raw->detail)
      return false;
    const auto &D = I.Raw->detail->aarch64;
    if ((I.Id == AARCH64_INS_RET && !D.op_count) || I.Id == AARCH64_INS_RETAA ||
        I.Id == AARCH64_INS_RETAB)
      return Stored || AddressReg == AARCH64_REG_X0;
    if (!D.op_count || D.operands[0].type != AARCH64_OP_REG)
      return false;
    if (D.op_count == 2 &&
        (I.Id == AARCH64_INS_STR || I.Id == AARCH64_INS_STUR) &&
        D.operands[0].reg == AddressReg &&
        D.operands[1].type == AARCH64_OP_MEM &&
        D.operands[1].mem.base != AddressReg &&
        D.operands[1].mem.index != AddressReg) {
      Stored = true;
    } else {
      const bool Copy =
          D.op_count == 2 && (I.Id == AARCH64_INS_MOV ||
                              (I.Id == AARCH64_INS_ORR && I.Raw->is_alias));
      const bool Offset =
          D.op_count == 3 &&
          (I.Id == AARCH64_INS_ADD || I.Id == AARCH64_INS_SUB) &&
          D.operands[2].type == AARCH64_OP_IMM;
      if ((!Copy && !Offset) || D.operands[0].reg < AARCH64_REG_X0 ||
          D.operands[0].reg > AARCH64_REG_X28 ||
          D.operands[1].type != AARCH64_OP_REG ||
          D.operands[1].reg != AddressReg)
        return false;
      AddressReg = D.operands[0].reg;
    }
    Bytes = Bytes.drop_front(Word);
    PC += Word;
  }
  return false;
}

// The existing PACM instruction table identifies the matching signing
// instruction and the consumer of the local signing PC in X16.
bool movesSigningPoint(const MachOLiftedSource &Source, va_t Start, va_t PC,
                       va_t Target, aarch64_reg Reg) {
  constexpr auto Word = aarch64::kInsnSize;
  if (Reg != AARCH64_REG_X16 || Target <= Start || Target % Word ||
      !rangeInBounds(Target - Start, Word, Source.Bytes.size()) ||
      Source.Bytes.size() - (PC - Start) < 3 * Word ||
      !machODataInCodeRanges(Source.Embedded, Target - Word, 2 * Word)
           .empty() ||
      !machODataInCodeRanges(Source.Embedded, PC + Word, 2 * Word).empty())
    return false;
  const auto At = [&](va_t VA) {
    return readLE<uint32_t>(Source.Bytes.data() + (VA - Start));
  };
  if (At(Target - Word) != kAArch64PACMEncoding ||
      At(PC + Word) != kAArch64PACMEncoding)
    return false;
  enum class Modifier { PC, X15, X16 };
  const auto Operation = [](uint32_t Bits, Modifier Expected) {
    switch (Bits) {
#define NEVERD_A64_PAUTH_M_CONSUMER(Encoding, Id, Mod2)                        \
  case Encoding:                                                               \
    if (Expected == Modifier::Mod2)                                            \
      return aarch64PAuthMOperation(Intrinsic::Id);                            \
    break;
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthM.def"
    }
    return std::optional<AArch64PAuthLROperation>{};
  };
  const auto Sign = Operation(At(Target), Modifier::PC);
  const auto Auth = Operation(At(PC + 2 * Word), Modifier::X16);
  return Sign && Auth && Sign->Key == Auth->Key;
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

std::optional<RegisterBody>
registerBody(Decoder &Decode, const BinaryImage &Image,
             const Entries &Functions, Entries::const_iterator Owner,
             const LowFunc *ControlFlow,
             llvm::function_ref<llvm::GlobalVariable *(va_t)> DataSymbol,
             bool Revalidate = false) {
  const va_t Start = Owner->first;
  auto Next = std::next(Owner);
  auto Source = machOLiftedSource(
      Image, Start, Next == Functions.end() ? InvalidVA : Next->first);
  if (!Source)
    return std::nullopt;
  if (Source->ForwardTarget) {
    const auto Target = Functions.find(*Source->ForwardTarget);
    if (Target == Functions.end())
      return std::nullopt;
    RegisterBody Body;
    Body.IsForwarder = true;
    Body.TailCallees.push_back(Target->second);
    llvm::raw_string_ostream OS(Body.Assembly);
    OS << kTail;
    emitOperand(Body, OS, Target->second, kSymbolConstraint);
    OS << '\n';
    Body.Constraints += kEffects;
    return Body;
  }
  const va_t End = Start + Source->Bytes.size();
  const size_t Word = aarch64::kInsnSize;
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

  // Pipeline lowering borrows the established CFG. At the patch boundary the
  // LLVM module is caller-supplied, so derive the edges from the image again.
  std::optional<LowFunc> OriginalControl;
  MachOEmbeddedDispatch Dispatch;
  if (Revalidate && (Owner->second->hasFnAttribute(kRecoveredData) ||
                     !Source->Embedded.empty())) {
    OriginalControl = CFGBuilder().build(Image, Decode, Start);
    ControlFlow = &*OriginalControl;
  }
  llvm::SmallVector<MachODataInCodeRange> EffectiveData;
  if (ControlFlow) {
    auto Layout = machOEmbeddedDispatch(Image, Decode, *Source, *ControlFlow);
    if (!Layout)
      return std::nullopt;
    Dispatch = std::move(*Layout);
    if (!Dispatch.Recovered.empty()) {
      llvm::append_range(EffectiveData, Source->Embedded);
      llvm::append_range(EffectiveData, Dispatch.Recovered);
      llvm::sort(EffectiveData, [](const auto &L, const auto &R) {
        return L.Begin < R.Begin;
      });
      Source->Embedded = EffectiveData;
    }
  }

  RegisterBody Body;
  Body.RecoveredData = std::move(Dispatch.Recovered);
  Body.HasEmbeddedData = !Source->Embedded.empty();
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
  const uint8_t *Code = Source->Bytes.data();
  bool HasCall = false;
  for (size_t Offset = 0; Offset < End - Start; Offset += Word)
    if (machODataInCodeRanges(Source->Embedded, Start + Offset, Word).empty() &&
        decodeDirectBranch(Image.Arch, InstructionMode::Default, Code + Offset,
                           Word, Start + Offset, {.Calls = true})) {
      HasCall = true;
      break;
    }
  llvm::SmallVector<ScratchFlow> Flow(HasCall ? (End - Start) / Word : 0);
  llvm::GlobalVariable *PageOffset = nullptr;
  auto Islands = Source->Embedded;
  auto Unreached = llvm::ArrayRef(Dispatch.UnreachedCode);
  for (va_t PC = Start, NextPC; PC < End; PC = NextPC) {
    NextPC = PC + Word;
    while (!Rows.empty() && Rows.front().CodeOffset == PC - Start) {
      if (!emitFrameRow(Rows.front(), OS))
        return std::nullopt;
      Rows = Rows.drop_front();
    }
    if (!Islands.empty() && Islands.front().Begin == PC) {
      const auto R = Islands.front();
      const char *Directive = nullptr;
      switch (R.Kind) {
#define MACHO_REGISTER_REGION(Kind, Text)                                      \
  case llvm::MachO::Kind:                                                      \
    Directive = Text;                                                          \
    break;
#include "MachORegisterResults.def"
      default:
        return std::nullopt;
      }
      if (hasRelocationProvenanceIn(Image, R.Begin, R.End))
        return std::nullopt;
      OS << Directive << '\n';
      for (const auto Byte : Source->Bytes.slice(PC - Start, R.End - PC))
        OS << kByte << unsigned(Byte) << '\n';
      OS << kEndDataRegion;
      NextPC = R.End;
      Islands = Islands.drop_front();
      continue;
    }
    if (PC % Word || End - PC < Word ||
        (!Islands.empty() && Islands.front().Begin - PC < Word))
      return std::nullopt;
    DecodedInsn I{};
    if (Decode.decodeOne(Code + PC - Start, Word, PC, I) != int(Word) ||
        !I.Raw || !I.Raw->detail)
      return std::nullopt;
    Body.HasBareReturn |= End - Start == Word && I.Id == AARCH64_INS_RET;
    const auto &D = I.Raw->detail->aarch64;
    const auto Ops = llvm::ArrayRef(D.operands, D.op_count);
    const bool Jump =
        decodeDirectBranch(Image.Arch, InstructionMode::Default, I.Raw->bytes,
                           Word, PC, {.Jumps = true})
            .has_value();
    const auto PCSigning = pcSigningInstruction(I.Id);
    const bool PACM =
        I.Id == AARCH64_INS_HINT && I.Raw->alias_id == AARCH64_INS_ALIAS_PACM;
    while (!Unreached.empty() && Unreached.front().second <= PC)
      Unreached = Unreached.drop_front();
    const bool OutsideCFG = !Unreached.empty() && Unreached.front().first <= PC;
    const bool KeepEncoding =
        OutsideCFG && !PCSigning && !PACM && independent(I);
    const bool RecoveredCode = OutsideCFG && !Body.RecoveredData.empty();
    const auto Targets = Dispatch.BranchTargets.lookup(PC);
    const bool Stops = Jump || !Targets.empty() || I.Id == AARCH64_INS_RET ||
                       I.Id == AARCH64_INS_RETAA || I.Id == AARCH64_INS_RETAB ||
                       I.Id == AARCH64_INS_BRK ||
                       (PCSigning && PCSigning->Returns);
    if (!Stops && !Islands.empty() && Islands.front().Begin == NextPC)
      return std::nullopt;
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
    if (I.Id == AARCH64_INS_BR) {
      if (Targets.empty())
        return std::nullopt;
      if (!Flow.empty())
        for (const auto Target : Targets) {
          auto &Predecessors = Flow[(Target - Start) / Word].Predecessors;
          if (Predecessors.empty() || Predecessors.back() != Index)
            Predecessors.push_back(Index);
        }
      OS << kInstructionWord << Encoding << '\n';
      continue;
    }
    if (I.Id == AARCH64_INS_BL) {
      if (RecoveredCode || Ops.size() != 1 || Ops[0].type != AARCH64_OP_IMM)
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
        if (RecoveredCode || TestReg || Tail == Functions.end() ||
            !decodeDirectBranch(Image.Arch, InstructionMode::Default,
                                I.Raw->bytes, Word, PC, {.Jumps = true}))
          return std::nullopt;
        Body.TailCallees.push_back(Tail->second);
        OS << kTail;
        emitOperand(Body, OS, Tail->second, kSymbolConstraint);
        OS << '\n';
        continue;
      }
      if ((Target - Start) % Word ||
          !machODataInCodeRanges(Source->Embedded, Target, Word).empty())
        return std::nullopt;
      if (!Flow.empty())
        Flow[(Target - Start) / Word].Predecessors.push_back(Index);
      if (!Jump)
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
    if (PCSigning && PCSigning->Operands) {
      const auto Distance =
          ((Encoding >> kAArch64PAuthLRImmShift) & kAArch64PAuthLRImmMask) *
          kAArch64PAuthLRScale;
      if (Distance > PC - Start)
        return std::nullopt;
    }
    if (PageOffset) {
      // The preceding ADRP proved these physical ADD operands. Emit PAGEOFF12
      // at its own label so CFI rows and incoming branch offsets stay intact.
      OS << I.Raw->mnemonic << ' '
         << cs_reg_name(Decode.getHandle(), Ops[0].reg) << kSeparator
         << cs_reg_name(Decode.getHandle(), Ops[1].reg) << kSeparator;
      emitOperand(Body, OS, PageOffset, kSymbolConstraint);
      OS << kPageOffset;
      PageOffset = nullptr;
      Fallthrough();
      continue;
    }
    // A page pair can materialize an original instruction or embedded byte.
    // Keep the ADD as a distinct instruction, including its result register.
    std::optional<MachOPageAddress> NumericPage;
    if (I.Id == AARCH64_INS_ADRP) {
      auto Pair = decodeMachOPageAddress(
          Decode, Source->Bytes.drop_front(PC - Start), PC);
      if (!Pair)
        return std::nullopt;
      const auto *Storage = Image.getSectionFor(Pair->Target);
      if (Storage && Storage->isReadable() &&
          !(Storage->Type & (llvm::MachO::S_ATTR_PURE_INSTRUCTIONS |
                             llvm::MachO::S_ATTR_SOME_INSTRUCTIONS)) &&
          Pair->Target - Storage->VA < Storage->FileSz) {
        PageOffset = DataSymbol(Pair->Target);
        if (!PageOffset)
          return std::nullopt;
        Body.HasDataPage = true;
        OS << kAddressPage
           << cs_reg_name(Decode.getHandle(), Pair->PageRegister) << kSeparator;
        emitOperand(Body, OS, PageOffset, kSymbolConstraint);
        OS << kPage;
        Fallthrough();
        continue;
      }
      NumericPage = Pair;
    }
    // A table or signing point moves with this body. An exported numeric
    // address denotes original storage even when lifting a published copy.
    const auto Rebased = aarch64::isPointerLiteralLoad(Encoding)
                             ? machORebasedLiteralAddress(Image, Encoding, PC)
                             : std::nullopt;
    if (NumericPage || I.Id == AARCH64_INS_ADR || Rebased) {
      if (!NumericPage && (Ops.size() != 2 || Ops[0].type != AARCH64_OP_REG ||
                           (!Rebased && Ops[1].type != AARCH64_OP_IMM)))
        return std::nullopt;
      const va_t Target = NumericPage ? NumericPage->Target
                          : Rebased   ? *Rebased
                                      : va_t(Ops[1].imm);
      const auto Data = machODataInCodeRanges(Image.MachODataInCode, Target, 1);
      const bool LiteralAddress =
          (!Data.empty() && Data.front().Kind == llvm::MachO::DICE_KIND_DATA) ||
          coverMachOEmbeddedRead(*Source, Start, Target, 1,
                                 llvm::MachO::DICE_KIND_DATA);
      const auto Reg = NumericPage ? NumericPage->ResultRegister : Ops[0].reg;
      if (Reg == AARCH64_REG_XZR)
        return std::nullopt;
      const bool Moves = !NumericPage && !Rebased && !OutsideCFG &&
                         (Dispatch.TableAddresses.contains(PC) ||
                          movesSigningPoint(*Source, Start, PC, Target, Reg));
      if (!Moves) {
        // The lookahead consumes the borrowed instruction. The source word
        // and destination register have already been copied.
        if (!LiteralAddress) {
          if (!Image.hasAuthenticatedFunctionEntryAt(Target)) {
            // An unvisited word can expose its own PC. Its unwind owner
            // identifies the storage, but gives no evidence of a value use.
            if (OutsideCFG)
              return std::nullopt;
            const auto Owner = machOInteriorAddressOwner(Image, Target);
            if (!Owner || !Functions.contains(*Owner))
              return std::nullopt;
          }
          const va_t Use = NumericPage ? NextPC + Word : NextPC;
          if (!exportsAddress(Decode, Source->Bytes.drop_front(Use - Start),
                              Use, Reg))
            return std::nullopt;
        }
        Body.SourceAddresses.push_back({PC, Target,
                                        NumericPage ? MachOAddressForm::PagePair
                                                    : MachOAddressForm::Single,
                                        OS.tell()});
        if (Rebased)
          Body.SourcePointerSlots.push_back(
              aarch64::decodeLiteralLoad(Encoding, PC)->Address);
      }
      Fallthrough();
      OS << kInstructionWord << Encoding << '\n';
      continue;
    }
    const auto Literal = aarch64::decodeLiteralLoad(Encoding, PC);
    if (Literal &&
        !coverMachOEmbeddedRead(*Source, Start, Literal->Address,
                                Literal->Width, llvm::MachO::DICE_KIND_DATA))
      return std::nullopt;
    // Local branches above and typed literal loads keep their displacement.
    // Other instructions outside the CFG must keep a PC-independent word.
    if (RecoveredCode && !KeepEncoding && !Literal)
      return std::nullopt;
    if (!Stops)
      Fallthrough();
    if (!SIMDData && !Literal && !KeepEncoding && !independent(I))
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
    if (KeepEncoding || SIMDData || PCSigning || PACM || Literal) {
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
        (Site.LandingPadVA &&
         (!Frame.CodeRange.contains(Site.LandingPadVA) ||
          !Image.hasExecutableCodeOwnerAt(Site.LandingPadVA))))
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
                          llvm::ArrayRef<va_t> RequiredNativeEntries,
                          llvm::ArrayRef<LowFunc> SourceFunctions) {
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
  auto *Byte = llvm::Type::getInt8Ty(M.getContext());
  for (const auto &[VA, F] : *Functions) {
    const auto *Frame = Image.ExceptionMetadata.findFunction(VA);
    if (!Admitted.contains(VA) || !Frame ||
        !canPublishMachOLanguageFrame(Image, *Frame))
      continue;
    const auto &Table = *Frame->Itanium;
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
  llvm::DenseMap<va_t, const LowFunc *> SourceByEntry;
  for (const auto &F : SourceFunctions)
    SourceByEntry.try_emplace(F.Entry, &F);
  llvm::SmallVector<llvm::GlobalVariable *> NewData;
  const auto DiscardUnused = llvm::scope_exit([&] {
    for (auto *G : NewData)
      if (G->use_empty())
        G->eraseFromParent();
  });
  const auto DataSymbol = [&](va_t Target) -> llvm::GlobalVariable * {
    const auto Name = makeNdDataSymbol(Target);
    if (auto *Existing = M.getNamedValue(Name))
      return llvm::dyn_cast<llvm::GlobalVariable>(Existing);
    auto *G = M.getOrInsertGlobal(Name, Byte);
    NewData.push_back(G);
    return G;
  };
  llvm::SmallVector<std::pair<llvm::Function *, bool>> AddressTaken;
  llvm::DenseSet<const llvm::Function *> AddressUsers;
  llvm::SmallVector<va_t> SourceSlots;
  for (auto It = Functions->begin(); It != Functions->end(); ++It) {
    auto &F = *It->second;
    if (!Admitted.contains(It->first))
      continue;
    const auto *Source = SourceByEntry.lookup(It->first);
    if (auto Body =
            registerBody(Decode, Image, *Functions, It, Source, DataSymbol)) {
      if (!Body->SourceAddresses.empty())
        AddressUsers.insert(&F);
      llvm::append_range(SourceSlots, Body->SourcePointerSlots);
      if (llvm::any_of(F, [](const auto &BB) { return BB.hasAddressTaken(); }))
        AddressTaken.emplace_back(&F, Source && !Source->JumpTables.empty());
      Bodies.try_emplace(&F, std::move(*Body));
    }
  }
  // A pointer mirror may hold the old label in its initializer. dyld owns
  // the actual cell. Bind its IR mirror before testing which labels escape.
  llvm::SmallVector<llvm::GlobalVariable *> SourceMirrors;
  if (!SourceSlots.empty()) {
    llvm::sort(SourceSlots);
    for (auto &G : M.globals()) {
      const auto Base = parseNdCodePtrSymbol(G.getName());
      if (!Base || !G.isConstant() || G.isThreadLocal() ||
          G.getAddressSpace() || (G.hasInitializer() && !G.hasLocalLinkage()))
        continue;
      const auto Slot = llvm::lower_bound(SourceSlots, *Base);
      if (Slot == SourceSlots.end())
        continue;
      const auto Size = M.getDataLayout().getTypeAllocSize(G.getValueType());
      if (Size.isScalable() ||
          !rangeInBounds(*Slot - *Base, sizeof(va_t), Size.getFixedValue()))
        continue;
      declareOriginalPointerMirror(G);
      SourceMirrors.push_back(&G);
    }
  }
  // A peer may store the address of an exact basic-block label. Its proved
  // native body records the original address and will discard that IR use.
  // A label referenced by surviving IR or metadata must keep its old body.
  llvm::DenseMap<const llvm::Function *, llvm::SmallVector<llvm::Function *>>
      DependsOn;
  llvm::SmallVector<llvm::Function *> Retain;
  for (auto [F, LocalDispatch] : AddressTaken) {
    const auto Escape = blockAddressEscape(*F, [&](const auto *User) {
      if (User == F && LocalDispatch)
        return true;
      if (!AddressUsers.contains(User))
        return false;
      if (User != F)
        DependsOn[User].push_back(F);
      return true;
    });
    if (!Escape.empty())
      Retain.push_back(F);
  }
  while (!Retain.empty()) {
    auto *F = Retain.pop_back_val();
    if (Bodies.erase(F))
      llvm::append_range(Retain, DependsOn.lookup(F));
  }
  const bool HasVariadicCursor = llvm::any_of(Bodies, [](const auto &Item) {
    return Item.first->isVarArg() && Item.second.MaterializesCursor;
  });
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
        Body.HasCompactFrame || Body.HasLanguageFrame || Body.HasBareReturn ||
        Body.IsForwarder || Body.HasEmbeddedData || Body.HasDataPage ||
        !Body.SourceAddresses.empty())
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
  // Drop uses across the whole selection before destroying a peer's label.
  for (auto *F : Selected)
    for (auto &BB : *F)
      BB.dropAllReferences();
  for (auto &[F, Body] : Bodies) {
    if (!Selected.contains(F))
      continue;
    while (!F->empty())
      F->begin()->eraseFromParent();
    F->addFnAttr(kRegisterBody);
    if (!Body.RecoveredData.empty())
      F->addFnAttr(kRecoveredData);
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
  for (auto *G : SourceMirrors) {
    G->removeDeadConstantUsers();
    if (G->use_empty() && !G->isUsedByMetadata())
      G->eraseFromParent();
  }
  return llvm::Error::success();
}

llvm::Expected<llvm::DenseSet<va_t>> validateMachORegisterResults(
    llvm::Module &M, const BinaryImage &Image,
    std::vector<MachODataInCodeRange> *RecoveredData,
    std::vector<MachOLiftedSourceAddress> *SourceAddresses) {
  llvm::DenseSet<va_t> Result;
  if (!llvm::any_of(
          M, [](const auto &F) { return F.hasFnAttribute(kRegisterBody); })) {
    if (RecoveredData)
      RecoveredData->clear();
    if (SourceAddresses)
      SourceAddresses->clear();
    return Result;
  }
  auto Functions = definedEntries(M, Image);
  if (!Functions)
    return Functions.takeError();
  Decoder Decode;
  if (!Decode.init(Image))
    return invalidBody();
  std::vector<MachODataInCodeRange> VerifiedData;
  std::vector<MachOLiftedSourceAddress> VerifiedAddresses;
  for (auto It = Functions->begin(); It != Functions->end(); ++It) {
    auto &F = *It->second;
    if (!F.hasFnAttribute(kRegisterBody))
      continue;
    if (!F.hasFnAttribute(llvm::Attribute::Naked) || F.size() != 1 ||
        F.front().size() != 2 ||
        !llvm::isa<llvm::UnreachableInst>(F.front().back()))
      return invalidBody();
    const auto *Call = llvm::dyn_cast<llvm::CallInst>(&F.front().front());
    auto Body = registerBody(
        Decode, Image, *Functions, It, nullptr,
        [&](va_t Target) { return M.getNamedGlobal(makeNdDataSymbol(Target)); },
        true);
    if (!Call || !Body || !Call->doesNotReturn() ||
        Call->getCalledOperand() != assembly(F, *Body) ||
        !llvm::equal(Call->args(), Body->Operands))
      return invalidBody();
    if (F.hasFnAttribute(kRecoveredData) != !Body->RecoveredData.empty())
      return invalidBody();
    if (RecoveredData)
      llvm::append_range(VerifiedData, Body->RecoveredData);
    if (SourceAddresses)
      llvm::append_range(VerifiedAddresses, Body->SourceAddresses);
    Result.insert(It->first);
  }
  if (RecoveredData)
    *RecoveredData = std::move(VerifiedData);
  if (SourceAddresses)
    *SourceAddresses = std::move(VerifiedAddresses);
  return Result;
}
} // namespace neverd
