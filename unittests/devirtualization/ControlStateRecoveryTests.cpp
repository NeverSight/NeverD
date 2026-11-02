//===- ControlStateRecoveryTests.cpp - Spilled dispatch control -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/InterpreterSpecialization.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Errc.h"

#include <map>
#include <optional>
#include <string>

using namespace neverd;
using namespace neverd::analysis;

namespace {
constexpr va_t Entry = 0x500;
constexpr va_t Route = 0x520;
constexpr va_t Reload = 0x540;
constexpr va_t Dispatch = 0x560;
constexpr va_t Latch = 0x580;
constexpr va_t Exit = 0x5a0;
constexpr va_t HandlerBase = 0x700;
constexpr va_t TableBase = 0x6000;
constexpr uint64_t ResultRegister = 0;
constexpr uint64_t CounterRegister = 24;
constexpr uint64_t FrameRegister = 32;
constexpr uint64_t LimitRegister = 48;
constexpr uint64_t InputRegister = 56;
constexpr uint64_t SelectorAddressRegister = 64;
constexpr uint64_t PayloadAddressRegister = 72;
constexpr uint64_t BankRegister = 80;
constexpr uint64_t SelectorRegister = 88;
constexpr uint64_t AddressRegister = 96;
constexpr uint64_t ValueRegister = 104;
constexpr uint64_t ConditionRegister = 112;
constexpr uint64_t UnknownPointerRegister = 120;
constexpr int64_t SelectorSlot = -72;

NdVar r(uint64_t Offset, uint16_t Bytes = 8) {
  return NdVar::reg(Offset, Bytes);
}
NdVar c(uint64_t Value, uint16_t Bytes = 8) {
  return NdVar::scalar(Value, Bytes);
}
LowOp operation(NdOp Opcode, NdVar Output,
                std::initializer_list<NdVar> Inputs) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  for (NdVar Input : Inputs)
    Op.addInput(Input);
  return Op;
}
LowOp jump(va_t Address) {
  return operation(NdOp::BRANCH, {}, {NdVar::cst(Address, 8)});
}
LowOp ret() { return operation(NdOp::RETURN, {}, {r(ResultRegister)}); }

class BankProvider : public SpecializationProvider {
public:
  std::map<va_t, SpecializationInstruction> Code;
  std::map<va_t, uint8_t> Image;

  void add(va_t Address, llvm::ArrayRef<LowOp> Ops,
           va_t Fallthrough = InvalidVA) {
    SpecializationInstruction Instruction;
    Instruction.Ops.assign(Ops.begin(), Ops.end());
    Instruction.Origin.Address = Address;
    Instruction.Origin.Size = 1;
    Instruction.Origin.OpCount = Ops.size();
    Instruction.Fallthrough = {Fallthrough == InvalidVA ? Address + 1
                                                        : Fallthrough};
    for (size_t I = 0; I < Instruction.Ops.size(); ++I) {
      LowOp &Op = Instruction.Ops[I];
      Op.Addr = Address;
      Op.Seq = static_cast<int>(I);
      if (Op.Opcode == NdOp::RETURN) {
        Instruction.Origin.Control = LowInstructionControl::Return;
        Instruction.Origin.ControlFlags = LowInstructionControlFlag::Return;
      } else if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
                 Op.Opcode == NdOp::INDIR_BR) {
        Instruction.Origin.Control = LowInstructionControl::Branch;
        Instruction.Origin.ControlFlags = LowInstructionControlFlag::Branch;
        if (Op.Opcode == NdOp::COND_BR)
          Instruction.Origin.ControlFlags |=
              LowInstructionControlFlag::Conditional;
        if (Op.Opcode == NdOp::INDIR_BR)
          Instruction.Origin.ControlFlags |=
              LowInstructionControlFlag::Indirect;
        else
          Instruction.Origin.Immediate = Op.Inputs[0].Offset;
      }
    }
    Code.emplace(Address, std::move(Instruction));
  }

  llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor Cursor) override {
    const auto At = Code.find(Cursor.Address);
    if (At == Code.end())
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "unknown synthetic handler");
    return At->second;
  }

  std::optional<SpecializationImmutableRead>
  immutableRead(va_t Address, uint16_t Bytes) override {
    SpecializationImmutableRead Read;
    Read.Evidence = "independently authored immutable table";
    for (unsigned I = 0; I < Bytes; ++I) {
      const auto At = Image.find(Address + I);
      if (At == Image.end())
        return std::nullopt;
      Read.Bytes.push_back(At->second);
    }
    return Read;
  }
};

enum class SelectorInput { MaskedScalar, UnboundedScalar, UnknownMemory };

// This toy register bank contains one dispatch selector and one unconstrained
// payload. Neither an image-specific layout nor an entry value is assumed.
BankProvider makeBankProgram(bool Loop,
                             SelectorInput Input = SelectorInput::MaskedScalar,
                             unsigned LaneCount = 4) {
  BankProvider P;
  P.add(Entry,
        {operation(NdOp::INT_SUB, r(BankRegister), {r(FrameRegister), c(96)}),
         operation(NdOp::INT_ADD, r(SelectorAddressRegister),
                   {r(BankRegister), c(24)}),
         operation(NdOp::INT_ADD, r(PayloadAddressRegister),
                   {r(BankRegister), c(32)}),
         operation(NdOp::COPY, r(ResultRegister), {c(0)}),
         operation(NdOp::COPY, r(CounterRegister), {c(0)}),
         operation(NdOp::INT_XOR, r(ValueRegister), {r(InputRegister), c(45)}),
         operation(NdOp::STORE, {},
                   {r(PayloadAddressRegister), r(ValueRegister)}),
         Input == SelectorInput::MaskedScalar
             ? operation(NdOp::INT_AND, r(SelectorRegister),
                         {r(InputRegister), c(LaneCount - 1)})
         : Input == SelectorInput::UnboundedScalar
             ? operation(NdOp::COPY, r(SelectorRegister), {r(InputRegister)})
             : operation(NdOp::LOAD, r(SelectorRegister),
                         {r(UnknownPointerRegister)}),
         operation(NdOp::STORE, {},
                   {r(SelectorAddressRegister), r(SelectorRegister)}),
         jump(Route)});
  P.add(Route,
        {operation(NdOp::INT_XOR, r(ValueRegister), {r(InputRegister), c(91)}),
         jump(Reload)});
  P.add(Reload, {operation(NdOp::LOAD, r(SelectorRegister),
                           {r(SelectorAddressRegister)}),
                 jump(Dispatch)});
  P.add(Dispatch,
        {operation(NdOp::INT_MULT, r(AddressRegister),
                   {r(SelectorRegister), c(8)}),
         operation(NdOp::INT_ADD, r(AddressRegister),
                   {r(AddressRegister), c(TableBase)}),
         operation(NdOp::LOAD, r(ValueRegister), {r(AddressRegister)}),
         operation(NdOp::INDIR_BR, {}, {r(ValueRegister)})});
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    const va_t Handler = HandlerBase + 0x20 * Lane;
    for (unsigned Byte = 0; Byte < 8; ++Byte)
      P.Image[TableBase + Lane * 8 + Byte] = Handler >> (Byte * 8);
    if (!Loop) {
      P.add(Handler, {operation(NdOp::LOAD, r(ValueRegister),
                                {r(PayloadAddressRegister)}),
                      operation(NdOp::INT_ADD, r(ResultRegister),
                                {r(ValueRegister), c(3 + Lane * 11)}),
                      ret()});
    } else {
      P.add(
          Handler,
          {operation(NdOp::LOAD, r(ValueRegister), {r(PayloadAddressRegister)}),
           operation(NdOp::INT_ADD, r(ValueRegister),
                     {r(ValueRegister), c(3 + Lane * 11)}),
           operation(NdOp::INT_ADD, r(ResultRegister),
                     {r(ResultRegister), r(ValueRegister)}),
           operation(NdOp::INT_ADD, r(SelectorRegister),
                     {r(SelectorRegister), c(1)}),
           operation(NdOp::INT_AND, r(SelectorRegister),
                     {r(SelectorRegister), c(LaneCount - 1)}),
           operation(NdOp::STORE, {},
                     {r(SelectorAddressRegister), r(SelectorRegister)}),
           jump(Latch)});
    }
  }
  if (Loop) {
    P.add(Latch,
          {operation(NdOp::INT_ADD, r(CounterRegister),
                     {r(CounterRegister), c(1)}),
           operation(NdOp::INT_LESS, r(ConditionRegister, 1),
                     {r(CounterRegister), r(LimitRegister)}),
           operation(NdOp::COND_BR, {}, {c(Route), r(ConditionRegister, 1)})},
          Exit);
    P.add(Exit, {ret()});
  }
  return P;
}

void insertTransparentPhases(BankProvider &Provider, va_t Predecessor,
                             va_t Successor, unsigned PhaseCount) {
  if (!PhaseCount)
    return;
  constexpr va_t PhaseBase = 0x2000;
  auto Operations = Provider.Code.at(Predecessor).Ops;
  Operations.back() = jump(PhaseBase);
  Provider.Code.erase(Predecessor);
  Provider.add(Predecessor, Operations);
  for (unsigned Phase = 0; Phase < PhaseCount; ++Phase)
    Provider.add(
        PhaseBase + 0x20 * Phase,
        {jump(Phase + 1 == PhaseCount ? Successor
                                      : PhaseBase + 0x20 * (Phase + 1))});
}

BankProvider
makeLongControlLoop(SelectorInput Input = SelectorInput::MaskedScalar) {
  auto Provider = makeBankProgram(true, Input);
  // These native phases transport the same selector without rebuilding its
  // bound. Its only bounds come from entry and the loop handlers' updates.
  insertTransparentPhases(Provider, Reload, Dispatch, 20);
  return Provider;
}

constexpr va_t FirstPhaseEntry = 0x200;
constexpr uint64_t FirstSelectorRegister = 0x300;
constexpr int64_t FirstSelectorSlot = -112;

BankProvider makeSeparateControlLifetimesProgram() {
  auto Provider = makeBankProgram(true, SelectorInput::MaskedScalar, 8);
  constexpr va_t FirstReload = 0x220;
  constexpr va_t FirstDispatch = 0x240;
  constexpr va_t FirstHandlerBase = 0x900;
  constexpr va_t FirstTableBase = 0x6800;
  constexpr uint64_t FirstSlotAddressRegister = 0x308;
  Provider.add(
      FirstPhaseEntry,
      {operation(NdOp::INT_ADD, r(FirstSlotAddressRegister),
                 {r(FrameRegister), c(FirstSelectorSlot)}),
       operation(NdOp::INT_AND, r(FirstSelectorRegister),
                 {r(InputRegister), c(7)}),
       operation(NdOp::STORE, {},
                 {r(FirstSlotAddressRegister), r(FirstSelectorRegister)}),
       jump(FirstReload)});
  Provider.add(FirstReload, {operation(NdOp::LOAD, r(FirstSelectorRegister),
                                       {r(FirstSlotAddressRegister)}),
                             jump(FirstDispatch)});
  Provider.add(FirstDispatch,
               {operation(NdOp::INT_MULT, r(AddressRegister),
                          {r(FirstSelectorRegister), c(8)}),
                operation(NdOp::INT_ADD, r(AddressRegister),
                          {r(AddressRegister), c(FirstTableBase)}),
                operation(NdOp::LOAD, r(ValueRegister), {r(AddressRegister)}),
                operation(NdOp::INDIR_BR, {}, {r(ValueRegister)})});
  for (unsigned Lane = 0; Lane < 8; ++Lane) {
    const va_t Handler = FirstHandlerBase + 0x20 * Lane;
    for (unsigned Byte = 0; Byte < 8; ++Byte)
      Provider.Image[FirstTableBase + Lane * 8 + Byte] = Handler >> (8 * Byte);
    Provider.add(Handler, {jump(Entry)});
  }
  // Both selector domains contain eight independent values. The first one
  // dies before the second phase and must not multiply its relation by eight.
  auto LoopEntry = Provider.Code.at(Entry).Ops;
  LoopEntry[7].Inputs[0] = r(SelectorRegister);
  LoopEntry.insert(LoopEntry.begin() + 7,
                   operation(NdOp::INT_RIGHT, r(SelectorRegister),
                             {r(InputRegister), c(3)}));
  Provider.Code.erase(Entry);
  Provider.add(Entry, LoopEntry);
  return Provider;
}

constexpr uint64_t FirstProducerRegister = 0x1200;
constexpr unsigned FirstProducerInputs = 17;

BankProvider makeFiniteProducerProgram() {
  auto Provider = makeSeparateControlLifetimesProgram();
  llvm::SmallVector<LowOp, 20> Producers{operation(
      NdOp::COPY, r(FirstSelectorRegister), {r(FirstProducerRegister)})};
  for (unsigned I = 1; I < FirstProducerInputs; ++I)
    Producers.push_back(operation(
        NdOp::INT_XOR, r(FirstSelectorRegister),
        {r(FirstSelectorRegister), r(FirstProducerRegister + I * 8)}));
  auto FirstEntry = Provider.Code.at(FirstPhaseEntry).Ops;
  FirstEntry[1].Inputs[0] = r(FirstSelectorRegister);
  FirstEntry.insert(FirstEntry.begin() + 1, Producers.begin(), Producers.end());
  Provider.Code.erase(FirstPhaseEntry);
  Provider.add(FirstPhaseEntry, FirstEntry);

  constexpr uint64_t FirstContributionRegister = 0x310;
  for (unsigned Lane = 0; Lane < 8; ++Lane) {
    const va_t Handler = 0x900 + 0x20 * Lane;
    Provider.Code.erase(Handler);
    Provider.add(Handler, {operation(NdOp::COPY, r(FirstContributionRegister),
                                     {c(9 + 7 * Lane)}),
                           jump(Entry)});
  }
  // The first dispatch contributes to the observable result. Its 17 caller
  // inputs must stay dynamic even when discovery can stop at the masked value.
  auto LoopEntry = Provider.Code.at(Entry).Ops;
  LoopEntry[3].Inputs[0] = r(FirstContributionRegister);
  Provider.Code.erase(Entry);
  Provider.add(Entry, LoopEntry);
  return Provider;
}

constexpr int64_t WideControlSlot = -32;

BankProvider makeCommonControlByteProgram(bool FrameField,
                                          llvm::endianness Order,
                                          bool HasConflictingByte = false) {
  BankProvider Provider;
  llvm::SmallVector<LowOp, 8> EntryOps{
      operation(NdOp::INT_EQUAL, r(ConditionRegister, 1),
                {r(InputRegister), c(0)}),
      operation(NdOp::SELECT, r(SelectorRegister),
                {r(ConditionRegister, 1), c(0x100), c(0)})};
  if (HasConflictingByte) {
    EntryOps.push_back(operation(NdOp::INT_EQUAL, r(ConditionRegister, 1),
                                 {r(InputRegister), c(1)}));
    EntryOps.push_back(
        operation(NdOp::SELECT, r(SelectorRegister),
                  {r(ConditionRegister, 1), c(8), r(SelectorRegister)}));
  }
  if (FrameField) {
    EntryOps.push_back(operation(NdOp::INT_ADD, r(SelectorAddressRegister),
                                 {r(FrameRegister), c(WideControlSlot)}));
    EntryOps.push_back(operation(
        NdOp::STORE, {}, {r(SelectorAddressRegister), r(SelectorRegister)}));
  }
  EntryOps.push_back(jump(Route));
  Provider.add(Entry, EntryOps);
  llvm::SmallVector<LowOp, 12> ConsumerOps;
  if (FrameField)
    ConsumerOps.push_back(operation(NdOp::LOAD, r(SelectorRegister),
                                    {r(SelectorAddressRegister)}));
  const uint64_t LowByte = Order == llvm::endianness::little ? 0 : 7;
  ConsumerOps.append(
      {operation(NdOp::INT_ZEXT, r(AddressRegister),
                 {r(SelectorRegister + LowByte, 1)}),
       operation(NdOp::INT_ADD, r(AddressRegister),
                 {r(FrameRegister), r(AddressRegister)}),
       operation(NdOp::INT_SUB, r(AddressRegister), {r(AddressRegister), c(8)}),
       operation(NdOp::INT_XOR, r(ValueRegister), {r(InputRegister), c(29)}),
       operation(NdOp::STORE, {}, {r(AddressRegister), r(ValueRegister)}),
       operation(NdOp::LOAD, r(ResultRegister), {r(AddressRegister)}),
       operation(NdOp::INT_ADD, r(ResultRegister),
                 {r(ResultRegister), r(SelectorRegister)}),
       ret()});
  Provider.add(Route, ConsumerOps);
  return Provider;
}

SpecializationOptions bankOptions(bool SlotHint, bool RegisterHint) {
  SpecializationOptions Options;
  Options.FrameBaseRegister = symbolic::SymRegisterRange{FrameRegister, 8};
  if (SlotHint)
    Options.ControlFrameSlots = {{SelectorSlot, 8}};
  if (RegisterHint)
    Options.ControlRegisters = {{SelectorRegister, 8}};
  return Options;
}

constexpr unsigned MemoryCursorPositions = 17;
constexpr int64_t MemoryCursorSlot = -24;

BankProvider makeMemoryCursorProgram(bool UnboundedOffset = false) {
  BankProvider Provider;
  // The table's addresses vary, but every record selects the same safe slot.
  // Relations can therefore establish one affine write address until the
  // cursor's 17 positions exceed the ordinary immutable-read address limit.
  // The payload and accumulated return value remain runtime data throughout.
  constexpr uint64_t DataDisplacement = uint64_t{0} - 96;
  for (unsigned I = 0; I < MemoryCursorPositions; ++I)
    for (unsigned Byte = 0; Byte < 8; ++Byte)
      Provider.Image[TableBase + I * 8 + Byte] = DataDisplacement >> (8 * Byte);
  Provider.add(Entry,
               {operation(NdOp::INT_ADD, r(SelectorAddressRegister),
                          {r(FrameRegister), c(MemoryCursorSlot)}),
                operation(NdOp::COPY, r(SelectorRegister), {c(0)}),
                operation(NdOp::COPY, r(ResultRegister), {c(0)}),
                operation(NdOp::STORE, {},
                          {r(SelectorAddressRegister), r(SelectorRegister)}),
                jump(Route)});
  Provider.add(Route, {operation(NdOp::LOAD, r(SelectorRegister),
                                 {r(SelectorAddressRegister)}),
                       jump(Reload)});
  Provider.add(Reload, {jump(Dispatch)});
  Provider.add(
      Dispatch,
      {operation(NdOp::INT_MULT, r(AddressRegister),
                 {r(SelectorRegister), c(8)}),
       operation(NdOp::INT_ADD, r(AddressRegister),
                 {r(AddressRegister), c(TableBase)}),
       UnboundedOffset
           ? operation(NdOp::COPY, r(ValueRegister), {r(InputRegister)})
           : operation(NdOp::LOAD, r(ValueRegister), {r(AddressRegister)}),
       operation(NdOp::INT_ADD, r(PayloadAddressRegister),
                 {r(FrameRegister), r(ValueRegister)}),
       operation(NdOp::INT_ADD, r(ValueRegister),
                 {r(InputRegister), r(SelectorRegister)}),
       operation(NdOp::STORE, {},
                 {r(PayloadAddressRegister), r(ValueRegister)}),
       operation(NdOp::LOAD, r(ValueRegister), {r(PayloadAddressRegister)}),
       operation(NdOp::INT_ADD, r(ResultRegister),
                 {r(ResultRegister), r(ValueRegister)}),
       operation(NdOp::INT_ADD, r(SelectorRegister),
                 {r(SelectorRegister), c(1)}),
       operation(NdOp::STORE, {},
                 {r(SelectorAddressRegister), r(SelectorRegister)}),
       operation(NdOp::INT_LESS, r(ConditionRegister, 1),
                 {r(SelectorRegister), c(MemoryCursorPositions)}),
       operation(NdOp::COND_BR, {}, {c(Route), r(ConditionRegister, 1)})},
      Exit);
  Provider.add(Exit, {ret()});
  return Provider;
}

SpecializationOptions memoryCursorOptions(bool ManualCursor, bool Automatic) {
  auto Options = bankOptions(false, ManualCursor);
  Options.RequireRestoredFrameAtReturn = true;
  Options.DiscoverControlState = Automatic;
  return Options;
}

constexpr int64_t FirstMaskSlot = -48;
constexpr int64_t SecondMaskSlot = -40;
constexpr int64_t DerivedDisplacementSlot = -32;

BankProvider makeFiniteDependencyProgram() {
  BankProvider Provider;
  for (unsigned I = 0; I < 4; ++I) {
    const uint64_t Displacement = uint64_t{0} - 144 + I * 8;
    for (unsigned Byte = 0; Byte < 8; ++Byte)
      Provider.Image[TableBase + I * 8 + Byte] = Displacement >> (8 * Byte);
  }
  Provider.add(
      Entry,
      {operation(NdOp::INT_ADD, r(SelectorAddressRegister),
                 {r(FrameRegister), c(FirstMaskSlot)}),
       operation(NdOp::INT_ADD, r(PayloadAddressRegister),
                 {r(FrameRegister), c(SecondMaskSlot)}),
       operation(NdOp::INT_ADD, r(AddressRegister),
                 {r(FrameRegister), c(DerivedDisplacementSlot)}),
       operation(NdOp::INT_AND, r(ValueRegister), {r(InputRegister), c(1)}),
       operation(NdOp::STORE, {},
                 {r(SelectorAddressRegister), r(ValueRegister)}),
       operation(NdOp::INT_XOR, r(ValueRegister), {r(ValueRegister), c(1)}),
       operation(NdOp::STORE, {},
                 {r(PayloadAddressRegister), r(ValueRegister)}),
       jump(Route)});
  Provider.add(
      Route,
      {operation(NdOp::LOAD, r(ValueRegister), {r(SelectorAddressRegister)}),
       operation(NdOp::LOAD, r(CounterRegister), {r(PayloadAddressRegister)}),
       operation(NdOp::INT_AND, r(ValueRegister), {r(ValueRegister), c(1)}),
       operation(NdOp::INT_AND, r(CounterRegister), {r(CounterRegister), c(1)}),
       operation(NdOp::INT_ADD, r(ValueRegister),
                 {r(ValueRegister), r(CounterRegister)}),
       operation(NdOp::INT_SUB, r(ValueRegister), {r(ValueRegister), c(1)}),
       operation(NdOp::STORE, {}, {r(AddressRegister), r(ValueRegister)}),
       jump(Reload)});
  Provider.add(
      Reload,
      {operation(NdOp::LOAD, r(BankRegister), {r(AddressRegister)}),
       operation(NdOp::INT_AND, r(BankRegister), {r(BankRegister), c(7)}),
       jump(Dispatch)});
  Provider.add(Dispatch, {operation(NdOp::INT_AND, r(SelectorRegister),
                                    {r(BankRegister), c(3)}),
                          jump(HandlerBase)});
  Provider.add(
      HandlerBase,
      {operation(NdOp::INT_MULT, r(AddressRegister),
                 {r(SelectorRegister), c(8)}),
       operation(NdOp::INT_ADD, r(AddressRegister),
                 {r(AddressRegister), c(TableBase)}),
       operation(NdOp::LOAD, r(ValueRegister), {r(AddressRegister)}),
       operation(NdOp::INT_ADD, r(AddressRegister),
                 {r(FrameRegister), r(ValueRegister)}),
       operation(NdOp::INT_XOR, r(ValueRegister), {r(InputRegister), c(83)}),
       operation(NdOp::STORE, {}, {r(AddressRegister), r(ValueRegister)}),
       operation(NdOp::LOAD, r(ResultRegister), {r(AddressRegister)}), ret()});
  return Provider;
}

BankProvider makeCrossPhaseBitDemandProgram(unsigned TransparentPhases = 0) {
  auto Provider = makeFiniteDependencyProgram();
  constexpr va_t PrefixReload = 0x220;
  constexpr va_t PrefixDispatch = 0x240;
  constexpr va_t PrefixTable = 0x6800;
  constexpr uint64_t PrefixSlotAddress = 0x308;
  constexpr uint64_t PrefixContribution = 0x310;
  llvm::SmallVector<LowOp, 28> ProducerOps{operation(
      NdOp::COPY, r(FirstSelectorRegister), {r(FirstProducerRegister)})};
  for (unsigned I = 1; I < FirstProducerInputs; ++I)
    ProducerOps.push_back(operation(
        NdOp::INT_XOR, r(FirstSelectorRegister),
        {r(FirstSelectorRegister), r(FirstProducerRegister + I * 8)}));
  ProducerOps.append(
      {operation(NdOp::INT_AND, r(FirstSelectorRegister),
                 {r(FirstSelectorRegister), c(15)}),
       operation(NdOp::INT_MULT, r(FirstSelectorRegister),
                 {r(FirstSelectorRegister), c(16)}),
       operation(NdOp::INT_SUB, r(PrefixSlotAddress),
                 {r(FrameRegister), c(160)}),
       operation(NdOp::STORE, {},
                 {r(PrefixSlotAddress), r(FirstSelectorRegister)}),
       jump(PrefixReload)});
  Provider.add(FirstPhaseEntry, ProducerOps);
  Provider.add(PrefixReload, {operation(NdOp::LOAD, r(FirstSelectorRegister),
                                        {r(PrefixSlotAddress)}),
                              jump(PrefixDispatch)});
  Provider.add(
      PrefixDispatch,
      {operation(NdOp::INT_AND, r(ValueRegister),
                 {r(FirstSelectorRegister), c(1)}),
       operation(NdOp::INT_MULT, r(AddressRegister), {r(ValueRegister), c(8)}),
       operation(NdOp::INT_ADD, r(AddressRegister),
                 {r(AddressRegister), c(PrefixTable)}),
       operation(NdOp::LOAD, r(ValueRegister), {r(AddressRegister)}),
       operation(NdOp::INDIR_BR, {}, {r(ValueRegister)})});
  constexpr va_t Handler = 0x900;
  for (unsigned Byte = 0; Byte < 8; ++Byte)
    Provider.Image[PrefixTable + Byte] = Handler >> (8 * Byte);
  // There is no second table record. Recovery must prove that the spilled
  // byte's low bit is zero, while preserving its observable high nibble.
  Provider.add(Handler, {operation(NdOp::INT_ADD, r(PrefixContribution),
                                   {r(FirstSelectorRegister), c(9)}),
                         jump(Entry)});
  auto ConsumerOps = Provider.Code.at(HandlerBase).Ops;
  ConsumerOps.insert(ConsumerOps.end() - 1,
                     operation(NdOp::INT_ADD, r(ResultRegister),
                               {r(ResultRegister), r(PrefixContribution)}));
  Provider.Code.erase(HandlerBase);
  Provider.add(HandlerBase, ConsumerOps);
  insertTransparentPhases(Provider, PrefixReload, PrefixDispatch,
                          TransparentPhases);
  return Provider;
}

SpecializationOptions finiteDependencyOptions(bool Manual, bool Automatic) {
  auto Options = bankOptions(false, false);
  Options.RequireRestoredFrameAtReturn = true;
  Options.DiscoverControlState = Automatic;
  if (Manual) {
    Options.ControlRegisters = {{BankRegister, 8}, {SelectorRegister, 8}};
    Options.ControlFrameSlots = {
        {FirstMaskSlot, 8}, {SecondMaskSlot, 8}, {DerivedDisplacementSlot, 8}};
  }
  return Options;
}

// Small byte-based concrete interpreter for this corpus only. It deliberately
// avoids symbolic execution and rejects missing runtime bytes or opcodes.
std::optional<uint64_t>
run(const LowFunc &Function, uint64_t Input, uint64_t Limit,
    const std::map<uint64_t, uint64_t> &ExtraRegisters = {},
    llvm::endianness Order = llvm::endianness::little) {
  using Key = std::pair<VnodeSpace, uint64_t>;
  std::map<Key, uint8_t> Values;
  std::map<uint64_t, uint8_t> Memory;
  std::map<int, const LowBlock *> Blocks;
  for (const auto &Block : Function.Blocks)
    Blocks.emplace(Block.Id, &Block);
  bool Valid = true;
  const auto ByteShift = [&](unsigned Byte, unsigned Bytes) {
    return 8 * (Order == llvm::endianness::little ? Byte : Bytes - Byte - 1);
  };
  const auto Write = [&](NdVar Destination, uint64_t Value) {
    for (unsigned I = 0; I < Destination.Size; ++I)
      Values[{Destination.Space, Destination.Offset + I}] =
          Value >> ByteShift(I, Destination.Size);
  };
  const auto Read = [&](NdVar Value) {
    uint64_t Result = Value.isConst() ? Value.Offset : 0;
    if (!Value.isConst())
      for (unsigned I = 0; I < Value.Size; ++I) {
        const auto At = Values.find({Value.Space, Value.Offset + I});
        if (At == Values.end()) {
          Valid = false;
          return uint64_t{0};
        }
        Result |= uint64_t{At->second} << ByteShift(I, Value.Size);
      }
    return Value.Size == 8 ? Result
                           : Result & ((uint64_t{1} << (Value.Size * 8)) - 1);
  };
  Write(r(FrameRegister), 0xa000);
  Write(r(InputRegister), Input);
  Write(r(LimitRegister), Limit);
  for (const auto &[Register, Value] : ExtraRegisters)
    Write(r(Register), Value);
  int Current = Function.Blocks.front().Id;
  for (unsigned Step = 0; Step < 10000; ++Step) {
    const auto Found = Blocks.find(Current);
    if (Found == Blocks.end())
      return std::nullopt;
    const auto &Block = *Found->second;
    std::optional<int> Next;
    for (const LowOp &Op : Block.Ops) {
      const auto A = [&] { return Read(Op.Inputs[0]); };
      const auto B = [&] { return Read(Op.Inputs[1]); };
      switch (Op.Opcode) {
      case NdOp::COPY:
      case NdOp::INT_ZEXT:
        Write(Op.Output, A());
        break;
      case NdOp::INT_ADD:
        Write(Op.Output, A() + B());
        break;
      case NdOp::INT_SUB:
        Write(Op.Output, A() - B());
        break;
      case NdOp::INT_MULT:
        Write(Op.Output, A() * B());
        break;
      case NdOp::INT_AND:
        Write(Op.Output, A() & B());
        break;
      case NdOp::INT_OR:
        Write(Op.Output, A() | B());
        break;
      case NdOp::INT_XOR:
        Write(Op.Output, A() ^ B());
        break;
      case NdOp::INT_RIGHT: {
        const auto Shift = B();
        Write(Op.Output, Shift < Op.Inputs[0].Size * 8 ? A() >> Shift : 0);
        break;
      }
      case NdOp::INT_EQUAL:
        Write(Op.Output, A() == B());
        break;
      case NdOp::INT_LESS:
        Write(Op.Output, A() < B());
        break;
      case NdOp::BOOL_AND:
        Write(Op.Output, A() && B());
        break;
      case NdOp::BOOL_OR:
        Write(Op.Output, A() || B());
        break;
      case NdOp::SELECT:
        Write(Op.Output, A() ? B() : Read(Op.Inputs[2]));
        break;
      case NdOp::LOAD: {
        const auto Access = lowMemoryOperands(Op);
        const uint64_t Address = Read(*Access.Address);
        uint64_t Value = 0;
        for (unsigned Byte = 0; Byte < Access.AccessSize; ++Byte) {
          const auto At = Memory.find(Address + Byte);
          if (At == Memory.end())
            return std::nullopt;
          Value |= uint64_t{At->second} << ByteShift(Byte, Access.AccessSize);
        }
        Write(Op.Output, Value);
        break;
      }
      case NdOp::STORE: {
        const auto Access = lowMemoryOperands(Op);
        const auto Address = Read(*Access.Address);
        const auto Value = Read(*Access.StoredValue);
        for (unsigned Byte = 0; Byte < Access.AccessSize; ++Byte)
          Memory[Address + Byte] = Value >> ByteShift(Byte, Access.AccessSize);
        break;
      }
      case NdOp::BRANCH:
      case NdOp::COND_BR: {
        const auto Target = A();
        const bool Taken = Op.Opcode == NdOp::BRANCH || B();
        for (int Successor : Block.Succs)
          if ((Blocks.at(Successor)->StartAddr == Target) == Taken)
            Next = Successor;
        if (!Next && Block.Succs.size() == 1)
          Next = Block.Succs.front();
        break;
      }
      case NdOp::RETURN: {
        const auto Result = Read(r(ResultRegister));
        return Valid ? std::optional(Result) : std::nullopt;
      }
      case NdOp::NOP:
        break;
      default:
        ADD_FAILURE() << "unexpected residual opcode " << ndOpName(Op.Opcode);
        return std::nullopt;
      }
      if (!Valid)
        return std::nullopt;
    }
    if (!Next)
      return std::nullopt;
    Current = *Next;
  }
  return std::nullopt;
}

void expectNoPublication(const SpecializationResult &Result) {
  EXPECT_EQ(Result.Status, SpecializationStatus::UnresolvedControl)
      << Result.Diagnostic;
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_TRUE(Result.Origins.empty());
  EXPECT_TRUE(Result.Reads.empty());
}

SpecializationOptions automaticBankOptions() {
  auto Options = bankOptions(false, false);
  Options.DiscoverControlState = true;
  return Options;
}

void expectBudgetRefusal(const SpecializationResult &Result) {
  EXPECT_EQ(Result.Status, SpecializationStatus::BudgetExceeded)
      << Result.Diagnostic;
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_TRUE(Result.Origins.empty());
  EXPECT_TRUE(Result.Reads.empty());
}
// Independent finite fields can exceed the joint tuple bound without either
// field exceeding its own bound. The business field must remain observable.
BankProvider makeMarginalDispatchProgram(bool LatePredecessor = false) {
  BankProvider Provider;
  const auto Producer = [&](va_t At, va_t TargetBase, va_t Next) {
    Provider.add(
        At,
        {operation(NdOp::INT_AND, r(AddressRegister), {r(InputRegister), c(1)}),
         operation(NdOp::INT_MULT, r(AddressRegister),
                   {r(AddressRegister), c(32)}),
         operation(NdOp::INT_ADD, r(AddressRegister),
                   {r(AddressRegister), c(TargetBase)}),
         operation(NdOp::INT_AND, r(ValueRegister), {r(LimitRegister), c(3)}),
         operation(NdOp::INT_MULT, r(ValueRegister), {r(ValueRegister), c(2)}),
         operation(NdOp::INT_ADD, r(ValueRegister), {r(ValueRegister), c(1)}),
         jump(Next)});
  };
  if (LatePredecessor) {
    Provider.add(
        Entry,
        {operation(NdOp::INT_AND, r(ConditionRegister),
                   {r(UnknownPointerRegister), c(1)}),
         operation(NdOp::COND_BR, {}, {c(Latch), r(ConditionRegister, 1)})},
        Route);
    Producer(Route, HandlerBase, Dispatch);
    Producer(Latch, HandlerBase + 32, Reload);
    Provider.add(Reload, {jump(Exit)});
    Provider.add(Exit, {jump(Dispatch)});
  } else {
    Producer(Entry, HandlerBase, Dispatch);
  }
  Provider.add(Dispatch, {operation(NdOp::INDIR_BR, {}, {r(AddressRegister)})});
  for (unsigned Lane = 0; Lane < (LatePredecessor ? 3u : 2u); ++Lane)
    Provider.add(HandlerBase + Lane * 32,
                 {operation(NdOp::INT_ADD, r(ResultRegister),
                            {r(ValueRegister), c(17 + 11 * Lane)}),
                  ret()});
  return Provider;
}

SpecializationOptions marginalDispatchOptions() {
  SpecializationOptions Options;
  Options.ControlRegisters = {{AddressRegister, 8}, {ValueRegister, 8}};
  Options.MaxControlTuples = 4;
  Options.MaxContextsPerAddress = 1;
  return Options;
}

BankProvider makeMixedCarrierProgram(bool Frame, bool Bounded = true,
                                     bool HighSelector = true) {
  BankProvider Provider;
  std::vector<LowOp> Producer{
      operation(NdOp::INT_AND, r(ValueRegister),
                {r(InputRegister), c(Bounded ? 1 : UINT32_MAX)}),
      operation(NdOp::INT_MULT, r(ValueRegister),
                {r(ValueRegister), c(HighSelector ? uint64_t{1} << 32 : 1)}),
      operation(NdOp::INT_AND, r(AddressRegister),
                {r(LimitRegister), c(UINT32_MAX)}),
      operation(NdOp::INT_MULT, r(AddressRegister),
                {r(AddressRegister), c(HighSelector ? 1 : uint64_t{1} << 32)}),
      operation(NdOp::INT_XOR, r(ValueRegister),
                {r(ValueRegister), r(AddressRegister)})};
  if (Frame) {
    Producer.push_back(operation(NdOp::INT_ADD, r(SelectorAddressRegister),
                                 {r(FrameRegister), c(-40)}));
    Producer.push_back(operation(
        NdOp::STORE, {}, {r(SelectorAddressRegister), r(ValueRegister)}));
  }
  Producer.push_back(jump(Route));
  Provider.add(Entry, Producer);
  std::vector<LowOp> Consumer;
  if (Frame)
    Consumer.push_back(
        operation(NdOp::LOAD, r(ValueRegister), {r(SelectorAddressRegister)}));
  Consumer.push_back(operation(
      HighSelector ? NdOp::INT_RIGHT : NdOp::INT_AND, r(AddressRegister),
      {r(ValueRegister), c(HighSelector ? 32 : UINT32_MAX)}));
  Consumer.push_back(operation(NdOp::INT_MULT, r(AddressRegister),
                               {r(AddressRegister), c(32)}));
  Consumer.push_back(operation(NdOp::INT_ADD, r(AddressRegister),
                               {r(AddressRegister), c(HandlerBase)}));
  Consumer.push_back(operation(NdOp::INDIR_BR, {}, {r(AddressRegister)}));
  Provider.add(Route, Consumer);
  for (unsigned Selector = 0; Selector < 2; ++Selector)
    Provider.add(HandlerBase + 32 * Selector,
                 {operation(NdOp::INT_XOR, r(ResultRegister),
                            {r(ValueRegister), c(91 + Selector)}),
                  ret()});
  return Provider;
}

SpecializationOptions mixedCarrierOptions(bool Frame, llvm::endianness Order) {
  SpecializationOptions Options;
  Options.ByteOrder = Order;
  if (Frame) {
    Options.FrameBaseRegister = symbolic::SymRegisterRange{FrameRegister, 8};
    Options.ControlFrameSlots = {{-40, 8}};
  } else {
    Options.ControlRegisters = {{ValueRegister, 8}};
  }
  return Options;
}

} // namespace

TEST(ControlStateRecovery, MixedCarrierKeepsFiniteSubwordAndRuntimePayload) {
  for (auto Packing : {NdOp::INT_XOR, NdOp::INT_ADD})
    for (bool Frame : {false, true})
      for (bool High : {false, true})
        for (bool RootPayload : {false, true})
          for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
            SCOPED_TRACE(static_cast<unsigned>(Packing));
            SCOPED_TRACE(Frame);
            SCOPED_TRACE(High);
            SCOPED_TRACE(RootPayload);
            auto Provider = makeMixedCarrierProgram(Frame, true, High);
            Provider.Code.at(Entry).Ops[4].Opcode = Packing;
            auto Options = mixedCarrierOptions(Frame, Order);
            if (RootPayload) {
              auto Producer = Provider.Code.at(Entry).Ops;
              Producer[2].Inputs[0] = r(FrameRegister);
              Provider.Code.erase(Entry);
              Provider.add(Entry, Producer);
              Options.FrameBaseRegister =
                  symbolic::SymRegisterRange{FrameRegister, 8};
            }
            const auto Result =
                specializeInterpreter(Provider, {Entry}, Options);
            ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
            for (uint64_t Input : {0ULL, 1ULL, 18ULL, ~0ULL})
              for (uint64_t Payload :
                   {0ULL, 1ULL, 0x123456789abcdef0ULL, ~0ULL}) {
                const auto Mixed =
                    High ? ((Input & 1) << 32) | (Payload & UINT32_MAX)
                         : (Input & 1) | ((Payload & UINT32_MAX) << 32);
                EXPECT_EQ(run(Result.Residual, Input, Payload,
                              {{FrameRegister, Payload}}, Order),
                          Mixed ^ (91 + (Input & 1)));
              }
          }
}

TEST(ControlStateRecovery,
     FiniteSubwordCannotHideAMissingTargetOrFreeSelector) {
  for (bool Frame : {false, true})
    for (bool Missing : {false, true}) {
      auto Provider = makeMixedCarrierProgram(Frame, Missing);
      if (Missing)
        Provider.Code.erase(HandlerBase + 32);
      const auto Result = specializeInterpreter(
          Provider, {Entry},
          mixedCarrierOptions(Frame, llvm::endianness::little));
      EXPECT_EQ(Result.Status, Missing
                                   ? SpecializationStatus::Unsupported
                                   : SpecializationStatus::UnresolvedControl)
          << Result.Diagnostic;
      EXPECT_TRUE(Result.Residual.Blocks.empty());
      EXPECT_TRUE(Result.Origins.empty());
      EXPECT_TRUE(Result.Reads.empty());
    }
}

TEST(ControlStateRecovery, AdditiveCarrierRetainsReachableCarryTargets) {
  for (bool Frame : {false, true}) {
    auto Provider = makeMixedCarrierProgram(Frame);
    auto Producer = Provider.Code.at(Entry).Ops;
    Producer[2].Inputs[0] = r(FrameRegister);
    Producer[4].Opcode = NdOp::INT_ADD;
    Producer.insert(
        Producer.begin() + 5,
        operation(NdOp::INT_ADD, r(ValueRegister), {r(ValueRegister), c(1)}));
    Provider.Code.erase(Entry);
    Provider.add(Entry, Producer);
    auto Options = mixedCarrierOptions(Frame, llvm::endianness::little);
    Options.FrameBaseRegister = symbolic::SymRegisterRange{FrameRegister, 8};
    // Low root bits all one and selector one carry into the missing third
    // target. The upper selector cannot be reused as a two-value domain.
    const auto Result = specializeInterpreter(Provider, {Entry}, Options);
    EXPECT_FALSE(Result.complete());
    EXPECT_TRUE(Result.Residual.Blocks.empty());
    EXPECT_TRUE(Result.Origins.empty());
    EXPECT_TRUE(Result.Reads.empty());
  }
}

TEST(ControlStateRecovery, LaterPredecessorInvalidatesFiniteSubwordFacts) {
  for (unsigned Change : {0U, 1U, 2U}) {
    auto Provider = makeMixedCarrierProgram(false);
    auto Producer = Provider.Code.at(Entry).Ops;
    Provider.Code.erase(Entry);
    Provider.add(
        Entry,
        {operation(NdOp::INT_AND, r(ConditionRegister),
                   {r(UnknownPointerRegister), c(1)}),
         operation(NdOp::COND_BR, {}, {c(Entry + 1), r(ConditionRegister, 1)})},
        Entry + 2);
    Provider.add(Entry + 1, Producer);
    if (Change == 1) {
      Producer = {operation(NdOp::COPY, r(ValueRegister), {r(LimitRegister)}),
                  jump(Entry + 3)};
    } else if (Change == 2) {
      Producer = makeMixedCarrierProgram(false, true, false).Code.at(Entry).Ops;
      Producer.back() = jump(Entry + 3);
    } else {
      Producer.insert(Producer.end() - 1,
                      operation(NdOp::INT_ADD, r(ValueRegister),
                                {r(ValueRegister), c(uint64_t{2} << 32)}));
      Producer.back() = jump(Entry + 3);
    }
    Provider.add(Entry + 2, Producer);
    Provider.add(Entry + 3, {jump(Entry + 4)});
    Provider.add(Entry + 4, {jump(Route)});
    auto Options = mixedCarrierOptions(false, llvm::endianness::little);
    Options.MaxContextsPerAddress = 1;
    const auto Incomplete = specializeInterpreter(Provider, {Entry}, Options);
    EXPECT_EQ(Incomplete.Status, Change
                                     ? SpecializationStatus::UnresolvedControl
                                     : SpecializationStatus::Unsupported)
        << Incomplete.Diagnostic;
    EXPECT_TRUE(Incomplete.Residual.Blocks.empty());
    EXPECT_TRUE(Incomplete.Origins.empty());
    EXPECT_TRUE(Incomplete.Reads.empty());
    if (!Change) {
      for (unsigned Selector : {2U, 3U})
        Provider.add(HandlerBase + 32 * Selector,
                     {operation(NdOp::INT_XOR, r(ResultRegister),
                                {r(ValueRegister), c(91 + Selector)}),
                      ret()});
      const auto Result = specializeInterpreter(Provider, {Entry}, Options);
      ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
      for (uint64_t Later : {0ULL, 1ULL})
        for (uint64_t Input : {0ULL, 1ULL, ~0ULL}) {
          const auto Selector = (Input & 1) + (Later ? 0 : 2);
          EXPECT_EQ(run(Result.Residual, Input, 0xdeadbeef,
                        {{UnknownPointerRegister, Later}}),
                    ((Selector << 32) | 0xdeadbeef) ^ (91 + Selector));
        }
    }
  }
}

TEST(ControlStateRecovery, FiniteSubwordHandlesNarrowCarriersInBothByteOrders) {
  for (uint16_t Bytes : {uint16_t{2}, uint16_t{4}})
    for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
      const uint64_t Half = Bytes * 4;
      const uint64_t Scale = uint64_t{1} << Half;
      const uint64_t InputByte =
          Order == llvm::endianness::little ? 0 : 8 - Bytes;
      BankProvider Provider;
      Provider.add(
          Entry,
          {operation(NdOp::INT_AND, r(ValueRegister, Bytes),
                     {r(InputRegister + InputByte, Bytes), c(1, Bytes)}),
           operation(NdOp::INT_MULT, r(ValueRegister, Bytes),
                     {r(ValueRegister, Bytes), c(Scale, Bytes)}),
           operation(
               NdOp::INT_AND, r(AddressRegister, Bytes),
               {r(LimitRegister + InputByte, Bytes), c(Scale - 1, Bytes)}),
           operation(NdOp::INT_XOR, r(ValueRegister, Bytes),
                     {r(ValueRegister, Bytes), r(AddressRegister, Bytes)}),
           jump(Route)});
      Provider.add(Route,
                   {operation(NdOp::INT_ZEXT, r(AddressRegister),
                              {r(ValueRegister, Bytes)}),
                    operation(NdOp::INT_RIGHT, r(AddressRegister),
                              {r(AddressRegister), c(Half)}),
                    operation(NdOp::INT_MULT, r(AddressRegister),
                              {r(AddressRegister), c(32)}),
                    operation(NdOp::INT_ADD, r(AddressRegister),
                              {r(AddressRegister), c(HandlerBase)}),
                    operation(NdOp::INDIR_BR, {}, {r(AddressRegister)})});
      for (unsigned Selector : {0U, 1U})
        Provider.add(HandlerBase + 32 * Selector,
                     {operation(NdOp::INT_ZEXT, r(ResultRegister),
                                {r(ValueRegister, Bytes)}),
                      ret()});
      SpecializationOptions Options;
      Options.ByteOrder = Order;
      Options.ControlRegisters = {{ValueRegister, Bytes}};
      const auto Result = specializeInterpreter(Provider, {Entry}, Options);
      ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
      for (uint64_t Input : {0ULL, 1ULL, 18ULL, ~0ULL})
        for (uint64_t Payload : {0ULL, 1ULL, ~0ULL})
          EXPECT_EQ(run(Result.Residual, Input, Payload, {}, Order),
                    (Input & 1) * Scale + (Payload & (Scale - 1)));
    }
}

TEST(ControlStateRecovery, FiniteSubwordSharesQueryBudgetsAndRejectsUnknown) {
  auto Provider = makeMixedCarrierProgram(false);
  auto Options = mixedCarrierOptions(false, llvm::endianness::little);
  const auto Complete = specializeInterpreter(Provider, {Entry}, Options);
  ASSERT_TRUE(Complete.complete()) << Complete.Diagnostic;
  ASSERT_GT(Complete.SolverQueries, 0U);
  Options.MaxSolverQueries = Complete.SolverQueries;
  EXPECT_TRUE(specializeInterpreter(Provider, {Entry}, Options).complete());
  --Options.MaxSolverQueries;
  const auto Short = specializeInterpreter(Provider, {Entry}, Options);
  expectBudgetRefusal(Short);
  EXPECT_LE(Short.SolverQueries, Options.MaxSolverQueries);
  Options.MaxSolverQueries = 4096;
  Options.MaxSolverGates = 1;
  expectBudgetRefusal(specializeInterpreter(Provider, {Entry}, Options));
}

TEST(ControlStateRecovery, AutomaticDiscoveryRetainsFiniteSubword) {
  for (bool Frame : {false, true}) {
    auto Provider = makeMixedCarrierProgram(Frame);
    auto Consumer = Provider.Code.at(Route).Ops;
    const auto Index = Frame ? 1U : 0U;
    // The range comparison demands the complete carrier. Its arbitrary low
    // payload must not hide the finite upper half across the block edge.
    Consumer.insert(
        Consumer.begin() + Index + 1,
        {operation(NdOp::INT_LESS, r(ConditionRegister, 1),
                   {r(ValueRegister), c(uint64_t{1} << 32)}),
         operation(NdOp::SELECT, r(AddressRegister),
                   {r(ConditionRegister, 1), c(0), r(AddressRegister)})});
    Provider.Code.erase(Route);
    Provider.add(Route, Consumer);
    SpecializationOptions Options;
    Options.DiscoverControlState = true;
    Options.FrameBaseRegister = symbolic::SymRegisterRange{FrameRegister, 8};
    const auto Result = specializeInterpreter(Provider, {Entry}, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    EXPECT_GT(Result.DiscoveredControlFields, 0U);
    for (uint64_t Input : {0ULL, 1ULL, 18ULL, ~0ULL})
      for (uint64_t Payload : {0ULL, 1ULL, 0x123456789abcdef0ULL, ~0ULL})
        EXPECT_EQ(run(Result.Residual, Input, Payload),
                  (((Input & 1) << 32) | (Payload & UINT32_MAX)) ^
                      (91 + (Input & 1)));
  }
}

TEST(ControlStateRecovery, UndemandedManualFieldsDoNotSearchSubwords) {
  auto Provider = makeMixedCarrierProgram(false);
  Provider.Code.erase(Route);
  Provider.add(
      Route,
      {operation(NdOp::COPY, r(ResultRegister), {r(ValueRegister)}), ret()});
  SpecializationOptions Options;
  Options.DiscoverControlState = true;
  const auto Baseline = specializeInterpreter(Provider, {Entry}, Options);
  ASSERT_TRUE(Baseline.complete()) << Baseline.Diagnostic;
  Options.ControlRegisters = {{ValueRegister, 8}};
  // The existing whole-field attempt can consume one complete enumeration.
  // An extra subword search would exceed this shared budget.
  Options.MaxSolverQueries = Options.MaxControlTuples + 1;
  const auto WithHint = specializeInterpreter(Provider, {Entry}, Options);
  ASSERT_TRUE(WithHint.complete()) << WithHint.Diagnostic;
  EXPECT_LE(WithHint.SolverQueries, Options.MaxSolverQueries);
  EXPECT_EQ(WithHint.ControlRefinements, 0U);
  for (uint64_t Input : {0ULL, 1ULL, ~0ULL})
    for (uint64_t Payload : {0ULL, 1ULL, 0x123456789abcdef0ULL, ~0ULL})
      EXPECT_EQ(run(WithHint.Residual, Input, Payload),
                ((Input & 1) << 32) | (Payload & UINT32_MAX));
}

TEST(ControlStateRecovery, ConstantLeadingWindowDoesNotHideFiniteSelector) {
  auto Provider = makeMixedCarrierProgram(false);
  auto Producer = Provider.Code.at(Entry).Ops;
  Producer[1].Inputs[1] = c(uint64_t{1} << 16);
  Producer[2].Inputs[1] = c(UINT16_MAX);
  Producer.insert(Producer.end() - 1,
                  operation(NdOp::INT_OR, r(ValueRegister),
                            {r(ValueRegister), c(0x8765432100000000)}));
  Provider.Code.erase(Entry);
  Provider.add(Entry, Producer);
  auto Consumer = Provider.Code.at(Route).Ops;
  Consumer[0].Inputs[1] = c(16);
  Consumer.insert(Consumer.begin() + 1,
                  operation(NdOp::INT_AND, r(AddressRegister),
                            {r(AddressRegister), c(UINT16_MAX)}));
  Provider.Code.erase(Route);
  Provider.add(Route, Consumer);
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    const auto Result = specializeInterpreter(
        Provider, {Entry}, mixedCarrierOptions(false, Order));
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    for (uint64_t Input : {0ULL, 1ULL, 18ULL, ~0ULL})
      for (uint64_t Payload : {0ULL, 1ULL, 0x123456789abcdef0ULL, ~0ULL})
        EXPECT_EQ(run(Result.Residual, Input, Payload, {}, Order),
                  (0x8765432100000000 | ((Input & 1) << 16) |
                   (Payload & UINT16_MAX)) ^
                      (91 + (Input & 1)));
  }
}

TEST(ControlStateRecovery, JointOverflowKeepsCompleteControlMarginals) {
  auto Provider = makeMarginalDispatchProgram();
  const auto Result =
      specializeInterpreter(Provider, {Entry}, marginalDispatchOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_GT(Result.RelationalWidenings, 0u);
  for (uint64_t Input : {0ULL, 1ULL, 14ULL, 19ULL, ~0ULL})
    for (uint64_t Business : {0ULL, 1ULL, 2ULL, 3ULL, 0xabcdefULL, ~0ULL})
      EXPECT_EQ(run(Result.Residual, Input, Business),
                2 * (Business & 3) + 1 + 17 + 11 * (Input & 1));
}

TEST(ControlStateRecovery, ControlMarginalsDoNotHideMissingReachableTarget) {
  auto Provider = makeMarginalDispatchProgram();
  Provider.Code.erase(HandlerBase + 32);
  const auto Result =
      specializeInterpreter(Provider, {Entry}, marginalDispatchOptions());
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported)
      << Result.Diagnostic;
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_TRUE(Result.Origins.empty());
  EXPECT_TRUE(Result.Reads.empty());
}

TEST(ControlStateRecovery, ChangedMarginalRevisitsWidenedJoint) {
  auto Provider = makeMarginalDispatchProgram(true);
  const auto Result =
      specializeInterpreter(Provider, {Entry}, marginalDispatchOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_GT(Result.RelationalWidenings, 0u);
  for (uint64_t Later : {0ULL, 1ULL})
    for (uint64_t Input : {0ULL, 1ULL, 18ULL, ~0ULL})
      for (uint64_t Business : {0ULL, 1ULL, 2ULL, 3ULL, ~0ULL})
        EXPECT_EQ(run(Result.Residual, Input, Business,
                      {{UnknownPointerRegister, Later}}),
                  2 * (Business & 3) + 1 + 17 + 11 * ((Input & 1) + Later));
}

TEST(ControlStateRecovery, LaterMarginalCannotPublishEarlierPartialGraph) {
  auto Provider = makeMarginalDispatchProgram(true);
  Provider.Code.erase(HandlerBase + 64);
  const auto Result =
      specializeInterpreter(Provider, {Entry}, marginalDispatchOptions());
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported)
      << Result.Diagnostic;
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_TRUE(Result.Origins.empty());
  EXPECT_TRUE(Result.Reads.empty());
}

TEST(ControlStateRecovery, OverflowingMarginalDropsItsEntireTargetDomain) {
  auto Provider = makeMarginalDispatchProgram(true);
  auto Options = marginalDispatchOptions();
  Options.MaxControlTuples = 2;
  const auto Result = specializeInterpreter(Provider, {Entry}, Options);
  // Each predecessor has two targets, but their union has three. Retaining
  // either predecessor's bounded subset would omit a feasible transfer.
  expectNoPublication(Result);
}

TEST(ControlStateRecovery, IncompleteMarginalsCannotSupplyTargetWitnesses) {
  auto Provider = makeMarginalDispatchProgram();
  auto Options = marginalDispatchOptions();
  Options.MaxControlTuples = 1;
  const auto Result = specializeInterpreter(Provider, {Entry}, Options);
  expectNoPublication(Result);
}

TEST(ControlStateRecovery, AliasingStoreInvalidatesIndependentFrameMarginal) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    for (bool MayAlias : {false, true}) {
      auto Provider = makeMarginalDispatchProgram();
      auto ProducerOps = Provider.Code.at(Entry).Ops;
      ProducerOps.pop_back();
      ProducerOps.push_back(operation(NdOp::INT_ADD, r(SelectorAddressRegister),
                                      {r(FrameRegister), c(SelectorSlot)}));
      ProducerOps.push_back(operation(
          NdOp::STORE, {}, {r(SelectorAddressRegister), r(AddressRegister)}));
      ProducerOps.push_back(jump(Route));
      Provider.Code.erase(Entry);
      Provider.add(Entry, ProducerOps);
      llvm::SmallVector<LowOp, 2> RouteOps;
      if (MayAlias)
        RouteOps.push_back(
            operation(NdOp::STORE, {}, {r(UnknownPointerRegister), c(0)}));
      RouteOps.push_back(jump(Dispatch));
      Provider.add(Route, RouteOps);
      Provider.Code.erase(Dispatch);
      Provider.add(Dispatch,
                   {operation(NdOp::LOAD, r(AddressRegister),
                              {r(SelectorAddressRegister)}),
                    operation(NdOp::INDIR_BR, {}, {r(AddressRegister)})});
      auto Options = marginalDispatchOptions();
      Options.ByteOrder = Order;
      Options.FrameBaseRegister = symbolic::SymRegisterRange{FrameRegister, 8};
      Options.ControlRegisters = {{ValueRegister, 8}};
      Options.ControlFrameSlots = {{SelectorSlot, 8}};
      const auto Result = specializeInterpreter(Provider, {Entry}, Options);
      if (MayAlias) {
        expectNoPublication(Result);
        continue;
      }
      ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
      EXPECT_GT(Result.RelationalWidenings, 0u);
      for (uint64_t Input : {0ULL, 1ULL, ~0ULL})
        for (uint64_t Business : {0ULL, 1ULL, 2ULL, 3ULL, ~0ULL})
          EXPECT_EQ(run(Result.Residual, Input, Business, {}, Order),
                    2 * (Business & 3) + 1 + 17 + 11 * (Input & 1));
    }
  }
}

TEST(ControlStateRecovery, SpilledSelectorNeedsBothCurrentProjectionHints) {
  auto Provider = makeBankProgram(false);
  for (const auto [SlotHint, RegisterHint] :
       {std::pair{false, false}, std::pair{true, false},
        std::pair{false, true}})
    expectNoPublication(specializeInterpreter(
        Provider, {Entry}, bankOptions(SlotHint, RegisterHint)));

  auto Result =
      specializeInterpreter(Provider, {Entry}, bankOptions(true, true));
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(Result.Reads.size(), 4u);
  for (uint64_t Input : {0ULL, 1ULL, 2ULL, 3ULL, 19ULL, ~0ULL})
    EXPECT_EQ(run(Result.Residual, Input, 1),
              (Input ^ 45) + 3 + 11 * (Input & 3));
}

TEST(ControlStateRecovery,
     SpilledSelectorLoopKeepsBusinessCounterUnpartitioned) {
  auto Provider = makeBankProgram(true);
  expectNoPublication(
      specializeInterpreter(Provider, {Entry}, bankOptions(false, false)));
  auto Result =
      specializeInterpreter(Provider, {Entry}, bankOptions(true, true));
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_LT(Result.Contexts, 48u);
  EXPECT_LT(Result.NodeEvaluations, 192u);
  for (uint64_t Input : {0ULL, 1ULL, 7ULL, 22ULL, ~0ULL})
    for (uint64_t Limit : {1ULL, 2ULL, 5ULL, 19ULL}) {
      uint64_t Expected = 0;
      for (uint64_t Iteration = 0; Iteration < Limit; ++Iteration)
        Expected += (Input ^ 45) + 3 + 11 * ((Input + Iteration) & 3);
      EXPECT_EQ(run(Result.Residual, Input, Limit), Expected);
    }
}

TEST(ControlStateRecovery, HintsCannotInventBoundsForUnknownSelectorBytes) {
  for (auto Input :
       {SelectorInput::UnboundedScalar, SelectorInput::UnknownMemory}) {
    auto Provider = makeBankProgram(false, Input);
    expectNoPublication(
        specializeInterpreter(Provider, {Entry}, bankOptions(true, true)));
  }
}

TEST(ControlStateRecovery, DiscoversSpilledAndReloadedSelectorWithoutHints) {
  auto Provider = makeBankProgram(false);
  const auto Options = automaticBankOptions();
  const auto Result = specializeInterpreter(Provider, {Entry}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_GE(Result.DiscoveredControlFields, 2u);
  EXPECT_EQ(Result.DiscoveredContextFields, 0u);
  EXPECT_LE(Result.DiscoveredControlFields, Options.MaxControlFields);
  EXPECT_GT(Result.ControlRefinements, 0u);
  EXPECT_LE(Result.ControlRefinements, Options.MaxControlRefinements);
  EXPECT_GT(Result.DiscoveryVisits, 0u);
  EXPECT_LE(Result.DiscoveryVisits, Options.MaxDiscoveryVisits);
  for (uint64_t Input : {0ULL, 1ULL, 2ULL, 3ULL, 19ULL, ~0ULL})
    EXPECT_EQ(run(Result.Residual, Input, 1),
              (Input ^ 45) + 3 + 11 * (Input & 3));
}

TEST(ControlStateRecovery, AutomaticFieldsNeverPartitionLoopCounterContexts) {
  auto Provider = makeBankProgram(true);
  auto Options = automaticBankOptions();
  // The selector changes on every backedge and the business counter grows
  // without a static bound. Automatically retained fields must use relational
  // projection only: even two distinct contexts at one address are forbidden.
  Options.MaxContextsPerAddress = 1;
  const auto Result = specializeInterpreter(Provider, {Entry}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_GE(Result.DiscoveredControlFields, 2u);
  EXPECT_EQ(Result.DiscoveredContextFields, 0u);
  EXPECT_LE(Result.ControlRefinements, Options.MaxControlRefinements);
  EXPECT_LE(Result.DiscoveryVisits, Options.MaxDiscoveryVisits);
  for (uint64_t Input : {0ULL, 1ULL, 7ULL, ~0ULL})
    for (uint64_t Limit : {1ULL, 5ULL, 19ULL, 300ULL}) {
      uint64_t Expected = 0;
      for (uint64_t Iteration = 0; Iteration < Limit; ++Iteration)
        Expected += (Input ^ 45) + 3 + 11 * ((Input + Iteration) & 3);
      EXPECT_EQ(run(Result.Residual, Input, Limit), Expected);
    }
}

TEST(ControlStateRecovery,
     LongTransparentLoopPhasesDoNotConsumeOneRefinementEach) {
  auto Provider = makeLongControlLoop();
  const auto CheckOracle = [](const SpecializationResult &Result) {
    for (uint64_t High : {0ULL, 0x123456789abcdef0ULL, ~3ULL})
      for (uint64_t Lane = 0; Lane < 4; ++Lane)
        for (uint64_t Limit : {1ULL, 3ULL, 9ULL}) {
          const uint64_t Input = High | Lane;
          SCOPED_TRACE(Input);
          SCOPED_TRACE(Limit);
          uint64_t Expected = 0;
          for (uint64_t Iteration = 0; Iteration < Limit; ++Iteration)
            Expected += (Input ^ 45) + 3 + 11 * ((Input + Iteration) & 3);
          EXPECT_EQ(run(Result.Residual, Input, Limit), Expected);
        }
  };

  // Establish that the loop is finite-control recoverable before checking
  // discovery. Neither the payload nor the business counter is a hint.
  const auto Hinted =
      specializeInterpreter(Provider, {Entry}, bankOptions(true, true));
  ASSERT_TRUE(Hinted.complete()) << Hinted.Diagnostic;
  CheckOracle(Hinted);

  auto Options = automaticBankOptions();
  Options.MaxContextsPerAddress = 1;
  const auto Result = specializeInterpreter(Provider, {Entry}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(Result.DiscoveredContextFields, 0u);
  EXPECT_LE(Result.DiscoveredControlFields, Options.MaxControlFields);
  EXPECT_LE(Result.ControlRefinements, Options.MaxControlRefinements);
  EXPECT_LE(Result.DiscoveryVisits, Options.MaxDiscoveryVisits);
  CheckOracle(Result);
}

TEST(ControlStateRecovery, LongTransparentLoopCannotBoundUnknownSelectors) {
  for (auto Input :
       {SelectorInput::UnboundedScalar, SelectorInput::UnknownMemory}) {
    SCOPED_TRACE(static_cast<unsigned>(Input));
    auto Provider = makeLongControlLoop(Input);
    expectNoPublication(
        specializeInterpreter(Provider, {Entry}, bankOptions(true, true)));
    auto Options = automaticBankOptions();
    Options.MaxContextsPerAddress = 1;
    expectNoPublication(specializeInterpreter(Provider, {Entry}, Options));
  }
}

TEST(ControlStateRecovery, ProducerClosureReplaysCommittedTransferChains) {
  for (bool Stop : {false, true}) {
    auto Provider = makeLongControlLoop();
    auto Options = automaticBankOptions();
    Options.StopChainingAtRepeatedDestination = Stop;
    Options.MaxChainedTransfers = 3;
    Options.MaxContextsPerAddress = 1;
    const auto Result = specializeInterpreter(Provider, {Entry}, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    EXPECT_GT(Result.ControlRefinements, 0U);
    EXPECT_GT(Result.DiscoveryVisits, 0U);
    for (uint64_t Input : {0ULL, 1ULL, 17ULL, ~0ULL})
      for (uint64_t Limit : {1ULL, 5ULL, 13ULL}) {
        uint64_t Expected = 0;
        for (uint64_t I = 0; I != Limit; ++I)
          Expected += (Input ^ 45) + 3 + 11 * ((Input + I) & 3);
        EXPECT_EQ(run(Result.Residual, Input, Limit), Expected);
      }
    Options.MaxDiscoveryVisits = 1;
    expectBudgetRefusal(specializeInterpreter(Provider, {Entry}, Options));
  }
}

TEST(ControlStateRecovery, LongTransparentLoopKeepsRefinementAndWorkBudgets) {
  for (unsigned Budget = 0; Budget < 3; ++Budget) {
    SCOPED_TRACE(Budget);
    auto Provider = makeLongControlLoop();
    auto Options = automaticBankOptions();
    Options.MaxContextsPerAddress = 1;
    switch (Budget) {
    case 0:
      Options.MaxControlRefinements = 0;
      break;
    case 1:
      Options.MaxDiscoveryVisits = 1;
      break;
    case 2:
      Options.MaxOperations = 1;
      break;
    }
    expectBudgetRefusal(specializeInterpreter(Provider, {Entry}, Options));
  }
}

TEST(ControlStateRecovery, ProducerClosureChargesWorkBeforeAnotherRestart) {
  auto Provider = makeLongControlLoop();
  auto Options = automaticBankOptions();
  Options.MaxContextsPerAddress = 1;
  const auto Complete = specializeInterpreter(Provider, {Entry}, Options);
  ASSERT_TRUE(Complete.complete()) << Complete.Diagnostic;
  ASSERT_GT(Complete.ControlRefinements, 1u);

  // Stop after the first field has been installed, immediately before the
  // next dependency closure. This measures the same prefix of work instead
  // of relying on a fixed evaluator or discovery count.
  auto BoundaryOptions = Options;
  BoundaryOptions.MaxControlRefinements = 1;
  const auto Boundary =
      specializeInterpreter(Provider, {Entry}, BoundaryOptions);
  expectBudgetRefusal(Boundary);
  ASSERT_EQ(Boundary.ControlRefinements, BoundaryOptions.MaxControlRefinements);
  ASSERT_LT(Boundary.NodeEvaluations + 1, Complete.NodeEvaluations);
  ASSERT_LT(Boundary.EvaluatedOperations + 1, Complete.EvaluatedOperations);
  ASSERT_LT(Boundary.DiscoveryVisits + 1, Complete.DiscoveryVisits);

  for (unsigned Budget = 0; Budget < 3; ++Budget) {
    SCOPED_TRACE(Budget);
    auto Limited = Options;
    switch (Budget) {
    case 0:
      Limited.MaxNodeEvaluations = Boundary.NodeEvaluations + 1;
      break;
    case 1:
      Limited.MaxOperations = Boundary.EvaluatedOperations + 1;
      break;
    case 2:
      Limited.MaxDiscoveryVisits = Boundary.DiscoveryVisits + 1;
      break;
    }
    const auto Result = specializeInterpreter(Provider, {Entry}, Limited);
    expectBudgetRefusal(Result);
    // Closure work has begun, but no fresh fixed point has started. Discovery
    // may stop while building its predecessor graph, before a node replay.
    if (Budget == 2)
      EXPECT_GT(Result.DiscoveryVisits, Boundary.DiscoveryVisits);
    else
      EXPECT_GT(Result.NodeEvaluations, Boundary.NodeEvaluations);
    EXPECT_EQ(Result.ControlRefinements, Boundary.ControlRefinements);
    EXPECT_NE(Result.Diagnostic.find("last recovery frontier: at 0x"),
              std::string::npos);
  }
}

TEST(ControlStateRecovery, DiscoveryIgnoresUnrelatedControlRegisterReuse) {
  auto Provider = makeBankProgram(false);
  constexpr va_t BranchEntry = 0x400;
  constexpr va_t BusinessPhase = 0x420;
  constexpr va_t BypassPhase = 0x440;
  constexpr uint64_t FirstBusinessRegister = 0x1000;
  constexpr unsigned BusinessInputs = 17;
  Provider.add(BranchEntry,
               {operation(NdOp::INT_AND, r(ConditionRegister, 1),
                          {r(InputRegister, 1), c(1, 1)}),
                operation(NdOp::COND_BR, {},
                          {c(BusinessPhase), r(ConditionRegister, 1)})},
               BypassPhase);
  llvm::SmallVector<LowOp, 20> BusinessOps{
      operation(NdOp::COPY, r(SelectorRegister), {r(FirstBusinessRegister)})};
  for (unsigned I = 1; I < BusinessInputs; ++I)
    BusinessOps.push_back(
        operation(NdOp::INT_XOR, r(SelectorRegister),
                  {r(SelectorRegister), r(FirstBusinessRegister + I * 8)}));
  BusinessOps.push_back(jump(Entry));
  Provider.add(BusinessPhase, BusinessOps);
  Provider.add(BypassPhase, {operation(NdOp::COPY, r(SelectorRegister), {c(0)}),
                             jump(Entry)});

  auto Options = automaticBankOptions();
  Options.MaxControlFields = 4;
  // The business phase is visited before dispatch loses its selector. On a
  // retry the same register is tracked, but its 17-input value is overwritten
  // by Entry before any control use. Only demanded successor phases may ask
  // discovery to retain this register's producers.
  const auto Result = specializeInterpreter(Provider, {BranchEntry}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_GE(Result.DiscoveredControlFields, 2u);
  EXPECT_LE(Result.DiscoveredControlFields, Options.MaxControlFields);
  EXPECT_EQ(Result.DiscoveredContextFields, 0u);
  EXPECT_GT(Result.ControlRefinements, 0u);
  EXPECT_LE(Result.ControlRefinements, Options.MaxControlRefinements);
  EXPECT_LE(Result.DiscoveryVisits, Options.MaxDiscoveryVisits);
  for (uint64_t Seed : {0ULL, 71ULL, ~0ULL}) {
    std::map<uint64_t, uint64_t> ExtraRegisters;
    for (unsigned I = 0; I < BusinessInputs; ++I)
      ExtraRegisters.emplace(FirstBusinessRegister + I * 8,
                             (Seed + 19 * I) ^ (Seed >> I));
    for (uint64_t Input : {0ULL, 1ULL, 2ULL, 3ULL, 19ULL, ~0ULL})
      EXPECT_EQ(run(Result.Residual, Input, 1, ExtraRegisters),
                (Input ^ 45) + 3 + 11 * (Input & 3));
  }
}

TEST(ControlStateRecovery, AutomaticRelationsFollowSeparateControlLifetimes) {
  auto Provider = makeSeparateControlLifetimesProgram();
  auto Options = automaticBankOptions();
  Options.MaxContextsPerAddress = 1;
  // Two independent eight-value selectors need only eight tuples at a time.
  // Their 64-value product exceeds the unchanged 32-tuple default. The first
  // selector is never used in the loop, so keeping that product would discard
  // precision needed by the second selector for no semantic reason.
  ASSERT_EQ(Options.MaxControlTuples, 32u);
  const auto Result =
      specializeInterpreter(Provider, {FirstPhaseEntry}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_GE(Result.DiscoveredControlFields, 2u);
  EXPECT_EQ(Result.DiscoveredContextFields, 0u);
  EXPECT_LE(Result.ControlRefinements, Options.MaxControlRefinements);
  EXPECT_LE(Result.DiscoveryVisits, Options.MaxDiscoveryVisits);
  EXPECT_LE(Result.SolverQueries, Options.MaxSolverQueries);
  for (uint64_t Input = 0; Input < 64; ++Input)
    for (uint64_t Limit : {1ULL, 3ULL, 11ULL}) {
      uint64_t Expected = 0;
      for (uint64_t Iteration = 0; Iteration < Limit; ++Iteration)
        Expected += (Input ^ 45) + 3 + 11 * (((Input >> 3) + Iteration) & 7);
      EXPECT_EQ(run(Result.Residual, Input, Limit), Expected);
    }
}

TEST(ControlStateRecovery,
     AlreadyFiniteProducersDoNotStarveLaterControlDiscovery) {
  auto Provider = makeFiniteProducerProgram();
  auto Options = automaticBankOptions();
  Options.MaxContextsPerAddress = 1;
  ASSERT_EQ(Options.MaxControlFields, 16u);
  // The first masked selector is already provably finite. Expanding all 17
  // of its producers while the second phase still needs new fields exhausts
  // the field budget without adding needed precision to the first dispatch.
  const auto Result =
      specializeInterpreter(Provider, {FirstPhaseEntry}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_LE(Result.DiscoveredControlFields, Options.MaxControlFields);
  EXPECT_EQ(Result.DiscoveredContextFields, 0u);
  EXPECT_LE(Result.ControlRefinements, Options.MaxControlRefinements);
  EXPECT_LE(Result.DiscoveryVisits, Options.MaxDiscoveryVisits);
  EXPECT_LE(Result.SolverQueries, Options.MaxSolverQueries);
  EXPECT_EQ(Result.Reads.size(), 16u);
  for (uint64_t FirstSelector = 0; FirstSelector < 8; ++FirstSelector) {
    std::map<uint64_t, uint64_t> ExtraRegisters;
    uint64_t Combined = 0;
    for (unsigned I = 0; I + 1 < FirstProducerInputs; ++I) {
      const auto Value = uint64_t{0x9e3779b97f4a7c15} * (I + 1);
      Combined ^= Value;
      ExtraRegisters.emplace(FirstProducerRegister + I * 8, Value);
    }
    ExtraRegisters.emplace(FirstProducerRegister +
                               (FirstProducerInputs - 1) * 8,
                           Combined ^ FirstSelector);
    for (uint64_t Input : {0ULL, 7ULL, 8ULL, 19ULL, 63ULL, ~0ULL})
      for (uint64_t Limit : {1ULL, 11ULL}) {
        uint64_t Expected = 9 + 7 * FirstSelector;
        for (uint64_t Iteration = 0; Iteration < Limit; ++Iteration)
          Expected += (Input ^ 45) + 3 + 11 * (((Input >> 3) + Iteration) & 7);
        EXPECT_EQ(run(Result.Residual, Input, Limit, ExtraRegisters), Expected);
      }
  }
}

TEST(ControlStateRecovery, CompleteWideDomainsRetainCommonByteLanes) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (bool FrameField : {false, true}) {
      SCOPED_TRACE(Order == llvm::endianness::little ? "little" : "big");
      SCOPED_TRACE(FrameField ? "frame" : "register");
      auto Provider = makeCommonControlByteProgram(FrameField, Order);
      auto Options = bankOptions(false, !FrameField);
      Options.ByteOrder = Order;
      Options.RequireRestoredFrameAtReturn = true;
      Options.MaxContextsPerAddress = 1;
      if (FrameField)
        Options.ControlFrameSlots = {{WideControlSlot, 8}};
      // Neither whole word is constant. Only a complete two-value proof can
      // retain the low byte as zero and establish the exact frame write.
      const auto Result = specializeInterpreter(Provider, {Entry}, Options);
      ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
      EXPECT_EQ(Result.DiscoveredControlFields, 0u);
      for (uint64_t Input : {0ULL, 1ULL, 2ULL, 19ULL, ~0ULL})
        EXPECT_EQ(run(Result.Residual, Input, 1, {}, Order),
                  (Input ^ 29) + (Input == 0 ? 0x100 : 0));
    }
}

TEST(ControlStateRecovery, IncompleteWideDomainsDoNotInventCommonBytes) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (bool FrameField : {false, true})
      for (uint32_t TupleLimit : {1u, 2u}) {
        SCOPED_TRACE(Order == llvm::endianness::little ? "little" : "big");
        SCOPED_TRACE(FrameField ? "frame" : "register");
        SCOPED_TRACE(TupleLimit);
        auto Provider = makeCommonControlByteProgram(FrameField, Order, true);
        auto Options = bankOptions(false, !FrameField);
        Options.ByteOrder = Order;
        Options.RequireRestoredFrameAtReturn = true;
        Options.MaxControlTuples = TupleLimit;
        if (FrameField)
          Options.ControlFrameSlots = {{WideControlSlot, 8}};
        // The third value has low byte eight, which would make the write
        // overlap the entry return slot. A prefix of the domain is no proof.
        const auto Result = specializeInterpreter(Provider, {Entry}, Options);
        EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported)
            << Result.Diagnostic;
        EXPECT_TRUE(Result.Residual.Blocks.empty());
        EXPECT_TRUE(Result.Origins.empty());
        EXPECT_TRUE(Result.Reads.empty());
      }
}

TEST(ControlStateRecovery, AutomaticDiscoveryCannotInventUnboundedInputs) {
  for (auto Input :
       {SelectorInput::UnboundedScalar, SelectorInput::UnknownMemory}) {
    auto Provider = makeBankProgram(false, Input);
    expectNoPublication(
        specializeInterpreter(Provider, {Entry}, automaticBankOptions()));
  }
}

TEST(ControlStateRecovery, AutomaticRefinementBudgetsPublishNoPartialGraph) {
  for (unsigned Budget = 0; Budget < 4; ++Budget) {
    SCOPED_TRACE(Budget);
    auto Provider = makeBankProgram(false);
    auto Options = automaticBankOptions();
    switch (Budget) {
    case 0:
      Options.MaxControlRefinements = 0;
      break;
    case 1:
      Options.MaxDiscoveryVisits = 0;
      break;
    case 2:
      Options.MaxDiscoveryVisits = 1;
      break;
    case 3:
      Options.MaxControlFields = 1;
      break;
    }
    expectBudgetRefusal(specializeInterpreter(Provider, {Entry}, Options));
  }
}

TEST(ControlStateRecovery, BudgetDiagnosticRetainsLastRecoveryFrontier) {
  auto Provider = makeBankProgram(false);
  auto Options = automaticBankOptions();
  Options.DiscoverControlState = false;
  const auto Unresolved = specializeInterpreter(Provider, {Entry}, Options);
  ASSERT_EQ(Unresolved.Status, SpecializationStatus::UnresolvedControl)
      << Unresolved.Diagnostic;

  Options.DiscoverControlState = true;
  Options.MaxControlRefinements = 0;
  const auto Limited = specializeInterpreter(Provider, {Entry}, Options);
  expectBudgetRefusal(Limited);
  EXPECT_EQ(Limited.Diagnostic,
            "control-state discovery/refinement budget exhausted; last "
            "recovery frontier: " +
                Unresolved.Diagnostic);

  Options = automaticBankOptions();
  Options.MaxOperations = 1;
  const auto Early = specializeInterpreter(Provider, {Entry}, Options);
  expectBudgetRefusal(Early);
  EXPECT_EQ(Early.Diagnostic.find("last recovery frontier"), std::string::npos);

  const auto Complete =
      specializeInterpreter(Provider, {Entry}, automaticBankOptions());
  ASSERT_TRUE(Complete.complete()) << Complete.Diagnostic;
  EXPECT_TRUE(Complete.Diagnostic.empty());
}

TEST(ControlStateRecovery, ManualAndAutomaticFieldsShareOneFieldBudget) {
  auto Provider = makeBankProgram(false);
  auto Options = bankOptions(true, false);
  Options.DiscoverControlState = true;
  Options.MaxControlFields = 1;
  expectBudgetRefusal(specializeInterpreter(Provider, {Entry}, Options));
}

TEST(ControlStateRecovery, ZeroDiscoveryBudgetsPermitAlreadyProvedControl) {
  BankProvider Provider;
  Provider.add(Entry,
               {operation(NdOp::COPY, r(ResultRegister), {c(17)}), ret()});
  auto Options = automaticBankOptions();
  Options.MaxControlRefinements = 0;
  Options.MaxDiscoveryVisits = 0;
  const auto Result = specializeInterpreter(Provider, {Entry}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(Result.DiscoveredControlFields, 0u);
  EXPECT_EQ(Result.ControlRefinements, 0u);
  EXPECT_EQ(Result.DiscoveryVisits, 0u);
  EXPECT_EQ(run(Result.Residual, 0, 0), 17u);
}

TEST(ControlStateRecovery, RestartChargesEarlierOperationWorkToSameBudget) {
  auto Provider = makeBankProgram(false);
  const auto Initial =
      specializeInterpreter(Provider, {Entry}, bankOptions(false, false));
  ASSERT_FALSE(Initial.complete());
  ASSERT_GT(Initial.EvaluatedOperations, 0u);
  auto Options = automaticBankOptions();
  Options.MaxOperations = Initial.EvaluatedOperations;
  const auto Result = specializeInterpreter(Provider, {Entry}, Options);
  expectBudgetRefusal(Result);
}

TEST(ControlStateRecovery, ManualCursorContextProvesBoundedFrameWriteLoop) {
  auto Provider = makeMemoryCursorProgram();
  const auto WithoutHints = specializeInterpreter(
      Provider, {Entry}, memoryCursorOptions(false, false));
  EXPECT_EQ(WithoutHints.Status, SpecializationStatus::Unsupported)
      << WithoutHints.Diagnostic;
  EXPECT_TRUE(WithoutHints.Residual.Blocks.empty());
  const auto Result = specializeInterpreter(Provider, {Entry},
                                            memoryCursorOptions(true, false));
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(Result.Reads.size(), MemoryCursorPositions);
  for (uint64_t Input : {0ULL, 1ULL, 23ULL, ~0ULL})
    EXPECT_EQ(run(Result.Residual, Input, 0),
              MemoryCursorPositions * Input +
                  MemoryCursorPositions * (MemoryCursorPositions - 1) / 2);
}

TEST(ControlStateRecovery, MemoryPrecisionCanPromoteProvedCursorContexts) {
  auto Provider = makeMemoryCursorProgram();
  const auto Options = memoryCursorOptions(false, true);
  ASSERT_GT(MemoryCursorPositions, Options.MaxImmutableReadAddresses);
  const auto Result = specializeInterpreter(Provider, {Entry}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_GT(Result.DiscoveredControlFields, 0u);
  EXPECT_GT(Result.DiscoveredContextFields, 0u);
  EXPECT_LE(Result.DiscoveredContextFields, Result.DiscoveredControlFields);
  EXPECT_GT(Result.ControlRefinements, 0u);
  EXPECT_LE(Result.ControlRefinements, Options.MaxControlRefinements);
  EXPECT_LE(Result.DiscoveryVisits, Options.MaxDiscoveryVisits);
  EXPECT_EQ(Result.Reads.size(), MemoryCursorPositions);
  for (const auto &Block : Result.Residual.Blocks)
    for (const auto &Op : Block.Ops)
      EXPECT_NE(Op.Opcode, NdOp::INDIR_BR);
  for (uint64_t Input : {0ULL, 1ULL, 23ULL, 0x123456789abcdef0ULL, ~0ULL})
    EXPECT_EQ(run(Result.Residual, Input, 0),
              MemoryCursorPositions * Input +
                  MemoryCursorPositions * (MemoryCursorPositions - 1) / 2);
}

TEST(ControlStateRecovery, MemoryContextPromotionKeepsContextBudget) {
  auto Provider = makeMemoryCursorProgram();
  auto Options = memoryCursorOptions(false, true);
  Options.MaxContextsPerAddress = 1;
  expectBudgetRefusal(specializeInterpreter(Provider, {Entry}, Options));
}

TEST(ControlStateRecovery, MemoryDiscoveryCannotBoundInputDerivedFrameWrites) {
  auto Provider = makeMemoryCursorProgram(true);
  const auto Result = specializeInterpreter(Provider, {Entry},
                                            memoryCursorOptions(false, true));
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported)
      << Result.Diagnostic;
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_TRUE(Result.Origins.empty());
  EXPECT_TRUE(Result.Reads.empty());
}

TEST(ControlStateRecovery, ManualFieldsPreserveFiniteProducerCorrelation) {
  auto Provider = makeFiniteDependencyProgram();
  const auto WithoutHints = specializeInterpreter(
      Provider, {Entry}, finiteDependencyOptions(false, false));
  EXPECT_EQ(WithoutHints.Status, SpecializationStatus::Unsupported)
      << WithoutHints.Diagnostic;
  EXPECT_TRUE(WithoutHints.Residual.Blocks.empty());
  const auto Result = specializeInterpreter(
      Provider, {Entry}, finiteDependencyOptions(true, false));
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  ASSERT_EQ(Result.Reads.size(), 1u);
  EXPECT_EQ(Result.Reads.front().Address, TableBase);
  for (uint64_t Input : {0ULL, 1ULL, 6ULL, 17ULL, ~0ULL})
    EXPECT_EQ(run(Result.Residual, Input, 0), Input ^ 83);
}

TEST(ControlStateRecovery, MemoryDemandFollowsAlreadyFiniteProducerChain) {
  // A and B are correlated bits, so A+B-1 is zero. Losing either spill makes
  // the displacement and cursor finite but imprecise. Finite projection alone
  // must not stop the dependency chain before reaching those producer slots.
  auto Provider = makeFiniteDependencyProgram();
  const auto Options = finiteDependencyOptions(false, true);
  const auto Result = specializeInterpreter(Provider, {Entry}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_GE(Result.DiscoveredControlFields, 5u);
  EXPECT_GT(Result.ControlRefinements, 1u);
  EXPECT_LE(Result.ControlRefinements, Options.MaxControlRefinements);
  EXPECT_LE(Result.DiscoveryVisits, Options.MaxDiscoveryVisits);
  ASSERT_EQ(Result.Reads.size(), 1u);
  EXPECT_EQ(Result.Reads.front().Address, TableBase);
  for (uint64_t Input : {0ULL, 1ULL, 6ULL, 17ULL, 0xabcdef0123456789ULL, ~0ULL})
    EXPECT_EQ(run(Result.Residual, Input, 0), Input ^ 83);
}

TEST(ControlStateRecovery, BitDemandsDoNotExpandAcrossProducerPhases) {
  auto Provider = makeCrossPhaseBitDemandProgram();
  const auto Options = finiteDependencyOptions(false, true);
  // Only bit zero of the first phase's 16-value byte selects a handler. Its
  // upper nibble comes from 17 unrelated inputs. Expanding a one-bit demand
  // to a byte while walking back over a boundary would register those inputs
  // when the later finite dependency chain needs deferred refinement.
  ASSERT_EQ(Options.MaxControlFields, 16u);
  const auto Result =
      specializeInterpreter(Provider, {FirstPhaseEntry}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_LE(Result.DiscoveredControlFields, Options.MaxControlFields);
  EXPECT_LE(Result.ControlRefinements, Options.MaxControlRefinements);
  EXPECT_LE(Result.DiscoveryVisits, Options.MaxDiscoveryVisits);
  EXPECT_LE(Result.SolverQueries, Options.MaxSolverQueries);
  for (uint64_t Seed : {0ULL, 37ULL, ~0ULL}) {
    std::map<uint64_t, uint64_t> ExtraRegisters;
    uint64_t Combined = 0;
    for (unsigned I = 0; I < FirstProducerInputs; ++I) {
      const auto Value = (Seed + 23 * I) ^ (Seed >> I);
      Combined ^= Value;
      ExtraRegisters.emplace(FirstProducerRegister + I * 8, Value);
    }
    for (uint64_t Input : {0ULL, 1ULL, 6ULL, 17ULL, 0xabcdefULL, ~0ULL})
      EXPECT_EQ(run(Result.Residual, Input, 0, ExtraRegisters),
                (Input ^ 83) + 9 + 16 * (Combined & 15));
  }
}

TEST(ControlStateRecovery, LongTransparentPhasesKeepExactBitDemands) {
  auto Provider = makeCrossPhaseBitDemandProgram(20);
  const auto Options = finiteDependencyOptions(false, true);
  // The low bit controls a one-record table; 17 unrelated inputs supply the
  // same byte's observable upper nibble. Backward closure across the branch
  // chain must retain only the demanded bit, not enroll the whole byte.
  ASSERT_GT(FirstProducerInputs, Options.MaxControlFields);
  const auto Result =
      specializeInterpreter(Provider, {FirstPhaseEntry}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_LE(Result.DiscoveredControlFields, Options.MaxControlFields);
  EXPECT_LE(Result.ControlRefinements, Options.MaxControlRefinements);
  EXPECT_LE(Result.DiscoveryVisits, Options.MaxDiscoveryVisits);
  EXPECT_LE(Result.SolverQueries, Options.MaxSolverQueries);
  for (uint64_t Seed : {0ULL, 37ULL, ~0ULL}) {
    std::map<uint64_t, uint64_t> ExtraRegisters;
    uint64_t Combined = 0;
    for (unsigned I = 0; I < FirstProducerInputs; ++I) {
      const uint64_t Value = (Seed + 23 * I) ^ (Seed >> I);
      Combined ^= Value;
      ExtraRegisters.emplace(FirstProducerRegister + I * 8, Value);
    }
    for (uint64_t Input : {0ULL, 1ULL, 6ULL, 17ULL, 0xabcdefULL, ~0ULL})
      EXPECT_EQ(run(Result.Residual, Input, 0, ExtraRegisters),
                (Input ^ 83) + 9 + 16 * (Combined & 15));
  }
}
