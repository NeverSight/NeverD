//===- WindowsSEHEmulatedFrame.h - x64 SEH callback frames ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// The original frame a lifted Windows x64 parent keeps for its SEH callbacks,
/// and what a callback reaches through the frame it is passed.

#ifndef NEVERD_BACKEND_LLVM_WINDOWSSEHEMULATEDFRAME_H
#define NEVERD_BACKEND_LLVM_WINDOWSSEHEMULATEDFRAME_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace neverd {

/// The alloca a lifted Windows x64 parent keeps its original frame in: the
/// byte at \ref EntryOffset stands for the stack pointer the original was
/// entered with.
struct SEHEmulatedFrame {
  const llvm::AllocaInst *Slot = nullptr;
  uint64_t EntryOffset = 0;
  uint64_t Size = 0;
};

/// The emulated frame native IR marked in \p Parent
/// (windows_eh_md::SEHEmulatedFrameAttachment): one static alloca of the
/// entry block, of fixed size, naming \p FunctionVA, whose entry byte lies
/// inside it.  None when nothing is marked; an error when a mark does not
/// hold.
llvm::Expected<std::optional<SEHEmulatedFrame>>
findSEHEmulatedFrame(const llvm::Function &Parent, va_t FunctionVA);

/// \p Value as a pointer it is computed from and a constant byte offset from
/// that pointer: through GEPs with constant indices, pointer casts, ptrtoint
/// and inttoptr at pointer width, freezes, and additions and subtractions of
/// constants.  None for anything else.
std::optional<std::pair<const llvm::Value *, int64_t>>
sehConstantAddress(const llvm::Value &Value, const llvm::DataLayout &Layout);

/// One access a callback makes through the frame it is passed: bytes
/// [Offset, Offset + Size) from that frame.
struct SEHFrameAccess {
  int64_t Offset = 0;
  uint64_t Size = 0;
  bool Write = false;
};

/// Every access \p Callback makes through its second argument, the
/// EstablisherFrame, by byte offset from that frame.  The frame may rest in
/// a home (an alloca that only its own loads and stores name, as a lifted
/// register does) or in a fixed slot of the callback's own frame and be read
/// back from there, also where another path leaves something else there.
/// None when the frame or an address computed from it leaves the callback
/// (to a call, a return, or memory other than those), is offset by anything
/// but a constant, or meets two offsets at one value; when a value read
/// through the frame reaches a call, since a stack address the lifted parent
/// keeps there is no frame the runtime knows; and when the module holds no
/// body of \p Callback.
std::optional<std::vector<SEHFrameAccess>>
collectSEHCallbackFrameAccesses(const llvm::Function &Callback);

/// The first memory access in \p Protected, blocks of \p Frame's function
/// that a scope guards, that could reach memory out of order with a fault
/// there, which no plain load or store can raise to LLVM: one that is not
/// volatile and is neither a store at a fixed offset inside a private slot of
/// the function (an alloca only its own loads and stores name, which no
/// callback can reach) nor a load at a fixed offset inside an alloca, which
/// cannot fault.  Null when there is none.
const llvm::Instruction *
findSEHUnorderedAccess(const SEHEmulatedFrame &Frame,
                       llvm::ArrayRef<const llvm::BasicBlock *> Protected);

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_WINDOWSSEHEMULATEDFRAME_H
