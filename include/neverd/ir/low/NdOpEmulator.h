//===- NdOpEmulator.h - Light-weight NdOp emulation -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// NdOp emulator interface for computing switch-table targets by
/// executing NdOp sequences along data-flow paths.
///
/// The emulator is designed for single-path, non-branching execution:
/// BRANCH/COND_BR/INDIR_BR terminate emulation.  Memory state is
/// tracked per-NdVar (register-file slots), and LOAD/STORE operate
/// against a read-only BinaryImage.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_NDOP_EMULATOR_H
#define NEVERD_IR_LOW_NDOP_EMULATOR_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace neverd {

/// Record of a memory LOAD performed during emulation.
struct LoadRecord {
  uint64_t Addr = 0;
  uint16_t Size = 0;
};

/// Light-weight NdOp emulation engine.
///
/// Usage:
/// \code
///   NdOpEmulator Emu(Img);
///   Emu.setRegister(regOff, value);
///   for (auto &Op : Ops)
///     Emu.step(Op);
///   auto Result = Emu.getRegister(targetRegOff);
/// \endcode
class NdOpEmulator {
public:
  explicit NdOpEmulator(const BinaryImage &Img) : Img(Img) {}

  void reset();

  void setRegister(uint64_t RegOff, uint64_t Value);
  std::optional<uint64_t> getRegister(uint64_t RegOff) const;

  /// Declare the registers that survive a call by ABI (the stack pointer, frame
  /// pointer, and callee-saved registers) and, by doing so, allow the emulator
  /// to step *over* a CALL/INDIR_CALL instead of stopping there.  A call then
  /// keeps these register values and drops the rest (the caller-saved /
  /// volatile set), so a table base materialised after an intervening call is
  /// still recoverable without ever reading a stale caller-saved value.
  /// Persists across reset().  When never called, a call conservatively ends
  /// the emulated path (the default).  Kept as configuration rather than
  /// derived internally so the emulator stays free of target-register tables.
  void setCallPreservedRegisters(std::vector<uint64_t> Regs);

  /// Execute a single NdOp.  Returns false if the op ends the single linear
  /// execution path: always for BRANCH/COND_BR/INDIR_BR/RETURN, and for
  /// CALL/INDIR_CALL unless call-preserved registers have been declared (see
  /// setCallPreservedRegisters), in which case the call is stepped over.
  bool step(const LowOp &Op);

  /// Execute a sequence of ops.  Stops at the first terminator.
  /// Returns the number of ops successfully executed.
  size_t run(const std::vector<LowOp> &Ops);

  /// Compute a jump-table target by emulating with a given index value.
  /// Sets the index register, runs the ops, and returns the INDIR_BR
  /// target register value.
  std::optional<uint64_t> computeTarget(const std::vector<LowOp> &Ops,
                                        uint64_t IndexRegOff,
                                        uint64_t IndexValue);

  /// Enable/disable collection of LOAD records during emulation.
  void setLoadCollect(bool Enable) { CollectLoads = Enable; }

  /// Retrieve the collected LOAD records (valid after run/computeTarget).
  const std::vector<LoadRecord> &getLoadRecords() const { return LoadLog; }

  /// Collapse contiguous LOAD records into merged entries.
  static void collapseLoadRecords(std::vector<LoadRecord> &Records);

private:
  const BinaryImage &Img;
  std::map<uint64_t, uint64_t> Registers;
  std::map<uint64_t, uint64_t> MemStore;
  bool CollectLoads = false;
  std::vector<LoadRecord> LoadLog;
  bool StepOverCalls = false;
  std::vector<uint64_t> CallPreservedRegs;

  uint64_t readOperand(const NdVar &Operand) const;
  void writeOutput(const NdVar &Output, uint64_t Value);
  std::optional<uint64_t> loadMemory(uint64_t Addr, uint16_t Size) const;
  void storeMemory(uint64_t Addr, uint16_t Size, uint64_t Value);

  bool executeArith(const LowOp &Op);
  bool executeLoad(const LowOp &Op);
  bool executeStore(const LowOp &Op);
  bool executeCopy(const LowOp &Op);
  bool executeCompare(const LowOp &Op);
  bool executeBool(const LowOp &Op);
  bool executeMisc(const LowOp &Op);

  /// Drop the recorded values of every register a call is permitted to
  /// overwrite (caller-saved / volatile registers), keeping only the
  /// call-preserved registers declared via setCallPreservedRegisters().  Called
  /// when stepping over a CALL/INDIR_CALL so that linear constant tracing can
  /// continue to a table base materialised after the call, without ever folding
  /// a stale caller-saved value.  Temp/IR-local map entries (which no call
  /// clobbers) are dropped too, but they are never live across an instruction
  /// boundary, so this is inconsequential.
  void clobberVolatileRegisters();
};

} // namespace neverd

#endif // NEVERD_IR_LOW_NDOP_EMULATOR_H
