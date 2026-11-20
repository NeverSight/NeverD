//===- MachOEmbeddedDispatch.cpp - Relocatable switch dispatch ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MachOEmbeddedDispatch.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/EmbeddedRead.h"
#include "neverd/loader/RelocationProvenance.h"
#include "neverd/support/AArch64Literal.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/MachO.h"

#include <array>
#include <iterator>

namespace neverd {
bool coverMachOEmbeddedRead(const MachOLiftedSource &Source, va_t Start,
                            va_t Address, uint64_t Size, uint16_t Kind,
                            std::vector<MachODataInCodeRange> *Missing) {
  if (Address < Start || !Size ||
      !rangeInBounds(Address - Start, Size, Source.Bytes.size()))
    return false;
  return coverMachODataInCodeRead(Source.Embedded, Address, Size, Kind,
                                  Missing != nullptr, Missing);
}

namespace {
#define MACHO_DISPATCH_VALUE(Name, Value) constexpr unsigned Name = Value;
#include "MachOEmbeddedDispatch.def"

struct Address {
  aarch64_reg Reg;
  va_t Target;
  va_t PC;
};

/// The CFG resolver already proved which slots can reach this BR. A numeric
/// target alone does not show that the table and its anchor move with the
/// copy. Bind the reaching ADRs to the load and add in the same basic block.
struct MovingTable {
  va_t Producer;
  uint16_t Kind;
};

std::optional<MovingTable> movingTableAddress(Decoder &Decode,
                                              const MachOLiftedSource &Source,
                                              va_t Start, const LowBlock &Block,
                                              const JumpTable &Table) {
  constexpr auto Word = aarch64::kInsnSize;
  constexpr auto SequenceBytes = kDispatchInstructions * Word;
  if (Block.StartAddr < Start || Block.EndAddr < Block.StartAddr ||
      Block.EndAddr - Block.StartAddr < SequenceBytes ||
      Block.EndAddr - Start > Source.Bytes.size())
    return std::nullopt;

  // NOPs can pad a dispatch without changing any reaching definition. Stop
  // at the block boundary and retain every other word in program order.
  std::array<va_t, kDispatchInstructions> Sites;
  va_t Cursor = Block.EndAddr;
  for (auto &Site : llvm::reverse(Sites)) {
    do {
      if (Cursor - Block.StartAddr < Word)
        return std::nullopt;
      Cursor -= Word;
    } while (readLE<uint32_t>(Source.Bytes.data() + (Cursor - Start)) ==
             aarch64::kNop);
    Site = Cursor;
  }
  auto Next = Sites.begin();
  DecodedInsn I{};
  const auto Read = [&]() {
    const va_t PC = *Next++;
    return Decode.decodeOne(Source.Bytes.data() + (PC - Start), Word, PC, I) ==
               Word &&
           I.Raw && I.Raw->detail;
  };
  const auto ADR = [&]() -> std::optional<Address> {
    if (I.Id != AARCH64_INS_ADR)
      return std::nullopt;
    const auto &D = I.Raw->detail->aarch64;
    if (D.op_count != 2 || D.operands[0].type != AARCH64_OP_REG ||
        D.operands[1].type != AARCH64_OP_IMM)
      return std::nullopt;
    return Address{D.operands[0].reg, va_t(D.operands[1].imm), I.Addr};
  };
  if (!Read())
    return std::nullopt;
  auto Storage = ADR();
  if (!Storage || !Read())
    return std::nullopt;

  // When both addresses precede the load their registers must coexist.
  // A later anchor may reuse the storage register once the load has read it.
  std::optional<Address> Anchor;
  if (I.Id == AARCH64_INS_ADR) {
    Anchor = ADR();
    if (!Anchor || Storage->Reg == Anchor->Reg)
      return std::nullopt;
    if (Storage->Target != Table.BaseAddr)
      std::swap(Storage, Anchor);
    if (!Read())
      return std::nullopt;
  }
  if (Storage->Target != Table.BaseAddr)
    return std::nullopt;

  uint16_t Kind;
  switch (I.Id) {
#define MACHO_DISPATCH_LOAD(Name, Record)                                      \
  case AARCH64_INS_##Name:                                                     \
    Kind = llvm::MachO::Record;                                                \
    break;
#include "MachOEmbeddedDispatch.def"
  default:
    return std::nullopt;
  }
  const auto &Load = I.Raw->detail->aarch64;
  if (Load.op_count != 2 || Load.operands[0].type != AARCH64_OP_REG ||
      Load.operands[1].type != AARCH64_OP_MEM ||
      Load.operands[1].mem.base != Storage->Reg)
    return std::nullopt;

  // Replacing the load's W result by its X view is the architectural zero
  // extension used by compact tables. LDRSW already defines an X register.
  const auto Offset = mapCapstoneReg(Load.operands[0].reg).Offset;
  if (!Anchor) {
    if (!Read())
      return std::nullopt;
    Anchor = ADR();
  }
  if (!Anchor || Anchor->Target != Table.TargetBase ||
      Offset == mapCapstoneReg(Anchor->Reg).Offset || !Read() ||
      I.Id != AARCH64_INS_ADD)
    return std::nullopt;
  const auto &Add = I.Raw->detail->aarch64;
  if (Add.op_count != 3 || Add.operands[0].type != AARCH64_OP_REG ||
      Add.operands[1].type != AARCH64_OP_REG ||
      Add.operands[2].type != AARCH64_OP_REG ||
      Add.operands[1].reg != Anchor->Reg ||
      mapCapstoneReg(Add.operands[2].reg).Offset != Offset)
    return std::nullopt;
  const auto Destination = Add.operands[0].reg;
  if (!Read() || I.Id != AARCH64_INS_BR)
    return std::nullopt;
  const auto &Branch = I.Raw->detail->aarch64;
  if (Branch.op_count != 1 || Branch.operands[0].type != AARCH64_OP_REG ||
      Branch.operands[0].reg != Destination)
    return std::nullopt;
  return MovingTable{Storage->PC, Kind};
}

bool recordTable(const MachOLiftedSource &Source, va_t Start,
                 const JumpTable &Table, uint16_t Kind,
                 std::vector<MachODataInCodeRange> &Recovered) {
  const auto Base = machODataInCodeRanges(Source.Embedded, Table.BaseAddr, 1);
  if (!Base.empty() && Base.front().Kind != Kind)
    return false;
  for (const auto &Range : Table.StorageRanges) {
    const auto End = Range.storageEnd();
    if (!End || Range.BaseAddr < Start || *End - Start > Source.Bytes.size() ||
        Range.EntrySize != Table.EntrySize)
      return false;
    if (Range.EntrySize == Range.EntryStride || Range.PhysicalSlotCount == 1) {
      if (!coverMachOEmbeddedRead(Source, Start, Range.BaseAddr,
                                  *End - Range.BaseAddr, Kind, &Recovered))
        return false;
      continue;
    }
    for (uint64_t I = 0; I < Range.PhysicalSlotCount; ++I) {
      const va_t At = Range.BaseAddr + I * Range.EntryStride;
      if (!coverMachOEmbeddedRead(Source, Start, At, Range.EntrySize, Kind,
                                  &Recovered))
        return false;
    }
  }
  return true;
}

// Multiple instructions may read overlapping bytes of the same type.
bool coalesceReads(std::vector<MachODataInCodeRange> &Recovered) {
  llvm::sort(Recovered, [](const auto &L, const auto &R) {
    return std::tie(L.Begin, L.End) < std::tie(R.Begin, R.End);
  });
  size_t Count = 0;
  for (const auto R : Recovered) {
    if (Count && R.Begin <= Recovered[Count - 1].End) {
      auto &Last = Recovered[Count - 1];
      if (R.Kind == Last.Kind) {
        Last.End = std::max(Last.End, R.End);
        continue;
      }
      if (R.Begin < Last.End)
        return false;
    }
    Recovered[Count++] = R;
  }
  Recovered.resize(Count);
  return true;
}

bool completeLayout(const BinaryImage &Image, const LowFunc &Function,
                    const MachOLiftedSource &Source,
                    MachOEmbeddedDispatch &Layout) {
  auto &Recovered = Layout.Recovered;
  if (!coalesceReads(Recovered))
    return false;
  const auto ReadCount = Recovered.size();
  llvm::SmallVector<std::pair<va_t, va_t>> Spans;
  for (const auto &B : Function.Blocks)
    Spans.emplace_back(B.StartAddr, B.EndAddr);
  for (const auto R : Source.Embedded)
    Spans.emplace_back(R.Begin, R.End);
  for (const auto R : Recovered)
    Spans.emplace_back(R.Begin, R.End);
  llvm::sort(Spans);
  const va_t Limit = Function.Entry + Source.Bytes.size();
  va_t Next = Function.Entry;
  const auto AddPadding = [&](va_t Begin, va_t End) {
    if (Begin < End)
      Recovered.push_back({Begin, End, llvm::MachO::DICE_KIND_DATA});
  };
  const auto CoverGap = [&](va_t End) {
    constexpr auto Word = aarch64::kInsnSize;
    if (Next == End)
      return true;
    if (Next > End)
      return false;
    // Only complete aligned words could be instructions. The partial words
    // on either side share an instruction word with an already owned span.
    const auto WordEnd = llvm::alignDown(End, Word);
    if (WordEnd <= Next || WordEnd - Next < Word) {
      AddPadding(Next, End);
      return true;
    }
    const auto FirstWord = llvm::alignTo(Next, Word);
    AddPadding(Next, FirstWord);
    Layout.UnreachedCode.emplace_back(FirstWord, WordEnd);
    AddPadding(WordEnd, End);
    return true;
  };
  for (const auto &[Begin, End] : Spans) {
    if (Begin >= End || End > Limit || !CoverGap(Begin))
      return false;
    Next = End;
  }
  if (!CoverGap(Limit))
    return false;
  if (Recovered.empty() || Layout.UnreachedCode.empty())
    return ReadCount == Recovered.size() || coalesceReads(Recovered);

  // A pure instruction section supplies the code owner outside declared data
  // islands. Use it for an unvisited interval only when every recovered read
  // closes a hole between two original records of its own kind. An open-ended
  // read or a SOME-only section cannot establish the rest of the layout.
  const auto *Section = Image.getSectionFor(Function.Entry);
  if (!Section || !(Section->Type & llvm::MachO::S_ATTR_PURE_INSTRUCTIONS))
    return false;
  for (const auto &Read : llvm::ArrayRef(Recovered).take_front(ReadCount)) {
    const auto Right = llvm::lower_bound(
        Source.Embedded, Read.End,
        [](const auto &Record, va_t End) { return Record.Begin < End; });
    if (Right == Source.Embedded.begin() || Right == Source.Embedded.end() ||
        Right->Begin != Read.End || Right->Kind != Read.Kind)
      return false;
    const auto &Left = *std::prev(Right);
    if (Left.End != Read.Begin || Left.Kind != Read.Kind)
      return false;
  }
  return ReadCount == Recovered.size() || coalesceReads(Recovered);
}
} // namespace

std::optional<MachOEmbeddedDispatch>
machOEmbeddedDispatch(const BinaryImage &Image, Decoder &Decode,
                      const MachOLiftedSource &Source,
                      const LowFunc &Function) {
  MachOEmbeddedDispatch Result;
  if (Function.AnalysisOnly)
    return Result;
  if (Source.Embedded.empty() && Function.JumpTables.empty() &&
      Function.Blocks.size() == 1 &&
      Function.Blocks.front().StartAddr == Function.Entry &&
      Function.Blocks.front().EndAddr - Function.Entry == Source.Bytes.size())
    return Result;
  llvm::DenseMap<va_t, const JumpTable *> Tables;
  for (const auto &Table : Function.JumpTables)
    if (!Table.AnalysisOnly && Table.IndexDomainAuthenticated &&
        !Table.MutatedUnsafe && Table.HasBaseAddr && Table.HasTargetBase &&
        Table.IsRelative && Table.HasDispatchSlotMap &&
        !Table.StorageRanges.empty() && !Table.Targets.empty())
      Tables.try_emplace(Table.InsnAddr, &Table);
  const auto LocalCode = [&](va_t PC) {
    return PC >= Function.Entry && PC - Function.Entry < Source.Bytes.size() &&
           PC % aarch64::kInsnSize == 0 && Image.hasExecutableCodeOwnerAt(PC);
  };
  for (const auto &Block : Function.Blocks) {
    for (const auto &I : Block.InstructionBoundaries) {
      if (I.Address < Function.Entry || I.Size != aarch64::kInsnSize ||
          !rangeInBounds(I.Address - Function.Entry, I.Size,
                         Source.Bytes.size()))
        continue;
      const auto Encoding =
          readLE<uint32_t>(Source.Bytes.data() + (I.Address - Function.Entry));
      const auto Load = aarch64::decodeLiteralLoad(Encoding, I.Address);
      if (Load && Load->Address >= Function.Entry &&
          rangeInBounds(Load->Address - Function.Entry, Load->Width,
                        Source.Bytes.size()) &&
          !coverMachOEmbeddedRead(Source, Function.Entry, Load->Address,
                                  Load->Width, llvm::MachO::DICE_KIND_DATA,
                                  &Result.Recovered))
        return std::nullopt;
    }
    if (Block.EndAddr < aarch64::kInsnSize)
      continue;
    const auto *Table = Tables.lookup(Block.EndAddr - aarch64::kInsnSize);
    if (!Table || !LocalCode(Table->TargetBase) ||
        !llvm::all_of(Table->Targets, LocalCode))
      continue;
    if (auto Producer =
            movingTableAddress(Decode, Source, Function.Entry, Block, *Table)) {
      if (!recordTable(Source, Function.Entry, *Table, Producer->Kind,
                       Result.Recovered))
        return std::nullopt;
      Result.TableAddresses.insert(Producer->Producer);
      Result.BranchTargets.try_emplace(Table->InsnAddr, Table->Targets);
    }
  }
  const bool MissingReads = !Result.Recovered.empty();
  if ((MissingReads || !Source.Embedded.empty()) &&
      !completeLayout(Image, Function, Source, Result)) {
    if (MissingReads)
      return std::nullopt;
    // Explicit data records need no inferred layout. The normal instruction
    // emitter still verifies the entire body in this case.
    Result.UnreachedCode.clear();
    Result.Recovered.clear();
  }
  // A retained instruction has no relocation emitter of its own, including
  // when every data record surrounding it was present in the input.
  for (const auto &[Begin, End] : Result.UnreachedCode)
    if (hasRelocationProvenanceIn(Image, Begin, End))
      return std::nullopt;
  return Result;
}
} // namespace neverd
