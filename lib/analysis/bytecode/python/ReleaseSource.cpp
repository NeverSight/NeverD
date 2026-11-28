//===- ReleaseSource.cpp - Normalize qualified Python source operations ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep disk instructions and their offsets for navigation. Feed equivalent
/// stack operations to the existing structured emitter only after verifying
/// their release-specific operands and calling convention.
//===----------------------------------------------------------------------===//

#include "Artifacts.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"

using namespace llvm;
using namespace neverd::python;

namespace {
std::optional<Op> sharedOp(StringRef Name) {
  return StringSwitch<std::optional<Op>>(Name)
#define SAME(N) .Case(#N, Op::N)
      SAME(NOP) SAME(POP_TOP) SAME(LOAD_CONST) SAME(LOAD_FAST) SAME(
             STORE_FAST) SAME(LOAD_NAME) SAME(STORE_NAME) SAME(LOAD_GLOBAL)
          SAME(STORE_GLOBAL) SAME(LOAD_ATTR) SAME(UNARY_NEGATIVE) SAME(
              UNARY_POSITIVE) SAME(UNARY_INVERT) SAME(UNARY_NOT) SAME(BINARY_SUBSCR)
              SAME(BINARY_ADD) SAME(BINARY_MULTIPLY) SAME(BINARY_SUBTRACT) SAME(
                  BINARY_MODULO) SAME(BINARY_POWER) SAME(BINARY_OR) SAME(BINARY_XOR)
                  SAME(BINARY_AND) SAME(BINARY_RSHIFT) SAME(BINARY_LSHIFT) SAME(
                      BINARY_FLOOR_DIVIDE) SAME(BINARY_TRUE_DIVIDE)
                      SAME(BINARY_MATRIX_MULTIPLY) SAME(BUILD_TUPLE) SAME(
                          BUILD_LIST) SAME(BUILD_SET) SAME(COMPARE_OP)
                          SAME(IS_OP) SAME(CONTAINS_OP) SAME(JUMP_FORWARD) SAME(
                              JUMP_ABSOLUTE) SAME(JUMP_IF_FALSE_OR_POP)
                              SAME(JUMP_IF_TRUE_OR_POP) SAME(POP_JUMP_IF_FALSE) SAME(
                                  POP_JUMP_IF_TRUE) SAME(MAKE_FUNCTION)
                                  SAME(RETURN_VALUE) SAME(LOAD_METHOD) SAME(
                                      CALL_METHOD) SAME(CALL_FUNCTION) SAME(GET_ITER)
                                      SAME(FOR_ITER) SAME(BUILD_CONST_KEY_MAP) SAME(
                                          INPLACE_ADD) SAME(INPLACE_SUBTRACT)
                                          SAME(INPLACE_MULTIPLY) SAME(
                                              INPLACE_MODULO) SAME(INPLACE_POWER)
                                              SAME(INPLACE_AND) SAME(INPLACE_OR) SAME(
                                                  INPLACE_XOR) SAME(INPLACE_RSHIFT)
                                                  SAME(INPLACE_LSHIFT) SAME(
                                                      INPLACE_TRUE_DIVIDE)
                                                      SAME(INPLACE_FLOOR_DIVIDE) SAME(
                                                          INPLACE_MATRIX_MULTIPLY)
#undef SAME
                                                          .Cases(
                                                              {"POP_JUMP_"
                                                               "FORWARD_IF_"
                                                               "FALSE",
                                                               "POP_JUMP_"
                                                               "BACKWARD_IF_"
                                                               "FALSE"},
                                                              Op::POP_JUMP_IF_FALSE)
                                                          .Cases(
                                                              {"POP_JUMP_"
                                                               "FORWARD_IF_"
                                                               "TRUE",
                                                               "POP_JUMP_"
                                                               "BACKWARD_IF_"
                                                               "TRUE"},
                                                              Op::POP_JUMP_IF_TRUE)
                                                          .Cases({"LOAD_FAST_"
                                                                  "CHECK",
                                                                  "LOAD_FAST_"
                                                                  "BORROW"},
                                                                 Op::LOAD_FAST)
                                                          .Case(
                                                              "JUMP_BACKWARD",
                                                              Op::JUMP_ABSOLUTE)
                                                          .Case("PUSH_NULL",
                                                                Op::PushNull)
                                                          .Case(
                                                              "CALL",
                                                              Op::CallWithSlot)
                                                          .Case(
                                                              "BUILD_FUNCTION",
                                                              Op::MAKE_FUNCTION)
                                                          .Case("LOAD_LOCAL",
                                                                Op::LOAD_NAME)
                                                          .Case(
                                                              "LOAD_SMALL_INT",
                                                              Op::SmallInteger)
                                                          .Default(
                                                              std::nullopt);
}

std::optional<Op> binaryOp(unsigned Arg) {
  static constexpr Op Binary[] = {Op::BINARY_ADD,
                                  Op::BINARY_AND,
                                  Op::BINARY_FLOOR_DIVIDE,
                                  Op::BINARY_LSHIFT,
                                  Op::BINARY_MATRIX_MULTIPLY,
                                  Op::BINARY_MULTIPLY,
                                  Op::BINARY_MODULO,
                                  Op::BINARY_OR,
                                  Op::BINARY_POWER,
                                  Op::BINARY_RSHIFT,
                                  Op::BINARY_SUBTRACT,
                                  Op::BINARY_TRUE_DIVIDE,
                                  Op::BINARY_XOR,
                                  Op::INPLACE_ADD,
                                  Op::INPLACE_AND,
                                  Op::INPLACE_FLOOR_DIVIDE,
                                  Op::INPLACE_LSHIFT,
                                  Op::INPLACE_MATRIX_MULTIPLY,
                                  Op::INPLACE_MULTIPLY,
                                  Op::INPLACE_MODULO,
                                  Op::INPLACE_OR,
                                  Op::INPLACE_POWER,
                                  Op::INPLACE_RSHIFT,
                                  Op::INPLACE_SUBTRACT,
                                  Op::INPLACE_TRUE_DIVIDE,
                                  Op::INPLACE_XOR,
                                  Op::BINARY_SUBSCR};
  if (Arg >= std::size(Binary))
    return std::nullopt;
  return Binary[Arg];
}
/// Before co_varnames, the function prologue carried both the parameter
/// unpacking and the mapping between frame slots and identifiers.
bool legacyParameters(Code &C, ArrayRef<InstructionRecord> Rows,
                      ArrayRef<unsigned> Incoming, BitVector &Skip) {
  if (C.isModule())
    return true;
  size_t N = 0;
  while (N < Rows.size() && Rows[N].Mnemonic == "SET_LINENO")
    ++N;
  auto &Names = C.Vars->Items;
  if (N < Rows.size() && Rows[N].Mnemonic == "RESERVE_FAST") {
    const Object &Map = *C.Consts->Items[Rows[N].Operand];
    if (Map.Type == Object::Dict && C.Version <= 100) {
      Names.resize(Map.Items.size() / 2);
      for (size_t K = 0; K < Map.Items.size(); K += 2) {
        auto *Key = Map.Items[K], *Slot = Map.Items[K + 1];
        if (Key->Type != Object::Bytes || Slot->Type != Object::Int ||
            Slot->Number < 0 || uint64_t(Slot->Number) >= Names.size() ||
            Names[Slot->Number])
          return false;
        Names[Slot->Number] = Key;
      }
    } else if (Map.Type == Object::Tuple && C.Version >= 101) {
      if (!llvm::all_of(Map.Items, [](const Object *O) {
            return O->Type == Object::Bytes;
          }))
        return false;
      Names = Map.Items;
    } else if (Map.Type != Object::None)
      return false;
    Skip.set(N++);
  }
  if (N == Rows.size() || Rows[N].Mnemonic != "UNPACK_ARG")
    return false;
  C.Args = Rows[N].Operand;
  Skip.set(N++);
  if (C.Args > Rows.size() - N)
    return false;
  bool Fast = !Names.empty();
  for (unsigned Arg = 0; Arg < C.Args; ++Arg, ++N) {
    if (Fast) {
      if (Rows[N].Mnemonic != "STORE_FAST" || Rows[N].Operand != Arg ||
          Arg >= Names.size())
        return false;
    } else {
      if (Rows[N].Mnemonic != "STORE_NAME")
        return false;
      Names.push_back(C.Names->Items[Rows[N].Operand]);
    }
    Skip.set(N);
  }
  C.Locals = Names.size();
  return llvm::none_of(Incoming.take_front(N),
                       [](unsigned Count) { return Count != 0; });
}
} // namespace

bool neverd::python::prepareSource(Function &F) {
  auto &C = *F.Node.Body;
  if (!F.Decoded || !F.ExceptionRegions.empty())
    return false;
  unsigned Unit = C.Version < 306 ? 1 : 2;
  F.Starts.assign(C.Bytes->Data.size() / Unit, -1);
  const auto &Rows = F.DiskInstructions;
  BitVector Skip;
  if (C.Version <= 300) {
    // Older conditionals leave the tested object on both edges. Consume it
    // once only when both successor POP_TOPs belong solely to this test.
    Skip.resize(Rows.size());
    std::vector<unsigned> Incoming(Rows.size());
    for (auto [N, Row] : llvm::enumerate(Rows))
      F.Starts[Row.PC / Unit] = N;
    for (const auto &Row : Rows)
      if (Row.Target)
        ++Incoming[F.Starts[*Row.Target / Unit]];
    if (C.Version < 103 && !legacyParameters(C, Rows, Incoming, Skip))
      return false;
    for (auto [N, Row] : llvm::enumerate(Rows)) {
      if (Row.Mnemonic != "JUMP_IF_FALSE" && Row.Mnemonic != "JUMP_IF_TRUE")
        continue;
      size_t Target = F.Starts[*Row.Target / Unit];
      if (N + 1 >= Rows.size() || Target <= N + 1 ||
          Rows[N + 1].Mnemonic != "POP_TOP" ||
          Rows[Target].Mnemonic != "POP_TOP" || Incoming[N + 1] ||
          Incoming[Target] != 1 || Rows[Target - 1].FallsThrough)
        return false;
      Skip.set(N + 1);
      Skip.set(Target);
    }
    size_t End = Rows.size();
    if (End >= 3 && Rows[End - 3].Mnemonic == "RETURN_VALUE" &&
        Rows[End - 2].Mnemonic == "LOAD_CONST" &&
        Rows.back().Mnemonic == "RETURN_VALUE" &&
        C.Consts->Items[Rows[End - 2].Operand]->Type == Object::None &&
        !Incoming[End - 2] && !Incoming[End - 1]) {
      Skip.set(End - 2);
      Skip.set(End - 1);
    }
  }
  for (const auto &[Index, I] : llvm::enumerate(F.DiskInstructions)) {
    StringRef Name = I.Mnemonic;
    unsigned Arg = I.Operand;
    F.Starts[I.PC / Unit] = F.Code.size();
    auto Emit = [&](Op O, unsigned Operand) {
      F.Code.emplace_back(
          Instruction{O, I.PC, I.End, Operand, I.Target, I.OriginalLine});
    };
    if ((!Skip.empty() && Skip.test(Index)) || Name == "SET_LINENO" ||
        (Name == "RESUME" && !I.PC && !Arg))
      continue;
    if (Name == "PRECALL") {
      if (Index + 1 == Rows.size() || Rows[Index + 1].Mnemonic != "CALL" ||
          Rows[Index + 1].Operand != Arg)
        return false;
      continue;
    }
    if ((Name == "CALL_FUNCTION" && C.Version < 306 && (Arg >> 8)) ||
        (Name == "FOR_ITER" && C.Version >= 312))
      return false;
    if (Name == "TO_BOOL" && Index + 1 < F.DiskInstructions.size()) {
      auto Next = F.DiskInstructions[Index + 1].Mnemonic;
      if (Next == "POP_JUMP_IF_TRUE" || Next == "POP_JUMP_IF_FALSE")
        continue;
    }
    if (Name == "NOT_TAKEN" && Index &&
        F.DiskInstructions[Index - 1].Mnemonic.starts_with("POP_JUMP_IF_"))
      continue;
    if (Name == "JUMP_IF_FALSE" || Name == "JUMP_IF_TRUE") {
      if (Skip.empty())
        return false;
      Emit(Name == "JUMP_IF_FALSE" ? Op::POP_JUMP_IF_FALSE
                                   : Op::POP_JUMP_IF_TRUE,
           Arg);
      continue;
    }
    if (Name == "LOAD_COMMON_CONSTANT" && C.Version >= 315 && Arg >= 7 &&
        Arg <= 11) {
      Emit(Op::CommonConstant, Arg - 7);
      continue;
    }
    if (Name == "RETURN_CONST") {
      Emit(Op::LOAD_CONST, Arg);
      Emit(Op::RETURN_VALUE, 0);
      continue;
    }
    if (Name == "LOAD_FAST_LOAD_FAST" ||
        Name == "LOAD_FAST_BORROW_LOAD_FAST_BORROW" ||
        Name == "STORE_FAST_LOAD_FAST" || Name == "STORE_FAST_STORE_FAST") {
      if (Arg >> 4 >= C.Locals || (Arg & 15) >= C.Locals)
        return false;
      Emit(Name.starts_with("LOAD") ? Op::LOAD_FAST : Op::STORE_FAST, Arg >> 4);
      Emit(Name.ends_with("STORE_FAST") ? Op::STORE_FAST : Op::LOAD_FAST,
           Arg & 15);
      continue;
    }
    auto OpCode = Name == "BINARY_OP" ? binaryOp(Arg) : sharedOp(Name);
    if (!OpCode)
      return false;
    if ((Name == "LOAD_GLOBAL" && C.Version >= 311) ||
        (Name == "LOAD_ATTR" && C.Version >= 312)) {
      if (Arg & 1) {
        if (Name == "LOAD_ATTR") {
          Emit(Op::LOAD_METHOD, Arg >> 1);
        } else {
          if (C.Version < 313)
            Emit(Op::PushNull, 0);
          Emit(Op::LOAD_GLOBAL, Arg >> 1);
          if (C.Version >= 313)
            Emit(Op::PushNull, 0);
        }
        continue;
      }
      Arg >>= 1;
    }
    if (Name == "COMPARE_OP") {
      if (C.Version >= 312) {
        // Newer comparisons may also coerce an overloaded result to bool.
        if (C.Version >= 313 && Arg & 16)
          return false;
        Arg >>= C.Version == 312 ? 4 : 5;
      }
      if (Arg >= 6)
        return false;
    }
    if (Name == "MAKE_FUNCTION" && Arg && (C.Version < 306 || C.Version >= 311))
      return false;
    if (C.Version <= 102 && (Name == "LOAD_FAST" || Name == "STORE_FAST") &&
        Arg >= C.Locals)
      return false;
    Emit(*OpCode, Arg);
  }
  return !F.Code.empty();
}
