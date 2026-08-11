//===- SolidityEmitter.cpp - EVM to recovered Solidity backend ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/SolidityEmitter.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <set>

namespace neverd::evm {
namespace {

inline constexpr llvm::StringLiteral kSolidityHostFunctionName = "_evmHost";
inline constexpr llvm::StringLiteral kSolidityTraceFunctionName = "_evmTrace";
inline constexpr llvm::StringLiteral kSolidityPushFunctionName = "_evmPush";
inline constexpr llvm::StringLiteral kSolidityPopFunctionName = "_evmPop";
inline constexpr llvm::StringLiteral kSoliditySwapFunctionName = "_evmSwap";
inline constexpr llvm::StringLiteral kSolidityExecuteFunctionName =
    "_executeEVM";

llvm::StringRef solidityExitStatusName(ExitStatus Status) {
  switch (Status) {
#define EVM_EXIT_STATUS(NAME, C_NAME, VALUE)                                   \
  case ExitStatus::NAME:                                                       \
    return #C_NAME;
#include "neverd/evm/EVMExitStatuses.def"
  }
  llvm_unreachable("invalid EVM exit status");
}

std::string solidityWord(const llvm::APInt &Value) {
  llvm::SmallString<kWordBytes * kHexDigitsPerByte> Digits;
  Value.toStringUnsigned(Digits, kHexRadix);
  return "uint256(0x" + Digits.str().str() + ")";
}

llvm::StringRef mutabilityText(Mutability MutabilityValue) {
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
  llvm_unreachable("invalid recovered Solidity mutability");
}

/// True when Solidity can write the type inline. A tuple needs a named struct,
/// which recovery has no way to invent, so a declaration that would contain
/// one is reported as its signature instead.
bool isInlineSpellable(llvm::StringRef Type) {
  return Type.find('(') == llvm::StringRef::npos;
}

/// The data location Solidity requires for a parameter of this type, empty
/// when it requires none. Only reference types carry one.
llvm::StringRef dataLocation(llvm::StringRef Type, llvm::StringRef Location) {
  const bool IsReference =
      Type == "bytes" || Type == "string" || Type.ends_with("]");
  return IsReference ? Location : llvm::StringRef();
}

bool allInlineSpellable(const RecoveredFunction &Function) {
  return llvm::all_of(Function.Arguments,
                      [](const RecoveredArgument &Argument) {
                        return isInlineSpellable(Argument.Type);
                      }) &&
         llvm::all_of(Function.Returns, [](const std::string &Type) {
           return isInlineSpellable(Type);
         });
}

std::string hostExpression(Opcode Op) {
  return kSolidityHostFunctionName.str() + "(0x" +
         llvm::utohexstr(opcodeByte(Op)) + ", args_, input)";
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
    return "args_[2] == 0 ? 0 : addmod(args_[0], args_[1], args_[2])";
  case Opcode::MULMOD:
    return "args_[2] == 0 ? 0 : mulmod(args_[0], args_[1], args_[2])";
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
    llvm_unreachable("unhandled EVM ALU opcode in Solidity backend");
  }
}

std::string advanceStatement(const llvm::DenseSet<uint64_t> &InstructionPCs,
                             uint64_t NextPC) {
  if (InstructionPCs.contains(NextPC))
    return "pc = " + std::to_string(NextPC) + "; continue;";
  return "return " + solidityExitStatusName(ExitStatus::Stopped).str() + ";";
}

void emitAdvance(llvm::raw_ostream &OS,
                 const llvm::DenseSet<uint64_t> &InstructionPCs,
                 uint64_t NextPC) {
  if (InstructionPCs.contains(NextPC))
    OS << "                pc = " << NextPC << "; continue;\n";
  else
    OS << "                return "
       << solidityExitStatusName(ExitStatus::Stopped) << ";\n";
}

} // namespace

llvm::Expected<std::string>
emitSolidity(const EVMProgram &Program, const SolidityEmitterOptions &Options) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  llvm::DenseSet<uint64_t> InstructionPCs;
  InstructionPCs.reserve(Program.Low.Instructions.size());
  for (const LowInstruction &Instruction : Program.Low.Instructions)
    InstructionPCs.insert(Instruction.PC);
  OS << "// SPDX-License-Identifier: UNLICENSED\n"
     << "pragma solidity " << Options.Pragma
     << ";\n\n"
        "/**\n"
        " * @notice Generated semantic reconstruction of EVM runtime "
        "bytecode.\n"
        " * @dev Names and ABI types marked recovered are heuristics; this "
        "file\n"
        " *      does not claim to reproduce the original Solidity source.\n"
        " *      Override "
     << kSolidityHostFunctionName
     << " to supply memory, storage, calldata, hashing,\n"
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
     << "    uint256 private constant _EVM_WORD_BITS = " << kWordBits << ";\n"
     << "    uint256 private constant _EVM_WORD_BYTES = " << kWordBytes << ";\n"
     << "    uint256 private constant _EVM_WORD_MAX_BYTE_INDEX = "
     << kWordMaxByteIndex << ";\n"
     << "    uint256 private constant _EVM_WORD_MSB = "
     << kWordMostSignificantBit << ";\n"
     << "    uint256 private constant _EVM_STACK_LIMIT = " << kStackLimit
     << ";\n";
#define EVM_EXIT_STATUS(NAME, C_NAME, VALUE)                                   \
  OS << "    uint8 private constant " #C_NAME " = "                            \
     << static_cast<unsigned>(exitStatusCode(ExitStatus::NAME)) << ";\n";
#include "neverd/evm/EVMExitStatuses.def"
  OS << "\n";

  if (Options.EmitRecoveredDeclarations) {
    // What a program calls is as much a part of what it does as what it
    // answers to. A set of selectors is still not an interface, though:
    // nothing here says the callee declares only these, so they are reported
    // rather than declared.
    std::set<std::string> OutgoingCalls;
    for (const auto &Fact : Program.High.Calls) {
      std::string Line = opcodeName(Fact.Op).str() + " " +
                         calleeKindName(Fact.TargetKind).str() + " " +
                         Fact.SuggestedName;
      if (Fact.Known)
        Line += " " + Fact.Known->Signature.str();
      if (Fact.Target)
        Line += " at " + solidityWord(*Fact.Target);
      else if (Fact.NamedSlot)
        Line += " at " + Fact.NamedSlot->Name.str();
      else if (Fact.Slot)
        Line += " at slot " + solidityWord(*Fact.Slot);
      OutgoingCalls.insert(std::move(Line));
    }
    for (const std::string &Line : OutgoingCalls)
      OS << "    // calls out: " << Line << "\n";
    if (!OutgoingCalls.empty())
      OS << "\n";

    std::set<std::string> StorageNames;
    for (const auto &Fact : Program.High.Storage) {
      if (!Fact.Slot)
        continue;
      const std::string Name =
          kRecoveredDeclarationPrefix.str() + Fact.SuggestedName;
      if (StorageNames.insert(Name).second)
        OS << "    // Recovered access to absolute EVM slot "
           << solidityWord(*Fact.Slot) << ".\n"
           << "    uint256 internal constant " << Name << " = "
           << solidityWord(*Fact.Slot) << ";\n";
    }
    if (!StorageNames.empty())
      OS << "\n";

    std::set<std::string> EventNames;
    for (const auto &Fact : Program.High.Events) {
      if (!EventNames.insert(Fact.SuggestedName).second)
        continue;
      // A log carries one topic for the signature and one for each indexed
      // parameter, so the topic count says how many of the leading parameters
      // the source marked indexed.
      const auto Declared = Fact.Known
                                ? signatureArgumentTypes(Fact.Known->Signature)
                                : llvm::SmallVector<llvm::StringRef, 8>{};
      if (!Fact.Known || !llvm::all_of(Declared, isInlineSpellable)) {
        OS << "    event " << Fact.SuggestedName
           << "(bytes data); // recovered LOG" << Fact.Topics;
        if (Fact.Known)
          OS << " " << Fact.Known->Signature;
        OS << "\n";
        continue;
      }
      OS << "    event " << Fact.SuggestedName << "(";
      for (size_t I = 0; I < Declared.size(); ++I) {
        if (I)
          OS << ", ";
        OS << Declared[I] << (I + 1 < Fact.Topics ? " indexed" : "");
      }
      OS << ");\n";
    }

    std::set<std::string> ErrorNames;
    for (const auto &Fact : Program.High.Errors) {
      // The two payloads the language reserves are already declared by the
      // language, so redeclaring them would not compile.
      if (Fact.Kind == RevertKind::Message || Fact.Kind == RevertKind::Panic)
        continue;
      if (!ErrorNames.insert(Fact.SuggestedName).second)
        continue;
      if (Fact.Known && isInlineSpellable(Fact.Known->Signature))
        OS << "    error " << Fact.Known->Signature << ";\n";
      else
        OS << "    error " << Fact.SuggestedName << "();\n";
    }
    if (!EventNames.empty() || !ErrorNames.empty())
      OS << "\n";

    for (const auto &Function : Program.High.Functions) {
      OS << "    // recovered selector 0x"
         << llvm::format_hex_no_prefix(Function.Selector, kSelectorHexDigits,
                                       false)
         << ", entry pc 0x" << llvm::utohexstr(Function.EntryPC) << "\n";
      if (Function.Known)
        OS << "    // hashed signature " << Function.Known->Signature << " ("
           << getKnownStandardInfo(Function.Known->Standard).Name << ")\n";
      if (!allInlineSpellable(Function)) {
        OS << "    // no declaration: the signature contains a tuple, which "
              "needs a named struct\n\n";
        continue;
      }
      OS << "    function " << Function.Name << "(";
      for (size_t I = 0; I < Function.Arguments.size(); ++I) {
        if (I)
          OS << ", ";
        OS << Function.Arguments[I].Type
           << dataLocation(Function.Arguments[I].Type, " calldata") << " "
           << Function.Arguments[I].Name;
      }
      OS << ") external" << mutabilityText(Function.StateMutability)
         << " virtual";
      if (!Function.Returns.empty()) {
        OS << " returns (";
        for (size_t I = 0; I < Function.Returns.size(); ++I) {
          if (I)
            OS << ", ";
          OS << Function.Returns[I]
             << dataLocation(Function.Returns[I], " memory");
        }
        OS << ")";
      }
      OS << ";\n\n";
    }
  }

  OS << "    function " << kSolidityHostFunctionName
     << "(uint8 opcode, uint256["
     << static_cast<unsigned>(maxHostOpcodeArguments())
     << "] memory args_, bytes memory input) internal virtual returns "
        "(uint256);\n\n"
        "    function "
     << kSolidityTraceFunctionName
     << "(uint256 pc, uint8 opcode) internal virtual {\n"
        "        emit EVMTrace(pc, opcode);\n"
        "    }\n\n"
        "    function "
     << kSolidityPushFunctionName << "(uint256[" << kStackLimit
     << "] memory stack_, uint256 sp, "
        "uint256 value, uint256 pc) internal pure returns (uint256) {\n"
        "        if (sp >= _EVM_STACK_LIMIT) revert EVMStackOverflow(pc);\n"
        "        stack_[sp] = value; return sp + 1;\n"
        "    }\n\n"
        "    function "
     << kSolidityPopFunctionName << "(uint256[" << kStackLimit
     << "] memory stack_, uint256 sp, "
        "uint256 pc) internal pure returns (uint256, uint256) {\n"
        "        if (sp == 0) revert EVMStackUnderflow(pc);\n"
        "        unchecked { --sp; } return (sp, stack_[sp]);\n"
        "    }\n\n"
        "    function "
     << kSoliditySwapFunctionName << "(uint256[" << kStackLimit
     << "] memory stack_, uint256 sp, uint256 depth, uint256 pc) "
        "internal pure {\n"
        "        if (sp <= depth) revert EVMStackUnderflow(pc);\n"
        "        uint256 topIndex = sp - 1;\n"
        "        uint256 otherIndex = topIndex - depth;\n"
        "        (stack_[topIndex], stack_[otherIndex]) = "
        "(stack_[otherIndex], stack_[topIndex]);\n"
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
        "(uint8) { return "
     << kSolidityExecuteFunctionName
     << "(input); }\n\n"
        "    fallback() external payable {\n"
        "        uint8 status = "
     << kSolidityExecuteFunctionName
     << "(msg.data);\n"
        "        if (status == NEVERD_EVM_REVERTED) "
        "revert EVMExecutionReverted();\n"
        "    }\n\n"
        "    receive() external payable {\n"
        "        uint8 status = "
     << kSolidityExecuteFunctionName
     << "(bytes(\"\"));\n"
        "        if (status == NEVERD_EVM_REVERTED) "
        "revert EVMExecutionReverted();\n"
        "    }\n\n"
        "    function "
     << kSolidityExecuteFunctionName
     << "(bytes memory input) internal returns "
        "(uint8 status) {\n"
        "        uint256["
     << kStackLimit
     << "] memory evmStack;\n"
        "        uint256 evmSP = 0;\n";
  if (!InstructionPCs.contains(kEntryPC)) {
    OS << "        return " << solidityExitStatusName(ExitStatus::Stopped)
       << ";\n"
          "    }\n"
          "}\n";
    return Text;
  }
  OS << "        uint256 pc = " << kEntryPC
     << ";\n"
        "        unchecked {\n"
        "        while (true) {\n";

  for (size_t Index = 0; Index < Program.Low.Instructions.size(); ++Index) {
    const auto &Instruction = Program.Low.Instructions[Index];
    const uint64_t PC = Instruction.PC;
    const Opcode Op = Instruction.opcode();
    OS << "            " << (Index == 0 ? "if" : "else if") << " (pc == " << PC
       << ") { // " << Instruction.Info.Name << "\n";
    if (Options.EmitTraceEvents)
      OS << "                " << kSolidityTraceFunctionName << "(" << PC
         << ", 0x" << llvm::utohexstr(opcodeByte(Op)) << ");\n";

    if (!Instruction.isExecutable() || Instruction.is(Opcode::INVALID)) {
      OS << "                revert EVMUnsupportedOpcode(pc, 0x"
         << llvm::utohexstr(opcodeByte(Op)) << ");\n            }\n";
      continue;
    }
    if (Instruction.is(Opcode::STOP)) {
      OS << "                return "
         << solidityExitStatusName(ExitStatus::Stopped) << ";\n            }\n";
      continue;
    }
    if (Instruction.isPush()) {
      OS << "                evmSP = " << kSolidityPushFunctionName
         << "(evmStack, evmSP, " << solidityWord(Instruction.Immediate)
         << ", pc);\n";
      emitAdvance(OS, InstructionPCs, Instruction.NextPC);
      OS << "            }\n";
      continue;
    }
    if (Instruction.isDup()) {
      const uint16_t Depth = Instruction.dupDepth();
      OS << "                if (evmSP < " << Depth
         << ") revert EVMStackUnderflow(pc);\n"
            "                evmSP = "
         << kSolidityPushFunctionName << "(evmStack, evmSP, evmStack[evmSP - "
         << Depth << "], pc);\n";
      emitAdvance(OS, InstructionPCs, Instruction.NextPC);
      OS << "            }\n";
      continue;
    }
    if (Instruction.isSwap()) {
      OS << "                " << kSoliditySwapFunctionName
         << "(evmStack, evmSP, " << Instruction.swapDepth() << ", pc);\n";
      emitAdvance(OS, InstructionPCs, Instruction.NextPC);
      OS << "            }\n";
      continue;
    }
    if (Instruction.isExchange()) {
      const auto [First, Second] = *Instruction.exchangeDepths();
      OS << "                " << kSoliditySwapFunctionName
         << "(evmStack, evmSP, " << First << ", pc);\n"
         << "                " << kSoliditySwapFunctionName
         << "(evmStack, evmSP, " << Second << ", pc);\n"
         << "                " << kSoliditySwapFunctionName
         << "(evmStack, evmSP, " << First << ", pc);\n";
      emitAdvance(OS, InstructionPCs, Instruction.NextPC);
      OS << "            }\n";
      continue;
    }
    if (Instruction.isJump()) {
      OS << "                uint256 destination;\n"
            "                (evmSP, destination) = "
         << kSolidityPopFunctionName << "(evmStack, evmSP, pc);\n";
      if (Op == Opcode::JUMPI) {
        OS << "                uint256 condition;\n"
              "                (evmSP, condition) = "
           << kSolidityPopFunctionName
           << "(evmStack, evmSP, pc);\n"
              "                if (condition == 0) { "
           << advanceStatement(InstructionPCs, Instruction.NextPC) << " }\n";
      }
      if (Program.Low.JumpDestinations.empty()) {
        OS << "                revert EVMInvalidJump(destination);\n";
      } else {
        bool FirstTarget = true;
        for (uint64_t Target : Program.Low.JumpDestinations) {
          OS << "                " << (FirstTarget ? "if" : "else if")
             << " (destination == " << Target << ") pc = " << Target << ";\n";
          FirstTarget = false;
        }
        OS << "                else revert EVMInvalidJump(destination);\n"
              "                continue;\n";
      }
      OS << "            }\n";
      continue;
    }

    if (Instruction.Info.StackPops != 0 ||
        (!isALU(Instruction.Info) && Op != Opcode::PC &&
         Op != Opcode::CODESIZE))
      OS << "                uint256["
         << static_cast<unsigned>(maxHostOpcodeArguments())
         << "] memory args_;\n";
    for (uint8_t I = 0; I < Instruction.Info.StackPops; ++I)
      OS << "                (evmSP, args_[" << static_cast<unsigned>(I)
         << "]) = " << kSolidityPopFunctionName << "(evmStack, evmSP, pc);\n";

    if (Op == Opcode::POP || Op == Opcode::JUMPDEST) {
      emitAdvance(OS, InstructionPCs, Instruction.NextPC);
      OS << "            }\n";
      continue;
    }
    if (Op == Opcode::PC || Op == Opcode::CODESIZE) {
      const uint64_t Value = Op == Opcode::PC ? PC : Program.Low.Code.size();
      OS << "                evmSP = " << kSolidityPushFunctionName
         << "(evmStack, evmSP, " << Value << ", pc);\n";
      emitAdvance(OS, InstructionPCs, Instruction.NextPC);
      OS << "            }\n";
      continue;
    }

    const bool HasInlineALULowering = isALU(Instruction.Info);
    const std::string Output =
        HasInlineALULowering ? pureExpression(Op) : hostExpression(Op);
    if (Instruction.Info.StackPushes != 0)
      OS << "                uint256 result = " << Output << ";\n";
    else if (!HasInlineALULowering)
      OS << "                " << Output << ";\n";

    if (Op == Opcode::RETURN || Op == Opcode::REVERT ||
        Op == Opcode::SELFDESTRUCT) {
      OS << "                return "
         << (Op == Opcode::RETURN ? solidityExitStatusName(ExitStatus::Returned)
             : Op == Opcode::REVERT
                 ? solidityExitStatusName(ExitStatus::Reverted)
                 : solidityExitStatusName(ExitStatus::SelfDestructed))
         << ";\n            }\n";
      continue;
    }
    if (Instruction.Info.IsTerminator) {
      OS << "                return "
         << solidityExitStatusName(ExitStatus::Stopped) << ";\n            }\n";
      continue;
    }
    if (Instruction.Info.StackPushes != 0)
      OS << "                evmSP = " << kSolidityPushFunctionName
         << "(evmStack, evmSP, result, pc);\n";
    emitAdvance(OS, InstructionPCs, Instruction.NextPC);
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
