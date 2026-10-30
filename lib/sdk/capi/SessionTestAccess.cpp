//===- SessionTestAccess.cpp - Private session test mutations ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SessionTestAccess.h"

#include "InstructionFlow.h"
#include "ProgramMain.h"
#include "SessionImpl.h"

#include "neverd/libc/LibCNames.h"

namespace neverd::sdk {
InstructionFlow SessionTestAccess::instructionFlow(const BinaryImage &Img,
                                                   Decoder &Dec,
                                                   const DecodedInsn &Insn) {
  const libc::NoReturnTargetIndex Targets(Img);
  InstructionFlowScratch Scratch(&Targets);
  return Scratch.summarize(Img, Dec, Insn);
}

std::vector<std::pair<neverd_va_t, unsigned>>
SessionTestAccess::instructionDataAccesses(const BinaryImage &Img, Decoder &Dec,
                                           const DecodedInsn &Insn,
                                           bool Filter) {
  InstructionFlowScratch Scratch;
  const auto Refs = Filter ? Scratch.dataAccesses(Img, Dec, Insn)
                           : Scratch.summarize(Img, Dec, Insn).Refs;
  std::vector<std::pair<neverd_va_t, unsigned>> Widths;
  for (const auto &Ref : Refs)
    if (Ref.Kind == "read" || Ref.Kind == "write")
      Widths.emplace_back(Ref.To, Ref.Bytes);
  return Widths;
}

void SessionTestAccess::resetFunctions(Session &State) {
  State.resetFunctionsFromImage();
}
void SessionTestAccess::selectFunction(Session &State, neverd_va_t Entry) {
  State.selectFunction(Entry);
}
bool SessionTestAccess::keepsFunctionList(const Session &State) {
  return State.functionListCurrent();
}
bool SessionTestAccess::functionListIsRebuilt(Session &State) {
  auto Functions = State.Functions;
  auto OriginalNames = State.OriginalNames;
  auto Inputs = State.ListInputs;
  auto Widths = std::move(State.AccessWidths);
  auto Extents = std::move(State.AccessWidthsFunctions);
  auto Scan = std::move(State.AccessScan);
  State.resetFunctionsFromImage();
  const bool Rebuilt =
      State.Functions == Functions && State.OriginalNames == OriginalNames;
  State.Functions = std::move(Functions);
  State.OriginalNames = std::move(OriginalNames);
  State.functionListChanged();
  State.ListInputs = std::move(Inputs);
  State.AccessWidths = std::move(Widths);
  State.AccessWidthsFunctions = std::move(Extents);
  State.AccessScan = std::move(Scan);
  return Rebuilt;
}
void SessionTestAccess::clearPipeline(Session &State) { State.clearPipeline(); }
void SessionTestAccess::clearFeatures(Session &State) { State.SigDB.clear(); }
const DataAccessWidths *SessionTestAccess::dataAccessWidths(Session &State) {
  return State.dataAccessWidths();
}
std::optional<DataAccessWidths>
SessionTestAccess::collectDataAccessWidths(Session &State) {
  return sdk::collectDataAccessWidths(State.Img, State.Functions);
}
std::optional<neverd_va_t>
SessionTestAccess::programMain(const BinaryImage &Img) {
  return findProgramMain(Img);
}
FunctionPage SessionTestAccess::functionPage(const Session &State,
                                             neverd_va_t First, int Limit) {
  return sdk::functionPage(State, First, Limit);
}
} // namespace neverd::sdk
