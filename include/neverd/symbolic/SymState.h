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
/// not, so the default store forgets the others. A per-write separation
/// contract can preserve declared current bytes. Optional conditional tracking
/// instead updates materialized bytes with exact alias predicates, while
/// unmaterialized memory retains the ordinary conservative unknown epochs.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMSTATE_H
#define NEVERD_SYMBOLIC_SYMSTATE_H

#include "neverd/ir/NdOps.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Endian.h"

#include <array>
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

/// One proven constant byte in register or temporary storage. Memory is
/// deliberately excluded: carrying memory facts requires an alias/effect
/// contract in addition to the scalar lattice used by partial evaluation.
struct SymConstantByte {
  SymSpace Space = SymSpace::Register;
  uint64_t Offset = 0;
  uint8_t Value = 0;
};

struct SymConstantRegionByte {
  uint64_t Offset = 0;
  uint8_t Value = 0;
};

enum class SymStorePreservationStatus : uint8_t {
  Invalid,
  Applied,
  BudgetExceeded,
};

struct SymStorePreservationRange {
  int64_t Begin = 0;
  int64_t End = 0;
};

/// One interval of a current memory region. An empty Base denotes absolute
/// memory; other bases must be nonconstant 64-bit values in this context.
struct SymStorePreservationRegion {
  SymRef Base;
  int64_t Begin = 0;
  int64_t End = 0;
};

/// Caller-established separation for one STORE or one atomic write phase:
/// none of its accessed bytes can equal a byte in the union of the declared
/// root-relative ranges. Addresses use modulo-2^64 arithmetic. This interface
/// does not prove separation or grant accessibility; callers supply a proof
/// or an explicit domain contract for every declared region. Only already
/// materialised bytes of each exact base survive. An empty Base denotes the
/// absolute bank, with the same modular range coordinates. References must
/// belong to the state context; SymRef indices cannot authenticate ownership
/// across contexts. Work counts validation, membership checks, saving and
/// restoring; budget failure applies no store or preservation. Never reuse this
/// descriptor as a global policy.
struct SymStorePreservation {
  SymRef Base;
  int64_t Begin = 0;
  int64_t End = 0;
  uint64_t MaxWork = 0;
  uint64_t WorkUsed = 0;
  SymStorePreservationStatus Status = SymStorePreservationStatus::Invalid;
  /// Borrowed for this single store. Empty retains the original single-range
  /// accounting; every additional range must independently have Begin < End.
  llvm::ArrayRef<SymStorePreservationRange> AdditionalRanges;
  /// Borrowed for this single store. Each region requires an empty or
  /// nonconstant 64-bit Base and Begin < End. No declared Base may be the
  /// STORE's own region. Overlapping and repeated declarations form a union;
  /// each is inspected and charged separately. Empty preserves the old
  /// accounting.
  llvm::ArrayRef<SymStorePreservationRegion> AdditionalRegions;
};

/// Bounded conditional semantics for one STORE or atomic write phase. This
/// policy is independent of SymStorePreservation: it assumes no separation.
/// Current bytes in absolute memory and other 64-bit symbolic regions receive
/// their conditional write effects. Missing bytes retain new unknown epochs.
/// References must belong to the state's context; indices do not authenticate
/// cross-context ownership. No accessibility or initial alias consistency is
/// established by this policy. Work counts a write-byte plan entry, each bank
/// inspected, and WriteBytes+3 units per current cross-region byte. All work
/// and validation precede expression construction and the actual store.
enum class SymConditionalStoreStatus : uint8_t {
  Invalid,
  Applied,
  BudgetExceeded,
};
struct SymConditionalStore {
  uint64_t MaxWork = 0;
  uint64_t WorkUsed = 0;
  SymConditionalStoreStatus Status = SymConditionalStoreStatus::Invalid;
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
  /// sub-register aliasing.  A zero-byte or wrapping request fails closed with
  /// an invalid reference and does not mutate the state.
  SymRef read(SymSpace Space, uint64_t Offset, uint16_t Bytes);
  /// Write a whole-byte value.  Invalid, wrapping, or wider-than-bank ranges
  /// fail closed without mutating the state.
  void write(SymSpace Space, uint64_t Offset, SymRef Value);

  /// Whether this whole-byte value fits a register-bank write. Admission only:
  /// no bytes are materialized and no expressions are constructed.
  bool canWriteRegister(uint64_t Offset, SymRef Value) const;

  //===--------------------------------------------------------------------===//
  // Memory
  //===--------------------------------------------------------------------===//

  /// Bind an x64 FS/GS linear-address base. Base must belong to this context;
  /// SymRef indices cannot authenticate ownership across contexts. Invalid,
  /// out-of-range and non-64-bit references, Default and unknown spaces are
  /// rejected without mutation. No base is supplied implicitly, including zero.
  /// These are machine-state inputs, not separate memory regions or evidence
  /// of accessibility, nonaliasing, canonical addresses or exception behavior.
  bool setX64MemoryAddressSpaceBase(NdMemoryAddressSpace Space, SymRef Base);
  SymRef x64MemoryAddressSpaceBase(NdMemoryAddressSpace Space) const;
  /// Calls and unmodelled operations may change segment state. A caller must
  /// authenticate new bases before continuing exact segmented accesses.
  void clearX64MemoryAddressSpaceBases();

  /// Read \p Bytes at \p Addr.  An address that is a base plus a constant is
  /// read out of that base's region; anything else is its own region, read as
  /// stable unknowns that later reads of the same address agree with. A 64-bit
  /// symbolic region uses modular relative coordinates, including across zero.
  /// This does not establish validity of the actual address. Invalid,
  /// zero-byte, and overflowing absolute requests return an invalid reference.
  SymRef load(SymRef Addr, uint16_t Bytes);

  /// Store \p Value and return whether an exact absolute write was recorded.
  ///
  /// For a valid symbolic-base write, false means the value was recorded in
  /// that region, using modular coordinates for a 64-bit base, but may also
  /// alias others, which are forgotten. Invalid or unrepresentable writes
  /// also return false and conservatively clobber
  /// memory instead of publishing a partial update.
  bool store(SymRef Addr, SymRef Value);

  /// Apply one valid 64-bit-address STORE under an explicit preservation
  /// contract. As with store(), false can denote a successful symbolic write;
  /// inspect Preservation.Status for success. Invalid contracts and budget
  /// exhaustion leave state unchanged. Stores to any protected base itself
  /// are rejected; they must use normal byte-overlap semantics instead.
  bool store(SymRef Addr, SymRef Value, SymStorePreservation &Preservation);

  /// Apply one 64-bit-address store, retaining conditional effects for already
  /// materialized bytes in other absolute or 64-bit symbolic banks. The actual
  /// bank uses ordinary byte-overlap semantics. Invalid input, unsupported
  /// materialized banks and budget refusal leave state unchanged; inspect
  /// Control.Status because false can also denote a successful symbolic store.
  /// Symbolic offsets wrap modulo 2^64; absolute writes must not overflow.
  bool storeConditional(SymRef Addr, SymRef Value,
                        SymConditionalStore &Control);

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

  struct LoadOriginBinding {
    SymRef Value;
    LoadOrigin Origin;
  };
  /// Bind historical observations after a caller-proved value rebind. All
  /// references belong to this context; each origin describes the complete
  /// value. This never creates available memory facts or revives old bytes.
  /// Conflicting origins remain a sorted union. Invalid values/addresses or
  /// width mismatches reject the entire batch before publishing any history.
  bool bindLoadOrigins(llvm::ArrayRef<LoadOriginBinding> Bindings);

  struct MemoryInputOrigin {
    SymRef Base;
    uint64_t Offset = 0;
    uint16_t Bytes = 0;

    friend bool operator==(const MemoryInputOrigin &,
                           const MemoryInputOrigin &) = default;
  };

  /// Where an untouched symbolic-region input was first materialised. Unlike
  /// loadOrigins(), this describes input creation, not later loads of equal
  /// or forwarded values. Post-clobber unknowns have no entry-memory origin.
  /// Composite reads retain these origins on their input leaves. This is
  /// dependency metadata, not a current-memory or accessibility certificate.
  llvm::ArrayRef<MemoryInputOrigin> memoryInputOrigins(SymRef Value) const;

  /// True once a store the engine could not resolve to a numeric address has
  /// happened. The target region remains exact; optional write policies may
  /// also retain current bytes elsewhere. Missing memory is still conservative,
  /// so this records lost precision rather than claiming nothing is known.
  bool memoryIsUnknown() const { return MemoryClobbered; }

  /// Forget memory outright, every region of it.  What a callee with no
  /// summary may have done.
  void clobberMemory();

  /// Available values, not a history of accesses. An invalid Base denotes
  /// absolute memory; otherwise Offset is relative to its canonical region.
  /// Keeping the displacement separate preserves aliases such as p and p+8.
  struct MemoryFact {
    SymRef Base;
    uint64_t Offset = 0;
    SymRef Value;
  };
  struct MemoryKnowledge {
    const SymContext *Owner = nullptr;
    llvm::endianness Order = llvm::endianness::little;
    std::vector<MemoryFact> Facts;
  };
  struct MaterializedRange {
    uint64_t Offset = 0;
    uint64_t Bytes = 0;
  };
  /// Current materialized extents for this exact symbolic region base, in
  /// unsigned offset order. Gaps and modular wrap separate ranges. This reads
  /// no memory and constructs no expressions or alias facts. Absent or invalid
  /// bases return an empty projection; references must belong to this context.
  /// Charge one lookup and one unit per materialized byte before allocating.
  /// Insufficient work returns nullopt; the state and context are unchanged.
  std::optional<llvm::SmallVector<MaterializedRange, 8>>
  materializedRegionRanges(SymRef Base, uint64_t &WorkBudget) const;

  /// Whether every byte is already materialized in this relative region.
  /// Does not read, create inputs, or promise constant values. Offsets use
  /// the same modular coordinates as the region's load/store operations.
  bool hasMaterializedRegionRange(SymRef Base, uint64_t Offset,
                                  uint16_t Bytes) const;

  /// Current absolute-memory extents, with the same read-only projection and
  /// work accounting as materializedRegionRanges(). No initial bytes appear.
  std::optional<llvm::SmallVector<MaterializedRange, 8>>
  materializedAbsoluteRanges(uint64_t &WorkBudget) const;

  /// Export only materialized bytes, in spans of at most eight bytes. This
  /// does not read unknown bytes or recover facts from old load provenance.
  MemoryKnowledge memoryKnowledge() const;
  /// Replace memory knowledge after a caller-proved symbolic rebind. This is
  /// not a program store: supplied regions coexist; future stores still apply
  /// normal alias invalidation. Other bytes belong to a fresh unknown epoch.
  /// Context/order/span errors and conflicting exact overlaps return false
  /// without changing this state. Registers and temporaries are preserved.
  /// All references must belong to Owner. Relative 64-bit region coordinates
  /// wrap as normal loads/stores do; absolute addresses must not overflow.
  bool restoreMemoryKnowledge(const MemoryKnowledge &Knowledge);

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

  /// Read-only scalar projection. Does not materialise unknown bytes and does
  /// not expose symbolic identities, epochs, or memory-region assumptions.
  llvm::SmallVector<SymConstantByte, 32> constantScalarBytes() const;

  /// Constant bytes currently retained for this exact symbolic region base.
  /// All guest writes have already applied overlap and alias effects. This
  /// read-only projection does not claim that the region is private or create
  /// missing bytes, and returns nothing for an absent/invalid base.
  llvm::SmallVector<SymConstantRegionByte, 32>
  constantRegionBytes(SymRef Base) const;

  /// Constant bytes currently retained in absolute memory, after all writes
  /// and alias invalidations. This pure projection neither reads missing bytes
  /// nor gives mutable memory an immutable-input contract.
  llvm::SmallVector<SymConstantRegionByte, 32> constantAbsoluteBytes() const;

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
  /// One exact byte view. Scalar writes retain the source word until a read
  /// needs a slice, avoiding dead extracts for overwritten whole words.
  struct ByteView {
    SymRef Value;
    uint32_t Low = 0;

    friend bool operator==(const ByteView &, const ByteView &) = default;
  };

  /// Lazily materialised bytes belonging to one forgetting event.  States
  /// copied after that event share the set, so they agree on what an untouched
  /// location holds; forks that forget independently afterwards do not.
  struct UnknownBytes {
    std::map<uint64_t, ByteView> Values;
  };

  /// Defaults for regions first touched after one memory effect.  Copies share
  /// these byte identities even if neither had materialised the region yet.
  struct UnknownRegions {
    std::map<uint32_t, std::shared_ptr<UnknownBytes>> Values;
  };

  /// One byte-addressed store: the register file, the temporaries, absolute
  /// memory, or one memory region.
  struct Bank {
    std::map<uint64_t, ByteView> Bytes;
    /// Null until this bank has been forgotten once — while an untouched byte
    /// can still be named for where it is rather than for when it was read.
    std::shared_ptr<UnknownBytes> Unknowns;
    /// What an untouched byte of this bank is named after.
    std::string Name;
    /// Machine kind for banks whose untouched epoch-zero values have structured
    /// input origins.  Symbolic pointer regions deliberately have no kind.
    std::optional<SymInputKind> InputKind;
    uint64_t Epoch = 0;
    /// Exact symbolic region identity; invalid for scalar/absolute banks.
    SymRef RegionBase;
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
  ByteView byteAt(Bank &B, uint64_t Offset);
  SymRef readBank(Bank &B, uint64_t Offset, uint16_t Bytes);
  bool writeBank(Bank &B, uint64_t Offset, SymRef Value);
  std::optional<llvm::SmallVector<MaterializedRange, 8>>
  materializedBankRanges(const Bank &B, uint64_t &WorkBudget) const;

  /// Give up everything \p B held, and start an epoch so that what is read
  /// from it next cannot be mistaken for what was read before.
  static void forget(Bank &B);
  /// Forget every region except the one based on \p Except; an invalid ref
  /// spares nothing.
  void forgetRegions(SymRef Except);

  bool holdsSameBytes(const Bank &A, const Bank &B) const;

  /// Add one fact to a value's load provenance without making insertion order
  /// observable.
  void recordLoadOrigin(SymRef Value, LoadOrigin Origin);
  void recordMemoryInputOrigin(SymRef Value, MemoryInputOrigin Origin);

  /// Join \p Bytes byte expressions into one word, in this state's byte order.
  SymRef joinBytes(llvm::ArrayRef<SymRef> Bytes) const;

  SymContext *Ctx;
  llvm::endianness Order;
  std::array<SymRef, 2> X64MemoryAddressSpaceBases;

  Bank Registers{{}, nullptr, "reg", SymInputKind::Register, 0, {}};
  Bank Temporaries{{}, nullptr, "tmp", SymInputKind::Temporary, 0, {}};
  Bank AbsoluteMemory{{}, nullptr, "mem", SymInputKind::AbsoluteMemory, 0, {}};
  /// One bank per symbolic base, keyed by the node that base interned to.
  std::map<uint32_t, Bank> Regions;
  /// Null for entry memory; replaced by every potentially aliasing write.
  std::shared_ptr<UnknownRegions> UnseenRegions;
  /// Keyed by the node index of the value the loads produced.
  std::map<uint32_t, llvm::SmallVector<LoadOrigin, 1>> LoadOrigins;
  std::map<uint32_t, llvm::SmallVector<MemoryInputOrigin, 1>>
      MemoryInputOrigins;

  bool MemoryClobbered = false;
};

} // namespace neverd::symbolic

#endif // NEVERD_SYMBOLIC_SYMSTATE_H
