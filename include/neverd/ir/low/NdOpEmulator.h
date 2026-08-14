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
/// It is also the concrete half of a concolic walk, through the adapter at the
/// bottom of this file: the symbolic engine executes the same operator set and
/// wants somebody to say which way a branch actually went.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_NDOP_EMULATOR_H
#define NEVERD_IR_LOW_NDOP_EMULATOR_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/symbolic/SymExplore.h"

#include "llvm/ADT/ArrayRef.h"

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

/// What an emulated run carried on past rather than carrying out.
///
/// Stepping past what cannot be modelled is the right default here: the
/// emulator exists to fold one constant along one path, every use of that
/// constant is checked against the image afterwards, and abandoning the path
/// at the first vector instruction loses real tables for nothing.  What was
/// wrong was doing it *silently* — a value folded after an opcode nobody
/// implemented arrives looking exactly like one that was computed.  Counting
/// is enough to tell the two apart after the fact; strict mode is for a caller
/// who would rather have no answer than an unexamined one.
struct EmulatorSkips {
  /// Operations with no model at all, stepped over without an effect.
  unsigned UnsupportedOps = 0;
  /// Stores dropped because the write-back store was already full.
  unsigned DroppedStores = 0;
  /// Operations carried out approximately: an opaque intrinsic whose output
  /// was invalidated rather than computed.
  unsigned ApproximatedOps = 0;

  bool any() const {
    return UnsupportedOps != 0 || DroppedStores != 0 || ApproximatedOps != 0;
  }
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

  /// Execute a sequence of ops.  Stops at the first terminator.  For backwards
  /// compatibility, a same-address non-zero COND_BR/control sequence is treated
  /// as one legacy predicated instruction; without boundaries this overload
  /// deliberately cannot infer predicated memory or intrinsic effects.
  /// Returns the number of ops successfully executed.
  size_t run(const std::vector<LowOp> &Ops);

  /// Execute a block while using instruction boundaries as the authoritative
  /// description of predicated-effect slices.  An internal guard can skip or
  /// execute the instruction's memory, intrinsic, and control effects.  Unlike
  /// the legacy vector overload, this remains correct at VA zero.  Malformed
  /// non-empty boundary metadata fails closed and executes no operation.
  size_t run(const LowBlock &Block);

  /// Compute a jump-table target by emulating with a given index value.
  /// Sets the index register, runs the ops, and returns the INDIR_BR
  /// target register value.
  std::optional<uint64_t> computeTarget(const std::vector<LowOp> &Ops,
                                        uint64_t IndexRegOff,
                                        uint64_t IndexValue);

  /// Compute and canonicalize an indirect target using the boundary that owns
  /// the terminating INDIR_BR.  Missing or inconsistent metadata and invalid
  /// mode/alignment combinations fail closed.  The vector overload above
  /// deliberately retains its raw-address compatibility contract.
  std::optional<LowControlTarget> computeTarget(const LowBlock &Block,
                                                uint64_t IndexRegOff,
                                                uint64_t IndexValue);

  /// Enable/disable collection of LOAD records during emulation.
  void setLoadCollect(bool Enable) { CollectLoads = Enable; }

  /// Retrieve the collected LOAD records (valid after run/computeTarget).
  const std::vector<LoadRecord> &getLoadRecords() const { return LoadLog; }

  /// Collapse contiguous LOAD records into merged entries.
  static void collapseLoadRecords(std::vector<LoadRecord> &Records);

  /// End the emulated path at anything the emulator cannot carry out exactly:
  /// an opcode with no model, an opaque intrinsic it can only invalidate the
  /// output of, and a store the write-back store has no room for.
  ///
  /// Off by default, and deliberately so — switch recovery wants the path to
  /// continue and validates the address it arrives at instead.  A caller
  /// reading values back out of the emulated state rather than validating
  /// them, a concolic walk above all, wants the opposite.  Persists across
  /// reset().
  void setStrictMode(bool Strict) { StrictMode = Strict; }
  bool strictMode() const { return StrictMode; }

  /// What was skipped or approximated.  Accumulates across reset(), so a
  /// caller emulating one path per switch index can ask once at the end
  /// whether any of them stepped over something.
  const EmulatorSkips &skips() const { return Skips; }
  void clearSkips() { Skips = EmulatorSkips(); }

private:
  const BinaryImage &Img;
  std::map<uint64_t, uint64_t> Registers;
  std::map<uint64_t, uint64_t> MemStore;
  bool CollectLoads = false;
  std::vector<LoadRecord> LoadLog;
  bool StepOverCalls = false;
  bool StrictMode = false;
  std::optional<NdVar> ReachedIndirectBranchTarget;
  std::optional<InstructionMode> ReachedSourceMode;
  std::optional<LowInstructionTargetMode> ReachedTargetMode;
  EmulatorSkips Skips;
  std::vector<uint64_t> CallPreservedRegs;

  uint64_t readOperand(const NdVar &Operand) const;
  void writeOutput(const NdVar &Output, uint64_t Value);
  std::optional<uint64_t> loadMemory(uint64_t Addr, uint16_t Size) const;
  /// Returns false when the write-back store had no room, which is recorded
  /// either way and only ends the path in strict mode.
  bool storeMemory(uint64_t Addr, uint16_t Size, uint64_t Value);

  bool executeArith(const LowOp &Op);
  bool executeLoad(const LowOp &Op);
  bool executeStore(const LowOp &Op);
  bool executeCopy(const LowOp &Op);
  bool executeCompare(const LowOp &Op);
  bool executeBool(const LowOp &Op);
  bool executeMisc(const LowOp &Op);

  size_t runImpl(llvm::ArrayRef<LowOp> Ops,
                 llvm::ArrayRef<LowInstructionBoundary> Boundaries);
  std::optional<uint64_t> reachedIndirectTarget() const;

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

/// Drives an \c NdOpEmulator as the concrete half of a concolic walk.
///
/// The two engines are joined here rather than in the symbolic library because
/// this side is the one allowed to know about both: the emulator reads a
/// loaded image, and the symbolic engine deliberately does not depend on the
/// loader.  That it comes to a handful of forwarding calls is the point — both
/// engines already execute the same operator set, so the concrete run needs no
/// second implementation of anything, and the two cannot drift apart in the
/// way a purpose-built shadow interpreter would.
///
/// Enable strict mode on the emulator before handing it over.  A concolic walk
/// reads values back out of the concrete state to decide branches, and the
/// lenient default would let it read a register an unmodelled operation left
/// stale — which is the one way a concrete run can be wrong without saying so.
class NdOpEmulatorShadow final : public symbolic::ConcreteShadow {
public:
  explicit NdOpEmulatorShadow(NdOpEmulator &Emu) : Emu(Emu) {}

  void reset() override { Emu.reset(); }

  void setRegister(uint64_t Offset, uint64_t Value) override {
    Emu.setRegister(Offset, Value);
  }

  bool step(const LowOp &Op) override { return Emu.step(Op); }

  std::optional<uint64_t> value(const NdVar &V) const override {
    if (V.isConst())
      return V.Offset;
    if (V.isReg() || V.isTemp())
      return Emu.getRegister(V.Offset);
    return std::nullopt;
  }

private:
  NdOpEmulator &Emu;
};

} // namespace neverd

#endif // NEVERD_IR_LOW_NDOP_EMULATOR_H
