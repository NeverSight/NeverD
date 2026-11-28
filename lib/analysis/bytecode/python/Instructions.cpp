//===- Instructions.cpp - CPython 3.10 wordcode and control flow ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decode disk wordcode, retain prefixes and line spans, and validate targets
/// against logical instruction starts before publishing successors.
//===----------------------------------------------------------------------===//

#include "Artifacts.h"

#include "llvm/ADT/StringRef.h"

using namespace llvm;
using namespace neverd::python;

namespace {
enum class Domain {
  Invalid,
  None,
  Immediate,
  Relative,
  Absolute,
  Constant,
  Name,
  Local,
  Free,
  Compare
};
Domain domain(Op Opcode) {
  switch (Opcode) {
#define PYTHON_OPCODE(ID, NAME, OPERAND)                                       \
  case Op::NAME:                                                               \
    return Domain::OPERAND;
#include "Opcodes310.def"
#undef PYTHON_OPCODE
  }
  return Domain::Invalid;
}
bool terminal(Op Opcode) {
  return Opcode == Op::RETURN_VALUE || Opcode == Op::RAISE_VARARGS ||
         Opcode == Op::RERAISE;
}
bool handler(Op Opcode) {
  return Opcode == Op::SETUP_FINALLY || Opcode == Op::SETUP_WITH ||
         Opcode == Op::SETUP_ASYNC_WITH;
}
} // namespace
StringRef neverd::python::mnemonic(Op Opcode) {
  switch (Opcode) {
#define PYTHON_OPCODE(ID, NAME, OPERAND)                                       \
  case Op::NAME:                                                               \
    return #NAME;
#include "Opcodes310.def"
#undef PYTHON_OPCODE
  case Op::SmallInteger:
    return "LOAD_SMALL_INT";
  case Op::CommonConstant:
    return "LOAD_COMMON_CONSTANT";
  case Op::PushNull:
    return "PUSH_NULL";
  case Op::CallWithSlot:
    return "CALL";
  }
  return "unknown";
}

void neverd::python::decode(Job &J, Function &F) {
  const Code &C = *F.Node.Body;
  StringRef Bytes = C.Bytes->Data, Table = C.Lines->Data;
  J.charge(J.Instructions, Bytes.size() / 2, limits::kInstructions,
           C.Bytes->DataOffset);
  F.Starts.assign(Bytes.size() / 2, -1);
  F.Code.reserve(Bytes.size() / 2);
  for (unsigned PC = 0; PC < Bytes.size();) {
    unsigned Start = PC;
    Op Opcode;
    uint32_t Arg = 0;
    do {
      if (PC == Bytes.size() || PC - Start > 6)
        malformed(C.Bytes->DataOffset + Start, "Invalid EXTENDED_ARG chain");
      Opcode = static_cast<Op>(uint8_t(Bytes[PC]));
      Arg = (Arg << 8) | uint8_t(Bytes[PC + 1]);
      PC += 2;
    } while (Opcode == Op::EXTENDED_ARG);
    F.Starts[Start / 2] = F.Code.size();
    auto &I = F.Code.emplace_back(Instruction{Opcode, Start, PC, Arg, {}, {}});
    Domain D = domain(Opcode);
    size_t Count = 0;
    switch (D) {
    case Domain::Invalid:
      unsupported(C.Bytes->DataOffset + Start,
                  "Unknown CPython 3.10 opcode " +
                      std::to_string(unsigned(Opcode)));
    case Domain::None:
      if (Arg || PC - Start != 2)
        malformed(C.Bytes->DataOffset + Start,
                  "Unexpected operand on operand-free instruction");
      continue;
    case Domain::Relative:
    case Domain::Absolute: {
      uint64_t To = uint64_t(Arg) * 2 + (D == Domain::Relative ? PC : 0);
      if (To >= Bytes.size())
        malformed(C.Bytes->DataOffset + Start, "Jump leaves code object");
      I.Target = To;
      continue;
    }
    case Domain::Constant:
      Count = C.Consts->Items.size();
      break;
    case Domain::Name:
      Count = C.Names->Items.size();
      break;
    case Domain::Free:
      Count = C.Free->Items.size() + C.Cells->Items.size();
      break;
    case Domain::Local:
      Count = C.Vars->Items.size();
      break;
    case Domain::Compare:
      Count = 6;
      break;
    case Domain::Immediate:
      continue;
    }
    if (Arg >= Count)
      malformed(C.Bytes->DataOffset + Start,
                "Instruction operand outside its code-object table");
  }
  for (const auto &I : F.Code) {
    if (I.Target && F.Starts[*I.Target / 2] < 0)
      malformed(C.Bytes->DataOffset + I.Byte,
                "Jump enters an EXTENDED_ARG operand");
    if (I.End == Bytes.size() && !terminal(I.Opcode) &&
        I.Opcode != Op::JUMP_ABSOLUTE && I.Opcode != Op::JUMP_FORWARD)
      malformed(C.Bytes->DataOffset + I.Byte, "Fallthrough leaves code object");
  }
  if (Table.size() % 2)
    malformed(C.Lines->DataOffset, "Truncated line table");
  int64_t Line = C.FirstLine;
  size_t Offset = 0, Index = 0;
  for (size_t N = 0; N < Table.size(); N += 2) {
    const int Delta = int8_t(Table[N + 1]);
    if (Delta != -128)
      Line += Delta;
    Offset += uint8_t(Table[N]);
    if (Offset > Bytes.size() || Line < 0 || Line > INT32_MAX)
      malformed(C.Lines->DataOffset + N, "Invalid line table extent");
    while (Index < F.Code.size() && F.Code[Index].Byte < Offset) {
      if (Delta != -128)
        F.Code[Index].Line = Line;
      ++Index;
    }
  }
  if (Offset != Bytes.size())
    malformed(C.Lines->DataOffset + Table.size(),
              "Line table does not cover the code object");
  F.Decoded = true;
}

void neverd::python::recoverCode(Job &J, unsigned Id, StringRef Bytes,
                                 size_t At, unsigned Version) {
  Marshal Reader(J, Bytes, At, Version);
  Object &Root = Reader.read();
  if (Root.Type != Object::CodeObject)
    malformed(At, "Expected a marshalled module code object");
  auto &Component = *J.Components[Id].getAsObject();
  if (!Component.getString("dialect"))
    Component["dialect"] = dialectName(Version);
  Component["representation"] =
      Version < 306 ? "variable-width bytecode" : "on-disk wordcode";
  std::vector<Function> Functions;
  Functions.reserve(Reader.codes().size());
  for (const Object *O : Reader.codes()) {
    auto &F = Functions.emplace_back(*O);
    try {
      if (Version == 311)
        decode311(J, F);
      else if (Version == 310)
        decode(J, F);
      else
        decodeRelease(J, F);
    } catch (const Failure &E) {
      if (E.Status != "unsupported" && E.Status != "malformed")
        throw;
      F.Error = E;
    }
  }
  render(J, Functions, Id);
}

InstructionRecord neverd::python::instructionRecord(const Function &F,
                                                    size_t Index) {
  const Instruction &I = F.Code[Index];
  InstructionRecord Row{mnemonic(I.Opcode),
                        F.Node.Body->Bytes->DataOffset + I.Byte,
                        I.Byte,
                        I.End,
                        static_cast<unsigned>(I.Opcode),
                        I.Arg};
  Row.Target = I.Target;
  Row.OriginalLine = I.Line;
  if (Index < F.Lines.size() && F.Lines[Index])
    Row.SourceLine = F.Lines[Index];
  Row.Complete = F.Decoded;
  Row.FallsThrough = !terminal(I.Opcode) && I.Opcode != Op::JUMP_ABSOLUTE &&
                     I.Opcode != Op::JUMP_FORWARD;
  Row.Handler = I.Target && handler(I.Opcode);
  return Row;
}
