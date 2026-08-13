//===- MedTypePass.cpp - Type inference pass for MedIR -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// MedIR type inference pass implementation. Infers return types,
/// parameter types, and local variable types from data-flow patterns
/// using architecture-agnostic queries via TargetRegInfo.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedTypePass.h"

#include "neverd/ir/TargetRegInfo.h"

#include <map>
#include <set>
#include <string>
#include <tuple>

namespace neverd {

//===----------------------------------------------------------------------===//
// Return type inference
//===----------------------------------------------------------------------===//

// Whether \p Opc is a scalar floating-point producing operation.
static bool isFloatProducer(NdOp Opc) {
  return Opc == NdOp::FLOAT_ADD || Opc == NdOp::FLOAT_SUB ||
         Opc == NdOp::FLOAT_MULT || Opc == NdOp::FLOAT_DIV ||
         Opc == NdOp::FLOAT_FMA || Opc == NdOp::FLOAT_NEG ||
         Opc == NdOp::FLOAT_ABS || Opc == NdOp::FLOAT_SQRT ||
         Opc == NdOp::FLOAT_CEIL || Opc == NdOp::FLOAT_FLOOR ||
         Opc == NdOp::FLOAT_ROUND || Opc == NdOp::FLOAT_MIN ||
         Opc == NdOp::FLOAT_MAX || Opc == NdOp::FLOAT_MINNUM ||
         Opc == NdOp::FLOAT_MAXNUM || Opc == NdOp::FLOAT_FLOAT2FLOAT ||
         Opc == NdOp::FLOAT_INT2FLOAT;
}

// The byte size of the scalar floating-point value a write to the FP return
// register carries, or 0 if the value is not floating-point.  The lifter
// materializes an XMM write as `CONCAT(preserved-high, fp-low)` (the scalar
// result occupies the low lane, input 1), so the FP signal is reached through
// the CONCAT / COPY / SUBBYTES chain, not just a bare FLOAT_* opcode.
static uint16_t
fpReturnElemSize(const std::map<std::pair<int, int>, const MedOp *> &Defs,
                 const MedVar &V, int Depth) {
  if (Depth > 16 || V.Kind == MedVar::Const)
    return 0;
  auto It = Defs.find({V.Id, V.SSAVer});
  if (It == Defs.end())
    return 0;
  const MedOp *Def = It->second;
  if (isFloatProducer(Def->Opcode))
    return Def->Output.Size <= 4 ? 4 : 8;
  if (Def->Opcode == NdOp::CONCAT && Def->NumInputs >= 2)
    return fpReturnElemSize(Defs, Def->Inputs[1], Depth + 1); // low lane
  if ((Def->Opcode == NdOp::COPY || Def->Opcode == NdOp::SUBBYTES) &&
      Def->NumInputs >= 1)
    return fpReturnElemSize(Defs, Def->Inputs[0], Depth + 1);
  // A floating-point shuffle/pack intrinsic assembling the FP return register
  // from scalar FP values — e.g. x86 `unpcklps` packing two floats into XMM0
  // for a `struct{float,float}` returned in one SSE register.  It is FP iff a
  // source is FP; the packed result is carried in the 16-byte XMM/V return
  // type, so the exact element size only signals "is floating point" here.
  if (Def->Opcode == NdOp::INTRINSIC) {
    uint16_t Best = 0;
    for (uint8_t I = 0; I < Def->NumInputs; ++I) {
      uint16_t E = fpReturnElemSize(Defs, Def->Inputs[I], Depth + 1);
      if (E > Best)
        Best = E;
    }
    return Best;
  }
  return 0;
}

// A scalar floating-point value loaded straight into the FP return register
// with no FLOAT_* producer — a `switch` returning floating-point constants
// lowers to a rodata table the callee loads into XMM0/D0/S0 and returns (`COPY
// r,(zext)load`). Distinguished from NEON integer SIMD (which assembles the FP
// register's lanes through CONCAT): tracing stops at CONCAT so a vector
// lane-assembly chain (e.g. a vectorized byte kernel returning an int) is NOT
// mistaken for a scalar FP return.  Returns the scalar width (4 float / 8
// double) or 0.
static uint16_t
scalarFPLoadElemSize(const std::map<std::pair<int, int>, const MedOp *> &Defs,
                     const MedVar &V, int Depth) {
  if (Depth > 16 || V.Kind == MedVar::Const)
    return 0;
  auto It = Defs.find({V.Id, V.SSAVer});
  if (It == Defs.end())
    return 0;
  const MedOp *Def = It->second;
  if (Def->Opcode == NdOp::LOAD)
    return Def->Output.Size == 4 ? 4 : (Def->Output.Size == 8 ? 8 : 0);
  // A bare sub-register view (offset 0) / widen / copy of a scalar load; a
  // non-zero SUBBYTES offset or a CONCAT is vector-lane assembly — stop.
  bool ChainOp = Def->Opcode == NdOp::COPY || Def->Opcode == NdOp::INT_ZEXT ||
                 Def->Opcode == NdOp::INT_SEXT ||
                 (Def->Opcode == NdOp::SUBBYTES && Def->NumInputs >= 2 &&
                  Def->Inputs[1].isConst() && Def->Inputs[1].ConstVal == 0);
  if (ChainOp && Def->NumInputs >= 1)
    return scalarFPLoadElemSize(Defs, Def->Inputs[0], Depth + 1);
  return 0;
}

// The FP return register is written but neither an FP producer nor an FP load
// supplies its value -- a floating-point *constant* materialized as integer
// bits and moved into the FP register (a leaf `return 3.5;` at -O2 lowers to
// `movz xN,#imm; fmov d0,xN; ret`, lifted as `COPY D0 = const` plus the d->q
// widening `INT_ZEXT Q0 = const`).  Such a value carries no type provenance, so
// it is only treated as a scalar FP return once the caller has established that
// NO integer return register is written (the result is left solely in the FP
// register).  Returns the scalar element width (4 float / 8 double) for a
// scalar-shaped move into the FP register, or 0 for a vector-lane assembly
// (CONCAT) / wide write that the dedicated single-vector-return path must own.
static uint16_t fpReturnFallbackElem(const MedOp &Op) {
  // `fmov s0,wN` lifts to `COPY S0:4 = <int>`; a scalar d/s value widened to
  // the full q register models the same write as `INT_ZEXT Q0:16 = <scalar>`
  // (its input is the low-lane scalar).  Either way the element is the low-lane
  // width.
  if (Op.Opcode == NdOp::INT_ZEXT && Op.NumInputs >= 1 &&
      Op.Inputs[0].Size > 0 && Op.Inputs[0].Size <= 8)
    return Op.Inputs[0].Size <= 4 ? 4 : 8;
  if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::SUBBYTES) &&
      Op.Output.Size > 0 && Op.Output.Size <= 8)
    return Op.Output.Size <= 4 ? 4 : 8;
  return 0;
}

// True when \p Op writes the integer return register by restoring a value
// loaded from the stack pointer — an epilogue `pop` (e.g. restoring a PIC-base
// scratch register), not a computed return value.  Such a write must not mask a
// genuine floating-point return left in the FP return register (clang's i386
// internal convention returns a double in XMM0; a rodata-using callee restores
// its PIC-base EAX with `pop eax` right before `ret`).
static bool
isReturnRegRestore(const std::map<std::pair<int, int>, const MedOp *> &Defs,
                   const MedOp &Op, const TargetRegInfo &TRI) {
  auto isStackPtr = [&](MedVar A) {
    for (int D = 0; D < 8; ++D) {
      if (A.Kind == MedVar::Reg && A.RegOff == TRI.StackPointer)
        return true;
      auto It = Defs.find({A.Id, A.SSAVer});
      if (It == Defs.end())
        return false;
      const MedOp *D2 = It->second;
      if ((D2->Opcode == NdOp::COPY || D2->Opcode == NdOp::INT_ZEXT ||
           D2->Opcode == NdOp::INT_SEXT || D2->Opcode == NdOp::SUBBYTES) &&
          D2->NumInputs >= 1)
        A = D2->Inputs[0];
      else
        return false;
    }
    return false;
  };
  const MedOp *Cur = &Op;
  for (int Depth = 0; Depth < 8 && Cur; ++Depth) {
    if (Cur->Opcode == NdOp::LOAD)
      return Cur->NumInputs >= 1 && isStackPtr(Cur->Inputs[0]);
    if ((Cur->Opcode == NdOp::COPY || Cur->Opcode == NdOp::INT_ZEXT ||
         Cur->Opcode == NdOp::INT_SEXT || Cur->Opcode == NdOp::SUBBYTES) &&
        Cur->NumInputs >= 1) {
      auto It = Defs.find({Cur->Inputs[0].Id, Cur->Inputs[0].SSAVer});
      Cur = It != Defs.end() ? It->second : nullptr;
    } else {
      return false;
    }
  }
  return false;
}

// Effective width of a value reaching the integer return register.  A
// zero-extension into the return register types the result by its pre-extension
// width (a function returning `int` zero-extends EAX into RAX); a
// return-register PHI (a merged epilogue) by the widest of its arguments.  Used
// so an i64 result returned through a PHI is not mistyped narrow by a sibling
// `return 0` path's zero-extend (bug class #157c, int-return analogue of the
// FP-PHI handling).
//
// The result for a given value is a pure function of (V, Depth) — Defs/PhiDefs
// are const — so the \p Memo map caches each (Id,SSAVer,Depth) result.  Without
// it a merged epilogue whose return PHI fans into further PHIs re-expands each
// shared sub-PHI along every incoming path, which is exponential (the dominant
// serial cost of return-type inference on large binaries).  The memo makes the
// walk visit each node at most once per depth, and preserves the exact
// depth-cap semantics of the original (each node is keyed by its depth, so the
// cap still truncates every path at depth 8 identically).
static uint16_t
intRetEffWidthRec(const std::map<std::pair<int, int>, const MedOp *> &Defs,
                  const std::map<std::pair<int, int>, const PhiNode *> &PhiDefs,
                  const MedVar &V, int Depth,
                  std::map<std::tuple<int, int, int>, uint16_t> &Memo) {
  if (Depth > 8 || V.isConst())
    return V.Size;
  auto OpIt = Defs.find({V.Id, V.SSAVer});
  if (OpIt != Defs.end()) {
    const MedOp *D = OpIt->second;
    if (D->Opcode == NdOp::INT_ZEXT && D->NumInputs >= 1)
      return D->Inputs[0].Size;
    return D->Output.Size;
  }
  auto PhiIt = PhiDefs.find({V.Id, V.SSAVer});
  if (PhiIt != PhiDefs.end()) {
    std::tuple<int, int, int> Key{V.Id, V.SSAVer, Depth};
    if (auto M = Memo.find(Key); M != Memo.end())
      return M->second;
    uint16_t W = 0;
    for (const auto &A : PhiIt->second->Args) {
      uint16_t AW = intRetEffWidthRec(Defs, PhiDefs, A.second, Depth + 1, Memo);
      if (AW > W)
        W = AW;
    }
    uint16_t Result = W ? W : V.Size;
    Memo[Key] = Result;
    return Result;
  }
  return V.Size;
}

static uint16_t
intRetEffWidth(const std::map<std::pair<int, int>, const MedOp *> &Defs,
               const std::map<std::pair<int, int>, const PhiNode *> &PhiDefs,
               const MedVar &V, int Depth) {
  std::map<std::tuple<int, int, int>, uint16_t> Memo;
  return intRetEffWidthRec(Defs, PhiDefs, V, Depth, Memo);
}

static TypeRef inferReturnType(const MedFunc &Func, const TargetRegInfo &TRI,
                               Arch TheArch) {
  uint16_t DefaultSize = TRI.PointerSize > 0 ? TRI.PointerSize : 4;

  // The epilogue register-restore filter below applies to x86/x86-64: a `pop`
  // into the integer return register right before `ret` is a stack-cleanup /
  // PIC-base restore (i386 `pop eax` for the GOT base, x86-64 `pop rax` undoing
  // a `push rax` stack-alignment for a call), not a computed return value — it
  // must not mask a genuine floating-point result left in XMM0 (clang's
  // internal convention returns a double there).  The filter matches only a
  // LOAD directly from the stack pointer (a `pop`); a genuine frame-slot int
  // return
  // (`mov rax,[rsp+k]`, address `rsp+k`) is not matched.  AArch64 returns FP
  // bits through the integer return register via a genuine `[sp]` reload
  // (soft-float external returns), which must NOT be filtered, so it stays off
  // there.
  const bool FilterRestore = (TheArch == Arch::X86 || TheArch == Arch::X64);

  std::map<std::pair<int, int>, const MedOp *> Defs;
  for (const auto &Blk : Func.Blocks)
    for (const auto &Op : Blk.Ops)
      if (Op.Output.Size > 0)
        Defs[{Op.Output.Id, Op.Output.SSAVer}] = &Op;

  std::map<std::pair<int, int>, const PhiNode *> PhiDefs;
  for (const auto &Blk : Func.Blocks)
    for (const auto &Phi : Blk.Phis)
      if (Phi.Output.Size > 0)
        PhiDefs[{Phi.Output.Id, Phi.Output.SSAVer}] = &Phi;

  // Aggregate across *every* RETURN, not just the first: a function with an
  // early `return arg` (no return-register write — the FP argument is passed
  // straight through in XMM0/D0) and a separate computed `return expr` path
  // must be typed by the path that actually computes the result.  Any return
  // path that genuinely leaves a floating-point value in the FP return register
  // (fpReturnElemSize sees a real FLOAT producer, not a live-in self-copy)
  // wins.
  bool AnyFloat = false;
  uint16_t BestFloatElem = 0;
  const MedOp *BestInt = nullptr;
  uint16_t BestIntPhiWidth = 0;

  for (const auto &Blk : Func.Blocks) {
    for (auto Rit = Blk.Ops.rbegin(); Rit != Blk.Ops.rend(); ++Rit) {
      if (Rit->Opcode != NdOp::RETURN)
        continue;

      // Pick the *widest* non-SUBBYTES write to the integer return register,
      // not the first one in reverse order.  A trailing narrow sub-register
      // COPY (e.g. `COPY W0` emitted right after `COPY X0` for `mov x0,x8`)
      // must not shrink the inferred return type (bugs #156/#157d).  The
      // int-vs-float category is still decided by whichever return register
      // was written closest to the RETURN.
      const MedOp *WidestInt = nullptr;
      int IntDist = -1;
      const MedOp *FirstFloat = nullptr;
      uint16_t FloatElem = 0;
      int FloatDist = -1;
      bool FloatViaPhi = false;
      const MedOp *FPRegWriteOp =
          nullptr; // closest FP-return-reg write (any op)

      int Dist = 0;
      for (auto Rit2 = Rit + 1; Rit2 != Blk.Ops.rend(); ++Rit2, ++Dist) {
        if (Rit2->Output.Kind != MedVar::Reg || Rit2->Output.Size == 0)
          continue;

        if (Rit2->Output.RegOff == TRI.IntReturnReg &&
            Rit2->Opcode != NdOp::SUBBYTES &&
            !(FilterRestore && isReturnRegRestore(Defs, *Rit2, TRI))) {
          if (IntDist < 0)
            IntDist = Dist;
          if (!WidestInt || Rit2->Output.Size > WidestInt->Output.Size)
            WidestInt = &*Rit2;
        }

        if (TRI.hasFPReturnReg() && Rit2->Output.RegOff == TRI.FPReturnReg) {
          if (!FPRegWriteOp)
            FPRegWriteOp =
                &*Rit2; // closest write, used by the constant fallback
          if (!FirstFloat) {
            uint16_t E = fpReturnElemSize(Defs, Rit2->Output, 0);
            if (!E)
              E = scalarFPLoadElemSize(Defs, Rit2->Output, 0);
            if (E) {
              FirstFloat = &*Rit2;
              FloatElem = E;
              FloatDist = Dist;
            }
          }
        }
      }

      // A shared epilogue can merge every return path before a single `ret`,
      // leaving the RETURN's own block with no return-register *write* — the FP
      // result is carried across the merge by an FP-return-register PHI (an
      // i386/ARM FP-returning recursive callee: `n<=0` passes the FP argument
      // through, the recursive arm computes it, and both join at the common
      // `add sp / pop / ret`).  Treat such a PHI as a floating-point return
      // when its arms trace to genuine FP producers AND the block does not
      // convert the FP value to an integer for the result (an `(int)acc`
      // reduction returns the converted integer, signalled by a float-to-int op
      // here). Gated on no in-block integer-return write: a genuine integer
      // return materializes the result in the int return register here (an FP
      // value reinterpreted to int via `vmov r0,s0` / store-load `memcpy`, then
      // returned), which must win over a merge-point FP PHI that is only an
      // intermediate.  The recursive FP callee has no such write — its FP PHI
      // is the live result returned in V0/D0.
      if (!FirstFloat && !WidestInt && TRI.hasFPReturnReg()) {
        bool ConvertsToInt = false;
        for (const auto &Op2 : Blk.Ops)
          if (Op2.Opcode == NdOp::FLOAT_FLOAT2INT) {
            ConvertsToInt = true;
            break;
          }
        if (!ConvertsToInt)
          for (const auto &Phi : Blk.Phis) {
            if (Phi.Output.Kind != MedVar::Reg ||
                Phi.Output.RegOff != TRI.FPReturnReg)
              continue;
            for (const auto &A : Phi.Args) {
              uint16_t E = fpReturnElemSize(Defs, A.second, 0);
              if (!E)
                E = scalarFPLoadElemSize(Defs, A.second, 0);
              if (E) {
                FloatElem = E;
                FloatDist = 0;
                FloatViaPhi = true;
                break;
              }
            }
            if (FloatViaPhi)
              break;
          }
      }

      // Constant FP return fallback: the FP return register is written near the
      // RETURN with no FP-producer / FP-load provenance (a floating constant
      // moved in via `fmov d0,xN`) AND no integer return register is written,
      // so the result is left solely in the FP register -> scalar FP return.
      // Without this a leaf `return 3.5;` at -O2 is mistyped as an integer
      // return that reads the never-written int return register (returning 0).
      uint16_t FloatFallbackElem = 0;
      if (!FirstFloat && !FloatViaPhi && IntDist < 0 && FPRegWriteOp)
        FloatFallbackElem = fpReturnFallbackElem(*FPRegWriteOp);

      bool UseFloat = (FirstFloat || FloatViaPhi || FloatFallbackElem) &&
                      (IntDist < 0 || FloatDist <= IntDist);
      if (UseFloat) {
        AnyFloat = true;
        uint16_t E = FloatElem ? FloatElem : FloatFallbackElem;
        if (E > BestFloatElem)
          BestFloatElem = E;
      } else if (WidestInt &&
                 (!BestInt || WidestInt->Output.Size > BestInt->Output.Size)) {
        BestInt = WidestInt;
      }

      // No in-block integer-return write: the result may arrive through a
      // return-register PHI at a merged epilogue.  Type it by the widest
      // effective width of the PHI arguments so an i64 result returned via the
      // merge is not truncated by a sibling narrow `return 0` path.
      if (!UseFloat && IntDist < 0) {
        for (const auto &Phi : Blk.Phis)
          if (Phi.Output.Kind == MedVar::Reg &&
              Phi.Output.RegOff == TRI.IntReturnReg)
            for (const auto &A : Phi.Args) {
              uint16_t W = intRetEffWidth(Defs, PhiDefs, A.second, 0);
              if (W > BestIntPhiWidth)
                BestIntPhiWidth = W;
            }
      }
    }
  }

  if (AnyFloat)
    return NdType::makeFloat(BestFloatElem ? BestFloatElem : 8);
  uint16_t IntSize = 0;
  if (BestInt)
    IntSize = (BestInt->Opcode == NdOp::INT_ZEXT && BestInt->NumInputs >= 1)
                  ? BestInt->Inputs[0].Size
                  : BestInt->Output.Size;
  if (BestIntPhiWidth > IntSize)
    IntSize = BestIntPhiWidth;
  if (IntSize)
    return NdType::makeInt(IntSize);
  return NdType::makeInt(DefaultSize);
}

//===----------------------------------------------------------------------===//
// Parameter type inference
//===----------------------------------------------------------------------===//

static void inferParamTypes(MedFunc &Func, const TargetRegInfo &TRI) {
  std::map<std::pair<int, int>, const MedOp *> DefMap;
  for (const auto &Blk : Func.Blocks)
    for (const auto &Op : Blk.Ops)
      if (Op.Output.Id >= 0 && Op.Output.Size > 0)
        DefMap[{Op.Output.Id, Op.Output.SSAVer}] = &Op;

  // A parameter register is a pointer only when its INCOMING value (SSA version
  // 0, the live-in) is used as a memory address / indirect target.  A register
  // that merely shares its number with a parameter but holds a later, unrelated
  // pointer (e.g. `x0 := &local_buffer` set up for an outgoing call argument, a
  // redefinition at SSA version > 0) must NOT taint the parameter: typing an
  // integer parameter as a pointer makes the caller pass
  // `inttoptr(smallconst)`, which the rewrite backend materialises as a bogus
  // PC-relative address rather than the literal value (an integer argument `10`
  // became `0x20000000a`).
  auto isLiveInReg = [](const MedVar &V) {
    return V.Kind == MedVar::Reg && V.Id >= 0 && V.SSAVer == 0;
  };
  std::set<uint64_t> PtrParamRegOffs;
  for (const auto &Blk : Func.Blocks) {
    for (const auto &Op : Blk.Ops) {
      if (Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::INDIR_CALL) {
        if (Op.NumInputs >= 1 && isLiveInReg(Op.Inputs[0]))
          PtrParamRegOffs.insert(Op.Inputs[0].RegOff);
      }
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Op.NumInputs >= 1) {
        const auto &AddrVar = Op.Inputs[0];
        if (isLiveInReg(AddrVar))
          PtrParamRegOffs.insert(AddrVar.RegOff);
        if (AddrVar.Kind == MedVar::Temp && AddrVar.Id >= 0) {
          auto Dit = DefMap.find({AddrVar.Id, AddrVar.SSAVer});
          if (Dit != DefMap.end() && Dit->second->Opcode == NdOp::INT_ADD &&
              Dit->second->NumInputs >= 2) {
            for (uint8_t KI = 0; KI < Dit->second->NumInputs; ++KI) {
              if (isLiveInReg(Dit->second->Inputs[KI]))
                PtrParamRegOffs.insert(Dit->second->Inputs[KI].RegOff);
            }
          }
        }
      }
    }
  }

  Func.TypedParams.clear();
  for (size_t PI = 0; PI < Func.Params.size(); ++PI) {
    const auto &MP = Func.Params[PI];
    MedTypedParam TP;
    TP.Name = "arg" + std::to_string(PI);
    if (PtrParamRegOffs.count(MP.RegOff) && !TRI.isFrameOrLinkReg(MP.RegOff))
      TP.Type = NdType::makePtr();
    else
      TP.Type = NdType::makeInt(MP.Size);
    Func.TypedParams.push_back(TP);
  }
}

//===----------------------------------------------------------------------===//
// Local variable type inference
//===----------------------------------------------------------------------===//

static bool isFloatOp(NdOp Opc) {
  return Opc == NdOp::FLOAT_ADD || Opc == NdOp::FLOAT_SUB ||
         Opc == NdOp::FLOAT_MULT || Opc == NdOp::FLOAT_DIV ||
         Opc == NdOp::FLOAT_FMA || Opc == NdOp::FLOAT_NEG ||
         Opc == NdOp::FLOAT_SQRT || Opc == NdOp::FLOAT_ABS ||
         Opc == NdOp::FLOAT_CEIL || Opc == NdOp::FLOAT_FLOOR ||
         Opc == NdOp::FLOAT_MIN || Opc == NdOp::FLOAT_MAX ||
         Opc == NdOp::FLOAT_MINNUM || Opc == NdOp::FLOAT_MAXNUM;
}

static void inferLocalTypes(MedFunc &Func) {
  std::set<int64_t> FloatStackOffs;
  for (const auto &Blk : Func.Blocks) {
    for (const auto &Op : Blk.Ops) {
      if (!isFloatOp(Op.Opcode))
        continue;
      for (uint8_t I = 0; I < Op.NumInputs; ++I) {
        if (Op.Inputs[I].Kind == MedVar::Stack)
          FloatStackOffs.insert(Op.Inputs[I].StackOff);
      }
      if (Op.Output.Kind == MedVar::Stack)
        FloatStackOffs.insert(Op.Output.StackOff);
    }
  }

  Func.TypedLocals.clear();
  for (const auto &ML : Func.Locals) {
    MedTypedLocal TL;
    TL.Name = ML.display();
    TL.StackOff = ML.StackOff;
    if (FloatStackOffs.count(ML.StackOff))
      TL.Type = NdType::makeFloat(ML.Size > 0 ? ML.Size : 4);
    else
      TL.Type = NdType::makeInt(ML.Size > 0 ? ML.Size : 4);
    Func.TypedLocals.push_back(TL);
  }
}

//===----------------------------------------------------------------------===//
// Entry point
//===----------------------------------------------------------------------===//

void inferMedTypes(MedFunc &Func, Arch TheArch) {
  const auto &TRI = getTargetRegInfo(TheArch);
  Func.ReturnType = inferReturnType(Func, TRI, TheArch);
  inferParamTypes(Func, TRI);
  inferLocalTypes(Func);

  // i386 cdecl returns a floating-point value through the x87 stack (st0): an
  // external callee loads its result onto st0 (`fldl`) before `ret`, so any x87
  // stack-register write in an FP-returning i386 function marks the st0 return
  // convention.  clang's internal convention for static functions returns FP in
  // XMM0 with no x87, so this cleanly separates the two.
  if (Func.ReturnType && Func.ReturnType->Kind == NdTypeKind::Float) {
    for (const auto &Blk : Func.Blocks) {
      for (const auto &Op : Blk.Ops)
        if (Op.Output.Kind == MedVar::Reg &&
            TRI.isX87StackReg(Op.Output.RegOff)) {
          Func.FPReturnViaX87 = true;
          break;
        }
      if (Func.FPReturnViaX87)
        break;
    }
  }
}

} // namespace neverd
