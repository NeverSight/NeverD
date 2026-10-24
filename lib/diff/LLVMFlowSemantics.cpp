//===- LLVMFlowSemantics.cpp - Finite scalar operation admission ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Acyclic control flow proves progress only when every operation is finite.
/// Keep that premise shared with the original LLVM projection whitelist.
//===----------------------------------------------------------------------===//

#include "LLVMFlowSemantics.h"

#include "neverd/diff/Program.h"

#include "llvm/ADT/StringSwitch.h"

#include <cstddef>
#include <limits>
#include <vector>

namespace neverd::diff {
LLVMFlowShape integerFlowShape(llvm::StringRef Op) {
  using S = LLVMFlowShape;
  return llvm::StringSwitch<S>(Op)
      .Cases({"add", "sub", "mul", "udiv", "sdiv"}, S::Binary)
      .Cases({"urem", "srem", "shl", "lshr", "ashr"}, S::Binary)
      .Cases({"and", "or", "xor"}, S::Binary)
      .Cases({"trunc", "zext", "sext"}, S::Cast)
      .Case("icmp", S::Compare)
      .Case("select", S::Select)
      .Case("freeze", S::Unary)
      .Default(S::Unsupported);
}
LLVMFlowShape intrinsicFlowShape(llvm::Intrinsic::ID ID) {
  using namespace llvm;
  switch (ID) {
  case Intrinsic::fshl:
  case Intrinsic::fshr:
    return LLVMFlowShape::Ternary;
  case Intrinsic::bswap:
  case Intrinsic::bitreverse:
  case Intrinsic::ctpop:
    return LLVMFlowShape::Unary;
  case Intrinsic::ctlz:
  case Intrinsic::cttz:
  case Intrinsic::abs:
    return LLVMFlowShape::BoolArgument;
  case Intrinsic::smin:
  case Intrinsic::smax:
  case Intrinsic::umin:
  case Intrinsic::umax:
  case Intrinsic::sadd_sat:
  case Intrinsic::uadd_sat:
  case Intrinsic::ssub_sat:
  case Intrinsic::usub_sat:
    return LLVMFlowShape::Binary;
  default:
    return LLVMFlowShape::Unsupported;
  }
}
namespace {
std::optional<unsigned> bits(llvm::StringRef Type, bool Void = false) {
  unsigned Width = 0;
  if (!Type.consume_front("i") || Type.getAsInteger(10, Width) ||
      (!Width && !Void) || Width > std::numeric_limits<uint64_t>::digits ||
      Type != std::to_string(Width))
    return {};
  return Width;
}
} // namespace

std::optional<std::string>
validateFiniteLLVMFlow(const DataFlowGraph &G, llvm::StringRef Contract,
                       llvm::function_ref<void(uint64_t)> Charge) {
  Charge(G.Nodes.size());
  std::vector<unsigned> Widths;
  for (const auto &N : G.Nodes) {
    Charge(1 + N.Semantics.size());
    llvm::StringRef S(N.Semantics);
    if (!S.consume_front(Contract) || !S.consume_front(";result="))
      return "progress operation has a different function contract";
    const auto [Type, Flags] = S.split(';');
    const auto Width = bits(Type, true);
    if (!Width || !Flags.starts_with("flags=") ||
        N.ResultBytes != (*Width + 7) / 8 || N.MemoryOrdering ||
        N.MemoryAddressSpace || N.CallArgumentBegin != NoIndex)
      return "progress operation has no finite scalar contract";
    Widths.push_back(*Width);
  }
  for (uint32_t I = 0; I < G.Nodes.size(); ++I) {
    const auto &N = G.Nodes[I];
    const auto W = Widths[I];
    Charge(1 + N.Inputs.size());
    std::vector<unsigned> Inputs;
    for (const auto &Input : N.Inputs) {
      Charge(Input.Semantics.size());
      const auto B = bits(llvm::StringRef(Input.Semantics).split(';').first);
      if (!B || Input.Bytes != (*B + 7) / 8 || Input.Provenance ||
          Input.AddressOwner ||
          (Input.Type == FlowOperand::Kind::Node &&
           (Input.Node >= Widths.size() || Widths[Input.Node] != *B)) ||
          (Input.Type == FlowOperand::Kind::Constant &&
           *B < std::numeric_limits<uint64_t>::digits && Input.Value >> *B))
        return "progress operand has an inconsistent scalar type";
      Inputs.push_back(*B);
    }
    if (N.Block >= G.Blocks.size())
      return "progress operation has no owning block";
    const auto &B = G.Blocks[N.Block];
    const bool Terminator = B.Terminator == I;
    if (N.Opcode == "ret" || N.Opcode == "br" || N.Opcode == "switch") {
      if (!Terminator || W || N.Phi)
        return "progress control operation is not its block terminator";
      if (N.Opcode == "ret") {
        if (Inputs.size() > 1 || !B.Successors.empty())
          return "progress return has invalid operands or successors";
      } else if (N.Opcode == "br") {
        if (Inputs.size() > 1 || B.Successors.size() != Inputs.size() + 1 ||
            (!Inputs.empty() && Inputs[0] != 1))
          return "progress branch has invalid operands or successors";
      } else {
        if (Inputs.empty() || B.Successors.size() != Inputs.size())
          return "progress switch has invalid operands or successors";
        for (size_t K = 1; K < Inputs.size(); ++K)
          if (Inputs[K] != Inputs[0] ||
              N.Inputs[K].Type != FlowOperand::Kind::Constant)
            return "progress switch has an invalid case";
      }
      continue;
    }
    if (!W || Terminator || (N.Phi != (N.Opcode == "phi")))
      return "progress value has an invalid result or control role";
    if (N.Phi) {
      if (Inputs.empty())
        return "progress PHI has no inputs";
      for (auto Width : Inputs)
        if (Width != W)
          return "progress PHI has inconsistent widths";
      continue;
    }
    auto Shape = integerFlowShape(N.Opcode);
    if (Shape == LLVMFlowShape::Unsupported) {
      const auto ID = llvm::Intrinsic::lookupIntrinsicID(N.Opcode);
      Shape = intrinsicFlowShape(ID);
      if (Shape == LLVMFlowShape::Unsupported ||
          N.Opcode !=
              llvm::Intrinsic::getBaseName(ID).str() + ".i" + std::to_string(W))
        return "progress body contains an unmodelled or nonfinite operation";
    }
    const size_t Arity =
        Shape == LLVMFlowShape::Unary || Shape == LLVMFlowShape::Cast       ? 1
        : Shape == LLVMFlowShape::Select || Shape == LLVMFlowShape::Ternary ? 3
                                                                            : 2;
    if (Inputs.size() != Arity)
      return "progress operation has invalid arity";
    if (Shape == LLVMFlowShape::Cast) {
      if (N.Opcode == "trunc" ? W >= Inputs[0] : W <= Inputs[0])
        return "progress cast has invalid widths";
    } else if (Shape == LLVMFlowShape::Compare) {
      if (W != 1 || Inputs[0] != Inputs[1])
        return "progress comparison has invalid widths";
    } else {
      for (size_t K = 0; K < Inputs.size(); ++K) {
        const auto Expected =
            (Shape == LLVMFlowShape::Select && K == 0) ||
                    (Shape == LLVMFlowShape::BoolArgument && K == 1)
                ? 1
                : W;
        if (Inputs[K] != Expected)
          return "progress operation has inconsistent widths";
      }
    }
  }
  for (const auto &B : G.Blocks) {
    Charge(1);
    if (B.Terminator >= G.Nodes.size())
      return "progress block has no explicit finite terminator";
  }
  return {};
}
} // namespace neverd::diff
