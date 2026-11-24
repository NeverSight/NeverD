//===- ELFv1Calls.cpp - PPC64 descriptor and TOC lowering ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recover native PPC64 ELFv1 function pointers and TOC-relative operands
/// from their reaching machine definitions.
///
//===----------------------------------------------------------------------===//

#include "ELFv1Calls.h"

#include "neverd/ArchSupport.h"
#include "neverd/ir/med/IndirectCallSlot.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/lift/IntegerRegisters.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/loader/ELF/PPC64FunctionDescriptor.h"
#include "neverd/loader/ELF/PowerPCABI.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

namespace neverd {
namespace {
using namespace ppc_elf;

MedVar stripCopies(MedVar V, MedDefinitionIndex &Defs) {
  for (int I = 0; I < limits::kCallTargetSlotDepth && !V.isConst(); ++I) {
    const auto *D = Defs.find(V);
    if (!D || D->Opcode != NdOp::COPY || D->NumInputs != 1 ||
        D->Inputs[0].Size != WordSize)
      break;
    V = D->Inputs[0];
  }
  return V;
}

// A high-adjusted TOC displacement can first produce an address outside any
// section. Bind the completed load/store address only after the low half.
std::optional<va_t> tocAddress(MedVar V, MedDefinitionIndex &Defs, va_t TOC) {
  uint64_t Displacement = 0;
  for (int I = 0; I < limits::kCallTargetSlotDepth && V.Size == WordSize; ++I) {
    if (V.isConst()) {
      if ((V.Provenance == ConstantAddressProvenance::AddressFragment ||
           V.Provenance == ConstantAddressProvenance::DataAddress) &&
          V.ConstVal == TOC)
        return TOC + Displacement;
      break;
    }
    const auto *D = Defs.find(V);
    if (!D)
      break;
    if (D->Opcode == NdOp::COPY && D->NumInputs == 1) {
      V = D->Inputs[0];
      continue;
    }
    if ((D->Opcode != NdOp::INT_ADD && D->Opcode != NdOp::INT_SUB) ||
        D->NumInputs != 2 || !D->Inputs[1].isConst() ||
        D->Inputs[1].Provenance != ConstantAddressProvenance::Scalar)
      break;
    Displacement += D->Opcode == NdOp::INT_ADD ? D->Inputs[1].ConstVal
                                               : -D->Inputs[1].ConstVal;
    V = D->Inputs[0];
  }
  return std::nullopt;
}

// Only loads still current at the branch can be replaced by the native ABI's
// descriptor call. A store or another call can change the descriptor.
std::optional<MedVar>
fieldBase(MedVar V, unsigned Offset, MedDefinitionIndex &Defs,
          const llvm::SmallPtrSetImpl<const MedOp *> &Loads) {
  const auto *Load = Defs.find(stripCopies(V, Defs));
  if (!Load || !Loads.contains(Load))
    return std::nullopt;
  MedVar EA = stripCopies(Load->Inputs[0], Defs);
  if (Offset == EntryOffset)
    return EA;
  const auto *Add = Defs.find(EA);
  if (!Add || Add->Opcode != NdOp::INT_ADD || Add->NumInputs != 2 ||
      !Add->Inputs[1].isConst() ||
      Add->Inputs[1].Provenance != ConstantAddressProvenance::Scalar ||
      Add->Inputs[1].ConstVal != Offset)
    return std::nullopt;
  return stripCopies(Add->Inputs[0], Defs);
}

// A linked data word can numerically equal an .opd address without being a
// pointer. Once a descriptor call proves the use, standalone code must either
// rebuild the word's initializer or report the missing relocation.
void checkDescriptorInitializer(const MedVar &Base, MedDefinitionIndex &Defs,
                                const BinaryImage &Image,
                                std::optional<va_t> EntryTOC) {
  const auto *Load = Defs.find(Base);
  if (!Load || Load->Opcode != NdOp::LOAD || Load->Output.Size != WordSize)
    return;
  const auto Address = stripCopies(Load->Inputs[0], Defs);
  std::optional<va_t> Slot;
  if (Address.isConst() && isDataAddressProvenance(Address.Provenance))
    Slot = Address.ConstVal;
  else if (EntryTOC)
    Slot = tocAddress(Address, Defs, *EntryTOC);
  if (!Slot || Image.CodePtrRelocSlots.count(*Slot))
    return;
  if (const auto *Bytes = Image.readVA(*Slot, WordSize))
    if (Image.PPC64Descriptors.contains(
            readPtr(Bytes, true, archLittleEndian(Image.Arch))))
      throw LowToMedConversionError("PPC64 ELFv1 function-pointer initializer "
                                    "has no supported relocation");
}
// A signed high adjustment can lie outside the image until the following add
// or memory access contributes the low half. Keep the original definitions
// available until every use has been inspected.
void bindTOCAddresses(MedFunc &Function, MedDefinitionIndex &Defs,
                      const BinaryImage &Image, va_t TOC) {
  llvm::SmallVector<std::pair<MedOp *, MedVar>> Pointers;
  for (auto &Block : Function.Blocks) {
    for (auto &Op : Block.Ops) {
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        if (auto VA = tocAddress(Op.Inputs[0], Defs, TOC);
            VA && Image.isDataAddress(*VA))
          Op.Inputs[0] = MedVar::makeConst(
              *VA, WordSize, ConstantAddressProvenance::DataAddress);
        continue;
      }
      if (Op.Output.Size != WordSize ||
          (Op.Opcode != NdOp::INT_ADD && Op.Opcode != NdOp::INT_SUB))
        continue;
      const auto VA = tocAddress(Op.Output, Defs, TOC);
      if (!VA)
        continue;
      const auto It = Image.PPC64Descriptors.find(*VA);
      if (It == Image.PPC64Descriptors.end()) {
        if (Image.isDataAddress(*VA))
          Pointers.emplace_back(
              &Op, MedVar::makeConst(*VA, WordSize,
                                     ConstantAddressProvenance::DataAddress));
        continue;
      }
      const auto Entry = nativePPC64DescriptorEntry(Image, *VA);
      if (!Entry)
        throw LowToMedConversionError(
            "PPC64 ELFv1 address requires a distinct function descriptor");
      Pointers.emplace_back(
          &Op, MedVar::makeConst(*Entry, WordSize,
                                 ConstantAddressProvenance::CodeAddress));
    }
  }
  for (auto &[Op, Pointer] : Pointers) {
    Op->Opcode = NdOp::COPY;
    Op->NumInputs = 1;
    Op->Inputs[0] = Pointer;
  }
}
} // namespace

void recoverPPC64ELFv1(MedFunc &Function, const BinaryImage *Image) {
  using namespace ppc_elf;
  MedDefinitionIndex Defs(Function);
  const auto TOCReg = integerreg::gpr(TOCRegister);
  const auto EnvReg = integerreg::gpr(EnvironmentRegister);
  std::optional<va_t> EntryTOC;
  if (Image) {
    const auto It = Image->PPC64EntryContexts.find(Function.Entry);
    if (It != Image->PPC64EntryContexts.end())
      EntryTOC = It->second.TOC;
  }
  for (auto &Block : Function.Blocks) {
    if (llvm::none_of(Block.Ops, [](const MedOp &Op) {
          return Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL;
        }))
      continue;
    llvm::SmallPtrSet<const MedOp *, 8> Loads;
    std::optional<MedVar> TOC, Env;
    for (auto &Op : Block.Ops) {
      // A direct branch does not read a descriptor. A linker thunk that
      // switches TOCs must be recovered before these two entries can share a
      // source-level call.
      if (EntryTOC && Op.Opcode == NdOp::CALL && Op.Inputs[0].isConst()) {
        auto Callee = Image->PPC64EntryContexts.find(Op.Inputs[0].ConstVal);
        if (Callee != Image->PPC64EntryContexts.end() && Callee->second.TOC &&
            Callee->second.TOC != EntryTOC)
          throw LowToMedConversionError(
              "PPC64 ELFv1 direct call crosses unproven TOC contexts");
      }
      if (Op.Opcode == NdOp::INDIR_CALL) {
        const auto *Mask = Defs.find(stripCopies(Op.Inputs[0], Defs));
        std::optional<MedVar> Base;
        if (Mask && Mask->Opcode == NdOp::INT_AND && Mask->NumInputs == 2 &&
            Mask->Inputs[1].isConst() &&
            Mask->Inputs[1].Provenance == ConstantAddressProvenance::Scalar &&
            Mask->Inputs[1].ConstVal == ~(uint64_t(InstructionAlignment) - 1))
          Base = fieldBase(Mask->Inputs[0], EntryOffset, Defs, Loads);
        if (!Base || !TOC || !Env ||
            fieldBase(*TOC, TOCOffset, Defs, Loads) != Base ||
            fieldBase(*Env, EnvironmentOffset, Defs, Loads) != Base)
          throw LowToMedConversionError(
              "PPC64 ELFv1 indirect call has no proven function descriptor");
        if (Image)
          checkDescriptorInitializer(*Base, Defs, *Image, EntryTOC);
        Op.Inputs[0] = *Base;
      }

      if (Op.Opcode == NdOp::LOAD && Op.Output.Size == WordSize &&
          Op.NumInputs == 1 &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          Op.MemoryOrdering == NdMemoryOrdering::None)
        Loads.insert(&Op);
      if ((opcodeSupportsMemoryAddressSpace(Op.Opcode) &&
           Op.Opcode != NdOp::LOAD) ||
          (Op.Output.Size && Op.Output.Kind == MedVar::Stack) ||
          Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        Loads.clear();
        if (Op.Opcode != NdOp::STORE) {
          TOC.reset();
          Env.reset();
        }
      }
      if (Op.Output.Kind == MedVar::Reg && Op.Output.Size == WordSize) {
        if (Op.Output.RegOff == TOCReg)
          TOC = Op.Output;
        if (Op.Output.RegOff == EnvReg)
          Env = Op.Output;
      }
    }
  }
  if (EntryTOC)
    bindTOCAddresses(Function, Defs, *Image, *EntryTOC);
}
} // namespace neverd
