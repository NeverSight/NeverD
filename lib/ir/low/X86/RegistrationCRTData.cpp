//===- RegistrationCRTData.cpp - Private CRT data receipts ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Track limited accesses through pointers to CRT-owned cells.
///
//===----------------------------------------------------------------------===//

#include "RegistrationCRTData.h"

#include "RegistrationABIPrivate.h"
#include "RegistrationScalarPackets.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/RuntimeProviders.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/ADT/StringRef.h"

namespace neverd::registration_abi {

std::optional<CRTDataAccess>
registrationCRTAccessorAccess(const Import &Imported) {
  if ((Imported.Name == "__p___argc" || Imported.Name == "__p___argv") &&
      coff_loader::isUCRTRuntimeProvider(Imported.Module))
    return CRTDataAccess::ReadArgumentWord;
  if (Imported.Name == "__p__commode" &&
      coff_loader::isUCRTStdioProvider(Imported.Module))
    return CRTDataAccess::WriteInt;
  return std::nullopt;
}

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

void CRTDataProof::clear() {
  Registers = {};
  Temps.clear();
  Active = false;
}

std::optional<CRTDataProof::Pointer>
CRTDataProof::read(const NdVar &Var) const {
  if (Var.isReg() && Var.Size == 4 &&
      Var.Offset < Registers.size() * x86reg::GeneralRegStride &&
      Var.Offset % x86reg::GeneralRegStride == 0) {
    const auto Access = Registers[Var.Offset / x86reg::GeneralRegStride];
    return Access ? std::optional<Pointer>{{*Access, 4}} : std::nullopt;
  }
  if (Var.isTemp())
    if (const auto It = Temps.find(Var.Offset);
        It != Temps.end() && It->second.Width == Var.Size)
      return It->second;
  return std::nullopt;
}

void CRTDataProof::write(const NdVar &Var, std::optional<Pointer> Value) {
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
      Registers[Var.Offset / x86reg::GeneralRegStride] = Value->Access;
  }
  Active |= Value.has_value();
}

void CRTDataProof::recordAccessorReturn(CRTDataAccess Access,
                                        const NdVar &Output) {
  clear();
  const Pointer Value{Access, 4};
  write(NdVar::reg(x86reg::RAX, 4), Value);
  write(Output, Value);
}

bool CRTDataProof::transfer(const LowOp &Op, bool AuthenticatedDataLoad,
                            bool BindingsLive, bool ClosedAccess, size_t &Work,
                            std::optional<CRTDataAccess> &CurrentAccess) {
  if (Instruction != Op.Addr) {
    Temps.clear();
    Instruction = Op.Addr;
  }
  if (!Active && !AuthenticatedDataLoad)
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
    if (Op.NumInputs == 1 && Op.Output.Size == 4)
      if (const auto Address = read(Op.Inputs[0]);
          Address && Address->Access == CRTDataAccess::ReadArgumentWord)
        CurrentAccess = Address->Access;
    if (!ClosedAccess && !CurrentAccess)
      clear();
    // The live argc or argv word carries no address or scalar provenance.
    // Only an independently authenticated import can introduce a new cell.
    if (AuthenticatedDataLoad && BindingsLive && Op.NumInputs == 1 &&
        Op.Output.isTemp() && Op.Output.Size == 4)
      Value = Pointer{CRTDataAccess::WriteInt, 4};
  } else if (Op.Opcode == NdOp::STORE) {
    if (Op.NumInputs == 2 && Op.Inputs[1].Size == 4)
      if (const auto Address = read(Op.Inputs[0]);
          Address && Address->Access == CRTDataAccess::WriteInt)
        CurrentAccess = Address->Access;
    // A write through the pointer consumes it. An independent, permitted
    // private stack store (such as PUSH imm) leaves the register untouched.
    if (CurrentAccess || !ClosedAccess)
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
