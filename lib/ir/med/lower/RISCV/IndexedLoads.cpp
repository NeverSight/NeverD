//===- IndexedLoads.cpp - RISC-V absolute table reads --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A linked LUI/ADDI can denote either a number or a table. A load whose
/// entire index domain reaches one data mapping identifies that particular
/// memory use, even after the linker discards the instruction relocations.
///
//===----------------------------------------------------------------------===//

#include "IndexedLoads.h"

#include "../../pass/MedScalarBounds.h"
#include "../InsertPending.h"

#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ELF/ELFIndexedData.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CheckedArithmetic.h"
#include "llvm/Support/MathExtras.h"

#include <vector>

namespace neverd::riscv {

void recoverIndexedLoads(
    MedFunc &Func, const BinaryImage &Image, const ELFIndexedData *SharedData,
    int &NextVarId, llvm::function_ref<const MedOp *(const MedVar &)> Def) {
  const unsigned Width = Image.getPointerSize();
  auto Copied = [&](const MedVar &V) -> const MedVar & {
    if (!V.isConst())
      if (const MedOp *D = Def(V);
          D && D->Opcode == NdOp::COPY && D->NumInputs == 1 &&
          D->Inputs[0].Size == V.Size &&
          D->RegistrationRoot == MedOp::RegistrationRootKind::None)
        return D->Inputs[0];
    return V;
  };
  struct Read {
    MedBlock *Block;
    size_t Position;
    va_t Base;
  };
  llvm::SmallVector<Read, 4> Reads;
  llvm::SmallVector<MedVar, 4> Indices;
  for (MedBlock &Block : Func.Blocks)
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const MedOp &Op = Block.Ops[I];
      if (Op.Opcode != NdOp::LOAD || Op.Dead || Op.NumInputs != 1 ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Op.Inputs[0].Size != Width || Op.Inputs[0].isConst())
        continue;
      const MedVar &Address = Copied(Op.Inputs[0]);
      const MedOp *Sum = Address.isConst() ? nullptr : Def(Address);
      if (!Sum || Sum->Opcode != NdOp::INT_ADD || Sum->NumInputs != 2 ||
          Sum->Output.Size != Width || Sum->Inputs[0].Size != Width ||
          Sum->Inputs[1].Size != Width)
        continue;
      const MedVar &Left = Copied(Sum->Inputs[0]);
      const MedVar &Right = Copied(Sum->Inputs[1]);
      if (Left.isConst() == Right.isConst())
        continue;
      const MedVar &Base = Left.isConst() ? Left : Right;
      if (Base.Provenance != ConstantAddressProvenance::Scalar ||
          !Image.hasObjectDataProvenance(Base.ConstVal))
        continue;
      Reads.push_back({&Block, I, Base.ConstVal});
      Indices.push_back(Left.isConst() ? Right : Left);
    }
  if (Reads.empty())
    return;

  const ELFIndexedData LocalData(Image);
  const auto &Data = SharedData ? *SharedData : LocalData;
  const auto Bounds = unsignedScalarUpperBounds(Func, Image, Indices);
  // Form the relocated address at the load. The old sum can have other,
  // numeric users which must still see the original machine bits. All SSA
  // queries above finish before insertPending moves any definitions.
  struct AddressOp {
    size_t Position;
    MedOp Op;
  };
  std::vector<AddressOp> Pending;
  const uint64_t MaxVA = llvm::maskTrailingOnes<uint64_t>(Width * 8);
  for (size_t I = 0; I < Reads.size();) {
    MedBlock &Block = *Reads[I].Block;
    Pending.clear();
    for (; I < Reads.size() && Reads[I].Block == &Block; ++I) {
      const auto &R = Reads[I];
      MedOp &Load = Block.Ops[R.Position];
      if (!Bounds[I])
        continue;
      const auto Span =
          llvm::checkedAddUnsigned(*Bounds[I], uint64_t(Load.Output.Size));
      if (!Span || !*Span || *Span - 1 > MaxVA - R.Base)
        continue;
      const auto Owner = Data.readOnlyOwner(R.Base, *Span);
      if (!Owner)
        continue;
      auto &Address = Pending.emplace_back(AddressOp{R.Position, {}}).Op;
      Address.Opcode = NdOp::INT_ADD;
      Address.Addr = Load.Addr;
      Address.Output.Kind = MedVar::Temp;
      Address.Output.TheArch = Image.Arch;
      Address.Output.Size = Width;
      Address.Output.Id = NextVarId++;
      Address.addInput(MedVar::makeConst(
          R.Base, Width, ConstantAddressProvenance::DataAddress, *Owner));
      Address.addInput(Indices[I]);
      Load.Inputs[0] = Address.Output;
    }
    insertPending(Block.Ops, Pending, &AddressOp::Position);
  }
}

} // namespace neverd::riscv
