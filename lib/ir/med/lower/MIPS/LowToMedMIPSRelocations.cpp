//===- LowToMedMIPSRelocations.cpp - Relocatable MIPS immediates ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Project each authenticated split address operand before SSA propagation.
/// Physical LowIR retains the original instruction effects and link-time bits.
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <initializer_list>
#include <utility>
#include <vector>

namespace neverd {

void LowToMedConverter::projectMIPSRelocatedImmediates(MedFunc &Func) {
  if (!Image || !Image->MIPS || Image->MIPS->InstructionRelocations.empty())
    return;
  const auto &Fields = Image->MIPS->InstructionRelocations;
  const uint16_t PointerBytes = Image->getPointerSize();
  for (auto &Block : Func.Blocks) {
    std::vector<MedOp> Projected;
    for (size_t Begin = 0; Begin < Block.Ops.size();) {
      size_t End = Begin + 1;
      const va_t Address = Block.Ops[Begin].Addr;
      while (End < Block.Ops.size() && Block.Ops[End].Addr == Address)
        ++End;
      const auto At = std::lower_bound(
          Fields.begin(), Fields.end(), Address,
          [](const auto &Field, va_t VA) { return Field.Address < VA; });
      if (At == Fields.end() || At->Address != Address) {
        if (!Projected.empty())
          Projected.insert(Projected.end(), Block.Ops.begin() + Begin,
                           Block.Ops.begin() + End);
        Begin = End;
        continue;
      }
      if (!At->Value)
        throw LowToMedConversionError(
            "MIPS symbolic instruction relocation has no authenticated source "
            "operand");
      const auto &Field = *At->Value;
      const unsigned Opcode = Field.Instruction >> 26;
      const bool Upper = Opcode == 15;
      const uint16_t Bytes = Upper || Opcode == 9   ? 4
                             : isMIPS64(TargetArch) ? 8
                                                    : 4;
      const uint64_t Expected =
          Upper          ? uint32_t(Field.Instruction << 16)
          : Opcode == 13 ? uint16_t(Field.Instruction)
          : Bytes == 4   ? uint32_t(int16_t(Field.Instruction))
                         : uint64_t(int64_t(int16_t(Field.Instruction)));
      const unsigned Input = Upper ? 0 : 1;
      size_t Match = End;
      for (size_t I = Begin; I < End; ++I) {
        const auto &Op = Block.Ops[I];
        const bool Shape =
            Upper
                ? ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_SEXT) &&
                   Op.NumInputs == 1)
                : (Op.Opcode == (Opcode == 13 ? NdOp::INT_OR : NdOp::INT_ADD) &&
                   Op.NumInputs == 2 && Op.Output.Size == Bytes);
        if (!Shape || Op.OriginSeq < 0 || !Op.Inputs[Input].isConst() ||
            Op.Inputs[Input].Size != Bytes ||
            (Bytes == 4 ? uint32_t(Op.Inputs[Input].ConstVal)
                        : Op.Inputs[Input].ConstVal) != Expected)
          continue;
        if (Match != End)
          throw LowToMedConversionError(
              "MIPS symbolic immediate has ambiguous LowIR occurrences");
        Match = I;
      }
      if (Match == End)
        throw LowToMedConversionError("MIPS symbolic immediate does not match "
                                      "its original LowIR operand");

      if (Projected.empty()) {
        Projected.reserve(Block.Ops.size());
        Projected.insert(Projected.end(), Block.Ops.begin(),
                         Block.Ops.begin() + Begin);
      }
      Projected.insert(Projected.end(), Block.Ops.begin() + Begin,
                       Block.Ops.begin() + Match);
      auto Scalar = [](uint64_t Value, uint16_t Size) {
        return MedVar::makeConst(Value, Size,
                                 ConstantAddressProvenance::Scalar);
      };
      const auto Emit = [&](NdOp Opcode, uint16_t Size,
                            std::initializer_list<MedVar> Inputs) {
        MedOp Op;
        Op.Opcode = Opcode;
        Op.Addr = Address;
        Op.Output.Kind = MedVar::Temp;
        Op.Output.Id = allocVarId();
        Op.Output.Size = Size;
        Op.Output.TheArch = TargetArch;
        for (const auto &Value : Inputs)
          Op.addInput(Value);
        Projected.push_back(std::move(Op));
        return Projected.back().Output;
      };
      MedVar Value = MedVar::makeConst(Field.SymbolValue, PointerBytes,
                                       ConstantAddressProvenance::CodeAddress,
                                       Field.SymbolValue & ~va_t{1});
      const auto Extraction = extractionOf(Field.Part);
      // Keep the symbol occurrence whole; the arithmetic below observes the
      // rebuilt callable bits. Each field has its own ABI carry and truncation,
      // so split registers and a LO instruction scheduled before HI work too.
      const uint64_t Bias = uint64_t(Field.Addend) + Extraction.Carry;
      if (Bias)
        Value = Emit(NdOp::INT_ADD, PointerBytes,
                     {Value, Scalar(Bias, PointerBytes)});
      if (Extraction.Shift)
        Value = Emit(NdOp::INT_RIGHT, PointerBytes,
                     {Value, Scalar(Extraction.Shift, PointerBytes)});
      Value = Emit(NdOp::SUBBYTES, 2, {Value, Scalar(0, 4)});
      Value = Emit(Upper || Opcode == 13 ? NdOp::INT_ZEXT : NdOp::INT_SEXT,
                   Bytes, {Value});
      if (Upper)
        Value = Emit(NdOp::INT_LEFT, Bytes, {Value, Scalar(16, Bytes)});
      auto Original = Block.Ops[Match];
      Original.Inputs[Input] = Value;
      Projected.push_back(std::move(Original));
      Projected.insert(Projected.end(), Block.Ops.begin() + Match + 1,
                       Block.Ops.begin() + End);
      Begin = End;
    }
    if (!Projected.empty())
      Block.Ops = std::move(Projected);
  }
}

} // namespace neverd
