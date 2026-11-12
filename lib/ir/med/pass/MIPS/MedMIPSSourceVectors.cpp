//===- MedMIPSSourceVectors.cpp - Scalar MSA leaf source contracts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// MSA source uses volatile vectors initialized on every reaching leaf path.
/// Incoming vector ABI, calls and saved FPR restoration need their
/// own proofs; a scalar result alone does not establish those contracts.
//===----------------------------------------------------------------------===//

#include "neverd/ArchSupport.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MIPS/MIPSSourceVectors.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/DenseMap.h"

#include <bitset>
#include <optional>
#include <vector>

namespace neverd {

void requireMIPSSourceVectors(const LowFunc &Function, Arch Target,
                              const BinaryImage *Image) {
  const auto IsVector = [](const NdVar &V) {
    return V.isReg() && mipsreg::isVector(V.Offset);
  };
  bool HasVectors = false;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      HasVectors |= IsVector(Op.Output);
      for (unsigned I = 0; I != Op.NumInputs; ++I)
        HasVectors |= IsVector(Op.Inputs[I]);
    }
  if (!HasVectors)
    return;
  if (!isMIPS(Target))
    throw LowToMedConversionError("MSA source requires a MIPS target");
  const bool Wide = isMIPS64(Target);
  const bool FP64 = Image && Image->MIPS && Image->MIPS->HasMSA &&
                    Image->MIPS->ISARevision == 5 &&
                    Image->MIPS->FPABI == (Wide ? 1 : 6);
  if (Image && !FP64)
    throw LowToMedConversionError("MSA source requires explicit FP64 profile");
  using Mask = std::bitset<mipsreg::VectorCount>;
  struct Transfer {
    Mask Needs, Defines, Kills;
    std::optional<Mask> Incoming;
  };
  const auto Index = [](const NdVar &V) {
    return (V.Offset - mipsreg::VectorBase) / mipsreg::VectorBytes;
  };
  std::vector<Transfer> Effects(Function.Blocks.size());
  llvm::DenseMap<int, size_t> Indices;
  for (size_t I = 0; I != Function.Blocks.size(); ++I) {
    const auto &Block = Function.Blocks[I];
    auto &T = Effects[I];
    Indices.try_emplace(Block.Id, I);
    if (!Block.ExceptionalSuccs.empty() || !Block.ExceptionalPreds.empty())
      throw LowToMedConversionError(
          "MSA source requires ordinary control flow");
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
        throw LowToMedConversionError("MSA source call ABI is not qualified");
      for (unsigned J = 0; J != Op.NumInputs; ++J) {
        if (!IsVector(Op.Inputs[J]))
          continue;
        const auto R = Index(Op.Inputs[J]);
        if (T.Kills[R])
          throw LowToMedConversionError(
              "MSA source observes a partial vector definition");
        if (!T.Defines[R])
          T.Needs.set(R);
      }
      if (!IsVector(Op.Output))
        continue;
      const auto R = Index(Op.Output);
      // n64 preserves F24–F31. o32 FP64 preserves F20,F22,...,F30.
      if (Wide ? R >= 24 : (R >= 20 && (!FP64 || R % 2 == 0)))
        throw LowToMedConversionError(
            "MSA source saved FPR restoration is not qualified");
      T.Defines[R] = Op.Output.Size == mipsreg::VectorBytes;
      T.Kills[R] = !T.Defines[R];
    }
  }

  // A join may use a vector only when every incoming execution defines it.
  // Revisiting the entry through a back edge cannot initialize a caller input.
  std::vector<size_t> Work{0};
  Effects.front().Incoming = Mask{};
  while (!Work.empty()) {
    const auto I = Work.back();
    Work.pop_back();
    auto &T = Effects[I];
    if ((T.Needs & ~*T.Incoming).any())
      throw LowToMedConversionError(
          "MSA incoming vector source ABI is not qualified");
    const auto Out = (*T.Incoming & ~T.Kills) | T.Defines;
    for (int Id : Function.Blocks[I].Succs) {
      const auto It = Indices.find(Id);
      if (It == Indices.end())
        throw LowToMedConversionError("MSA source has an unresolved successor");
      auto &In = Effects[It->second].Incoming;
      const Mask Next = In ? *In & Out : Out;
      if (!In || *In != Next) {
        In = Next;
        Work.push_back(It->second);
      }
    }
  }
  for (const auto &T : Effects)
    if (!T.Incoming && T.Needs.any())
      throw LowToMedConversionError(
          "MSA unreachable block has unbound vector inputs");
}

} // namespace neverd
