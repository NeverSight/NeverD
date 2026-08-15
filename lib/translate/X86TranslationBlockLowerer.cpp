//===- X86TranslationBlockLowerer.cpp - Canonical x86 block IR ----------===//

#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/translate/GuestMemoryRuntime.h"
#include "neverd/translate/GuestState.h"
#include "neverd/translate/RuntimeABI.h"
#include "neverd/translate/RuntimeGuestState.h"
#include "neverd/translate/RuntimeHelpers.h"
#include "neverd/translate/TranslationBlockLowerer.h"
#include "neverd/translate/X86TranslationBlockBuilder.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace neverd::translate {

char TranslationBlockLoweringError::ID;

TranslationBlockLoweringError::TranslationBlockLoweringError(
    TranslationBlockLoweringErrorCode Code, uint64_t GuestPC,
    std::optional<uint64_t> OpIndex, std::optional<NdOp> Opcode,
    std::string Detail)
    : Code(Code), GuestPC(GuestPC), OpIndex(OpIndex), Opcode(Opcode),
      Detail(std::move(Detail)) {}

void TranslationBlockLoweringError::log(llvm::raw_ostream &OS) const {
  OS << "translation block lowering at guest PC 0x" << llvm::utohexstr(GuestPC)
     << ": ";
  switch (Code) {
  case TranslationBlockLoweringErrorCode::InvalidDescriptor:
    OS << "invalid block descriptor";
    break;
  case TranslationBlockLoweringErrorCode::UnsupportedHostTarget:
    OS << "unsupported host target";
    break;
  case TranslationBlockLoweringErrorCode::InvalidHostDataLayout:
    OS << "invalid host data layout";
    break;
  case TranslationBlockLoweringErrorCode::UnsupportedBlockShape:
    OS << "unsupported block shape";
    break;
  case TranslationBlockLoweringErrorCode::UnsupportedOperation:
    OS << "unsupported LowIR operation";
    break;
  case TranslationBlockLoweringErrorCode::InvalidOperand:
    OS << "invalid LowIR operand";
    break;
  case TranslationBlockLoweringErrorCode::UnsupportedRegister:
    OS << "unsupported x86-64 register";
    break;
  case TranslationBlockLoweringErrorCode::UndefinedTemporary:
    OS << "LowIR temporary is read before its definition";
    break;
  case TranslationBlockLoweringErrorCode::InvalidControlFlow:
    OS << "invalid block control flow";
    break;
  case TranslationBlockLoweringErrorCode::IRVerificationFailed:
    OS << "lowered IR failed verification";
    break;
  }
  if (OpIndex)
    OS << " at operation " << *OpIndex;
  if (Opcode)
    OS << " (" << ndOpName(*Opcode) << ')';
  if (!Detail.empty())
    OS << " (" << Detail << ')';
}

std::error_code TranslationBlockLoweringError::convertToErrorCode() const {
  return std::make_error_code(std::errc::invalid_argument);
}

LoweredTranslationBlockV1::LoweredTranslationBlockV1(
    std::unique_ptr<llvm::Module> Module, std::string BlockSymbol)
    : Module(std::move(Module)), BlockSymbol(std::move(BlockSymbol)) {}

LoweredTranslationBlockV1::LoweredTranslationBlockV1(
    LoweredTranslationBlockV1 &&) noexcept = default;

LoweredTranslationBlockV1 &LoweredTranslationBlockV1::operator=(
    LoweredTranslationBlockV1 &&) noexcept = default;

LoweredTranslationBlockV1::~LoweredTranslationBlockV1() = default;

std::unique_ptr<llvm::Module> LoweredTranslationBlockV1::takeModule() && {
  return std::move(Module);
}

namespace {

using VarKey = std::tuple<uint8_t, uint64_t, uint16_t>;

struct StateField {
  uint64_t Offset = 0;
  llvm::Align Alignment = llvm::Align(1);
};

struct DirtyRegister {
  NdVar Register;
  llvm::Value *Value = nullptr;
};

llvm::Error failureAtGuestPC(TranslationBlockLoweringErrorCode Code,
                             uint64_t GuestPC,
                             std::optional<uint64_t> OpIndex = std::nullopt,
                             std::optional<NdOp> Opcode = std::nullopt,
                             llvm::StringRef Detail = {}) {
  return llvm::make_error<TranslationBlockLoweringError>(Code, GuestPC, OpIndex,
                                                         Opcode, Detail.str());
}

llvm::Error failure(TranslationBlockLoweringErrorCode Code,
                    const TranslationBlockDescriptorV1 &Block,
                    std::optional<uint64_t> OpIndex = std::nullopt,
                    std::optional<NdOp> Opcode = std::nullopt,
                    llvm::StringRef Detail = {}) {
  const uint64_t GuestPC = OpIndex && *OpIndex < Block.Ops.size()
                               ? Block.Ops[static_cast<size_t>(*OpIndex)].Addr
                               : Block.Header.EntryPC;
  return failureAtGuestPC(Code, GuestPC, OpIndex, Opcode, Detail);
}

std::string blockSymbol(uint64_t EntryPC) {
  constexpr char HexDigits[] = "0123456789abcdef";
  std::string Result = "nvd_x86_64_block_0000000000000000";
  for (size_t Index = 0; Index != 16; ++Index) {
    const unsigned Shift = static_cast<unsigned>((15 - Index) * 4);
    Result[Result.size() - 16 + Index] = HexDigits[(EntryPC >> Shift) & 0xf];
  }
  return Result;
}

VarKey keyFor(const NdVar &Variable) {
  return {static_cast<uint8_t>(Variable.Space), Variable.Offset, Variable.Size};
}

bool isSupportedIntegerSize(uint16_t Size) {
  return Size == 1 || Size == 2 || Size == 4 || Size == 8;
}

uint8_t meaningfulREXExtensionMask(uint8_t Opcode) {
  constexpr uint8_t REXR = 0x04;
  constexpr uint8_t REXB = 0x01;
  if (Opcode >= 0xb8 && Opcode <= 0xbf)
    return REXB;

  switch (Opcode) {
  case 0x89: // MOV r/m64, r64
  case 0x8b: // MOV r64, r/m64
  case 0x01: // ADD r/m64, r64
  case 0x03: // ADD r64, r/m64
  case 0x09: // OR r/m64, r64
  case 0x0b: // OR r64, r/m64
  case 0x21: // AND r/m64, r64
  case 0x23: // AND r64, r/m64
  case 0x29: // SUB r/m64, r64
  case 0x2b: // SUB r64, r/m64
  case 0x31: // XOR r/m64, r64
  case 0x33: // XOR r64, r/m64
  case 0x39: // CMP r/m64, r64
  case 0x3b: // CMP r64, r/m64
  case 0x85: // TEST r/m64, r64
    return REXR | REXB;
  case 0x81: // Scalar immediate with a ModR/M opcode-extension field.
  case 0x83:
  case 0xc7:
  case 0xf7: // TEST r/m64, imm32; /0 is an opcode-extension field.
    return REXB;
  default:
    return 0;
  }
}

struct SingleFlagBranchEncoding {
  uint8_t ShortOpcode;
  uint8_t NearOpcode;
  uint64_t Flag;
  NdOp PredicateOpcode;
};

std::optional<SingleFlagBranchEncoding>
singleFlagBranchEncoding(unsigned InstructionID) {
  switch (InstructionID) {
  case X86_INS_JO:
    return SingleFlagBranchEncoding{0x70, 0x80, x86reg::OF, NdOp::COPY};
  case X86_INS_JNO:
    return SingleFlagBranchEncoding{0x71, 0x81, x86reg::OF, NdOp::BOOL_NOT};
  case X86_INS_JB:
    return SingleFlagBranchEncoding{0x72, 0x82, x86reg::CF, NdOp::COPY};
  case X86_INS_JAE:
    return SingleFlagBranchEncoding{0x73, 0x83, x86reg::CF, NdOp::BOOL_NOT};
  case X86_INS_JE:
    return SingleFlagBranchEncoding{0x74, 0x84, x86reg::ZF, NdOp::COPY};
  case X86_INS_JNE:
    return SingleFlagBranchEncoding{0x75, 0x85, x86reg::ZF, NdOp::BOOL_NOT};
  case X86_INS_JS:
    return SingleFlagBranchEncoding{0x78, 0x88, x86reg::SF, NdOp::COPY};
  case X86_INS_JNS:
    return SingleFlagBranchEncoding{0x79, 0x89, x86reg::SF, NdOp::BOOL_NOT};
  case X86_INS_JP:
    return SingleFlagBranchEncoding{0x7a, 0x8a, x86reg::PF, NdOp::COPY};
  case X86_INS_JNP:
    return SingleFlagBranchEncoding{0x7b, 0x8b, x86reg::PF, NdOp::BOOL_NOT};
  default:
    return std::nullopt;
  }
}

enum class MultiFlagBranchPredicate : uint8_t {
  BelowOrEqual,
  Above,
  Less,
  GreaterOrEqual,
  LessOrEqual,
  Greater,
};

struct MultiFlagBranchEncoding {
  uint8_t ShortOpcode;
  uint8_t NearOpcode;
  MultiFlagBranchPredicate Predicate;
};

std::optional<MultiFlagBranchEncoding>
multiFlagBranchEncoding(unsigned InstructionID) {
  switch (InstructionID) {
  case X86_INS_JBE:
    return MultiFlagBranchEncoding{0x76, 0x86,
                                   MultiFlagBranchPredicate::BelowOrEqual};
  case X86_INS_JA:
    return MultiFlagBranchEncoding{0x77, 0x87, MultiFlagBranchPredicate::Above};
  case X86_INS_JL:
    return MultiFlagBranchEncoding{0x7c, 0x8c, MultiFlagBranchPredicate::Less};
  case X86_INS_JGE:
    return MultiFlagBranchEncoding{0x7d, 0x8d,
                                   MultiFlagBranchPredicate::GreaterOrEqual};
  case X86_INS_JLE:
    return MultiFlagBranchEncoding{0x7e, 0x8e,
                                   MultiFlagBranchPredicate::LessOrEqual};
  case X86_INS_JG:
    return MultiFlagBranchEncoding{0x7f, 0x8f,
                                   MultiFlagBranchPredicate::Greater};
  default:
    return std::nullopt;
  }
}

bool isCanonicalScalarTempOperation(const LowOp &Operation, NdOp Opcode,
                                    std::initializer_list<NdVar> Inputs) {
  return Operation.Opcode == Opcode && Operation.Output.isTemp() &&
         Operation.Output.Size == sizeof(uint8_t) &&
         Operation.NumInputs == Inputs.size() &&
         std::equal(Inputs.begin(), Inputs.end(), std::begin(Operation.Inputs));
}

bool isCanonicalConditionalBranch(const LowOp &Operation,
                                  const NdVar &Predicate,
                                  uint64_t StaticTargetPC) {
  return Operation.Opcode == NdOp::COND_BR && Operation.Output.Size == 0 &&
         Operation.NumInputs == 2 && Operation.Inputs[0].isConst() &&
         Operation.Inputs[0].Size == sizeof(uint64_t) &&
         Operation.Inputs[0].Offset == StaticTargetPC &&
         Operation.Inputs[1] == Predicate;
}

bool hasCanonicalMultiFlagBranchShape(llvm::ArrayRef<LowOp> Operations,
                                      MultiFlagBranchPredicate Predicate,
                                      uint64_t StaticTargetPC) {
  const NdVar CF = NdVar::reg(x86reg::CF, 1);
  const NdVar ZF = NdVar::reg(x86reg::ZF, 1);
  const NdVar SF = NdVar::reg(x86reg::SF, 1);
  const NdVar OF = NdVar::reg(x86reg::OF, 1);

  switch (Predicate) {
  case MultiFlagBranchPredicate::BelowOrEqual:
    return Operations.size() == 2 &&
           isCanonicalScalarTempOperation(Operations[0], NdOp::BOOL_OR,
                                          {CF, ZF}) &&
           isCanonicalConditionalBranch(Operations[1], Operations[0].Output,
                                        StaticTargetPC);
  case MultiFlagBranchPredicate::Above:
    return Operations.size() == 4 &&
           isCanonicalScalarTempOperation(Operations[0], NdOp::BOOL_NOT,
                                          {CF}) &&
           isCanonicalScalarTempOperation(Operations[1], NdOp::BOOL_NOT,
                                          {ZF}) &&
           isCanonicalScalarTempOperation(
               Operations[2], NdOp::BOOL_AND,
               {Operations[0].Output, Operations[1].Output}) &&
           isCanonicalConditionalBranch(Operations[3], Operations[2].Output,
                                        StaticTargetPC);
  case MultiFlagBranchPredicate::Less:
    return Operations.size() == 2 &&
           isCanonicalScalarTempOperation(Operations[0], NdOp::INT_NOTEQUAL,
                                          {SF, OF}) &&
           isCanonicalConditionalBranch(Operations[1], Operations[0].Output,
                                        StaticTargetPC);
  case MultiFlagBranchPredicate::GreaterOrEqual:
    return Operations.size() == 2 &&
           isCanonicalScalarTempOperation(Operations[0], NdOp::INT_EQUAL,
                                          {SF, OF}) &&
           isCanonicalConditionalBranch(Operations[1], Operations[0].Output,
                                        StaticTargetPC);
  case MultiFlagBranchPredicate::LessOrEqual:
    return Operations.size() == 3 &&
           isCanonicalScalarTempOperation(Operations[0], NdOp::INT_NOTEQUAL,
                                          {SF, OF}) &&
           isCanonicalScalarTempOperation(Operations[1], NdOp::BOOL_OR,
                                          {ZF, Operations[0].Output}) &&
           isCanonicalConditionalBranch(Operations[2], Operations[1].Output,
                                        StaticTargetPC);
  case MultiFlagBranchPredicate::Greater:
    return Operations.size() == 4 &&
           isCanonicalScalarTempOperation(Operations[0], NdOp::BOOL_NOT,
                                          {ZF}) &&
           isCanonicalScalarTempOperation(Operations[1], NdOp::INT_EQUAL,
                                          {SF, OF}) &&
           isCanonicalScalarTempOperation(
               Operations[2], NdOp::BOOL_AND,
               {Operations[0].Output, Operations[1].Output}) &&
           isCanonicalConditionalBranch(Operations[3], Operations[2].Output,
                                        StaticTargetPC);
  }
  return false;
}

llvm::Align stateFieldAlignment(uint64_t Offset) {
  return llvm::Align(
      std::gcd<uint64_t>(alignof(RuntimeGuestStateX86_64V1), Offset));
}

class X86BlockIRLowering final {
public:
  X86BlockIRLowering(const TranslationBlockDescriptorV1 &Block,
                     llvm::Module &Module, llvm::Function &Function,
                     llvm::BasicBlock &Entry)
      : Block(Block), Module(Module), Function(Function), Builder(&Entry),
        State(Function.getArg(0)), Runtime(Function.getArg(1)) {}

  llvm::Error lower() {
    for (uint64_t Index = 0; Index != Block.Ops.size(); ++Index) {
      CurrentIndex = Index;
      CurrentOpcode = Block.Ops[static_cast<size_t>(Index)].Opcode;
      if (llvm::Error Error =
              lowerOperation(Block.Ops[static_cast<size_t>(Index)], Index))
        return Error;
    }
    switch (Block.Header.Terminator) {
    case TranslationBlockTerminatorKindV1::Return:
      if (!SawReturn || SawDirectBranch || SawConditionalBranch)
        return currentFailure(
            TranslationBlockLoweringErrorCode::InvalidControlFlow,
            "return descriptor has no final canonical LowIR return");
      return lowerReturn();
    case TranslationBlockTerminatorKindV1::DirectBranch:
      if (!SawDirectBranch || SawReturn || SawConditionalBranch)
        return currentFailure(
            TranslationBlockLoweringErrorCode::InvalidControlFlow,
            "direct-branch descriptor has no final canonical LowIR branch");
      return lowerDirectBranch();
    case TranslationBlockTerminatorKindV1::ConditionalBranch:
      if (!SawConditionalBranch || SawReturn || SawDirectBranch ||
          !ConditionalBranchCondition)
        return currentFailure(
            TranslationBlockLoweringErrorCode::InvalidControlFlow,
            "conditional-branch descriptor has no final canonical LowIR "
            "predicate");
      return lowerConditionalBranch();
    default:
      return currentFailure(
          TranslationBlockLoweringErrorCode::InvalidControlFlow,
          "descriptor terminator is outside the published lowering slice");
    }
  }

private:
  llvm::Error currentFailure(TranslationBlockLoweringErrorCode Code,
                             llvm::StringRef Detail = {}) const {
    return failure(Code, Block, CurrentIndex, CurrentOpcode, Detail);
  }

  llvm::Type *integerType(uint16_t Size) const {
    return llvm::IntegerType::get(Module.getContext(), Size * 8);
  }

  llvm::Expected<StateField> stateField(const NdVar &Register) const {
    if (!Register.isReg() || !isSupportedIntegerSize(Register.Size))
      return currentFailure(
          TranslationBlockLoweringErrorCode::UnsupportedRegister,
          "register has an unsupported class or width");

    if (Register.Size == sizeof(uint64_t) && Register.Offset <= x86reg::R15 &&
        Register.Offset % sizeof(uint64_t) == 0)
      return StateField{offsetof(RuntimeGuestStateX86_64V1, GPR) +
                            Register.Offset,
                        llvm::Align(alignof(uint64_t))};
    if (Register.Size == sizeof(uint64_t) && Register.Offset == x86reg::RIP)
      return StateField{offsetof(RuntimeGuestStateX86_64V1, RIP),
                        llvm::Align(alignof(uint64_t))};
    if (Register.Size != sizeof(uint8_t))
      return currentFailure(
          TranslationBlockLoweringErrorCode::UnsupportedRegister,
          "only full-width GPRs and scalar flags are published in v1");

    switch (Register.Offset) {
    case x86reg::CF:
      return StateField{
          offsetof(RuntimeGuestStateX86_64V1, CF),
          stateFieldAlignment(offsetof(RuntimeGuestStateX86_64V1, CF))};
    case x86reg::PF:
      return StateField{
          offsetof(RuntimeGuestStateX86_64V1, PF),
          stateFieldAlignment(offsetof(RuntimeGuestStateX86_64V1, PF))};
    case x86reg::AF:
      return StateField{
          offsetof(RuntimeGuestStateX86_64V1, AF),
          stateFieldAlignment(offsetof(RuntimeGuestStateX86_64V1, AF))};
    case x86reg::ZF:
      return StateField{
          offsetof(RuntimeGuestStateX86_64V1, ZF),
          stateFieldAlignment(offsetof(RuntimeGuestStateX86_64V1, ZF))};
    case x86reg::SF:
      return StateField{
          offsetof(RuntimeGuestStateX86_64V1, SF),
          stateFieldAlignment(offsetof(RuntimeGuestStateX86_64V1, SF))};
    case x86reg::DF:
      return StateField{
          offsetof(RuntimeGuestStateX86_64V1, DF),
          stateFieldAlignment(offsetof(RuntimeGuestStateX86_64V1, DF))};
    case x86reg::OF:
      return StateField{
          offsetof(RuntimeGuestStateX86_64V1, OF),
          stateFieldAlignment(offsetof(RuntimeGuestStateX86_64V1, OF))};
    default:
      return currentFailure(
          TranslationBlockLoweringErrorCode::UnsupportedRegister,
          "register is outside the fixed runtime-state slots");
    }
  }

  llvm::Value *statePointer(uint64_t Offset, llvm::StringRef Name = {}) {
    return Builder.CreateGEP(Builder.getInt8Ty(), State,
                             Builder.getInt64(Offset), Name);
  }

  llvm::Expected<llvm::Value *> readValue(const NdVar &Variable) {
    if (!isSupportedIntegerSize(Variable.Size))
      return currentFailure(TranslationBlockLoweringErrorCode::InvalidOperand,
                            "operand has an unsupported integer width");
    if (Variable.isConst())
      return llvm::ConstantInt::get(integerType(Variable.Size),
                                    Variable.Offset);

    const VarKey Key = keyFor(Variable);
    if (const auto It = Values.find(Key); It != Values.end())
      return It->second;
    if (Variable.Space == VnodeSpace::TEMP)
      return currentFailure(
          TranslationBlockLoweringErrorCode::UndefinedTemporary);
    if (!Variable.isReg())
      return currentFailure(TranslationBlockLoweringErrorCode::InvalidOperand,
                            "guest memory is not a host pointer operand");

    llvm::Expected<StateField> Field = stateField(Variable);
    if (!Field)
      return Field.takeError();
    llvm::Value *Pointer = statePointer(Field->Offset, "state.slot");
    llvm::Value *Loaded = Builder.CreateAlignedLoad(
        integerType(Variable.Size), Pointer, Field->Alignment, "state.value");
    Values.emplace(Key, Loaded);
    return Loaded;
  }

  llvm::Error writeValue(const NdVar &Output, llvm::Value *Value) {
    if (!Value || !isSupportedIntegerSize(Output.Size) ||
        !Value->getType()->isIntegerTy(Output.Size * 8))
      return currentFailure(TranslationBlockLoweringErrorCode::InvalidOperand,
                            "result width disagrees with its LowIR output");
    if (Output.Space == VnodeSpace::TEMP) {
      Values[keyFor(Output)] = Value;
      return llvm::Error::success();
    }
    if (!Output.isReg())
      return currentFailure(TranslationBlockLoweringErrorCode::InvalidOperand,
                            "operation output is not a register or temporary");
    if (llvm::Expected<StateField> Field = stateField(Output)) {
      (void)*Field;
    } else {
      return Field.takeError();
    }
    const VarKey Key = keyFor(Output);
    Values[Key] = Value;
    Dirty[Key] = DirtyRegister{Output, Value};
    return llvm::Error::success();
  }

  llvm::Error requireShape(const LowOp &Operation, uint8_t InputCount,
                           bool HasOutput = true) const {
    if (Operation.NumInputs != InputCount)
      return currentFailure(TranslationBlockLoweringErrorCode::InvalidOperand,
                            "operation has an unexpected input count");
    if (HasOutput && (Operation.Output.Space != VnodeSpace::REG &&
                      Operation.Output.Space != VnodeSpace::TEMP))
      return currentFailure(TranslationBlockLoweringErrorCode::InvalidOperand,
                            "operation has no writable output");
    if (HasOutput && !isSupportedIntegerSize(Operation.Output.Size))
      return currentFailure(TranslationBlockLoweringErrorCode::InvalidOperand,
                            "operation output has an unsupported width");
    return llvm::Error::success();
  }

  llvm::Expected<std::pair<llvm::Value *, llvm::Value *>>
  readBinary(const LowOp &Operation) {
    if (llvm::Error Error = requireShape(Operation, 2))
      return std::move(Error);
    llvm::Expected<llvm::Value *> Left = readValue(Operation.Inputs[0]);
    if (!Left)
      return Left.takeError();
    llvm::Expected<llvm::Value *> Right = readValue(Operation.Inputs[1]);
    if (!Right)
      return Right.takeError();
    if ((*Left)->getType() != (*Right)->getType())
      return currentFailure(TranslationBlockLoweringErrorCode::InvalidOperand,
                            "binary operands have different widths");
    return std::make_pair(*Left, *Right);
  }

  llvm::Error lowerCopy(const LowOp &Operation) {
    if (llvm::Error Error = requireShape(Operation, 1))
      return Error;
    llvm::Expected<llvm::Value *> Input = readValue(Operation.Inputs[0]);
    if (!Input)
      return Input.takeError();
    return writeValue(Operation.Output, *Input);
  }

  llvm::Error lowerBinary(const LowOp &Operation, NdOp Opcode) {
    llvm::Expected<std::pair<llvm::Value *, llvm::Value *>> Operands =
        readBinary(Operation);
    if (!Operands)
      return Operands.takeError();
    llvm::Value *Result = nullptr;
    switch (Opcode) {
    case NdOp::INT_ADD:
      Result = Builder.CreateAdd(Operands->first, Operands->second, "add");
      break;
    case NdOp::INT_SUB:
      Result = Builder.CreateSub(Operands->first, Operands->second, "sub");
      break;
    case NdOp::INT_AND:
      Result = Builder.CreateAnd(Operands->first, Operands->second, "and");
      break;
    case NdOp::INT_OR:
      Result = Builder.CreateOr(Operands->first, Operands->second, "or");
      break;
    case NdOp::INT_XOR:
      Result = Builder.CreateXor(Operands->first, Operands->second, "xor");
      break;
    default:
      return currentFailure(
          TranslationBlockLoweringErrorCode::UnsupportedOperation);
    }
    return writeValue(Operation.Output, Result);
  }

  llvm::Error lowerComparison(const LowOp &Operation, NdOp Opcode) {
    llvm::Expected<std::pair<llvm::Value *, llvm::Value *>> Operands =
        readBinary(Operation);
    if (!Operands)
      return Operands.takeError();
    if (Operation.Output.Size != 1)
      return currentFailure(TranslationBlockLoweringErrorCode::InvalidOperand,
                            "comparison output is not a scalar flag byte");

    llvm::CmpInst::Predicate Predicate;
    switch (Opcode) {
    case NdOp::INT_EQUAL:
      Predicate = llvm::CmpInst::ICMP_EQ;
      break;
    case NdOp::INT_NOTEQUAL:
      Predicate = llvm::CmpInst::ICMP_NE;
      break;
    case NdOp::INT_LESS:
      Predicate = llvm::CmpInst::ICMP_ULT;
      break;
    case NdOp::INT_SLESS:
      Predicate = llvm::CmpInst::ICMP_SLT;
      break;
    default:
      return currentFailure(
          TranslationBlockLoweringErrorCode::UnsupportedOperation);
    }
    llvm::Value *Condition = Builder.CreateICmp(Predicate, Operands->first,
                                                Operands->second, "condition");
    llvm::Value *Flag =
        Builder.CreateZExt(Condition, Builder.getInt8Ty(), "flag");
    return writeValue(Operation.Output, Flag);
  }

  llvm::Error lowerSubbytes(const LowOp &Operation) {
    if (llvm::Error Error = requireShape(Operation, 2))
      return Error;
    if (!Operation.Inputs[1].isConst())
      return currentFailure(TranslationBlockLoweringErrorCode::InvalidOperand,
                            "SUBBYTES offset is not constant");
    llvm::Expected<llvm::Value *> Input = readValue(Operation.Inputs[0]);
    if (!Input)
      return Input.takeError();
    const uint64_t ByteOffset = Operation.Inputs[1].Offset;
    if (Operation.Output.Size > Operation.Inputs[0].Size ||
        ByteOffset > Operation.Inputs[0].Size - Operation.Output.Size)
      return currentFailure(TranslationBlockLoweringErrorCode::InvalidOperand,
                            "SUBBYTES range exceeds its input");

    llvm::Value *Result = *Input;
    if (ByteOffset != 0)
      Result = Builder.CreateLShr(
          Result, llvm::ConstantInt::get(Result->getType(), ByteOffset * 8),
          "subbytes.shift");
    if (Operation.Output.Size != Operation.Inputs[0].Size)
      Result = Builder.CreateTrunc(Result, integerType(Operation.Output.Size),
                                   "subbytes");
    return writeValue(Operation.Output, Result);
  }

  llvm::Error lowerPopcount(const LowOp &Operation) {
    if (llvm::Error Error = requireShape(Operation, 1))
      return Error;
    llvm::Expected<llvm::Value *> Input = readValue(Operation.Inputs[0]);
    if (!Input)
      return Input.takeError();
    if (Operation.Output.Size != Operation.Inputs[0].Size)
      return currentFailure(TranslationBlockLoweringErrorCode::InvalidOperand,
                            "POPCOUNT output width disagrees with its input");
    llvm::Function *Intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
        &Module, llvm::Intrinsic::ctpop, {(*Input)->getType()});
    llvm::CallInst *Result =
        Builder.CreateCall(Intrinsic, {*Input}, "popcount");
    Result->addFnAttr(llvm::Attribute::NoUnwind);
    return writeValue(Operation.Output, Result);
  }

  llvm::Error lowerCarry(const LowOp &Operation) {
    llvm::Expected<std::pair<llvm::Value *, llvm::Value *>> Operands =
        readBinary(Operation);
    if (!Operands)
      return Operands.takeError();
    if (Operation.Output.Size != 1)
      return currentFailure(TranslationBlockLoweringErrorCode::InvalidOperand,
                            "carry output is not a scalar flag byte");
    llvm::Value *Sum =
        Builder.CreateAdd(Operands->first, Operands->second, "carry.sum");
    llvm::Value *Carry =
        Builder.CreateICmpULT(Sum, Operands->first, "carry.condition");
    return writeValue(Operation.Output,
                      Builder.CreateZExt(Carry, Builder.getInt8Ty(), "carry"));
  }

  llvm::Error lowerSignedOverflow(const LowOp &Operation) {
    llvm::Expected<std::pair<llvm::Value *, llvm::Value *>> Operands =
        readBinary(Operation);
    if (!Operands)
      return Operands.takeError();
    if (Operation.Output.Size != 1)
      return currentFailure(TranslationBlockLoweringErrorCode::InvalidOperand,
                            "overflow output is not a scalar flag byte");

    llvm::Value *Sum =
        Builder.CreateAdd(Operands->first, Operands->second, "overflow.sum");
    llvm::Value *ChangedA =
        Builder.CreateXor(Operands->first, Sum, "overflow.changed.a");
    llvm::Value *ChangedB =
        Builder.CreateXor(Operands->second, Sum, "overflow.changed.b");
    llvm::Value *SignEvidence =
        Builder.CreateAnd(ChangedA, ChangedB, "overflow.evidence");
    llvm::Value *Overflow = Builder.CreateICmpSLT(
        SignEvidence, llvm::ConstantInt::get(SignEvidence->getType(), 0),
        "overflow.condition");
    return writeValue(
        Operation.Output,
        Builder.CreateZExt(Overflow, Builder.getInt8Ty(), "overflow"));
  }

  llvm::Error lowerSignedBorrow(const LowOp &Operation) {
    llvm::Expected<std::pair<llvm::Value *, llvm::Value *>> Operands =
        readBinary(Operation);
    if (!Operands)
      return Operands.takeError();
    if (Operation.Output.Size != 1)
      return currentFailure(TranslationBlockLoweringErrorCode::InvalidOperand,
                            "signed-borrow output is not a scalar flag byte");

    llvm::Value *Difference =
        Builder.CreateSub(Operands->first, Operands->second, "borrow.diff");
    llvm::Value *ChangedOperands = Builder.CreateXor(
        Operands->first, Operands->second, "borrow.changed.operands");
    llvm::Value *ChangedResult =
        Builder.CreateXor(Operands->first, Difference, "borrow.changed.result");
    llvm::Value *SignEvidence =
        Builder.CreateAnd(ChangedOperands, ChangedResult, "borrow.evidence");
    llvm::Value *Overflow = Builder.CreateICmpSLT(
        SignEvidence, llvm::ConstantInt::get(SignEvidence->getType(), 0),
        "borrow.condition");
    return writeValue(
        Operation.Output,
        Builder.CreateZExt(Overflow, Builder.getInt8Ty(), "borrow"));
  }

  llvm::Error lowerBooleanNot(const LowOp &Operation) {
    if (llvm::Error Error = requireShape(Operation, 1))
      return Error;
    if (Operation.Output.Size != sizeof(uint8_t) ||
        Operation.Inputs[0].Size != sizeof(uint8_t))
      return currentFailure(TranslationBlockLoweringErrorCode::InvalidOperand,
                            "boolean negation is not a scalar flag operation");
    llvm::Expected<llvm::Value *> Input = readValue(Operation.Inputs[0]);
    if (!Input)
      return Input.takeError();
    llvm::Value *IsZero = Builder.CreateICmpEQ(
        *Input, llvm::ConstantInt::get((*Input)->getType(), 0), "bool.not");
    return writeValue(
        Operation.Output,
        Builder.CreateZExt(IsZero, Builder.getInt8Ty(), "bool.not.byte"));
  }

  llvm::Error lowerBooleanOr(const LowOp &Operation) {
    llvm::Expected<std::pair<llvm::Value *, llvm::Value *>> Operands =
        readBinary(Operation);
    if (!Operands)
      return Operands.takeError();
    if (Operation.Output.Size != sizeof(uint8_t) ||
        Operation.Inputs[0].Size != sizeof(uint8_t) ||
        Operation.Inputs[1].Size != sizeof(uint8_t))
      return currentFailure(TranslationBlockLoweringErrorCode::InvalidOperand,
                            "boolean disjunction is not a scalar flag "
                            "operation");
    llvm::Value *Left = Builder.CreateICmpNE(
        Operands->first, llvm::ConstantInt::get(Operands->first->getType(), 0),
        "bool.or.left");
    llvm::Value *Right = Builder.CreateICmpNE(
        Operands->second,
        llvm::ConstantInt::get(Operands->second->getType(), 0),
        "bool.or.right");
    llvm::Value *Either = Builder.CreateOr(Left, Right, "bool.or");
    return writeValue(
        Operation.Output,
        Builder.CreateZExt(Either, Builder.getInt8Ty(), "bool.or.byte"));
  }

  llvm::Error lowerBooleanAnd(const LowOp &Operation) {
    llvm::Expected<std::pair<llvm::Value *, llvm::Value *>> Operands =
        readBinary(Operation);
    if (!Operands)
      return Operands.takeError();
    if (Operation.Output.Size != sizeof(uint8_t) ||
        Operation.Inputs[0].Size != sizeof(uint8_t) ||
        Operation.Inputs[1].Size != sizeof(uint8_t))
      return currentFailure(TranslationBlockLoweringErrorCode::InvalidOperand,
                            "boolean conjunction is not a scalar flag "
                            "operation");
    llvm::Value *Left = Builder.CreateICmpNE(
        Operands->first, llvm::ConstantInt::get(Operands->first->getType(), 0),
        "bool.and.left");
    llvm::Value *Right = Builder.CreateICmpNE(
        Operands->second,
        llvm::ConstantInt::get(Operands->second->getType(), 0),
        "bool.and.right");
    llvm::Value *Both = Builder.CreateAnd(Left, Right, "bool.and");
    return writeValue(
        Operation.Output,
        Builder.CreateZExt(Both, Builder.getInt8Ty(), "bool.and.byte"));
  }

  llvm::Error lowerLowIRReturn(const LowOp &Operation, uint64_t Index) {
    if (llvm::Error Error = requireShape(Operation, 1, false))
      return Error;
    if (SawReturn || Index + 1 != Block.Ops.size() ||
        Operation.Output.Size != 0 ||
        Operation.Inputs[0] != NdVar::reg(x86reg::RAX, sizeof(uint64_t)))
      return currentFailure(
          TranslationBlockLoweringErrorCode::InvalidControlFlow,
          "decompiler return is not the final canonical x86-64 return marker");
    SawReturn = true;
    return llvm::Error::success();
  }

  llvm::Error lowerLowIRDirectBranch(const LowOp &Operation, uint64_t Index) {
    if (llvm::Error Error = requireShape(Operation, 1, false))
      return Error;
    if (SawDirectBranch || SawReturn || Index + 1 != Block.Ops.size() ||
        Operation.Output.Size != 0 || !Operation.Inputs[0].isConst() ||
        Operation.Inputs[0].Size != sizeof(uint64_t) ||
        Operation.Inputs[0].Offset != Block.Header.StaticTargetPC)
      return currentFailure(
          TranslationBlockLoweringErrorCode::InvalidControlFlow,
          "direct jump is not the final canonical static-target branch");
    SawDirectBranch = true;
    return llvm::Error::success();
  }

  llvm::Error lowerLowIRConditionalBranch(const LowOp &Operation,
                                          uint64_t Index) {
    if (llvm::Error Error = requireShape(Operation, 2, false))
      return Error;
    if (SawConditionalBranch || SawDirectBranch || SawReturn ||
        Index + 1 != Block.Ops.size() || Operation.Output.Size != 0 ||
        !Operation.Inputs[0].isConst() ||
        Operation.Inputs[0].Size != sizeof(uint64_t) ||
        Operation.Inputs[0].Offset != Block.Header.StaticTargetPC ||
        Operation.Inputs[1].Size != sizeof(uint8_t))
      return currentFailure(
          TranslationBlockLoweringErrorCode::InvalidControlFlow,
          "conditional jump is not the final canonical static-target branch");
    llvm::Expected<llvm::Value *> Condition = readValue(Operation.Inputs[1]);
    if (!Condition)
      return Condition.takeError();
    if (!(*Condition)->getType()->isIntegerTy(8))
      return currentFailure(TranslationBlockLoweringErrorCode::InvalidOperand,
                            "conditional jump predicate is not a flag byte");
    ConditionalBranchCondition = *Condition;
    SawConditionalBranch = true;
    return llvm::Error::success();
  }

  llvm::Error lowerOperation(const LowOp &Operation, uint64_t Index) {
    switch (Operation.Opcode) {
    case NdOp::COPY:
      return lowerCopy(Operation);
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
    case NdOp::INT_AND:
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
      return lowerBinary(Operation, Operation.Opcode);
    case NdOp::INT_EQUAL:
    case NdOp::INT_NOTEQUAL:
    case NdOp::INT_LESS:
    case NdOp::INT_SLESS:
      return lowerComparison(Operation, Operation.Opcode);
    case NdOp::SUBBYTES:
      return lowerSubbytes(Operation);
    case NdOp::POPCOUNT:
      return lowerPopcount(Operation);
    case NdOp::INT_CARRY:
      return lowerCarry(Operation);
    case NdOp::INT_SOVF:
      return lowerSignedOverflow(Operation);
    case NdOp::INT_SBOR:
      return lowerSignedBorrow(Operation);
    case NdOp::BOOL_NOT:
      return lowerBooleanNot(Operation);
    case NdOp::BOOL_AND:
      return lowerBooleanAnd(Operation);
    case NdOp::BOOL_OR:
      return lowerBooleanOr(Operation);
    case NdOp::RETURN:
      return lowerLowIRReturn(Operation, Index);
    case NdOp::BRANCH:
      return lowerLowIRDirectBranch(Operation, Index);
    case NdOp::COND_BR:
      return lowerLowIRConditionalBranch(Operation, Index);
    default:
      return currentFailure(
          TranslationBlockLoweringErrorCode::UnsupportedOperation);
    }
  }

  llvm::Error storeState(const NdVar &Register, llvm::Value *Value) {
    llvm::Expected<StateField> Field = stateField(Register);
    if (!Field)
      return Field.takeError();
    llvm::Value *Pointer = statePointer(Field->Offset, "state.commit.slot");
    Builder.CreateAlignedStore(Value, Pointer, Field->Alignment);
    return llvm::Error::success();
  }

  llvm::Error commitState(llvm::Value *NextRIP) {
    if (!NextRIP || !NextRIP->getType()->isIntegerTy(64))
      return currentFailure(TranslationBlockLoweringErrorCode::InvalidOperand,
                            "next guest PC is not a 64-bit integer");
    for (const auto &[Key, DirtyValue] : Dirty) {
      (void)Key;
      if (llvm::Error Error = storeState(DirtyValue.Register, DirtyValue.Value))
        return Error;
    }
    llvm::Value *RIP = statePointer(offsetof(RuntimeGuestStateX86_64V1, RIP),
                                    "state.commit.rip.slot");
    Builder.CreateAlignedStore(NextRIP, RIP, llvm::Align(alignof(uint64_t)));
    return llvm::Error::success();
  }

  llvm::Error commitState(uint64_t NextRIP) {
    return commitState(Builder.getInt64(NextRIP));
  }

  llvm::Function *load64Helper() {
    llvm::Type *Pointer = llvm::PointerType::getUnqual(Module.getContext());
    llvm::FunctionType *Type = llvm::FunctionType::get(
        Builder.getInt32Ty(),
        {Pointer, Builder.getInt64Ty(), Builder.getInt32Ty()}, false);
    llvm::Function *Helper =
        llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage,
                               "nvd_rt_v1_load64_le", &Module);
    Helper->setVisibility(llvm::GlobalValue::HiddenVisibility);
    Helper->setDSOLocal(true);
    Helper->addFnAttr(llvm::Attribute::NoUnwind);
    return Helper;
  }

  llvm::Error lowerReturn() {
    CurrentIndex = Block.Ops.empty()
                       ? std::optional<uint64_t>()
                       : std::optional<uint64_t>(Block.Ops.size() - 1);
    CurrentOpcode = NdOp::RETURN;
    const uint64_t ReturnPC = Block.InstructionBoundaries.back().Address;
    llvm::Expected<llvm::Value *> RSP =
        readValue(NdVar::reg(x86reg::RSP, sizeof(uint64_t)));
    if (!RSP)
      return RSP.takeError();
    if (llvm::Error Error = commitState(ReturnPC))
      return Error;

    llvm::Function *Helper = load64Helper();
    llvm::CallInst *Status = Builder.CreateCall(
        Helper, {Runtime, *RSP, Builder.getInt32(1)}, "return.load.status");
    Status->setCallingConv(llvm::CallingConv::C);
    Status->addFnAttr(llvm::Attribute::NoUnwind);
    llvm::Value *Succeeded = Builder.CreateICmpEQ(
        Status,
        Builder.getInt32(static_cast<uint32_t>(RuntimeABIExitKindV1::None)),
        "return.load.succeeded");

    llvm::BasicBlock *Success = llvm::BasicBlock::Create(
        Module.getContext(), "return.loaded", &Function);
    llvm::BasicBlock *Failure = llvm::BasicBlock::Create(
        Module.getContext(), "return.fault", &Function);
    Builder.CreateCondBr(Succeeded, Success, Failure);

    Builder.SetInsertPoint(Failure);
    Builder.CreateRet(Status);

    Builder.SetInsertPoint(Success);
    llvm::Value *ResultPointer = Builder.CreateGEP(
        Builder.getInt8Ty(), Runtime,
        Builder.getInt64(offsetof(RuntimeControlBlockV1, ScalarResult)),
        "return.target.slot");
    llvm::Value *Target = Builder.CreateAlignedLoad(
        Builder.getInt64Ty(), ResultPointer, llvm::Align(alignof(uint64_t)),
        "return.target");

    const uint64_t StackDelta = sizeof(uint64_t) + Block.Header.ReturnImmediate;
    llvm::Value *NextRSP =
        Builder.CreateAdd(*RSP, Builder.getInt64(StackDelta), "return.rsp");
    if (llvm::Error Error =
            storeState(NdVar::reg(x86reg::RSP, sizeof(uint64_t)), NextRSP))
      return Error;
    if (llvm::Error Error =
            storeState(NdVar::reg(x86reg::RIP, sizeof(uint64_t)), Target))
      return Error;
    Builder.CreateRet(
        Builder.getInt32(static_cast<uint32_t>(BlockExitKindV1::Return)));
    return llvm::Error::success();
  }

  llvm::Error lowerDirectBranch() {
    CurrentIndex = Block.Ops.empty()
                       ? std::optional<uint64_t>()
                       : std::optional<uint64_t>(Block.Ops.size() - 1);
    CurrentOpcode = NdOp::BRANCH;
    if (llvm::Error Error = commitState(Block.Header.StaticTargetPC))
      return Error;
    Builder.CreateRet(
        Builder.getInt32(static_cast<uint32_t>(BlockExitKindV1::DirectBranch)));
    return llvm::Error::success();
  }

  llvm::Error lowerConditionalBranch() {
    CurrentIndex = Block.Ops.empty()
                       ? std::optional<uint64_t>()
                       : std::optional<uint64_t>(Block.Ops.size() - 1);
    CurrentOpcode = NdOp::COND_BR;
    llvm::Value *IsTaken = Builder.CreateICmpNE(
        ConditionalBranchCondition, Builder.getInt8(0), "branch.taken");
    llvm::Value *SelectedPC = Builder.CreateSelect(
        IsTaken, Builder.getInt64(Block.Header.StaticTargetPC),
        Builder.getInt64(Block.Header.FallthroughPC), "branch.target");
    if (llvm::Error Error = commitState(SelectedPC))
      return Error;
    Builder.CreateRet(
        Builder.getInt32(static_cast<uint32_t>(BlockExitKindV1::DirectBranch)));
    return llvm::Error::success();
  }

  const TranslationBlockDescriptorV1 &Block;
  llvm::Module &Module;
  llvm::Function &Function;
  llvm::IRBuilder<> Builder;
  llvm::Value *State = nullptr;
  llvm::Value *Runtime = nullptr;
  std::map<VarKey, llvm::Value *> Values;
  std::map<VarKey, DirtyRegister> Dirty;
  std::optional<uint64_t> CurrentIndex;
  std::optional<NdOp> CurrentOpcode;
  llvm::Value *ConditionalBranchCondition = nullptr;
  bool SawReturn = false;
  bool SawDirectBranch = false;
  bool SawConditionalBranch = false;
};

llvm::Error
validatePublishedScalarSlice(const TranslationBlockDescriptorV1 &Block) {
  if (Block.InstructionBoundaries.empty() || Block.Ops.empty())
    return failure(TranslationBlockLoweringErrorCode::UnsupportedBlockShape,
                   Block, std::nullopt, std::nullopt,
                   "v1 requires a non-empty scalar-register block");
  switch (Block.Header.Terminator) {
  case TranslationBlockTerminatorKindV1::Return:
    if (Block.Ops.back().Opcode != NdOp::RETURN)
      return failure(TranslationBlockLoweringErrorCode::UnsupportedBlockShape,
                     Block, std::nullopt, std::nullopt,
                     "return block has no final LowIR return");
    break;
  case TranslationBlockTerminatorKindV1::DirectBranch:
    if (Block.Ops.back().Opcode != NdOp::BRANCH ||
        !hasTranslationBlockDescriptorFlag(
            Block.Header.Flags,
            TranslationBlockDescriptorFlagV1::HasStaticTarget))
      return failure(TranslationBlockLoweringErrorCode::UnsupportedBlockShape,
                     Block, std::nullopt, std::nullopt,
                     "direct-jump block has no final static-target branch");
    break;
  case TranslationBlockTerminatorKindV1::ConditionalBranch:
    if (Block.Ops.back().Opcode != NdOp::COND_BR ||
        !hasTranslationBlockDescriptorFlag(
            Block.Header.Flags,
            TranslationBlockDescriptorFlagV1::HasStaticTarget))
      return failure(
          TranslationBlockLoweringErrorCode::UnsupportedBlockShape, Block,
          std::nullopt, std::nullopt,
          "conditional-jump block has no final static-target predicate");
    break;
  default:
    return failure(TranslationBlockLoweringErrorCode::UnsupportedBlockShape,
                   Block, std::nullopt, std::nullopt,
                   "v1 publishes only scalar-register blocks ending in "
                   "return or a canonical direct control transfer");
  }

  Decoder InstructionDecoder;
  InstructionDecoder.setStrict(true);
  if (!InstructionDecoder.init(Arch::X64, InstructionMode::Default))
    return failure(TranslationBlockLoweringErrorCode::UnsupportedBlockShape,
                   Block, std::nullopt, std::nullopt,
                   "cannot initialize the x86-64 publication gate");

  uint64_t ByteOffset = 0;
  for (size_t BoundaryIndex = 0;
       BoundaryIndex != Block.InstructionBoundaries.size(); ++BoundaryIndex) {
    const LowInstructionBoundary &Boundary =
        Block.InstructionBoundaries[BoundaryIndex];
    if (ByteOffset > Block.Bytes.size() ||
        Boundary.Size > Block.Bytes.size() - ByteOffset)
      return failure(TranslationBlockLoweringErrorCode::InvalidDescriptor,
                     Block);

    DecodedInsn Instruction{};
    const int DecodedSize = InstructionDecoder.decodeOneForLift(
        Block.Bytes.data() + ByteOffset,
        static_cast<size_t>(Block.Bytes.size() - ByteOffset), Boundary.Address,
        Instruction);
    const std::optional<uint64_t> FirstOp =
        Boundary.OpCount != 0 && Boundary.FirstOp < Block.Ops.size()
            ? std::optional<uint64_t>(Boundary.FirstOp)
            : std::nullopt;
    const auto Reject = [&](llvm::StringRef Detail) -> llvm::Error {
      return failureAtGuestPC(
          TranslationBlockLoweringErrorCode::UnsupportedBlockShape,
          Boundary.Address, FirstOp, std::nullopt, Detail);
    };
    if (DecodedSize != Boundary.Size || !Instruction.Raw ||
        !Instruction.Raw->detail)
      return Reject("cannot reproduce the published instruction boundary");

    const cs_x86 &X86 = Instruction.Raw->detail->x86;
    for (uint8_t Prefix : X86.prefix)
      if (Prefix != 0)
        return Reject("v1 scalar instructions do not accept legacy prefixes");
    const auto IsFullWidthRegister = [](const cs_x86_op &Operand) {
      if (Operand.type != X86_OP_REG || Operand.size != sizeof(uint64_t))
        return false;
      const RegInfo Register =
          mapCapstoneReg(static_cast<x86_reg>(Operand.reg));
      return Register.Size == sizeof(uint64_t) &&
             Register.Offset <= x86reg::R15;
    };
    const auto IsFullWidthScalarInput = [&](const cs_x86_op &Operand) {
      return IsFullWidthRegister(Operand) || Operand.type == X86_OP_IMM;
    };
    const auto ScalarInputVariable = [&](const cs_x86_op &Operand)
        -> std::optional<NdVar> {
      if (IsFullWidthRegister(Operand)) {
        const RegInfo Register =
            mapCapstoneReg(static_cast<x86_reg>(Operand.reg));
        return NdVar::reg(Register.Offset, sizeof(uint64_t));
      }
      if (Operand.type == X86_OP_IMM)
        return NdVar::cst(static_cast<uint64_t>(Operand.imm),
                          sizeof(uint64_t));
      return std::nullopt;
    };

    if (Boundary.FirstOp > Block.Ops.size() ||
        Boundary.OpCount > Block.Ops.size() - Boundary.FirstOp)
      return failure(TranslationBlockLoweringErrorCode::InvalidDescriptor,
                     Block);
    const llvm::ArrayRef<LowOp> InstructionOps(
        Block.Ops.data() + Boundary.FirstOp,
        static_cast<size_t>(Boundary.OpCount));
    const llvm::ArrayRef<uint8_t> InstructionBytes =
        llvm::ArrayRef<uint8_t>(Block.Bytes)
            .slice(static_cast<size_t>(ByteOffset), Boundary.Size);
    const auto DestinationRegister = [&]() {
      return mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].reg));
    };
    const auto HasCanonicalREXW = [&]() {
      if (InstructionBytes.size() < 2)
        return false;
      constexpr uint8_t REXExtensionBits = 0x07;
      const uint8_t ExtensionBits = InstructionBytes.front() & REXExtensionBits;
      const uint8_t MeaningfulBits =
          meaningfulREXExtensionMask(InstructionBytes[1]);
      return (InstructionBytes.front() & 0xf8u) == 0x48u &&
             X86.rex == InstructionBytes.front() &&
             (InstructionBytes[1] < 0x40u || InstructionBytes[1] > 0x4fu) &&
             (ExtensionBits & static_cast<uint8_t>(~MeaningfulBits)) == 0;
    };

    switch (Instruction.Id) {
    case X86_INS_MOV:
    case X86_INS_MOVABS:
      if (BoundaryIndex + 1 == Block.InstructionBoundaries.size() ||
          !HasCanonicalREXW() || X86.op_count != 2 ||
          !IsFullWidthRegister(X86.operands[0]) ||
          !IsFullWidthScalarInput(X86.operands[1]))
        return Reject("v1 MOV requires full-width GPR register/immediate form");
      if (InstructionOps.size() != 1 ||
          InstructionOps.front().Opcode != NdOp::COPY ||
          InstructionOps.front().Output !=
              NdVar::reg(DestinationRegister().Offset, sizeof(uint64_t)))
        return Reject("v1 MOV LowIR shape is not canonical");
      break;
    case X86_INS_ADD:
    case X86_INS_SUB:
      if (BoundaryIndex + 1 == Block.InstructionBoundaries.size() ||
          !HasCanonicalREXW() || X86.op_count != 2 ||
          !IsFullWidthRegister(X86.operands[0]) ||
          !IsFullWidthScalarInput(X86.operands[1]))
        return Reject(
            "v1 ADD/SUB requires full-width GPR register/immediate form");
      {
        const NdVar Destination =
            NdVar::reg(DestinationRegister().Offset, sizeof(uint64_t));
        const NdOp ArithmeticOpcode =
            Instruction.Id == X86_INS_ADD ? NdOp::INT_ADD : NdOp::INT_SUB;
        const size_t ArithmeticWrites =
            llvm::count_if(InstructionOps, [&](const LowOp &Operation) {
              return Operation.Opcode == ArithmeticOpcode &&
                     Operation.Output == Destination;
            });
        constexpr std::array<uint64_t, 6> RequiredFlags = {
            x86reg::CF, x86reg::PF, x86reg::AF,
            x86reg::ZF, x86reg::SF, x86reg::OF};
        const bool HasAllFlagWrites =
            llvm::all_of(RequiredFlags, [&](uint64_t Flag) {
              return llvm::count_if(
                         InstructionOps, [&](const LowOp &Operation) {
                           return Operation.Output == NdVar::reg(Flag, 1);
                         }) == 1;
            });
        if (ArithmeticWrites != 1 || !HasAllFlagWrites)
          return Reject("v1 ADD/SUB LowIR shape is not canonical");
      }
      break;
    case X86_INS_AND:
    case X86_INS_OR:
    case X86_INS_XOR:
      if (BoundaryIndex + 1 == Block.InstructionBoundaries.size() ||
          !HasCanonicalREXW() || X86.op_count != 2 ||
          !IsFullWidthRegister(X86.operands[0]) ||
          !IsFullWidthScalarInput(X86.operands[1]))
        return Reject(
            "v1 AND/OR/XOR requires full-width GPR register/immediate form");
      {
        const NdVar Destination =
            NdVar::reg(DestinationRegister().Offset, sizeof(uint64_t));
        NdOp LogicOpcode = NdOp::INT_AND;
        if (Instruction.Id == X86_INS_OR)
          LogicOpcode = NdOp::INT_OR;
        else if (Instruction.Id == X86_INS_XOR)
          LogicOpcode = NdOp::INT_XOR;
        const bool SameRegister = X86.operands[1].type == X86_OP_REG &&
                                  X86.operands[0].reg == X86.operands[1].reg;
        const size_t DestinationWrites =
            llvm::count_if(InstructionOps, [&](const LowOp &Operation) {
              return Operation.Output == Destination;
            });
        const size_t LogicOperations =
            llvm::count_if(InstructionOps, [&](const LowOp &Operation) {
              return Operation.Opcode == LogicOpcode &&
                     Operation.Output.Size == sizeof(uint64_t);
            });
        const size_t ZeroingWrites =
            llvm::count_if(InstructionOps, [&](const LowOp &Operation) {
              return Operation.Opcode == NdOp::COPY &&
                     Operation.Output == Destination &&
                     Operation.NumInputs == 1 &&
                     Operation.Inputs[0] == NdVar::cst(0, sizeof(uint64_t));
            });
        constexpr std::array<uint64_t, 5> RequiredFlags = {
            x86reg::CF, x86reg::PF, x86reg::ZF, x86reg::SF, x86reg::OF};
        const bool HasAllFlagWrites =
            llvm::all_of(RequiredFlags, [&](uint64_t Flag) {
              return llvm::count_if(
                         InstructionOps, [&](const LowOp &Operation) {
                           return Operation.Output == NdVar::reg(Flag, 1);
                         }) == 1;
            });
        const bool HasNoAFWrite =
            llvm::none_of(InstructionOps, [](const LowOp &Operation) {
              return Operation.Output == NdVar::reg(x86reg::AF, 1);
            });
        const bool HasCanonicalResult =
            (Instruction.Id == X86_INS_XOR && SameRegister)
                ? DestinationWrites == 1 && ZeroingWrites == 1 &&
                      LogicOperations == 0
            : (Instruction.Id == X86_INS_AND && SameRegister)
                ? DestinationWrites == 0 && LogicOperations == 1
                : DestinationWrites == 1 && LogicOperations == 1;
        if (!HasCanonicalResult || !HasAllFlagWrites || !HasNoAFWrite) {
          std::string Detail;
          llvm::raw_string_ostream Stream(Detail);
          Stream << "v1 AND/OR/XOR LowIR shape is not canonical: destination "
                    "writes="
                 << DestinationWrites
                 << ", logic operations=" << LogicOperations
                 << ", zeroing writes=" << ZeroingWrites
                 << ", flag writes complete=" << HasAllFlagWrites
                 << ", AF preserved=" << HasNoAFWrite;
          return Reject(Stream.str());
        }
      }
      break;
    case X86_INS_CMP:
      if (BoundaryIndex + 1 == Block.InstructionBoundaries.size() ||
          !HasCanonicalREXW() || X86.op_count != 2 ||
          !IsFullWidthRegister(X86.operands[0]) ||
          !IsFullWidthScalarInput(X86.operands[1]))
        return Reject(
            "v1 CMP requires full-width GPR register/immediate form");
      {
        const std::optional<NdVar> Left =
            ScalarInputVariable(X86.operands[0]);
        const std::optional<NdVar> Right =
            ScalarInputVariable(X86.operands[1]);
        const size_t SubtractOperations =
            llvm::count_if(InstructionOps, [&](const LowOp &Operation) {
              return Left && Right && Operation.Opcode == NdOp::INT_SUB &&
                     Operation.Output.isTemp() &&
                     Operation.Output.Size == sizeof(uint64_t) &&
                     Operation.NumInputs == 2 &&
                     Operation.Inputs[0] == *Left &&
                     Operation.Inputs[1] == *Right;
            });
        constexpr std::array<uint64_t, 6> RequiredFlags = {
            x86reg::CF, x86reg::PF, x86reg::AF,
            x86reg::ZF, x86reg::SF, x86reg::OF};
        const bool HasAllFlagWrites =
            llvm::all_of(RequiredFlags, [&](uint64_t Flag) {
              return llvm::count_if(
                         InstructionOps, [&](const LowOp &Operation) {
                           return Operation.Output == NdVar::reg(Flag, 1);
                         }) == 1;
            });
        const bool HasNoGPRWrite =
            llvm::none_of(InstructionOps, [](const LowOp &Operation) {
              return Operation.Output.isReg() &&
                     Operation.Output.Size == sizeof(uint64_t) &&
                     Operation.Output.Offset <= x86reg::R15;
            });
        if (SubtractOperations != 1 || !HasAllFlagWrites || !HasNoGPRWrite)
          return Reject("v1 CMP LowIR shape is not canonical");
      }
      break;
    case X86_INS_TEST:
      if (BoundaryIndex + 1 == Block.InstructionBoundaries.size() ||
          !HasCanonicalREXW() || X86.op_count != 2 ||
          !IsFullWidthRegister(X86.operands[0]) ||
          !IsFullWidthScalarInput(X86.operands[1]))
        return Reject(
            "v1 TEST requires full-width GPR register/immediate form");
      {
        const std::optional<NdVar> Left =
            ScalarInputVariable(X86.operands[0]);
        const std::optional<NdVar> Right =
            ScalarInputVariable(X86.operands[1]);
        const size_t AndOperations =
            llvm::count_if(InstructionOps, [&](const LowOp &Operation) {
              return Left && Right && Operation.Opcode == NdOp::INT_AND &&
                     Operation.Output.isTemp() &&
                     Operation.Output.Size == sizeof(uint64_t) &&
                     Operation.NumInputs == 2 &&
                     Operation.Inputs[0] == *Left &&
                     Operation.Inputs[1] == *Right;
            });
        constexpr std::array<uint64_t, 5> RequiredFlags = {
            x86reg::CF, x86reg::PF, x86reg::ZF, x86reg::SF, x86reg::OF};
        const bool HasAllFlagWrites =
            llvm::all_of(RequiredFlags, [&](uint64_t Flag) {
              return llvm::count_if(
                         InstructionOps, [&](const LowOp &Operation) {
                           return Operation.Output == NdVar::reg(Flag, 1);
                         }) == 1;
            });
        const bool HasNoAFWrite =
            llvm::none_of(InstructionOps, [](const LowOp &Operation) {
              return Operation.Output == NdVar::reg(x86reg::AF, 1);
            });
        const bool HasNoGPRWrite =
            llvm::none_of(InstructionOps, [](const LowOp &Operation) {
              return Operation.Output.isReg() &&
                     Operation.Output.Size == sizeof(uint64_t) &&
                     Operation.Output.Offset <= x86reg::R15;
            });
        if (AndOperations != 1 || !HasAllFlagWrites || !HasNoAFWrite ||
            !HasNoGPRWrite)
          return Reject("v1 TEST LowIR shape is not canonical");
      }
      break;
    case X86_INS_RET:
      if (BoundaryIndex + 1 != Block.InstructionBoundaries.size() ||
          X86.rex != 0 || (X86.op_count != 0 && X86.op_count != 1) ||
          (X86.op_count == 1 && X86.operands[0].type != X86_OP_IMM))
        return Reject("v1 RET must be the final instruction");
      if (!((InstructionBytes.size() == 1 && InstructionBytes[0] == 0xc3) ||
            (InstructionBytes.size() == 3 && InstructionBytes[0] == 0xc2)))
        return Reject("v1 RET requires canonical C3 or C2 iw encoding");
      if (InstructionOps.size() != 1 ||
          InstructionOps.front().Opcode != NdOp::RETURN ||
          InstructionOps.front().NumInputs != 1 ||
          InstructionOps.front().Inputs[0] !=
              NdVar::reg(x86reg::RAX, sizeof(uint64_t)))
        return Reject("v1 RET LowIR shape is not canonical");
      break;
    case X86_INS_JMP:
      if (BoundaryIndex + 1 != Block.InstructionBoundaries.size() ||
          X86.rex != 0 || X86.op_count != 1 ||
          X86.operands[0].type != X86_OP_IMM)
        return Reject("v1 JMP must be the final direct-relative instruction");
      if (!((InstructionBytes.size() == 2 && InstructionBytes[0] == 0xeb) ||
            (InstructionBytes.size() == 5 && InstructionBytes[0] == 0xe9)))
        return Reject("v1 JMP requires canonical EB cb or E9 cd encoding");
      if (InstructionOps.size() != 1 ||
          InstructionOps.front().Opcode != NdOp::BRANCH ||
          InstructionOps.front().Output.Size != 0 ||
          InstructionOps.front().NumInputs != 1 ||
          !InstructionOps.front().Inputs[0].isConst() ||
          InstructionOps.front().Inputs[0].Size != sizeof(uint64_t) ||
          InstructionOps.front().Inputs[0].Offset !=
              Block.Header.StaticTargetPC ||
          static_cast<uint64_t>(X86.operands[0].imm) !=
              Block.Header.StaticTargetPC)
        return Reject("v1 JMP LowIR and descriptor targets are not exact");
      break;
    case X86_INS_JO:
    case X86_INS_JNO:
    case X86_INS_JB:
    case X86_INS_JAE:
    case X86_INS_JE:
    case X86_INS_JNE:
    case X86_INS_JS:
    case X86_INS_JNS:
    case X86_INS_JP:
    case X86_INS_JNP: {
      const std::optional<SingleFlagBranchEncoding> Encoding =
          singleFlagBranchEncoding(Instruction.Id);
      if (!Encoding)
        return Reject("v1 single-flag branch has no publication contract");
      if (BoundaryIndex + 1 != Block.InstructionBoundaries.size() ||
          X86.rex != 0 || X86.op_count != 1 ||
          X86.operands[0].type != X86_OP_IMM)
        return Reject("v1 single-flag branch must be the final "
                      "direct-relative instruction");
      const bool HasCanonicalEncoding =
          (InstructionBytes.size() == 2 &&
           InstructionBytes[0] == Encoding->ShortOpcode) ||
          (InstructionBytes.size() == 6 && InstructionBytes[0] == 0x0f &&
           InstructionBytes[1] == Encoding->NearOpcode);
      if (!HasCanonicalEncoding)
        return Reject("v1 single-flag branch requires its exact canonical "
                      "short or near relative encoding");
      if (InstructionOps.size() != 2 ||
          InstructionOps[0].Opcode != Encoding->PredicateOpcode ||
          !InstructionOps[0].Output.isTemp() ||
          InstructionOps[0].Output.Size != sizeof(uint8_t) ||
          InstructionOps[0].NumInputs != 1 ||
          InstructionOps[0].Inputs[0] != NdVar::reg(Encoding->Flag, 1) ||
          InstructionOps[1].Opcode != NdOp::COND_BR ||
          InstructionOps[1].Output.Size != 0 ||
          InstructionOps[1].NumInputs != 2 ||
          !InstructionOps[1].Inputs[0].isConst() ||
          InstructionOps[1].Inputs[0].Size != sizeof(uint64_t) ||
          InstructionOps[1].Inputs[0].Offset != Block.Header.StaticTargetPC ||
          InstructionOps[1].Inputs[1] != InstructionOps[0].Output ||
          static_cast<uint64_t>(X86.operands[0].imm) !=
              Block.Header.StaticTargetPC)
        return Reject("v1 single-flag branch LowIR predicate and descriptor "
                      "targets are not exact");
      break;
    }
    case X86_INS_JBE:
    case X86_INS_JA:
    case X86_INS_JL:
    case X86_INS_JGE:
    case X86_INS_JLE:
    case X86_INS_JG: {
      const std::optional<MultiFlagBranchEncoding> Encoding =
          multiFlagBranchEncoding(Instruction.Id);
      if (!Encoding)
        return Reject("v1 multi-flag branch has no publication contract");
      if (BoundaryIndex + 1 != Block.InstructionBoundaries.size() ||
          X86.rex != 0 || X86.op_count != 1 ||
          X86.operands[0].type != X86_OP_IMM)
        return Reject("v1 multi-flag branch must be the final "
                      "direct-relative instruction");
      const bool HasCanonicalEncoding =
          (InstructionBytes.size() == 2 &&
           InstructionBytes[0] == Encoding->ShortOpcode) ||
          (InstructionBytes.size() == 6 && InstructionBytes[0] == 0x0f &&
           InstructionBytes[1] == Encoding->NearOpcode);
      if (!HasCanonicalEncoding)
        return Reject("v1 multi-flag branch requires its exact canonical "
                      "short or near relative encoding");
      if (!hasCanonicalMultiFlagBranchShape(InstructionOps, Encoding->Predicate,
                                            Block.Header.StaticTargetPC) ||
          static_cast<uint64_t>(X86.operands[0].imm) !=
              Block.Header.StaticTargetPC)
        return Reject("v1 multi-flag branch LowIR predicate and descriptor "
                      "targets are not exact");
      break;
    }
    default:
      return Reject("guest instruction is outside the published v1 scalar "
                    "register and canonical control-flow subset");
    }
    ByteOffset += Boundary.Size;
  }
  if (ByteOffset != Block.Bytes.size())
    return failure(TranslationBlockLoweringErrorCode::InvalidDescriptor, Block);
  return llvm::Error::success();
}

bool equalLowOperation(const LowOp &Left, const LowOp &Right) {
  if (Left.Opcode != Right.Opcode || Left.Output != Right.Output ||
      Left.NumInputs != Right.NumInputs || Left.Addr != Right.Addr ||
      Left.Seq != Right.Seq)
    return false;
  return std::equal(std::begin(Left.Inputs), std::end(Left.Inputs),
                    std::begin(Right.Inputs));
}

bool equalInstructionBoundary(const LowInstructionBoundary &Left,
                              const LowInstructionBoundary &Right) {
  return Left.Address == Right.Address && Left.Size == Right.Size &&
         Left.FirstOp == Right.FirstOp && Left.OpCount == Right.OpCount &&
         Left.Mode == Right.Mode && Left.Control == Right.Control &&
         Left.ControlFlags == Right.ControlFlags &&
         Left.TargetMode == Right.TargetMode &&
         Left.Immediate == Right.Immediate;
}

llvm::Error
validateCanonicalSemantics(const TranslationBlockDescriptorV1 &Block) {
  llvm::Expected<GuestState> StateOrErr =
      createZeroedGuestState(GuestArchitecture::X86_64);
  if (!StateOrErr)
    return failure(TranslationBlockLoweringErrorCode::InvalidDescriptor, Block,
                   std::nullopt, std::nullopt,
                   llvm::toString(StateOrErr.takeError()));
  StateOrErr->Memory.push_back(
      {Block.Header.EntryPC, MemoryPermission::Read | MemoryPermission::Execute,
       /*Generation=*/1, Block.Bytes});

  llvm::Expected<std::unique_ptr<GuestMemoryRuntime>> RuntimeOrErr =
      GuestMemoryRuntime::create(*StateOrErr);
  if (!RuntimeOrErr)
    return failure(TranslationBlockLoweringErrorCode::InvalidDescriptor, Block,
                   std::nullopt, std::nullopt,
                   llvm::toString(RuntimeOrErr.takeError()));
  llvm::Expected<std::unique_ptr<X86TranslationBlockBuilder>> BuilderOrErr =
      X86TranslationBlockBuilder::create();
  if (!BuilderOrErr)
    return failure(TranslationBlockLoweringErrorCode::InvalidDescriptor, Block,
                   std::nullopt, std::nullopt,
                   llvm::toString(BuilderOrErr.takeError()));
  llvm::Expected<TranslationBlockDescriptorV1> CanonicalOrErr =
      (*BuilderOrErr)
          ->build(**RuntimeOrErr, Block.Header.EntryPC,
                  Block.Header.GuestInstructionCount);
  if (!CanonicalOrErr)
    return failure(TranslationBlockLoweringErrorCode::InvalidDescriptor, Block,
                   std::nullopt, std::nullopt,
                   llvm::toString(CanonicalOrErr.takeError()));
  const TranslationBlockDescriptorV1 &Canonical = *CanonicalOrErr;

  const bool HeaderMatches =
      Canonical.Header.FallthroughPC == Block.Header.FallthroughPC &&
      Canonical.Header.StaticTargetPC == Block.Header.StaticTargetPC &&
      Canonical.Header.GuestInstructionCount ==
          Block.Header.GuestInstructionCount &&
      Canonical.Header.GuestByteCount == Block.Header.GuestByteCount &&
      Canonical.Header.ReturnImmediate == Block.Header.ReturnImmediate &&
      Canonical.Header.Terminator == Block.Header.Terminator &&
      Canonical.Header.Flags == Block.Header.Flags;
  const bool OperationsMatch =
      Canonical.Ops.size() == Block.Ops.size() &&
      std::equal(Canonical.Ops.begin(), Canonical.Ops.end(), Block.Ops.begin(),
                 equalLowOperation);
  const bool BoundariesMatch =
      Canonical.InstructionBoundaries.size() ==
          Block.InstructionBoundaries.size() &&
      std::equal(Canonical.InstructionBoundaries.begin(),
                 Canonical.InstructionBoundaries.end(),
                 Block.InstructionBoundaries.begin(), equalInstructionBoundary);
  if (!HeaderMatches || Canonical.Bytes != Block.Bytes || !OperationsMatch ||
      !BoundariesMatch)
    return failure(TranslationBlockLoweringErrorCode::InvalidDescriptor, Block,
                   std::nullopt, std::nullopt,
                   "guest bytes and canonical LowIR disagree");
  return llvm::Error::success();
}

} // namespace

llvm::Expected<LoweredTranslationBlockV1>
lowerX86TranslationBlockV1(const TranslationBlockDescriptorV1 &Block,
                           const ResolvedHostTarget &HostTarget,
                           const llvm::DataLayout &HostDataLayout,
                           llvm::LLVMContext &Context) {
  if (llvm::Error Error = validateTranslationBlockDescriptorV1(Block))
    return failure(TranslationBlockLoweringErrorCode::InvalidDescriptor, Block,
                   std::nullopt, std::nullopt,
                   llvm::toString(std::move(Error)));
  if (llvm::Error Error = validateCanonicalSemantics(Block))
    return std::move(Error);
  if (llvm::Error Error = validatePublishedScalarSlice(Block))
    return std::move(Error);

  const llvm::Triple Triple(HostTarget.triple());
  if (HostTarget.architecture() != GuestArchitecture::AArch64 ||
      Triple.getArch() != llvm::Triple::aarch64)
    return failure(TranslationBlockLoweringErrorCode::UnsupportedHostTarget,
                   Block, std::nullopt, std::nullopt, HostTarget.triple());
  if (!HostTarget.hasCanonicalDataLayout())
    return failure(
        TranslationBlockLoweringErrorCode::InvalidHostDataLayout, Block,
        std::nullopt, std::nullopt,
        "host target has no canonical target-machine data-layout binding");
  const std::string SuppliedDataLayout =
      HostDataLayout.getStringRepresentation();
  if (llvm::StringRef(SuppliedDataLayout) != HostTarget.canonicalDataLayout())
    return failure(TranslationBlockLoweringErrorCode::InvalidHostDataLayout,
                   Block, std::nullopt, std::nullopt,
                   "supplied data layout does not exactly match the canonical "
                   "target-machine layout");
  if (HostDataLayout.isDefault() || !HostDataLayout.isLittleEndian() ||
      HostDataLayout.getPointerSizeInBits(0) != 64)
    return failure(
        TranslationBlockLoweringErrorCode::InvalidHostDataLayout, Block,
        std::nullopt, std::nullopt,
        "AArch64 lowering requires an explicit little-endian 64-bit layout");

  const std::string Symbol = blockSymbol(Block.Header.EntryPC);
  auto Module =
      std::make_unique<llvm::Module>("nvd.x86_64.translation.block", Context);
  Module->setTargetTriple(Triple);
  Module->setDataLayout(HostDataLayout);

  llvm::Type *Pointer = llvm::PointerType::getUnqual(Context);
  llvm::FunctionType *Type = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(Context), {Pointer, Pointer}, false);
  llvm::Function *Function = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, Symbol, Module.get());
  Function->setCallingConv(llvm::CallingConv::C);
  Function->setVisibility(llvm::GlobalValue::HiddenVisibility);
  Function->setDSOLocal(true);
  Function->addFnAttr(llvm::Attribute::NoUnwind);
  Function->getArg(0)->setName("state");
  Function->getArg(1)->setName("runtime");
  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);

  X86BlockIRLowering Lowering(Block, *Module, *Function, *Entry);
  if (llvm::Error Error = Lowering.lower())
    return std::move(Error);
  if (llvm::Error Error = verifyRuntimeTranslationIRV1(
          *Module, Triple, HostDataLayout, kRuntimeGuestStateX86_64SizeV1,
          runtimeGuestStateX86_64MemorySlotsV1()))
    return failure(TranslationBlockLoweringErrorCode::IRVerificationFailed,
                   Block, std::nullopt, std::nullopt,
                   llvm::toString(std::move(Error)));

  return LoweredTranslationBlockV1(std::move(Module), Symbol);
}

} // namespace neverd::translate
