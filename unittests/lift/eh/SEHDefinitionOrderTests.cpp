//===- SEHDefinitionOrderTests.cpp - Text that ignores the print order ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// HighC prints the definitions of a unit from one writer, or from one writer
/// per worker thread, each of which prints whichever definitions it claims.
/// What a writer learned about the frame of the definition it printed before
/// must decide nothing for the next one: the text of a definition would
/// otherwise depend on how the threads were scheduled.
///
/// The case kept here is an `__except` arm that leaves by a jump to the code
/// after its try.  Whether that jump needs a label depends on which statements
/// between the try and its target print, and a copy of a frame address into a
/// name that is an alias of that address prints nothing.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/Support/raw_ostream.h"

#include <string>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kWork = 0x140003000;
constexpr va_t kFilter = 0x140002000;

MedVar stackPointer() {
  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.Size = 8;
  SP.TheArch = Arch::X64;
  SP.RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  return SP;
}

MedVar temporary(unsigned Id) {
  MedVar Temp;
  Temp.Kind = MedVar::Temp;
  Temp.Id = Id;
  Temp.Size = 8;
  Temp.TheArch = Arch::X64;
  return Temp;
}

ExprPtr read(unsigned Id) {
  return HighExpr::makeVar(temporary(Id), NdType::makeInt(8, false));
}

/// The address of the slot at SP - 8.
ExprPtr slotAddress() {
  return HighExpr::makeBinop(
      NdOp::INT_SUB,
      HighExpr::makeVar(stackPointer(), NdType::makeInt(8, false)),
      HighExpr::makeConst(8, 8));
}

HighStmt assign(va_t Address, unsigned Id, ExprPtr Value) {
  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Addr = Address;
  Assign.Dst = read(Id);
  Assign.Val = std::move(Value);
  return Assign;
}

HighStmt callWork(va_t Address, std::vector<ExprPtr> Arguments = {}) {
  HighStmt Call;
  Call.Kind = StmtKind::Call;
  Call.Addr = Address;
  Call.CallExpr = HighExpr::makeCall("work", kWork, std::move(Arguments));
  return Call;
}

HighStmt returnAt(va_t Address) {
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.Addr = Address;
  return Return;
}

/// `__try { work(); } __except (filter) { goto Target; }` at \p Address.
HighStmt tryWhoseArmJumps(va_t Address, va_t Target) {
  HighStmt Try;
  Try.Kind = StmtKind::SEHTry;
  Try.Addr = Address;
  Try.EHIsReducible = true;
  Try.Body = {callWork(Address)};
  HighEHClause Except;
  Except.Kind = HighEHClauseKind::SEHExcept;
  Except.FilterOrActionVA = kFilter;
  Except.HandlerVA = Address + 0x30;
  Try.EHClauses.push_back(Except);
  HighStmt Leave;
  Leave.Kind = StmtKind::Goto;
  Leave.Addr = Address + 0x34;
  Leave.GotoTarget = Target;
  Try.EHClauseBodies.push_back({Leave});
  return Try;
}

HighFunc function(const char *Name, va_t Entry, std::vector<HighStmt> Body) {
  HighFunc Func;
  Func.Name = Name;
  Func.Entry = Entry;
  Func.FrameSize = 32;
  Func.ReturnType = NdType::makeVoid();
  Func.Body = std::move(Body);
  return Func;
}

/// Names no frame address at all.
HighFunc callsOnly() {
  return function("calls_only", 0x140001400,
                  {callWork(0x140001404), returnAt(0x140001410)});
}

/// `t4 = SP - 8; t5 = SP - 8; work(t4, t5);`: leaves a writer knowing both
/// names as aliases of one frame slot.
HighFunc namesItsSlotTwice() {
  std::vector<ExprPtr> Arguments;
  Arguments.push_back(read(4));
  Arguments.push_back(read(5));
  return function("names_its_slot_twice", 0x140001800,
                  {assign(0x140001804, 4, slotAddress()),
                   assign(0x140001808, 5, slotAddress()),
                   callWork(0x140001810, std::move(Arguments)),
                   returnAt(0x140001820)});
}

/// ```
/// __try { work(); } __except (filter) { goto after; }
/// t4 = SP - 8;
/// after: work(t4); return;
/// ```
/// The copy of the slot's address prints nothing, so the arm's jump lands
/// where the try falls through anyway and needs no label.
HighFunc armJumpsPastItsOwnAlias() {
  std::vector<ExprPtr> Arguments;
  Arguments.push_back(read(4));
  return function("arm_jumps_past_its_own_alias", 0x140001000,
                  {tryWhoseArmJumps(0x140001010, 0x140001060),
                   assign(0x140001050, 4, slotAddress()),
                   callWork(0x140001060, std::move(Arguments)),
                   returnAt(0x140001070)});
}

/// ```
/// t4 = work();
/// __try { work(); } __except (filter) { goto after; }
/// t5 = t4;
/// after: work(t5); return;
/// ```
/// Neither name is an address of this frame: the copy is a statement, and the
/// arm's jump passes it.
HighFunc armJumpsPastACopy() {
  std::vector<ExprPtr> Arguments;
  Arguments.push_back(read(5));
  return function(
      "arm_jumps_past_a_copy", 0x140001200,
      {assign(0x140001204, 4, HighExpr::makeCall("work", kWork, {})),
       tryWhoseArmJumps(0x140001210, 0x140001260),
       assign(0x140001250, 5, read(4)),
       callWork(0x140001260, std::move(Arguments)), returnAt(0x140001270)});
}

std::string emit(const std::vector<HighFunc> &Functions) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  EXPECT_TRUE(HighCEmitter().emit(Functions, OS, Options));
  OS.flush();
  return Source;
}

/// The definition of \p Name in \p Source, from its signature to its closing
/// brace, or empty.
std::string definitionOf(const std::string &Source, const std::string &Name) {
  for (size_t At = Source.find(" " + Name + "("); At != std::string::npos;
       At = Source.find(" " + Name + "(", At + 1)) {
    const size_t LineEnd = Source.find('\n', At);
    if (LineEnd == std::string::npos || LineEnd == 0 ||
        Source[LineEnd - 1] != '{')
      continue;
    const size_t LineBegin = Source.rfind('\n', At) + 1;
    const size_t End = Source.find("\n}\n", LineEnd);
    if (End == std::string::npos)
      return {};
    return Source.substr(LineBegin, End + 3 - LineBegin);
  }
  return {};
}

/// \p Subject's text when the unit prints \p Before first.
std::string printedAfter(const HighFunc &Before, const HighFunc &Subject) {
  return definitionOf(emit({Before, Subject}), Subject.Name);
}

// A definition's own aliases decide what its jump passes, whatever the writer
// printed before: after a definition that names no frame address the copy
// still prints nothing, and the jump still needs no label.
TEST(SEHDefinitionOrder, AnArmThatPassesItsOwnAliasPrintsTheSameInAnyOrder) {
  const HighFunc Subject = armJumpsPastItsOwnAlias();
  const std::string Alone = definitionOf(emit({Subject}), Subject.Name);
  ASSERT_FALSE(Alone.empty()) << emit({Subject});
  EXPECT_NE(Alone.find("__except"), std::string::npos) << Alone;
  EXPECT_EQ(Alone.find("goto "), std::string::npos) << Alone;
  EXPECT_EQ(Alone.find("L_140001060"), std::string::npos) << Alone;

  EXPECT_EQ(printedAfter(callsOnly(), Subject), Alone);
  EXPECT_EQ(printedAfter(namesItsSlotTwice(), Subject), Alone);
  EXPECT_EQ(printedAfter(armJumpsPastACopy(), Subject), Alone);
  EXPECT_EQ(definitionOf(emit({Subject, callsOnly()}), Subject.Name), Alone);
}

// The aliases of the definition printed before hide nothing in the next one:
// a copy between two names that were aliases of one slot there is a statement
// here, and the jump that passes it keeps its label.
TEST(SEHDefinitionOrder, AnArmThatPassesACopyPrintsTheSameInAnyOrder) {
  const HighFunc Subject = armJumpsPastACopy();
  const std::string Alone = definitionOf(emit({Subject}), Subject.Name);
  ASSERT_FALSE(Alone.empty()) << emit({Subject});
  EXPECT_NE(Alone.find("__except"), std::string::npos) << Alone;

  EXPECT_EQ(printedAfter(namesItsSlotTwice(), Subject), Alone);
  EXPECT_EQ(printedAfter(callsOnly(), Subject), Alone);
  EXPECT_EQ(printedAfter(armJumpsPastItsOwnAlias(), Subject), Alone);
  EXPECT_EQ(definitionOf(emit({Subject, namesItsSlotTwice()}), Subject.Name),
            Alone);
}

// The definitions around them print the same in either place too.
TEST(SEHDefinitionOrder, TheDefinitionsAroundPrintTheSameInAnyOrder) {
  for (const HighFunc &Subject : {callsOnly(), namesItsSlotTwice()}) {
    const std::string Alone = definitionOf(emit({Subject}), Subject.Name);
    ASSERT_FALSE(Alone.empty()) << emit({Subject});
    for (const HighFunc &Before :
         {armJumpsPastItsOwnAlias(), armJumpsPastACopy()})
      EXPECT_EQ(printedAfter(Before, Subject), Alone) << Before.Name;
  }
}

} // namespace
