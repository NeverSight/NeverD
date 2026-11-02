//===- PlatformEvidence.cpp - What code shows of its platform -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// x86-64 code shows its calling convention at every call: System V passes
// the first argument in rdi, Win64 in rcx, and only Win64 preserves rsi and
// rdi or spills arguments to the home slots above the return address.  The
// argument and preserved registers come from TargetRegInfo, so the clues
// follow the same tables decompiling does.  Other clues -- thread blocks,
// system calls, library names -- are rows of PlatformEvidence.def.
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/PlatformEvidence.h"

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/Raw/RawLoader.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"

#include <array>
#include <limits>
#include <optional>
#include <tuple>

namespace neverd {
namespace {

struct ClueInfo {
  BinaryFormat Platform;
  uint64_t Weight;
  llvm::StringLiteral Text;
};

constexpr ClueInfo ClueTable[] = {
#define NEVERD_PLATFORM_CLUE(Id, Platform, Weight, Text)                       \
  {BinaryFormat::Platform, Weight, Text},
#include "neverd/ir/low/PlatformEvidence.def"
};
constexpr size_t ClueCount = std::size(ClueTable);

constexpr BinaryFormat Platforms[] = {BinaryFormat::ELF, BinaryFormat::COFF,
                                      BinaryFormat::MachO};

const ClueInfo &infoOf(PlatformClue Clue) {
  return ClueTable[static_cast<size_t>(Clue)];
}

/// The clues seen so far, and the platforms they point to.
class Tally {
public:
  void add(PlatformClue Clue) { ++Counts[static_cast<size_t>(Clue)]; }
  /// An x86 instruction the sweep read, which evidence must be dense among.
  void read() { ++Read; }
  uint64_t count(size_t Clue) const { return Counts[Clue]; }

  uint64_t score(BinaryFormat Platform) const {
    uint64_t Sum = 0;
    for (size_t I = 0; I < ClueCount; ++I)
      if (ClueTable[I].Platform == Platform)
        Sum += Counts[I] * ClueTable[I].Weight;
    return Sum;
  }

  /// The leading platform, its score and the runner-up's.
  std::tuple<BinaryFormat, uint64_t, uint64_t> standing() const {
    BinaryFormat Leader = BinaryFormat::ELF;
    uint64_t Top = 0, Second = 0;
    for (BinaryFormat Platform : Platforms) {
      const uint64_t Score = score(Platform);
      if (Score > Top) {
        Second = Top;
        Top = Score;
        Leader = Platform;
      } else if (Score > Second) {
        Second = Score;
      }
    }
    return {Leader, Top, Second};
  }

  /// Whether evidence of \p Top points against the runner-up's \p Second
  /// decides, at \p Least points and \p Ratio times the runner-up's, and
  /// dense enough among the x86 instructions read.
  bool decides(uint64_t Top, uint64_t Second, uint64_t Least,
               uint64_t Ratio) const {
    return Top >= Least && Top >= Second * Ratio &&
           Top * limits::kPlatformInstructionsPerScore >= Read;
  }

  /// Whether reading further could not change the decision.
  bool decisive() const {
    const auto [Leader, Top, Second] = standing();
    (void)Leader;
    return decides(Top, Second, limits::kPlatformDecisiveScore,
                   limits::kPlatformDecisiveRatio);
  }

private:
  std::array<uint64_t, ClueCount> Counts{};
  uint64_t Read = 0;
};

struct SegmentRead {
  PlatformClue Clue;
  Arch TheArch;
  x86_reg Segment;
  int64_t Offset;
};

constexpr SegmentRead SegmentReads[] = {
#define NEVERD_PLATFORM_SEGMENT(Clue, A, Seg, Offset)                          \
  {PlatformClue::Clue, Arch::A, X86_REG_##Seg, Offset},
#include "neverd/ir/low/PlatformEvidence.def"
};

struct Interrupt {
  PlatformClue Clue;
  Arch TheArch;
  int64_t Vector;
};

constexpr Interrupt Interrupts[] = {
#define NEVERD_PLATFORM_INTERRUPT(Clue, A, Vector)                             \
  {PlatformClue::Clue, Arch::A, Vector},
#include "neverd/ir/low/PlatformEvidence.def"
};

struct AbsoluteRead {
  PlatformClue Clue;
  uint64_t Low, High;
};

constexpr AbsoluteRead AbsoluteReads[] = {
#define NEVERD_PLATFORM_ABSOLUTE(Clue, Low, High)                              \
  {PlatformClue::Clue, Low, High},
#include "neverd/ir/low/PlatformEvidence.def"
};

struct WordPattern {
  PlatformClue Clue;
  Arch TheArch;
  InstructionMode Mode;
  uint32_t Mask, Value;
};

constexpr WordPattern WordPatterns[] = {
#define NEVERD_PLATFORM_WORD(Clue, A, M, Mask, Value)                          \
  {PlatformClue::Clue, Arch::A, InstructionMode::M, Mask, Value},
#include "neverd/ir/low/PlatformEvidence.def"
};

struct Marker {
  PlatformClue Clue;
  llvm::StringLiteral Text;
};

constexpr Marker Markers[] = {
#define NEVERD_PLATFORM_STRING(Clue, Text) {PlatformClue::Clue, Text},
#include "neverd/ir/low/PlatformEvidence.def"
};

/// The x86-64 registers the call and prologue clues read, from the tables
/// decompiling follows.
struct X64Conventions {
  uint64_t SysVFirst = 0, Win64First = 0;
  /// Argument registers of System V that Win64 does not pass arguments in.
  llvm::SmallVector<uint64_t, 2> SysVOnly;
  /// Registers Win64 preserves that System V passes arguments in.
  llvm::SmallVector<uint64_t, 2> Win64Preserved;
  llvm::ArrayRef<uint64_t> Win64Arguments;

  explicit X64Conventions(const TargetRegInfo &TRI) {
    const llvm::ArrayRef<uint64_t> SysV =
        TRI.integerParamRegs(BinaryFormat::ELF);
    Win64Arguments = TRI.integerParamRegs(BinaryFormat::COFF);
    SysVFirst = SysV.front();
    Win64First = Win64Arguments.front();
    for (uint64_t Reg : SysV) {
      if (!llvm::is_contained(Win64Arguments, Reg))
        SysVOnly.push_back(Reg);
      if (TRI.isCallPreserved(Reg, TRI.PointerSize, BinaryFormat::COFF))
        Win64Preserved.push_back(Reg);
    }
  }
};

/// Whether an x86 instruction carries an FS or GS segment prefix.
bool hasThreadSegmentPrefix(const uint8_t *Bytes, int Size) {
  constexpr uint8_t Prefixes[] = {0xf0, 0xf2, 0xf3, 0x2e, 0x36, 0x3e,
                                  0x26, 0x64, 0x65, 0x66, 0x67};
  for (int I = 0; I < Size && llvm::is_contained(Prefixes, Bytes[I]); ++I)
    if (Bytes[I] == 0x64 || Bytes[I] == 0x65)
      return true;
  return false;
}

/// Whether an x86 instruction's bytes hold an address one of the fixed
/// reads names.
bool mentionsFixedAddress(const uint8_t *Bytes, int Size) {
  for (int I = 0; I + 4 <= Size; ++I) {
    const uint32_t Value = llvm::support::endian::read32le(Bytes + I);
    for (const AbsoluteRead &Read : AbsoluteReads)
      if (Value >= Read.Low && Value < Read.High)
        return true;
  }
  return false;
}

/// Whether an x86 instruction is a conditional branch, which ends the
/// instructions a call's arguments are set in but starts no function.
bool isConditionalBranch(const uint8_t *Bytes, int Size) {
  return (Size >= 1 && Bytes[0] >= 0x70 && Bytes[0] <= 0x7f) ||
         (Size >= 2 && Bytes[0] == 0x0f && Bytes[1] >= 0x80 &&
          Bytes[1] <= 0x8f);
}

void readX86(const BinaryImage &Img, Decoder &Dec, Tally &Clues) {
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  // i386 passes arguments on the stack under either platform.
  std::optional<X64Conventions> Conventions;
  if (Img.Arch == Arch::X64)
    Conventions.emplace(TRI);
  // The image's extent.  A call that code makes lands in it or beside it,
  // as a binary file's text calls the stubs next to it; most of the calls
  // data decodes to land far away.
  int64_t Low = std::numeric_limits<int64_t>::max(), High = 0;
  for (const Segment &Seg : Img.Segments) {
    Low = std::min(Low, static_cast<int64_t>(Seg.VA));
    High = std::max(High, static_cast<int64_t>(Seg.VA + Seg.Data.size()));
  }
  const auto isCall = [&](const uint8_t *Bytes, int Size, va_t At) {
    // call rel32, to a target in or beside the image.
    if (Size == 5 && Bytes[0] == 0xe8) {
      const int64_t Target =
          static_cast<int64_t>(At) + Size +
          static_cast<int32_t>(llvm::support::endian::read32le(Bytes + 1));
      return Target >= Low - limits::kPlatformCallReach &&
             Target < High + limits::kPlatformCallReach;
    }
    // call [rip+disp32] (call [disp32] in i386): an import's slot.
    return Size == 6 && Bytes[0] == 0xff && Bytes[1] == 0x15;
  };

  // Most instructions need only their size and kind; the few whose operands
  // tell are decoded again with operand detail, which comes only with text.
  const bool KeptDetail = Dec.detailEnabled(), KeptText = Dec.textEnabled();
  const auto detailed = [&](const Segment &Seg, size_t Off,
                            DecodedInsn &DI) -> const cs_x86 * {
    Dec.setText(true);
    Dec.setDetail(true);
    const int Size = Dec.decodeOneLight(
        Seg.Data.data() + Off, Seg.Data.size() - Off, Seg.VA + Off, DI);
    Dec.setText(false);
    Dec.setDetail(false);
    return Size > 0 && DI.Raw && DI.Raw->detail ? &DI.Raw->detail->x86
                                                : nullptr;
  };
  Dec.setText(false);
  Dec.setDetail(false);

  uint64_t Budget = limits::kPlatformEvidenceInstructions;
  for (const Segment &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.empty())
      continue;
    // The instructions since the last transfer of control, whose writes a
    // call's arguments are; whether a function may start at the next
    // instruction; and the instructions of a function start left to read.
    llvm::SmallVector<std::pair<size_t, int>, 8> Window;
    bool AfterBoundary = true;
    unsigned PrologueLeft = 0;
    for (size_t Off = 0; Off < Seg.Data.size() && Budget != 0;) {
      const uint8_t *Bytes = Seg.Data.data() + Off;
      const va_t At = Seg.VA + Off;
      DecodedInsn DI;
      const int Size = Dec.decodeOneLight(Bytes, Seg.Data.size() - Off, At, DI);
      if (Size <= 0) {
        ++Off;
        Window.clear();
        AfterBoundary = false;
        PrologueLeft = 0;
        continue;
      }
      --Budget;
      Clues.read();
      const unsigned Id = DI.Id;
      const bool Padding = Id == X86_INS_NOP || Id == X86_INS_INT3;
      // Compilers start functions aligned, after a return, a jump or the
      // padding that follows one.
      if (AfterBoundary && !Padding) {
        PrologueLeft = At % limits::kPlatformFunctionAlignment == 0
                           ? limits::kPlatformPrologueInstructions
                           : 0;
        AfterBoundary = false;
      }

      DecodedInsn Full;
      if (hasThreadSegmentPrefix(Bytes, Size) ||
          mentionsFixedAddress(Bytes, Size))
        if (const cs_x86 *X86 = detailed(Seg, Off, Full))
          for (const cs_x86_op &Op :
               llvm::ArrayRef<cs_x86_op>(X86->operands, X86->op_count)) {
            if (Op.type != X86_OP_MEM || Op.mem.base != X86_REG_INVALID ||
                Op.mem.index != X86_REG_INVALID)
              continue;
            for (const SegmentRead &Read : SegmentReads)
              if (Read.TheArch == Img.Arch && Read.Segment == Op.mem.segment &&
                  Read.Offset == Op.mem.disp)
                Clues.add(Read.Clue);
            if (Op.mem.segment == X86_REG_INVALID)
              for (const AbsoluteRead &Read : AbsoluteReads)
                if (static_cast<uint64_t>(Op.mem.disp) >= Read.Low &&
                    static_cast<uint64_t>(Op.mem.disp) < Read.High)
                  Clues.add(Read.Clue);
          }

      // int imm8 is CD ib.
      if (Id == X86_INS_INT && Size == 2 && Bytes[0] == 0xcd)
        for (const Interrupt &Vector : Interrupts)
          if (Vector.TheArch == Img.Arch && Vector.Vector == Bytes[1])
            Clues.add(Vector.Clue);

      // A function's prologue: Win64 saves what System V passes arguments
      // in, and spills arguments to the home slots above the return address.
      if (PrologueLeft != 0) {
        --PrologueLeft;
        const cs_x86 *X86 = nullptr;
        if (Conventions && (Id == X86_INS_PUSH || Id == X86_INS_MOV))
          X86 = detailed(Seg, Off, Full);
        const llvm::ArrayRef<cs_x86_op> Operands =
            X86 ? llvm::ArrayRef<cs_x86_op>(X86->operands, X86->op_count)
                : llvm::ArrayRef<cs_x86_op>();
        if (Id == X86_INS_PUSH && Operands.size() == 1 &&
            Operands[0].type == X86_OP_REG &&
            llvm::is_contained(Conventions->Win64Preserved,
                               mapCapstoneReg(Operands[0].reg).Offset))
          Clues.add(PlatformClue::Win64SavesRsiRdi);
        if (Id == X86_INS_MOV && Operands.size() == 2 &&
            Operands[0].type == X86_OP_MEM &&
            Operands[0].mem.base == X86_REG_RSP &&
            Operands[0].mem.index == X86_REG_INVALID &&
            Operands[1].type == X86_OP_REG &&
            mapCapstoneReg(Operands[1].reg).Size == TRI.PointerSize) {
          const uint64_t Reg = mapCapstoneReg(Operands[1].reg).Offset;
          for (size_t I = 0; I < Conventions->Win64Arguments.size(); ++I)
            if (Conventions->Win64Arguments[I] == Reg &&
                Operands[0].mem.disp ==
                    static_cast<int64_t>((I + 1) * TRI.PointerSize))
              Clues.add(PlatformClue::Win64HomeSpill);
        }
      }

      if (Id == X86_INS_CALL) {
        // The register a call's first argument went to names the convention
        // when the other convention's argument registers stayed untouched.
        if (Conventions && isCall(Bytes, Size, At)) {
          llvm::SmallSet<uint64_t, 8> Written;
          for (const auto &[Earlier, EarlierSize] : Window) {
            (void)EarlierSize;
            if (const cs_x86 *X86 = detailed(Seg, Earlier, Full))
              for (const cs_x86_op &Op :
                   llvm::ArrayRef<cs_x86_op>(X86->operands, X86->op_count))
                if (Op.type == X86_OP_REG && (Op.access & CS_AC_WRITE))
                  Written.insert(mapCapstoneReg(Op.reg).Offset);
          }
          const bool SysV = Written.contains(Conventions->SysVFirst);
          const bool Win64 = Written.contains(Conventions->Win64First);
          const bool SysVOnly =
              llvm::any_of(Conventions->SysVOnly,
                           [&](uint64_t Reg) { return Written.contains(Reg); });
          if (SysV && !Win64)
            Clues.add(PlatformClue::SysVFirstArgument);
          else if (Win64 && !SysVOnly)
            Clues.add(PlatformClue::Win64FirstArgument);
        }
        Window.clear();
      } else if (Id == X86_INS_JMP || Id == X86_INS_RET || Id == X86_INS_RETF ||
                 Id == X86_INS_INT3) {
        Window.clear();
        AfterBoundary = true;
      } else if (isConditionalBranch(Bytes, Size)) {
        Window.clear();
      } else if (!Padding) {
        if (Window.size() == limits::kPlatformCallWindow)
          Window.erase(Window.begin());
        Window.emplace_back(Off, Size);
      }
      Off += static_cast<size_t>(Size);
      if ((Budget & 0xfff) == 0 && Clues.decisive())
        break;
    }
  }
  Dec.setDetail(KeptDetail);
  Dec.setText(KeptText);
}

void readWords(const BinaryImage &Img, Tally &Clues) {
  const unsigned Width =
      Img.Arch == Arch::ARM && Img.Mode == InstructionMode::Thumb ? 2 : 4;
  const InstructionMode Mode =
      Img.Arch == Arch::ARM ? Img.Mode : InstructionMode::Default;
  llvm::SmallVector<const WordPattern *, 8> Patterns;
  for (const WordPattern &Pattern : WordPatterns)
    if (Pattern.TheArch == Img.Arch && Pattern.Mode == Mode)
      Patterns.push_back(&Pattern);
  if (Patterns.empty())
    return;
  uint64_t Budget = limits::kPlatformEvidenceBytes;
  for (const Segment &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.size() < Width)
      continue;
    // Instructions sit at multiples of their width from the segment start.
    const size_t First = (Width - Seg.VA % Width) % Width;
    for (size_t Off = First; Off + Width <= Seg.Data.size() && Budget >= Width;
         Off += Width, Budget -= Width) {
      const uint32_t Word =
          Width == 2 ? llvm::support::endian::read16le(Seg.Data.data() + Off)
                     : llvm::support::endian::read32le(Seg.Data.data() + Off);
      for (const WordPattern *Pattern : Patterns)
        if ((Word & Pattern->Mask) == Pattern->Value)
          Clues.add(Pattern->Clue);
    }
  }
}

void readStrings(const BinaryImage &Img, Tally &Clues) {
  for (const Marker &Mark : Markers)
    for (const Segment &Seg : Img.Segments) {
      const llvm::StringRef Bytes(
          reinterpret_cast<const char *>(Seg.Data.data()), Seg.Data.size());
      if (Bytes.contains(Mark.Text)) {
        Clues.add(Mark.Clue);
        break;
      }
    }
}

} // namespace

llvm::StringRef getPlatformClueText(PlatformClue Clue) {
  return infoOf(Clue).Text;
}

PlatformEvidence readPlatformEvidence(const BinaryImage &Img, Decoder &Dec) {
  Tally Clues;
  readStrings(Img, Clues);
  if (Img.Arch == Arch::X86 || Img.Arch == Arch::X64)
    readX86(Img, Dec, Clues);
  else
    readWords(Img, Clues);

  PlatformEvidence Evidence;
  const auto [Leader, Top, Second] = Clues.standing();
  Evidence.Decided = Clues.decides(Top, Second, limits::kPlatformMinimumScore,
                                   limits::kPlatformMarginRatio);
  Evidence.Platform = Evidence.Decided ? Leader : BinaryFormat::ELF;
  for (size_t I = 0; I < ClueCount; ++I)
    if (Clues.count(I) != 0)
      Evidence.Clues.emplace_back(static_cast<PlatformClue>(I), Clues.count(I));
  llvm::stable_sort(Evidence.Clues, [](const auto &A, const auto &B) {
    return A.second * infoOf(A.first).Weight >
           B.second * infoOf(B.first).Weight;
  });
  return Evidence;
}

std::string PlatformEvidence::describe() const {
  constexpr size_t Shown = 3;
  std::string Text;
  for (size_t I = 0; I < Clues.size() && I < Shown; ++I) {
    if (!Text.empty())
      Text += "; ";
    Text += llvm::formatv("{0} ({1})", getPlatformClueText(Clues[I].first),
                          Clues[I].second)
                .str();
  }
  if (Decided)
    return Text;
  const std::string Assumed =
      (getRawPlatformText(BinaryFormat::ELF) + " assumed").str();
  return Text.empty() ? "no evidence in the code; " + Assumed
                      : Text + "; too little to tell, " + Assumed;
}

} // namespace neverd
