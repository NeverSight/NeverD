//===- X64RtmFaultSource.cpp - Render an observed fault step -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Render the small scalar language produced by the V6 fault writer. Its
/// guards, IDT accesses, frame stores and exits remain the original LowIR.
//===----------------------------------------------------------------------===//

#include "X64RtmCapture.h"

#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <string>
#include <tuple>

namespace neverd {
namespace {

llvm::StringRef binaryToken(NdOp Code) {
  switch (Code) {
  case NdOp::INT_ADD:
    return "+";
  case NdOp::INT_SUB:
    return "-";
  case NdOp::INT_MULT:
    return "*";
  case NdOp::INT_AND:
    return "&";
  case NdOp::INT_OR:
    return "|";
  case NdOp::INT_XOR:
    return "^";
  case NdOp::INT_LEFT:
    return "<<";
  case NdOp::INT_RIGHT:
    return ">>";
  case NdOp::INT_EQUAL:
    return "==";
  case NdOp::INT_NOTEQUAL:
    return "!=";
  case NdOp::INT_LESS:
    return "<";
  case NdOp::BOOL_AND:
    return "&&";
  case NdOp::BOOL_OR:
    return "||";
  default:
    return {};
  }
}

std::string scalarType(unsigned Bytes) {
  return "uint" + std::to_string(Bytes * 8) + "_t";
}

} // namespace

std::optional<std::string> renderX64RtmFaultStep(const LowFunc &F) {
  // Guard and effect writers may reuse a scratch offset at a different width.
  // C locals follow the (space, offset, width) identity in the original ops.
  std::map<std::tuple<VnodeSpace, uint64_t, uint16_t>, unsigned> Locals;
  std::string Source, Statements;
  llvm::raw_string_ostream Decl(Source), Body(Statements);
  Decl << "uint64_t " << F.Name << "(uint8_t *machine_state) {\n";
  const auto Value = [&](NdVar V) -> std::string {
    if (V.isConst())
      return "0x" + llvm::utohexstr(V.Offset) + "ULL";
    if (V == NdVar::reg(x86reg::RDI, 8))
      return "(uintptr_t)machine_state";
    const auto [It, Inserted] =
        Locals.try_emplace({V.Space, V.Offset, V.Size}, Locals.size());
    const auto Name = "v" + std::to_string(It->second);
    if (Inserted)
      Decl << "    " << scalarType(V.Size) << " " << Name << ";\n";
    return Name;
  };
  for (const LowBlock &Block : F.Blocks) {
    Body << "block_" << Block.Id << ":\n";
    for (const LowOp &Op : Block.Ops) {
      const auto A = [&] { return Value(Op.Inputs[0]); };
      const auto B = [&] { return Value(Op.Inputs[1]); };
      auto Token = binaryToken(Op.Opcode);
      Body << "    ";
      if (!Token.empty()) {
        // A uint64_t first operand also keeps the C integer promotions from
        // introducing signed arithmetic for narrower LowIR values.
        Body << Value(Op.Output) << " = (uint64_t)" << A() << ' ' << Token
             << ' ' << B() << ";\n";
        continue;
      }
      switch (Op.Opcode) {
      case NdOp::COPY:
      case NdOp::INT_ZEXT:
        Body << Value(Op.Output) << " = " << A() << ";\n";
        break;
      case NdOp::SUBBYTES:
        if (!Op.Inputs[1].isConst() || Op.Inputs[1].Offset != 0)
          return std::nullopt;
        Body << Value(Op.Output) << " = (" << scalarType(Op.Output.Size) << ")"
             << A() << ";\n";
        break;
      case NdOp::INT_ASHR:
        // The IA-32e canonical address test sign-extends a 64-bit value.
        if (Op.Inputs[0].Size != 8)
          return std::nullopt;
        Body << Value(Op.Output) << " = (int64_t)" << A() << " >> " << B()
             << ";\n";
        break;
      case NdOp::SELECT:
        Body << Value(Op.Output) << " = " << A() << " ? " << B() << " : "
             << Value(Op.Inputs[2]) << ";\n";
        break;
      case NdOp::LOAD:
        Body << "__builtin_memcpy(&" << Value(Op.Output)
             << ", (const void *)(uintptr_t)" << A() << ", " << Op.Output.Size
             << ");\n";
        break;
      case NdOp::STORE:
        Body << "{ " << scalarType(Op.Inputs[1].Size) << " value = " << B()
             << "; __builtin_memcpy((void *)(uintptr_t)" << A()
             << ", &value, sizeof(value)); }\n";
        break;
      case NdOp::BRANCH:
        Body << "goto block_" << Block.Succs[0] << ";\n";
        break;
      case NdOp::COND_BR:
        Body << "if (" << B() << ") goto block_" << Block.Succs[1] << ";\n";
        if (Block.Succs[0] != Block.Id + 1)
          Body << "    goto block_" << Block.Succs[0] << ";\n";
        break;
      case NdOp::RETURN:
        Body << "return " << A() << ";\n";
        break;
      default:
        // A future event operation can always use the full backend.
        return std::nullopt;
      }
    }
  }
  Decl << Statements << "}\n";
  return Source;
}

} // namespace neverd
