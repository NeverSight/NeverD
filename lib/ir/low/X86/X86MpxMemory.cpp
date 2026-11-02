//===- X86MpxMemory.cpp - MPX memory and register completion -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Sequence BDE/BTE transactions and publish bounds only at the architectural
/// completion boundary. Memory faults retain the provider's partial effects.
//===----------------------------------------------------------------------===//

#include "neverd/ir/X86/X86MpxMemory.h"

namespace neverd {
namespace {

X86MpxResult result(X86MpxMemoryOutcome Outcome) {
  switch (Outcome) {
  case X86MpxMemoryOutcome::Completed:
    return X86MpxResult::Completed;
  case X86MpxMemoryOutcome::Fault:
    return X86MpxResult::MemoryFault;
  case X86MpxMemoryOutcome::Unavailable:
    return X86MpxResult::UnknownMemory;
  }
  return X86MpxResult::UnknownMemory;
}

uint64_t operandAddress(X86MpxMode Mode, uint64_t Address) {
  return Mode == X86MpxMode::Bits64 ? Address : uint32_t(Address);
}

X86MpxAddress table(X86MpxContext &Context, X86MpxMode Mode,
                    std::optional<uint64_t> PointerAddress,
                    X86MpxMemoryProvider &Memory) {
  const auto Directory = Context.directoryAddress(Mode, PointerAddress);
  if (Directory.Result != X86MpxResult::Completed)
    return Directory;
  const auto Entry = Memory.access(
      {X86MpxMemoryOperation::LoadDirectory, Mode, Directory.Address});
  if (Entry.Outcome != X86MpxMemoryOutcome::Completed)
    return {result(Entry.Outcome)};
  return Context.tableAddress(Mode, PointerAddress, Entry.Words[0]);
}

} // namespace

X86MpxResult X86MpxContext::load(X86MpxMode Mode, unsigned Destination,
                                 std::optional<uint64_t> Address,
                                 X86MpxMemoryProvider &Memory,
                                 X86SegmentRegister Segment,
                                 bool DefaultStack) {
  if (!x86SegmentRegisterName(unsigned(Segment)))
    return X86MpxResult::InvalidInput;
  const auto Ready = operationStatus(Mode, Destination);
  if (Ready != X86MpxResult::Completed)
    return Ready;
  if (!Address)
    return X86MpxResult::UnknownOperand;
  const auto Loaded = Memory.access({X86MpxMemoryOperation::LoadBounds,
                                     Mode,
                                     operandAddress(Mode, *Address),
                                     false,
                                     {},
                                     Segment,
                                     DefaultStack});
  if (Loaded.Outcome == X86MpxMemoryOutcome::Completed)
    writeBounds(Mode, Destination, Loaded.Words[0], Loaded.Words[1]);
  return result(Loaded.Outcome);
}

X86MpxResult X86MpxContext::store(X86MpxMode Mode, unsigned Source,
                                  std::optional<uint64_t> Address, bool Locked,
                                  X86MpxMemoryProvider &Memory,
                                  X86SegmentRegister Segment,
                                  bool DefaultStack) {
  if (!x86SegmentRegisterName(unsigned(Segment)))
    return X86MpxResult::InvalidInput;
  const auto Stored = storeValue(Mode, Source);
  if (Stored.Result != X86MpxResult::Completed)
    return Stored.Result;
  if (!Address)
    return X86MpxResult::UnknownOperand;
  return result(Memory
                    .access({X86MpxMemoryOperation::StoreBounds,
                             Mode,
                             operandAddress(Mode, *Address),
                             Locked,
                             {Stored.Lower, Stored.Upper, 0},
                             Segment,
                             DefaultStack})
                    .Outcome);
}

X86MpxResult
X86MpxContext::loadTable(X86MpxMode Mode, unsigned Destination,
                         std::optional<uint64_t> LinearPointerAddress,
                         std::optional<uint64_t> PointerValue,
                         X86MpxMemoryProvider &Memory) {
  const auto Ready = operationStatus(Mode, Destination);
  if (Ready != X86MpxResult::Completed)
    return Ready;
  if (!PointerValue)
    return X86MpxResult::UnknownOperand;
  const auto Table = table(*this, Mode, LinearPointerAddress, Memory);
  if (Table.Result != X86MpxResult::Completed)
    return Table.Result;
  const auto Loaded =
      Memory.access({X86MpxMemoryOperation::LoadTable, Mode, Table.Address});
  if (Loaded.Outcome == X86MpxMemoryOutcome::Fault)
    tableReadFault(Mode, Destination);
  if (Loaded.Outcome != X86MpxMemoryOutcome::Completed)
    return result(Loaded.Outcome);
  return completeTableLoad(Mode, Destination, Loaded.Words[0], Loaded.Words[1],
                           Loaded.Words[2], *PointerValue);
}

X86MpxResult
X86MpxContext::storeTable(X86MpxMode Mode, unsigned Source,
                          std::optional<uint64_t> LinearPointerAddress,
                          std::optional<uint64_t> PointerValue,
                          X86MpxMemoryProvider &Memory) {
  const auto Stored = storeValue(Mode, Source);
  if (Stored.Result != X86MpxResult::Completed)
    return Stored.Result;
  if (!PointerValue)
    return X86MpxResult::UnknownOperand;
  const auto Table = table(*this, Mode, LinearPointerAddress, Memory);
  if (Table.Result != X86MpxResult::Completed)
    return Table.Result;
  return result(Memory
                    .access({X86MpxMemoryOperation::StoreTable,
                             Mode,
                             Table.Address,
                             false,
                             {Stored.Lower, Stored.Upper,
                              operandAddress(Mode, *PointerValue)}})
                    .Outcome);
}

} // namespace neverd
