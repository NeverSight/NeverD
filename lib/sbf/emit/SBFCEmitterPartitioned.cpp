//===- SBFCEmitterPartitioned.cpp - Function-owned C regions --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Renders bounded C dispatch regions using the shared VM state. Native
/// function pointers select trusted rendering regions, never guest addresses.
/// All guest calls and returns retain their existing software frame protocol.
///
//===----------------------------------------------------------------------===//

#include "SBFCEmitterDetail.h"

#include "neverd/sbf/emit/SBFSourceStatus.h"

namespace neverd::sbf::c_emitter_detail {

void emitPartitionedProgram(
    llvm::raw_ostream &OS, const SBFProgram &Program,
    const std::map<size_t, const MedInstruction *> &BySlot,
    const SourcePartition &Partition) {
  emitCallHelpers(OS, Program);
  emitMemoryHelpers(OS, Program);
  OS << "typedef struct nd_machine {\n"
        "  uint64_t r[NEVERD_SBF_REGISTER_COUNT];\n"
        "  uint64_t "
        "saved[NEVERD_SBF_MAX_CALL_DEPTH][NEVERD_SBF_SAVED_REGISTERS];\n"
        "  uint64_t saved_fp[NEVERD_SBF_MAX_CALL_DEPTH];\n"
        "  uint32_t return_pc[NEVERD_SBF_MAX_CALL_DEPTH];\n"
        "  size_t depth;\n"
        "  uint32_t pc;\n"
        "  int halted;\n"
        "  uint64_t result;\n"
        "} nd_machine;\n\n";
  for (const auto &Region : Partition.Regions) {
    OS << "static neverd_sbf_status_v2 " << Region.Name
       << "(nd_environment_view *env, nd_machine *machine) {\n"
          "  uint64_t *r = machine->r;\n"
          "  uint64_t (*saved)[NEVERD_SBF_SAVED_REGISTERS] = machine->saved;\n"
          "  uint64_t *saved_fp = machine->saved_fp;\n"
          "  uint32_t *return_pc = machine->return_pc;\n"
          "  size_t depth = machine->depth, i = 0; uint32_t pc = machine->pc;\n"
          "  uint64_t *result = &machine->result;\n"
          "  neverd_sbf_status_v2 memory_status = "
       << cSourceStatusName(SourceStatus::Ok)
       << ";\n"
          "  (void)env; (void)r; (void)saved; (void)saved_fp;\n"
          "  (void)return_pc; (void)depth; (void)i; (void)result; "
          "(void)memory_status;\n"
          "  for (;;) {\n    switch (pc) {\n";
    for (size_t Index = 0; Index < Region.Slots.size(); ++Index) {
      const size_t Slot = Region.Slots[Index];
      const auto Found = BySlot.find(Slot);
      if (Program.Low.Instructions[Slot].IsContinuation ||
          Found == BySlot.end()) {
        OS << "      case " << Slot
           << ": machine->pc = pc; machine->depth = depth; return "
           << cSourceStatusName(FaultCode::InvalidInstruction) << ";\n";
        continue;
      }
      // Each raw entry keeps its case label. Sequential execution can use C
      // fallthrough only when the next rendered case is the actual next PC.
      // In particular an LDDW must skip its continuation's trapping case.
      const bool FallThrough =
          Index + 1 < Region.Slots.size() &&
          Slot + Found->second->SlotWidth == Region.Slots[Index + 1];
      emitInstruction(OS, *Found->second, Program, true, FallThrough);
    }
    OS << "      default: machine->pc = pc; machine->depth = depth; return "
       << cSourceStatusName(SourceStatus::Ok) << ";\n    }\n  }\n}\n\n";
  }
  OS << "static const uint32_t nd_region_by_pc[] = {\n";
  for (size_t Region : Partition.RegionBySlot)
    OS << "  " << Region << ",\n";
  OS << "};\n"
        "typedef neverd_sbf_status_v2 (*nd_region_handler)(nd_environment_view "
        "*, nd_machine *);\n"
        "static const nd_region_handler nd_region_handlers[] = {\n";
  for (const auto &Region : Partition.Regions)
    OS << "  " << Region.Name << ",\n";
  OS << "};\n\n"
        "static neverd_sbf_status_v2 nd_program_impl("
        "nd_environment_view *env, uint64_t input, uint64_t instruction_data, "
        "uint64_t *result) {\n"
        "  nd_machine machine = {0};\n"
        "  machine.pc = "
     << Program.Low.EntrySlot
     << ";\n"
        "  machine.r[NEVERD_SBF_INPUT_REGISTER] = input;\n"
        "  machine.r[NEVERD_SBF_INSTRUCTION_DATA_REGISTER] = "
        "instruction_data;\n"
        "  machine.r[NEVERD_SBF_FRAME_POINTER] = NEVERD_SBF_STACK_START + "
     << (versionHasFeature(Program.Low.TheVersion,
                           VersionFeature::ManualStackFrames)
             ? "NEVERD_SBF_STACK_SIZE"
             : "NEVERD_SBF_STACK_FRAME_SIZE")
     << ";\n"
        "  for (;;) {\n"
        "    neverd_sbf_status_v2 status;\n"
        "    if (machine.pc >= NEVERD_SBF_INSTRUCTION_COUNT) return "
     << cSourceStatusName(FaultCode::ExecutionOverrun)
     << ";\n"
        "    status = nd_region_handlers[nd_region_by_pc[machine.pc]](env, "
        "&machine);\n"
        "    if (status != "
     << cSourceStatusName(SourceStatus::Ok)
     << ") return status;\n"
        "    if (machine.halted) { if (result) *result = machine.result; "
        "return "
     << cSourceStatusName(SourceStatus::Ok)
     << "; }\n"
        "  }\n}\n";
}

} // namespace neverd::sbf::c_emitter_detail
