//===- MedLLVMAddrResolve.cpp - Shared address tracing helpers --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared SSA constant tracing and address-base decomposition helpers for the
/// literal/select, indexed/induction, and code-pointer resolvers.
///
//===----------------------------------------------------------------------===//

#include "neverd/Common.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"

#include <cstring>
#include <optional>
#include <vector>

namespace neverd {

//===----------------------------------------------------------------------===//
// SSA constant tracing
//===----------------------------------------------------------------------===//

std::optional<uint64_t> MedLLVMEmitter::traceSSAConst(const MedVar &V) const {
  if (V.isConst())
    return V.ConstVal;

  if (!CurMedFunc)
    return std::nullopt;

  MedVar Cur = V;
  for (int Depth = 0; Depth < 8; ++Depth) {
    const MedOp *Def = lookupDef(Cur);
    if (!Def)
      return std::nullopt;
    if (Def->Opcode == NdOp::COPY && Def->NumInputs >= 1) {
      if (Def->Inputs[0].isConst())
        return Def->Inputs[0].ConstVal;
      Cur = Def->Inputs[0];
      continue;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<uint64_t>
MedLLVMEmitter::traceTableBaseConst(const MedVar &V, int Depth,
                                    bool *SawLoad) const {
  if (V.isConst())
    return V.ConstVal;
  if (!CurMedFunc || Depth > 8)
    return std::nullopt;

  const MedOp *Def = lookupDef(V);
  if (!Def)
    return std::nullopt;

  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
    return Def->NumInputs >= 1
               ? traceTableBaseConst(Def->Inputs[0], Depth + 1, SawLoad)
               : std::nullopt;
  case NdOp::INT_ADD: {
    if (Def->NumInputs < 2)
      return std::nullopt;
    auto A = traceTableBaseConst(Def->Inputs[0], Depth + 1, SawLoad);
    auto B = traceTableBaseConst(Def->Inputs[1], Depth + 1, SawLoad);
    if (A && B)
      return *A + *B;
    return std::nullopt;
  }
  case NdOp::LOAD: {
    // Literal-pool load: the table base word lives in a read-only segment and
    // the loader has already applied its relocation, so read it directly.
    if (Def->NumInputs < 1 || !Img)
      return std::nullopt;
    auto Addr = traceTableBaseConst(Def->Inputs[0], Depth + 1, SawLoad);
    if (!Addr)
      return std::nullopt;
    const auto *Seg = Img->getSegmentFor(*Addr);
    if (!Seg || Seg->isWritable() || Seg->Data.empty())
      return std::nullopt;
    size_t Off = static_cast<size_t>(*Addr - Seg->VA);
    uint16_t Sz = Def->Output.Size ? Def->Output.Size : 4;
    if (Sz > 8 || !rangeInBounds(Off, Sz, Seg->Data.size()))
      return std::nullopt;
    uint64_t Val = 0;
    std::memcpy(&Val, Seg->Data.data() + Off, Sz);
    // The literal stores a signed PC-relative displacement; sign-extend so the
    // subsequent `+ pc` produces the absolute table address.
    if (Sz < 8 && (Val & (1ull << (Sz * 8 - 1))))
      Val |= ~uint64_t(0) << (Sz * 8);
    if (SawLoad)
      *SawLoad = true;
    return Val;
  }
  default:
    return std::nullopt;
  }
}

std::optional<uint64_t>
MedLLVMEmitter::indexedConstBase(const MedVar &AddrVar) const {
  if (!CurMedFunc || AddrVar.isConst())
    return std::nullopt;

  const MedOp *Def = lookupDef(AddrVar);
  if (!Def || Def->Opcode != NdOp::INT_ADD || Def->NumInputs < 2)
    return std::nullopt;

  const MedVar &A = Def->Inputs[0];
  const MedVar &B = Def->Inputs[1];
  // Exactly one operand must be a compile-time constant (the base); the other
  // is the runtime index.  A frame store `[SP + disp]` is NOT a const-based
  // array: its constant operand is a small stack displacement, not a global
  // base. Reporting it here poisoned StoredConstBases (any function with a
  // stack array store) and disabled all anonymous-table redirection — clang's
  // loop-idiom CRC table (no named symbol) then read its original VA, unmapped
  // at runtime.
  if (auto CA = traceSSAConst(A);
      CA && !traceSSAConst(B) && !varIsFrameDerived(B))
    return *CA;
  if (auto CB = traceSSAConst(B);
      CB && !traceSSAConst(A) && !varIsFrameDerived(A))
    return *CB;
  return std::nullopt;
}

bool MedLLVMEmitter::collectIndexedGlobalBase(const MedVar &V, uint64_t &Base,
                                              bool &HaveBase,
                                              std::vector<MedVar> &IdxTerms,
                                              int Depth) const {
  if (!CurMedFunc || Depth > 8)
    return false;

  const MedOp *Def = lookupDef(V);
  // A COPY renames and a ZEXT/SEXT widens its source; descend so a `base +
  // index` computation reached through one (e.g. an induction PHI whose init is
  // a COPY of `lea base(%rip), reg; lea (reg,idx), ptr`, or an i386 32-bit
  // `base+idx` zero-extended to the 64-bit address temp) is still decomposed.
  if (Def &&
      (Def->Opcode == NdOp::COPY || Def->Opcode == NdOp::INT_ZEXT ||
       Def->Opcode == NdOp::INT_SEXT) &&
      Def->NumInputs >= 1)
    return collectIndexedGlobalBase(Def->Inputs[0], Base, HaveBase, IdxTerms,
                                    Depth + 1);
  if (!Def || Def->NumInputs < 2 ||
      (Def->Opcode != NdOp::INT_ADD && Def->Opcode != NdOp::INT_SUB))
    return false;

  // INT_SUB(minuend, k): base/index live in the minuend; a constant subtrahend
  // is a negative index addend (reverse-order vectorized gather `base+i*s-k`).
  // A non-constant subtrahend is not a foldable offset, so keep it absolute.
  if (Def->Opcode == NdOp::INT_SUB) {
    auto KC = traceSSAConst(Def->Inputs[1]);
    if (!KC || !collectIndexedGlobalBase(Def->Inputs[0], Base, HaveBase,
                                         IdxTerms, Depth + 1))
      return false;
    uint16_t KSz = Def->Inputs[1].Size ? Def->Inputs[1].Size : 8;
    IdxTerms.push_back(MedVar::makeConst(uint64_t(0) - *KC, KSz));
    return true;
  }

  // Descend only along the branch that exposes the base; each non-base operand
  // is kept whole as one index term (so a constant *inside* the index — e.g.
  // `base + (i+1)` — stays part of that term, never mistaken for the base). The
  // base is identified as a lone constant operand (its value is validated as a
  // resolvable global by the caller), matching the one-level form's leniency.
  // The base is a constant pointing into a non-executable data segment (.rodata
  // /.data).  A small struct-field offset (`tab[i].y` = base+i*s+4) lands in
  // the executable .text range (a .o places .text at VA 0) — treating it as the
  // base would lose the real table base nested deeper, so it is kept as an
  // index addend instead.
  auto isBaseConst = [&](const std::optional<uint64_t> &C) {
    if (!C || *C == 0)
      return false;
    const auto *Seg = Img->getSegmentFor(*C);
    return Seg && !Seg->isExecutable() && !Seg->Data.empty();
  };
  const MedVar &A = Def->Inputs[0];
  const MedVar &B = Def->Inputs[1];
  auto CA = traceSSAConst(A);
  auto CB = traceSSAConst(B);
  bool ABase = isBaseConst(CA);
  bool BBase = isBaseConst(CB);
  if (ABase && BBase)
    return false; // two segment-resident constants — ambiguous
  if (ABase) {
    Base = *CA;
    HaveBase = true;
    IdxTerms.push_back(B);
    return true;
  }
  if (BBase) {
    Base = *CB;
    HaveBase = true;
    IdxTerms.push_back(A);
    return true;
  }
  // Neither operand is the base.  Recurse into a non-constant side to find the
  // base nested under multi-dimensional indexing (`base + row*stride + col`) or
  // past a constant field offset (`base + i*stride + off`); each non-base side
  // (including a constant offset) becomes an index addend.
  if (!CA && collectIndexedGlobalBase(A, Base, HaveBase, IdxTerms, Depth + 1)) {
    IdxTerms.push_back(B);
    return true;
  }
  if (!CB && collectIndexedGlobalBase(B, Base, HaveBase, IdxTerms, Depth + 1)) {
    IdxTerms.push_back(A);
    return true;
  }
  return false;
}

bool MedLLVMEmitter::collectLiteralPoolBase(const MedVar &V, uint64_t &Base,
                                            bool &HaveBase,
                                            std::vector<MedVar> &IdxTerms,
                                            int Depth) const {
  if (!CurMedFunc || Depth > 8)
    return false;

  const MedOp *Def = lookupDef(V);
  if (!Def || Def->NumInputs < 2 ||
      (Def->Opcode != NdOp::INT_ADD && Def->Opcode != NdOp::INT_SUB))
    return false;

  // INT_SUB(minuend, k): the base/index live in the minuend; a constant
  // subtrahend is a negative index addend (clang's reverse-order vectorized
  // gather emits `base + i*stride - k`).  A non-constant subtrahend is not a
  // foldable table offset, so leave such an access absolute.
  if (Def->Opcode == NdOp::INT_SUB) {
    auto KC = traceSSAConst(Def->Inputs[1]);
    if (!KC || !collectLiteralPoolBase(Def->Inputs[0], Base, HaveBase, IdxTerms,
                                       Depth + 1))
      return false;
    uint16_t KSz = Def->Inputs[1].Size ? Def->Inputs[1].Size : 8;
    IdxTerms.push_back(MedVar::makeConst(uint64_t(0) - *KC, KSz));
    return true;
  }

  const MedVar &A = Def->Inputs[0];
  const MedVar &B = Def->Inputs[1];
  bool SawA = false, SawB = false;
  auto CA = traceTableBaseConst(A, 0, &SawA);
  auto CB = traceTableBaseConst(B, 0, &SawB);
  if (CA && SawA && !CB) {
    Base = *CA;
    HaveBase = true;
    IdxTerms.push_back(B);
    return true;
  }
  if (CB && SawB && !CA) {
    Base = *CB;
    HaveBase = true;
    IdxTerms.push_back(A);
    return true;
  }
  // Neither side is itself the literal-pool base: descend the side that exposes
  // one (`base + row*stride + col`); the other whole side is an index term.
  if (!CA && collectLiteralPoolBase(A, Base, HaveBase, IdxTerms, Depth + 1)) {
    IdxTerms.push_back(B);
    return true;
  }
  if (!CB && collectLiteralPoolBase(B, Base, HaveBase, IdxTerms, Depth + 1)) {
    IdxTerms.push_back(A);
    return true;
  }
  return false;
}

} // namespace neverd
