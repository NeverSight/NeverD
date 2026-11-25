//===- MedByteDemand.h - Bytes a function reads of a value ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Which bytes of each SSA value something a function does depends on.  A
/// value an operation reads is not yet one the function reads: the lift keeps
/// the narrower views of a register in step with it and merges them where
/// paths join, and a write of one byte keeps the rest of its register.  What
/// a function reads of a value is what reaches a store, a call, a branch or
/// another effect, byte by byte.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDBYTEDEMAND_H
#define NEVERD_IR_MED_MEDBYTEDEMAND_H

#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"

#include <cstdint>
#include <tuple>
#include <vector>

namespace neverd {

struct TargetRegInfo;

/// An SSA value's identity: its kind, id and version.
using MedValueKey = std::tuple<MedVar::VarKind, int, int>;

inline MedValueKey medValueKey(const MedVar &Value) {
  return {Value.Kind, Value.Id, Value.SSAVer};
}

/// Hashes a value's key for MedValueMap.
struct MedValueKeyInfo {
  static MedValueKey getEmptyKey() {
    return {static_cast<MedVar::VarKind>(0xFE), -1, -1};
  }
  static MedValueKey getTombstoneKey() {
    return {static_cast<MedVar::VarKind>(0xFF), -1, -1};
  }
  static unsigned getHashValue(const MedValueKey &Key) {
    return static_cast<unsigned>(
        llvm::hash_combine(static_cast<unsigned>(std::get<0>(Key)),
                           std::get<1>(Key), std::get<2>(Key)));
  }
  static bool isEqual(const MedValueKey &A, const MedValueKey &B) {
    return A == B;
  }
};
template <typename T>
using MedValueMap = llvm::DenseMap<MedValueKey, T, MedValueKeyInfo>;

/// The bytes of a value something depends on: bit N for byte N, up to the
/// 64 of the widest vector register.  A wider value is depended on whole.
using MedByteMask = uint64_t;
constexpr unsigned kMedByteMaskBytes = 64;

/// Every byte \p Value has.
MedByteMask medBytesOf(const MedVar &Value);

/// Whether \p Op is run for more than the value it computes, or leaves that
/// value where no operation shows who reads it: a store, a call, a branch,
/// a load, a division, an intrinsic, and a write of anything but a register
/// or a temporary.  Such an operation reads its inputs whole whatever reads
/// its output.
bool medReadsInputsWhole(const MedOp &Op);

/// Whether something no operation of \p Func shows may read what it holds
/// in a register other than the integer return register: a caller, where
/// the function returns a record of registers or a floating-point or
/// vector value, or nothing yet says it returns an integer; and an
/// exception handler, where the function has an exceptional edge.
bool medLeavesOtherRegisters(const MedFunc &Func);

/// Which bytes of each value something the function does depends on.
///
/// An operation run for more than its value reads its inputs whole
/// (medReadsInputsWhole).  One that only computes a value reads what the
/// readers of that value need of it (inputDemand), and a PHI what its
/// readers need: the views of the return register a call leaves (`eax`,
/// `al`) and the PHIs that merge them read nothing where no operation reads
/// them, and the bytes a `setcc` merges a byte over read nothing where only
/// that byte is read.
///
/// No operation shows every reader.  A caller may take what a function
/// leaves in a register other than the integer return register
/// (MedFunc::MultiReturn, a floating-point result), and an exception
/// handler lifted apart from its function reads the registers the function
/// held when it threw.  So a value in such a register that nothing reads is
/// read whole, unless the function is known to return an integer and has no
/// exceptional edge (medLeavesOtherRegisters).  An unread value of the return
/// register or of a temporary reads nothing: what a return hands back is
/// known (returnRegisterAt), and a temporary goes nowhere else.
class MedByteDemand {
public:
  /// \p OtherRegistersRead: whether something no operation shows may read a
  /// register other than the integer return register
  /// (medLeavesOtherRegisters).
  MedByteDemand(const TargetRegInfo &TRI, bool OtherRegistersRead)
      : TRI(TRI), OtherRegistersRead(OtherRegistersRead) {}

  /// Notes the value \p Phi defines and those it merges.
  void define(const PhiNode &Phi);
  /// Notes the value \p Op defines and those it reads; an operation run for
  /// more than its value needs its inputs whole.
  void define(const MedOp &Op);
  /// Notes that an operation, a PHI or a return reads \p Value.
  void use(const MedVar &Value);
  /// Notes that something depends on \p Bytes of \p Value.
  void need(const MedVar &Value, MedByteMask Bytes);
  /// Notes that a return hands \p Value back.  A call's result handed back
  /// as it came, through the PHIs that merge it and the views of it a return
  /// reads, is not read by that (handsBack): whether it is depends on whether
  /// the function returns a value at all.  Any other value is one the
  /// function computes, read whole.
  void handBack(const MedVar &Value);
  /// Reads whole each value nothing reads that something unseen may, then
  /// carries every need back to the values it rests on.
  void settle();
  /// The bytes of \p Value that reach an observation (after settle).
  MedByteMask bytesRead(const MedVar &Value) const;
  /// Whether something depends on a byte of \p Value (after settle).
  bool reads(const MedVar &Value) const;
  /// Whether an operation, a PHI or a return reads \p Value.
  bool used(const MedVar &Value) const;
  /// Whether a return hands \p Value back as it came (after settle).
  bool handsBack(const MedVar &Value) const;
  /// The bytes of \p Value that are zero whatever the function runs on:
  /// those of a constant, those a zero extension adds, and what a copy, a
  /// view, a byte merge, a bitwise operation and a PHI make of them.  The
  /// values \p Value rests on are worked out together, each PHI starting as
  /// zero wherever its arms may be, until no answer changes: a flag ANDed
  /// around a loop stays the byte it started as.  Nothing is known past the
  /// bound.
  MedByteMask zeroBytes(const MedVar &Value);

private:
  struct Node {
    const MedOp *Op = nullptr;
    const PhiNode *Phi = nullptr;
    MedByteMask Bytes = 0;
    /// An operation, a PHI or a return reads it.
    bool Used = false;
    /// A return hands it back as it came.
    bool HandedBack = false;
    bool Pending = false;
  };

  unsigned nodeOf(const MedVar &Value);

  const TargetRegInfo &TRI;
  /// Whether something no operation shows may read a register other than
  /// the integer return register (medLeavesOtherRegisters).
  bool OtherRegistersRead;
  MedValueMap<unsigned> Index;
  std::vector<Node> Nodes;
  std::vector<unsigned> Pending;
  /// The values returns hand back, until settle follows them.
  std::vector<MedVar> Returns;
  /// The zero bytes of the values asked about, by node (zeroBytes).
  llvm::DenseMap<unsigned, MedByteMask> Zero;
};

} // namespace neverd

#endif // NEVERD_IR_MED_MEDBYTEDEMAND_H
