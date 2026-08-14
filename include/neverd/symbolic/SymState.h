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
/// Memory is that same store, once per *region* — everything an address
/// reaches from one symbolic base by a constant displacement.  `sp - 8` and
/// `sp - 16` share a base and differ by eight, so a write to one is known not
/// to be a write to the other, and a frame survives its own spills.  Two
/// different bases may be the same pointer and nothing here can say they are
/// not, so a write through one still forgets the others.  The model is exact
/// where separation is provable and blunt where it is not, which is the only
/// arrangement that is both useful and never wrong.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMSTATE_H
#define NEVERD_SYMBOLIC_SYMSTATE_H

#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Endian.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>

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
/// has to sit in a container and be assigned like anything else.  A copy is a
/// handful of maps of small integers, which is cheap enough that nothing more
/// elaborate has earned its place yet.
class SymState {
public:
  SymState(SymContext &Ctx, llvm::endianness Order = llvm::endianness::little)
      : Ctx(&Ctx), Order(Order) {}

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

  /// Read \p Bytes at \p Addr.  An address that is a base plus a constant is
  /// read out of that base's region; anything else is its own region, read as
  /// stable unknowns that later reads of the same address agree with.
  SymRef load(SymRef Addr, uint16_t Bytes);

  /// Store \p Value and return whether its address came out a number.
  ///
  /// A false result does not mean the value was thrown away — it was written
  /// to its region, and reads at other displacements of that same base still
  /// see what they saw.  It means the store may have landed in some *other*
  /// region as well, so those were forgotten, and the caller is looking at an
  /// approximation.
  bool store(SymRef Addr, SymRef Value);

  /// What a load that could not be resolved was reading.
  struct LoadOrigin {
    SymRef Address;
    uint16_t Bytes = 0;

    friend bool operator==(const LoadOrigin &, const LoadOrigin &) = default;
  };

  /// Every address a value is known to have been loaded from.
  ///
  /// The value itself is only a name — there is nothing in the expression
  /// language for "the contents of this address" — so the address would
  /// otherwise be lost at the moment it becomes interesting.  It is exactly
  /// what says a computed branch is a jump table: not that the target is
  /// unknown, but that it is a load from somewhere linear in an index.
  ///
  /// An empty range means no load provenance is known.  More than one entry is
  /// a conflict: expression interning proved the values equal, but cannot say
  /// which load occurrence produced a later use.  The range is sorted and
  /// contains no duplicates, so its meaning is independent of path order.
  llvm::ArrayRef<LoadOrigin> loadOrigins(SymRef Value) const;

  /// The unique load origin of `Value`, or null when it has none or has
  /// conflicting origins.  Consumers that answer a may-dependence question
  /// should inspect `loadOrigins` instead.
  const LoadOrigin *loadOrigin(SymRef Value) const;

  /// True once a store the engine could not resolve to a numeric address has
  /// happened.  Such a store keeps its own region exactly and forgets every
  /// other one, so this does not say that nothing is known any more; it says
  /// that something was conservatively given up and a result reached through
  /// memory is an approximation.
  bool memoryIsUnknown() const { return MemoryClobbered; }

  /// Forget memory outright, every region of it.  What a callee with no
  /// summary may have done.
  void clobberMemory();

  //===--------------------------------------------------------------------===//
  // Whole-state operations
  //===--------------------------------------------------------------------===//

  /// Forget the registers a call is allowed to overwrite, keeping the ones it
  /// is not.  Passing an empty list forgets all of them.
  void clobberRegistersExcept(llvm::ArrayRef<SymRegisterRange> PreservedRanges);

  /// Continue \p Other as part of this state when the two hold the same value
  /// in every location, and report whether they did.
  ///
  /// This is what lets two routes to one block go on as one path instead of
  /// two.  It deliberately merges nothing that differs: a merge of differing
  /// states would have to write a conditional into every byte they disagree
  /// on, which trades the exactness of a per-path answer for a cheaper walk.
  /// Where the states already agree there is nothing to trade.
  bool mergeIdentical(const SymState &Other);

  /// A value nothing determines, of the given width.  Every unknown the engine
  /// invents comes from here, so they are all named and none can collide.
  SymRef freshInput(llvm::StringRef Prefix, uint32_t Width);

  /// Number of bytes with a recorded value.  Diagnostic only.
  size_t numLiveBytes() const;

  /// Memory regions this state has touched.  Diagnostic only: it is the number
  /// of distinct bases the code addressed through, so one for a function that
  /// only ever uses its frame.
  size_t numMemoryRegions() const { return Regions.size(); }

private:
  /// Lazily materialised bytes belonging to one forgetting event.  States
  /// copied after that event share the set, so they agree on what an untouched
  /// location holds; forks that forget independently afterwards do not.
  struct UnknownBytes {
    std::map<uint64_t, SymRef> Values;
  };

  /// One byte-addressed store: the register file, the temporaries, absolute
  /// memory, or one memory region.
  struct Bank {
    std::map<uint64_t, SymRef> Bytes;
    /// Null until this bank has been forgotten once — while an untouched byte
    /// can still be named for where it is rather than for when it was read.
    std::shared_ptr<UnknownBytes> Unknowns;
    /// What an untouched byte of this bank is named after.
    std::string Name;
  };

  /// Where an address points, as a base and a displacement from it.  An
  /// invalid base is the absolute region, which every address the engine
  /// pinned down to a number shares.
  struct Location {
    SymRef Base;
    uint64_t Offset = 0;
  };

  /// Split \p Addr into the region it names and the displacement into it.
  Location locate(SymRef Addr);
  Bank &bankFor(const Location &Where);
  Bank &bank(SymSpace Space);

  /// One byte of a bank, minting a named input for it when it has none.
  SymRef byteAt(Bank &B, uint64_t Offset);
  SymRef readBank(Bank &B, uint64_t Offset, uint16_t Bytes);
  void writeBank(Bank &B, uint64_t Offset, SymRef Value);

  /// Give up everything \p B held, and start an epoch so that what is read
  /// from it next cannot be mistaken for what was read before.
  static void forget(Bank &B);
  /// Forget every region except the one based on \p Except; an invalid ref
  /// spares nothing.
  void forgetRegions(SymRef Except);

  static bool holdsSameBytes(const Bank &A, const Bank &B);

  /// Add one fact to a value's load provenance without making insertion order
  /// observable.
  void recordLoadOrigin(SymRef Value, LoadOrigin Origin);

  /// Join \p Bytes byte expressions into one word, in this state's byte order.
  SymRef joinBytes(llvm::ArrayRef<SymRef> Bytes) const;

  SymContext *Ctx;
  llvm::endianness Order;

  Bank Registers{{}, nullptr, "reg"};
  Bank Temporaries{{}, nullptr, "tmp"};
  Bank AbsoluteMemory{{}, nullptr, "mem"};
  /// One bank per symbolic base, keyed by the node that base interned to.
  std::map<uint32_t, Bank> Regions;
  /// Keyed by the node index of the value the loads produced.
  std::map<uint32_t, llvm::SmallVector<LoadOrigin, 1>> LoadOrigins;

  bool MemoryClobbered = false;
};

} // namespace neverd::symbolic

#endif // NEVERD_SYMBOLIC_SYMSTATE_H
