//===- RegistrationCRTData.cpp - Private CRT data receipts ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Prove only that an authenticated CRT int store cannot alias image IATs.
///
//===----------------------------------------------------------------------===//

#include "RegistrationCRTData.h"

#include "RegistrationABIPrivate.h"
#include "RegistrationScalarPackets.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/ADT/StringRef.h"

namespace neverd::registration_abi {

bool isCheckedRegistrationCRTDataSlot(const BinaryImage &Image, va_t Slot) {
  const auto *Import = checkedImportSlot(Image, Slot);
  if (!Import || Import->IsDelayImport ||
      !isFileBackedPEImportSlot(Image, Slot) ||
      (Import->Name != "_fmode" && Import->Name != "_commode"))
    return false;
  const llvm::StringRef Module = Import->Module;
  return Module.equals_insensitive("msvcrt.dll") ||
         Module.equals_insensitive("msvcr100.dll");
}

void CRTDataStoreProof::clear() {
  Registers = {};
  Temps.clear();
  Active = false;
}

std::optional<CRTDataStoreProof::Pointer>
CRTDataStoreProof::read(const NdVar &Var) const {
  if (Var.isReg() && Var.Size == 4 &&
      Var.Offset < Registers.size() * x86reg::GeneralRegStride &&
      Var.Offset % x86reg::GeneralRegStride == 0) {
    const auto Slot = Registers[Var.Offset / x86reg::GeneralRegStride];
    return Slot ? std::optional<Pointer>{{*Slot, 4}} : std::nullopt;
  }
  if (Var.isTemp())
    if (const auto It = Temps.find(Var.Offset);
        It != Temps.end() && It->second.Width == Var.Size)
      return It->second;
  return std::nullopt;
}

void CRTDataStoreProof::write(const NdVar &Var, std::optional<Pointer> Value) {
  if (Var.isTemp()) {
    if (Value && Value->Width == Var.Size)
      Temps.insert_or_assign(Var.Offset, *Value);
    else
      Temps.erase(Var.Offset);
  } else if (Var.isReg() &&
             Var.Offset < Registers.size() * x86reg::GeneralRegStride) {
    // Partial aliases destroy the complete runtime address. Only a full x86
    // GPR copy can introduce it; wide instruction-local address temporaries
    // remain separate from architectural registers.
    for (size_t I = 0; I != Registers.size(); ++I)
      if (Var.Offset < I * x86reg::GeneralRegStride + 4 &&
          I * x86reg::GeneralRegStride < Var.Offset + Var.Size)
        Registers[I].reset();
    if (Value && Value->Width == 4 && Var.Size == 4 &&
        Var.Offset % x86reg::GeneralRegStride == 0)
      Registers[Var.Offset / x86reg::GeneralRegStride] = Value->Slot;
  }
  Active |= Value.has_value();
}

bool CRTDataStoreProof::transfer(const LowOp &Op,
                                 std::optional<va_t> AuthenticatedLoad,
                                 bool BindingsLive, bool ClosedRead,
                                 size_t &Work, bool &DisjointStore) {
  DisjointStore = false;
  if (Instruction != Op.Addr) {
    Temps.clear();
    Instruction = Op.Addr;
  }
  if (!Active && !AuthenticatedLoad)
    return true;
  if (!chargeCalleeWork(Work, Registers.size() + Temps.size() + 1))
    return false;
  if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None) {
    clear();
    return true;
  }
  std::optional<Pointer> Value;
  if (Op.Opcode == NdOp::LOAD) {
    // An opaque read can dispatch a continuing handler. It must not preserve
    // an earlier register address, even if that register is not the output.
    // A separately closed current read retains only untouched carriers;
    // write() still invalidates any full or partial destination alias below.
    if (!ClosedRead)
      clear();
    if (AuthenticatedLoad && BindingsLive && Op.NumInputs == 1 &&
        Op.Output.isTemp() && Op.Output.Size == 4)
      Value = Pointer{*AuthenticatedLoad, 4};
  } else if (Op.Opcode == NdOp::STORE) {
    if (Op.NumInputs == 2 && Op.Inputs[1].Size == 4)
      DisjointStore = read(Op.Inputs[0]).has_value();
    // A receipt is consumed once. A second write needs a fresh authenticated
    // load, never a saved memory cell or an assumption about trap callbacks.
    clear();
    return true;
  } else if (!isNontrappingFrameScalarOperation(Op)) {
    clear();
    return true;
  } else if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
             Op.Output.Size == Op.Inputs[0].Size)
    Value = read(Op.Inputs[0]);
  else if (Op.Opcode == NdOp::INT_ZEXT && Op.NumInputs == 1 &&
           Op.Inputs[0].Size == 4 && Op.Output.Size == 8) {
    Value = read(Op.Inputs[0]);
    if (Value)
      Value->Width = 8;
  }
  write(Op.Output, Value);
  return true;
}

} // namespace neverd::registration_abi
