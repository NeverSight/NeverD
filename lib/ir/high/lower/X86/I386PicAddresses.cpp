//===- I386PicAddresses.cpp - i386 ELF GOT-relative data addresses --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// i386 has no PC-relative data addressing, so position-independent code
/// finds its data through the GOT: `call $+5; pop ebx; add ebx, GOTPC` puts
/// the GOT's address in a register, and `[ebx + table@GOTOFF]` reads the
/// table.  The CFG proves which register value is that GOT base, and an
/// unlinked object places the GOT at address zero (MedIR's
/// I386ELFGOTBaseZero model).  The base then converts to zero, and the
/// constant a GOT-relative address adds to it is the address of the data it
/// names, so the C reads the data object instead of a number.
///
//===----------------------------------------------------------------------===//

#include "../HighEntryStackOffsets.h"

#include "neverd/Limits.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <utility>

namespace neverd {

void MedToHighConverter::collectI386GotBase(const MedFunc &Med) {
  I386GotBase.clear();
  I386GotRooted.clear();
  for (const MedScalarAddressModel &Model : Med.ScalarAddressModels)
    if (Model.Model == RelocatedInstructionScalarModelOccurrence::ModelKind::
                           I386ELFGOTBaseZero &&
        !Model.Value.isConst() && Model.Value.Size)
      I386GotBase.insert(varKey(Model.Value));
  if (I386GotBase.empty())
    return;
  auto IsBase = [&](const MedVar &V) {
    return !V.isConst() && I386GotBase.count(varKey(V));
  };
  auto IsRooted = [&](const MedVar &V) {
    return IsBase(V) || (!V.isConst() && I386GotRooted.count(varKey(V)));
  };
  // A copy of the base is the base.  A sum of the base and terms that are no
  // constants (an index) is rooted at it: a constant it adds later is still
  // the GOT-relative displacement.  SSA defines each value once, so the walk
  // ends when no value changes kind.
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (const MedBlock &Block : Med.Blocks)
      for (const MedOp &Op : Block.Ops) {
        if (!Op.Output.Size || Op.Output.isConst() || IsRooted(Op.Output))
          continue;
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
            Op.Inputs[0].Size == Op.Output.Size && IsBase(Op.Inputs[0])) {
          I386GotBase.insert(varKey(Op.Output));
          Changed = true;
        } else if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs == 2 &&
                   IsRooted(Op.Inputs[0]) != IsRooted(Op.Inputs[1]) &&
                   !Op.Inputs[IsRooted(Op.Inputs[0]) ? 1 : 0].isConst()) {
          I386GotRooted.insert(varKey(Op.Output));
          Changed = true;
        }
      }
  }
}

std::map<VarKey, uint32_t>
MedToHighConverter::i386PcRootedNumbers(const MedFunc &Med, Arch TheArch,
                                        const BinaryImage *Img) {
  std::map<VarKey, uint32_t> Numbers;
  if (TheArch != Arch::X86 || Med.I386GetPcModels.empty())
    return Numbers;
  MedToHighConverter Proof;
  Proof.Image = Img;
  Proof.CurMed = &Med;
  Proof.TargetArch = TheArch;
  Proof.collectI386GotBase(Med);
  Proof.collectI386PcRootedValues(Med);
  for (const auto &[Key, Value] : Proof.I386PcRootedValues)
    if (Value.second == 4)
      Numbers.emplace(Key, Value.first);
  return Numbers;
}

void MedToHighConverter::collectI386PcRootedValues(const MedFunc &Med) {
  // LowToMed binds a call-next/POP only where the raw PC is authenticated:
  // outside an unlinked ELF object, whose GOTPC model replaces it.  Reading
  // the bound value as that PC leaves the call's pushed slot without a
  // reader, so the slot does not survive as a local.
  I386PcRootedValues.clear();
  std::set<VarKey> Conflicting;
  for (const MedI386GetPcModel &Model : Med.I386GetPcModels)
    for (const MedVar *Value : {&Model.Output, &Model.Value}) {
      if (!Value->Size || Value->isConst())
        continue;
      const VarKey Key = varKey(*Value);
      const auto [It, Inserted] = I386PcRootedValues.try_emplace(
          Key, std::make_pair(Model.PCValue, Value->Size));
      if (!Inserted && It->second != std::make_pair(Model.PCValue, Value->Size))
        Conflicting.insert(Key);
    }
  for (const VarKey &Key : Conflicting)
    I386PcRootedValues.erase(Key);

  // A constant added to such a value, or subtracted from it, and a copy of it
  // are exact arithmetic on the PC: `call; pop; add $_GLOBAL_OFFSET_TABLE_`
  // computes the GOT address, and a displacement from it the data there.
  // SSA defines each value once, so the walk ends when none is added.
  auto Rooted = [&](const MedVar &V) -> std::optional<uint32_t> {
    if (V.isConst())
      return std::nullopt;
    const auto It = I386PcRootedValues.find(varKey(V));
    if (It == I386PcRootedValues.end() || It->second.second != V.Size)
      return std::nullopt;
    return It->second.first;
  };
  auto Scalar = [](const MedVar &V) -> std::optional<uint32_t> {
    if (!V.isConst() || (V.Provenance != ConstantAddressProvenance::Unknown &&
                         V.Provenance != ConstantAddressProvenance::Scalar))
      return std::nullopt;
    return static_cast<uint32_t>(V.ConstVal);
  };
  // clang -O0 spills the GOT address to a frame slot and reloads it at each
  // use.  As LLVM's frame-reload proof decides (MedLLVMSpillPredicate.cpp),
  // only flow-insensitively, a reload reads the PC-rooted value every write of
  // its slot stores: each exact write of the four bytes stores that value, no
  // other frame write may reach them, and the slot's address is not passed on.
  // A write through a pointer the frame does not derive cannot reach it.
  indexMedDefinitions();
  std::map<VarKey, const PhiNode *> Phis;
  for (const MedBlock &Block : Med.Blocks)
    for (const PhiNode &Phi : Block.Phis)
      Phis.emplace(varKey(Phi.Output), &Phi);
  auto Placed = [&](const MedVar &V) {
    return !V.isConst() && EntryStackOffsets->offsetFrom(V, V).has_value();
  };
  auto FrameDerived = [&](const MedVar &Address) {
    std::vector<MedVar> Pending{Address};
    std::set<VarKey> Seen;
    while (!Pending.empty()) {
      if (Seen.size() >= limits::kCallArgStoreAddressDepth)
        return true;
      const MedVar V = Pending.back();
      Pending.pop_back();
      if (V.isConst() || !Seen.insert(varKey(V)).second)
        continue;
      if (Placed(V))
        return true;
      if (const auto Phi = Phis.find(varKey(V)); Phi != Phis.end()) {
        for (const auto &[Predecessor, Input] : Phi->second->Args)
          Pending.push_back(Input);
        continue;
      }
      // A loaded or returned pointer, or an incoming value other than the
      // stack pointer, which has a coordinate, is not this frame's.
      const MedOp *Def = uniqueMedDefinition(V);
      if (!Def || Def->Opcode == NdOp::LOAD || Def->Opcode == NdOp::CALL ||
          Def->Opcode == NdOp::INDIR_CALL)
        continue;
      for (uint8_t I = 0; I < Def->NumInputs; ++I)
        Pending.push_back(Def->Inputs[I]);
    }
    return false;
  };
  auto SpilledValue = [&](const MedVar &Slot) -> std::optional<uint32_t> {
    // A value that may point into the slot, here or through another anchor.
    auto AtSlot = [&](const MedVar &V) {
      return Placed(V) && EntryStackOffsets->mayOverlap(V, 1, Slot, 4);
    };
    std::optional<uint32_t> Value;
    for (const MedBlock &Block : Med.Blocks)
      for (const MedOp &Op : Block.Ops) {
        // The slot's address passed on as a value, or held in a register a
        // call may read without naming it.
        if (Op.Output.Kind == MedVar::Reg && Op.Output.Size &&
            !Op.Output.isConst() && AtSlot(Op.Output))
          return std::nullopt;
        const bool Arithmetic =
            Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB ||
            Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT;
        for (uint8_t I = 0; I < Op.NumInputs && !Arithmetic; ++I)
          if (!((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
                I == 0) &&
              !Op.Inputs[I].isConst() && AtSlot(Op.Inputs[I]))
            return std::nullopt;
        if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2 ||
            Op.Inputs[0].isConst())
          continue;
        const int64_t Bytes = Op.Inputs[1].Size;
        const auto At = EntryStackOffsets->offsetFrom(Op.Inputs[0], Slot);
        if (!At) {
          // A frame write the coordinates do not place may reach the slot,
          // and so may one they place from another anchor within its
          // alignment.
          if (Placed(Op.Inputs[0])
                  ? EntryStackOffsets->mayOverlap(Op.Inputs[0], Bytes, Slot, 4)
                  : FrameDerived(Op.Inputs[0]))
            return std::nullopt;
          continue;
        }
        if (*At + Bytes <= 0 || *At >= 4)
          continue;
        const auto Stored = Rooted(Op.Inputs[1]);
        if (*At != 0 || Bytes != 4 || !Stored || (Value && *Value != *Stored))
          return std::nullopt;
        Value = Stored;
      }
    return Value;
  };
  for (bool Changed = !I386PcRootedValues.empty(); Changed;) {
    Changed = false;
    // The slots a PC-rooted value is stored to, each decided once per pass:
    // a value found rooted in this pass may change a slot's writes.
    std::vector<MedVar> SpillSlots;
    for (const MedBlock &Block : Med.Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
            Op.Inputs[1].Size == 4 && Rooted(Op.Inputs[1]) &&
            Placed(Op.Inputs[0]))
          SpillSlots.push_back(Op.Inputs[0]);
    std::map<size_t, std::optional<uint32_t>> SlotValues;
    auto Reloaded = [&](const MedVar &Address) -> std::optional<uint32_t> {
      for (size_t K = 0; K < SpillSlots.size(); ++K) {
        const auto At = EntryStackOffsets->offsetFrom(Address, SpillSlots[K]);
        if (!At || *At != 0)
          continue;
        auto [It, Inserted] = SlotValues.try_emplace(K);
        if (Inserted)
          It->second = SpilledValue(SpillSlots[K]);
        return It->second;
      }
      return std::nullopt;
    };
    // A join of one PC-rooted value on every edge is that value, as the
    // GOT address reloaded on each path to it is.
    for (const MedBlock &Block : Med.Blocks)
      for (const PhiNode &Phi : Block.Phis) {
        if (Phi.Output.Size != 4 || Phi.Output.isConst() || Phi.Args.empty() ||
            Phi.ExceptionalEntry ||
            I386PcRootedValues.count(varKey(Phi.Output)))
          continue;
        std::optional<uint32_t> Value;
        for (const auto &[Predecessor, Input] : Phi.Args) {
          const auto Incoming = Rooted(Input);
          if (!Incoming || (Value && *Value != *Incoming)) {
            Value.reset();
            break;
          }
          Value = Incoming;
        }
        if (!Value)
          continue;
        I386PcRootedValues.emplace(varKey(Phi.Output),
                                   std::make_pair(*Value, Phi.Output.Size));
        Changed = true;
      }
    for (const MedBlock &Block : Med.Blocks)
      for (const MedOp &Op : Block.Ops) {
        if (Op.Output.Size != 4 || Op.Output.isConst() ||
            I386PcRootedValues.count(varKey(Op.Output)) ||
            uniqueMedDefinition(Op.Output) != &Op)
          continue;
        std::optional<uint32_t> Value;
        if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 &&
            !Op.Inputs[0].isConst() && !SpillSlots.empty() &&
            Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
          Value = Reloaded(Op.Inputs[0]);
        } else if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
          Value = Rooted(Op.Inputs[0]);
        } else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
                   Op.NumInputs == 2) {
          if (const auto Base = Rooted(Op.Inputs[0]),
              Offset = Scalar(Op.Inputs[1]);
              Base && Offset)
            Value =
                Op.Opcode == NdOp::INT_ADD ? *Base + *Offset : *Base - *Offset;
          else if (const auto Offset = Scalar(Op.Inputs[0]),
                   Base = Rooted(Op.Inputs[1]);
                   Op.Opcode == NdOp::INT_ADD && Base && Offset)
            Value = *Offset + *Base;
        }
        if (!Value)
          continue;
        I386PcRootedValues.emplace(varKey(Op.Output),
                                   std::make_pair(*Value, Op.Output.Size));
        Changed = true;
      }
  }

  // A proven spill whose every load is a reload of the value is observed
  // only through that value, which each reload reads instead.  A load the
  // coordinates do not place may read the slot, so it keeps the spill.
  I386PcRootedStores.clear();
  std::vector<MedVar> ProvenSlots;
  bool UnplacedFrameLoad = false;
  for (const MedBlock &Block : Med.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1 &&
          !Op.Inputs[0].isConst() && !Placed(Op.Inputs[0]) &&
          FrameDerived(Op.Inputs[0]))
        UnplacedFrameLoad = true;
  for (const MedBlock &Block : Med.Blocks)
    for (const MedOp &Op : Block.Ops) {
      if (UnplacedFrameLoad || Op.Opcode != NdOp::STORE || Op.NumInputs < 2 ||
          Op.Inputs[1].Size != 4 || !Rooted(Op.Inputs[1]) ||
          !Placed(Op.Inputs[0]) ||
          llvm::any_of(
              ProvenSlots,
              [&](const MedVar &Slot) {
                const auto At =
                    EntryStackOffsets->offsetFrom(Op.Inputs[0], Slot);
                return At && *At == 0;
              }))
        continue;
      const MedVar &Slot = Op.Inputs[0];
      if (!SpilledValue(Slot))
        continue;
      bool OnlyReloads = true;
      for (const MedBlock &Other : Med.Blocks)
        for (const MedOp &Read : Other.Ops)
          if (OnlyReloads && Read.Opcode == NdOp::LOAD && Read.NumInputs >= 1 &&
              Placed(Read.Inputs[0]) &&
              EntryStackOffsets->mayOverlap(Read.Inputs[0], Read.Output.Size,
                                            Slot, 4)) {
            const auto At = EntryStackOffsets->offsetFrom(Read.Inputs[0], Slot);
            OnlyReloads = At && *At == 0 && Read.Output.Size == 4 &&
                          Rooted(Read.Output).has_value();
          }
      if (!OnlyReloads)
        continue;
      ProvenSlots.push_back(Slot);
      for (const MedBlock &Other : Med.Blocks)
        for (const MedOp &Write : Other.Ops)
          if (Write.Opcode == NdOp::STORE && Write.NumInputs >= 2 &&
              !Write.Inputs[0].isConst()) {
            const auto At =
                EntryStackOffsets->offsetFrom(Write.Inputs[0], Slot);
            if (At && *At == 0)
              I386PcRootedStores.insert({Write.Addr, Write.OriginSeq});
          }
    }

  // The pop read the call's push through the same stack pointer value; once
  // the pop reads as the PC, nothing reads the slot, which lies below the
  // stack pointer after the pop.
  if (TargetArch != Arch::X86)
    return;
  // In an unlinked object the GOTPC model, not the PC, gives the pop's sum
  // its value, the GOT base (collectI386GotBase): a popped temporary nothing
  // but that sum reads is read for nothing as well.
  std::set<VarKey> GotBaseAddends;
  if (!I386GotBase.empty()) {
    std::map<VarKey, unsigned> Reads;
    auto Note = [&](const MedVar &V) {
      if (!V.isConst())
        ++Reads[varKey(V)];
    };
    for (const MedBlock &Block : Med.Blocks) {
      for (const PhiNode &Phi : Block.Phis)
        for (const auto &[Predecessor, Input] : Phi.Args)
          Note(Input);
      for (const MedOp &Op : Block.Ops)
        for (uint8_t I = 0; I < Op.NumInputs; ++I)
          Note(Op.Inputs[I]);
    }
    for (const MedBlock &Block : Med.Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs == 2 &&
            Op.Inputs[0].isConst() != Op.Inputs[1].isConst() &&
            I386GotBase.count(varKey(Op.Output)))
          if (const MedVar &Addend = Op.Inputs[Op.Inputs[0].isConst() ? 1 : 0];
              Addend.Kind == MedVar::Temp && Reads[varKey(Addend)] == 1)
            GotBaseAddends.insert(varKey(Addend));
  }
  // A pop whose value nothing used is gone, and so is its read of the slot:
  // the push of `call $+5` then has no reader at all.  The `pop r32` right
  // after the call is the slot's only reader, since i386 code keeps nothing
  // below its stack pointer; a load of the slot that survived keeps it.
  std::set<VarKey> ReadSlots;
  for (const MedBlock &Block : Med.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1 &&
          !Op.Inputs[0].isConst() &&
          !I386PcRootedValues.count(varKey(Op.Output)) &&
          !GotBaseAddends.count(varKey(Op.Output)))
        ReadSlots.insert(varKey(Op.Inputs[0]));
  constexpr uint8_t CallNext[] = {0xE8, 0, 0, 0, 0};
  constexpr uint8_t PopOpcode = 0x58, PopRegisterMask = 0x07;
  for (const MedBlock &Block : Med.Blocks)
    for (const MedOp &Op : Block.Ops) {
      if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2 ||
          Op.Inputs[0].isConst() || ReadSlots.count(varKey(Op.Inputs[0])) ||
          !Op.Inputs[1].isConst() || Op.Inputs[1].Size != 4 ||
          Op.Addr == InvalidVA ||
          Op.Inputs[1].ConstVal != Op.Addr + sizeof(CallNext) || !Image)
        continue;
      const uint8_t *Bytes = Image->readVA(Op.Addr, sizeof(CallNext) + 1);
      if (Bytes &&
          std::equal(std::begin(CallNext), std::end(CallNext), Bytes) &&
          (Bytes[sizeof(CallNext)] & ~PopRegisterMask) == PopOpcode)
        I386PcRootedStores.insert({Op.Addr, Op.OriginSeq});
    }
  for (const MedI386GetPcModel &Model : Med.I386GetPcModels) {
    if (!I386PcRootedValues.count(varKey(Model.Value)))
      continue;
    const MedOp *Pop = uniqueMedDefinition(Model.Value);
    if (!Pop || Pop->Opcode != NdOp::LOAD || Pop->NumInputs != 1 ||
        Pop->Inputs[0].isConst())
      continue;
    const VarKey Slot = varKey(Pop->Inputs[0]);
    for (const MedBlock &Block : Med.Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
            !Op.Inputs[0].isConst() && varKey(Op.Inputs[0]) == Slot &&
            Op.Inputs[1].isConst() && Op.Inputs[1].Size == Model.Value.Size &&
            Op.Inputs[1].ConstVal == Model.PCValue)
          I386PcRootedStores.insert({Op.Addr, Op.OriginSeq});
  }
}

ExprPtr MedToHighConverter::i386PcRootedValue(const MedOp &Op) const {
  if (Op.Output.isConst())
    return nullptr;
  const auto It = I386PcRootedValues.find(varKey(Op.Output));
  if (It == I386PcRootedValues.end() || It->second.second != Op.Output.Size)
    return nullptr;
  return i386PcRootedConstant(It->second.first, Op.Output.Size);
}

ExprPtr MedToHighConverter::i386PcRootedConstant(uint32_t Value,
                                                 uint16_t Size) const {
  // An address of data the image holds is that data's, as a GOT-relative one
  // is in an object (i386GotRelativeAddress); the PC, code, the GOT address
  // each such address is relative to, and any other value stay the numbers
  // they are.
  const Segment *Seg = Image ? Image->getSegmentFor(Value) : nullptr;
  const bool Data = Seg && Seg->isReadable() && !Seg->isExecutable() &&
                    Image->linkedI386GlobalOffsetTable() != Value;
  return HighExpr::makeConst(Value, Size,
                             Data ? ConstantAddressProvenance::DataAddress
                                  : ConstantAddressProvenance::Scalar);
}

std::optional<unsigned>
MedToHighConverter::i386GotDisplacement(const MedOp &Op) const {
  if (I386GotBase.empty() || Op.Opcode != NdOp::INT_ADD || Op.NumInputs != 2)
    return std::nullopt;
  for (unsigned I = 0; I < 2; ++I) {
    const MedVar &Displacement = Op.Inputs[I];
    const MedVar &Base = Op.Inputs[1 - I];
    if (Displacement.isConst() && !Base.isConst() &&
        (Displacement.Provenance == ConstantAddressProvenance::Unknown ||
         Displacement.Provenance == ConstantAddressProvenance::Scalar) &&
        (I386GotBase.count(varKey(Base)) || I386GotRooted.count(varKey(Base))))
      return I;
  }
  return std::nullopt;
}

ExprPtr MedToHighConverter::i386GotRelativeAddress(const MedOp &Op,
                                                   unsigned Displacement) {
  const MedVar &Constant = Op.Inputs[Displacement];
  const MedVar &Base = Op.Inputs[1 - Displacement];
  const uint64_t Mask = Op.Output.Size >= sizeof(uint64_t)
                            ? ~uint64_t{0}
                            : (uint64_t{1} << (8 * Op.Output.Size)) - 1;
  const uint64_t Address = Constant.ConstVal & Mask;
  // Data the image holds is an object to read; anything else stays the
  // number it is, as a GOT slot offset would.
  const Segment *Seg = Image ? Image->getSegmentFor(Address) : nullptr;
  const bool Data = Seg && Seg->isReadable() && !Seg->isExecutable();
  ExprPtr Target = HighExpr::makeConst(
      Address, Op.Output.Size,
      Data ? ConstantAddressProvenance::DataAddress : Constant.Provenance,
      Constant.AddressOwnerVA);
  if (I386GotBase.count(varKey(Base)))
    return Target;
  auto Sum = HighExpr::makeBinop(NdOp::INT_ADD, medvarToExpr(Base), Target);
  Sum->Type = NdType::makeInt(Op.Output.Size, false);
  return Sum;
}

} // namespace neverd
