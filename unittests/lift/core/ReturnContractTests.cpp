//===- ReturnContractTests.cpp - Functions that return no value ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// settleReturnContracts decides, on MedIR and for both C backends, which
/// functions return no value a caller could rely on.
///
//===----------------------------------------------------------------------===//

#include "../../../lib/pipeline/PipelineReturnModelingDetail.h"
#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/LLVMSourceUnknownValue.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/MedReturnArguments.h"
#include "neverd/ir/med/MedReturnValue.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Limits.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t CalleeEntry = 0x1000;
constexpr va_t CallerEntry = 0x2000;

MedVar rax(int Version) {
  MedVar Value;
  Value.Kind = MedVar::Reg;
  Value.TheArch = Arch::X64;
  Value.Id = 0;
  Value.SSAVer = Version;
  Value.Size = 8;
  Value.RegOff = x86reg::RAX;
  return Value;
}

MedVar temporary(int Id) {
  MedVar Value;
  Value.Kind = MedVar::Temp;
  Value.TheArch = Arch::X64;
  Value.Id = Id;
  Value.SSAVer = 1;
  Value.Size = 8;
  return Value;
}

/// \p Value at \p Size bytes.
MedVar sized(MedVar Value, uint16_t Size) {
  Value.Size = Size;
  return Value;
}

/// A value of \p Size bytes in the register at \p Offset, which \p Id names
/// apart from the other values there (a register and its narrower views).
MedVar inRegister(uint64_t Offset, uint16_t Size, int Id, int Version) {
  MedVar Value;
  Value.Kind = MedVar::Reg;
  Value.TheArch = Arch::X64;
  Value.Id = Id;
  Value.SSAVer = Version;
  Value.Size = Size;
  Value.RegOff = Offset;
  return Value;
}

MedOp operation(NdOp Opcode, MedVar Output, std::vector<MedVar> Inputs) {
  MedOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  for (const MedVar &Input : Inputs)
    Op.addInput(Input);
  return Op;
}

/// The entry seed of RAX: the register as the function entered it.
MedOp seed() { return operation(NdOp::COPY, rax(0), {rax(0)}); }

MedOp returning(const MedVar &Value) {
  return operation(NdOp::RETURN, {}, {Value});
}

MedFunc function(va_t Entry, std::vector<MedBlock> Blocks) {
  MedFunc Func;
  Func.Entry = Entry;
  Func.Name = "f" + std::to_string(Entry);
  Func.ReturnType = NdType::makeInt(8);
  Func.Blocks = std::move(Blocks);
  for (size_t I = 0; I < Func.Blocks.size(); ++I)
    Func.Blocks[I].Id = static_cast<int>(I);
  return Func;
}

/// A block that returns \p Value after \p Ops.
MedBlock block(std::vector<MedOp> Ops, std::vector<int> Preds = {},
               std::vector<int> Succs = {}) {
  MedBlock Block;
  Block.Ops = std::move(Ops);
  Block.Preds = std::move(Preds);
  Block.Succs = std::move(Succs);
  return Block;
}

/// One path hands back RAX as the function entered it; the other, what
/// \p Last defines.
MedFunc branching(va_t Entry, MedOp Last) {
  const MedVar Value = Last.Output;
  return function(Entry,
                  {block({seed(), operation(NdOp::COND_BR, {}, {temporary(9)})},
                         {}, {1, 2}),
                   block({returning(rax(0))}, {0}),
                   block({std::move(Last), returning(Value)}, {0})});
}

TEST(ReturnContract, ExplicitX87PrecisionRequiresAnExactIEEEValue) {
  MedVar IEEE = temporary(1);
  IEEE.Size = 8;
  MedVar Extended = temporary(2);
  Extended.Size = 10;
  MedVar Result = temporary(3);
  Result.Size = 10;
  const MedOp Widen = operation(NdOp::FLOAT_FLOAT2FLOAT, Extended, {IEEE});
  auto Width = [&](std::vector<MedOp> Ops) {
    Ops.push_back(returning(Result));
    auto Func = function(CalleeEntry, {block(std::move(Ops))});
    Func.ExplicitX87ReturnValue = true;
    inferMedTypes(Func, Arch::X86);
    return Func.ReturnType->Size;
  };
  EXPECT_EQ(Width({Widen, operation(NdOp::COPY, Result, {Extended})}), 8u);
  EXPECT_EQ(Width({Widen, operation(NdOp::SUBBYTES, Result,
                                    {Extended, MedVar::makeConst(0, 4)})}),
            8u);
  EXPECT_EQ(Width({Widen, operation(NdOp::SUBBYTES, Result,
                                    {Extended, MedVar::makeConst(1, 4)})}),
            10u);
  EXPECT_EQ(
      Width({Widen, operation(NdOp::FLOAT_ADD, Result, {Extended, Extended})}),
      10u);
  EXPECT_EQ(Width({Widen, operation(NdOp::INT_XOR, Result,
                                    {Extended, MedVar::makeConst(1, 10)})}),
            10u);
  EXPECT_EQ(Width({operation(NdOp::LOAD, Result, {IEEE})}), 10u);
}

MedOp callTo(va_t Target, int Version) {
  return operation(
      NdOp::CALL, rax(Version),
      {MedVar::makeConst(Target, 8, ConstantAddressProvenance::CodeAddress)});
}

/// Debug information that declares what some functions return, by their
/// entries, and the prototypes of some routines, by their names.
class DeclaringDebugContext final : public DebugContext {
public:
  std::map<va_t, TypeRef> Returns;
  std::map<std::string, TypeRef> Prototypes;
  std::optional<FunctionSym> resolveFunction(va_t Addr) const override {
    const auto Return = Returns.find(Addr);
    if (Return == Returns.end())
      return std::nullopt;
    FunctionSym Function;
    Function.Addr = Addr;
    Function.ReturnType = Return->second;
    return Function;
  }
  std::optional<FunctionSym>
  resolvePrototype(llvm::StringRef Name) const override {
    const auto Return = Prototypes.find(Name.str());
    if (Return == Prototypes.end())
      return std::nullopt;
    FunctionSym Function;
    Function.Name = Name.str();
    Function.ReturnType = Return->second;
    return Function;
  }
  std::optional<VariableSym> resolveVariable(va_t, int64_t) const override {
    return std::nullopt;
  }
  std::optional<TypeSym> resolveType(uint64_t) const override {
    return std::nullopt;
  }
  std::optional<SourceLoc> sourceLocation(va_t) const override {
    return std::nullopt;
  }
  std::vector<FunctionSym> allFunctions() const override { return {}; }
  bool hasInfo() const override { return true; }
};

std::vector<bool> settle(std::vector<MedFunc> Funcs,
                         BinaryFormat Format = BinaryFormat::ELF,
                         const DebugContext *Dbg = nullptr) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = Format;
  PipelineResult Result;
  Result.MedFuncs = std::move(Funcs);
  settleReturnContracts(Image, Result, Dbg);
  std::vector<bool> NoValue;
  for (const MedFunc &Func : Result.MedFuncs)
    NoValue.push_back(Func.ReturnsNoValue);
  return NoValue;
}

/// As settle, returning the functions too.
PipelineResult settled(std::vector<MedFunc> Funcs,
                       BinaryFormat Format = BinaryFormat::ELF) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = Format;
  PipelineResult Result;
  Result.MedFuncs = std::move(Funcs);
  settleReturnContracts(Image, Result, nullptr);
  return Result;
}

/// A callee one path of which hands back RAX as it entered and the other
/// what an unknown call left: no value unless a caller reads one.
MedFunc releasing() {
  return branching(CalleeEntry,
                   operation(NdOp::INDIR_CALL, rax(1), {temporary(5)}));
}

/// A caller that calls the callee, runs \p Ops on what RAX then holds
/// (rax(1)) and returns zero.
MedFunc callerRunning(std::vector<MedOp> Ops) {
  std::vector<MedOp> All = {callTo(CalleeEntry, 1)};
  for (MedOp &Op : Ops)
    All.push_back(std::move(Op));
  All.push_back(returning(MedVar::makeConst(0, 8)));
  return function(CallerEntry, {block(std::move(All))});
}

/// `setcc %al` after the call: RAX keeps the bytes the call left above the
/// one written.  The merged register is rax(2).
std::vector<MedOp> byteSetOverTheResult() {
  return {operation(NdOp::INT_AND, temporary(2),
                    {rax(1), MedVar::makeConst(~0xFFull, 8)}),
          operation(NdOp::INT_ZEXT, temporary(3), {sized(temporary(1), 1)}),
          operation(NdOp::INT_OR, rax(2), {temporary(2), temporary(3)})};
}

std::vector<MedOp> operator+(std::vector<MedOp> Ops, std::vector<MedOp> More) {
  for (MedOp &Op : More)
    Ops.push_back(std::move(Op));
  return Ops;
}

TEST(ReturnContracts, TheRegisterAsTheFunctionEnteredItIsNoValue) {
  EXPECT_EQ(
      settle({function(CalleeEntry, {block({seed(), returning(rax(0))})})}),
      std::vector<bool>{true});
}

TEST(ReturnContracts, AArch64IncomingResultRegisterRequiresACallerRead) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::COFF;
  MedVar X0;
  X0.Kind = MedVar::Reg;
  X0.TheArch = Image.Arch;
  X0.RegOff = getTargetRegInfo(Image.Arch).IntReturnReg;
  X0.Size = 8;
  MedVar Argument = X0;
  Argument.Kind = MedVar::Param;
  MedVar Link = temporary(9);
  Link.TheArch = Image.Arch;
  // AArch64 RET reads its link. The callee only writes through its pointer.
  MedFunc Writer = function(
      CalleeEntry,
      {block({operation(NdOp::COPY, X0, {X0}),
              operation(NdOp::STORE, {}, {X0, MedVar::makeConst(7, 8)}),
              returning(Link)})});
  Writer.Params.push_back(Argument);
  PipelineResult Result;
  Result.MedFuncs = {Writer};
  settleReturnContracts(Image, Result);
  EXPECT_TRUE(Result.MedFuncs.front().ReturnsNoValue);

  MedVar Value = X0;
  Value.SSAVer = 1;
  MedFunc Caller = function(
      CallerEntry,
      {block({operation(NdOp::CALL, Value, {MedVar::makeConst(CalleeEntry, 8)}),
              operation(NdOp::STORE, {}, {Argument, Value}),
              returning(Link)})});
  Result.MedFuncs = {Writer, Caller};
  settleReturnContracts(Image, Result);
  EXPECT_FALSE(Result.MedFuncs.front().ReturnsNoValue);

  // A real write supplies a result even with no observed caller.
  Writer.Blocks.front().Ops.insert(
      Writer.Blocks.front().Ops.end() - 1,
      operation(NdOp::COPY, Value, {MedVar::makeConst(1, 8)}));
  Result.MedFuncs = {Writer};
  settleReturnContracts(Image, Result);
  EXPECT_FALSE(Result.MedFuncs.front().ReturnsNoValue);
}

TEST(ReturnContracts, DefinitionsAndCallsUseTheSameRuntimePrototype) {
  for (const char *Name : {"__cxa_end_catch", "_Unwind_Resume", "free"}) {
    MedFunc Runtime = function(
        CalleeEntry,
        {block({operation(NdOp::COPY, rax(1), {MedVar::makeConst(7, 8)}),
                returning(rax(1))})});
    Runtime.Name = Name;
    EXPECT_EQ(settle({Runtime}), std::vector<bool>{true}) << Name;
  }
  MedFunc Scalar = function(CalleeEntry, {block({seed(), returning(rax(0))})});
  Scalar.Name = "malloc";
  EXPECT_EQ(settle({Scalar}), std::vector<bool>{false});
}

TEST(ReturnContracts, ItaniumConstructionNamesRespectTheTargetABI) {
  for (const char *Name :
       {"_ZN1AC1Ev", "_ZN1AC2Ev", "_ZN1AD0Ev", "_ZN1AD1Ev", "_ZN1AD2Ev",
        "_ZN1A2D1Ev", "_ZN1AD1Evjunk", "_Znot_mangled"})
    for (Arch Architecture : {Arch::X64, Arch::AArch64, Arch::ARM})
      for (BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::MachO}) {
        BinaryImage Image;
        Image.Arch = Architecture;
        Image.Format = Format;
        PipelineResult Result;
        MedFunc Function = function(
            CalleeEntry,
            {block({operation(NdOp::COPY, rax(1), {MedVar::makeConst(7, 8)}),
                    returning(rax(1))})});
        Function.Name = Name;
        Result.MedFuncs.push_back(std::move(Function));
        // The declaration, rather than the chosen leftover register value,
        // settles the ELF constructor/destructor contract.
        settleReturnContracts(Image, Result, nullptr);
        const bool Special = llvm::StringRef(Name).size() == 9;
        EXPECT_EQ(Result.MedFuncs.front().ReturnsNoValue,
                  Special && Format == BinaryFormat::ELF &&
                      Architecture != Arch::ARM)
            << Name << " " << static_cast<unsigned>(Architecture) << " "
            << static_cast<unsigned>(Format);
      }
}

TEST(ReturnContracts, TheResultOfACalleeThatReturnsNoneIsNoValue) {
  // tail_void: `jmp sink`, where sink hands back RAX as it entered.
  const MedFunc Sink =
      function(CalleeEntry, {block({seed(), returning(rax(0))})});
  const MedFunc TailVoid = function(
      CallerEntry, {block({callTo(CalleeEntry, 1), returning(rax(1))})});
  EXPECT_EQ(settle({TailVoid, Sink}), (std::vector<bool>{true, true}));
}

TEST(ReturnContracts, TheResultOfAnUnknownCalleeIsAValue) {
  // PLT0: what the dynamic linker's resolver returns, the function returns.
  EXPECT_EQ(settle({function(CallerEntry,
                             {block({operation(NdOp::INDIR_CALL, rax(1),
                                               {MedVar::makeConst(0x3FC8, 8)}),
                                     returning(rax(1))})})}),
            std::vector<bool>{false});
}

TEST(ReturnContracts, AnUndefinedPathWithoutADeliberateValueIsNoValue) {
  // One path returns RAX as the caller left it, so no caller can rely on a
  // result, whatever the other path's call leaves.
  EXPECT_EQ(settle({branching(CalleeEntry, operation(NdOp::INDIR_CALL, rax(1),
                                                     {temporary(5)}))}),
            std::vector<bool>{true});
}

TEST(ReturnContracts, ACallerThatReadsTheResultKeepsAValue) {
  MedFunc Callee = branching(
      CalleeEntry, operation(NdOp::INDIR_CALL, rax(1), {temporary(5)}));
  // The caller adds 1 to what the callee returns.
  MedFunc Caller = function(
      CallerEntry, {block({callTo(CalleeEntry, 1),
                           operation(NdOp::INT_ADD, rax(2),
                                     {rax(1), MedVar::makeConst(1, 8)}),
                           returning(rax(2))})});
  EXPECT_EQ(settle({std::move(Callee), std::move(Caller)}),
            (std::vector<bool>{false, false}));
}

/// A caller that calls the callee on one path only and merges RAX at the
/// join, then returns what \p Join returns.
MedFunc conditionalCaller(MedVar Untaken, MedOp Return) {
  PhiNode Merge;
  Merge.Output = rax(2);
  Merge.Args = {{0, std::move(Untaken)}, {1, rax(1)}};
  MedBlock Join = block({std::move(Return)}, {0, 1});
  Join.Phis.push_back(std::move(Merge));
  return function(CallerEntry,
                  {block({seed(), operation(NdOp::COND_BR, {}, {temporary(9)})},
                         {}, {1, 2}),
                   block({callTo(CalleeEntry, 1)}, {0}, {2}), std::move(Join)});
}

TEST(ReturnContracts, AResultOnlyADeadMergeReadsIsNoValue) {
  // init_multithreading: RAX merges where nothing reads it, and the caller
  // returns a constant.
  MedFunc Callee = branching(
      CalleeEntry, operation(NdOp::INDIR_CALL, rax(1), {temporary(5)}));
  MedFunc Caller =
      conditionalCaller(rax(0), returning(MedVar::makeConst(0, 8)));
  EXPECT_EQ(settle({std::move(Callee), std::move(Caller)}),
            (std::vector<bool>{true, false}));
}

TEST(ReturnContracts, AResultAMergeHandsBackIsReadByACallerThatReturnsAValue) {
  // The caller returns 5 on the other path, and no path of the callee shows
  // it means no value (one returns what an unknown call left, the other
  // follows a call that does not return): what the caller hands back from
  // it is a value too.
  MedOp Abort = callTo(0x3000, 2);
  Abort.DoesNotReturn = true;
  MedFunc Callee =
      function(CalleeEntry,
               {block({seed(), operation(NdOp::COND_BR, {}, {temporary(9)})},
                      {}, {1, 2}),
                block({operation(NdOp::INDIR_CALL, rax(1), {temporary(5)}),
                       returning(rax(1))},
                      {0}),
                block({Abort, returning(rax(2))}, {0})});
  MedFunc Caller =
      conditionalCaller(MedVar::makeConst(5, 8), returning(rax(2)));
  EXPECT_EQ(settle({std::move(Callee), std::move(Caller)}),
            (std::vector<bool>{false, false}));
}

TEST(ReturnContracts, ACalleeAndTheCallerThatHandsItsResultBackReturnNone) {
  // sqlite3_free and sqlite3BtreeClearCursor: the callee's result is read
  // only if the wrapper returns a value, and the wrapper returns one only if
  // the callee does.  Neither does.
  const MedFunc Wrapper = function(
      CallerEntry, {block({callTo(CalleeEntry, 1), returning(rax(1))})});
  EXPECT_EQ(settle({releasing(), Wrapper}), (std::vector<bool>{true, true}));
}

TEST(ReturnContracts, AReadReachesDownThroughACallerThatHandsTheResultBack) {
  // seh_probe: main compares what a wrapper returns, and the wrapper returns
  // what its callee does.  The callee's result is read.
  const MedFunc Wrapper = function(
      CallerEntry, {block({callTo(CalleeEntry, 1), returning(rax(1))})});
  const MedFunc Reader = function(
      0x3000, {block({callTo(CallerEntry, 1),
                      operation(NdOp::STORE, {}, {temporary(7), rax(1)}),
                      returning(MedVar::makeConst(0, 8))})});
  EXPECT_EQ(settle({releasing(), Wrapper, Reader}),
            (std::vector<bool>{false, false, false}));

  // A read of the float a function returns is no read of the integer
  // register it hands back.
  MedFunc Measuring = Wrapper;
  Measuring.ReturnType = NdType::makeFloat(8);
  EXPECT_EQ(settle({releasing(), Measuring, Reader}),
            (std::vector<bool>{true, false, false}));
}

TEST(ReturnContracts, ACalleeNoPathShowsMeansNoValueWaitsForItsCaller) {
  // MSVC's AArch64 cookie check returns the X0 it was given unless the
  // cookie is wrong, when it calls a routine that does not return; every
  // function it guards returns through it.  No path of it shows it means
  // no value, so a caller that hands its result back may be returning what
  // it passed: both keep their values.
  MedOp Abort = callTo(0x3000, 2);
  Abort.DoesNotReturn = true;
  const MedFunc Check =
      function(CalleeEntry,
               {block({seed(), operation(NdOp::COND_BR, {}, {temporary(9)})},
                      {}, {1, 2}),
                block({operation(NdOp::INDIR_CALL, rax(1), {temporary(5)}),
                       returning(rax(1))},
                      {0}),
                block({Abort, returning(rax(2))}, {0})});
  const MedFunc Guarded = function(
      CallerEntry, {block({callTo(CalleeEntry, 1), returning(rax(1))})});
  EXPECT_EQ(settle({Check, Guarded}), (std::vector<bool>{false, false}));

  // Nothing hands its result back: it returns none, as before.
  EXPECT_EQ(settle({Check}), std::vector<bool>{true});

  // Nor does a caller that is itself settled to return none, whichever of
  // the two is looked at first: one of its paths never wrote the register.
  const MedFunc Early =
      function(CallerEntry,
               {block({seed(), operation(NdOp::COND_BR, {}, {temporary(9)})},
                      {}, {1, 2}),
                block({returning(rax(0))}, {0}),
                block({callTo(CalleeEntry, 1), returning(rax(1))}, {0})});
  EXPECT_EQ(settle({Check, Early}), (std::vector<bool>{true, true}));
  EXPECT_EQ(settle({Early, Check}), (std::vector<bool>{true, true}));
}

TEST(ReturnContracts, AFunctionThatReturnsWhatItsOwnCallReturnsShowsNothing) {
  // One path returns what an unknown call left, the other what the function
  // itself returns: that it may return nothing is no evidence that it does.
  const MedFunc Recursive =
      function(CalleeEntry,
               {block({seed(), operation(NdOp::COND_BR, {}, {temporary(9)})},
                      {}, {1, 2}),
                block({operation(NdOp::INDIR_CALL, rax(1), {temporary(5)}),
                       returning(rax(1))},
                      {0}),
                block({callTo(CalleeEntry, 2), returning(rax(2))}, {0})});
  EXPECT_EQ(settle({Recursive}), std::vector<bool>{false});
}

TEST(ReturnContracts, AResultOnlyUnreadViewsOfTheRegisterReadIsNotRead) {
  // sqlite3BtreeIndexMoveto: the lift keeps the narrower views of RAX a
  // function reads anywhere (`eax`, `al`) in step with it, so a call's
  // result is copied into each view and merged where paths join.  Nothing
  // reads these merges.
  const MedVar Entered = inRegister(x86reg::RAX, 4, 1, 0),
               Left = inRegister(x86reg::RAX, 4, 1, 1),
               Joined = inRegister(x86reg::RAX, 4, 1, 2);
  PhiNode Merge;
  Merge.Output = Joined;
  Merge.Args = {{0, Entered}, {1, Left}};
  MedBlock Join = block({returning(MedVar::makeConst(0, 8))}, {0, 1});
  Join.Phis.push_back(std::move(Merge));
  const MedFunc Caller =
      function(CallerEntry,
               {block({seed(), operation(NdOp::COND_BR, {}, {temporary(9)})},
                      {}, {1, 2}),
                block({callTo(CalleeEntry, 1),
                       operation(NdOp::SUBBYTES, Left,
                                 {rax(1), MedVar::makeConst(0, 4)})},
                      {0}, {2}),
                std::move(Join)});
  EXPECT_EQ(settle({releasing(), Caller}), (std::vector<bool>{true, false}));
}

TEST(ReturnContracts, AnUnreadViewOfTheReturnRegisterReadsNothing) {
  // sqlite3GenerateConstraintChecks: the view of a call's result that not
  // even a merge reads.
  const MedFunc Caller =
      callerRunning({operation(NdOp::SUBBYTES, inRegister(x86reg::RAX, 4, 1, 1),
                               {rax(1), MedVar::makeConst(0, 4)})});
  EXPECT_EQ(settle({releasing(), Caller}), (std::vector<bool>{true, false}));
}

TEST(ReturnContracts, TheBytesASetccMergesOverReadNothing) {
  // `call release; setne %al; mov %al, (%rbx)`: the store reads the byte
  // written, not what the call left above it.  The merge then reads zero in
  // place of the result of a call that returns none.
  const MedVar Low = sized(temporary(4), 1);
  const MedFunc Caller = callerRunning(
      byteSetOverTheResult() +
      std::vector<MedOp>{
          operation(NdOp::SUBBYTES, Low, {rax(2), MedVar::makeConst(0, 8)}),
          operation(NdOp::STORE, {}, {temporary(7), Low})});
  const PipelineResult Result = settled({releasing(), Caller});
  EXPECT_TRUE(Result.MedFuncs[0].ReturnsNoValue);
  const MedOp &Kept = Result.MedFuncs[1].Blocks[0].Ops[1];
  ASSERT_EQ(Kept.Opcode, NdOp::INT_AND);
  EXPECT_TRUE(Kept.Inputs[0].isConst() && Kept.Inputs[0].ConstVal == 0);

  // A store of the whole register reads what the call left.
  const MedFunc Whole = callerRunning(
      byteSetOverTheResult() +
      std::vector<MedOp>{operation(NdOp::STORE, {}, {temporary(7), rax(2)})});
  const PipelineResult Read = settled({releasing(), Whole});
  EXPECT_FALSE(Read.MedFuncs[0].ReturnsNoValue);
  EXPECT_FALSE(Read.MedFuncs[1].Blocks[0].Ops[1].Inputs[0].isConst());
}

TEST(ReturnContracts, AnAndWithAZeroExtendedByteReadsOneByte) {
  // sub_59DC0: `call sqlite3_free; sete %al; and %eax, %ebx` where EBX holds
  // a zero-extended flag.  The bytes of EAX above AL, which the call left,
  // meet zero bytes; a 32-bit store of the result reads none of them.
  const MedVar Eax = sized(temporary(4), 4);
  const auto Ebx = [](int Version) {
    return inRegister(x86reg::RBX, 4, 2, Version);
  };
  const std::vector<MedOp> Anded = {
      operation(NdOp::SUBBYTES, Eax, {rax(2), MedVar::makeConst(0, 8)}),
      operation(NdOp::INT_AND, Ebx(2), {Ebx(1), Eax}),
      operation(NdOp::STORE, {}, {temporary(7), Ebx(2)})};
  const MedFunc Flag =
      callerRunning(std::vector<MedOp>{operation(NdOp::INT_ZEXT, Ebx(1),
                                                 {sized(temporary(6), 1)})} +
                    byteSetOverTheResult() + Anded);
  EXPECT_EQ(settle({releasing(), Flag}), (std::vector<bool>{true, false}));

  // An EBX of unknown bytes shows every byte of EAX.
  const MedFunc Unknown = callerRunning(byteSetOverTheResult() + Anded);
  EXPECT_EQ(settle({releasing(), Unknown}), (std::vector<bool>{false, false}));

  // A flag ANDed around a loop stays the byte it started as: the merge at
  // the loop's head is zero above its low byte because every arm is.
  PhiNode Head;
  Head.Output = Ebx(1);
  Head.Args = {{0, Ebx(0)}, {1, Ebx(2)}};
  std::vector<MedOp> Body = {callTo(CalleeEntry, 1)};
  for (MedOp &Op : byteSetOverTheResult() + Anded)
    Body.push_back(std::move(Op));
  Body.push_back(operation(NdOp::COND_BR, {}, {temporary(9)}));
  MedBlock Loop = block(std::move(Body), {0, 1}, {1, 2});
  Loop.Phis.push_back(std::move(Head));
  const MedFunc Looping = function(
      CallerEntry,
      {block({operation(NdOp::INT_ZEXT, Ebx(0), {sized(temporary(6), 1)})}, {},
             {1}),
       std::move(Loop), block({returning(MedVar::makeConst(0, 8))}, {1})});
  EXPECT_EQ(settle({releasing(), Looping}), (std::vector<bool>{true, false}));
}

TEST(ReturnContracts, ALeftShiftReadsNothingAboveTheBytesRead) {
  // sqlite3_progress_handler: `setne %al; shl $6, %eax; mov %al, 0x6e(%rbx)`.
  // A left shift moves bits up only, so the byte stored comes from AL.
  const MedVar Eax = sized(temporary(4), 4), Shifted = sized(temporary(5), 4),
               Low = sized(temporary(6), 1);
  const MedFunc Caller = callerRunning(
      byteSetOverTheResult() +
      std::vector<MedOp>{
          operation(NdOp::SUBBYTES, Eax, {rax(2), MedVar::makeConst(0, 8)}),
          operation(NdOp::INT_LEFT, Shifted, {Eax, MedVar::makeConst(6, 4)}),
          operation(NdOp::SUBBYTES, Low, {Shifted, MedVar::makeConst(0, 8)}),
          operation(NdOp::STORE, {}, {temporary(7), Low})});
  EXPECT_EQ(settle({releasing(), Caller}), (std::vector<bool>{true, false}));
}

TEST(ReturnContracts, AnOperationPassesOnOnlyTheBytesReadOfIt) {
  // After `setcc %al` the register holds a known byte below the seven the
  // call left.  Each case stores some bytes of what an operation makes of
  // it: the call's result is read exactly where the operation carries one
  // of its bytes into what is stored.
  const MedVar Eax = sized(temporary(4), 4), Out = temporary(5),
               Out4 = sized(temporary(5), 4), Stored = sized(temporary(6), 1);
  const MedOp ViewOfEax =
      operation(NdOp::SUBBYTES, Eax, {rax(2), MedVar::makeConst(0, 8)});
  struct Case {
    const char *Name;
    std::vector<MedOp> Ops;
    /// The operation's result and the byte of it stored.
    MedVar Made;
    uint64_t Byte;
    bool ReadsResult;
  };
  const MedVar Ones = MedVar::makeConst(~0xFFull, 8);
  const std::vector<Case> Cases = {
      {"copy", {operation(NdOp::COPY, Out, {rax(2)})}, Out, 0, false},
      {"copy", {operation(NdOp::COPY, Out, {rax(2)})}, Out, 1, true},
      {"not", {operation(NdOp::INT_NOT, Out, {rax(2)})}, Out, 0, false},
      {"not", {operation(NdOp::INT_NOT, Out, {rax(2)})}, Out, 1, true},
      // A zero extension adds bytes that read nothing.
      {"zext",
       {ViewOfEax, operation(NdOp::INT_ZEXT, Out, {Eax})},
       Out,
       0,
       false},
      {"zext",
       {ViewOfEax, operation(NdOp::INT_ZEXT, Out, {Eax})},
       Out,
       1,
       true},
      {"zext",
       {ViewOfEax, operation(NdOp::INT_ZEXT, Out, {Eax})},
       Out,
       5,
       false},
      // A sign extension repeats the top byte of its source above it.
      {"sext",
       {ViewOfEax, operation(NdOp::INT_SEXT, Out, {Eax})},
       Out,
       0,
       false},
      {"sext",
       {ViewOfEax, operation(NdOp::INT_SEXT, Out, {Eax})},
       Out,
       5,
       true},
      // The first operand is the high part: its low byte is the known one.
      {"concat",
       {ViewOfEax, operation(NdOp::CONCAT, Out, {Eax, sized(temporary(8), 4)})},
       Out,
       4,
       false},
      {"concat",
       {ViewOfEax, operation(NdOp::CONCAT, Out, {Eax, sized(temporary(8), 4)})},
       Out,
       5,
       true},
      // A carry runs upwards only.
      {"add",
       {ViewOfEax,
        operation(NdOp::INT_ADD, Out4, {Eax, MedVar::makeConst(1, 4)})},
       Out4,
       0,
       false},
      {"add",
       {ViewOfEax,
        operation(NdOp::INT_ADD, Out4, {Eax, MedVar::makeConst(1, 4)})},
       Out4,
       1,
       true},
      {"sub",
       {ViewOfEax,
        operation(NdOp::INT_SUB, Out4, {Eax, MedVar::makeConst(1, 4)})},
       Out4,
       0,
       false},
      {"sub",
       {ViewOfEax,
        operation(NdOp::INT_SUB, Out4, {Eax, MedVar::makeConst(1, 4)})},
       Out4,
       1,
       true},
      {"mult",
       {ViewOfEax,
        operation(NdOp::INT_MULT, Out4, {Eax, MedVar::makeConst(3, 4)})},
       Out4,
       0,
       false},
      {"mult",
       {ViewOfEax,
        operation(NdOp::INT_MULT, Out4, {Eax, MedVar::makeConst(3, 4)})},
       Out4,
       1,
       true},
      {"xor",
       {operation(NdOp::INT_XOR, Out, {rax(2), temporary(8)})},
       Out,
       0,
       false},
      {"xor",
       {operation(NdOp::INT_XOR, Out, {rax(2), temporary(8)})},
       Out,
       1,
       true},
      // A byte ORed with ones shows nothing of the other operand.
      {"or ones",
       {operation(NdOp::INT_OR, Out, {rax(2), Ones})},
       Out,
       1,
       false},
      {"or zero",
       {operation(NdOp::INT_OR, Out, {rax(2), MedVar::makeConst(0, 8)})},
       Out,
       1,
       true},
  };
  for (const Case &Each : Cases) {
    const MedFunc Caller = callerRunning(
        byteSetOverTheResult() + Each.Ops +
        std::vector<MedOp>{
            operation(NdOp::SUBBYTES, Stored,
                      {Each.Made, MedVar::makeConst(Each.Byte, 8)}),
            operation(NdOp::STORE, {}, {temporary(7), Stored})});
    EXPECT_EQ(settle({releasing(), Caller}),
              (std::vector<bool>{!Each.ReadsResult, false}))
        << Each.Name << " byte " << Each.Byte;
  }
}

TEST(ReturnContracts, AWordInsertedIntoAVectorReadsNoneOfItsOtherLanes) {
  // sqlite3ExprListDelete's caller: `call f; pinsrw $0, mem, %xmm0; ...;
  // movd %xmm0, mem`.  The insert keeps the other lanes of what the call
  // left in XMM0, and the store reads the inserted word's bytes alone.
  const MedVar Xmm = inRegister(x86reg::XMM0, 16, 3, 1);
  const MedVar Word = sized(temporary(1), 2), Rest = sized(temporary(2), 14),
               Inserted = sized(temporary(3), 16),
               Stored = sized(temporary(4), 2);
  const MedFunc Caller = function(
      CallerEntry,
      {block({operation(
                  NdOp::CALL, Xmm,
                  {MedVar::makeConst(CalleeEntry, 8,
                                     ConstantAddressProvenance::CodeAddress)}),
              operation(NdOp::SUBBYTES, Rest, {Xmm, MedVar::makeConst(2, 8)}),
              operation(NdOp::CONCAT, Inserted, {Rest, Word}),
              operation(NdOp::SUBBYTES, Stored,
                        {Inserted, MedVar::makeConst(0, 8)}),
              operation(NdOp::STORE, {}, {temporary(7), Stored}),
              returning(MedVar::makeConst(0, 8))})});
  const PipelineResult Result = settled({releasing(), Caller});
  EXPECT_TRUE(Result.MedFuncs[0].ReturnsNoValue);
  // That the callee returns no value says nothing of what it left in XMM0:
  // the insert still reads the register.
  const MedOp &Kept = Result.MedFuncs[1].Blocks[0].Ops[1];
  ASSERT_EQ(Kept.Opcode, NdOp::SUBBYTES);
  EXPECT_FALSE(Kept.Inputs[0].isConst());
}

TEST(ReturnContracts, AFunctionThatReturnsAFloatHandsBackNoInteger) {
  // sqlite3_column_double ends in sqlite3_mutex_leave and returns XMM0:
  // RAX is as the call left it at its return, which reads nothing.
  const auto Measuring = [] {
    return function(CallerEntry,
                    {block({callTo(CalleeEntry, 1), returning(rax(1))})});
  };
  MedFunc Inferred = Measuring();
  Inferred.ReturnType = NdType::makeFloat(8);
  EXPECT_EQ(settle({releasing(), Inferred}), (std::vector<bool>{true, false}));

  // A source signature and debug information say the same of theirs.
  MedFunc Hinted = Measuring();
  Hinted.SourceTypeHint.emplace();
  Hinted.SourceTypeHint->ReturnType = NdType::makeFloat(8);
  EXPECT_EQ(settle({releasing(), Hinted}), (std::vector<bool>{true, false}));
  DeclaringDebugContext Debug;
  Debug.Returns[CallerEntry] = NdType::makeFloat(8);
  EXPECT_EQ(settle({releasing(), Measuring()}, BinaryFormat::ELF, &Debug),
            (std::vector<bool>{true, false}));

  // One declared to return an integer reads what it hands back.
  Debug.Returns[CallerEntry] = NdType::makeInt(8);
  EXPECT_EQ(settle({releasing(), Measuring()}, BinaryFormat::ELF, &Debug),
            (std::vector<bool>{false, false}));

  // A target that may return a float in the integer register: read.
  BinaryImage Soft;
  Soft.Arch = Arch::ARM;
  Soft.Format = BinaryFormat::ELF;
  const TargetRegInfo &ARM = getTargetRegInfo(Soft.Arch);
  const auto R0 = [&](int Version) {
    MedVar Value = inRegister(ARM.IntReturnReg, 4, 0, Version);
    Value.TheArch = Soft.Arch;
    return Value;
  };
  MedVar Link = sized(temporary(9), 4);
  Link.TheArch = Soft.Arch;
  const MedOp Seed = operation(NdOp::COPY, R0(0), {R0(0)});
  MedFunc Callee = function(
      CalleeEntry,
      {block({Seed, operation(NdOp::COND_BR, {}, {temporary(8)})}, {}, {1, 2}),
       block({returning(Link)}, {0}),
       block({operation(NdOp::INDIR_CALL, R0(1), {temporary(5)}),
              returning(Link)},
             {0})});
  Callee.ReturnType = NdType::makeInt(4);
  MedFunc Caller = function(
      CallerEntry,
      {block({operation(
                  NdOp::CALL, R0(1),
                  {MedVar::makeConst(CalleeEntry, 4,
                                     ConstantAddressProvenance::CodeAddress)}),
              returning(Link)})});
  Caller.ReturnType = NdType::makeFloat(4);
  PipelineResult Result;
  Result.MedFuncs = {Callee, Caller};
  settleReturnContracts(Soft, Result);
  EXPECT_FALSE(Result.MedFuncs[0].ReturnsNoValue);
}

TEST(ReturnContracts, WhatSomethingUnseenMayReadIsRead) {
  // Memory is read through pointers, and a load or a division runs whatever
  // reads its result: neither lets a call's result go unread.
  const std::vector<std::pair<const char *, MedOp>> Cases = {
      {"a load", operation(NdOp::LOAD, temporary(2), {rax(1)})},
      {"a division",
       operation(NdOp::INT_DIV, temporary(2), {temporary(8), rax(1)})},
  };
  for (const auto &[Name, Op] : Cases)
    EXPECT_EQ(settle({releasing(), callerRunning({Op})}),
              (std::vector<bool>{false, false}))
        << Name;

  // A frame slot is memory: reading one byte of it by name leaves the rest
  // to whatever reads it through a pointer.
  MedVar Slot = temporary(3);
  Slot.Kind = MedVar::Stack;
  const MedVar Low = sized(temporary(4), 1);
  const MedFunc Spilling = callerRunning(
      byteSetOverTheResult() +
      std::vector<MedOp>{
          operation(NdOp::COPY, Slot, {rax(2)}),
          operation(NdOp::SUBBYTES, Low, {Slot, MedVar::makeConst(0, 8)}),
          operation(NdOp::STORE, {}, {temporary(7), Low})});
  EXPECT_EQ(settle({releasing(), Spilling}), (std::vector<bool>{false, false}));

  // The value an exception hands a merge is one of its arguments.
  PhiNode Handler;
  Handler.Output = temporary(4);
  Handler.Args = {{0, temporary(2)}};
  Handler.ExceptionalEntry = rax(1);
  MedBlock Handling =
      block({operation(NdOp::STORE, {}, {temporary(7), temporary(4)}),
             returning(MedVar::makeConst(0, 8))},
            {0});
  Handling.Phis.push_back(Handler);
  const MedFunc Catching = function(
      CallerEntry, {block({callTo(CalleeEntry, 1)}, {}, {1}), Handling});
  EXPECT_EQ(settle({releasing(), Catching}), (std::vector<bool>{false, false}));
}

TEST(ReturnContracts, AnotherRegisterIsReadWhereSomethingUnseenMayTakeIt) {
  // sqlite3WindowCodeInit: `call sqlite3ExprListDelete; pinsrw $0, mem,
  // %xmm0`.  The insert keeps the lanes the call left, and XMM0 then reaches
  // the function's end unread.  The function returns an integer and nothing
  // catches an exception in it, so nothing takes XMM0 from it.
  const MedVar Rdx = inRegister(x86reg::RDX, 8, 4, 1);
  const auto Leaving = [&] {
    return callerRunning({operation(NdOp::COPY, Rdx, {rax(1)})});
  };
  EXPECT_EQ(settle({releasing(), Leaving()}), (std::vector<bool>{true, false}));
  for (const TypeRef &Type : {NdType::makePtr(nullptr), NdType::makeVoid()}) {
    MedFunc Typed = Leaving();
    Typed.ReturnType = Type;
    EXPECT_EQ(settle({releasing(), Typed}), (std::vector<bool>{true, false}));
  }

  // A merge of the register that nothing reads is no more a reader.
  PhiNode Merge;
  Merge.Output = inRegister(x86reg::RDX, 8, 4, 2);
  Merge.Args = {{0, inRegister(x86reg::RDX, 8, 4, 0)}, {1, Rdx}};
  MedBlock Join = block({returning(MedVar::makeConst(0, 8))}, {0, 1});
  Join.Phis.push_back(Merge);
  const auto Merging = [&] {
    return function(
        CallerEntry,
        {block({seed(), operation(NdOp::COND_BR, {}, {temporary(9)})}, {},
               {1, 2}),
         block({callTo(CalleeEntry, 1), operation(NdOp::COPY, Rdx, {rax(1)})},
               {0}, {2}),
         Join});
  };
  EXPECT_EQ(settle({releasing(), Merging()}), (std::vector<bool>{true, false}));

  // A caller takes the register from a function that returns a record of
  // registers, a floating-point or vector value, or nothing typed yet; an
  // exception handler lifted apart reads what the function held.
  const std::vector<std::pair<const char *, void (*)(MedFunc &)>> Unseen = {
      {"a record of registers",
       [](MedFunc &F) {
         F.MultiReturn = {{x86reg::RAX, 8, false}, {x86reg::RDX, 8, false}};
       }},
      {"an x87 result", [](MedFunc &F) { F.FPReturnViaX87 = true; }},
      {"an explicit x87 result",
       [](MedFunc &F) { F.ExplicitX87ReturnValue = true; }},
      {"a vector result", [](MedFunc &F) { F.ReturnsVectorRegister = true; }},
      {"an untyped result", [](MedFunc &F) { F.ReturnType = nullptr; }},
      {"a float result",
       [](MedFunc &F) { F.ReturnType = NdType::makeFloat(8); }},
      {"a source signature's float",
       [](MedFunc &F) {
         F.SourceTypeHint.emplace();
         F.SourceTypeHint->ReturnType = NdType::makeFloat(8);
       }},
      {"an exceptional edge",
       [](MedFunc &F) {
         F.Blocks.front().ExceptionalSuccs.push_back(
             {-1, 0x4000, ExceptionalEdgeKind::CxxCatch});
       }},
      {"an exceptional entry",
       [](MedFunc &F) {
         F.Blocks.front().ExceptionalPreds.push_back(
             {-1, 0x4000, ExceptionalEdgeKind::CxxCatch});
       }},
  };
  for (const auto &[Name, Change] : Unseen) {
    MedFunc Changed = Leaving(), Joined = Merging();
    Change(Changed);
    Change(Joined);
    EXPECT_FALSE(settle({releasing(), Changed})[0]) << Name;
    EXPECT_FALSE(settle({releasing(), Joined})[0]) << Name << " (merge)";
  }
}

TEST(ReturnContracts, TheUnreadResultOfARoutineDeclaredVoidReadsZero) {
  // `call _set_app_type; setne %al; mov %al, (%rbx)`: the routine is void by
  // its prototype, and the merge reads zero in place of what it left.
  MedFunc Routine = function(
      CalleeEntry,
      {block({seed(), operation(NdOp::COPY, rax(1), {MedVar::makeConst(7, 8)}),
              returning(rax(1))})});
  Routine.Name = "_set_app_type";
  const MedVar Low = sized(temporary(4), 1);
  const MedFunc Caller = callerRunning(
      byteSetOverTheResult() +
      std::vector<MedOp>{
          operation(NdOp::SUBBYTES, Low, {rax(2), MedVar::makeConst(0, 8)}),
          operation(NdOp::STORE, {}, {temporary(7), Low})});
  const PipelineResult Result = settled({Routine, Caller}, BinaryFormat::COFF);
  EXPECT_TRUE(Result.MedFuncs[0].ReturnsNoValue);
  const MedOp &Kept = Result.MedFuncs[1].Blocks[0].Ops[1];
  ASSERT_EQ(Kept.Opcode, NdOp::INT_AND);
  EXPECT_TRUE(Kept.Inputs[0].isConst() && Kept.Inputs[0].ConstVal == 0);
}

TEST(ReturnContracts,
     AParameterInTheReturnRegisterIsAValueForACallerThatReadsIt) {
  // Go passes its first argument in RAX.  runtime.compute0 writes through
  // its second and returns with RAX as it came: no result, unless a caller
  // reads one, as it does of a function that returns its argument.
  MedVar Argument = rax(0);
  Argument.Kind = MedVar::Param;
  MedFunc ByParameter = function(CalleeEntry, {block({returning(Argument)})});
  ByParameter.Params.push_back(Argument);
  MedFunc ByRegister =
      function(CalleeEntry, {block({seed(), returning(rax(0))})});
  ByRegister.Params.push_back(Argument);
  const MedFunc Reader = function(
      CallerEntry, {block({callTo(CalleeEntry, 1),
                           operation(NdOp::STORE, {}, {temporary(7), rax(1)}),
                           returning(MedVar::makeConst(0, 8))})});
  for (const MedFunc &Callee : {ByParameter, ByRegister}) {
    EXPECT_EQ(settle({Callee}), std::vector<bool>{true});
    EXPECT_EQ(settle({Callee, Reader}), (std::vector<bool>{false, false}));
  }

  // One that also writes the register hands the argument back as a value.
  MedFunc Writing =
      function(CalleeEntry,
               {block({seed(), operation(NdOp::COND_BR, {}, {temporary(9)})},
                      {}, {1, 2}),
                block({returning(rax(0))}, {0}),
                block({operation(NdOp::INDIR_CALL, rax(1), {temporary(5)}),
                       returning(rax(1))},
                      {0})});
  Writing.Params.push_back(Argument);
  EXPECT_EQ(settle({Writing}), std::vector<bool>{false});
}

TEST(ReturnContracts, AnotherRegisterAsEnteredShowsNothing) {
  // `mov %rbx, %rax; ret` of an RBX the function never set leaves nothing
  // defined, but the register was written: beside a `return 1` the function
  // keeps its value, where a return that never wrote RAX would not.
  const MedVar Rbx = inRegister(x86reg::RBX, 8, 2, 0);
  const auto Beside = [](MedVar Entered) {
    return function(
        CalleeEntry,
        {block({seed(), operation(NdOp::COPY, Entered, {Entered}),
                operation(NdOp::COND_BR, {}, {temporary(9)})},
               {}, {1, 2}),
         block({returning(Entered)}, {0}),
         block({operation(NdOp::COPY, rax(2), {MedVar::makeConst(1, 8)}),
                returning(rax(2))},
               {0})});
  };
  EXPECT_EQ(settle({Beside(Rbx)}), std::vector<bool>{false});
  EXPECT_EQ(settle({Beside(rax(0))}), std::vector<bool>{true});
}

TEST(ReturnContracts, AReturnOfACopiedArgumentIsAValue) {
  // `mov %rdi, %rax; ret`: copy propagation left the RETURN reading RDI,
  // the argument, where the register holds it.
  MedVar Rdi = rax(0);
  Rdi.Id = 7;
  Rdi.RegOff = x86reg::RDI;
  MedFunc Func =
      function(CalleeEntry,
               {block({operation(NdOp::COPY, Rdi, {Rdi}), returning(Rdi)})});
  MedVar Argument = Rdi;
  Argument.Kind = MedVar::Param;
  Func.Params.push_back(Argument);
  EXPECT_EQ(settle({std::move(Func)}), std::vector<bool>{false});
}

/// RAX written by intrinsic \p Id with \p Inputs after it.
MedOp intrinsic(Intrinsic Id, std::vector<MedVar> Inputs = {}) {
  Inputs.insert(Inputs.begin(),
                MedVar::makeConst(static_cast<uint64_t>(Id), 2));
  return operation(NdOp::INTRINSIC, rax(1), std::move(Inputs));
}

TEST(ReturnContracts, ASystemRegisterReadIsAValue) {
  // `mov rax, cr3; ret` hands back the register it reads, though reading it
  // has side effects.
  EXPECT_EQ(settle({function(CalleeEntry, {block({intrinsic(Intrinsic::ReadCr3),
                                                  returning(rax(1))})})}),
            std::vector<bool>{false});
}

TEST(ReturnContracts, CheckedAArch64PointersRemainReturnValues) {
  const auto Pointer = MedVar::makeConst(0x1234, 8);
  const auto Modifier = MedVar::makeConst(0x5678, 8);
  const struct {
    Intrinsic Id;
    std::vector<MedVar> Inputs;
  } Cases[] = {
      {Intrinsic::Pacia, {Pointer, Modifier}},
      {Intrinsic::Pacib, {Pointer, Modifier}},
      {Intrinsic::Pacda, {Pointer, Modifier}},
      {Intrinsic::Pacdb, {Pointer, Modifier}},
      {Intrinsic::Autia, {Pointer, Modifier}},
      {Intrinsic::Autib, {Pointer, Modifier}},
      {Intrinsic::Autda, {Pointer, Modifier}},
      {Intrinsic::Autdb, {Pointer, Modifier}},
      {Intrinsic::Xpaci, {Pointer}},
      {Intrinsic::Xpacd, {Pointer}},
      {Intrinsic::A64_Pacga, {Pointer, Modifier}},
      {Intrinsic::A64_AuthBranch, {Pointer, MedVar::makeConst(0, 4), Modifier}},
      {Intrinsic::A64_Addpt, {Pointer, Modifier}},
      {Intrinsic::A64_Subpt, {Pointer, Modifier}},
      {Intrinsic::A64_Maddpt, {Pointer, Modifier, Pointer}},
      {Intrinsic::A64_Msubpt, {Pointer, Modifier, Pointer}},
      {Intrinsic::A64_Pacia2, {Pointer, Modifier, Pointer}},
      {Intrinsic::A64_Pacib2, {Pointer, Modifier, Pointer}},
      {Intrinsic::A64_Autia2, {Pointer, Modifier, Pointer}},
      {Intrinsic::A64_Autib2, {Pointer, Modifier, Pointer}},
      {Intrinsic::A64_AuthReturnIA2, {Pointer, Modifier, Pointer}},
      {Intrinsic::A64_AuthReturnIB2, {Pointer, Modifier, Pointer}},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(static_cast<unsigned>(Case.Id));
    for (bool Malformed : {false, true}) {
      MedOp Op = intrinsic(Case.Id, Case.Inputs);
      Op.Output.TheArch = Arch::AArch64;
      Op.Output.RegOff = 0; // AArch64 x0, the integer result register.
      if (Malformed)
        Op.Inputs[1].Size = 4;
      BinaryImage Image;
      Image.Arch = Arch::AArch64;
      Image.Format = BinaryFormat::MachO;
      PipelineResult Result;
      Result.MedFuncs = {
          function(CalleeEntry, {block({Op, returning(Op.Output)})})};
      settleReturnContracts(Image, Result);
      EXPECT_EQ(Result.MedFuncs.front().ReturnsNoValue, Malformed);
    }
  }
}

TEST(ReturnContracts, MipsScalarMemoryEffectsDoNotEraseTheirReturnValues) {
  for (Arch Target :
       {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    const unsigned Width = getTargetRegInfo(Target).FullRegWidth;
    for (auto Id : {Intrinsic::MIPSLwl, Intrinsic::MIPSLwr, Intrinsic::MIPSLdl,
                    Intrinsic::MIPSLdr, Intrinsic::MIPSUnpredictable}) {
      const unsigned Bytes =
          Id == Intrinsic::MIPSLdl || Id == Intrinsic::MIPSLdr ? 8 : 4;
      if (Bytes > Width)
        continue;
      for (bool Malformed : {false, true}) {
        MedOp Op = intrinsic(Id);
        Op.Output.TheArch = Target;
        Op.Output.RegOff = mipsreg::V0;
        Op.Output.Size = Id == Intrinsic::MIPSUnpredictable ? Width : Bytes;
        if (Id != Intrinsic::MIPSUnpredictable) {
          Op.addInput(MedVar::makeConst(0x3001, Width));
          Op.addInput(MedVar::makeConst(0, Bytes));
        }
        if (Malformed)
          Op.Output.Size = 1;
        BinaryImage Image;
        Image.Arch = Target;
        Image.Format = BinaryFormat::ELF;
        PipelineResult Result;
        Result.MedFuncs = {
            function(CalleeEntry, {block({Op, returning(Op.Output)})})};
        settleReturnContracts(Image, Result);
        EXPECT_EQ(Result.MedFuncs.front().ReturnsNoValue, Malformed);
      }
    }
  }
}

TEST(ReturnContracts, ValidatedX86SystemCompletionsRemainReturnValues) {
  const auto Constant = [](uint64_t Value, uint16_t Size) {
    return MedVar::makeConst(Value, Size);
  };
  struct Case {
    Intrinsic Id;
    uint16_t Width;
    std::vector<MedVar> Inputs;
  };
  const Case Cases[] = {
      {Intrinsic::Verr, 1, {Constant(0x23, 2)}},
      {Intrinsic::Verw, 1, {Constant(0x23, 2)}},
      {Intrinsic::Verr, 1, {Constant(0x3000, 8)}},
      {Intrinsic::Verw, 1, {Constant(0x3000, 8)}},
      {Intrinsic::Sldt, 2, {}},
      {Intrinsic::Sldt, 4, {}},
      {Intrinsic::Sldt, 8, {}},
      {Intrinsic::Str, 2, {}},
      {Intrinsic::Str, 4, {}},
      {Intrinsic::Str, 8, {}},
      {Intrinsic::Smsw, 2, {}},
      {Intrinsic::Smsw, 4, {}},
      {Intrinsic::Smsw, 8, {}},
      {Intrinsic::X86ReadPid, 8, {}},
      {Intrinsic::X86ReadFsBase, 8, {}},
      {Intrinsic::X86UserWait,
       1,
       {Constant(0, 4), Constant(1, 4), Constant(2, 4)}},
      {Intrinsic::X86LoadAccessRights, 16, {Constant(0x23, 2)}},
      {Intrinsic::X86ReadMsrList,
       8,
       {Constant(0x3000, 8), Constant(0x4000, 8), Constant(3, 8)}},
      {Intrinsic::X86WriteMsrList,
       8,
       {Constant(0x3000, 8), Constant(0x4000, 8), Constant(3, 8)}},
      {Intrinsic::VmxRead, 16, {Constant(0x4402, 8)}},
      {Intrinsic::VmxWrite, 2, {Constant(0x4402, 8), Constant(0, 8)}},
      {Intrinsic::X86MsrAccess,
       8,
       {Constant(static_cast<uint64_t>(X86MsrAccessKind::RdmsrVexImmediate), 1),
        Constant(0x10, 4)}},
  };
  for (const auto &C : Cases) {
    SCOPED_TRACE(static_cast<unsigned>(C.Id));
    MedOp Read = intrinsic(C.Id, C.Inputs);
    Read.Output = temporary(20);
    Read.Output.Size = C.Width;
    MedOp Project =
        C.Width < 8 ? operation(NdOp::INT_ZEXT, rax(1), {Read.Output})
        : C.Width > 8
            ? operation(NdOp::SUBBYTES, rax(1), {Read.Output, Constant(0, 4)})
            : operation(NdOp::COPY, rax(1), {Read.Output});
    EXPECT_EQ(settle({function(CalleeEntry,
                               {block({Read, Project, returning(rax(1))})})}),
              std::vector<bool>{false});
    // A fabricated extra argument invalidates the owned completion contract.
    Read.addInput(Constant(0, 8));
    EXPECT_EQ(settle({function(CalleeEntry,
                               {block({Read, Project, returning(rax(1))})})}),
              std::vector<bool>{true});
  }
  EXPECT_EQ(
      settle({function(CalleeEntry,
                       {block({intrinsic(Intrinsic::X86WriteMsrNonSerializing,
                                         {Constant(0x10, 4), Constant(1, 4),
                                          Constant(2, 4)}),
                               returning(rax(1))})})}),
      std::vector<bool>{true});
}

TEST(ReturnContracts, ASystemCallResultACallerReadsIsAValue) {
  // An ntdll stub returns its system call's NTSTATUS, which its caller
  // tests.
  MedFunc Stub =
      function(CalleeEntry, {block({intrinsic(Intrinsic::X64WindowsSyscall,
                                              {MedVar::makeConst(0x55, 8)}),
                                    returning(rax(1))})});
  MedFunc Caller = function(
      CallerEntry, {block({callTo(CalleeEntry, 1),
                           operation(NdOp::INT_ADD, rax(2),
                                     {rax(1), MedVar::makeConst(1, 8)}),
                           returning(rax(2))})});
  EXPECT_EQ(settle({std::move(Stub), std::move(Caller)}, BinaryFormat::COFF),
            (std::vector<bool>{false, false}));
}

TEST(ReturnContracts, ASystemCallResultNoCallerReadsIsNoValue) {
  // A system call writes its result whatever the function means: a wrapper
  // nothing reads the result of, as `void f(void) { exit_group(5); }`,
  // returns none.
  EXPECT_EQ(settle({function(CalleeEntry,
                             {block({intrinsic(Intrinsic::X64WindowsSyscall,
                                               {MedVar::makeConst(0x55, 8)}),
                                     returning(rax(1))})})},
                   BinaryFormat::COFF),
            std::vector<bool>{true});
}

TEST(ReturnContracts, AnInstructionRunForItsEffectIsNoValue) {
  // What the lift writes for a breakpoint is no result.
  EXPECT_EQ(settle({function(CalleeEntry, {block({intrinsic(Intrinsic::Int3),
                                                  returning(rax(1))})})}),
            std::vector<bool>{true});
}

/// One path hands back what a call to the function at 0x3000 left, the
/// other a constant.
MedFunc constantBesideACall() {
  return function(
      CalleeEntry,
      {block({seed(), operation(NdOp::COND_BR, {}, {temporary(9)})}, {},
             {1, 2}),
       block({callTo(0x3000, 1), returning(rax(1))}, {0}),
       block({operation(NdOp::COPY, rax(2), {MedVar::makeConst(1, 8)}),
              returning(rax(2))},
             {0})});
}

TEST(ReturnContracts, AConstantIsADeliberateValue) {
  // A `return 1` beside the code after a call that does not return: that
  // path shows nothing of the function, and the constant is its value.
  MedOp Abort = callTo(0x3000, 1);
  Abort.DoesNotReturn = true;
  EXPECT_EQ(
      settle({function(
          CalleeEntry,
          {block({seed(), operation(NdOp::COND_BR, {}, {temporary(9)})}, {},
                 {1, 2}),
           block({Abort, returning(rax(1))}, {0}),
           block({operation(NdOp::COPY, rax(2), {MedVar::makeConst(1, 8)}),
                  returning(rax(2))},
                 {0})})}),
      std::vector<bool>{false});
}

TEST(ReturnContracts, AConstantBesideTheResultOfAFunctionThatReturnsNoneIsNot) {
  // sqlite3ExprAttachSubtrees: `mov $1, %eax` to store a height on one path
  // and `jmp sqlite3ExprDelete` on the other.  A function that means to
  // return a value does not hand back the result of one that returns none:
  // the constant is what the code happened to hold.
  const MedFunc Sink = function(0x3000, {block({seed(), returning(rax(0))})});
  EXPECT_EQ(settle({Sink, constantBesideACall()}),
            (std::vector<bool>{true, true}));

  // The result of a routine declared void shows the same.
  MedFunc Routine = function(
      0x3000,
      {block({seed(), operation(NdOp::COPY, rax(1), {MedVar::makeConst(7, 8)}),
              returning(rax(1))})});
  Routine.Name = "_set_app_type";
  EXPECT_EQ(settle({Routine, constantBesideACall()}, BinaryFormat::COFF),
            (std::vector<bool>{true, true}));
}

TEST(ReturnContracts, AConstantBesideAPathThatNeverWroteTheRegisterIsNoValue) {
  // sqlite3DeleteColumnNames: `xor %eax, %eax; mov %ax, 0x36(%r13)` on one
  // path and an early `ret` that never wrote RAX on another.  Compiled code
  // that means to return a value has no such path.
  EXPECT_EQ(
      settle({branching(CalleeEntry, operation(NdOp::COPY, rax(1),
                                               {MedVar::makeConst(0, 8)}))}),
      std::vector<bool>{true});
}

TEST(ReturnContracts, OnlyAProvenChoiceOfReturnValuesOutweighsABareReturn) {
  const auto C = [](uint64_t Value) { return MedVar::makeConst(Value, 8); };
  const auto Cond = [&](va_t Target, int Predicate) {
    return operation(NdOp::COND_BR, {}, {C(Target), temporary(Predicate)});
  };
  // The guard's false arm leaves RAX as it entered. The other arm chooses
  // between two blocks whose only effect is the integer they return.
  MedFunc F = function(
      CalleeEntry,
      {block({seed(), Cond(0x1020, 9)}, {}, {1, 2}),
       block({returning(rax(0))}, {0}), block({Cond(0x1040, 10)}, {0}, {3, 4}),
       block({returning(C(10))}, {2}),
       block({operation(NdOp::COPY, rax(1), {C(15)}), returning(rax(1))},
             {2})});
  for (auto &B : F.Blocks)
    B.StartAddr = CalleeEntry + 0x10 * B.Id;
  F.Blocks[2].Ops.back().Addr = 0x1024;
  auto IsVoid = [](const MedFunc &F) { return bool(settle({F}).front()); };
  EXPECT_FALSE(IsVoid(F));
  std::reverse(F.Blocks[2].Succs.begin(), F.Blocks[2].Succs.end());
  EXPECT_FALSE(IsVoid(F));

  MedFunc Same = F;
  Same.Blocks[3].Ops.back() = returning(C(15));
  EXPECT_TRUE(IsVoid(Same));
  MedFunc Narrow = F;
  Narrow.ReturnType = NdType::makeInt(1);
  Narrow.Blocks[3].Ops.back() = returning(C(0x10f));
  EXPECT_TRUE(IsVoid(Narrow));
  MedFunc Address = F;
  Address.Blocks[3].Ops.back().Inputs[0].Provenance =
      ConstantAddressProvenance::CodeAddress;
  EXPECT_TRUE(IsVoid(Address));
  MedFunc Store = F;
  auto &Ops = Store.Blocks[4].Ops;
  Ops.insert(Ops.end() - 1, operation(NdOp::STORE, {}, {temporary(7), rax(1)}));
  EXPECT_TRUE(IsVoid(Store));
  MedFunc Fixed = F;
  Fixed.Blocks[2].Ops.back().Inputs[1] = C(0);
  EXPECT_TRUE(IsVoid(Fixed));
  Fixed.Blocks[2].Ops.back().Inputs[1] = MedVar::makeUnspecified(1, Arch::X64);
  EXPECT_TRUE(IsVoid(Fixed));
  MedFunc Unreachable = F;
  Unreachable.Blocks[0].Ops.back() = operation(NdOp::BRANCH, {}, {C(0x1010)});
  Unreachable.Blocks[0].Succs = {1};
  Unreachable.Blocks[2].Preds.clear();
  EXPECT_TRUE(IsVoid(Unreachable));

  auto &Branch = F.Blocks[2].Ops.back();
  Branch = operation(NdOp::INDIR_BR, {}, {temporary(8)});
  Branch.Addr = 0x1024;
  EXPECT_TRUE(IsVoid(F));
  JumpTable &Table = F.JumpTables.emplace_back();
  Table.InsnAddr = Branch.Addr;
  Table.IndexDomainAuthenticated = true;
  Table.Targets = {0x1030, 0x1040};
  EXPECT_FALSE(IsVoid(F));
  ++Table.InsnAddr;
  EXPECT_TRUE(IsVoid(F));
  --Table.InsnAddr;
  Table.Targets = {0x1030, 0x1030};
  EXPECT_TRUE(IsVoid(F));
  Table.Targets.back() = 0x1040;
  Table.MutatedUnsafe = true;
  EXPECT_TRUE(IsVoid(F));
  Table.MutatedUnsafe = false;
  Table.AnalysisOnly = true;
  EXPECT_TRUE(IsVoid(F));
  Table.AnalysisOnly = false;
  Table.IndexDomainAuthenticated = false;
  EXPECT_TRUE(IsVoid(F));
  Table.IndexDomainAuthenticated = true;
  F.UnsafeIndirectBranchAddresses.insert(Branch.Addr);
  EXPECT_TRUE(IsVoid(F));
}

TEST(ReturnContracts, ASharedReturnTakesItsLiteralFromEachBranchArm) {
  const auto C = [](uint64_t Value) { return MedVar::makeConst(Value, 8); };
  const auto Jump = [&](int Id) {
    return operation(NdOp::BRANCH, {}, {C(CalleeEntry + 16 * Id)});
  };
  const auto Choose = [&](int Id, int Condition) {
    return operation(NdOp::COND_BR, {},
                     {C(CalleeEntry + 16 * Id), temporary(Condition)});
  };
  MedFunc F = function(
      CalleeEntry,
      {block({seed(), Choose(2, 9)}, {}, {1, 2}),
       block({returning(rax(0))}, {0}), block({Choose(4, 10)}, {0}, {3, 4}),
       block({operation(NdOp::COPY, rax(1), {C(10)}), Jump(5)}, {2}, {5}),
       block({operation(NdOp::COPY, rax(2), {C(15)})}, {2}, {5}),
       block({returning(rax(3))}, {3, 4})});
  for (auto &B : F.Blocks)
    B.StartAddr = CalleeEntry + 16 * B.Id;
  F.Blocks[5].Phis.push_back({rax(3), {{3, rax(1)}, {4, rax(2)}}, {}});
  auto IsVoid = [](const MedFunc &F) { return bool(settle({F}).front()); };
  EXPECT_FALSE(IsVoid(F));
  std::reverse(F.Blocks[5].Phis.front().Args.begin(),
               F.Blocks[5].Phis.front().Args.end());
  std::reverse(F.Blocks[2].Succs.begin(), F.Blocks[2].Succs.end());
  EXPECT_FALSE(IsVoid(F));

  MedFunc SingleExit = F;
  SingleExit.Blocks[1].Ops = {Jump(5)};
  SingleExit.Blocks[1].Succs = {5};
  SingleExit.Blocks[5].Preds.push_back(1);
  SingleExit.Blocks[5].Phis.front().Args.push_back({1, rax(0)});
  EXPECT_FALSE(IsVoid(SingleExit));

  MedFunc Folded = F;
  Folded.Blocks[5].Phis.front().Args = {{4, C(15)}, {3, C(10)}};
  EXPECT_FALSE(IsVoid(Folded));
  Folded.Blocks[3].Ops.front().Inputs[0] = C(0x10f);
  Folded.Blocks[5].Phis.front().Args.back().second = C(0x10f);
  Folded.ReturnType = NdType::makeInt(1);
  EXPECT_TRUE(IsVoid(Folded));

  MedFunc WrongArm = F;
  WrongArm.Blocks[5].Phis.front().Args = {{3, rax(2)}, {4, rax(1)}};
  EXPECT_TRUE(IsVoid(WrongArm));
  MedFunc Copies = F;
  Copies.Blocks[3].Ops = {operation(NdOp::COPY, temporary(14), {C(10)}),
                          operation(NdOp::COPY, rax(1), {temporary(14)}),
                          operation(NdOp::NOP, {}, {}), Jump(5)};
  EXPECT_FALSE(IsVoid(Copies));
  Copies.Blocks[3].Ops[1].Inputs[0] = temporary(15);
  EXPECT_TRUE(IsVoid(Copies));
  Copies.Blocks[3].Ops[1].Inputs[0] = sized(temporary(14), 4);
  EXPECT_TRUE(IsVoid(Copies));
  MedFunc Narrow = F;
  Narrow.Blocks[5].Phis.front().Args.back().second.Size = 4;
  EXPECT_TRUE(IsVoid(Narrow));
  Narrow = F;
  Narrow.Blocks[5].Phis.front().Output.Size = 4;
  EXPECT_TRUE(IsVoid(Narrow));
  const MedOp Store = operation(NdOp::STORE, {}, {temporary(11), rax(1)});
  MedFunc Effect = F;
  Effect.Blocks[3].Ops.insert(Effect.Blocks[3].Ops.begin() + 1, Store);
  EXPECT_TRUE(IsVoid(Effect));
  Effect = F;
  Effect.Blocks[5].Ops.insert(Effect.Blocks[5].Ops.begin(), Store);
  EXPECT_TRUE(IsVoid(Effect));
  MedFunc Exceptional = F;
  Exceptional.Blocks[5].Phis.front().ExceptionalEntry =
      MedVar::makeUnspecified(8, Arch::X64);
  EXPECT_TRUE(IsVoid(Exceptional));

  // The table must bind the arms that set the results at their common return.
  F.Blocks[2].Ops.back() = operation(NdOp::INDIR_BR, {}, {temporary(9)});
  F.Blocks[2].Ops.back().Addr = F.Blocks[2].StartAddr;
  JumpTable &Table = F.JumpTables.emplace_back();
  Table.InsnAddr = F.Blocks[2].StartAddr;
  Table.IndexDomainAuthenticated = true;
  Table.Targets = {F.Blocks[3].StartAddr, F.Blocks[4].StartAddr};
  EXPECT_FALSE(IsVoid(F));
  Table.Targets.back() = F.Blocks[5].StartAddr;
  EXPECT_TRUE(IsVoid(F));
}

TEST(ReturnContracts, NestedReturnsCompareOnlyBitsThatReachTheCaller) {
  const auto C = [](uint64_t V) { return MedVar::makeConst(V, 8); };
  const auto Choose = [&](int Id) {
    return operation(NdOp::COND_BR, {},
                     {C(CalleeEntry + 16 * Id), temporary(Id)});
  };
  MedFunc F = function(
      CalleeEntry,
      {block({seed(), Choose(2)}, {}, {1, 2}), block({}, {0}, {6}),
       block({Choose(4)}, {0}, {3, 4}),
       block({operation(NdOp::COPY, rax(1), {C(0x10a)})}, {2}, {5}),
       block({operation(NdOp::COPY, rax(2), {C(15)})}, {2}, {5}),
       block({operation(NdOp::INT_AND, rax(4), {rax(3), C(~0xff00ULL)})},
             {3, 4}, {6}),
       block({returning(rax(5))}, {1, 5})});
  for (auto &B : F.Blocks)
    B.StartAddr = CalleeEntry + 16 * B.Id;
  F.Blocks[5].Phis.push_back({rax(3), {{4, rax(2)}, {3, rax(1)}}, {}});
  F.Blocks[6].Phis.push_back({rax(5), {{5, rax(4)}, {1, rax(0)}}, {}});
  auto IsVoid = [&] { return bool(settle({F}).front()); };
  EXPECT_FALSE(IsVoid());
  F.Blocks[4].Ops.front().Inputs[0] = C(10);
  EXPECT_TRUE(IsVoid()); // The differing byte was overwritten.
  for (NdOp Opcode : {NdOp::INT_XOR, NdOp::INT_SUB}) {
    F.Blocks[5].Ops.front() = operation(Opcode, rax(4), {C(7), rax(3)});
    EXPECT_FALSE(IsVoid());
    F.ReturnType = NdType::makeInt(1);
    EXPECT_TRUE(IsVoid());
    F.ReturnType = NdType::makeInt(8);
  }
  F.Blocks[5].Ops.front() = operation(NdOp::INT_XOR, rax(4), {rax(3), rax(3)});
  EXPECT_TRUE(IsVoid()); // Both inputs change together and cancel.
  F.Blocks[5].Ops.front() = operation(NdOp::INT_ADD, rax(4), {rax(3), C(7)});
  EXPECT_FALSE(IsVoid());
  F.ReturnType = NdType::makeInt(1);
  EXPECT_TRUE(IsVoid()); // Addition preserves the equal low bytes.
  F.ReturnType = NdType::makeInt(8);
  F.Blocks[5].Phis.front().Args[0].second.Size = 1;
  EXPECT_TRUE(IsVoid()); // One incoming byte cannot establish the register.

  F.Blocks[5].Phis.front().Args[0].second = rax(2);
  const auto Pair = sized(temporary(12), 2);
  F.Blocks[3].Ops = {
      operation(NdOp::CONCAT, Pair,
                {MedVar::makeConst(0x62, 1), MedVar::makeConst(15, 1)}),
      operation(NdOp::CONCAT, rax(1), {MedVar::makeConst(0, 6), Pair})};
  EXPECT_FALSE(IsVoid()); // This arm supplied all bytes locally.
  F.Blocks[3].Ops.back().Inputs[0] = sized(temporary(13), 6);
  EXPECT_TRUE(IsVoid()); // The upper bytes have no source on this arm.
}

TEST(ReturnContracts, SharedShiftsProveOnlyObservableDifferences) {
  const auto C = [](uint64_t V) { return MedVar::makeConst(V, 8); };
  const auto Choose = [&](int Id) {
    return operation(NdOp::COND_BR, {},
                     {C(CalleeEntry + 16 * Id), temporary(Id)});
  };
  MedFunc F = function(
      CalleeEntry,
      {block({seed(), Choose(2)}, {}, {1, 2}), block({returning(rax(0))}, {0}),
       block({Choose(4)}, {0}, {3, 4}),
       block({operation(NdOp::COPY, rax(1), {C(0)})}, {2}, {5}),
       block({operation(NdOp::COPY, rax(2), {C(0)})}, {2}, {5}),
       block({}, {3, 4})});
  F.Blocks[5].Phis.push_back({rax(3), {{4, rax(2)}, {3, rax(1)}}, {}});
  auto Distinguishes = [&](NdOp Opcode, const MedVar &Count, uint64_t Delta,
                           uint64_t Mask) {
    F.Blocks[4].Ops.front().Inputs[0] = C(Delta);
    F.Blocks[5].Ops = {operation(Opcode, rax(4), {rax(3), Count}),
                       operation(NdOp::INT_AND, rax(5), {rax(4), C(Mask)}),
                       returning(rax(5))};
    return !settle({F}).front();
  };
  constexpr uint64_t Sign = uint64_t{1} << 63;
  EXPECT_TRUE(Distinguishes(NdOp::INT_LEFT, C(60), 8, Sign));
  EXPECT_FALSE(Distinguishes(NdOp::INT_LEFT, C(60), 16, UINT64_MAX));
  EXPECT_TRUE(Distinguishes(NdOp::INT_RIGHT, C(4), 16, 1));
  EXPECT_FALSE(Distinguishes(NdOp::INT_RIGHT, C(4), 15, UINT64_MAX));
  EXPECT_TRUE(Distinguishes(NdOp::INT_ASHR, C(7), Sign, Sign));
  EXPECT_FALSE(Distinguishes(NdOp::INT_ASHR, C(7), Sign >> 1, Sign));
  for (NdOp Opcode : {NdOp::INT_LEFT, NdOp::INT_RIGHT, NdOp::INT_ASHR}) {
    SCOPED_TRACE(static_cast<unsigned>(Opcode));
    EXPECT_TRUE(Distinguishes(Opcode, C(0), Sign, Sign));
    for (uint64_t Count : {uint64_t{64}, uint64_t{65}, UINT64_MAX})
      EXPECT_EQ(Distinguishes(Opcode, C(Count), Sign, 1),
                Opcode == NdOp::INT_ASHR);
    EXPECT_FALSE(Distinguishes(Opcode, temporary(9), Sign, UINT64_MAX));
    EXPECT_FALSE(Distinguishes(
        Opcode,
        MedVar::makeConst(1, 8, ConstantAddressProvenance::AddressFragment),
        Sign, UINT64_MAX));
    EXPECT_FALSE(
        Distinguishes(Opcode, MedVar::makeConst(256, 1), Sign, UINT64_MAX));
  }
  const auto Byte = sized(temporary(12), 1);
  for (NdOp Opcode : {NdOp::INT_ZEXT, NdOp::INT_SEXT}) {
    F.Blocks[5].Ops = {operation(NdOp::SUBBYTES, Byte, {rax(3), C(1)}),
                       operation(Opcode, rax(4), {Byte}),
                       operation(NdOp::INT_AND, rax(5), {rax(4), C(Sign)}),
                       returning(rax(5))};
    F.Blocks[4].Ops.front().Inputs[0] = C(0x8000);
    EXPECT_EQ(!settle({F}).front(), Opcode == NdOp::INT_SEXT);
    F.Blocks[4].Ops.front().Inputs[0] = C(0x4080);
    EXPECT_TRUE(settle({F}).front());
  }
}

TEST(ReturnContracts, VariableShiftsNeedDisjointObservedResults) {
  const auto C = [](uint64_t V) { return MedVar::makeConst(V, 8); };
  const auto Choose = [&](int Id) {
    return operation(NdOp::COND_BR, {},
                     {C(CalleeEntry + 16 * Id), temporary(Id)});
  };
  MedVar Count = temporary(9);
  Count.Kind = MedVar::Param;
  MedFunc F = function(
      CalleeEntry,
      {block({seed(), Choose(2)}, {}, {1, 2}), block({returning(rax(0))}, {0}),
       block({Choose(4)}, {0}, {3, 4}),
       block({operation(NdOp::COPY, rax(1), {C(0)})}, {2}, {5}),
       block({operation(NdOp::COPY, rax(2), {C(1)})}, {2}, {5}),
       block({}, {3, 4})});
  auto &Join = F.Blocks[5];
  Join.Phis.push_back({rax(3), {{4, rax(2)}, {3, rax(1)}}, {}});
  auto Kept = [&] { return !settle({F}).front(); };
  const auto Shift = [&](NdOp Opcode, MedVar N, uint64_t Observed) {
    Join.Ops = {operation(Opcode, rax(4), {rax(3), N}),
                operation(NdOp::INT_AND, rax(5), {rax(4), C(Observed)}),
                returning(rax(5))};
  };
  Shift(NdOp::INT_LEFT, Count, UINT64_MAX);
  EXPECT_FALSE(Kept()); // MedIR permits counts at and above the width.
  Shift(NdOp::INT_LEFT, temporary(10), UINT64_MAX);
  auto Mask = [&](uint64_t M) {
    return operation(NdOp::INT_AND, temporary(10), {Count, C(M)});
  };
  Join.Ops.insert(Join.Ops.begin(), Mask(63));
  EXPECT_TRUE(Kept());
  F.Blocks[4].Ops.front().Inputs[0] = C(2);
  EXPECT_FALSE(Kept()); // A shift by 63 also produces zero on this arm.
  F.Blocks[3].Ops.front().Inputs[0] = C(1);
  Join.Ops.front() = Mask(1);
  EXPECT_FALSE(Kept()); // Both arms can deliver two, using different counts.
  F.Blocks[3].Ops.front().Inputs[0] = C(0);
  Shift(NdOp::INT_RIGHT, temporary(10), 1);
  Join.Ops.insert(Join.Ops.begin(), Mask(2));
  F.Blocks[4].Ops.front().Inputs[0] = C(5);
  EXPECT_TRUE(Kept()); // Only counts zero and two are possible.
  Join.Ops.front() = Mask(3);
  EXPECT_FALSE(Kept());
  Shift(NdOp::INT_ASHR, Count, uint64_t{1} << 63);
  F.Blocks[4].Ops.front().Inputs[0] = C(UINT64_MAX);
  EXPECT_TRUE(Kept()); // Even an oversized shift repeats opposite signs.
  Join.Ops.front().Inputs[1] = MedVar::makeUnspecified(8, Arch::X64);
  EXPECT_FALSE(Kept());

  const auto RCX = [](int V) { return inRegister(x86reg::RCX, 8, 20, V); };
  F.Blocks[3].Ops = {operation(NdOp::COPY, rax(1), {C(1)}),
                     operation(NdOp::COPY, RCX(1), {C(0)})};
  F.Blocks[4].Ops = {operation(NdOp::COPY, rax(2), {C(2)}),
                     operation(NdOp::COPY, RCX(2), {C(1)})};
  Join.Phis.push_back({RCX(3), {{3, RCX(1)}, {4, RCX(2)}}, {}});
  Shift(NdOp::INT_LEFT, RCX(3), UINT64_MAX);
  EXPECT_TRUE(Kept()); // 1 << 0 and 2 << 1.
  const auto NamedInputs = Join.Phis.back().Args;
  Join.Phis.back().Args = {{4, C(1)}, {3, C(0)}};
  EXPECT_TRUE(Kept()); // The folded inputs match the retained count writes.
  Join.Phis.back().Args.front().second = C(2);
  EXPECT_FALSE(Kept()); // Distinct arithmetic results, but the edge writes one.
  Join.Phis.back().Args.front().second = C(1);
  std::swap(Join.Phis.back().Args[0].second, Join.Phis.back().Args[1].second);
  EXPECT_FALSE(Kept());
  Join.Phis.back().Args = NamedInputs;
  std::reverse(Join.Phis.back().Args.begin(), Join.Phis.back().Args.end());
  EXPECT_TRUE(Kept());
  std::swap(Join.Phis.back().Args[0].second, Join.Phis.back().Args[1].second);
  EXPECT_FALSE(Kept()); // The defining register writes are on the wrong edges.
  std::swap(Join.Phis.back().Args[0].second, Join.Phis.back().Args[1].second);
  std::swap(F.Blocks[3].Ops.back().Inputs[0], F.Blocks[4].Ops.back().Inputs[0]);
  EXPECT_FALSE(Kept()); // 1 << 1 and 2 << 0 both equal two.
  std::swap(F.Blocks[3].Ops.back().Inputs[0], F.Blocks[4].Ops.back().Inputs[0]);
  F.Blocks[3].Ops.push_back(
      operation(NdOp::STORE, {}, {temporary(11), RCX(1)}));
  EXPECT_FALSE(Kept()); // The shift cannot hide an observable store.
  F.Blocks[3].Ops.pop_back();
  for (uint64_t Register : {x86reg::RSP, x86reg::RBX}) {
    MedFunc Clobbered = F;
    Clobbered.Blocks[3].Ops.back().Output.RegOff = Register;
    Clobbered.Blocks[4].Ops.back().Output.RegOff = Register;
    Clobbered.Blocks[5].Ops.front().Inputs[1].RegOff = Register;
    auto &Phi = Clobbered.Blocks[5].Phis.back();
    Phi.Output.RegOff = Register;
    for (auto &[Id, In] : Phi.Args)
      In.RegOff = Register;
    EXPECT_TRUE(settle({Clobbered}).front());
  }
  Join.Phis.back().Args.pop_back();
  EXPECT_FALSE(Kept());
}

TEST(ReturnContracts, PairedShiftPathsCloseEveryBackedgeBeforeSelectingAValue) {
  const auto C = [](uint64_t N) { return MedVar::makeConst(N, 8); };
  const auto RCX = [](int V) { return inRegister(x86reg::RCX, 8, 20, V); };
  const auto Choose = [&](int Target, int Cond) {
    return operation(NdOp::COND_BR, {},
                     {C(CalleeEntry + 16 * Target), temporary(Cond)});
  };
  MedFunc F = function(
      CalleeEntry,
      {block({seed(), Choose(4, 11)}, {}, {1, 4}),
       block({Choose(3, 12)}, {0}, {2, 3}),
       block({operation(NdOp::COPY, rax(1), {C(1)}),
              operation(NdOp::COPY, RCX(1), {C(0)})},
             {1}, {5}),
       block({operation(NdOp::COPY, rax(2), {C(4)}),
              operation(NdOp::COPY, RCX(2), {C(1)})},
             {1}, {5}),
       block({returning(rax(0))}, {0}),
       block({Choose(7, 13)}, {2, 3, 6}, {6, 7}),
       block({operation(NdOp::INT_XOR, rax(4), {rax(3), C(2)}),
              operation(NdOp::INT_XOR, RCX(4), {RCX(3), C(2)})},
             {5}, {5}),
       block({operation(NdOp::INT_AND, temporary(14), {RCX(3), C(63)}),
              operation(NdOp::INT_LEFT, rax(5), {rax(3), temporary(14)}),
              returning(rax(5))},
             {5})});
  for (auto &B : F.Blocks)
    B.StartAddr = CalleeEntry + 16 * B.Id;
  auto ZeroFlag = inRegister(x86reg::ZF, 1, 21, 1);
  ZeroFlag.Kind = MedVar::Flag;
  F.Blocks[5].Ops.insert(
      F.Blocks[5].Ops.begin(),
      operation(NdOp::INT_EQUAL, ZeroFlag, {temporary(13), C(0)}));
  auto &Phis = F.Blocks[5].Phis;
  Phis.push_back({rax(3), {{3, rax(2)}, {6, rax(4)}, {2, rax(1)}}, {}});
  Phis.push_back({RCX(3), {{2, RCX(1)}, {3, RCX(2)}, {6, RCX(4)}}, {}});
  const auto IsVoid = [&] { return bool(settle({F}).front()); };
  EXPECT_FALSE(IsVoid()); // {1,12} and {8,48}, including zero iterations.
  {
    MedFunc Narrow = F;
    auto Low = temporary(19);
    Low.Size = 1;
    auto &Ops = Narrow.Blocks.back().Ops;
    Ops.back() = operation(NdOp::SUBBYTES, Low, {rax(5), C(0)});
    Ops.push_back(returning(Low));
    EXPECT_TRUE(settle({Narrow}).front());
    Narrow.ReturnType = NdType::makeInt(1);
    EXPECT_FALSE(settle({Narrow}).front());
  }
  {
    MedFunc Masked = F;
    Masked.Blocks[7].Ops.pop_back();
    Masked.Blocks[7].Succs = {8};
    auto &Exit = Masked.Blocks.emplace_back(block(
        {operation(NdOp::INT_AND, rax(6), {rax(5), C(7)}), returning(rax(6))},
        {7}));
    Exit.Id = 8;
    Exit.StartAddr = CalleeEntry + 16 * Exit.Id;
    EXPECT_FALSE(settle({Masked}).front()); // {1,4} and {0} at the return.
    Exit.Ops.front().Inputs[1] = C(3);
    EXPECT_TRUE(settle({Masked}).front()); // Both can return zero.
    Masked.Blocks[7].Ops.back().Output = temporary(20);
    Exit.Ops.front().Inputs = {rax(3), C(7)};
    EXPECT_TRUE(settle({Masked}).front()); // The shift no longer feeds it.
  }
  // One latch changes only the value and the other only the count. Both
  // must run before the changed count can make the two arms return six.
  {
    MedFunc TwoLatches = F;
    MedBlock First = F.Blocks[6], Second = First;
    First.Id = 8;
    Second.Id = 9;
    First.StartAddr = CalleeEntry + 16 * First.Id;
    Second.StartAddr = CalleeEntry + 16 * Second.Id;
    First.Preds = Second.Preds = {6};
    First.Ops[1] = operation(NdOp::COPY, RCX(4), {RCX(3)});
    Second.Ops[0] = operation(NdOp::COPY, rax(6), {rax(3)});
    Second.Ops[1].Output = RCX(6);
    Second.Ops[1].Inputs[1] = C(64);
    auto &Split = TwoLatches.Blocks[6];
    Split.Ops = {Choose(9, 16)};
    Split.Succs = {8, 9};
    auto &Head = TwoLatches.Blocks[5];
    Head.Preds = {2, 3, 8, 9};
    for (auto &P : Head.Phis) {
      for (auto &Arg : P.Args)
        if (Arg.first == 6)
          Arg.first = 8;
      P.Args.emplace_back(9, P.Output.RegOff == x86reg::RCX ? RCX(6) : rax(6));
    }
    TwoLatches.Blocks.push_back(std::move(First));
    TwoLatches.Blocks.push_back(std::move(Second));
    EXPECT_FALSE(settle({TwoLatches}).front());
    TwoLatches.Blocks.back().Ops[1].Inputs[1] = C(1);
    EXPECT_TRUE(settle({TwoLatches}).front());
  }
  F.Blocks[6].Ops.back().Inputs[1] = C(1);
  EXPECT_TRUE(IsVoid()); // Both arms can return six after one lap.
  F.Blocks[6].Ops.back().Inputs[1] = C(2);
  std::reverse(Phis.begin(), Phis.end());
  EXPECT_FALSE(IsVoid());
  auto Original = Phis.front().Args;
  Phis.front().Args.pop_back();
  EXPECT_TRUE(IsVoid());
  Phis.front().Args = Original;
  F.Blocks[6].Ops.push_back(
      operation(NdOp::STORE, {}, {temporary(15), rax(4)}));
  EXPECT_TRUE(IsVoid());
  F.Blocks[6].Ops.pop_back();
  F.Blocks[6].Ops.back().Opcode = NdOp::INT_ADD;
  EXPECT_TRUE(
      IsVoid()); // An unfinished sequence of distinct states is no proof.

  // Both PHIs read the previous header on a backedge that swaps the values.
  // (0,2) becomes (2,0) and collides with (1,1). Sequential assignments
  // could instead make (2,2) and miss the collision.
  F.Blocks[2].Ops[0].Inputs[0] = C(0);
  F.Blocks[2].Ops[1].Inputs[0] = C(2);
  F.Blocks[3].Ops[0].Inputs[0] = C(1);
  F.Blocks[6].Ops.clear();
  for (auto &P : Phis)
    for (auto &[From, Value] : P.Args)
      if (From == 6)
        Value = P.Output.RegOff == x86reg::RCX ? rax(3) : RCX(3);
  EXPECT_TRUE(IsVoid());
  std::reverse(Phis.begin(), Phis.end());
  EXPECT_TRUE(IsVoid());
}

TEST(ReturnContracts, SharedRegisterViewsNeedTheirOwnCompleteIncomingValue) {
  const auto C = [](uint64_t V) { return MedVar::makeConst(V, 8); };
  const auto Byte = [](unsigned V) { return MedVar::makeConst(V, 1); };
  const auto Choose = [&](int Id) {
    return operation(NdOp::COND_BR, {},
                     {C(CalleeEntry + 16 * Id), temporary(Id)});
  };
  for (const unsigned Offset : {0u, 1u}) {
    SCOPED_TRACE(Offset);
    const auto View = [Offset](int Version) {
      return inRegister(x86reg::RAX + Offset, 1, 11, Version);
    };
    const auto Left = Byte(Offset ? 0x23 : 0x1a);
    const auto Right = Byte(Offset ? 0x34 : 0x1b);
    auto Result = sized(temporary(12), 1);
    MedFunc F = function(
        CalleeEntry,
        {block({seed(), Choose(2)}, {}, {1, 2}),
         block({returning(rax(0))}, {0}), block({Choose(4)}, {0}, {3, 4}),
         block({operation(NdOp::COPY, rax(1), {C(0x231a)}),
                operation(NdOp::COPY, View(1), {Left})},
               {2}, {5}),
         block({operation(NdOp::COPY, rax(2), {C(0x341b)}),
                operation(NdOp::COPY, View(2), {Right})},
               {2}, {5}),
         block({operation(NdOp::INT_XOR, Result, {View(3), Byte(0xa5)}),
                operation(NdOp::INT_ZEXT, rax(4), {Result}), returning(rax(4))},
               {3, 4})});
    auto &Phis = F.Blocks[5].Phis;
    Phis.push_back({rax(3), {{3, rax(1)}, {4, rax(2)}}, {}});
    Phis.push_back({View(3), {{4, View(2)}, {3, View(1)}}, {}});
    auto IsVoid = [&] { return bool(settle({F}).front()); };
    EXPECT_FALSE(IsVoid());
    MedFunc Shifted = F;
    MedVar Count = sized(temporary(13), 1);
    Count.Kind = MedVar::Param;
    auto N = sized(temporary(14), 1);
    auto &ShiftOps = Shifted.Blocks[5].Ops;
    ShiftOps.front() = operation(NdOp::INT_LEFT, Result, {View(3), N});
    ShiftOps.insert(ShiftOps.begin(),
                    operation(NdOp::INT_AND, N, {Count, Byte(1)}));
    EXPECT_FALSE(settle({Shifted}).front());
    Shifted.Blocks[3].Ops.back().Inputs[0] = Byte(0xcc);
    EXPECT_TRUE(settle({Shifted}).front());
    std::reverse(Phis.begin(), Phis.end());
    EXPECT_FALSE(IsVoid());
    auto &Input = Phis.back().Args.front().second;
    Input = sized(rax(1), 1);
    EXPECT_TRUE(IsVoid());
    Input = rax(1);
    F.Blocks[3].Ops.back().Inputs[0] = Byte(0xcc);
    EXPECT_TRUE(
        IsVoid()); // The view and the full register disagree on this edge.
    F.Blocks[3].Ops.back().Inputs[0] = Left;
    std::swap(Phis.front().Args[0].second, Phis.front().Args[1].second);
    EXPECT_TRUE(IsVoid());
  }
}

TEST(ReturnContracts, AFunctionThatNeverReturnsReturnsNoValue) {
  MedOp Abort = callTo(0x3000, 1);
  Abort.DoesNotReturn = true;
  EXPECT_EQ(settle({function(CalleeEntry, {block({seed(), Abort})})}),
            std::vector<bool>{true});
}

TEST(ReturnContracts, MissingOrdinaryReturnsDoNotEraseExceptionalResults) {
  MedOp Throw = callTo(0x3000, 1);
  Throw.DoesNotReturn = true;
  MedFunc Parent = function(CalleeEntry, {block({seed(), Throw})});
  // The catch body and its parent continuation are not ordinary blocks in
  // this snapshot. Its physical no-return call can still dispatch to them.
  Parent.Blocks.front().ExceptionalSuccs.push_back(
      {-1, 0x4000, ExceptionalEdgeKind::CxxCatch});
  EXPECT_EQ(settle({Parent}, BinaryFormat::COFF), std::vector<bool>{false});

  MedFunc Unresolved = function(CalleeEntry, {block({seed()})});
  EXPECT_EQ(settle({Unresolved}), std::vector<bool>{false});

  // An independently proved termination still returns no value.
  Parent.Blocks.front().ExceptionalSuccs.clear();
  EXPECT_EQ(settle({Parent}, BinaryFormat::COFF), std::vector<bool>{true});
}

TEST(ReturnContracts, AWrapperOfADestructorReturnsNoValue) {
  // MSVC's destructor decoration declares no return type.  A function that
  // only hands back what the destructor left in RAX returns nothing; one
  // that computes with it returns a value.
  MedFunc Destructor =
      function(CalleeEntry, {block({seed(), returning(rax(0))})});
  Destructor.Name = "??1SC_DEVICE@@QEAA@XZ";
  const MedFunc Wrapper = function(
      CallerEntry, {block({callTo(CalleeEntry, 1), returning(rax(1))})});
  const MedFunc Computing =
      function(0x3000, {block({callTo(CalleeEntry, 1),
                               operation(NdOp::INT_ADD, rax(2),
                                         {rax(1), MedVar::makeConst(1, 8)}),
                               returning(rax(2))})});
  EXPECT_EQ(settle({std::move(Destructor), Wrapper, Computing}),
            (std::vector<bool>{true, true, false}));
}

TEST(ReturnContracts, ACallToARoutineDeclaredVoidHandsBackNoValue) {
  // A thunk that jumps to _set_app_type hands back what that routine, void
  // by its prototype, left in RAX, whatever its body seems to return. The
  // definition has the same contract and cannot expose that scratch value.
  MedFunc Routine = function(
      CalleeEntry,
      {block({seed(), operation(NdOp::COPY, rax(1), {MedVar::makeConst(7, 8)}),
              returning(rax(1))})});
  Routine.Name = "_set_app_type";
  const MedFunc Thunk = function(
      CallerEntry, {block({callTo(CalleeEntry, 1), returning(rax(1))})});
  EXPECT_EQ(settle({std::move(Routine), Thunk}, BinaryFormat::COFF),
            (std::vector<bool>{true, true}));
}

TEST(ReturnContracts, AStubOfAVoidImportReturnsNoValue) {
  // A PLT stub hands back what __cxa_finalize, void by its prototype, left
  // in RAX: `jmp *slot`, the slot bound to the import.
  constexpr va_t Slot = 0x4018;
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = BinaryFormat::ELF;
  Import Finalize;
  Finalize.Name = "__cxa_finalize";
  Finalize.IATAddr = Slot;
  Image.Imports.push_back(Finalize);
  PipelineResult Result;
  Result.MedFuncs = {function(
      CallerEntry,
      {block({operation(NdOp::LOAD, temporary(4), {MedVar::makeConst(Slot, 8)}),
              operation(NdOp::INDIR_CALL, rax(1), {temporary(4)}),
              returning(rax(1))})})};
  settleReturnContracts(Image, Result);
  EXPECT_TRUE(Result.MedFuncs.front().ReturnsNoValue);
}

TEST(ReturnContracts, ACallToAFunctionDeclaredVoidElsewhereHandsBackNoValue) {
  // A function decompiled alone tail-calls one the debug information, or
  // the prototype of its name, declares void: the C writers print a call
  // that returns nothing, and so the caller returns nothing either.
  const MedFunc Caller = function(
      CallerEntry, {block({callTo(CalleeEntry, 1), returning(rax(1))})});
  DeclaringDebugContext Debug;
  EXPECT_EQ(settle({Caller}, BinaryFormat::COFF, &Debug),
            std::vector<bool>{false});
  Debug.Returns[CalleeEntry] = NdType::makeVoid();
  EXPECT_EQ(settle({Caller}, BinaryFormat::COFF, &Debug),
            std::vector<bool>{true});
  // One declared to return a value hands it back.
  Debug.Returns[CalleeEntry] = NdType::makeInt(4);
  EXPECT_EQ(settle({Caller}, BinaryFormat::COFF, &Debug),
            std::vector<bool>{false});
}

TEST(ReturnContracts, AStubOfAnImportDeclaredVoidReturnsNoValue) {
  // The kernel's headers declare ExFreePool void; the libc tables know it
  // not.
  constexpr va_t Slot = 0x4018;
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = BinaryFormat::COFF;
  Import Free;
  Free.Name = "ExFreePool";
  Free.IATAddr = Slot;
  Image.Imports.push_back(Free);
  DeclaringDebugContext Debug;
  Debug.Prototypes["ExFreePool"] = NdType::makeVoid();
  PipelineResult Result;
  Result.MedFuncs = {function(
      CallerEntry,
      {block({operation(NdOp::LOAD, temporary(4), {MedVar::makeConst(Slot, 8)}),
              operation(NdOp::INDIR_CALL, rax(1), {temporary(4)}),
              returning(rax(1))})})};
  settleReturnContracts(Image, Result, &Debug);
  EXPECT_TRUE(Result.MedFuncs.front().ReturnsNoValue);
}

TEST(ReturnContracts, CopiedImportSlotsShareTheCallABIProvenance) {
  constexpr va_t Slot = 0x4018;
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
        SCOPED_TRACE(unsigned(Architecture));
        SCOPED_TRACE(unsigned(Format));
        SCOPED_TRACE(Mutation);
        BinaryImage Image;
        Image.Arch = Architecture;
        Image.Format = Format;
        const auto &Registers = getTargetRegInfo(Architecture);
        const auto Width = Registers.PointerSize;
        Import Entry;
        Entry.Name = "release_text";
        Entry.IATAddr = Slot;
        Image.Imports.push_back(Entry);
        DeclaringDebugContext Debug;
        Debug.Prototypes[Entry.Name] =
            Mutation == 5 ? NdType::makeInt(Width) : NdType::makeVoid();
        const auto Temporary = [&](int Id) {
          auto Value = temporary(Id);
          Value.TheArch = Architecture;
          Value.Size = Width;
          return Value;
        };
        auto Result = rax(1);
        Result.TheArch = Architecture;
        Result.RegOff = Registers.IntReturnReg;
        Result.Size = Width;
        const auto Address = Temporary(10);
        const auto Loaded = Temporary(11);
        const auto Target = Temporary(12);
        auto Load = operation(NdOp::LOAD, Loaded, {Address});
        if (Mutation == 4)
          Load.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
        std::vector<MedOp> Ops{
            operation(NdOp::COPY, Address,
                      {Mutation == 2 ? Temporary(13)
                                     : MedVar::makeConst(Slot, Width)}),
            Load,
            operation(NdOp::COPY, Target,
                      {Mutation == 1   ? MedVar::makeConst(Slot, Width)
                       : Mutation == 3 ? Target
                                       : Loaded}),
            operation(NdOp::INDIR_CALL, Result, {Target}), returning(Result)};
        PipelineResult Pipeline;
        Pipeline.MedFuncs = {function(CallerEntry, {block(std::move(Ops))})};
        settleReturnContracts(Image, Pipeline, &Debug);
        // An address copied before the load and the loaded callable copied
        // afterward retain the import. A numerically equal callable value,
        // dynamic address, cycle, segment override or nonvoid declaration
        // cannot acquire the void contract of that storage slot.
        EXPECT_EQ(Pipeline.MedFuncs.front().ReturnsNoValue, Mutation == 0);
      }
}

TEST(ReturnContracts, ADeclaredReturnTypeStays) {
  MedFunc Declared =
      function(CalleeEntry, {block({seed(), returning(rax(0))})});
  Declared.SourceTypeHint.emplace();
  EXPECT_EQ(settle({std::move(Declared)}), std::vector<bool>{false});
}

} // namespace

TEST(ReturnContract, PropagatedParametersOwnCompleteIntegerReturnValues) {
  for (Arch A : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    const auto &TRI = getTargetRegInfo(A);
    auto Value = temporary(0);
    Value.TheArch = A;
    Value.Kind = MedVar::Param;
    Value.Size = TRI.PointerSize;
    auto Return = returning(Value);
    EXPECT_EQ(hasPropagatedIntegerReturnValue(Return, A, TRI.PointerSize),
              TRI.ReturnOperandIsValue);
    // A parameter narrower than the result cannot prove its missing bytes.
    EXPECT_FALSE(hasPropagatedIntegerReturnValue(Return, A, Value.Size + 1));
    Return.Inputs[0].Id = -1;
    EXPECT_FALSE(hasPropagatedIntegerReturnValue(Return, A, Value.Size));
    Return = returning(Value);
    Return.Opcode = NdOp::COPY;
    EXPECT_FALSE(hasPropagatedIntegerReturnValue(Return, A, Value.Size));
    Return = returning(Value);
    Return.addInput(Value);
    EXPECT_FALSE(hasPropagatedIntegerReturnValue(Return, A, Value.Size));
    Value.Kind = MedVar::Reg;
    Value.RegOff = x86reg::RDI;
    Return = returning(Value);
    EXPECT_EQ(hasPropagatedIntegerReturnValue(Return, A, Value.Size),
              TRI.ReturnOperandIsValue);
    Return.Inputs[0].RegOff = TRI.IntReturnReg;
    EXPECT_FALSE(hasPropagatedIntegerReturnValue(Return, A, Value.Size));
  }
}

TEST(ReturnContract, IncomingRegisterNeedsAnEntryPathWithNoClobber) {
  MedFunc F = function(CalleeEntry, {block({}, {}, {1}), block({}, {0})});
  F.Blocks[0].StartAddr = F.Entry;
  auto Untouched = [&] {
    return registerUntouchedFromEntry(F, F.Blocks[1], x86reg::RAX, 8,
                                      Arch::X64);
  };
  EXPECT_TRUE(Untouched());
  F.Blocks[0].Ops = {operation(NdOp::SUBBYTES, sized(rax(1), 4),
                               {rax(0), MedVar::makeConst(0, 4)})};
  EXPECT_TRUE(Untouched());
  F.Blocks[0].Ops.front().Inputs[1] = temporary(11);
  EXPECT_FALSE(Untouched());
  F.Blocks[0].Ops = {callTo(0x3000, 1)};
  EXPECT_FALSE(Untouched());
  F.Blocks[0].Ops.clear();
  F.Blocks[0].Succs.clear();
  F.Blocks[1].Preds = F.Blocks[1].Succs = {1};
  EXPECT_FALSE(Untouched());
}

void runIntegerReturnSources(Arch Target, std::vector<uint8_t> Bytes,
                             const char *Oracle, const char *Preamble = "") {
  BinaryImage Image;
  Image.Arch = Target;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = Bitness::Bits64;
  Image.Base = Image.Entry = CalleeEntry;
  Segment Code;
  Code.VA = CalleeEntry;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data = std::move(Bytes);
  Code.Size = Code.FileSz = Code.Data.size();
  auto Sym = Symbol::makeFunc(CalleeEntry, Code.Size);
  Sym.Name = "choose_result";
  Image.Symbols.push_back(std::move(Sym));
  Image.Segments.push_back(std::move(Code));
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {CalleeEntry};
  CEmitterOptions COptions;
  COptions.TheArch = Image.Arch;
  COptions.Format = Image.Format;
  COptions.Image = &Image;
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    Options.LiftMode = LLVM;
    Options.SourceProjection = LLVM;
    llvm::LLVMContext Context;
    auto Result = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    std::string Source(Preamble);
    llvm::raw_string_ostream OS(Source);
    if (LLVM) {
      ASSERT_TRUE(Result.LlvmModule);
      ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
      ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, OS, COptions));
    } else {
      ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, COptions));
    }
    OS.flush();
    source_call_execution_test::compileAndRun(Source + Oracle, {"-O2"});
  }
}

TEST(ReturnContract, BareAArch64ReturnKeepsTheIncomingIntegerArgument) {
  // cbnz w2,bare; cbz w1,ten; mov w0,#15; ret; ten: mov w0,#10; ret; bare: ret
  runIntegerReturnSources(Arch::AArch64,
                          {0xc2, 0x00, 0x00, 0x35, 0x61, 0x00, 0x00,
                           0x34, 0xe0, 0x01, 0x80, 0x52, 0xc0, 0x03,
                           0x5f, 0xd6, 0x40, 0x01, 0x80, 0x52, 0xc0,
                           0x03, 0x5f, 0xd6, 0xc0, 0x03, 0x5f, 0xd6},
                          R"(
int main(void) {
    uint64_t value = UINT64_C(0x1234567887654321);
    return (uint64_t)choose_result(value, 1, 1) != value ||
           choose_result(value, 0, 0) != 10 || choose_result(value, 1, 0) != 15;
}
)");
}

TEST(ReturnContract, SharedIntegerReturnTrapsOnlyOnTheUnwrittenEdge) {
  const char *Oracle = R"(
int main(void) {
    if (choose_result(0, 0) != 10 || choose_result(1, 0) != 15) return 1;
    expect_missing = 1;
    (void)choose_result(1, 1);
    return 2;
}
)";
  const char *Preamble =
      "#include <stdlib.h>\nstatic int expect_missing;\n"
      "#define __builtin_trap() _Exit(expect_missing ? 0 : 3)\n";
  // All three paths reach the final RET. Only the two MOVs define EAX.
  runIntegerReturnSources(Arch::X64, {0x85, 0xf6, 0x75, 0x10, 0x85, 0xff, 0x74,
                                      0x07, 0xb8, 0x0f, 0,    0,    0,    0xeb,
                                      0x05, 0xb8, 0x0a, 0,    0,    0,    0xc3},
                          Oracle, Preamble);
  // On MIPS64 the DADDIU arms share a JR and its empty delay slot. The
  // third arm executes its own JR without writing V0.
  runIntegerReturnSources(Arch::MIPS64EL,
                          {9,  0, 0xa0, 0x14, 0, 0, 0, 0, // bnez a1,bare; nop
                           4,  0, 0x80, 0x10, 0, 0, 0, 0, // beqz a0,ten; nop
                           15, 0, 2,    0x64,             // daddiu v0,zero,15
                           6,  0, 0,    0x10, 0, 0, 0, 0, // b done; nop
                           10, 0, 2,    0x64, // ten: daddiu v0,zero,10
                           3,  0, 0,    0x10, 0, 0, 0, 0,  // b done; nop
                           8,  0, 0xe0, 3,    0, 0, 0, 0,  // bare: jr ra; nop
                           8,  0, 0xe0, 3,    0, 0, 0, 0}, // done: jr ra; nop
                          Oracle, Preamble);
}

TEST(ReturnContract, NestedSharedReturnOverwritesOnlyAH) {
  runIntegerReturnSources(
      Arch::X64,
      {0x48, 0x83, 0xfe, 2,    0x74, 0x20,       // cmp rsi,2; je bare
       0x48, 0x83, 0xfe, 1,    0x74, 0x13,       // cmp rsi,1; je third
       0x48, 0x85, 0xff, 0x74, 7,                // test rdi,rdi; je ten
       0xb8, 15,   0,    0,    0,    0xeb, 0x10, // mov eax,15; jmp inner
       0xb8, 10,   0,    0,    0,    0xeb, 9,    // ten: mov eax,10; jmp inner
       0xb8, 31,   0,    0,    0,    0xeb, 6,    // third: mov eax,31; jmp done
       0xeb, 4,                                  // bare: jmp done
       0xb4, 0x62, 0xeb, 0,    0xc3}, // inner: mov ah,0x62; jmp done; ret
      R"(
int main(void) {
    return choose_result(0, 0) != 0x620a || choose_result(1, 0) != 0x620f ||
           choose_result(0, 1) != 31;
}
)");
}

TEST(ReturnContract, SharedX86ReturnUsesTheChangedRegisterView) {
  // The two MOVs write the whole register. A third path returns it unwritten.
  const std::vector<uint8_t> Arms = {
      0x48, 0x83, 0xfe, 2, 0x74, 0x14, // cmp rsi,2; je bare
      0x48, 0x83, 0xff, 0, 0x74, 7,    // cmp rdi,0; je left
      0xb8, 0x15, 3,    0, 0,    0xeb, 8,   0xb8,
      0x14, 2,    0,    0, 0xeb, 1,    0xc3}; // bare: ret
  auto Code = Arms;
  Code.insert(Code.end(), {0x34, 0xa5, 0xb4, 0x62, 0xc3}); // xor al; mov ah
  runIntegerReturnSources(Arch::X64, std::move(Code), R"(
int main(void) {
    return choose_result(0, 0) != 0x62b1 || choose_result(1, 0) != 0x62b0;
}
)");
  Code = Arms;
  Code.insert(Code.end(), {0x67, 0x8d, 0x40, 0xf9, 0xc3}); // lea eax,[eax-7]
  runIntegerReturnSources(Arch::X64, std::move(Code), R"(
int main(void) {
    return choose_result(0, 0) != 0x20d || choose_result(1, 0) != 0x30e;
}
)");
}

TEST(ReturnContract, SharedX86ReturnKeepsTheShiftedLowNibble) {
  runIntegerReturnSources(Arch::X64,
                          {0x48, 0x83, 0xfe, 2,
                           0x74, 0x14, // cmp rsi,2; je bare
                           0x48, 0x83, 0xff, 0,
                           0x74, 7, // cmp rdi,0; je left
                           0xb8, 0x23, 0x31, 0,
                           0,    0xeb, 8, // mov eax,0x3123; jmp shared
                           0xb8, 1,    0x21, 0,
                           0,    0xeb, 1, // left: mov eax,0x2101; jmp shared
                           0xc3, 0x48, 0xc1, 0xe0,
                           60,   0xc3}, // bare: ret; shared: shl rax,60; ret
                          R"(
int main(void) {
    return choose_result(0, 0) != UINT64_C(0x1000000000000000) ||
           choose_result(1, 0) != UINT64_C(0x3000000000000000);
}
)");
}

TEST(ReturnContract, SharedX86ShiftPairsCLWithItsOwnArm) {
  std::vector<uint8_t> Code = {
      0x48, 0x81, 0xfe, 1,    1,    0,  0, 0x74, 0x22, // mode 257 goes to bare
      0x48, 0x83, 0xff, 0,    0x74, 14,                // cmp rdi,0; je left
      0x48, 0xc7, 0xc0, 2,    0,    0,  0, 0xb9, 1,
      0,    0,    0,    0xeb, 15, // (rax,ecx)=(2,1)
      0x48, 0xc7, 0xc0, 1,    0,    0,  0, 0xb9, 0,
      0,    0,    0,    0xeb, 1,     // (rax,ecx)=(1,0)
      0xc3, 0x48, 0xd3, 0xe0, 0xc3}; // bare: ret; shl rax,cl; ret
  runIntegerReturnSources(Arch::X64, Code, R"(
int main(void) {
    return choose_result(0, 0) != 1 || choose_result(1, 0) != 4;
}
)");
  std::swap(Code[23], Code[37]); // Both paths now return two.
  runIntegerReturnSources(Arch::X64, std::move(Code), R"(
_Static_assert(__builtin_types_compatible_p(__typeof__(choose_result(0, 0)), void),
               "a colliding pair cannot establish a return value");
int main(void) { choose_result(1, 0); choose_result(0, 257); return 0; }
)");
}

TEST(ReturnContract, SharedX86ShiftUsesEveryEarlierCount) {
  std::vector<uint8_t> Code = {
      0x48, 0x81, 0xfe, 1,    1,    0,    0, 0x74, 44, // mode 257 goes to bare
      0x40, 0xf6, 0xc6, 1,    0x75, 7,                 // test sil,1; jne odd
      0xb9, 0,    0,    0,    0,    0xeb, 7,           // ecx=0; jmp select
      0xb9, 2,    0,    0,    0,    0xeb, 0,           // odd: ecx=2; jmp select
      0x48, 0x83, 0xff, 0,    0x74, 9, // select: test first argument
      0x48, 0xc7, 0xc0, 5,    0,    0,    0, 0xeb, 10, // rax=5; jmp shared
      0x48, 0xc7, 0xc0, 0,    0,    0,    0, 0xeb, 1,  // rax=0; jmp shared
      0xc3, 0x48, 0xd3, 0xe8, 0x83, 0xe0, 1, 0xc3};    // shr rax,cl; and eax,1
  runIntegerReturnSources(Arch::X64, Code, R"(
int main(void) {
    for (unsigned mode = 0; mode != 2; ++mode)
        if (choose_result(0, mode) != 0 || choose_result(1, mode) != 1)
            return 1;
    return 0;
}
)");
  Code[23] = 1; // The odd path now shifts the difference out of the low bit.
  runIntegerReturnSources(Arch::X64, std::move(Code), R"(
_Static_assert(__builtin_types_compatible_p(__typeof__(choose_result(0, 0)), void),
               "every earlier count must preserve an observable difference");
int main(void) { choose_result(1, 1); choose_result(0, 257); return 0; }
)");
}

TEST(ReturnContract, SharedX86VariableShiftCrossesTwoJoins) {
  const auto WithCounts = [](uint8_t Right, uint8_t Left, uint8_t Third) {
    return std::vector<uint8_t>{
        // cmp rsi,257; je bare; test esi,1; jnz third
        0x48, 0x81, 0xfe, 1, 1, 0, 0, 0x74, 42, 0xf7, 0xc6, 1, 0, 0, 0, 0x75,
        40,
        // cmp rdi,0; je left
        0x48, 0x83, 0xff, 0, 0x74, 14,
        // mov rax,2; mov ecx,Right; jmp inner
        0x48, 0xc7, 0xc0, 2, 0, 0, 0, 0xb9, Right, 0, 0, 0, 0xeb, 15,
        // left: mov rax,1; mov ecx,Left; jmp inner
        0x48, 0xc7, 0xc0, 1, 0, 0, 0, 0xb9, Left, 0, 0, 0, 0xeb, 1,
        // bare: ret; inner: and ecx,63; jmp done
        0xc3, 0x83, 0xe1, 63, 0xeb, 14,
        // third: mov rax,Third; mov ecx,0; jmp done
        0x48, 0xc7, 0xc0, Third, 0, 0, 0, 0xb9, 0, 0, 0, 0, 0xeb, 0,
        // done: shl rax,cl; ret
        0x48, 0xd3, 0xe0, 0xc3};
  };
  runIntegerReturnSources(Arch::X64, WithCounts(1, 0, 2), R"(
int main(void) {
    return choose_result(0,0)!=1 || choose_result(1,0)!=4 ||
           choose_result(0,1)!=2 || choose_result(1,1)!=2;
}
)");
  runIntegerReturnSources(Arch::X64, WithCounts(0, 1, 4), R"(
int main(void) {
    return choose_result(0,0)!=2 || choose_result(1,0)!=2 ||
           choose_result(0,1)!=4 || choose_result(1,1)!=4;
}
)");
  runIntegerReturnSources(Arch::X64, WithCounts(0, 1, 2), R"(
_Static_assert(__builtin_types_compatible_p(__typeof__(choose_result(0,0)),void),
               "the inner and outer paths all produce two");
int main(void) { choose_result(1,0); choose_result(0,257); return 0; }
)");
}

TEST(ReturnContract, SharedX86ReturnObservesBothShiftDirectionsAfterTheJoin) {
  const auto WithMask = [](uint8_t HighByte) {
    return std::vector<uint8_t>{
        // cmp rsi,257; je bare; test esi,8; jnz third
        0x48, 0x81, 0xfe, 1, 1, 0, 0, 0x74, 42, 0xf7, 0xc6, 8, 0, 0, 0, 0x75,
        35,
        // cmp rdi,0; je left
        0x48, 0x83, 0xff, 0, 0x74, 14,
        // (rax,ecx) = (129,1); jump to the direction test
        0x48, 0xc7, 0xc0, 129, 0, 0, 0, 0xb9, 1, 0, 0, 0, 0xeb, 24,
        // left: (rax,ecx) = (2,0); jump to the direction test
        0x48, 0xc7, 0xc0, 2, 0, 0, 0, 0xb9, 0, 0, 0, 0, 0xeb, 10,
        // bare: ret; third: mov rax,2; jmp observe
        0xc3, 0x48, 0xc7, 0xc0, 2, 0, 0, 0, 0xeb, 18,
        // test esi,1; jnz right; shl rax,cl; jmp observe
        0xf7, 0xc6, 1, 0, 0, 0, 0x75, 5, 0x48, 0xd3, 0xe0, 0xeb, 5,
        // right: shr rax,cl; jmp observe
        0x48, 0xd3, 0xe8, 0xeb, 0,
        // observe: and eax,mask; ret
        0x25, 0xff, HighByte, 0, 0, 0xc3};
  };
  runIntegerReturnSources(Arch::X64, WithMask(1), R"(
int main(void) {
    return choose_result(0,0)!=2 || choose_result(0,1)!=2 ||
           choose_result(1,0)!=258 || choose_result(1,1)!=64 ||
           choose_result(1,8)!=2;
}
)");
  runIntegerReturnSources(Arch::X64, WithMask(0), R"(
_Static_assert(__builtin_types_compatible_p(__typeof__(choose_result(0,0)),void),
               "the left shift can also return two after the byte mask");
int main(void) { choose_result(1,0); choose_result(0,257); return 0; }
)");
}

TEST(ReturnContract, SharedAArch64WordShiftKeepsEachLatchAndTheIncomingX0) {
  for (const bool Collision : {false, true}) {
    SCOPED_TRACE(Collision);
    const uint32_t Words[] = {
        // cmp x1,257; b.eq bare; and x4,x1,3; cbz x0,left
        0xf104043f, 0x54000120, 0x92400424, 0xb4000080,
        // (w0,w3) = (4,1) or (1,0); branch to head; bare: ret
        0x52800080, 0x52800023, 0x14000005, 0x52800020, 0x52800003, 0x14000002,
        0xd65f03c0,
        // head: cbz x4,done; tbnz x1,2,other; eor w0,w0,2; eor w3,w3,mask
        0xb4000124, 0x371000a1, 0x521f0000,
        Collision ? 0x52000063u : 0x521f0063u,
        // sub x4,x4,1; b head; other: eor w3,w3,32; sub x4,x4,1; b head
        0xd1000484, 0x17fffffb, 0x521b0063, 0xd1000484, 0x17fffff8,
        // done: lslv w0,w0,w3; ret
        0x1ac32000, 0xd65f03c0};
    std::vector<uint8_t> Code;
    for (uint32_t Word : Words)
      for (unsigned Shift = 0; Shift < 32; Shift += 8)
        Code.push_back(static_cast<uint8_t>(Word >> Shift));
    runIntegerReturnSources(Arch::AArch64, std::move(Code), R"(
int main(void) {
    for (unsigned n=0;n!=8;++n) {
        int changed = !(n & 4) && (n & 1);
        if (choose_result(0,n)!=(changed ? (COLLISION ? 6 : 12) : 1) ||
            choose_result(1,n)!=(changed ? (COLLISION ? 6 : 48) : 8)) return 1;
    }
    uint64_t incoming = UINT64_C(0x1234567887654321);
    return (uint64_t)choose_result(incoming,257)!=incoming;
}
)",
                            Collision ? "#define COLLISION 1\n"
                                      : "#define COLLISION 0\n");
  }
}

TEST(ReturnContract, SharedMIPSShiftCountChangesInTheBackedgeSlot) {
  for (const bool Little : {false, true}) {
    SCOPED_TRACE(Little);
    const auto WithCountUpdate = [&](uint16_t Mask) {
      const uint32_t Words[] = {
          // daddiu t0,zero,257; beq a1,t0,bare; nop; andi t2,a1,3
          0x640c0101, 0x10ac000c, 0, 0x30ae0003,
          // beqz a0,left; nop; v0=4; t1=1; b head; nop
          0x10800005, 0, 0x64020004, 0x640d0001, 0x10000007, 0,
          // left: v0=1; t1=0; b head; nop; bare: jr ra; nop
          0x64020001, 0x640d0000, 0x10000003, 0, 0x03e00008, 0,
          // head: beqz t2,done; nop; xori v0,v0,2; daddiu t2,t2,-1
          0x11c00005, 0, 0x38420002, 0x65ceffff,
          // b head; xori t1,t1,Mask; done: jr ra; dsllv v0,v0,t1
          0x1000fffb, 0x39ad0000u | Mask, 0x03e00008, 0x01a21014};
      std::vector<uint8_t> Code;
      for (uint32_t Word : Words)
        for (unsigned I = 0; I < 4; ++I)
          Code.push_back(
              static_cast<uint8_t>(Word >> (8 * (Little ? I : 3 - I))));
      return Code;
    };
    const Arch Target = Little ? Arch::MIPS64EL : Arch::MIPS64;
    runIntegerReturnSources(Target, WithCountUpdate(2), R"(
int main(void) {
    for (unsigned n=0;n!=4;++n) {
        if (choose_result(0,n)!=(n%2?12:1) ||
            choose_result(1,n)!=(n%2?48:8)) return 1;
    }
    return 0;
}
)");
    runIntegerReturnSources(Target, WithCountUpdate(1), R"(
_Static_assert(__builtin_types_compatible_p(__typeof__(choose_result(0,0)),void),
               "both entry paths can return six after one trip");
int main(void) { choose_result(0,1); choose_result(1,3); return 0; }
)");
  }
}

TEST(ReturnContract, SharedMIPSRegisterShiftObservesTheDelaySlotMask) {
  std::vector<uint8_t> Code = {
      1,    1,    12,   0x64, 11, 0, 0xac, 0x10,
      0,    0,    0,    0,                       // mode 257 goes to bare
      0x25, 0x68, 0xa0, 0,    2,  0, 0xad, 0x31, // count = a1 & 2
      4,    0,    0x80, 0x10, 0,  0, 0,    0,    // beqz a0,left; nop
      5,    0,    2,    0x64, 6,  0, 0,    0x10,
      0,    0,    0,    0, // v0=5; b shared
      0,    0,    2,    0x64, 3,  0, 0,    0x10,
      0,    0,    0,    0,                    // left: v0=0; b shared
      8,    0,    0xe0, 3,    0,  0, 0,    0, // bare: jr ra; nop
      0x16, 0x10, 0xa2, 1,    8,  0, 0xe0, 3,
      1,    0,    0x42, 0x30}; // dsrlv; jr ra; andi v0,1
  runIntegerReturnSources(Arch::MIPS64EL, Code, R"(
int main(void) {
    return choose_result(0, 2) != 0 || choose_result(1, 0) != 1 ||
           choose_result(1, 66) != 1;
}
)");
  Code[28] = 1; // A count of two erases the only distinguishing bit.
  runIntegerReturnSources(Arch::MIPS64EL, std::move(Code), R"(
_Static_assert(__builtin_types_compatible_p(__typeof__(choose_result(0, 0)), void),
               "one surviving count is insufficient");
int main(void) { choose_result(1, 2); choose_result(0, 257); return 0; }
)");
}

TEST(ReturnContract, SharedMIPSWordShiftChecksTheOperandOnEachArm) {
  std::vector<uint8_t> Code = {
      1, 1, 12,   0x64, 11, 0,    0xac, 0x10,
      0, 0, 0,    0,                          // mode 257 goes to bare
      5, 0, 0x80, 0x10, 0,  0,    0,    0,    // beqz a0,left; nop
      2, 0, 2,    0x64, 0,  0,    13,   0x64, // (v0,t1)=(2,0)
      7, 0, 0,    0x10, 0,  0,    0,    0,    // b shared; nop
      1, 0, 2,    0x64, 1,  0,    13,   0x64, // left: (v0,t1)=(1,1)
      3, 0, 0,    0x10, 0,  0,    0,    0,    // b shared; nop
      8, 0, 0xe0, 3,    0,  0,    0,    0,    // bare: jr ra; nop
      8, 0, 0xe0, 3,    6,  0x10, 0xa2, 1};   // shared: jr ra; srlv
  runIntegerReturnSources(Arch::MIPS64EL, Code, R"(
int main(void) {
    return choose_result(0, 0) != 0 || choose_result(1, 0) != 2;
}
)");
  std::swap(Code[24], Code[40]); // 1 >> 0 and 2 >> 1 coincide.
  runIntegerReturnSources(Arch::MIPS64EL, std::move(Code), R"(
_Static_assert(__builtin_types_compatible_p(__typeof__(choose_result(0, 0)), void),
               "both guarded results are one");
int main(void) { choose_result(0, 0); choose_result(1, 257); return 0; }
)");
}

TEST(ReturnContract, SharedMIPSSignExtensionRequiresAWordOnEachArm) {
  std::vector<uint8_t> Code = {
      2,    0,    12,   0x64, 9,    0,    0xac, 0x10,
      0,    0,    0,    0,                         // mode 2 goes to bare
      4,    0,    0x80, 0x10, 0,    0,    0,    0, // beqz a0,left; nop
      0xf9, 3,    2,    0x64, 6,    0,    0,    0x10,
      0,    0,    0,    0, // v0 = 0x3f9; b shared
      1,    4,    2,    0x64, 3,    0,    0,    0x10,
      0,    0,    0,    0,                            // left: v0 = 0x401
      8,    0,    0xe0, 3,    0,    0,    0,    0,    // bare: jr ra; nop
      0xfa, 0x10, 2,    0,    0x20, 0x14, 2,    0x7c, // dsrl v0,3; seb v0,v0
      8,    0,    0xe0, 3,    0xfe, 0x17, 2,    0};   // jr ra; dsrl32 v0,31
  runIntegerReturnSources(Arch::MIPS64EL, Code, R"(
int main(void) {
    return choose_result(0, 0) != 1 || choose_result(1, 0) != 0;
}
)");
  // Shifting -8 by 3 or 32 still leaves a value outside SEB's word domain.
  Code[20] = 0xf8;
  Code[21] = 0xff;
  auto &ShiftEncoding = Code[Code.size() - 16];
  for (uint8_t Shift : {0xfa, 0x3e}) {
    SCOPED_TRACE(static_cast<unsigned>(Shift));
    ShiftEncoding = Shift;
    runIntegerReturnSources(
        Arch::MIPS64EL, Code, R"(
_Static_assert(__builtin_types_compatible_p(__typeof__(choose_result(0, 0)), void),
               "one valid result cannot prove a choice");
int main(void) {
    choose_result(0, 0);
    expect_word_trap = 1;
    choose_result(1, 0);
    return 2;
}
)",
        "#include <stdlib.h>\nstatic int expect_word_trap;\n"
        "#define __builtin_trap() _Exit(expect_word_trap ? 0 : 3)\n");
  }
  ShiftEncoding = 0x7e; // dsrl32 v0,v0,1 leaves at most 31 bits.
  runIntegerReturnSources(Arch::MIPS64EL, std::move(Code), R"(
int main(void) {
    return choose_result(0, 0) != 0 || choose_result(1, 0) != 1;
}
)");
}

TEST(ReturnContract, SharedMIPSReturnAppliesTheDelaySlotXor) {
  const std::vector<uint8_t> Arms = {
      2,    0,    12,   0x64, 9, 0, 0xac, 0x10,
      0,    0,    0,    0,                   // mode 2 branches to bare
      4,    0,    0x80, 0x10, 0, 0, 0,    0, // beqz a0,left
      0x15, 0x31, 2,    0x64, 6, 0, 0,    0x10,
      0,    0,    0,    0, // v0 = 0x3115; b shared
      0x14, 0x22, 2,    0x64, 3, 0, 0,    0x10,
      0,    0,    0,    0,                    // v0 = 0x2214; b shared
      8,    0,    0xe0, 3,    0, 0, 0,    0}; // bare: jr ra; nop
  auto Code = Arms;
  Code.insert(Code.end(), {8, 0, 0xe0, 3, 0xa5, 0, 0x42, 0x38}); // jr ra; xori
  runIntegerReturnSources(Arch::MIPS64EL, std::move(Code), R"(
int main(void) {
    return choose_result(0, 0) != 0x22b1 || choose_result(1, 0) != 0x31b0;
}
)");
  Code = Arms;
  Code.insert(Code.end(), {0xa5, 0, 0x42, 0x38, 8, 0, 0xe0, 3, 7, 0x7b, 2,
                           0x7c}); // xori; jr ra; dins v0,zero,12,4
  runIntegerReturnSources(Arch::MIPS64EL, std::move(Code), R"(
int main(void) {
    return choose_result(0, 0) != 0x2b1 || choose_result(1, 0) != 0x1b0;
}
)");
}

TEST(ReturnContract, SharedMIPSBranchLikelyExecutesOnlyTheTakenSlot) {
  for (bool BranchOnZero : {true, false}) {
    SCOPED_TRACE(BranchOnZero);
    const uint32_t Words[] = {
        0x640c0002,
        0x10ac000f,
        0, // mode 2 branches to the bare path
        0x640c0001,
        0x10ac0009,
        0, // mode 1 branches to the third constant
        0x10800004,
        0, // beqz a0,ten
        0x6402000f,
        0x10000009,
        0, // v0 = 15; b inner
        0x6402000a,
        0x10000006,
        0, // ten: v0 = 10; b inner
        0x6402001f,
        0x10000007,
        0, // v0 = 31; b done
        0x10000005,
        0,                                        // bare: b done
        BranchOnZero ? 0x50800003u : 0x54800003u, // inner: beqzl/bnezl a0,done
        0x64420007,                               // taken slot: daddiu v0,v0,7
        0x10000001,
        0,
        0x03e00008,
        0}; // b done; done: jr ra
    std::vector<uint8_t> Code;
    for (uint32_t Word : Words)
      for (unsigned Shift = 0; Shift < 32; Shift += 8)
        Code.push_back(static_cast<uint8_t>(Word >> Shift));
    runIntegerReturnSources(Arch::MIPS64EL, std::move(Code), R"(
int main(void) {
    return choose_result(0, 0) != (ON_ZERO ? 17 : 10) ||
           choose_result(1, 0) != (ON_ZERO ? 15 : 22) ||
           choose_result(0, 1) != 31;
}
)",
                            BranchOnZero ? "#define ON_ZERO 1\n"
                                         : "#define ON_ZERO 0\n");
  }
}

TEST(ReturnContract, MissingPointerResultUsesTheSourceABIPointerWidth) {
  for (Arch Target : {Arch::MIPS64N32EL, Arch::MIPS64EL}) {
    const auto &TRI = getTargetRegInfo(Target);
    MedVar Link = inRegister(TRI.LinkRegister, TRI.FullRegWidth, 31, 0);
    Link.TheArch = Target;
    MedFunc F = function(CalleeEntry, {block({returning(Link)})});
    F.ReturnType = NdType::makePtr();
    F.Blocks.front().StartAddr = F.Entry;
    for (bool Source : {false, true}) {
      SCOPED_TRACE(testing::Message() << unsigned(Target) << " " << Source);
      llvm::LLVMContext Context;
      MedLLVMEmitter Emitter;
      Emitter.setPreserveUnknownSourceValues(Source);
      auto Module = Emitter.emit({F}, Context, "missing_pointer", Target, {},
                                 nullptr, BinaryFormat::ELF);
      ASSERT_TRUE(Module);
      ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
      const auto *Function = Module->getFunction(F.Name);
      ASSERT_TRUE(Function);
      const auto *Ret =
          llvm::dyn_cast<llvm::ReturnInst>(Function->back().getTerminator());
      ASSERT_TRUE(Ret);
      const auto *Pointer = Ret->getReturnValue();
      ASSERT_TRUE(Pointer && Pointer->getType()->isPointerTy());
      if (Source) {
        const auto *Cast = llvm::dyn_cast<llvm::IntToPtrInst>(Pointer);
        ASSERT_TRUE(Cast);
        EXPECT_TRUE(llvm_source_unknown::isValue(Cast->getOperand(0)));
        EXPECT_TRUE(Cast->getSrcTy()->isIntegerTy(TRI.PointerSize * 8));
      } else {
        EXPECT_TRUE(llvm::isa<llvm::ConstantPointerNull>(Pointer));
      }
    }
  }
}

TEST(ReturnContract, DeclaredReturnKeepsFullValueAfterNarrowRead) {
  for (bool Pointer : {false, true})
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(testing::Message() << Pointer << " " << NoOpt);
      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Format = BinaryFormat::MachO;
      Image.Bits = Bitness::Bits64;
      Image.Base = Image.Entry = CalleeEntry;
      Segment Code;
      Code.VA = CalleeEntry;
      Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      // mov rax,rdi; mov [rsi],ax; ret. Reading AX leaves all of RAX intact.
      Code.Data = {0x48, 0x89, 0xf8, 0x66, 0x89, 0x06, 0xc3};
      Code.Size = Code.FileSz = Code.Data.size();
      auto FunctionSymbol = Symbol::makeFunc(CalleeEntry, Code.Size);
      FunctionSymbol.Name = "narrow_read_return";
      Image.Symbols.push_back(std::move(FunctionSymbol));
      Image.Segments.push_back(std::move(Code));
      SourceFunctionTypeHint Hint;
      Hint.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
      Hint.ReturnType = Pointer ? NdType::makePtr() : NdType::makeInt(8, false);
      Hint.Parameters = {{"value", Hint.ReturnType},
                         {"low", NdType::makePtr(NdType::makeInt(2, false))}};
      std::string Error;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Arch::X64, Error)) << Error;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.NoOpt = NoOpt;
      Options.OnlyFunctionEntries = {CalleeEntry};
      Options.SourceTypeHints = {{CalleeEntry, Hint}};
      llvm::LLVMContext Context;
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      std::string Dump;
      llvm::raw_string_ostream OS(Dump);
      Pipeline::dumpMedIR(Result.MedFuncs, OS);
      Pipeline::dumpHighIR(Result.HighFuncs, OS);
      SCOPED_TRACE(Dump);
      ASSERT_EQ(Result.HighFuncs.size(), 1u);
      const auto &Body = Result.HighFuncs.front().Body;
      ASSERT_FALSE(Body.empty());
      ASSERT_EQ(Body.back().Kind, StmtKind::Return);
      ASSERT_TRUE(Body.back().RetVal);
      ASSERT_TRUE(Body.back().RetVal->Type);
      EXPECT_EQ(Body.back().RetVal->Type->Size, 8u);
      CEmitterOptions COptions;
      COptions.TheArch = Image.Arch;
      COptions.Format = Image.Format;
      COptions.Image = &Image;
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(LLVM);
        std::string Source;
        llvm::raw_string_ostream SourceOS(Source);
        if (LLVM) {
          auto Module =
              MedLLVMEmitter().emit(Result.MedFuncs, Context, "narrow_read",
                                    Image.Arch, {}, &Image, Image.Format);
          ASSERT_TRUE(Module);
          ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
          ASSERT_TRUE(LLVMCEmitter().emit(*Module, SourceOS, COptions));
        } else {
          ASSERT_TRUE(
              HighCEmitter().emit(Result.HighFuncs, SourceOS, COptions));
        }
        SourceOS.flush();
        Source += R"(
#if defined(__x86_64__) && !defined(_WIN32)
__attribute__((naked, noinline))
static uint64_t original(uint64_t value, uint16_t *low) {
  __asm__ volatile(".byte 0x48,0x89,0xf8,0x66,0x89,0x06,0xc3");
}
#endif
int main(void) {
  uint64_t value = UINT64_C(0x800012345678ffff);
  for (unsigned i = 0; i != 65536; ++i) {
    uint64_t expected = UINT64_C(0xabcdef0123456789), actual = expected;
    uint64_t result = value;
#if defined(__x86_64__) && !defined(_WIN32)
    result = original(value, (uint16_t *)&expected);
#else
    expected = (expected & ~UINT64_C(0xffff)) | (value & 0xffff);
#endif
    uint64_t got = (uintptr_t)narrow_read_return()";
        Source += Pointer ? "(void *)(uintptr_t)value" : "value";
        Source += R"(, (uint16_t *)&actual);
    if (got != result || actual != expected) return 1;
    value ^= value << 13; value ^= value >> 7; value ^= value << 17;
  }
  return 0;
}
)";
        for (const char *Optimization : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(
              Source, {Optimization, "-Werror=int-conversion"});
      }
    }
}

namespace {

struct ReturnWidthShape {
  const char *Name;
  std::vector<uint8_t> Code;
  /// What `Name(a, b)` returns, or null where that is an address of the
  /// image, which the compiled HighC places elsewhere.
  const char *Reference;
};

/// Lift each of \p Shapes, the code of an x86-64 function of two arguments,
/// with and without optimization.  Its result is to have \p Bytes, and its
/// HighC is to return what the reference does, both read as \p Read.
void expectReturnWidth(llvm::ArrayRef<ReturnWidthShape> Shapes, unsigned Bytes,
                       const char *Read) {
  for (const ReturnWidthShape &S : Shapes)
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(testing::Message() << S.Name << " " << NoOpt);
      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Format = BinaryFormat::ELF;
      Image.Bits = Bitness::Bits64;
      Image.Base = Image.Entry = CalleeEntry;
      Segment Code;
      Code.VA = CalleeEntry;
      Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      Code.Data = S.Code;
      Code.Size = Code.FileSz = Code.Data.size();
      auto FunctionSymbol = Symbol::makeFunc(CalleeEntry, Code.Size);
      FunctionSymbol.Name = S.Name;
      Image.Symbols.push_back(std::move(FunctionSymbol));
      Image.Segments.push_back(std::move(Code));
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.NoOpt = NoOpt;
      Options.OnlyFunctionEntries = {CalleeEntry};
      llvm::LLVMContext Context;
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      std::string Dump;
      llvm::raw_string_ostream OS(Dump);
      Pipeline::dumpMedIR(Result.MedFuncs, OS);
      Pipeline::dumpHighIR(Result.HighFuncs, OS);
      SCOPED_TRACE(Dump);
      ASSERT_EQ(Result.MedFuncs.size(), 1u);
      ASSERT_TRUE(Result.MedFuncs.front().ReturnType);
      EXPECT_EQ(Result.MedFuncs.front().ReturnType->Size, Bytes);
      ASSERT_EQ(Result.HighFuncs.size(), 1u);
      ASSERT_TRUE(Result.HighFuncs.front().ReturnType);
      EXPECT_EQ(Result.HighFuncs.front().ReturnType->Size, Bytes);
      if (!S.Reference)
        continue;
      CEmitterOptions COptions;
      COptions.TheArch = Image.Arch;
      COptions.Format = Image.Format;
      COptions.Image = &Image;
      std::string Source;
      llvm::raw_string_ostream SourceOS(Source);
      ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, SourceOS, COptions));
      SourceOS.flush();
      Source += std::string(R"(
int main(void) {
  uint64_t b = UINT64_C(0x800012345678ffff);
  for (unsigned i = 0; i != 4096; ++i) {
    const uint64_t a = i & 1 ? b >> 7 : 0;
    if (()") + Read +
                ")" + S.Name + "(a, b) != (" + Read + ")(" + S.Reference +
                R"()) return 1;
    b ^= b << 13; b ^= b >> 7; b ^= b << 17;
  }
  return 0;
}
)";
      for (const char *Optimization : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            Source, {Optimization, "-Werror=int-conversion"});
    }
}

} // namespace

TEST(ReturnContract, TheWidestReturnPathTypesAnInferredResult) {
  // One path returns a zero-extended 32-bit value and another all of RAX.
  // Whichever block comes first, the function returns the register: typing
  // it by the 32-bit path truncates the other, in the function and again in
  // every caller that extends the declared result.
  expectReturnWidth(
      {
          // test rdi,rdi; je +3; mov eax,esi; ret; mov rax,rsi; ret
          {"narrow_then_wide",
           {0x48, 0x85, 0xff, 0x74, 0x03, 0x89, 0xf0, 0xc3, 0x48, 0x89, 0xf0,
            0xc3},
           "a ? (uint64_t)(uint32_t)b : b"},
          // test rdi,rdi; je +4; mov rax,rsi; ret; mov eax,esi; ret
          {"wide_then_narrow",
           {0x48, 0x85, 0xff, 0x74, 0x04, 0x48, 0x89, 0xf0, 0xc3, 0x89, 0xf0,
            0xc3},
           "a ? b : (uint64_t)(uint32_t)b"},
          // lea rax,[rdi+rsi*2]; test rdi,rdi; je +3; mov eax,esi; ret; nop;
          // ret: the second return takes what the entry block left in RAX.
          {"narrow_then_earlier",
           {0x48, 0x8d, 0x04, 0x77, 0x48, 0x85, 0xff, 0x74, 0x03, 0x89, 0xf0,
            0xc3, 0x90, 0xc3},
           "a ? (uint64_t)(uint32_t)b : a + b * 2"},
      },
      8, "uint64_t");
}

TEST(ReturnContract, AnAddressOneReturnPathLeavesIsAPointer) {
  // The address is a small number in this image, and one a 32-bit move can
  // write.  It is a pointer all the same: relinked, it need not fit 32 bits.
  expectReturnWidth(
      {
          // test rdi,rdi; jne +3; mov eax,esi; ret; lea rax,[rip+1]; ret;
          // then the quadword the address is of
          {"narrow_then_address",
           {0x48, 0x85, 0xff, 0x75, 0x03, 0x89, 0xf0, 0xc3,
            0x48, 0x8d, 0x05, 0x01, 0x00, 0x00, 0x00, 0xc3,
            0,    0,    0,    0,    0,    0,    0,    0},
           nullptr},
      },
      8, "uint64_t");
}

TEST(ReturnContract, ANarrowWriteThatReachesAReturnKeepsTheResultNarrow) {
  // A return without a write of its own takes the register as the blocks
  // before it left it.  That alone widens nothing: the write that reaches it
  // is a zero-extended 32-bit one here, directly, under a byte `sete` merges
  // in, and under a 16-bit rotate.  These results keep the width they had.
  expectReturnWidth(
      {
          // mov eax,esi; test rdi,rdi; je +1; ret; mov eax,edi; ret
          {"earlier_narrow",
           {0x89, 0xf0, 0x48, 0x85, 0xff, 0x74, 0x01, 0xc3, 0x89, 0xf8, 0xc3},
           "a ? b : a"},
          // test rdi,rdi; je +12; mov eax,esi; cmp rdi,rsi; sete al; je +1;
          // nop; ret; mov eax,edi; ret
          {"merged_low_byte",
           {0x48, 0x85, 0xff, 0x74, 0x0c, 0x89, 0xf0, 0x48, 0x39, 0xf7,
            0x0f, 0x94, 0xc0, 0x74, 0x01, 0x90, 0xc3, 0x89, 0xf8, 0xc3},
           "a ? ((uint32_t)b & 0xffffff00u) | (a == b) : a"},
          // test rdi,rdi; je +14; movzx eax,si; rol ax,8; test rsi,rsi;
          // je +1; nop; ret; movzx eax,si; ret
          {"rotated_word",
           {0x48, 0x85, 0xff, 0x74, 0x0e, 0x0f, 0xb7, 0xc6,
            0x66, 0xc1, 0xc0, 0x08, 0x48, 0x85, 0xf6, 0x74,
            0x01, 0x90, 0xc3, 0x0f, 0xb7, 0xc6, 0xc3},
           "a ? (uint16_t)((uint16_t)b << 8 | (uint16_t)b >> 8) : (uint16_t)b"},
      },
      4, "uint32_t");
}
