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
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/MachO.h"

namespace neverd {
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
std::optional<va_t> movingTableAddress(Decoder &Decode,
                                       const MachOLiftedSource &Source,
                                       va_t Start, const LowBlock &Block,
                                       const JumpTable &Table) {
  constexpr auto Word = aarch64::kInsnSize;
  constexpr auto SequenceBytes = kDispatchInstructions * Word;
  if (Block.StartAddr < Start || Block.EndAddr < Block.StartAddr ||
      Block.EndAddr - Block.StartAddr < SequenceBytes ||
      Block.EndAddr - Start > Source.Bytes.size())
    return std::nullopt;

  va_t Next = Block.EndAddr - SequenceBytes;
  DecodedInsn I{};
  const auto Read = [&]() {
    const auto PC = Next;
    Next += Word;
    return Decode.decodeOne(Source.Bytes.data() + (PC - Start), Word, PC, I) ==
               Word &&
           I.Raw && I.Raw->detail;
  };
  const auto ADR = [&]() -> std::optional<Address> {
    if (!Read() || I.Id != AARCH64_INS_ADR)
      return std::nullopt;
    const auto &D = I.Raw->detail->aarch64;
    if (D.op_count != 2 || D.operands[0].type != AARCH64_OP_REG ||
        D.operands[1].type != AARCH64_OP_IMM)
      return std::nullopt;
    return Address{D.operands[0].reg, va_t(D.operands[1].imm), I.Addr};
  };
  auto Storage = ADR();
  auto Anchor = ADR();
  if (!Storage || !Anchor || Storage->Reg == Anchor->Reg)
    return std::nullopt;
  if (Storage->Target != Table.BaseAddr)
    std::swap(Storage, Anchor);
  if (Storage->Target != Table.BaseAddr || Anchor->Target != Table.TargetBase ||
      !Read())
    return std::nullopt;

  unsigned Kind;
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
  const auto Islands =
      machODataInCodeRanges(Source.Embedded, Table.BaseAddr, 1);
  if (Islands.empty() || Islands.front().Kind != Kind)
    return std::nullopt;
  // Resolvers may inventory dense tables one slot at a time or retain only
  // reachable ranges of a sparse table. Every possible read must move with
  // the same immutable data-in-code record.
  const auto &Island = Islands.front();
  for (const auto &Range : Table.StorageRanges) {
    const auto End = Range.storageEnd();
    if (!End || Range.EntrySize != Table.EntrySize ||
        Range.BaseAddr < Island.Begin || *End > Island.End)
      return std::nullopt;
  }

  // Replacing the load's W result by its X view is the architectural zero
  // extension used by compact tables. LDRSW already defines an X register.
  const auto Offset = mapCapstoneReg(Load.operands[0].reg).Offset;
  if (Offset == mapCapstoneReg(Anchor->Reg).Offset || !Read() ||
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
  return Storage->PC;
}
} // namespace

MachOEmbeddedDispatch machOEmbeddedDispatch(const BinaryImage &Image,
                                            Decoder &Decode,
                                            const MachOLiftedSource &Source,
                                            const LowFunc &Function) {
  MachOEmbeddedDispatch Result;
  if (Function.AnalysisOnly || Function.JumpTables.empty())
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
    if (Block.EndAddr < aarch64::kInsnSize)
      continue;
    const auto *Table = Tables.lookup(Block.EndAddr - aarch64::kInsnSize);
    if (!Table || !LocalCode(Table->TargetBase) ||
        !llvm::all_of(Table->Targets, LocalCode))
      continue;
    if (auto Producer =
            movingTableAddress(Decode, Source, Function.Entry, Block, *Table)) {
      Result.TableAddresses.insert(*Producer);
      Result.BranchTargets.try_emplace(Table->InsnAddr, Table->Targets);
    }
  }
  return Result;
}
} // namespace neverd
