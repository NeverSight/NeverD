//===- ProgramMain.cpp - The C program's main -----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "ProgramMain.h"

#include "InstructionFlow.h"
#include "SessionImpl.h"

#include "neverd/decode/Decoder.h"
#include "neverd/decode/InstructionRelocations.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/libc/LibCStartup.h"
#include "neverd/loader/SymbolDecoration.h"
#include "neverd/sdk/Limits.h"
#include "neverd/support/BinaryEncoding.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace neverd::sdk {
namespace {

/// The entry's instructions read before giving up: a start routine's caller
/// sets its arguments in a few.

/// What a register, a temporary or a stack word holds: a number, a place on
/// the stack, or the word stored at a constant address.  A place on the stack
/// is an offset from a base: the entry's stack pointer, or a place an
/// alignment (`and rsp, -16`) leaves somewhere below it.
struct Value {
  enum class Kind : uint8_t { Number, Stack, Slot };
  Kind What = Kind::Number;
  uint64_t Bits = 0;
  unsigned Base = 0;
};

uint64_t lowBytes(uint64_t Bits, uint16_t Size) {
  return Size >= 8 ? Bits : Bits & ((uint64_t{1} << (8 * Size)) - 1);
}

/// The values the entry's code has put in registers, temporaries and stack
/// words so far.  A variable is known only as wide as it was written.
class EntryState {
  struct Held {
    uint16_t Size;
    Value Content;
  };
  std::map<std::pair<VnodeSpace, uint64_t>, Held> Variables;
  std::map<std::pair<unsigned, uint64_t>, Value> StackWords;
  unsigned Bases = 1;

public:
  EntryState(uint64_t StackPointer, uint16_t PointerSize) {
    Variables[{VnodeSpace::REG, StackPointer}] = {PointerSize,
                                                  {Value::Kind::Stack, 0, 0}};
  }
  std::optional<Value> read(const NdVar &V) const {
    if (V.isConst())
      return Value{Value::Kind::Number, V.Offset, 0};
    const auto It = Variables.find({V.Space, V.Offset});
    if (It == Variables.end() || It->second.Size != V.Size)
      return std::nullopt;
    return It->second.Content;
  }
  void write(const NdVar &V, std::optional<Value> Content) {
    for (auto It = Variables.begin(); It != Variables.end();)
      if (It->first.first == V.Space && It->first.second < V.Offset + V.Size &&
          V.Offset < It->first.second + It->second.Size)
        It = Variables.erase(It);
      else
        ++It;
    if (Content)
      Variables[{V.Space, V.Offset}] = {V.Size, *Content};
  }
  std::optional<Value> word(const Value &Place) const {
    const auto It = StackWords.find({Place.Base, Place.Bits});
    return It == StackWords.end() ? std::nullopt
                                  : std::optional<Value>(It->second);
  }
  void store(const Value &Place, std::optional<Value> Content) {
    if (Content)
      StackWords[{Place.Base, Place.Bits}] = *Content;
    else
      StackWords.erase({Place.Base, Place.Bits});
  }
  /// A place an alignment of the stack pointer leaves: unknown, but where
  /// the code then pushes and reads words.
  Value alignedStack() { return {Value::Kind::Stack, 0, Bases++}; }
};

/// What \p Op leaves in its output, or in the stack word it stores.
void evaluate(EntryState &State, const LowOp &Op) {
  const NdVar &Out = Op.Output;
  const auto In = [&](unsigned Index) -> std::optional<Value> {
    return Index < Op.NumInputs ? State.read(Op.Inputs[Index]) : std::nullopt;
  };
  const auto Number = [&](uint64_t Bits) {
    return Value{Value::Kind::Number, lowBytes(Bits, Out.Size), 0};
  };
  std::optional<Value> Result;
  const auto A = In(0), B = In(1);
  const bool Numbers = A && B && A->What == Value::Kind::Number &&
                       B->What == Value::Kind::Number;
  switch (Op.Opcode) {
  case NdOp::COPY:
    Result = A;
    break;
  case NdOp::INT_ZEXT:
    if (A && A->What == Value::Kind::Number)
      Result = Number(A->Bits);
    break;
  case NdOp::INT_SEXT:
    if (A && A->What == Value::Kind::Number && Op.Inputs[0].Size &&
        Op.Inputs[0].Size < 8) {
      const unsigned Shift = 64 - 8 * Op.Inputs[0].Size;
      Result = Number(static_cast<uint64_t>(
          static_cast<int64_t>(A->Bits << Shift) >> Shift));
    } else if (A && A->What == Value::Kind::Number) {
      Result = Number(A->Bits);
    }
    break;
  case NdOp::INT_ADD:
  case NdOp::INT_SUB: {
    const bool Add = Op.Opcode == NdOp::INT_ADD;
    if (Numbers)
      Result = Number(Add ? A->Bits + B->Bits : A->Bits - B->Bits);
    else if (A && B && A->What == Value::Kind::Stack &&
             B->What == Value::Kind::Number)
      Result = Value{Value::Kind::Stack,
                     Add ? A->Bits + B->Bits : A->Bits - B->Bits, A->Base};
    else if (Add && A && B && A->What == Value::Kind::Number &&
             B->What == Value::Kind::Stack)
      Result = Value{Value::Kind::Stack, A->Bits + B->Bits, B->Base};
    break;
  }
  case NdOp::INT_AND:
    if (Numbers)
      Result = Number(A->Bits & B->Bits);
    else if (A && B &&
             (A->What == Value::Kind::Stack || B->What == Value::Kind::Stack))
      Result = State.alignedStack();
    break;
  case NdOp::SUBBYTES:
    if (A && A->What == Value::Kind::Number && Op.NumInputs == 2 &&
        Op.Inputs[1].isConst() && Op.Inputs[1].Offset < 8)
      Result = Number(A->Bits >> (8 * Op.Inputs[1].Offset));
    break;
  case NdOp::LOAD:
  case NdOp::STORE: {
    const LowMemoryOperandView Memory = lowMemoryOperands(Op);
    const auto Place =
        Memory.Complete && Memory.Address &&
                Op.MemoryAddressSpace == NdMemoryAddressSpace::Default
            ? State.read(*Memory.Address)
            : std::nullopt;
    if (Op.Opcode == NdOp::STORE) {
      if (Place && Place->What == Value::Kind::Stack)
        State.store(*Place, Memory.StoredValue ? State.read(*Memory.StoredValue)
                                               : std::nullopt);
      return;
    }
    if (Place && Place->What == Value::Kind::Number)
      Result = Value{Value::Kind::Slot, Place->Bits, 0};
    else if (Place && Place->What == Value::Kind::Stack)
      Result = State.word(*Place);
    break;
  }
  default:
    break;
  }
  if (Out.Size && !Out.isConst())
    State.write(Out, Result);
}

bool transfersControl(NdOp Opcode) {
  return Opcode == NdOp::BRANCH || Opcode == NdOp::COND_BR ||
         Opcode == NdOp::INDIR_BR || Opcode == NdOp::RETURN;
}

/// The code address \p Argument, the start routine's main argument, holds.
std::optional<va_t> codeAddress(const BinaryImage &Img,
                                const std::optional<Value> &Argument) {
  if (!Argument || Argument->What == Value::Kind::Stack)
    return std::nullopt;
  std::optional<va_t> Address = Argument->Bits;
  if (Argument->What == Value::Kind::Slot)
    Address = relocatedPointer(Img, Argument->Bits);
  if (Address && Img.Arch == Arch::ARM)
    Address = clearThumbBit(*Address);
  const Segment *Code = Address ? Img.getSegmentFor(*Address) : nullptr;
  return Code && Code->isExecutable() ? Address : std::nullopt;
}

/// The main the call \p Call passes a start routine: its arguments as they
/// were before the call's instruction, \p Before, and its target as the
/// instruction computed it, \p Now.  None for a call of anything else.
std::optional<va_t> passedMain(const BinaryImage &Img,
                               const TargetRegInfo &Registers,
                               const EntryState &Before, const EntryState &Now,
                               const LowOp &Call) {
  if (!Call.NumInputs)
    return std::nullopt;
  const auto Target = Now.read(Call.Inputs[0]);
  // `call [slot]` names the slot; a register holds what a slot held.
  if (!Target || Target->What == Value::Kind::Stack)
    return std::nullopt;
  const auto Parameter =
      libc::startRoutineMainParameter(importCalleeCName(Img, Target->Bits));
  if (!Parameter)
    return std::nullopt;
  IntegerArgumentLayout Layout =
      Registers.integerArgumentLayout(Img.abiFormat());
  // A C library's routines take what their platform's C passes: on 32-bit
  // x86 (cdecl), every argument in a stack word, none in the registers
  // fastcall uses.
  if (Img.Arch == Arch::X86)
    Layout.Registers = {};
  if (*Parameter < Layout.Registers.size())
    return codeAddress(Img, Before.read(NdVar::reg(Layout.Registers[*Parameter],
                                                   Registers.PointerSize)));
  // The rest are the stack words above the stack pointer at the call.
  const auto Pointer =
      Before.read(NdVar::reg(Registers.StackPointer, Registers.PointerSize));
  if (!Pointer || Pointer->What != Value::Kind::Stack || !Layout.SlotBytes)
    return std::nullopt;
  const uint64_t Slot = *Parameter - Layout.Registers.size();
  return codeAddress(
      Img, Before.word({Value::Kind::Stack,
                        Pointer->Bits + uint64_t(Layout.CallStackBase) +
                            Slot * Layout.SlotBytes,
                        Pointer->Base}));
}

std::optional<va_t> startRoutineMain(const BinaryImage &Img) {
  const TargetRegInfo &Registers = getTargetRegInfo(Img.Arch);
  if (!Img.Entry || !Registers.PointerSize)
    return std::nullopt;
  Decoder Dec;
  if (!Dec.init(Img))
    return std::nullopt;
  EntryState State(Registers.StackPointer, Registers.PointerSize);
  std::vector<LowOp> Ops;
  std::optional<va_t> Main;
  (void)decodeNativeRange(
      Img, Dec, Img.Entry, 0, ::neverd::sdk::kProgramMainEntryInstructions,
      [&](const DecodedInsn &DI, const uint8_t *, int) {
        Ops.clear();
        Dec.resetX86FpuState();
        try {
          const InstructionRelocations Relocations =
              instructionRelocations(Img, DI);
          Dec.liftToLow(DI, Ops, Relocations.Addresses, Relocations.Scalars);
        } catch (const UnliftedInstruction &) {
          return false;
        }
        const EntryState Before = State;
        for (const LowOp &Op : Ops) {
          // The entry's first call is the start routine's, or not.
          if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
            Main = passedMain(Img, Registers, Before, State, Op);
            return false;
          }
          if (transfersControl(Op.Opcode))
            return false;
          evaluate(State, Op);
        }
        return true;
      });
  return Main;
}

} // namespace

std::optional<va_t> findProgramMain(const BinaryImage &Img) {
  if (Img.Arch == Arch::SBF)
    return std::nullopt;
  if (const auto Main = startRoutineMain(Img))
    return Main;
  for (const Symbol &Sym : Img.Symbols)
    if (Sym.IsFunc && Sym.Origin == NameOrigin::Stated &&
        cNameOfStatedName(Sym.Name, Img.Format, Img.Arch) == "main")
      return Sym.Addr;
  return std::nullopt;
}

std::optional<va_t> Session::programMain() {
  if (!ProgramMainFound)
    ProgramMainFound = Loaded ? findProgramMain(Img) : std::nullopt;
  return *ProgramMainFound;
}

} // namespace neverd::sdk
