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
#include "gtest/gtest.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

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

TEST(ReturnContracts, TheRegisterAsTheFunctionEnteredItIsNoValue) {
  EXPECT_EQ(
      settle({function(CalleeEntry, {block({seed(), returning(rax(0))})})}),
      std::vector<bool>{true});
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
  // The caller returns 5 on the other path, so what it hands back from the
  // callee is a value too.
  MedFunc Callee = branching(
      CalleeEntry, operation(NdOp::INDIR_CALL, rax(1), {temporary(5)}));
  MedFunc Caller =
      conditionalCaller(MedVar::makeConst(5, 8), returning(rax(2)));
  EXPECT_EQ(settle({std::move(Callee), std::move(Caller)}),
            (std::vector<bool>{false, false}));
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

TEST(ReturnContracts, AConstantIsADeliberateValue) {
  // register_tm_clones' `return 0` beside the bare path: a value.
  EXPECT_EQ(
      settle({branching(CalleeEntry, operation(NdOp::COPY, rax(1),
                                               {MedVar::makeConst(0, 8)}))}),
      std::vector<bool>{false});
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
  Parent.Blocks.front().ExceptionalSuccs.emplace_back(-1, 0);
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
  // by its prototype, left in RAX, whatever its body seems to return.
  MedFunc Routine = function(
      CalleeEntry,
      {block({seed(), operation(NdOp::COPY, rax(1), {MedVar::makeConst(7, 8)}),
              returning(rax(1))})});
  Routine.Name = "_set_app_type";
  const MedFunc Thunk = function(
      CallerEntry, {block({callTo(CalleeEntry, 1), returning(rax(1))})});
  EXPECT_EQ(settle({std::move(Routine), Thunk}, BinaryFormat::COFF),
            (std::vector<bool>{false, true}));
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

TEST(ReturnContracts, ADeclaredReturnTypeStays) {
  MedFunc Declared =
      function(CalleeEntry, {block({seed(), returning(rax(0))})});
  Declared.SourceTypeHint.emplace();
  EXPECT_EQ(settle({std::move(Declared)}), std::vector<bool>{false});
}

} // namespace
