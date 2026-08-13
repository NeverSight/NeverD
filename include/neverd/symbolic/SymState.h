//===- SymState.h - Symbolic machine state ----------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What every location a lifted program can name currently holds, as an
/// expression rather than a number.
///
/// Storage is by the byte, for registers as much as for memory.  That is not
/// an implementation detail: on x86 `EAX` *is* the low half of `RAX`, and a
/// register file keyed by name — or by offset, ignoring width — cannot say
/// what a four-byte write leaves an eight-byte read to see.  Addressing bytes
/// makes the overlap fall out instead of having to be enumerated, and it is
/// the same reason the same map shape serves for memory.
///
/// A location never written reads as a fresh input named after itself, so an
/// expression carries where its unknowns came from: `reg$0` rather than an
/// anonymous variable.  Nothing has to be initialised before execution starts,
/// and what comes out says which registers and addresses it depended on.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMSTATE_H
#define NEVERD_SYMBOLIC_SYMSTATE_H

#include "neverd/symbolic/SymExpr.h"

#include "llvm/Support/Endian.h"

#include <cstdint>
#include <map>
#include <memory>

namespace neverd::symbolic {

/// Where a byte lives.  Mirrors the address spaces a lifted operand can name,
/// with the two register-like ones kept apart because a lifter's temporaries
/// share an offset range with nothing.
enum class SymSpace : uint8_t { Register, Temporary, Memory };

/// A contiguous byte range in the register space.
struct SymRegisterRange {
  uint64_t Offset = 0;
  uint16_t Bytes = 0;
};

/// The state is a regular value: copying one is what forking a path is, so it
/// has to sit in a container and be assigned like anything else.  A copy is
/// three maps of small integers, which is cheap enough that nothing more
/// elaborate has earned its place yet.
class SymState {
public:
  SymState(SymContext &Ctx, llvm::endianness Order = llvm::endianness::little)
      : Ctx(&Ctx), Order(Order),
        SymbolicLoads(std::make_shared<SymbolicLoadValues>()) {}

  SymContext &context() const { return *Ctx; }
  llvm::endianness byteOrder() const { return Order; }

  //===--------------------------------------------------------------------===//
  // Registers and temporaries
  //===--------------------------------------------------------------------===//

  /// Read \p Bytes bytes starting at \p Offset.  A wholly untouched range
  /// starts as one input; overlapping reads thereafter use its exact byte
  /// views.  This keeps ordinary whole-register inputs compact without losing
  /// sub-register aliasing.
  SymRef read(SymSpace Space, uint64_t Offset, uint16_t Bytes);
  void write(SymSpace Space, uint64_t Offset, SymRef Value);

  //===--------------------------------------------------------------------===//
  // Memory
  //===--------------------------------------------------------------------===//

  /// Read \p Bytes at \p Addr.  A symbolic address reads as a fresh input.
  /// After an unknown store, concrete loads read stable unknown bytes except
  /// where a later concrete store has established a value again.
  SymRef load(SymRef Addr, uint16_t Bytes);
  /// Store \p Value and return whether its address was concrete.  A false
  /// result means memory was conservatively clobbered instead.
  bool store(SymRef Addr, SymRef Value);

  /// What a load that could not be resolved was reading.
  struct LoadOrigin {
    SymRef Address;
    uint16_t Bytes = 0;
  };

  /// The address behind a value that came from an unresolved load, if it did.
  ///
  /// The value itself is only a name — there is nothing in the expression
  /// language for "the contents of this address" — so the address would
  /// otherwise be lost at the moment it becomes interesting.  It is exactly
  /// what says a computed branch is a jump table: not that the target is
  /// unknown, but that it is a load from somewhere linear in an index.
  const LoadOrigin *loadOrigin(SymRef Value) const;

  /// True once a store through an address the engine could not pin down has
  /// happened.  From that point every load has to be treated as unknown,
  /// because the store may have landed on it.  Deliberately blunt: without an
  /// aliasing model the alternative to forgetting everything is being wrong
  /// about something.
  bool memoryIsUnknown() const { return MemoryClobbered; }
  void clobberMemory();

  //===--------------------------------------------------------------------===//
  // Whole-state operations
  //===--------------------------------------------------------------------===//

  /// Forget the registers a call is allowed to overwrite, keeping the ones it
  /// is not.  Passing an empty list forgets all of them.
  void clobberRegistersExcept(llvm::ArrayRef<SymRegisterRange> PreservedRanges);

  /// A value nothing determines, of the given width.  Every unknown the engine
  /// invents comes from here, so they are all named and none can collide.
  SymRef freshInput(llvm::StringRef Prefix, uint32_t Width);

  /// Number of bytes with a recorded value.  Diagnostic only.
  size_t numLiveBytes() const {
    return Registers.size() + Temporaries.size() + Memory.size();
  }

private:
  /// Lazily materialised bytes belonging to one clobber event.  States copied
  /// after that event share the set; independently clobbered forks do not.
  struct UnknownBytes {
    std::map<uint64_t, SymRef> Values;
  };

  /// Values read through unresolved addresses during one memory epoch.
  struct SymbolicLoadValues {
    std::map<uint32_t, SymRef> Bytes;
  };

  std::map<uint64_t, SymRef> &spaceMap(SymSpace Space);
  const std::map<uint64_t, SymRef> &spaceMap(SymSpace Space) const;
  std::shared_ptr<UnknownBytes> &unknownBytes(SymSpace Space);
  static const char *spaceName(SymSpace Space);

  /// One byte of a space, minting a named input for it when it has none.
  SymRef byteAt(SymSpace Space, uint64_t Offset);

  /// Join \p Bytes byte expressions into one word, in this state's byte order.
  SymRef joinBytes(llvm::ArrayRef<SymRef> Bytes) const;

  SymContext *Ctx;
  llvm::endianness Order;

  std::map<uint64_t, SymRef> Registers;
  std::map<uint64_t, SymRef> Temporaries;
  std::map<uint64_t, SymRef> Memory;
  std::shared_ptr<UnknownBytes> RegisterUnknowns;
  std::shared_ptr<UnknownBytes> TemporaryUnknowns;
  std::shared_ptr<UnknownBytes> MemoryUnknowns;
  std::shared_ptr<SymbolicLoadValues> SymbolicLoads;
  /// Keyed by the node index of the value the load produced.
  std::map<uint32_t, LoadOrigin> LoadOrigins;

  bool MemoryClobbered = false;
};

} // namespace neverd::symbolic

#endif // NEVERD_SYMBOLIC_SYMSTATE_H
