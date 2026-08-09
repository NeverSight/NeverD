//===- SolidityEmitter.cpp - EVM to recovered Solidity backend ----------===//

#include "neverd/evm/SolidityEmitter.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <set>

namespace neverd::evm {
namespace {

llvm::StringRef solidityExitStatusName(ExitStatus Status) {
  switch (Status) {
#define EVM_EXIT_STATUS(NAME, C_NAME, VALUE)                                   \
  case ExitStatus::NAME:                                                      \
    return #C_NAME;
#include "neverd/evm/EVMExitStatuses.def"
  }
  llvm_unreachable("invalid EVM exit status");
}

std::string solidityWord(const llvm::APInt &Value) {
  llvm::SmallString<kWordBytes * kHexDigitsPerByte> Digits;
  Value.toStringUnsigned(Digits, 16);
  return "uint256(0x" + Digits.str().str() + ")";
}

std::string mutabilityText(Mutability MutabilityValue) {
  switch (MutabilityValue) {
  case Mutability::Pure:
    return " pure";
  case Mutability::View:
    return " view";
  case Mutability::Payable:
    return " payable";
  case Mutability::NonPayable:
    return "";
  }
  return "";
}

std::string hostExpression(Opcode Op) {
  return "_evmHost(0x" + llvm::utohexstr(opcodeByte(Op)) +
         ", args_, input)";
}

bool isInlinePure(Opcode Op) {
  const auto Info = opcodeInfo(Op);
  return Info && (Info->Class == OpcodeClass::Arithmetic ||
                  Info->Class == OpcodeClass::Comparison ||
                  Info->Class == OpcodeClass::Bitwise);
}

std::string pureExpression(Opcode Op) {
  switch (Op) {
  case Opcode::ADD:
    return "args_[0] + args_[1]";
  case Opcode::MUL:
    return "args_[0] * args_[1]";
  case Opcode::SUB:
    return "args_[0] - args_[1]";
  case Opcode::DIV:
    return "args_[1] == 0 ? 0 : args_[0] / args_[1]";
  case Opcode::SDIV:
    return "_evmSDiv(args_[0], args_[1])";
  case Opcode::MOD:
    return "args_[1] == 0 ? 0 : args_[0] % args_[1]";
  case Opcode::SMOD:
    return "_evmSMod(args_[0], args_[1])";
  case Opcode::ADDMOD:
    return "addmod(args_[0], args_[1], args_[2])";
  case Opcode::MULMOD:
    return "mulmod(args_[0], args_[1], args_[2])";
  case Opcode::EXP:
    return "args_[0] ** args_[1]";
  case Opcode::SIGNEXTEND:
    return "_evmSignExtend(args_[0], args_[1])";
  case Opcode::LT:
    return "args_[0] < args_[1] ? 1 : 0";
  case Opcode::GT:
    return "args_[0] > args_[1] ? 1 : 0";
  case Opcode::SLT:
    return "int256(args_[0]) < int256(args_[1]) ? 1 : 0";
  case Opcode::SGT:
    return "int256(args_[0]) > int256(args_[1]) ? 1 : 0";
  case Opcode::EQ:
    return "args_[0] == args_[1] ? 1 : 0";
  case Opcode::ISZERO:
    return "args_[0] == 0 ? 1 : 0";
  case Opcode::AND:
    return "args_[0] & args_[1]";
  case Opcode::OR:
    return "args_[0] | args_[1]";
  case Opcode::XOR:
    return "args_[0] ^ args_[1]";
  case Opcode::NOT:
    return "~args_[0]";
  case Opcode::BYTE:
    return "_evmByte(args_[0], args_[1])";
  case Opcode::SHL:
    return "args_[0] >= _EVM_WORD_BITS ? 0 : args_[1] << args_[0]";
  case Opcode::SHR:
    return "args_[0] >= _EVM_WORD_BITS ? 0 : args_[1] >> args_[0]";
  case Opcode::SAR:
    return "_evmSar(args_[0], args_[1])";
  case Opcode::CLZ:
    return "_evmClz(args_[0])";
  default:
    return "0";
  }
}

bool hasPC(const EVMProgram &Program, uint64_t PC) {
  for (const auto &Instruction : Program.Low.Instructions)
    if (Instruction.PC == PC)
      return true;
  return false;
}

void emitAdvance(llvm::raw_ostream &OS, const EVMProgram &Program,
                 uint64_t NextPC) {
  if (hasPC(Program, NextPC))
    OS << "        pc = " << NextPC << "; continue;\n";
  else
    OS << "        return " << solidityExitStatusName(ExitStatus::Stopped)
       << ";\n";
}

} // namespace

llvm::Expected<std::string> emitSolidity(const EVMProgram &Program,
                                         SolidityEmitterOptions Options) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << "// SPDX-License-Identifier: UNLICENSED\n"
     << "pragma solidity " << Options.Pragma
     << ";\n\n"
        "/**\n"
        " * @notice Generated semantic reconstruction of EVM runtime "
        "bytecode.\n"
        " * @dev Names and ABI types marked recovered are heuristics; this "
        "file\n"
        " *      does not claim to reproduce the original Solidity source.\n"
        " *      Override _evmHost to supply memory, storage, calldata, "
        "hashing,\n"
        " *      call, log, return/revert, and blockchain-environment "
        "effects.\n"
        " *      args_[0] is the original stack top; the hook returns the "
        "first\n"
        " *      value pushed by an opcode and may implement side effects.\n"
        " */\n"
     << "abstract contract " << Options.ContractName
     << " {\n"
        "    error EVMStackUnderflow(uint256 pc);\n"
        "    error EVMStackOverflow(uint256 pc);\n"
        "    error EVMInvalidJump(uint256 destination);\n"
        "    error EVMUnsupportedOpcode(uint256 pc, uint8 opcode);\n"
        "    error EVMExecutionReverted();\n"
        "    event EVMTrace(uint256 indexed pc, uint8 opcode);\n\n"
     << "    uint256 private constant _EVM_BITS_PER_BYTE = " << kBitsPerByte
     << ";\n"
     << "    uint256 private constant _EVM_BYTE_MAX = " << kByteMax << ";\n"
     << "    uint256 private constant _EVM_WORD_BITS = " << kWordBits
     << ";\n"
     << "    uint256 private constant _EVM_WORD_BYTES = " << kWordBytes
     << ";\n"
     << "    uint256 private constant _EVM_WORD_MAX_BYTE_INDEX = "
     << kWordMaxByteIndex << ";\n"
     << "    uint256 private constant _EVM_WORD_MSB = "
     << kWordMostSignificantBit << ";\n"
     << "    uint256 private constant _EVM_STACK_LIMIT = " << kStackLimit
     << ";\n";
#define EVM_EXIT_STATUS(NAME, C_NAME, VALUE)                                   \
  OS << "    uint8 private constant " #C_NAME " = "                          \
     << static_cast<unsigned>(exitStatusCode(ExitStatus::NAME)) << ";\n";
#include "neverd/evm/EVMExitStatuses.def"
  OS << "\n";

  if (Options.EmitRecoveredDeclarations) {
    std::set<std::string> StorageNames;
    for (const auto &Fact : Program.High.Storage) {
      if (!Fact.Slot)
        continue;
      const std::string Name = "recovered_" + Fact.SuggestedName;
      if (StorageNames.insert(Name).second)
        OS << "    // Recovered access to absolute EVM slot "
           << solidityWord(*Fact.Slot) << ".\n"
           << "    uint256 internal " << Name << ";\n";
    }
    if (!StorageNames.empty())
      OS << "\n";

    std::set<std::string> EventNames;
    for (const auto &Fact : Program.High.Events)
      if (EventNames.insert(Fact.SuggestedName).second)
        OS << "    event " << Fact.SuggestedName
           << "(bytes data); // recovered LOG" << Fact.Topics << "\n";
    std::set<std::string> ErrorNames;
    for (const auto &Fact : Program.High.Errors)
      if (ErrorNames.insert(Fact.SuggestedName).second)
        OS << "    error " << Fact.SuggestedName << "();\n";
    if (!EventNames.empty() || !ErrorNames.empty())
      OS << "\n";

    for (const auto &Function : Program.High.Functions) {
      OS << "    // recovered selector 0x"
         << llvm::format_hex_no_prefix(Function.Selector, kSelectorHexDigits,
                                       false)
         << ", entry pc 0x" << llvm::utohexstr(Function.EntryPC) << "\n"
         << "    function " << Function.Name << "(";
      for (size_t I = 0; I < Function.Arguments.size(); ++I) {
        if (I)
          OS << ", ";
        OS << Function.Arguments[I].Type << " " << Function.Arguments[I].Name;
      }
      OS << ") external" << mutabilityText(Function.StateMutability)
         << " virtual";
      if (!Function.Returns.empty()) {
        OS << " returns (";
        for (size_t I = 0; I < Function.Returns.size(); ++I) {
          if (I)
            OS << ", ";
          OS << Function.Returns[I];
        }
        OS << ")";
      }
      OS << ";\n\n";
    }
  }

  OS << "    function _evmHost(uint8 opcode, uint256["
     << static_cast<unsigned>(maxHostOpcodeArguments())
     << "] memory args_, bytes memory input) internal virtual returns "
        "(uint256);\n\n"
        "    function _evmTrace(uint256 pc, uint8 opcode) internal virtual {\n"
        "        emit EVMTrace(pc, opcode);\n"
        "    }\n\n"
        "    function _evmPush(uint256["
     << kStackLimit
     << "] memory stack_, uint256 sp, "
        "uint256 value, uint256 pc) internal pure returns (uint256) {\n"
        "        if (sp >= _EVM_STACK_LIMIT) revert EVMStackOverflow(pc);\n"
        "        stack_[sp] = value; return sp + 1;\n"
        "    }\n\n"
        "    function _evmPop(uint256["
     << kStackLimit
     << "] memory stack_, uint256 sp, "
        "uint256 pc) internal pure returns (uint256, uint256) {\n"
        "        if (sp == 0) revert EVMStackUnderflow(pc);\n"
        "        unchecked { --sp; } return (sp, stack_[sp]);\n"
        "    }\n\n"
        "    function _evmSDiv(uint256 a, uint256 b) internal pure returns "
        "(uint256) { int256 x = int256(a); int256 y = int256(b); "
        "if (y == 0) return 0; if (x == type(int256).min && y == -1) return a; "
        "return uint256(x / y); }\n"
        "    function _evmSMod(uint256 a, uint256 b) internal pure returns "
        "(uint256) { int256 x = int256(a); int256 y = int256(b); "
        "if (y == 0) return 0; if (x == type(int256).min && y == -1) return 0; "
        "return uint256(x % y); }\n"
        "    function _evmSignExtend(uint256 k, uint256 v) internal pure "
        "returns (uint256 r) { assembly { r := signextend(k, v) } }\n"
        "    function _evmByte(uint256 k, uint256 v) internal pure returns "
        "(uint256) { return k >= _EVM_WORD_BYTES ? 0 : (v >> "
        "((_EVM_WORD_MAX_BYTE_INDEX - k) * _EVM_BITS_PER_BYTE)) & "
        "_EVM_BYTE_MAX; }\n"
        "    function _evmSar(uint256 s, uint256 v) internal pure returns "
        "(uint256) { if (s >= _EVM_WORD_BITS) return int256(v) < 0 ? "
        "type(uint256).max : "
        "0; "
        "return uint256(int256(v) >> s); }\n"
        "    function _evmClz(uint256 v) internal pure returns (uint256 n) { "
        "if (v == 0) return _EVM_WORD_BITS; while ((v >> _EVM_WORD_MSB) == 0) "
        "{ unchecked { ++n; "
        "v <<= 1; } } }\n\n"
        "    function execute(bytes calldata input) external payable returns "
        "(uint8) { return _executeEVM(input); }\n\n"
        "    fallback() external payable {\n"
        "        uint8 status = _executeEVM(msg.data);\n"
        "        if (status == NEVERD_EVM_REVERTED) "
        "revert EVMExecutionReverted();\n"
        "    }\n\n"
        "    receive() external payable {\n"
        "        uint8 status = _executeEVM(bytes(\"\"));\n"
        "        if (status == NEVERD_EVM_REVERTED) "
        "revert EVMExecutionReverted();\n"
        "    }\n\n"
        "    function _executeEVM(bytes memory input) internal returns "
        "(uint8 status) {\n"
        "        uint256["
     << kStackLimit
     << "] memory evmStack;\n"
        "        uint256 evmSP = 0;\n"
        "        uint256 pc = 0;\n"
        "        unchecked {\n"
        "        while (true) {\n";

  for (size_t Index = 0; Index < Program.Low.Instructions.size(); ++Index) {
    const auto &Instruction = Program.Low.Instructions[Index];
    const uint64_t PC = Instruction.PC;
    const Opcode Op = Instruction.Op;
    OS << "            " << (Index == 0 ? "if" : "else if") << " (pc == " << PC
       << ") { // " << Instruction.Info.Name << "\n";
    if (Options.EmitTraceEvents)
      OS << "                _evmTrace(" << PC << ", 0x"
         << llvm::utohexstr(opcodeByte(Op)) << ");\n";

    if (!Instruction.Known || Op == Opcode::INVALID) {
      OS << "                revert EVMUnsupportedOpcode(pc, 0x"
         << llvm::utohexstr(opcodeByte(Op)) << ");\n            }\n";
      continue;
    }
    if (Op == Opcode::STOP) {
      OS << "                return "
         << solidityExitStatusName(ExitStatus::Stopped)
         << ";\n            }\n";
      continue;
    }
    if (isPush(Op)) {
      OS << "                evmSP = _evmPush(evmStack, evmSP, "
         << solidityWord(Instruction.Immediate) << ", pc);\n";
      emitAdvance(OS, Program, Instruction.NextPC);
      OS << "            }\n";
      continue;
    }
    if (isDup(Op)) {
      const unsigned Depth = dupDepth(Op);
      OS << "                if (evmSP < " << Depth
         << ") revert EVMStackUnderflow(pc);\n"
            "                evmSP = _evmPush(evmStack, evmSP, evmStack[evmSP "
            "- "
         << Depth << "], pc);\n";
      emitAdvance(OS, Program, Instruction.NextPC);
      OS << "            }\n";
      continue;
    }
    if (isSwap(Op)) {
      const unsigned Depth = swapDepth(Op);
      OS << "                if (evmSP <= " << Depth
         << ") revert EVMStackUnderflow(pc);\n"
            "                uint256 swapValue = evmStack[evmSP - 1];\n"
            "                evmStack[evmSP - 1] = evmStack[evmSP - "
         << Depth + 1 << "];\n                evmStack[evmSP - " << Depth + 1
         << "] = swapValue;\n";
      emitAdvance(OS, Program, Instruction.NextPC);
      OS << "            }\n";
      continue;
    }
    if (isJump(Op)) {
      OS << "                uint256 destination;\n"
            "                (evmSP, destination) = _evmPop(evmStack, evmSP, "
            "pc);\n";
      if (Op == Opcode::JUMPI) {
        OS << "                uint256 condition;\n"
              "                (evmSP, condition) = _evmPop(evmStack, evmSP, "
              "pc);\n"
              "                if (condition == 0) { pc = "
           << Instruction.NextPC << "; continue; }\n";
      }
      bool FirstTarget = true;
      for (uint64_t Target : Program.Low.JumpDestinations) {
        OS << "                " << (FirstTarget ? "if" : "else if")
           << " (destination == " << Target << ") pc = " << Target << ";\n";
        FirstTarget = false;
      }
      OS << "                else revert EVMInvalidJump(destination);\n"
            "                continue;\n            }\n";
      continue;
    }

    if (Instruction.Info.StackInputs != 0)
      OS << "                uint256["
         << static_cast<unsigned>(maxHostOpcodeArguments())
         << "] memory args_;\n";
    else if (!isInlinePure(Op) && Op != Opcode::PC && Op != Opcode::CODESIZE)
      OS << "                uint256["
         << static_cast<unsigned>(maxHostOpcodeArguments())
         << "] memory args_;\n";
    for (uint8_t I = 0; I < Instruction.Info.StackInputs; ++I)
      OS << "                (evmSP, args_[" << static_cast<unsigned>(I)
         << "]) = _evmPop(evmStack, evmSP, pc);\n";

    if (Op == Opcode::POP || Op == Opcode::JUMPDEST) {
      emitAdvance(OS, Program, Instruction.NextPC);
      OS << "            }\n";
      continue;
    }
    if (Op == Opcode::PC || Op == Opcode::CODESIZE) {
      const uint64_t Value =
          Op == Opcode::PC ? PC : Program.Low.Code.size();
      OS << "                evmSP = _evmPush(evmStack, evmSP, " << Value
         << ", pc);\n";
      emitAdvance(OS, Program, Instruction.NextPC);
      OS << "            }\n";
      continue;
    }

    const std::string Output =
        isInlinePure(Op) ? pureExpression(Op) : hostExpression(Op);
    if (Instruction.Info.StackOutputs != 0)
      OS << "                uint256 result = " << Output << ";\n";
    else if (!isInlinePure(Op))
      OS << "                " << Output << ";\n";

    if (Op == Opcode::RETURN || Op == Opcode::REVERT ||
        Op == Opcode::SELFDESTRUCT) {
      OS << "                return "
         << (Op == Opcode::RETURN
                 ? solidityExitStatusName(ExitStatus::Returned)
                 : Op == Opcode::REVERT
                       ? solidityExitStatusName(ExitStatus::Reverted)
                       : solidityExitStatusName(ExitStatus::SelfDestructed))
         << ";\n            }\n";
      continue;
    }
    if (Instruction.Info.IsTerminator) {
      OS << "                return "
         << solidityExitStatusName(ExitStatus::Stopped)
         << ";\n            }\n";
      continue;
    }
    for (uint8_t I = 0; I < Instruction.Info.StackOutputs; ++I)
      OS << "                evmSP = _evmPush(evmStack, evmSP, "
         << (I == 0 ? "result" : "0") << ", pc);\n";
    emitAdvance(OS, Program, Instruction.NextPC);
    OS << "            }\n";
  }
  OS << "            else { revert EVMInvalidJump(pc); }\n"
        "        }\n"
        "        }\n"
        "    }\n"
        "}\n";
  return Text;
}

} // namespace neverd::evm
