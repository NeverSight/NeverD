//===- MedVariadicDarwin.cpp - Apple arm64 variadic prologue --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The Apple arm64 variadic prologue recognizer.  Darwin passes every
/// variadic argument on the stack, so its va_list is a single pointer into
/// the incoming stack arguments: homed to a frame slot at -O0, walked in a
/// register at -O2.
///
//===----------------------------------------------------------------------===//

#include "../MedVariadicDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/Limits.h"

#include <functional>

namespace neverd {
namespace med_variadic_detail {

/// A stack va_list homed and reloaded, or walked in place.
bool hasDarwinVariadicPrologue(VariadicScan &S) {
  const MedFunc &Func = S.Func;
  const TargetRegInfo &TRI = S.TRI;
  auto getValueIndex = [&S]() -> const VariadicValueIndex & {
    return S.values();
  };
  bool Marked = false;
  // Apple/Darwin arm64 passes EVERY variadic argument on the stack, so a
  // variadic function emits NO register save area -- the AAPCS64 save-area
  // test (MedVariadicAArch64.cpp) would never fire.  Its va_start instead
  // materializes a single overflow pointer at entry SP + named-stack bytes and
  // homes it to a frame slot, reloading it for each va_arg / forward.  Detect
  // the home-slot round-trip: an entry-SP pointer at a NON-NEGATIVE,
  // slot-aligned delta stored to a frame slot S and reloaded from S.  Delta 0
  // is the common printf-style wrapper / `f(a,...)` shape (every named arg
  // register-passed, overflow base 0).  A delta > 0 is a function with MORE
  // than 8 named integer args: args 9.. are stack-passed FIXED args, so the
  // overflow pointer sits above them (base = named-stack bytes); detectCc
  // recovers that named prefix with a bounded detectStackParams and
  // finalizeVariadicCallees sizes the fixed prefix accordingly.  Locals are at
  // NEGATIVE entry-SP deltas, so a non-negative homed-and-reloaded pointer is
  // the va_list overflow pointer, not `&local` (this mirrors the i386 detection
  // in MedVariadicX86.cpp).  Requiring the home-and-reload also distinguishes
  // it from a VLA's saved SP (kept in the frame-pointer register, not a homed
  // slot) and from any incidental SP store.
  {
    std::set<int64_t> HomeSlots;
    for (const auto &Blk : Func.Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
            Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
            !Op.Inputs[1].isConst())
          if (auto VD = entrySpDelta(S, Op.Inputs[1], 0))
            if (*VD >= 0 && *VD <= limits::kVariadicOverflowBaseMax &&
                TRI.PointerSize > 0 && (*VD % TRI.PointerSize) == 0)
              if (auto AD = entrySpDelta(S, Op.Inputs[0], 0))
                HomeSlots.insert(*AD);
    bool Reloaded = false;
    if (!HomeSlots.empty())
      for (const auto &Blk : Func.Blocks)
        for (const auto &Op : Blk.Ops)
          if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1 &&
              Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
            if (auto AD = entrySpDelta(S, Op.Inputs[0], 0))
              if (HomeSlots.count(*AD))
                Reloaded = true;
    Marked = !HomeSlots.empty() && Reloaded;
  }
  // -O2 direct-overflow walk (no home slot): at -O2 clang keeps the va_list
  // overflow pointer in a register and *walks* it in place
  // (`add x8,sp,#k; orr x8,x8,#8; ldr d,[x8],#8` repeated/unrolled) rather
  // than homing it to a frame slot, so the home-slot round-trip above never
  // fires. Detect the post-increment walk directly: a pointer P used as a
  // LOAD address that is advanced by a constant stride (`P' = P + c`) whose
  // result P' is ALSO used as a LOAD address (a genuine load/advance/load
  // walk, not a one-off offset), with the walk base resolving to an entry-SP
  // NON-NEGATIVE pointer (the variadic overflow area sits at/above entry SP;
  // locals are negative). The load+advance+load chain over entry-SP-positive
  // memory is specific to a va_arg overflow walk -- ordinary stack parameters
  // are read at fixed
  // `[sp+k]` offsets, and >16-byte by-value aggregates are passed by
  // reference (a register pointer), not walked on the stack -- so this does
  // not false-positive on a non-variadic callee (which would silently drop
  // args).
  if (!Marked) {
    // Resolve a value through COPY chains to its underlying definition (the
    // post-indexed load address is often a COPY of the walked register).
    std::function<MedVar(const MedVar &, int)> thruCopy =
        [&](const MedVar &V, int Depth) -> MedVar {
      if (Depth >
              ::neverd::limits::ir::
                  kMedVariadicDarwinHasDarwinVariadicPrologueThruCopyDepthLimit ||
          V.isConst())
        return V;
      const auto &Copies = getValueIndex().FirstCopies;
      auto It = Copies.find(VariadicValueIndex::walkKeyOf(V));
      if (It != Copies.end())
        return thruCopy(It->second->Inputs[0], Depth + 1);
      return V;
    };
    // entry-SP delta allowing the va_arg alignment `orr base,#c`: entry SP is
    // 16-aligned and the overflow base is slot-aligned, so OR with a small
    // constant acts as +c.  Used only inside this tight walk gate.
    std::function<std::optional<int64_t>(const MedVar &, int)> ovfDelta =
        [&](const MedVar &V, int Depth) -> std::optional<int64_t> {
      if (Depth >
          ::neverd::limits::ir::
              kMedVariadicDarwinHasDarwinVariadicPrologueOvfDeltaDepthLimit)
        return std::nullopt;
      if (auto D = entrySpDelta(S, V, 0))
        return D;
      const auto &Ors = getValueIndex().OrDefinitions;
      auto It = Ors.find(VariadicValueIndex::walkKeyOf(V));
      if (It != Ors.end())
        for (const MedOp *Op : It->second)
          if (auto B = ovfDelta(Op->Inputs[0], Depth + 1))
            return *B + static_cast<int64_t>(Op->Inputs[1].ConstVal);
      return std::nullopt;
    };
    // Whether some LOAD's address (through COPYs) is the register RegOff (the
    // walked va_list pointer is reused across post-indexed loads as the same
    // ABI register, re-versioned each advance).
    std::optional<std::set<uint64_t>> LoadAddressRegs;
    auto regIsLoadAddr = [&](uint64_t RegOff) {
      if (!LoadAddressRegs) {
        LoadAddressRegs.emplace();
        for (const auto &Blk : Func.Blocks)
          for (const auto &Op : Blk.Ops)
            if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1 &&
                Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
              MedVar A = thruCopy(Op.Inputs[0], 0);
              if (A.Kind == MedVar::Reg)
                LoadAddressRegs->insert(A.RegOff);
            }
      }
      return LoadAddressRegs->count(RegOff) != 0;
    };
    // Find a register walk: `R.(v+1) = R.v + const` (same ABI register
    // advancing) that is used as a LOAD address and whose value is an
    // entry-SP NON-NEGATIVE pointer (the variadic overflow area; locals are
    // negative).
    for (const auto &Blk : Func.Blocks) {
      if (Marked)
        break;
      for (const auto &Op : Blk.Ops) {
        if (Op.Opcode != NdOp::INT_ADD || Op.NumInputs < 2 ||
            !Op.Inputs[1].isConst())
          continue;
        const MedVar &In = Op.Inputs[0];
        if (Op.Output.Kind != MedVar::Reg || In.Kind != MedVar::Reg ||
            Op.Output.RegOff != In.RegOff)
          continue; // not the same ABI register advancing
        if (!regIsLoadAddr(In.RegOff))
          continue;
        if (auto D = ovfDelta(In, 0))
          if (*D >= 0 && *D <= limits::kVariadicOverflowBaseMax) {
            Marked = true;
            break;
          }
      }
    }
  }
  return Marked;
}

} // namespace med_variadic_detail
} // namespace neverd
