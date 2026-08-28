//===- ProcessInputReplay.cpp - Defensive replay-plan validation ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "ProcessInputReplay.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace neverd::safety {
namespace {

bool checkedCharge(uint64_t &Used, uint64_t Amount, uint64_t Limit) {
  if (Amount > std::numeric_limits<uint64_t>::max() - Used)
    return false;
  Used += Amount;
  return Used <= Limit;
}

bool hasByte(const std::string &Value, char Needle) {
  return Value.find(Needle) != std::string::npos;
}

std::optional<llvm::APInt> assignmentValue(const SolverAssignment &Assignment) {
  if (Assignment.Width == 0)
    return std::nullopt;
  llvm::StringRef Digits = Assignment.ValueHex;
  if (!Digits.consume_front("0x") && !Digits.consume_front("0X"))
    return std::nullopt;
  llvm::APInt Value(Assignment.Width, 0);
  if (Digits.empty() || Digits.getAsInteger(16, Value))
    return std::nullopt;
  return Value;
}

} // namespace

std::optional<std::string>
validateProcessInputReplay(const ReplayPlan &Plan,
                           const std::vector<SolverAssignment> &Model,
                           uint64_t ByteBudget) {
  if (Plan.Version != 1)
    return "unsupported process-input replay version";
  if (Plan.Inputs.empty())
    return "process-input replay has no literal inputs";
  ByteBudget = std::min(ByteBudget, kProcessInputReplayByteBudget);

  std::set<uint32_t> QueryVariables;
  for (uint32_t Id : Plan.QueryVariables)
    if (!QueryVariables.insert(Id).second)
      return "process-input replay repeats a query variable";

  std::set<uint32_t> ModelVariables;
  std::map<uint32_t, const SolverAssignment *> ModelById;
  for (const SolverAssignment &Assignment : Model)
    if (QueryVariables.count(Assignment.Id) != 0 &&
        !ModelVariables.insert(Assignment.Id).second) {
      return "process-input replay has an ambiguous solver assignment";
    } else if (QueryVariables.count(Assignment.Id) != 0) {
      ModelById.emplace(Assignment.Id, &Assignment);
    }
  if (ModelVariables != QueryVariables)
    return "process-input replay query variable has no solver assignment";

  uint64_t LiteralBytes = 0;
  std::set<uint32_t> BoundVariables;
  std::set<std::tuple<unsigned, va_t, int, uint64_t>> SourceOccurrences;
  bool HasStandardInput = false;
  for (const ReplayInput &Input : Plan.Inputs) {
    if (Input.CallVA == 0 || Input.Seq < 0)
      return "process-input replay source occurrence is incomplete";
    if (Input.Invocation >
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      return "process-input replay source invocation is not serializable";
    if (!SourceOccurrences
             .insert({static_cast<unsigned>(Input.Kind), Input.CallVA,
                      Input.Seq, Input.Invocation})
             .second)
      return "process-input replay repeats a source occurrence";

    const uint64_t ByteCount = static_cast<uint64_t>(Input.Bytes.size());
    if (ByteCount > std::numeric_limits<uint64_t>::max() - Input.Offset)
      return "process-input replay source range overflows";
    uint64_t SourceEnd = Input.Offset + ByteCount;
    if (Input.TerminatorImplicit) {
      if (SourceEnd == std::numeric_limits<uint64_t>::max())
        return "process-input replay terminator offset overflows";
      ++SourceEnd;
    }
    (void)SourceEnd;

    switch (Input.Kind) {
    case ReplayInputKind::Environment:
      if (Input.Name.empty() || hasByte(Input.Name, '\0') ||
          hasByte(Input.Name, '='))
        return "process-input replay has an invalid environment name";
      if (Input.Offset != 0 || !Input.TerminatorImplicit || Input.EOFAfter)
        return "process-input replay has invalid environment boundaries";
      for (uint8_t Byte : Input.Bytes)
        if (Byte == 0)
          return "process-input replay environment value contains NUL";
      if (!checkedCharge(LiteralBytes, static_cast<uint64_t>(Input.Name.size()),
                         ByteBudget))
        return "process-input replay byte budget exceeded";
      break;
    case ReplayInputKind::StandardInput:
      if (!Input.Name.empty() || Input.TerminatorImplicit)
        return "process-input replay has invalid standard-input boundaries";
      if (HasStandardInput || Input.Invocation != 0 || Input.Offset != 0 ||
          !Input.EOFAfter)
        return "process-input replay supports only the first standard-input "
               "consumption";
      HasStandardInput = true;
      break;
    default:
      return "process-input replay uses an unsupported input kind";
    }

    if (!checkedCharge(LiteralBytes, ByteCount, ByteBudget) ||
        (Input.TerminatorImplicit &&
         !checkedCharge(LiteralBytes, 1, ByteBudget)))
      return "process-input replay byte budget exceeded";

    for (const ReplayBinding &Binding : Input.Bindings) {
      const auto AssignmentIt = ModelById.find(Binding.AssignmentId);
      if (AssignmentIt == ModelById.end())
        return "process-input replay binding has no solver assignment";
      const SolverAssignment &Assignment = *AssignmentIt->second;
      const std::optional<llvm::APInt> Value = assignmentValue(Assignment);
      if (!Value)
        return "process-input replay has a malformed solver assignment";
      switch (Binding.Role) {
      case ReplayBindingRole::Extent: {
        if (Binding.ByteOffset != 0)
          return "process-input replay extent binding has a byte offset";
        const uint64_t Extent =
            ByteCount + (Input.TerminatorImplicit ? uint64_t(1) : uint64_t(0));
        if (Value->getActiveBits() > 64 || Value->getZExtValue() != Extent)
          return "process-input replay extent disagrees with literal bytes";
        break;
      }
      case ReplayBindingRole::Success:
        if (Binding.ByteOffset != 0 || Value->isZero())
          return "process-input replay success binding is not satisfied";
        break;
      case ReplayBindingRole::Byte:
        if (Assignment.Width != 8 || Binding.ByteOffset >= Input.Bytes.size() ||
            Value->getZExtValue() != Input.Bytes[Binding.ByteOffset])
          return "process-input replay byte binding disagrees with literal "
                 "bytes";
        break;
      default:
        return "process-input replay has an untyped symbolic binding";
      }
      if (QueryVariables.count(Binding.AssignmentId) == 0)
        return "process-input replay binds a non-query variable";
      if (!BoundVariables.insert(Binding.AssignmentId).second)
        return "process-input replay query variable has multiple bindings";
    }
  }

  if (BoundVariables != QueryVariables)
    return "process-input replay query variable lacks a typed binding";
  return std::nullopt;
}

ProcessInputReplayResult
buildProcessInputReplay(ReplayPlan Candidate,
                        const std::vector<SolverAssignment> &Model,
                        uint64_t ByteBudget) {
  if (std::optional<std::string> Error =
          validateProcessInputReplay(Candidate, Model, ByteBudget))
    return {std::nullopt, std::move(*Error)};
  return {std::move(Candidate), {}};
}

} // namespace neverd::safety
