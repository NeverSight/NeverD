//===- Instructions.cpp - JVM bytecode boundaries and operands -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decode physical byte offsets before resolving branches and handler ranges.
//===----------------------------------------------------------------------===//

#include "Class.h"

#include "llvm/ADT/StringExtras.h"

using namespace llvm;
namespace neverd::jvm {
StringRef mnemonic(unsigned Op) {
  static constexpr StringRef Names[] = {
#include "Opcodes.def"
  };
  return Op < std::size(Names) ? Names[Op] : StringRef();
}
namespace {
bool wideLocal(unsigned Op) {
  return Op == 22 || Op == 24 || Op == 55 || Op == 57 ||
         (Op >= 30 && Op <= 33) || (Op >= 38 && Op <= 41) ||
         (Op >= 63 && Op <= 66) || (Op >= 71 && Op <= 74);
}
void checkConstant(const Class &C, Instruction &I, size_t At) {
  unsigned Op = I.Opcode, K = I.Pool;
  if (Op >= 18 && Op <= 20) {
    const auto &V = C.constant(K, {3, 4, 5, 6, 7, 8, 15, 16, 17}, At);
    bool Wide = V.Tag == 5 || V.Tag == 6;
    if (V.Tag == 17)
      Wide = slots(C.text(C.nameAndType(V).B, At)) == 2;
    if ((Op == 20) != Wide || (V.Tag == 7 && C.Major < 49))
      invalid(At, "ldc_type",
              "Constant category or version disagrees with ldc opcode");
  } else if (Op >= 178 && Op <= 181)
    C.constant(K, {9}, At);
  else if (Op == 182)
    C.constant(K, {10}, At);
  else if (Op == 183 || Op == 184) {
    if (C.Major >= 52)
      C.constant(K, {10, 11}, At);
    else
      C.constant(K, {10}, At);
  } else if (Op == 185) {
    const auto &V = C.constant(K, {11}, At);
    if (1 + methodDescriptor(C.text(C.nameAndType(V).B, At), At).Slots !=
        unsigned(I.Operand))
      invalid(At, "invoke_count",
              "invokeinterface count differs from descriptor slots");
  } else if (Op == 186)
    C.constant(K, {18}, At);
  else {
    const auto &N = C.className(K, At);
    if (Op == 187 && StringRef(N).starts_with("["))
      invalid(At, "new_type", "new requires a non-array class");
    if (Op == 197) {
      unsigned Rank =
          StringRef(N).take_while([](char V) { return V == '['; }).size();
      if (!I.Operand || unsigned(I.Operand) > Rank)
        invalid(At, "array_dimensions", "Invalid multianewarray dimensions");
    }
  }
  if (Op >= 182 && Op <= 185) {
    const auto &V = C.Pool[K];
    auto &N = C.text(C.nameAndType(V).A, At);
    if (N == "<init>" && Op != 183)
      invalid(At, "invoke_constructor", "Constructor requires invokespecial");
  }
}
} // namespace
Instructions decode(const Class &C, const Code &B, Job &J) {
  Reader R{B.Bytes, J, B.At};
  Instructions Out;
  Out.Index.assign(B.Bytes.size() + 1, -1);
  while (R.left()) {
    size_t At = R.at();
    J.charge(1, At);
    if (Out.Rows.size() >= J.Work)
      throw Failure{"budget_exceeded", "method_work",
                    "Method instruction budget exceeded", At};
    Instruction I;
    I.PC = R.Pos;
    I.Opcode = R.u1();
    unsigned Op = I.Opcode;
    if (mnemonic(Op).empty())
      invalid(At, "opcode", "Unknown or reserved JVM opcode");
    auto Target = [&](int64_t Delta) {
      int64_t Dest = int64_t(I.PC) + Delta;
      if (Dest < 0 || uint64_t(Dest) >= B.Bytes.size())
        invalid(At, "branch_extent", "Branch leaves method code");
      I.Targets.push_back(unsigned(Dest));
    };
    bool Local = false;
    if (Op == 196) {
      I.Wide = true;
      Op = I.Opcode = R.u1();
      if (!((Op >= 21 && Op <= 25) || (Op >= 54 && Op <= 58) || Op == 132 ||
            Op == 169))
        invalid(At, "wide_opcode", "Invalid opcode following wide");
      I.Local = R.u2();
      Local = true;
      if (Op == 132)
        I.Increment = int16_t(R.u2());
    } else if (Op == 16)
      I.Operand = int8_t(R.u1());
    else if (Op == 17)
      I.Operand = int16_t(R.u2());
    else if (Op >= 18 && Op <= 20)
      I.Pool = Op == 18 ? R.u1() : R.u2();
    else if ((Op >= 21 && Op <= 25) || (Op >= 54 && Op <= 58) || Op == 169 ||
             Op == 132) {
      Local = true;
      I.Local = R.u1();
      if (Op == 132)
        I.Increment = int8_t(R.u1());
    } else if ((Op >= 26 && Op <= 45) || (Op >= 59 && Op <= 78)) {
      Local = true;
      I.Local = (Op - (Op < 59 ? 26 : 59)) % 4;
    } else if ((Op >= 153 && Op <= 168) || Op == 198 || Op == 199)
      Target(int16_t(R.u2()));
    else if (Op == 200 || Op == 201)
      Target(int32_t(R.u4()));
    else if (Op == 170 || Op == 171) {
      while (R.Pos % 4)
        if (R.u1())
          invalid(R.at() - 1, "switch_padding", "Nonzero JVM switch padding");
      Target(int32_t(R.u4()));
      if (Op == 170) {
        int32_t First = R.u4(), Last = R.u4();
        int64_t N = int64_t(Last) - First + 1;
        if (N <= 0 || uint64_t(N) > R.left() / 4)
          invalid(At, "switch_table", "Invalid tableswitch range");
        J.charge(N, At);
        for (int64_t K = First; K <= Last; ++K) {
          I.Keys.push_back(int32_t(K));
          Target(int32_t(R.u4()));
        }
      } else {
        int32_t N = R.u4();
        if (N < 0 || unsigned(N) > R.left() / 8)
          invalid(At, "switch_pairs", "Invalid lookupswitch pair count");
        J.charge(N, At);
        for (int32_t K = 0; K < N; ++K) {
          int32_t Key = R.u4();
          if (K && Key <= I.Keys.back())
            invalid(At, "switch_order", "lookupswitch keys must increase");
          I.Keys.push_back(Key);
          Target(int32_t(R.u4()));
        }
      }
    } else if ((Op >= 178 && Op <= 187) || Op == 189 || Op == 192 ||
               Op == 193 || Op == 197) {
      I.Pool = R.u2();
      if (Op == 185) {
        I.Operand = R.u1();
        if (R.u1())
          invalid(At, "invoke_reserved", "Nonzero invokeinterface padding");
      }
      if (Op == 186 && R.u2())
        invalid(At, "invoke_reserved", "Nonzero invokedynamic padding");
      if (Op == 197)
        I.Operand = R.u1();
    } else if (Op == 188) {
      I.Operand = R.u1();
      if (I.Operand < 4 || I.Operand > 11)
        invalid(At, "array_type", "Invalid primitive array type");
    }
    if ((Op >= 18 && Op <= 20) || (Op >= 178 && Op <= 187) || Op == 189 ||
        Op == 192 || Op == 193 || Op == 197)
      checkConstant(C, I, At);
    if (Local && I.Local + 1 + unsigned(wideLocal(Op)) > B.MaxLocals)
      invalid(At, "local_slot", "Instruction local index exceeds max_locals");
    if ((Op == 168 || Op == 169 || Op == 201) && C.Major >= 51)
      invalid(At, "legacy_subroutine",
              "jsr/ret is forbidden in this class version");
    I.Width = R.Pos - I.PC;
    bool Next =
        !(Op == 167 || Op == 168 || Op == 169 || Op == 170 || Op == 171 ||
          (Op >= 172 && Op <= 177) || Op == 191 || Op == 200 || Op == 201);
    if (Next && R.left())
      I.Targets.push_back(R.Pos);
    else if (Next)
      invalid(At, "code_fallthrough",
              "Execution falls off the end of the method");
    Out.Index[I.PC] = int(Out.Rows.size());
    Out.Rows.push_back(std::move(I));
  }
  auto boundary = [&](unsigned PC, bool End, size_t At) {
    if ((End && PC == B.Bytes.size()))
      return;
    if (PC >= B.Bytes.size() || Out.Index[PC] < 0)
      invalid(At, "instruction_boundary",
              "Offset enters an operand or padding");
  };
  for (const auto &I : Out.Rows)
    for (unsigned Target : I.Targets)
      boundary(Target, false, B.At + I.PC);
  for (const auto &H : B.Handlers) {
    boundary(H.Start, false, B.At + H.Start);
    boundary(H.End, true, B.At + H.End);
    boundary(H.Target, false, B.At + H.Target);
  }
  for (const auto &F : B.Frames) {
    boundary(F.PC, false, B.At + F.PC);
    auto Type = [&](StringRef T) {
      if (!T.consume_front("new@"))
        return;
      unsigned PC = 0;
      if (T.getAsInteger(10, PC))
        invalid(B.At + F.PC, "stack_map_new", "Invalid uninitialized identity");
      boundary(PC, false, B.At + F.PC);
      if (Out.Rows[Out.Index[PC]].Opcode != 187)
        invalid(B.At + F.PC, "stack_map_new",
                "Uninitialized value does not refer to new");
    };
    for (auto &T : F.Locals)
      Type(T);
    for (auto &T : F.Stack)
      Type(T);
  }
  for (auto [PC, Line] : B.Lines)
    boundary(PC, false, B.At + PC);
  for (auto &L : B.Variables) {
    auto &O = *L.getAsObject();
    boundary(*O.getInteger("start_pc"), false, B.At);
    boundary(*O.getInteger("end_pc"), true, B.At);
  }
  return Out;
}
} // namespace neverd::jvm
