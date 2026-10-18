//===- MachineInstructionText.h - The instruction behind a call -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The instruction an LLVM call or fence was lifted from, where the C a
/// writer prints for it leaves out what the instruction carries: the
/// `neverd.instruction` metadata holds the instruction as a listing spells
/// it (`brk #0x3E8`, `dmb ishld`, intrinsicInstructionText), which LLVM C
/// prints beside `__builtin_trap()` or the fence.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_MACHINEINSTRUCTIONTEXT_H
#define NEVERD_BACKEND_MACHINEINSTRUCTIONTEXT_H

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Metadata.h"

#include <string>

namespace neverd::machine_instruction_text {

inline constexpr llvm::StringLiteral Attachment("neverd.instruction");

/// Record \p Text as the instruction \p I stands for; nothing for no text.
inline void set(llvm::Instruction &I, llvm::StringRef Text) {
  if (Text.empty())
    return;
  llvm::LLVMContext &Ctx = I.getContext();
  I.setMetadata(Attachment,
                llvm::MDNode::get(Ctx, {llvm::MDString::get(Ctx, Text)}));
}

/// The instruction \p I stands for, or empty.
inline std::string get(const llvm::Instruction &I) {
  const llvm::MDNode *Node = I.getMetadata(Attachment);
  if (!Node || Node->getNumOperands() != 1)
    return {};
  if (const auto *Text = llvm::dyn_cast<llvm::MDString>(Node->getOperand(0)))
    return Text->getString().str();
  return {};
}

} // namespace neverd::machine_instruction_text

#endif // NEVERD_BACKEND_MACHINEINSTRUCTIONTEXT_H
