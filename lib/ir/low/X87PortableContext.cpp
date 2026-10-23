//===- X87PortableContext.cpp - Portable owned x87 state slice -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Operate on the existing raw context, then publish only a completed step.
/// Guest memory authentication and pending-fault address ordering remain with
/// the concrete executor; this owner also checks its independent call boundary.
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/X87PortableContext.h"

#include "neverd/ir/X87Capabilities.h"
#include "neverd/ir/X87Context.h"
#include "neverd/ir/X87Control.h"
#include "neverd/ir/low/X87Arithmetic.h"

#include <algorithm>

namespace neverd {
namespace {
using Outcome = X87PortableOutcome;
using Operation = X87PortableOperation;
constexpr uint16_t ExceptionFlags = 0x3f;
constexpr uint16_t C1 = 0x0200;

uint16_t word(std::span<const uint8_t> Bytes, unsigned Offset = 0) {
  return uint16_t(Bytes[Offset]) | (uint16_t(Bytes[Offset + 1]) << 8);
}
void putWord(std::span<uint8_t> Bytes, unsigned Offset, uint16_t Value) {
  Bytes[Offset] = Value;
  Bytes[Offset + 1] = Value >> 8;
}
void putAddress(std::span<uint8_t> Bytes, unsigned Offset, uint64_t Value) {
  for (unsigned I = 0; I != 8; ++I)
    Bytes[Offset + I] = Value >> (I * 8);
}

// A transient view of the existing context, not another persistent stack.
struct Context {
  std::array<uint8_t, 512> Image;
  uint8_t Known;

  unsigned top() const { return (word(Image, 2) >> 11) & 7; }
  unsigned physical(unsigned Logical) const { return (top() + Logical) & 7; }
  Outcome readable(unsigned Logical) const {
    const unsigned Mask = 1U << physical(Logical);
    if (!(Image[4] & Mask))
      return Outcome::StackFault;
    return Known & Mask ? Outcome::Completed : Outcome::UnknownPayload;
  }
  X87Binary80Bytes read(unsigned Logical) const {
    X87Binary80Bytes Value{};
    std::copy_n(Image.begin() + 32 + Logical * 16, 10, Value.begin());
    return Value;
  }
  void write(unsigned Logical, const X87Binary80Bytes &Value) {
    std::copy(Value.begin(), Value.end(), Image.begin() + 32 + Logical * 16);
    const unsigned Mask = 1U << physical(Logical);
    Image[4] |= Mask;
    Known |= Mask;
  }
  void setTop(unsigned NewTop) {
    const unsigned OldTop = top();
    const auto Old = Image;
    for (unsigned I = 0; I != 8; ++I) {
      const unsigned OldLogical = (NewTop + I + 8 - OldTop) & 7;
      std::copy_n(Old.begin() + 32 + OldLogical * 16, 16,
                  Image.begin() + 32 + I * 16);
    }
    putWord(Image, 2, (word(Image, 2) & ~0x3800U) | (NewTop << 11));
  }
  Outcome push(const X87Binary80Bytes &Value) {
    const unsigned NewTop = (top() + 7) & 7;
    if (Image[4] & (1U << NewTop))
      return Outcome::StackFault;
    setTop(NewTop);
    write(0, Value);
    return Outcome::Completed;
  }
  void pop() {
    Image[4] &= ~(1U << top());
    setTop((top() + 1) & 7);
  }
  void clearC1() { putWord(Image, 2, word(Image, 2) & ~C1); }
};

Outcome arithmetic(Operation Op, unsigned Escape, unsigned Index, Context &S) {
  if (auto Status = S.readable(0); Status != Outcome::Completed)
    return Status;
  if (auto Status = S.readable(Index); Status != Outcome::Completed)
    return Status;
  const unsigned Destination = Escape == 0xd8 ? 0 : Index;
  auto Left = S.read(Destination);
  auto Right = S.read(Escape == 0xd8 ? Index : 0);
  X87ArithmeticOperation Kind;
  switch (Op) {
  case Operation::Add:
    Kind = X87ArithmeticOperation::Add;
    break;
  case Operation::Multiply:
    Kind = X87ArithmeticOperation::Multiply;
    break;
  case Operation::ReverseSubtract:
    std::swap(Left, Right);
    [[fallthrough]];
  case Operation::Subtract:
    Kind = X87ArithmeticOperation::Subtract;
    break;
  case Operation::ReverseDivide:
    std::swap(Left, Right);
    [[fallthrough]];
  case Operation::Divide:
    Kind = X87ArithmeticOperation::Divide;
    break;
  default:
    return Outcome::UnsupportedInstruction;
  }
  const auto Value =
      evaluateFiniteX87Arithmetic(Kind, Left, Right, word(S.Image));
  if (!Value)
    return Outcome::UnsupportedValue;
  S.write(Destination, Value->Value);
  putWord(S.Image, 2,
          (word(S.Image, 2) & ~C1) | Value->Exceptions |
              (Value->RoundedUp ? C1 : 0));
  if (Escape == 0xde)
    S.pop();
  return Outcome::Completed;
}

Outcome execute(Operation Op, unsigned Escape, unsigned Index, Context &S,
                std::span<uint8_t> Memory, uint64_t &Result) {
  switch (Op) {
  case Operation::Initialize:
    S.setTop(0); // Retain physical payloads even though all tags become empty.
    putWord(S.Image, 0, 0x037f);
    putWord(S.Image, 2, 0);
    S.Image[4] = 0;
    putWord(S.Image, 6, 0);
    putAddress(S.Image, 8, 0);
    putAddress(S.Image, 16, 0);
    return Outcome::Completed;
  case Operation::ClearExceptions:
    putWord(S.Image, 2, word(S.Image, 2) & 0x7f00);
    return Outcome::Completed;
  case Operation::Wait:
    return Outcome::Completed;
  case Operation::LoadControl: {
    const uint16_t Control = word(Memory);
    if (!x87ControlWordIsValid(Control))
      return Outcome::InvalidControl;
    if ((Control & ExceptionFlags) != ExceptionFlags)
      return Outcome::UnmaskedControl;
    putWord(S.Image, 0, Control | 0x0040); // Reserved bit 6 reads as one.
    return Outcome::Completed;
  }
  case Operation::StoreControl:
    putWord(Memory, 0, word(S.Image));
    return Outcome::Completed;
  case Operation::StoreStatus:
    if (Memory.empty())
      Result = word(S.Image, 2);
    else
      putWord(Memory, 0, word(S.Image, 2));
    return Outcome::Completed;
  case Operation::LoadRaw: {
    X87Binary80Bytes Value{};
    std::copy_n(Memory.begin(), Value.size(), Value.begin());
    S.clearC1();
    return S.push(Value);
  }
  case Operation::LoadInteger: {
    uint64_t Bits = 0;
    for (unsigned I = 0; I != Memory.size(); ++I)
      Bits |= uint64_t(Memory[I]) << (I * 8);
    const auto Value = convertIntegerToX87(Bits, Memory.size(), true);
    if (!Value)
      return Outcome::UnsupportedValue;
    S.clearC1();
    return S.push(*Value);
  }
  case Operation::StoreInteger:
  case Operation::StoreIntegerPop: {
    if (auto Status = S.readable(0); Status != Outcome::Completed)
      return Status;
    const auto Value =
        convertFiniteX87ToInteger(S.read(0), Memory.size(), word(S.Image));
    if (!Value)
      return Outcome::UnsupportedValue;
    for (unsigned I = 0; I != Memory.size(); ++I)
      Memory[I] = Value->Bits >> (I * 8);
    putWord(S.Image, 2,
            (word(S.Image, 2) & ~C1) | Value->Exceptions |
                (Value->RoundedUp ? C1 : 0));
    if (Op == Operation::StoreIntegerPop)
      S.pop();
    return Outcome::Completed;
  }
  case Operation::Negate:
  case Operation::Absolute: {
    if (auto Status = S.readable(0); Status != Outcome::Completed)
      return Status;
    auto Value = S.read(0);
    Value[9] = Op == Operation::Negate ? Value[9] ^ 0x80 : Value[9] & 0x7f;
    S.write(0, Value);
    S.clearC1();
    return Outcome::Completed;
  }
  case Operation::LoadRegister:
    if (auto Status = S.readable(Index); Status != Outcome::Completed)
      return Status;
    S.clearC1();
    return S.push(S.read(Index));
  case Operation::StoreRawPop:
  case Operation::StoreRegister:
  case Operation::StoreRegisterPop:
    if (auto Status = S.readable(0); Status != Outcome::Completed)
      return Status;
    S.clearC1();
    if (Op == Operation::StoreRawPop) {
      const auto Value = S.read(0);
      std::copy(Value.begin(), Value.end(), Memory.begin());
    } else {
      S.write(Index, S.read(0));
    }
    if (Op != Operation::StoreRegister)
      S.pop();
    return Outcome::Completed;
  case Operation::Exchange: {
    if (auto Status = S.readable(0); Status != Outcome::Completed)
      return Status;
    if (auto Status = S.readable(Index); Status != Outcome::Completed)
      return Status;
    const auto Old = S.read(0);
    S.write(0, S.read(Index));
    S.write(Index, Old);
    S.clearC1();
    return Outcome::Completed;
  }
  case Operation::Compare:
  case Operation::ComparePop:
  case Operation::ComparePopTwice:
  case Operation::CompareFlags:
  case Operation::CompareFlagsPop:
  case Operation::TestZero: {
    if (auto Status = S.readable(0); Status != Outcome::Completed)
      return Status;
    X87Binary80Bytes Right{};
    if (Op != Operation::TestZero) {
      if (auto Status = S.readable(Index); Status != Outcome::Completed)
        return Status;
      Right = S.read(Index);
    }
    const auto Compared = evaluateFiniteX87Compare(S.read(0), Right);
    if (!Compared)
      return Outcome::UnsupportedValue;
    uint16_t Status = (word(S.Image, 2) & ~C1) | Compared->Exceptions;
    const bool Less = Compared->Relation == X87Comparison::Less;
    const bool Equal = Compared->Relation == X87Comparison::Equal;
    if (Op == Operation::CompareFlags || Op == Operation::CompareFlagsPop)
      Result = Less ? 1 : Equal ? 0x40 : 0;
    else
      Status = (Status & ~0x4500U) | (Less ? 0x100 : Equal ? 0x4000 : 0);
    putWord(S.Image, 2, Status);
    if (Op == Operation::ComparePop || Op == Operation::CompareFlagsPop ||
        Op == Operation::ComparePopTwice)
      S.pop();
    if (Op == Operation::ComparePopTwice)
      S.pop();
    return Outcome::Completed;
  }
  default:
    return arithmetic(Op, Escape, Index, S);
  }
}
} // namespace

const char *x87PortableOutcomeName(X87PortableOutcome Outcome) {
  switch (Outcome) {
  case X87PortableOutcome::Completed:
    return "completed";
  case X87PortableOutcome::InvalidInstruction:
    return "invalid-instruction";
  case X87PortableOutcome::UnsupportedInstruction:
    return "instruction-outside-portable-profile";
  case X87PortableOutcome::InvalidControl:
    return "invalid-control-word";
  case X87PortableOutcome::PendingException:
    return "pending-unmasked-exception";
  case X87PortableOutcome::UnmaskedControl:
    return "unmasked-exception-control-outside-profile";
  case X87PortableOutcome::UnknownPayload:
    return "unknown-register-payload";
  case X87PortableOutcome::StackFault:
    return "stack-fault-outside-profile";
  case X87PortableOutcome::UnsupportedValue:
    return "operand-outside-portable-numerical-domain";
  case X87PortableOutcome::InvalidMemory:
    return "invalid-memory-operand-size";
  }
  return "invalid-outcome";
}

X87PortableOutcome
executePortableX87Context(uint32_t Selector, std::array<uint8_t, 512> &State,
                          uint8_t &KnownPayload, std::span<uint8_t> Memory,
                          uint64_t IP, uint64_t DP, uint64_t &Result) {
  const auto Spec = x87ContextInstruction(Selector);
  const auto *Form = x87InstructionForm(Selector);
  if (!Spec || !Form)
    return Outcome::InvalidInstruction;
  const auto Op = Form->PortableOperation;
  if (Op == Operation::None)
    return Outcome::UnsupportedInstruction;
  const uint16_t Control = word(State), Status = word(State, 2);
  if (Spec->Waiting && ((Status & 0x80) || (Status & ~Control & 0x3f)))
    return Outcome::PendingException;
  if (Op != Operation::Initialize &&
      (!x87ControlWordIsValid(Control) || !(Control & 0x0040)))
    return Outcome::InvalidControl;
  if (x87ContextUpdatesInstructionPointer(Selector) &&
      (Control & ExceptionFlags) != ExceptionFlags)
    return Outcome::UnmaskedControl;
  if (Memory.size() != Spec->MemoryBytes)
    return Outcome::InvalidMemory;

  Context Next{State, KnownPayload};
  std::array<uint8_t, 10> Operand{};
  if (Memory.size() > Operand.size())
    return Outcome::InvalidMemory;
  std::copy(Memory.begin(), Memory.end(), Operand.begin());
  auto OperandView = std::span(Operand).first(Memory.size());
  auto Output = Result;
  const auto Completion = execute(Op, Selector & 0xff, (Selector >> 8) & 7,
                                  Next, OperandView, Output);
  if (Completion != Outcome::Completed)
    return Completion;
  if (x87ContextUpdatesInstructionPointer(Selector)) {
    putAddress(Next.Image, 8, IP);
    if (Spec->MemoryBytes)
      putAddress(Next.Image, 16, DP);
    // FOP is defined for an unmasked exception in this Intel 64 profile;
    // arithmetic in this slice requires all masks and retains the old FOP.
  }
  State = Next.Image;
  KnownPayload = Next.Known;
  if (Spec->WritesMemory)
    std::copy(OperandView.begin(), OperandView.end(), Memory.begin());
  Result = Output;
  return Outcome::Completed;
}
} // namespace neverd
