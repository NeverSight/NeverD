// Test the compiled meaning of HighC, independently of temporary spellings.
#ifndef NEVERD_TEST_AARCH64_HIGHC_BEHAVIOR_H
#define NEVERD_TEST_AARCH64_HIGHC_BEHAVIOR_H

#include "NeverDLiftFixture.h"

#include "llvm/ADT/APInt.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <tuple>

class AArch64HighCBehaviorTest : public NeverDLiftTest {
protected:
  std::unique_ptr<llvm::Module> compileHighCFlow(const fs::path &Source,
                                                 const std::string &March,
                                                 llvm::LLVMContext &Context) {
    if (!hasCrossTargetClang()) {
      ADD_FAILURE() << "compiled HighC value-flow checks require Clang";
      return nullptr;
    }
    const auto Include = tmpFile("flow-include");
    fs::create_directory(Include);
    std::ofstream(Include / "string.h")
        << "void *memcpy(void *, const void *, __SIZE_TYPE__);\n";
    const auto IR = tmpFile("highc-flow.ll");
    // Use hosted memcpy semantics so Clang exposes the actual byte copies as
    // IR loads/stores. No behavioral stubs replace the instructions under test.
    auto Compile =
        exec(NEVERD_TEST_CLANG,
             {"-target", "aarch64-none-elf", "-march=" + March, "-std=gnu11",
              "-O2", "-S", "-emit-llvm", "-I", Include.string(), "-include",
              std::string(TEST_SOURCE_DIR) + "/aarch64/HostClangBuiltins.h",
              Source.string(), "-o", IR.string()});
    if (Compile.exitCode) {
      ADD_FAILURE() << Compile.err;
      return nullptr;
    }
    llvm::SMDiagnostic Error;
    auto Module = llvm::parseIRFile(IR.string(), Error, Context);
    if (!Module) {
      std::string Diagnostic;
      llvm::raw_string_ostream OS(Diagnostic);
      Error.print("compiled HighC", OS);
      ADD_FAILURE() << OS.str();
    }
    return Module;
  }

  void executePortableHighC(const std::string &Source,
                            const std::string &Harness) {
    ASSERT_TRUE(hasCrossTargetClang()) << "HighC execution requires Clang";
    auto CFile = tmpFile("highc-behavior.c");
    std::ofstream(CFile) << Source << "\n" << Harness;
    auto Program = tmpFile("highc-behavior.exe");
    auto Compile = exec(NEVERD_TEST_CLANG, {"-std=c11", "-O2", CFile.string(),
                                            "-o", Program.string()});
    ASSERT_EQ(Compile.exitCode, 0) << Compile.err << "\n" << Source;
    auto Run = exec(Program.string(), {});
    EXPECT_EQ(Run.exitCode, 0) << Run.err << "\n" << Source;
  }
};

namespace a64_highc_test {

// These fixtures compile to straight-line IR. Resolve a load only from an
// earlier, equally sized store to the same private stack bytes. This preserves
// snapshots: a later store must never change what an earlier load means.
class ValueFlow {
  struct Address {
    const llvm::Value *Base;
    int64_t Offset;
    bool operator<(const Address &Other) const {
      return std::tie(Base, Offset) < std::tie(Other.Base, Other.Offset);
    }
  };
  struct Cell {
    const llvm::Value *Value;
    uint64_t Bytes;
  };
  const llvm::DataLayout &Layout;
  std::map<const llvm::Value *, const llvm::Value *> Snapshots;
  std::map<Address, Cell> Memory;
  bool Valid = true;

  bool privateBytes(const Address &Location, uint64_t Bytes) const {
    const auto *Allocation = llvm::dyn_cast<llvm::AllocaInst>(Location.Base);
    const auto *Count =
        Allocation
            ? llvm::dyn_cast<llvm::ConstantInt>(Allocation->getArraySize())
            : nullptr;
    if (!Count || !Count->equalsInt(1) || Location.Offset < 0)
      return false;
    const uint64_t Size =
        Layout.getTypeAllocSize(Allocation->getAllocatedType()).getFixedValue();
    return static_cast<uint64_t>(Location.Offset) <= Size &&
           Bytes <= Size - static_cast<uint64_t>(Location.Offset);
  }

  std::optional<Address> address(const llvm::Value *V,
                                 unsigned Depth = 0) const {
    if (!V || Depth > 64)
      return std::nullopt;
    if (llvm::isa<llvm::AllocaInst>(V))
      return Address{V, 0};
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V)) {
      if (Cast->getOpcode() == llvm::Instruction::BitCast ||
          Cast->getOpcode() == llvm::Instruction::PtrToInt ||
          Cast->getOpcode() == llvm::Instruction::IntToPtr)
        return address(Cast->getOperand(0), Depth + 1);
    }
    auto addOffset = [](std::optional<Address> Base, int64_t Offset) {
      if (!Base)
        return Base;
      bool Overflow = false;
      auto Sum = llvm::APInt(64, Base->Offset, true)
                     .sadd_ov(llvm::APInt(64, Offset, true), Overflow);
      if (Overflow)
        return std::optional<Address>{};
      Base->Offset = Sum.getSExtValue();
      return Base;
    };
    if (const auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(V)) {
      llvm::APInt Offset(Layout.getIndexTypeSizeInBits(GEP->getType()), 0);
      if (!GEP->accumulateConstantOffset(Layout, Offset) ||
          !Offset.isSignedIntN(64))
        return std::nullopt;
      return addOffset(address(GEP->getPointerOperand(), Depth + 1),
                       Offset.getSExtValue());
    }
    if (const auto *Op = llvm::dyn_cast<llvm::BinaryOperator>(V)) {
      if (Op->getOpcode() != llvm::Instruction::Add &&
          Op->getOpcode() != llvm::Instruction::Sub)
        return std::nullopt;
      const auto *Constant =
          llvm::dyn_cast<llvm::ConstantInt>(Op->getOperand(1));
      if (!Constant || !Constant->getValue().isSignedIntN(64))
        return std::nullopt;
      auto Offset = Constant->getValue().sextOrTrunc(64);
      if (Op->getOpcode() == llvm::Instruction::Sub) {
        if (Offset.isMinSignedValue())
          return std::nullopt;
        Offset = -Offset;
      }
      return addOffset(address(Op->getOperand(0), Depth + 1),
                       Offset.getSExtValue());
    }
    return std::nullopt;
  }

public:
  explicit ValueFlow(const llvm::Function &Function)
      : Layout(Function.getParent()->getDataLayout()) {
    if (Function.size() != 1) {
      Valid = false;
      return;
    }
    for (const llvm::Instruction &Instruction : Function.front()) {
      if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction)) {
        auto Address = address(Store->getPointerOperand());
        if (!Address || Store->isAtomic() || Store->isVolatile()) {
          Valid = false;
          continue;
        }
        const auto Bytes =
            Layout.getTypeStoreSize(Store->getValueOperand()->getType())
                .getFixedValue();
        if (!privateBytes(*Address, Bytes)) {
          Valid = false;
          continue;
        }
        for (auto It = Memory.begin(); It != Memory.end();) {
          if (It->first.Base == Address->Base &&
              It->first.Offset <
                  Address->Offset + static_cast<int64_t>(Bytes) &&
              Address->Offset <
                  It->first.Offset + static_cast<int64_t>(It->second.Bytes))
            It = Memory.erase(It);
          else
            ++It;
        }
        Memory[*Address] = {Store->getValueOperand(), Bytes};
      } else if (const auto *Load =
                     llvm::dyn_cast<llvm::LoadInst>(&Instruction)) {
        auto Address = address(Load->getPointerOperand());
        auto Cell = Address ? Memory.find(*Address) : Memory.end();
        if (Cell == Memory.end() || Load->isAtomic() || Load->isVolatile() ||
            Cell->second.Bytes !=
                Layout.getTypeStoreSize(Load->getType()).getFixedValue()) {
          Valid = false;
          continue;
        }
        Snapshots[Load] = Cell->second.Value;
      } else if (const auto *Call =
                     llvm::dyn_cast<llvm::CallBase>(&Instruction)) {
        const auto *Callee = Call->getCalledFunction();
        if (!Callee || !Callee->isIntrinsic()) {
          Valid = false;
          continue;
        }
        // A surviving memcpy or an unknown call cannot silently preserve our
        // private-memory map. These fixture intrinsics do not write its bytes.
        const auto Name = Callee->getName();
        if (!(Name.starts_with("llvm.lifetime.") ||
              Name.starts_with("llvm.aarch64.fjcvtzs") ||
              Name.starts_with("llvm.experimental.constrained.") ||
              Name.starts_with("llvm.nearbyint.") ||
              Name.starts_with("llvm.read_register.") ||
              Name.starts_with("llvm.read_volatile_register.") ||
              Name.starts_with("llvm.write_register.")))
          Valid = false;
      }
    }
  }

  bool valid() const { return Valid; }

  const llvm::Value *origin(const llvm::Value *Value) const {
    for (unsigned Depth = 0; Value && Depth < 64; ++Depth) {
      if (auto It = Snapshots.find(Value); It != Snapshots.end()) {
        Value = It->second;
        continue;
      }
      const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Value);
      if (Cast && (Cast->getOpcode() == llvm::Instruction::BitCast ||
                   Cast->getOpcode() == llvm::Instruction::PtrToInt ||
                   Cast->getOpcode() == llvm::Instruction::IntToPtr)) {
        Value = Cast->getOperand(0);
        continue;
      }
      return Value;
    }
    return nullptr;
  }

  using Bindings = std::map<const llvm::Value *, llvm::APInt>;

  std::optional<llvm::APInt> integer(const llvm::Value *Value,
                                     const Bindings &Inputs,
                                     unsigned Depth = 0) const {
    if (!Value || Depth > 64)
      return std::nullopt;
    if (auto It = Inputs.find(Value); It != Inputs.end())
      return It->second;
    if (const auto *Constant = llvm::dyn_cast<llvm::ConstantInt>(Value))
      return Constant->getValue();
    if (auto It = Snapshots.find(Value); It != Snapshots.end())
      return integer(It->second, Inputs, Depth + 1);
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Value)) {
      auto Operand = integer(Cast->getOperand(0), Inputs, Depth + 1);
      if (!Operand || !Cast->getType()->isIntegerTy())
        return std::nullopt;
      const unsigned Bits = Cast->getType()->getIntegerBitWidth();
      if (Cast->getOpcode() == llvm::Instruction::SExt)
        return Operand->sextOrTrunc(Bits);
      if (Cast->getOpcode() == llvm::Instruction::ZExt ||
          Cast->getOpcode() == llvm::Instruction::Trunc ||
          Cast->getOpcode() == llvm::Instruction::BitCast)
        return Operand->zextOrTrunc(Bits);
      return std::nullopt;
    }
    const auto *Operation = llvm::dyn_cast<llvm::BinaryOperator>(Value);
    if (Operation) {
      auto Left = integer(Operation->getOperand(0), Inputs, Depth + 1);
      auto Right = integer(Operation->getOperand(1), Inputs, Depth + 1);
      if (!Left || !Right || Left->getBitWidth() != Right->getBitWidth())
        return std::nullopt;
      switch (Operation->getOpcode()) {
      case llvm::Instruction::And:
        return *Left & *Right;
      case llvm::Instruction::Or:
        return *Left | *Right;
      case llvm::Instruction::Xor:
        return *Left ^ *Right;
      case llvm::Instruction::Add:
        return *Left + *Right;
      case llvm::Instruction::Sub:
        return *Left - *Right;
      case llvm::Instruction::Shl:
      case llvm::Instruction::LShr:
        if (Right->uge(Left->getBitWidth()))
          return std::nullopt;
        return Operation->getOpcode() == llvm::Instruction::Shl
                   ? Left->shl(Right->getZExtValue())
                   : Left->lshr(Right->getZExtValue());
      default:
        return std::nullopt;
      }
    }
    if (const auto *Compare = llvm::dyn_cast<llvm::ICmpInst>(Value)) {
      auto Left = integer(Compare->getOperand(0), Inputs, Depth + 1);
      auto Right = integer(Compare->getOperand(1), Inputs, Depth + 1);
      if (!Left || !Right || Left->getBitWidth() != Right->getBitWidth())
        return std::nullopt;
      if (Compare->getPredicate() == llvm::CmpInst::ICMP_EQ)
        return llvm::APInt(1, *Left == *Right);
      if (Compare->getPredicate() == llvm::CmpInst::ICMP_NE)
        return llvm::APInt(1, *Left != *Right);
    }
    if (const auto *Select = llvm::dyn_cast<llvm::SelectInst>(Value)) {
      auto Condition = integer(Select->getCondition(), Inputs, Depth + 1);
      if (Condition)
        return integer(Condition->isZero() ? Select->getFalseValue()
                                           : Select->getTrueValue(),
                       Inputs, Depth + 1);
    }
    return std::nullopt;
  }
};

inline std::vector<const llvm::CallBase *> calls(const llvm::Function &Function,
                                                 llvm::StringRef Prefix) {
  std::vector<const llvm::CallBase *> Result;
  for (const auto &Block : Function)
    for (const auto &Instruction : Block)
      if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction))
        if (const auto *Callee = Call->getCalledFunction();
            Callee && Callee->getName().starts_with(Prefix))
          Result.push_back(Call);
  return Result;
}

inline bool metadataText(const llvm::Value *Value, llvm::StringRef Expected) {
  const auto *Metadata = llvm::dyn_cast<llvm::MetadataAsValue>(Value);
  if (!Metadata)
    return false;
  const llvm::Metadata *Payload = Metadata->getMetadata();
  if (const auto *Node = llvm::dyn_cast<llvm::MDNode>(Payload)) {
    if (Node->getNumOperands() != 1)
      return false;
    Payload = Node->getOperand(0).get();
  }
  const auto *Text = llvm::dyn_cast<llvm::MDString>(Payload);
  return Text && Text->getString().equals_insensitive(Expected);
}

} // namespace a64_highc_test
#endif
