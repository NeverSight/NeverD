//===- SBFRustEmitterPartitioned.cpp - Function-owned Rust regions --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounds per-function Rust compiler work while preserving one VM state and
/// every raw dynamic entry. One static handler table uses the existing host
/// trait through a borrowed trait object, without allocating a handler vector.
///
//===----------------------------------------------------------------------===//

#include "SBFRustEmitterDetail.h"

#include "neverd/sbf/emit/SBFSourceStatus.h"

namespace neverd::sbf::rust_emitter_detail {

void emitPartitionedProgram(
    llvm::raw_ostream &OS, const SBFProgram &Program,
    const std::map<size_t, const MedInstruction *> &BySlot,
    const SourcePartition &Partition) {
  emitCallHelpers(OS, Program);
  emitMemoryHelpers(OS, Program);
  OS << "#[allow(dead_code)]\nstruct NdCallStack {\n"
        "    saved: [[u64; SAVED_REGISTERS]; MAX_CALL_DEPTH],\n"
        "    saved_fp: [u64; MAX_CALL_DEPTH],\n"
        "    return_pc: [usize; MAX_CALL_DEPTH],\n"
        "    depth: usize,\n"
        "}\n\n"
        "struct NdMachine {\n"
        "    r: NdRegisters,\n"
        "    stack: NdCallStack,\n"
        "    pc: usize,\n"
        "}\n\n";
  for (const auto &Region : Partition.Regions) {
    bool NeedsFaultCheck = false;
    bool NeedsFallthroughPC = false;
    for (size_t Slot : Region.Slots) {
      const auto Found = BySlot.find(Slot);
      if (Program.Low.Instructions[Slot].IsContinuation ||
          Found == BySlot.end())
        continue;
      const auto &Instruction = *Found->second;
      const bool HasNext = Instruction.Slot + Instruction.SlotWidth <
                           Program.Low.Instructions.size();
      NeedsFaultCheck |=
          (HasNext && (Instruction.Op == Operation::Load ||
                       Instruction.Op == Operation::Store)) ||
          Instruction.Op == Operation::CallX ||
          (Instruction.Op == Operation::Call &&
           Instruction.Call == CallKind::Internal && Instruction.CallTarget);
      NeedsFallthroughPC |=
          HasNext &&
          ((Instruction.Semantics.Terminator == TerminatorKind::None &&
            Instruction.Op != Operation::Invalid) ||
           (Instruction.Op == Operation::Call &&
            (Instruction.Call == CallKind::Syscall ||
             Instruction.Call == CallKind::Unresolved)));
    }
    OS << "#[inline(never)]\nfn " << Region.Name
       << "(env: &mut dyn SbfEnvironmentV2, machine: &mut NdMachine) -> "
          "Result<Option<u64>, SbfErrorV2> {\n"
          "    let mut pc = machine.pc;\n"
          "    let mut fault: Option<SbfErrorV2> = None;\n"
          "    let r = &mut machine.r; let stack = &mut machine.stack;\n"
          "    let _ = (&mut *env, &mut pc, &mut fault, &mut *r, &mut "
          "*stack);\n"
          "    loop {\n"
          "        let current_pc = pc;\n";
    // One default advance replaces per-arm assignments. A region containing
    // only transfers/faults has no fallthrough and needs no unused assignment.
    if (NeedsFallthroughPC)
      OS << "        pc = pc.wrapping_add(1);\n";
    OS << "        match current_pc {\n";
    for (size_t Slot : Region.Slots) {
      const auto Found = BySlot.find(Slot);
      if (Program.Low.Instructions[Slot].IsContinuation ||
          Found == BySlot.end()) {
        OS << "            " << Slot
           << " => { machine.pc = current_pc; return "
              "Err("
           << rustSourceErrorName(FaultCode::InvalidInstruction) << "); },\n";
        continue;
      }
      emitInstruction(OS, *Found->second, Program, true);
    }
    OS << "            _ => { machine.pc = current_pc; "
          "return "
          "Ok(None); },\n"
          "        }\n";
    if (NeedsFaultCheck)
      OS << "        if let Some(error) = fault { return Err(error); }\n";
    OS << "    }\n}\n\n";
  }
  OS << "const ND_REGION_BY_PC: &[usize] = &[\n";
  for (size_t Region : Partition.RegionBySlot)
    OS << "    " << Region << ",\n";
  OS << "];\n"
        "type NdRegionHandler = fn(&mut dyn SbfEnvironmentV2, &mut NdMachine) "
        "-> "
        "Result<Option<u64>, SbfErrorV2>;\n"
        "const ND_REGION_HANDLERS: &[NdRegionHandler] = &[\n";
  for (const auto &Region : Partition.Regions)
    OS << "    " << Region.Name << ",\n";
  OS << "];\n\n"
        "fn nd_program_impl<E: SbfEnvironmentV2>(env: &mut E, input: u64, "
        "instruction_data: u64) -> Result<u64, SbfErrorV2> {\n"
        "    let mut machine = NdMachine {\n"
        "        r: NdRegisters::default(), stack: NdCallStack {\n"
        "        saved: [[0u64; SAVED_REGISTERS]; "
        "MAX_CALL_DEPTH],\n"
        "        saved_fp: [0u64; MAX_CALL_DEPTH], return_pc: [0usize; "
        "MAX_CALL_DEPTH],\n"
        "        depth: 0 }, pc: "
     << Program.Low.EntrySlot
     << ",\n"
        "    };\n"
        "    machine."
     << reg(kFirstArgumentRegister) << " = input; machine."
     << reg(kInstructionDataRegister) << " = instruction_data;\n    machine."
     << reg(kFramePointerRegister) << " = STACK_START + "
     << (versionHasFeature(Program.Low.TheVersion,
                           VersionFeature::ManualStackFrames)
             ? "STACK_SIZE"
             : "STACK_FRAME_SIZE")
     << ";\n"
        "    let handlers = ND_REGION_HANDLERS;\n"
        "    loop {\n"
        "        let region = *ND_REGION_BY_PC.get(machine.pc).ok_or("
     << rustSourceErrorName(FaultCode::ExecutionOverrun)
     << ")?;\n"
        "        if let Some(value) = handlers[region](env, &mut machine)? { "
        "return Ok(value); }\n"
        "    }\n}\n";
}

} // namespace neverd::sbf::rust_emitter_detail
