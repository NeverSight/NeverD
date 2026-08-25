//===- MedIR.h - Medium-level IR definitions ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the medium-level IR: MedVar (SSA variables), MedOp (SSA
/// operations), MedBlock (basic blocks with phi nodes), and MedFunc
/// (function-level container with calling convention and type info).
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDIR_H
#define NEVERD_IR_MED_MEDIR_H

#include "neverd/Common.h"
#include "neverd/ir/NdTypes.h"
#include "neverd/ir/low/LowIR.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neverd {

/// Sentinel RegOff for a stack-passed parameter (Kind==Param with no backing
/// register).  The emitter must not enter such a param into its register->arg
/// map; a stack parameter is wired to the body purely by its display name.
constexpr uint64_t kNoParamReg = ~0ULL;

struct MedVar {
  enum VarKind : uint8_t {
    Reg,
    Stack,
    Temp,
    Param,
    RetVal,
    Flag,
    Const,
    EHException,
    EHSelector
  };
  VarKind Kind = Temp;
  Arch TheArch = Arch::Unknown;
  int16_t RenameTag = -1;
  int Id = 0;
  int SSAVer = 0;
  uint16_t Size = 0;
  /// Occurrence-sensitive provenance copied from LowIR.  It is meaningful for
  /// Kind==Const and remains separate from the numeric value so scalar,
  /// incomplete-page, and complete-address occurrences cannot share a model.
  ConstantAddressProvenance Provenance = ConstantAddressProvenance::Unknown;
  uint64_t AddressOwnerVA = InvalidVA;

  union {
    int64_t StackOff = 0;
    uint64_t RegOff;
    uint64_t ConstVal;
  };

  bool operator==(const MedVar &O) const {
    if (Kind != O.Kind)
      return false;
    if (Kind == Const)
      return ConstVal == O.ConstVal && Size == O.Size &&
             Provenance == O.Provenance && AddressOwnerVA == O.AddressOwnerVA;
    return Id == O.Id && SSAVer == O.SSAVer;
  }
  bool operator!=(const MedVar &O) const { return !(*this == O); }

  bool isConst() const { return Kind == Const; }

  static MedVar makeConst(uint64_t Val, uint16_t Sz,
                          ConstantAddressProvenance AddressProvenance =
                              ConstantAddressProvenance::Unknown,
                          uint64_t AddressOwner = InvalidVA) {
    MedVar V;
    V.Kind = Const;
    V.Id = -1;
    V.Size = Sz;
    V.ConstVal = Val;
    V.Provenance = AddressProvenance;
    V.AddressOwnerVA = AddressOwner;
    return V;
  }

  std::string display() const;
};

/// Selector identity resolved after Low-to-Med SSA and propagation.  Backends
/// consume this plan instead of rediscovering a value from physical register
/// numbers or by rescanning a mutable MedIR DAG.
struct MedSwitchSelectorPlan {
  enum class Kind : uint8_t { Direct, SelectOffset, EdgeMerged };

  Kind PlanKind = Kind::Direct;
  /// Direct selector, or byte-coordinate index for SelectOffset.
  MedVar Selector = {};
  /// Canonical table-selection condition for SelectOffset.
  MedVar Condition = {};
  uint64_t TrueOffset = 0;
  uint64_t FalseOffset = 0;
  uint16_t ResultSize = 0;
  /// One exact selector per direct predecessor of the shared dispatch block.
  /// Backends may materialize this edge merge without rediscovering values
  /// from physical register numbers or target-spill address heuristics.
  std::vector<std::pair<int, MedVar>> EdgeSelectors;
};

/// Post-SSA binding of a relocation-authenticated scalar address model.
/// Backends compare the complete MedVar identity; a rewritten, deleted, or
/// lane-changed defining operation therefore cannot inherit the certificate.
struct MedScalarAddressModel {
  RelocatedInstructionScalarModelOccurrence::ModelKind Model =
      RelocatedInstructionScalarModelOccurrence::ModelKind::I386ELFGOTBaseZero;
  MedVar Value = {};
};

/// Post-SSA binding of a CFG-authenticated i386 `call $+5; pop reg` result.
/// The value is a raw architectural PC used in PIC arithmetic, not permission
/// to reinterpret another numerically equal value as an address.
struct MedI386GetPcModel {
  /// Exact surviving architectural COPY output.  This is the only operation
  /// the emitter may replace with the authenticated raw PC.
  MedVar Output = {};
  /// Exact COPY input carrying the POP LOAD result.  Med copy propagation may
  /// route later PIC arithmetic through this SSA value instead of Output.
  MedVar Value = {};
  uint32_t PCValue = 0;
};

/// Post-rewrite binding of one LowIR Windows C++ continuation RETURN.  The
/// source pair survives for auditing, while BlockId and ReturnValue name the
/// exact final MedIR occurrence after CFG simplification, SSA, and propagation.
struct MedCxxContinuationExitEvidence {
  va_t ReturnAddr = InvalidVA;
  int ReturnSeq = -1;
  int BlockId = -1;
  MedVar ReturnValue = {};
  std::vector<va_t> Targets;
  bool Complete = false;

  std::optional<va_t> uniqueTarget() const {
    if (!Complete || Targets.size() != 1)
      return std::nullopt;
    return Targets.front();
  }
};

struct PhiNode {
  MedVar Output;
  std::vector<std::pair<int, MedVar>> Args;
};

struct MedOp {
  NdOp Opcode = NdOp::NOP;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  MedVar Output = {};
  MedVar Inputs[6] = {};
  uint8_t NumInputs = 0;
  va_t Addr = 0;
  /// Sequence number of the original LowOp.  Synthetic MedOps keep -1, so a
  /// public Low occurrence can be rebound only to its exact surviving op after
  /// all SSA/fixup/propagation passes have completed.
  int OriginSeq = -1;
  uint32_t CallSiteId = 0;
  bool Dead = false;
  bool PreservesCallerSaved = false;
  /// The source instruction is a proven no-return call.  This is explicit MedIR
  /// control provenance: consumers must not infer it again from a mutable name.
  bool DoesNotReturn = false;

  void addInput(MedVar V) {
    if (NumInputs < 6)
      Inputs[NumInputs++] = V;
  }
};

enum class CallingConv : uint8_t {
  SysV_AMD64,
  Win64,
  CDECL,
  ARM_AAPCS,
  Unknown,
};

struct MedBlock {
  int Id = -1;
  /// Original half-open machine-code extent.  This survives lifting so
  /// address-based metadata (notably Windows EH regions) never has to infer a
  /// block boundary from an instruction name or from a possibly empty Op list.
  va_t StartAddr = 0;
  va_t EndAddr = 0;
  std::vector<PhiNode> Phis;
  std::vector<MedOp> Ops;
  std::vector<int> Succs;
  std::vector<int> Preds;
  std::vector<ExceptionalEdge> ExceptionalSuccs;
  std::vector<ExceptionalEdge> ExceptionalPreds;
};

struct MedTypedParam {
  std::string Name;
  TypeRef Type;
};

/// One register of a small aggregate (struct-by-value) returned in multiple
/// registers: SysV x86-64 returns a <=16-byte struct in up to two eightbyte
/// registers (an INTEGER eightbyte in RAX/RDX, an SSE eightbyte in XMM0/XMM1),
/// and AArch64 returns a homogeneous floating aggregate in V0..V3.  Listed in
/// aggregate-field order (the order the LLVM struct return type is built in),
/// which the ABI lowering maps back to the registers.
struct MedReturnReg {
  uint64_t RegOff = 0; ///< ABI register offset (RAX/RDX/X0/X1/XMM0/V0...).
  uint16_t Size = 0;   ///< Field value size in bytes (8 double/long, 4 float).
  bool IsFP = false;   ///< Returned in an FP/vector register (XMM/V), else GPR.
};

/// An SSA value implicitly defined by a particular call because its physical
/// register is caller-saved.
struct MedCallClobber {
  MedVar Value;
  uint32_t CallSiteId = 0;
  /// Pre-call value supplying the preserved low prefix.  Empty when the whole
  /// register view is caller-saved.
  MedVar PreservedInput = {};
  uint16_t PreservedPrefixSize = 0;
};

/// Caller-observed AArch64 direct-call return fields that include implicit
/// clobbers.  Whole-program recovery validates these against the callee before
/// materializing a multi-register return.
struct MedStructReturnCandidate {
  uint32_t CallSiteId = 0;
  std::vector<MedVar> Fields;
};

struct MedTypedLocal {
  std::string Name;
  TypeRef Type;
  int64_t StackOff = 0;
};

struct MedCallInfo {
  int BlockId = -1;
  int OpIdx = -1;
  va_t TargetAddr = 0;
  std::string TargetName;
  std::vector<MedVar> Args;
  bool IsIndirect = false;
  /// Darwin AArch64 indirect variadic call: number of fixed (register-prefix)
  /// arguments before the stack-passed variadic tail.  -1 = not variadic.
  int VarArgFixedCount = -1;
};

struct MedFunc {
  va_t Entry = 0;
  uint64_t OriginalSize = 0;
  std::string Name;
  std::string DebugName;
  std::string SourceFile;
  uint32_t SourceLine = 0;
  std::vector<MedBlock> Blocks;
  std::vector<MedVar> Params;
  std::vector<MedVar> Locals;
  /// Exact SSA values implicitly defined by CALL/INDIR_CALL for caller-saved
  /// registers not represented by the call's explicit return value.
  std::vector<MedCallClobber> CallClobbers;
  std::vector<MedStructReturnCandidate> StructReturnCandidates;
  CallingConv CC = CallingConv::Unknown;
  /// Bytes reserved below and above the synthetic entry stack pointer.
  int64_t FrameSize = 0;
  int64_t FrameHeadroom = 0;

  TypeRef ReturnType;

  /// A small struct returned by value across multiple registers (x86-64 SysV
  /// eightbytes / AArch64 HFA): when non-empty the function returns an LLVM
  /// aggregate built from these registers (in this order) so the backend's ABI
  /// lowering places each field in the correct return register.  Empty for the
  /// ordinary single-register / sret return paths.  Populated in the pipeline
  /// from the caller-side struct-return remodel (modelCallStructReturn).
  std::vector<MedReturnReg> MultiReturn;

  std::vector<MedTypedParam> TypedParams;
  std::vector<MedTypedLocal> TypedLocals;
  std::vector<MedCallInfo> CallInfos;

  /// The floating-point return value leaves through the x87 top-of-stack (st0),
  /// the i386 cdecl convention for an external function: it is declared with a
  /// scalar FP return type so LLVM lowers it to st0 (instead of the XMM0 vector
  /// return used by clang's internal convention for static functions).
  bool FPReturnViaX87 = false;

  /// Every reachable path is proven to terminate without returning.  This is
  /// derived from explicit architectural traps and no-return callees, never
  /// from the function name or from an unexplained missing return.
  bool DoesNotReturn = false;

  /// A variadic function (`f(fixed..., ...)`): its prologue spills the argument
  /// registers to a register save area and `va_arg` walks that area then the
  /// caller's overflow (incoming-stack) area.  The register save area
  /// round-trips through ordinary register parameters (store-to-load
  /// forwarding), but the overflow reads land above the synthetic alloca frame;
  /// the emitter therefore reserves headroom above frame_end and spills the
  /// recovered overflow stack parameters into it so the unchanged va_arg walk
  /// reads the correct values.
  bool IsVariadic = false;

  /// Byte offset above the entry stack pointer (frame_end) where the variadic
  /// overflow area starts — the first incoming stack argument the va_arg walk
  /// reads (8 on x86-64 past the return address, 0 on AArch64/ARM).
  int64_t VariadicOverflowBase = 0;

  /// Number of NAMED stack parameters that precede the variadic overflow area
  /// (the fixed prefix passed on the stack rather than in registers).  Nonzero
  /// only for a Darwin AArch64 variadic function with more than 8 named integer
  /// arguments, where args 9.. are stack-passed *fixed* args before the varargs
  /// (overflow base > 0).  finalizeVariadicCallees adds this to the register
  /// parameter count to size the fixed prefix; zero elsewhere (every named arg
  /// is register-passed, overflow base 0).
  int VariadicFixedStackArgs = 0;

  /// Number of overflow stack parameters spilled into the headroom (the
  /// trailing stack parameters of a variadic function), set once all call sites
  /// are known.
  int VariadicOverflowCount = 0;

  /// Incoming stack-argument home slots the function also WRITES (a parameter
  /// updated in place, e.g. in a loop): the loads/stores stay as memory
  /// accesses through [frame_end + offset], so the emitter reserves headroom
  /// there and spills the corresponding parameter into each at entry.  Without
  /// the spill a read of the uninitialised home slot would diverge; folding the
  /// read to the incoming parameter value instead (the read-only case) would
  /// drop the update. Each entry is (param_index, byte offset above frame_end).
  std::vector<std::pair<int, int64_t>> MutableStackParamHomes;

  /// Resolved jump tables (carried from LowFunc) so the LLVM emitter can
  /// lower an INDIR_BR into a switch on the table index.
  std::vector<JumpTable> JumpTables;
  /// Exact Med SSA selector plans keyed by the indirect-branch address.
  std::map<va_t, MedSwitchSelectorPlan> SwitchSelectorPlans;
  std::vector<MedScalarAddressModel> ScalarAddressModels;
  std::vector<MedI386GetPcModel> I386GetPcModels;
  /// Exact continuation exits rebound after all MedIR rewrites.  Binding is a
  /// transaction: any missing, duplicate, or ABI-width-invalid RETURN leaves
  /// this vector empty and the completion flag false.
  std::vector<MedCxxContinuationExitEvidence> CxxContinuationExits;
  bool CxxContinuationExitAnalysisComplete = false;
  /// LowIR fail-closed identity for mutable/uncertain indirect branches.  Kept
  /// separately from JumpTables so HighIR never turns a rejected table back
  /// into an indirect call merely because the CFG has no static successors.
  std::set<va_t> UnsafeIndirectBranchAddresses;
  std::optional<ExceptionFunction> ExceptionMetadata;

  bool hasTypeInfo() const { return ReturnType != nullptr; }

  const MedCallInfo *findCall(int BlockId, int OpIdx) const {
    for (const auto &CI : CallInfos)
      if (CI.BlockId == BlockId && CI.OpIdx == OpIdx)
        return &CI;
    return nullptr;
  }
};

} // namespace neverd

#endif // NEVERD_IR_MED_MEDIR_H
