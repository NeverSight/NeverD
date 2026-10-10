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
#include "neverd/ir/SourceCallTypeHint.h"
#include "neverd/ir/SourceTypeHint.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/SourceClassGetterCall.h"
#include "neverd/ir/low/SourceRegisterCopy.h"

#include "llvm/ADT/SmallVector.h"

#include <map>
#include <memory>
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
    EHSelector,
    /// The exception code a Windows __except handler is entered with.  Each
    /// handler entry has its own value: SSAVer numbers the entries from 1 in
    /// address order and ConstVal holds the entry's address.
    SEHExceptionCode,
    /// A value the machine leaves unspecified, such as a volatile register
    /// when the unwinder enters an exception handler. It lowers to an
    /// explicit unknown rather than any variable's value.
    Unspecified
  };
  /// The id of every SEHExceptionCode value.  Value maps key a variable by
  /// id and version, so it must differ from the -1 of other synthetic values.
  static constexpr int SEHExceptionCodeId = -2;
  /// The id of every Unspecified value, distinct from the ids above.
  static constexpr int UnspecifiedId = -3;
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

  static MedVar makeUnspecified(uint16_t Sz, Arch A) {
    MedVar V;
    V.Kind = Unspecified;
    V.TheArch = A;
    V.Id = UnspecifiedId;
    V.Size = Sz;
    return V;
  }

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
  /// The value the PHI takes when its block is entered by the exception
  /// dispatcher rather than from a predecessor: an SEH handler block that is
  /// also an ordinary join receives the exception code this way.
  std::optional<MedVar> ExceptionalEntry;
};

struct MedOp {
  enum class RegistrationRootKind : uint8_t {
    None,
    EstablishedFramePointer,
    CallbackStackPointer,
    RestoredStackPointer,
    /// Runtime establisher and restored ESP of a separately aligned frame.
    /// Their non-affine coordinates come from the checked LowIR contract.
    RealignedFramePointer,
    RealignedRestoredStackPointer,
    /// Runtime EBP below source EBP in a checked fixed C++ frame.
    DisplacedFramePointer,
  };
  NdOp Opcode = NdOp::NOP;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  MedVar Output = {};
  /// Values defined by a multi-result intrinsic besides its primary Output.
  /// LowIR writes these through following COPY/sub-register operations; keep
  /// the definition on the intrinsic so SSA does not invent a live-in value
  /// when those transport operations are later propagated away.
  std::vector<MedVar> IntrinsicOutputs;
  // Ordinary operations retain six inline slots. Source-bound calls may carry
  // their full scalar argument list through the same SSA/liveness operands.
  llvm::SmallVector<MedVar, 6> Inputs = llvm::SmallVector<MedVar, 6>(6);
  uint8_t NumInputs = 0;
  va_t Addr = 0;
  /// Sequence number of the original LowOp.  Synthetic MedOps keep -1, so a
  /// public Low occurrence can be rebound only to its exact surviving op after
  /// all SSA/fixup/propagation passes have completed.
  int OriginSeq = -1;
  /// An implicit runtime entry definition proved by SSA's x86 registration
  /// frame owner. Keep it as a definition: substituting its ordinary incoming
  /// register would lose the callback's distinct ABI context.
  RegistrationRootKind RegistrationRoot = RegistrationRootKind::None;
  /// For a proven C++ continuation, ESP is this signed offset from the
  /// established source EBP. Other registration root kinds keep zero.
  int32_t RegistrationStackOffset = 0;
  uint32_t CallSiteId = 0;
  std::shared_ptr<const SourceCallTypeHint> SourceCallHint;
  /// RETURN's final input is the separately published Swift error value.
  /// Only source-call lowering may establish this transport before SSA.
  bool HasSourceErrorResult = false;
  bool Dead = false;
  bool PreservesCallerSaved = false;
  /// GPR families (see CallRegisterEffects.h) the direct callee provably
  /// never writes, so SSA keeps their pre-call values across this call.
  uint32_t CallPreservedGPRs = 0;
  /// Win64 register arguments the direct callee reads (RCX, RDX, R8, R9 in
  /// order), published as Inputs[1..N]; -1 when the callee is unsummarized.
  int8_t CalleeRegisterArgs = -1;
  /// The vector argument registers the summarized callee reads, published
  /// after the register arguments as Inputs[1 + CalleeRegisterArgs..]; -1
  /// when the convention publishes none.
  int8_t CalleeVectorArgs = -1;
  /// Where a positional convention passes a floating argument in its slot's
  /// vector register instead: bit K for register argument K, Inputs[1 + K].
  uint8_t CalleeVectorSlots = 0;
  /// The inputs after the target are every argument the call passes, in the
  /// source's order (a FormattedCall).  Bit K of ExactFloatInputs marks
  /// Inputs[1 + K] as the bits of a floating one, of ExactPointerInputs as an
  /// address.
  bool ExactArguments = false;
  uint64_t ExactFloatInputs = 0;
  uint64_t ExactPointerInputs = 0;
  /// The bytes of each of those whole vector registers the callee reads:
  /// four bits per argument, or per slot, in four-byte units (1 float, 2
  /// double, 4 all).
  uint32_t CalleeVectorArgWidths = 0;
  /// The bytes the callee reads of input \p Input, a vector argument, or 0.
  uint16_t vectorArgumentWidth(unsigned Input) const {
    if (CalleeVectorSlots) {
      const unsigned Slot = Input - 1;
      if (Input == 0 || Slot >= 8 || !((CalleeVectorSlots >> Slot) & 1))
        return 0;
      return static_cast<uint16_t>(
          ((CalleeVectorArgWidths >> (4 * Slot)) & 0xF) * 4);
    }
    if (CalleeVectorArgs <= 0 || CalleeRegisterArgs < 0 ||
        Input < 1u + CalleeRegisterArgs ||
        Input >= 1u + CalleeRegisterArgs + CalleeVectorArgs)
      return 0;
    const unsigned K = Input - 1 - CalleeRegisterArgs;
    return static_cast<uint16_t>(((CalleeVectorArgWidths >> (4 * K)) & 0xF) *
                                 4);
  }
  /// Positional arguments implied by the incoming stack slots that same
  /// summarized callee reads (0 when it reads none); -1 when unbounded.
  int8_t CalleeStackArgs = -1;
  /// The source instruction is a proven no-return call.  This is explicit MedIR
  /// control provenance: consumers must not infer it again from a mutable name.
  bool DoesNotReturn = false;

  void addInput(MedVar V) {
    if (NumInputs < 65) {
      if (NumInputs == Inputs.size())
        Inputs.push_back(V);
      else
        Inputs[NumInputs] = V;
      ++NumInputs;
    }
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

/// Declaration-quality evidence about whether a function returns a semantic
/// value.  This is deliberately independent of MedFunc::ReturnType: the type
/// pass must choose a backend type even when it only sees a stale ABI return
/// register, so that heuristic type is not proof that a source-level value is
/// returned.  Only an authenticated declaration or an explicit semantic model
/// may publish a concrete return contract; absence of either stays Unknown.
enum class MedReturnValueEvidence : uint8_t {
  Unknown,
  ReturnsNoValue,
  /// A semantic value exists, but its ABI carrier class is not authenticated.
  /// This can justify only potential ownership flow.
  ReturnsValue,
  ReturnsPointer,
  ReturnsInteger,
  ReturnsFloatingPoint,
  ReturnsAggregate,
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
  std::shared_ptr<const SourceCallTypeHint> SourceCallHint;
  bool IsIndirect = false;
  /// Darwin AArch64 indirect variadic call: number of fixed (register-prefix)
  /// arguments before the stack-passed variadic tail.  -1 = not variadic.
  int VarArgFixedCount = -1;
  /// Args passes the register arguments in the callee's own register order
  /// (MedFunc::IntegerArgumentRegisters), not the convention's.
  bool ArgumentsInCalleeRegisterOrder = false;
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
  SourceRegisterCopies RegisterCopyProjections;
  SourceClassGetterCalls ClassGetterCallFacts;
  CallingConv CC = CallingConv::Unknown;
  /// Source x86 return cleanup, retained independently of inferred signatures.
  int CalleePopBytes = 0;
  /// Source and preserved callees have a checked caller-cleanup stack contract.
  bool RegistrationCallerCleanupABIComplete = false;
  /// Bytes reserved below and above the synthetic entry stack pointer.
  int64_t FrameSize = 0;
  int64_t FrameHeadroom = 0;

  TypeRef ReturnType;

  /// Source projection only; never an authenticated semantic return contract.
  std::optional<SourceFunctionTypeHint> SourceTypeHint;
  /// Internal bookkeeping: Param operands already use source declaration
  /// indices, rather than generic ABI register / pointer-sized stack slots.
  bool SourceParametersBound = false;

  /// Trusted return-value evidence for consumers that need declaration
  /// semantics rather than a code-generation type.  Debug providers remain
  /// queryable separately, so ordinary lifting leaves this Unknown.
  MedReturnValueEvidence ReturnValueEvidence = MedReturnValueEvidence::Unknown;

  /// A small struct returned by value across multiple registers (x86-64 SysV
  /// eightbytes / AArch64 HFA): when non-empty the function returns an LLVM
  /// aggregate built from these registers (in this order) so the backend's ABI
  /// lowering places each field in the correct return register.  Empty for the
  /// ordinary single-register / sret return paths.  Populated in the pipeline
  /// from the caller-side struct-return remodel (modelCallStructReturn).
  std::vector<MedReturnReg> MultiReturn;

  std::vector<MedTypedParam> TypedParams;
  /// The width (4 or 8 bytes) of the scalar each floating-point argument
  /// register parameter carries in its low lane, by register offset, when
  /// the function reads no other byte of it: a float or double, no vector.
  std::map<uint64_t, uint16_t> FPParamScalarBytes;
  std::vector<MedTypedLocal> TypedLocals;
  std::vector<MedCallInfo> CallInfos;

  /// The floating-point return value leaves through the x87 top-of-stack (st0),
  /// the i386 cdecl convention for an external function: it is declared with a
  /// scalar FP return type so LLVM lowers it to st0 (instead of the XMM0 vector
  /// return used by clang's internal convention for static functions).
  bool FPReturnViaX87 = false;
  /// LowIR explicitly bound every RETURN to the proven logical x87 top.
  /// The operand remains the result after SSA propagation changes its carrier
  /// to a temporary or constant; this is independent of heuristic typing.
  bool ExplicitX87ReturnValue = false;
  /// Bytes of the result every return path defines when that is fewer than
  /// the return register holds (a `bool` left in AL over an undefined RAX);
  /// 0 otherwise.  The bytes above belong to no result.
  uint16_t DefinedReturnBytes = 0;

  /// Every reachable path is proven to terminate without returning.  This is
  /// derived from explicit architectural traps and no-return callees, never
  /// from the function name or from an unexplained missing return.
  bool DoesNotReturn = false;

  /// The function returns no value a caller could rely on
  /// (settleReturnContracts), so C shows it as void.  Code generation keeps
  /// the return register.
  bool ReturnsNoValue = false;

  /// How execution reaches Entry, which fixes the alignment of the stack
  /// pointer there (functionEntryKind).
  StackEntryKind EntryKind = StackEntryKind::Call;

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

  /// The position of the first integer argument register a variadic
  /// function's register save area spills (AAPCS64): the registers before it
  /// carry its named parameters, the rest only what each caller passes.  -1
  /// where unknown.
  int VariadicFirstRegister = -1;

  /// The registers this function takes its integer register arguments in,
  /// in argument order, where they are not its convention's
  /// (TargetRegInfo::integerArgumentLayout): a local i386 function GCC
  /// compiled takes them in EAX, EDX, ECX.  Empty otherwise.
  std::vector<uint64_t> IntegerArgumentRegisters;

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
  /// Exact Med SSA selector plans keyed by native branch address and current
  /// Med block. Cloned blocks can share a machine address but never an SSA
  /// selector occurrence merely because those addresses compare equal.
  std::map<std::pair<va_t, int>, MedSwitchSelectorPlan> SwitchSelectorPlans;
  std::vector<MedScalarAddressModel> ScalarAddressModels;
  std::vector<MedI386GetPcModel> I386GetPcModels;
  /// Published machine entry/exception/address-taken/continuation roots.
  /// An ordinary edge becoming infeasible never removes another entry role.
  std::set<va_t> ModuleAnalysisRoots;
  /// Exact continuation exits rebound after all MedIR rewrites.  Binding is a
  /// transaction: any missing, duplicate, or ABI-width-invalid RETURN leaves
  /// this vector empty and the completion flag false.
  std::vector<MedCxxContinuationExitEvidence> CxxContinuationExits;
  std::vector<LowCxxContinuationEntryEvidence> CxxContinuationEntries;
  bool CxxContinuationExitAnalysisComplete = false;
  /// LowIR fail-closed identity for mutable/uncertain indirect branches.  Kept
  /// separately from JumpTables so HighIR never turns a rejected table back
  /// into an indirect call merely because the CFG has no static successors.
  std::set<va_t> UnsafeIndirectBranchAddresses;
  std::optional<ExceptionFunction> ExceptionMetadata;
  std::optional<RegistrationStateAnalysis> RegistrationStates;

  bool hasTypeInfo() const { return ReturnType != nullptr; }

  const MedCallInfo *findCall(int BlockId, int OpIdx) const {
    for (const auto &CI : CallInfos)
      if (CI.BlockId == BlockId && CI.OpIdx == OpIdx)
        return &CI;
    return nullptr;
  }
  /// LowToMed kept the unoptimized, non-SSA form (the function exceeded the
  /// SSA size limit): a register variable has no unique definition.
  bool SkippedSSA = false;
};

} // namespace neverd

#endif // NEVERD_IR_MED_MEDIR_H
