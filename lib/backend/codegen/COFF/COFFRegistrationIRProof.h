//===- COFFRegistrationIRProof.h - Shared PE32 IR contract checks ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_COFFREGISTRATIONIRPROOF_H
#define NEVERD_COFFREGISTRATIONIRPROOF_H
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/BinaryRewrite.h"
#include "llvm/Support/Errc.h"

#include <map>
#include <set>
namespace neverd::coff_registration {
inline llvm::Error rejectIR(const llvm::Twine &Detail) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "coff registration patch: " + Detail);
}
inline const llvm::Instruction *next(const llvm::Instruction &Instruction) {
  const llvm::Instruction *Result = Instruction.getNextNode();
  while (Result && Result->isDebugOrPseudoInst())
    Result = Result->getNextNode();
  return Result;
}

inline std::optional<uint64_t> integer(const llvm::Value *Value,
                                       unsigned Width) {
  const auto *C = llvm::dyn_cast_or_null<llvm::ConstantInt>(Value);
  if (!C || C->getBitWidth() != Width)
    return std::nullopt;
  return C->getZExtValue();
}

inline bool
exactSemanticToken(const llvm::Instruction &Pad,
                   const llvm::mc_rewrite::RewriteWinEHSemanticToken &Token) {
  const auto *MD =
      Pad.getMetadata(llvm::mc_rewrite::RewriteWinEHSemanticAttachment);
  if (!MD || MD->getNumOperands() !=
                 llvm::mc_rewrite::RewriteWinEHSemanticOperandCount)
    return false;
  const uint64_t Values[] = {
      llvm::mc_rewrite::RewriteWinEHSemanticSchemaVersion,
      static_cast<uint8_t>(Token.Kind),
      Token.Region,
      Token.Clause,
      Token.Digest[0],
      Token.Digest[1],
      Token.Digest[2],
      Token.Digest[3]};
  const unsigned Widths[] = {32, 8, 32, 32, 64, 64, 64, 64};
  static_assert(std::size(Values) ==
                llvm::mc_rewrite::RewriteWinEHSemanticOperandCount);
  for (unsigned I = 0; I < std::size(Values); ++I) {
    const auto *CAM = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(
        MD->getOperand(I).get());
    if (!CAM || integer(CAM->getValue(), Widths[I]) != Values[I])
      return false;
  }
  return true;
}

using CallbackKey = std::pair<va_t, bool>;

/// Authenticate the actual callback set, runtime roots and recovered frame.
/// Table validation uses the same identities as independent source replay.
llvm::Expected<std::map<CallbackKey, const llvm::Function *>> callbacks(
    const llvm::Function &Parent, const ExceptionFunction &Source,
    const std::map<CallbackKey, std::set<uint8_t>> *ExpectedRoots = nullptr,
    std::map<const llvm::Function *, const llvm::StoreInst *>
        *ExceptionBridges = nullptr);

llvm::Error validateSymbolicImagePointers(const llvm::Module &Module,
                                          uint64_t ImageBase,
                                          uint64_t ImageSize);
llvm::Error
validateIncomingCallerFrame(const llvm::Function &Parent, const MedFunc &Source,
                            llvm::ArrayRef<const llvm::Function *> Functions,
                            std::set<const llvm::Instruction *> &Accesses,
                            std::set<const llvm::Instruction *> &Setup);

struct RegistrationFrame {
  const llvm::AllocaInst *Slot = nullptr;
  uint64_t EntrySP = 0;
  uint64_t Establisher = 0;
};

inline std::optional<uint64_t> metadataInteger(const llvm::MDNode &Node,
                                               unsigned Index, unsigned Width) {
  if (Index >= Node.getNumOperands())
    return std::nullopt;
  const auto *Value = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(
      Node.getOperand(Index).get());
  return Value ? integer(Value->getValue(), Width) : std::nullopt;
}

inline llvm::Expected<RegistrationFrame>
registrationFrame(const llvm::Function &Parent,
                  const ExceptionFunction &Source) {
  RegistrationFrame Frame;
  for (const auto &Block : Parent)
    for (const auto &Instruction : Block)
      if (const auto *Node = Instruction.getMetadata(
              windows_eh_md::RegistrationFrameAttachment)) {
        const auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(&Instruction);
        auto Entry = metadataInteger(*Node, 1, 64);
        if (Frame.Slot || !Slot || Node->getNumOperands() != 2 ||
            metadataInteger(*Node, 0, 64) != Source.CodeRange.Begin || !Entry ||
            *Entry < 24 || *Entry > UINT32_MAX || !Slot->isStaticAlloca() ||
            Slot->getAddressSpace() || &Block != &Parent.getEntryBlock())
          return rejectIR("native registration frame identity changed");
        auto Size =
            Slot->getAllocationSize(Parent.getParent()->getDataLayout());
        if (!Size || Size->isScalable() || *Entry > Size->getFixedValue() ||
            Size->getFixedValue() > UINT32_MAX)
          return rejectIR("native registration frame extent changed");
        Frame = {Slot, *Entry, *Entry - 4};
      }
  if (!Frame.Slot)
    return rejectIR("native registration frame has no compiler-owned identity");
  return Frame;
}

struct SourceSegment {
  const llvm::Instruction *Enter = nullptr;
  const llvm::Instruction *Exit = nullptr;
};
bool matchesSourceOperation(const llvm::Instruction &I, va_t Owner,
                            va_t Address, uint8_t Kind);
llvm::Expected<std::map<int, SourceSegment>> validateSourceSegments(
    const llvm::Function &Parent, const MedFunc &Source,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks,
    llvm::ArrayRef<const llvm::Function *> Functions,
    bool ReachedCxxSource = false);
} // namespace neverd::coff_registration
#endif
