//===- NativeVectorCall.h - Header intrinsics for vectors -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The compiler-header intrinsic a target vector operation is spelled with.
/// Scalarizing leaves a target intrinsic whole, since its lanes are not
/// independent; C names it as arm_neon.h or the x86 headers do, converting
/// each vector operand to the header's type and the result back. A call whose
/// types or immediates the header would not accept has no entry here and is
/// refused where it is met.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_NATIVEVECTORCALL_H
#define NEVERD_BACKEND_C_RENDER_NATIVEVECTORCALL_H

#include "neverd/Common.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsAArch64.h"
#include "llvm/IR/IntrinsicsARM.h"
#include "llvm/IR/IntrinsicsX86.h"

#include <cstdint>
#include <optional>
#include <string>

namespace neverd {

struct NativeVectorCall {
  struct Operand {
    /// The header's parameter type the argument converts to; empty for an
    /// immediate, which the header takes as a constant expression.
    std::string Type;
    /// The call argument this operand is, when it has a type.
    unsigned Argument = 0;
    /// The immediate's value, when it has none.
    int64_t Immediate = 0;
  };
  /// The header's name of the intrinsic.
  std::string Name;
  llvm::SmallVector<Operand, 5> Operands;
  /// A header function that turns the intrinsic's result into a vector, for
  /// a result type C cannot cast to one (poly128_t).
  const char *ResultThrough = nullptr;
  /// The x86 target feature the header declares the intrinsic with.
  const char *Feature = nullptr;
};

namespace native_vector {

/// arm_neon.h's name of a vector type: int32x4_t, poly8x8_t, float64x2_t.
/// \p Kind is 's', 'u' or 'p' for integer lanes and 'f' for floating-point
/// ones. Empty for a type the header has no name for.
inline std::string neonType(llvm::Type *Type, char Kind) {
  const auto *Vector = llvm::dyn_cast<llvm::FixedVectorType>(Type);
  if (!Vector)
    return {};
  auto *Lane = Vector->getElementType();
  const unsigned Bits = Lane->getPrimitiveSizeInBits();
  const unsigned Lanes = Vector->getNumElements();
  if ((Bits * Lanes != 64 && Bits * Lanes != 128) ||
      (Bits != 8 && Bits != 16 && Bits != 32 && Bits != 64))
    return {};
  const char *Family = nullptr;
  if (Kind == 'f') {
    if (!Lane->isHalfTy() && !Lane->isFloatTy() && !Lane->isDoubleTy())
      return {};
    Family = "float";
  } else {
    if (!Lane->isIntegerTy() || (Kind == 'p' && Bits == 32))
      return {};
    Family = Kind == 's' ? "int" : Kind == 'u' ? "uint" : "poly";
  }
  return std::string(Family) + std::to_string(Bits) + "x" +
         std::to_string(Lanes) + "_t";
}

/// What follows an operation's stem in arm_neon.h's name for \p Type: "q"
/// for a 128-bit vector when the name carries the size, "_n" for a form with
/// an immediate, and the lane type (qshl + q_s32, qshrn + _n_u16).
inline std::string neonSuffix(const llvm::Type *Type, char Kind,
                              bool Sized = true, bool Immediate = false) {
  const auto *Vector = llvm::cast<llvm::FixedVectorType>(Type);
  std::string Text;
  if (Sized && Vector->getPrimitiveSizeInBits() == 128)
    Text += 'q';
  Text += Immediate ? "_n_" : "_";
  Text += Kind;
  return Text + std::to_string(Vector->getScalarSizeInBits());
}

/// The value every lane of \p Value holds, when it is a constant vector of
/// one integer.
inline std::optional<int64_t> constantSplat(const llvm::Value *Value) {
  const auto *Constant = llvm::dyn_cast<llvm::Constant>(Value);
  if (!Constant || !Constant->getType()->isVectorTy())
    return std::nullopt;
  const auto *Lane =
      llvm::dyn_cast_or_null<llvm::ConstantInt>(Constant->getSplatValue());
  if (!Lane || Lane->getBitWidth() > 64)
    return std::nullopt;
  return Lane->getSExtValue();
}

inline std::optional<int64_t> constantInteger(const llvm::Value *Value) {
  const auto *Constant = llvm::dyn_cast<llvm::ConstantInt>(Value);
  if (!Constant || Constant->getBitWidth() > 64)
    return std::nullopt;
  return Constant->getSExtValue();
}

/// A NEON call named \p Name. \p Kinds holds one letter per argument: 's',
/// 'u', 'p' or 'f' for a vector passed as the header's type of that kind, 'i'
/// for an integer scalar. \p ResultKind is the result's, which only has to be
/// a type the header names.
inline std::optional<NativeVectorCall> neon(const llvm::CallBase &Call,
                                            std::string Name,
                                            llvm::StringRef Kinds,
                                            char ResultKind) {
  if (Call.arg_size() != Kinds.size() ||
      neonType(Call.getType(), ResultKind).empty())
    return std::nullopt;
  NativeVectorCall Native;
  Native.Name = std::move(Name);
  for (unsigned I = 0; I != Kinds.size(); ++I) {
    auto *Type = Call.getArgOperand(I)->getType();
    std::string Header;
    if (Kinds[I] == 'i') {
      if (!Type->isIntegerTy(32) && !Type->isIntegerTy(64))
        return std::nullopt;
      Header = Type->isIntegerTy(32) ? "uint32_t" : "uint64_t";
    } else {
      Header = neonType(Type, Kinds[I]);
    }
    if (Header.empty())
      return std::nullopt;
    Native.Operands.push_back({std::move(Header), I, 0});
  }
  return Native;
}

/// The operation \p Stem (qshl, rshl, mulx) of the first argument's lane
/// type, on operands of one shape.
inline std::optional<NativeVectorCall>
neonSameShape(const llvm::CallBase &Call, llvm::StringRef Stem, char Suffix,
              llvm::StringRef Kinds, char ResultKind) {
  for (const auto &Argument : Call.args())
    if (Argument->getType() != Call.getType())
      return std::nullopt;
  if (!llvm::isa<llvm::FixedVectorType>(Call.getType()))
    return std::nullopt;
  return neon(Call, ("v" + Stem + neonSuffix(Call.getType(), Suffix)).str(),
              Kinds, ResultKind);
}

/// A shift by an immediate: \p Stem of the first argument's lane type, the
/// count \p Count in [\p Least, \p Most].
inline std::optional<NativeVectorCall>
neonShiftImmediate(const llvm::CallBase &Call, llvm::StringRef Stem,
                   char Suffix, char ResultKind, bool Sized,
                   std::optional<int64_t> Count, int64_t Least, int64_t Most) {
  auto *Source = Call.getArgOperand(0)->getType();
  const std::string Header = neonType(Source, Suffix);
  if (!Count || *Count < Least || *Count > Most || Header.empty() ||
      neonType(Call.getType(), ResultKind).empty())
    return std::nullopt;
  NativeVectorCall Native;
  Native.Name =
      ("v" + Stem + neonSuffix(Source, Suffix, Sized, /*Immediate=*/true))
          .str();
  Native.Operands.push_back({Header, 0, 0});
  Native.Operands.push_back({{}, 0, *Count});
  return Native;
}

/// A narrowing right shift by an immediate: the source has lanes twice the
/// result's, and the count is 1 to the result's lane width.
inline std::optional<NativeVectorCall>
neonNarrowingShift(const llvm::CallBase &Call, llvm::StringRef Stem,
                   char Suffix, char ResultKind, std::optional<int64_t> Count) {
  const auto *Source =
      llvm::dyn_cast<llvm::FixedVectorType>(Call.getArgOperand(0)->getType());
  const auto *Result = llvm::dyn_cast<llvm::FixedVectorType>(Call.getType());
  if (Call.arg_size() != 2 || !Source || !Result ||
      Source->getNumElements() != Result->getNumElements() ||
      Source->getScalarSizeInBits() != 2 * Result->getScalarSizeInBits())
    return std::nullopt;
  return neonShiftImmediate(Call, Stem, Suffix, ResultKind, /*Sized=*/false,
                            Count, 1, Result->getScalarSizeInBits());
}

/// An x86 header call. \p Kinds holds one letter per argument: 'x' for an
/// integer vector passed as __m128i or __m256i, 'i' for an integer scalar,
/// 'n' for an integer the header takes as a constant, of which the
/// instruction reads the low \p ImmediateBits.
inline std::optional<NativeVectorCall>
x86(const llvm::CallBase &Call, const char *Name, llvm::StringRef Kinds,
    unsigned VectorBits, const char *Feature, unsigned ImmediateBits = 8) {
  if (Call.arg_size() != Kinds.size())
    return std::nullopt;
  const auto IsVector = [&](llvm::Type *Type) {
    const auto *Vector = llvm::dyn_cast<llvm::FixedVectorType>(Type);
    return Vector && Vector->getElementType()->isIntegerTy() &&
           Vector->getPrimitiveSizeInBits() == VectorBits;
  };
  if (!IsVector(Call.getType()) && !Call.getType()->isIntegerTy(32))
    return std::nullopt;
  NativeVectorCall Native;
  Native.Name = Name;
  Native.Feature = Feature;
  for (unsigned I = 0; I != Kinds.size(); ++I) {
    const auto *Argument = Call.getArgOperand(I);
    auto *Type = Argument->getType();
    if (Kinds[I] == 'x') {
      if (!IsVector(Type))
        return std::nullopt;
      Native.Operands.push_back(
          {VectorBits == 256 ? "__m256i" : "__m128i", I, 0});
    } else if (Kinds[I] == 'i') {
      if (!Type->isIntegerTy(32))
        return std::nullopt;
      Native.Operands.push_back({"int", I, 0});
    } else {
      const auto Value = constantInteger(Argument);
      if (!Value || !Type->isIntegerTy(8))
        return std::nullopt;
      Native.Operands.push_back(
          {{}, 0, *Value & ((int64_t(1) << ImmediateBits) - 1)});
    }
  }
  return Native;
}

} // namespace native_vector

/// The header intrinsic \p Call is spelled with, when it is a target vector
/// intrinsic of \p TheArch with the types and constants the header takes.
inline std::optional<NativeVectorCall>
nativeVectorCall(const llvm::CallBase &Call, Arch TheArch) {
  using namespace native_vector;
  namespace I = llvm::Intrinsic;
  const auto *Callee = Call.getCalledFunction();
  if (!Callee || !Callee->isIntrinsic() || Call.hasOperandBundles() ||
      Call.getFunctionType() != Callee->getFunctionType() ||
      !llvm::isa<llvm::CallInst>(Call))
    return std::nullopt;
  const auto Id = Callee->getIntrinsicID();
  const bool IsA64 = TheArch == Arch::AArch64;
  const bool IsARM = TheArch == Arch::ARM;
  const bool IsX86 = TheArch == Arch::X86 || TheArch == Arch::X64;
  // The two NEON back ends name one operation differently; the header does
  // not.
  const auto Is = [&](I::ID A64, I::ID ARM) {
    return (IsA64 && Id == A64) || (IsARM && Id == ARM);
  };
  const auto Argument = [&](unsigned Index) -> const llvm::Value * {
    return Index < Call.arg_size() ? Call.getArgOperand(Index) : nullptr;
  };
  const auto Splat = [&](unsigned Index) -> std::optional<int64_t> {
    const auto *Value = Argument(Index);
    return Value ? constantSplat(Value) : std::nullopt;
  };
  const auto Integer = [&](unsigned Index) -> std::optional<int64_t> {
    const auto *Value = Argument(Index);
    return Value ? constantInteger(Value) : std::nullopt;
  };
  // AArch64 gives a narrowing shift its count as a scalar; AArch32 gives a
  // vector of the negated count.
  const auto NarrowingCount = [&]() -> std::optional<int64_t> {
    if (IsA64)
      return Integer(1);
    const auto Negated = Splat(1);
    return Negated ? std::optional<int64_t>(-*Negated) : std::nullopt;
  };
  const auto LaneBits = [&]() -> unsigned {
    return Call.getType()->isVectorTy() ? Call.getType()->getScalarSizeInBits()
                                        : 0;
  };

  if (IsA64 || IsARM) {
    // AES and SHA: fixed 128-bit forms.
    if (Is(I::aarch64_crypto_aese, I::arm_neon_aese))
      return neon(Call, "vaeseq_u8", "uu", 'u');
    if (Is(I::aarch64_crypto_aesd, I::arm_neon_aesd))
      return neon(Call, "vaesdq_u8", "uu", 'u');
    if (Is(I::aarch64_crypto_aesmc, I::arm_neon_aesmc))
      return neon(Call, "vaesmcq_u8", "u", 'u');
    if (Is(I::aarch64_crypto_aesimc, I::arm_neon_aesimc))
      return neon(Call, "vaesimcq_u8", "u", 'u');
    if (Is(I::aarch64_crypto_sha1c, I::arm_neon_sha1c))
      return neon(Call, "vsha1cq_u32", "uiu", 'u');
    if (Is(I::aarch64_crypto_sha1p, I::arm_neon_sha1p))
      return neon(Call, "vsha1pq_u32", "uiu", 'u');
    if (Is(I::aarch64_crypto_sha1m, I::arm_neon_sha1m))
      return neon(Call, "vsha1mq_u32", "uiu", 'u');
    if (Is(I::aarch64_crypto_sha1su0, I::arm_neon_sha1su0))
      return neon(Call, "vsha1su0q_u32", "uuu", 'u');
    if (Is(I::aarch64_crypto_sha1su1, I::arm_neon_sha1su1))
      return neon(Call, "vsha1su1q_u32", "uu", 'u');
    if (Is(I::aarch64_crypto_sha256h, I::arm_neon_sha256h))
      return neon(Call, "vsha256hq_u32", "uuu", 'u');
    if (Is(I::aarch64_crypto_sha256h2, I::arm_neon_sha256h2))
      return neon(Call, "vsha256h2q_u32", "uuu", 'u');
    if (Is(I::aarch64_crypto_sha256su0, I::arm_neon_sha256su0))
      return neon(Call, "vsha256su0q_u32", "uu", 'u');
    if (Is(I::aarch64_crypto_sha256su1, I::arm_neon_sha256su1))
      return neon(Call, "vsha256su1q_u32", "uuu", 'u');

    // Saturating and rounding shifts by a vector of signed counts.
    if (Is(I::aarch64_neon_sqshl, I::arm_neon_vqshifts))
      return neonSameShape(Call, "qshl", 's', "ss", 's');
    if (Is(I::aarch64_neon_uqshl, I::arm_neon_vqshiftu))
      return neonSameShape(Call, "qshl", 'u', "us", 'u');
    if (Is(I::aarch64_neon_sqrshl, I::arm_neon_vqrshifts))
      return neonSameShape(Call, "qrshl", 's', "ss", 's');
    if (Is(I::aarch64_neon_uqrshl, I::arm_neon_vqrshiftu))
      return neonSameShape(Call, "qrshl", 'u', "us", 'u');
    if (Is(I::aarch64_neon_srshl, I::arm_neon_vrshifts))
      return neonSameShape(Call, "rshl", 's', "ss", 's');
    if (Is(I::aarch64_neon_urshl, I::arm_neon_vrshiftu))
      return neonSameShape(Call, "rshl", 'u', "us", 'u');
    // Shifts by an immediate, which both back ends carry as an operand.
    if (Is(I::aarch64_neon_sqshlu, I::arm_neon_vqshiftsu))
      return Call.arg_size() == 2 &&
                     Call.getArgOperand(0)->getType() == Call.getType()
                 ? neonShiftImmediate(Call, "qshlu", 's', 'u', /*Sized=*/true,
                                      Splat(1), 0, int64_t(LaneBits()) - 1)
                 : std::nullopt;
    if (Is(I::aarch64_neon_sqshrn, I::arm_neon_vqshiftns))
      return neonNarrowingShift(Call, "qshrn", 's', 's', NarrowingCount());
    if (Is(I::aarch64_neon_uqshrn, I::arm_neon_vqshiftnu))
      return neonNarrowingShift(Call, "qshrn", 'u', 'u', NarrowingCount());
    if (Is(I::aarch64_neon_sqrshrn, I::arm_neon_vqrshiftns))
      return neonNarrowingShift(Call, "qrshrn", 's', 's', NarrowingCount());
    if (Is(I::aarch64_neon_uqrshrn, I::arm_neon_vqrshiftnu))
      return neonNarrowingShift(Call, "qrshrn", 'u', 'u', NarrowingCount());
    if (Is(I::aarch64_neon_sqshrun, I::arm_neon_vqshiftnsu))
      return neonNarrowingShift(Call, "qshrun", 's', 'u', NarrowingCount());
    if (Is(I::aarch64_neon_sqrshrun, I::arm_neon_vqrshiftnsu))
      return neonNarrowingShift(Call, "qrshrun", 's', 'u', NarrowingCount());

    // Polynomial products.
    if (Is(I::aarch64_neon_pmul, I::arm_neon_vmulp))
      return neonSameShape(Call, "mul", 'p', "pp", 'p');
    if (Is(I::aarch64_neon_pmull, I::arm_neon_vmullp))
      return Call.arg_size() == 2 &&
                     Call.getArgOperand(0)->getType() ==
                         Call.getArgOperand(1)->getType() &&
                     neonType(Call.getArgOperand(0)->getType(), 'p') ==
                         "poly8x8_t"
                 ? neon(Call, "vmull_p8", "pp", 'p')
                 : std::nullopt;

    // Reciprocal and reciprocal square root estimates and steps.
    if (Is(I::aarch64_neon_frecpe, I::arm_neon_vrecpe) &&
        Call.getType()->isFPOrFPVectorTy())
      return neonSameShape(Call, "recpe", 'f', "f", 'f');
    if (Is(I::aarch64_neon_frsqrte, I::arm_neon_vrsqrte) &&
        Call.getType()->isFPOrFPVectorTy())
      return neonSameShape(Call, "rsqrte", 'f', "f", 'f');
    if (Is(I::aarch64_neon_frecps, I::arm_neon_vrecps))
      return neonSameShape(Call, "recps", 'f', "ff", 'f');
    if (Is(I::aarch64_neon_frsqrts, I::arm_neon_vrsqrts))
      return neonSameShape(Call, "rsqrts", 'f', "ff", 'f');
    if (Is(I::aarch64_neon_urecpe, I::arm_neon_vrecpe) && LaneBits() == 32)
      return neonSameShape(Call, "recpe", 'u', "u", 'u');
    if (Is(I::aarch64_neon_ursqrte, I::arm_neon_vrsqrte) && LaneBits() == 32)
      return neonSameShape(Call, "rsqrte", 'u', "u", 'u');
  }

  if (IsARM) {
    // Saturating doubling products.
    if (Id == I::arm_neon_vqdmulh)
      return neonSameShape(Call, "qdmulh", 's', "ss", 's');
    if (Id == I::arm_neon_vqrdmulh)
      return neonSameShape(Call, "qrdmulh", 's', "ss", 's');
    if (Id == I::arm_neon_vqdmull) {
      const auto *Source = llvm::dyn_cast<llvm::FixedVectorType>(
          Call.arg_size() == 2 ? Call.getArgOperand(0)->getType() : nullptr);
      const auto *Result =
          llvm::dyn_cast<llvm::FixedVectorType>(Call.getType());
      if (!Source || !Result || Call.getArgOperand(1)->getType() != Source ||
          Source->getNumElements() != Result->getNumElements() ||
          Result->getScalarSizeInBits() != 2 * Source->getScalarSizeInBits())
        return std::nullopt;
      return neon(Call, "vqdmull" + neonSuffix(Source, 's', /*Sized=*/false),
                  "ss", 's');
    }
  }

  if (IsA64) {
    if (Id == I::aarch64_crypto_sha512h)
      return neon(Call, "vsha512hq_u64", "uuu", 'u');
    if (Id == I::aarch64_crypto_sha512h2)
      return neon(Call, "vsha512h2q_u64", "uuu", 'u');
    if (Id == I::aarch64_crypto_sha512su0)
      return neon(Call, "vsha512su0q_u64", "uu", 'u');
    if (Id == I::aarch64_crypto_sha512su1)
      return neon(Call, "vsha512su1q_u64", "uuu", 'u');
    if (Id == I::aarch64_crypto_sm3ss1)
      return neon(Call, "vsm3ss1q_u32", "uuu", 'u');
    if (Id == I::aarch64_crypto_sm3partw1)
      return neon(Call, "vsm3partw1q_u32", "uuu", 'u');
    if (Id == I::aarch64_crypto_sm3partw2)
      return neon(Call, "vsm3partw2q_u32", "uuu", 'u');
    if (Id == I::aarch64_crypto_sm4e)
      return neon(Call, "vsm4eq_u32", "uu", 'u');
    if (Id == I::aarch64_crypto_sm4ekey)
      return neon(Call, "vsm4ekeyq_u32", "uu", 'u');
    const char *Sm3 = Id == I::aarch64_crypto_sm3tt1a   ? "vsm3tt1aq_u32"
                      : Id == I::aarch64_crypto_sm3tt1b ? "vsm3tt1bq_u32"
                      : Id == I::aarch64_crypto_sm3tt2a ? "vsm3tt2aq_u32"
                      : Id == I::aarch64_crypto_sm3tt2b ? "vsm3tt2bq_u32"
                                                        : nullptr;
    if (Sm3) {
      // The fourth operand selects a lane of the third.
      const auto Lane = Integer(3);
      if (Call.arg_size() != 4 || !Lane || *Lane < 0 || *Lane > 3 ||
          neonType(Call.getType(), 'u') != "uint32x4_t")
        return std::nullopt;
      NativeVectorCall Native;
      Native.Name = Sm3;
      for (unsigned Index = 0; Index != 3; ++Index) {
        if (Call.getArgOperand(Index)->getType() != Call.getType())
          return std::nullopt;
        Native.Operands.push_back({"uint32x4_t", Index, 0});
      }
      Native.Operands.push_back({{}, 0, *Lane});
      return Native;
    }
    if (Id == I::aarch64_neon_fmulx && Call.getType()->isVectorTy())
      return neonSameShape(Call, "mulx", 'f', "ff", 'f');
    if (Id == I::aarch64_neon_fcvtxn)
      return Call.arg_size() == 1 &&
                     neonType(Call.getType(), 'f') == "float32x2_t" &&
                     neonType(Call.getArgOperand(0)->getType(), 'f') ==
                         "float64x2_t"
                 ? neon(Call, "vcvtx_f32_f64", "f", 'f')
                 : std::nullopt;
    if (Id == I::aarch64_neon_pmull64) {
      // The carry-less product of two 64-bit polynomials is a poly128_t.
      if (Call.arg_size() != 2 ||
          !Call.getArgOperand(0)->getType()->isIntegerTy(64) ||
          !Call.getArgOperand(1)->getType()->isIntegerTy(64) ||
          neonType(Call.getType(), 'u') != "uint8x16_t")
        return std::nullopt;
      NativeVectorCall Native;
      Native.Name = "vmull_p64";
      Native.ResultThrough = "vreinterpretq_u8_p128";
      Native.Operands.push_back({"poly64_t", 0, 0});
      Native.Operands.push_back({"poly64_t", 1, 0});
      return Native;
    }
  }

  if (IsX86) {
    switch (Id) {
    case I::x86_sse42_pcmpistri128:
      return x86(Call, "_mm_cmpistri", "xxn", 128, "sse4.2");
    case I::x86_sse42_pcmpistria128:
      return x86(Call, "_mm_cmpistra", "xxn", 128, "sse4.2");
    case I::x86_sse42_pcmpistric128:
      return x86(Call, "_mm_cmpistrc", "xxn", 128, "sse4.2");
    case I::x86_sse42_pcmpistrio128:
      return x86(Call, "_mm_cmpistro", "xxn", 128, "sse4.2");
    case I::x86_sse42_pcmpistris128:
      return x86(Call, "_mm_cmpistrs", "xxn", 128, "sse4.2");
    case I::x86_sse42_pcmpistriz128:
      return x86(Call, "_mm_cmpistrz", "xxn", 128, "sse4.2");
    case I::x86_sse42_pcmpistrm128:
      return x86(Call, "_mm_cmpistrm", "xxn", 128, "sse4.2");
    case I::x86_sse42_pcmpestri128:
      return x86(Call, "_mm_cmpestri", "xixin", 128, "sse4.2");
    case I::x86_sse42_pcmpestria128:
      return x86(Call, "_mm_cmpestra", "xixin", 128, "sse4.2");
    case I::x86_sse42_pcmpestric128:
      return x86(Call, "_mm_cmpestrc", "xixin", 128, "sse4.2");
    case I::x86_sse42_pcmpestrio128:
      return x86(Call, "_mm_cmpestro", "xixin", 128, "sse4.2");
    case I::x86_sse42_pcmpestris128:
      return x86(Call, "_mm_cmpestrs", "xixin", 128, "sse4.2");
    case I::x86_sse42_pcmpestriz128:
      return x86(Call, "_mm_cmpestrz", "xixin", 128, "sse4.2");
    case I::x86_sse42_pcmpestrm128:
      return x86(Call, "_mm_cmpestrm", "xixin", 128, "sse4.2");
    case I::x86_sse41_mpsadbw:
      return x86(Call, "_mm_mpsadbw_epu8", "xxn", 128, "sse4.1");
    case I::x86_avx2_mpsadbw:
      return x86(Call, "_mm256_mpsadbw_epu8", "xxn", 256, "avx2");
    case I::x86_avx2_permd:
      return x86(Call, "_mm256_permutevar8x32_epi32", "xx", 256, "avx2");
    case I::x86_pclmulqdq:
      return x86(Call, "_mm_clmulepi64_si128", "xxn", 128, "pclmul");
    case I::x86_sha1rnds4:
      return x86(Call, "_mm_sha1rnds4_epu32", "xxn", 128, "sha",
                 /*ImmediateBits=*/2);
    case I::x86_sha1msg1:
      return x86(Call, "_mm_sha1msg1_epu32", "xx", 128, "sha");
    case I::x86_sha1msg2:
      return x86(Call, "_mm_sha1msg2_epu32", "xx", 128, "sha");
    case I::x86_sha1nexte:
      return x86(Call, "_mm_sha1nexte_epu32", "xx", 128, "sha");
    default:
      break;
    }
  }
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_NATIVEVECTORCALL_H
