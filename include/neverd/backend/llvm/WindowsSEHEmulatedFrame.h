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
#include <string>
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

/// The adapter native IR writes for the filter of row \p Region of the
/// function at \p FunctionVA when the filter reads its frame: the runtime
/// calls it with the rewritten parent's frame, and it recovers the emulated
/// frame from there (`llvm.eh.recoverfp`, `llvm.localrecover`) and calls the
/// source filter with it.  Output finds the adapter's address by this name
/// among the compiler's receipts for the definitions it compiled.
std::string sehFilterAdapterName(va_t FunctionVA, uint32_t Region);

/// Whether \p Function is named as a filter adapter (sehFilterAdapterName).
bool isSEHFilterAdapter(const llvm::Function &Function);

/// Whether \p Module holds a filter adapter (sehFilterAdapterName): a
/// function with no source address that only the section patch can place.
bool hasSEHFilterAdapter(const llvm::Module &Module);

/// What a filter adapter of \p Parent does: the function it calls with the
/// exception pointers, and the byte of the alloca \p Parent escapes that it
/// passes as the EstablisherFrame.
struct SEHFilterAdapterCall {
  const llvm::CallInst *Call = nullptr;
  const llvm::Function *Filter = nullptr;
  const llvm::AllocaInst *Frame = nullptr;
  int64_t Offset = 0;
};

/// \p Adapter read as a filter adapter of \p Parent: one block that
/// recovers \p Parent's frame from its second argument (`llvm.eh.recoverfp`),
/// recovers an alloca \p Parent's one `llvm.localescape` in its entry block
/// names (`llvm.localrecover`), computes an address at a fixed offset from it
/// and returns what a direct call of a function with its first argument and
/// that address returns, and does nothing else.  None for anything else.
std::optional<SEHFilterAdapterCall>
matchSEHFilterAdapter(const llvm::Function &Adapter,
                      const llvm::Function &Parent);

/// The first memory access in \p Protected, blocks of \p Frame's function
/// that a scope guards, that could reach memory out of order with a fault
/// there, which no plain load or store can raise to LLVM: one that is not
/// volatile and is neither a store at a fixed offset inside a private slot of
/// the function (an alloca only its own loads and stores name, which no
/// callback can reach) nor a load at a fixed offset inside an alloca or a
/// global the image maps, which cannot fault.  Null when there is none.
const llvm::Instruction *
findSEHUnorderedAccess(const SEHEmulatedFrame &Frame,
                       llvm::ArrayRef<const llvm::BasicBlock *> Protected);

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_WINDOWSSEHEMULATEDFRAME_H
