//===- MedVariadic.cpp - Variadic (...) function detection --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Variadic-function prologue detection for the calling-convention recovery
/// framework.  Split out of MedCallingConv.cpp so the architecture-generic
/// parameter-detection framework and the per-ABI variadic recognizers each
/// stay a readable size (mirroring the MedCallingConvX86.cpp split).
///
/// The per-ABI recognizers here cover:
///   - x86-64: the va_start GP/FP-offset word + register save area;
///   - AArch64: the AAPCS64/ELF dual GP+FP save area, plus the Darwin
///     home-slot round-trip and -O2 register-walk overflow shapes;
///   - ARM32: the save area abutting entry SP;
///   - i386: the stack va_list home-and-reload.
///
/// detectVariadic is declared in MedCallingConv.cpp and called from detectCc;
/// see that file for the dispatch order.
///
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedCallingConvDetail.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/SmallVector.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace neverd {

using med_calling_conv_detail::computeForwardValueClosure;
using med_calling_conv_detail::containsValue;

namespace {

// Byte offset of \p V relative to the entry stack pointer. Every SSA/PHI
// definition must be unique and every PHI arm must prove the same offset.
std::optional<int64_t> entrySpDelta(const MedFunc &Func, uint64_t SpOff,
                                    const MedVar &Root, int Depth) {
  using Key = std::tuple<uint8_t, int, int, uint64_t, uint16_t>;
  auto keyOf = [](const MedVar &Value) -> Key {
    return {static_cast<uint8_t>(Value.Kind), Value.Id, Value.SSAVer,
            Value.Kind == MedVar::Reg ? Value.RegOff : 0, Value.Size};
  };
  std::map<Key, const MedOp *> Definitions;
  std::set<Key> AmbiguousDefinitions;
  std::map<Key, const PhiNode *> Phis;
  std::set<Key> AmbiguousPhis;
  for (const auto &Block : Func.Blocks) {
    for (const auto &Op : Block.Ops) {
      if (Op.Output.isConst())
        continue;
      const Key K = keyOf(Op.Output);
      auto [It, Inserted] = Definitions.emplace(K, &Op);
      if (!Inserted && It->second != &Op)
        AmbiguousDefinitions.insert(K);
    }
    for (const auto &Phi : Block.Phis) {
      const Key K = keyOf(Phi.Output);
      auto [It, Inserted] = Phis.emplace(K, &Phi);
      if (!Inserted && It->second != &Phi)
        AmbiguousPhis.insert(K);
    }
  }

  std::set<Key> Active;
  std::function<std::optional<int64_t>(const MedVar &, int)> Eval =
      [&](const MedVar &V, int Remaining) -> std::optional<int64_t> {
    if (Remaining <= 0 || V.isConst()) {
      if (V.isConst() &&
          (V.Provenance == ConstantAddressProvenance::Unknown ||
           V.Provenance == ConstantAddressProvenance::Scalar))
        return static_cast<int64_t>(V.ConstVal);
      return std::nullopt;
    }
    if (V.Kind != MedVar::Reg && V.Kind != MedVar::Temp)
      return std::nullopt;
    if (V.Kind == MedVar::Reg && V.RegOff == SpOff && V.SSAVer == 0)
      return int64_t{0};

    const Key K = keyOf(V);
    if (!Active.insert(K).second || AmbiguousDefinitions.count(K) ||
        AmbiguousPhis.count(K))
      return std::nullopt;
    struct PopActive {
      std::set<Key> &Set;
      Key Value;
      ~PopActive() { Set.erase(Value); }
    } Guard{Active, K};

    if (auto It = Phis.find(K); It != Phis.end()) {
      std::optional<int64_t> Merged;
      for (const auto &[Pred, Arg] : It->second->Args) {
        (void)Pred;
        auto Offset = Eval(Arg, Remaining - 1);
        if (!Offset || (Merged && *Merged != *Offset))
          return std::nullopt;
        Merged = Offset;
      }
      return Merged;
    }
    auto It = Definitions.find(K);
    if (It == Definitions.end())
      return std::nullopt;
    const MedOp &Op = *It->second;
    auto constOf = [](const MedVar &Value) -> std::optional<int64_t> {
      if (!Value.isConst() ||
          (Value.Provenance != ConstantAddressProvenance::Unknown &&
           Value.Provenance != ConstantAddressProvenance::Scalar))
        return std::nullopt;
      return static_cast<int64_t>(Value.ConstVal);
    };
    if (Op.NumInputs < 1)
      return std::nullopt;
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      return Eval(Op.Inputs[0], Remaining - 1);
    case NdOp::SUBBYTES:
      return Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
                     Op.Inputs[1].ConstVal == 0
                 ? Eval(Op.Inputs[0], Remaining - 1)
                 : std::nullopt;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
      if (Op.NumInputs < 2)
        return std::nullopt;
      if (auto C = constOf(Op.Inputs[1]))
        if (auto Base = Eval(Op.Inputs[0], Remaining - 1))
          return Op.Opcode == NdOp::INT_ADD ? *Base + *C : *Base - *C;
      if (Op.Opcode == NdOp::INT_ADD)
        if (auto C = constOf(Op.Inputs[0]))
          if (auto Base = Eval(Op.Inputs[1], Remaining - 1))
            return *Base + *C;
      return std::nullopt;
    default:
      return std::nullopt;
    }
  };
  return Eval(Root, Depth > 0 ? 64 - Depth : 64);
}

// Whether \p V is the x86-64 SysV va_start word ((fp_offset << 32) | gp_offset)
// with each field a legal save-area offset.  Distinctive enough, paired with a
// parameter-register spill, to mark a variadic prologue.
bool isX64VaStartWord(uint64_t V) {
  const uint64_t Gp = V & 0xFFFFFFFFu;
  const uint64_t Fp = V >> 32;
  return (V >> 48) == 0 && Gp <= limits::kX64VaGpOffsetMax &&
         (Gp % limits::kX64VaGpOffsetStep) == 0 &&
         Fp >= limits::kX64VaFpOffsetMin && Fp <= limits::kX64VaFpOffsetMax &&
         ((Fp - limits::kX64VaFpOffsetMin) % limits::kX64VaFpOffsetStep) == 0;
}

// Count distinct live-in (SSA version 0) parameter registers from \p Regs that
// the function spills to the stack — the variadic prologue's register save
// area.
int countParamRegSpills(const MedFunc &Func, llvm::ArrayRef<uint64_t> Regs) {
  std::set<uint64_t> Seen;
  for (const auto &Blk : Func.Blocks)
    for (const auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
          Op.Inputs[1].Kind == MedVar::Reg && Op.Inputs[1].SSAVer == 0)
        for (uint64_t R : Regs)
          if (Op.Inputs[1].RegOff == R) {
            Seen.insert(R);
            break;
          }
  return static_cast<int>(Seen.size());
}

} // anonymous namespace

//===----------------------------------------------------------------------===//
// Variadic (...) function detection
//===----------------------------------------------------------------------===//

// Detect a variadic function and its overflow-area base, setting
// Func.IsVariadic and Func.VariadicOverflowBase.  The register save area
// round-trips through the ordinary register parameters (its spill/reload
// forwards), but the va_arg overflow reads land above the synthetic frame and
// are spilled there by the emitter; the caller-side count is finalized once all
// call sites are known.
void detectVariadic(MedFunc &Func, const TargetRegInfo &TRI, Arch TargetArch,
                    BinaryFormat Fmt) {
  if (Func.Blocks.empty())
    return;
  const uint64_t SpOff = TRI.StackPointer;

  // The minimum number of saved parameter registers that distinguishes a
  // variadic register save area from an ordinary function spilling a few of its
  // own arguments.
  constexpr int kMinSaveAreaRegs = 4;

  bool Marked = false;
  if (TargetArch == Arch::X64) {
    // The va_start GP/FP-offset word identifies a variadic prologue; pairing it
    // with a parameter-register spill keeps a stray constant from qualifying.
    bool HasVaWord = false;
    for (const auto &Blk : Func.Blocks)
      for (const auto &Op : Blk.Ops)
        for (uint8_t I = 0; I < Op.NumInputs; ++I)
          if (Op.Inputs[I].isConst() && isX64VaStartWord(Op.Inputs[I].ConstVal))
            HasVaWord = true;
    Marked = HasVaWord && countParamRegSpills(Func, TRI.IntParamRegs) >= 1;
  } else if (TargetArch == Arch::AArch64) {
    // AArch64 saves both the GP (x0-x7) and FP (q0-q7) argument registers to a
    // contiguous save area; spilling most of both register files at entry is
    // the variadic prologue's signature.  This is the AAPCS64 (Linux/ELF)
    // layout ONLY: Apple/Darwin arm64 passes EVERY variadic argument on the
    // stack and emits NO register save area (its va_start homes a single
    // overflow pointer, detected below).  On Mach-O this save-area test would
    // FALSE-POSITIVE on an ordinary -O0 function that merely spills >=4 GP and
    // >=4 FP *named* parameters to its frame (e.g. a non-variadic
    // f(int,double,int,double,int,double,long,double)); the misclassification
    // skips detectXMMParams and silently drops every FP argument.  Gate the
    // save-area test to non-Mach-O; Darwin variadics use the home-slot test.
    if (Fmt != BinaryFormat::MachO)
      Marked =
          countParamRegSpills(Func, TRI.IntParamRegs) >= kMinSaveAreaRegs &&
          countParamRegSpills(Func, TRI.FPParamRegs) >= kMinSaveAreaRegs;
    // Apple/Darwin arm64 passes EVERY variadic argument on the stack, so a
    // variadic function emits NO register save area -- the AAPCS64 test above
    // never fires.  Its va_start instead materializes a single overflow pointer
    // at entry SP + named-stack bytes and homes it to a frame slot, reloading
    // it for each va_arg / forward.  Detect the home-slot round-trip: an
    // entry-SP pointer at a NON-NEGATIVE, slot-aligned delta stored to a frame
    // slot S and reloaded from S.  Delta 0 is the common printf-style wrapper /
    // `f(a,...)` shape (every named arg register-passed, overflow base 0).  A
    // delta > 0 is a function with MORE than 8 named integer args: args 9.. are
    // stack-passed FIXED args, so the overflow pointer sits above them (base =
    // named-stack bytes); detectCc recovers that named prefix with a bounded
    // detectStackParams and finalizeVariadicCallees sizes the fixed prefix
    // accordingly.  Locals are at NEGATIVE entry-SP deltas, so a non-negative
    // homed-and-reloaded pointer is the va_list overflow pointer, not `&local`
    // (this mirrors the i386 detection below).  Requiring the home-and-reload
    // also distinguishes it from a VLA's saved SP (kept in the frame-pointer
    // register, not a homed slot) and from any incidental SP store.  Gated to
    // Mach-O; ELF AArch64 keeps the save-area test.
    if (!Marked && Fmt == BinaryFormat::MachO) {
      std::set<int64_t> HomeSlots;
      for (const auto &Blk : Func.Blocks)
        for (const auto &Op : Blk.Ops)
          if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
              !Op.Inputs[1].isConst())
            if (auto VD = entrySpDelta(Func, SpOff, Op.Inputs[1], 0))
              if (*VD >= 0 && *VD <= limits::kVariadicOverflowBaseMax &&
                  TRI.PointerSize > 0 && (*VD % TRI.PointerSize) == 0)
                if (auto AD = entrySpDelta(Func, SpOff, Op.Inputs[0], 0))
                  HomeSlots.insert(*AD);
      bool Reloaded = false;
      for (const auto &Blk : Func.Blocks)
        for (const auto &Op : Blk.Ops)
          if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1)
            if (auto AD = entrySpDelta(Func, SpOff, Op.Inputs[0], 0))
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
    if (!Marked && Fmt == BinaryFormat::MachO) {
      auto sameVar = [](const MedVar &A, const MedVar &B) {
        return A.Kind == B.Kind && A.Id == B.Id && A.SSAVer == B.SSAVer;
      };
      // Resolve a value through COPY chains to its underlying definition (the
      // post-indexed load address is often a COPY of the walked register).
      std::function<MedVar(const MedVar &, int)> thruCopy =
          [&](const MedVar &V, int Depth) -> MedVar {
        if (Depth > 32 || V.isConst())
          return V;
        for (const auto &Blk : Func.Blocks)
          for (const auto &Op : Blk.Ops)
            if (Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
                sameVar(Op.Output, V))
              return thruCopy(Op.Inputs[0], Depth + 1);
        return V;
      };
      // entry-SP delta allowing the va_arg alignment `orr base,#c`: entry SP is
      // 16-aligned and the overflow base is slot-aligned, so OR with a small
      // constant acts as +c.  Used only inside this tight walk gate.
      std::function<std::optional<int64_t>(const MedVar &, int)> ovfDelta =
          [&](const MedVar &V, int Depth) -> std::optional<int64_t> {
        if (Depth > 32)
          return std::nullopt;
        if (auto D = entrySpDelta(Func, SpOff, V, 0))
          return D;
        for (const auto &Blk : Func.Blocks)
          for (const auto &Op : Blk.Ops)
            if (Op.Opcode == NdOp::INT_OR && Op.NumInputs >= 2 &&
                Op.Inputs[1].isConst() && sameVar(Op.Output, V))
              if (auto B = ovfDelta(Op.Inputs[0], Depth + 1))
                return *B + static_cast<int64_t>(Op.Inputs[1].ConstVal);
        return std::nullopt;
      };
      // Whether some LOAD's address (through COPYs) is the register RegOff (the
      // walked va_list pointer is reused across post-indexed loads as the same
      // ABI register, re-versioned each advance).
      auto regIsLoadAddr = [&](uint64_t RegOff) {
        for (const auto &Blk : Func.Blocks)
          for (const auto &Op : Blk.Ops)
            if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1) {
              MedVar A = thruCopy(Op.Inputs[0], 0);
              if (A.Kind == MedVar::Reg && A.RegOff == RegOff)
                return true;
            }
        return false;
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
  } else if (TargetArch == Arch::ARM) {
    // ARM32 (softfp) has no FP save area: the GP argument registers are spilled
    // to a save area abutting the entry SP so va_arg walks them then the
    // incoming stack arguments contiguously.  The AAPCS save area is laid out
    // so the LAST parameter register (r3) sits in the slot just below the entry
    // SP (entry_sp - slot), making r0..r3 contiguous with the overflow area at
    // [entry_sp + 0].  Requiring specifically the *last* parameter register
    // there — not just any — distinguishes a real save area from an ordinary
    // -O0 callee that happens to spill its FIRST argument register (r0) into
    // the top frame slot (r0 at entry_sp-slot, r3 at the bottom), which is the
    // reverse order and must NOT be read as variadic (else its incoming stack
    // arguments are dropped; see OptStress312).
    const int LastArgIdx = static_cast<int>(TRI.IntParamRegs.size()) - 1;
    bool AbutsEntry = false;
    for (const auto &Blk : Func.Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
            Op.Inputs[1].Kind == MedVar::Reg && Op.Inputs[1].SSAVer == 0 &&
            LastArgIdx >= 0 &&
            TRI.regToArgIdx(Op.Inputs[1].RegOff) == LastArgIdx)
          if (auto D = entrySpDelta(Func, SpOff, Op.Inputs[0], 0))
            if (*D == -TRI.PointerSize)
              AbutsEntry = true;
    Marked = AbutsEntry && countParamRegSpills(Func, TRI.IntParamRegs) >= 2;
  } else if (TargetArch == Arch::X86) {
    // i386 cdecl passes every argument (named and variadic) on the stack, so a
    // variadic prologue has no register save area.  va_start instead points a
    // va_list at the first unnamed argument -- entry_sp + PointerSize (return
    // address) + named-argument bytes, an entry-SP-positive pointer at least
    // two slots above entry SP -- and spills it to a home slot in the callee
    // frame; va_arg reloads that slot and walks it.  The tell-tale, robust
    // against
    // `&secondParam` (stored as an outgoing arg but never reloaded) and against
    // a plain incoming-stack-parameter read (loaded but never stored as a
    // pointer), is BOTH: an entry-SP-positive pointer (delta >= 2*PointerSize)
    // stored to a frame slot S, AND a reload from that same slot S.
    const int64_t MinPtrDelta = 2 * TRI.PointerSize;
    std::set<int64_t> HomeSlots;
    llvm::SmallVector<MedVar, 2> DirectSeeds;
    for (const auto &Blk : Func.Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2)
          if (auto VD = entrySpDelta(Func, SpOff, Op.Inputs[1], 0))
            if (*VD >= MinPtrDelta && *VD <= limits::kVariadicOverflowBaseMax &&
                TRI.PointerSize > 0 && (*VD % TRI.PointerSize) == 0)
              if (auto AD = entrySpDelta(Func, SpOff, Op.Inputs[0], 0)) {
                HomeSlots.insert(*AD);
                DirectSeeds.push_back(Op.Inputs[1]);
              }
    bool Reloaded = false;
    for (const auto &Blk : Func.Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1)
          if (auto AD = entrySpDelta(Func, SpOff, Op.Inputs[0], 0))
            if (HomeSlots.count(*AD))
              Reloaded = true;

    // At -O2 the va_list pointer can remain live in registers after its
    // mandatory home store, so va_arg walks it directly and never reloads the
    // home slot.  Follow that seed through PHIs, width-preserving views, and
    // constant pointer advances; a load through the resulting value proves the
    // stored entry-SP-positive pointer is an active va_arg walk.
    auto forwardsTransparent = [](const MedOp &Op, unsigned InputIdx) {
      if (InputIdx != 0)
        return false;
      switch (Op.Opcode) {
      case NdOp::COPY:
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
        return Op.NumInputs >= 1;
      case NdOp::SUBBYTES:
        return Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
               Op.Inputs[1].ConstVal == 0;
      default:
        return false;
      }
    };
    auto forwardsConstantAdvance = [](const MedOp &Op, unsigned InputIdx) {
      if (Op.NumInputs < 2)
        return false;
      if (Op.Opcode == NdOp::INT_ADD)
        return (InputIdx == 0 && Op.Inputs[1].isConst()) ||
               (InputIdx == 1 && Op.Inputs[0].isConst());
      return Op.Opcode == NdOp::INT_SUB && InputIdx == 0 &&
             Op.Inputs[1].isConst();
    };

    auto DirectWalkPtrs = computeForwardValueClosure(
        Func, DirectSeeds, [&](const MedOp &Op, unsigned InputIdx) {
          return forwardsTransparent(Op, InputIdx) ||
                 forwardsConstantAdvance(Op, InputIdx);
        });
    llvm::SmallVector<MedVar, 4> AdvancedSeeds;
    for (const MedBlock &Block : Func.Blocks)
      for (const MedOp &Op : Block.Ops)
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          if (forwardsConstantAdvance(Op, I) &&
              containsValue(DirectWalkPtrs, Op.Inputs[I])) {
            AdvancedSeeds.push_back(Op.Output);
            break;
          }
    auto AdvancedWalkPtrs =
        computeForwardValueClosure(Func, AdvancedSeeds, forwardsTransparent);

    bool UsedAsLoad = false;
    for (const auto &Blk : Func.Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1 &&
            containsValue(AdvancedWalkPtrs, Op.Inputs[0]))
          UsedAsLoad = true;
    Marked = !HomeSlots.empty() && (Reloaded || UsedAsLoad);
  }
  if (!Marked)
    return;
  // The overflow area base: the smallest non-negative entry-SP offset stored as
  // a pointer value (the va_list overflow/__stack pointer).  x86-64 places it
  // one slot past the return address (+8); AArch64 at the entry SP (+0).  The
  // register-save-area pointer is entry-SP-negative and does not match.
  std::optional<int64_t> Base;
  for (const auto &Blk : Func.Blocks)
    for (const auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2)
        if (auto D = entrySpDelta(Func, SpOff, Op.Inputs[1], 0))
          if (*D >= 0 && *D <= limits::kVariadicOverflowBaseMax)
            if (!Base || *D < *Base)
              Base = *D;

  Func.IsVariadic = true;
  Func.VariadicOverflowBase =
      Base.value_or(TargetArch == Arch::X64 ? TRI.PointerSize : 0);
}

} // namespace neverd
