//===- UnalignedMemory.h - Scalar source memory spelling --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_UNALIGNEDMEMORY_H
#define NEVERD_BACKEND_C_UNALIGNEDMEMORY_H

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <optional>
#include <string>

namespace neverd::c_memory {

using ScalarPointerSpelling = CEmitterOptions::ScalarPointerSpelling;

inline void applyMIPSVectorAlignment(CEmitterOptions &Opts,
                                     const BinaryImage *Image) {
  // MSA LD/ST allow any byte address even when scalar LH/LW/LD would fault.
  // A standalone MIPS32 unit has no image profile ruling those accesses out.
  const auto Target = Image ? Image->Arch : Opts.TheArch;
  if ((Target == Arch::MIPS32 || Target == Arch::MIPS32EL) &&
      (!Image || (Image->MIPS && Image->MIPS->HasMSA)))
    Opts.ScalarPointers = ScalarPointerSpelling::AliasTypes;
}

/// A scalar type an access can name directly (`*(uint64_t *)p`), and the
/// alias that reads or writes it at any address under any GCC/Clang build:
/// one byte aligned and allowed to alias every object.  The alias names follow
/// the sizes a classic decompiler prints (`*(_QWORD *)p`); signed integers add
/// `S`.  They are not the identically named types of such a decompiler's
/// headers, which carry neither attribute.
struct Scalar {
  const char *Type;
  const char *Alias;
  /// The type in the names of the memory helpers: `neverd_mem_load_..._i16`.
  const char *Suffix;
  unsigned Bytes;
  bool Integer;
};

inline constexpr Scalar Scalars[] = {
    {"uint8_t", "_BYTE", "u8", 1, true},
    {"int8_t", "_SBYTE", "i8", 1, true},
    {"uint16_t", "_WORD", "u16", 2, true},
    {"int16_t", "_SWORD", "i16", 2, true},
    {"uint32_t", "_DWORD", "u32", 4, true},
    {"int32_t", "_SDWORD", "i32", 4, true},
    {"uint64_t", "_QWORD", "u64", 8, true},
    {"int64_t", "_SQWORD", "i64", 8, true},
    {"unsigned __int128", "_OWORD", "u128", 16, true},
    {"__int128", "_SOWORD", "i128", 16, true},
    {"float", "_FLOAT", "f32", 4, false},
    {"double", "_DOUBLE", "f64", 8, false},
};

/// The type an access to \p Type names in \p Spelling, or empty if such an
/// access is a byte copy instead.
inline std::string alias(llvm::StringRef Type, ScalarPointerSpelling Spelling) {
  for (const auto &S : Scalars) {
    if (Type != S.Type)
      continue;
    if (Spelling == ScalarPointerSpelling::AliasTypes)
      return S.Alias;
    // Compilers move a 16-byte scalar with aligned vector instructions.
    return S.Bytes < 16 ? S.Type : std::string();
  }
  return {};
}

/// \p Type's short name in helper names, or empty if it is no scalar here.
inline std::string suffix(llvm::StringRef Type) {
  for (const auto &S : Scalars)
    if (Type == S.Type)
      return S.Suffix;
  return {};
}

/// Whether \p Text is an integer access in either spelling, parenthesized or
/// not: `*(uint64_t *)p`, `*(_QWORD *)p`.
inline bool integerAccess(llvm::StringRef Text) {
  if (!Text.consume_front("(*("))
    Text.consume_front("*(");
  const auto Names = [&](llvm::StringRef Name) {
    return Text.starts_with(Name) &&
           Text.drop_front(Name.size()).starts_with(" *)");
  };
  return llvm::any_of(Scalars, [&](const Scalar &S) {
    return S.Integer && (Names(S.Alias) || Names(S.Type));
  });
}

/// What accesses in \p Spelling need before the code that uses them: the
/// alias types, or a note of what the standard types assume.
template <typename Stream>
inline void writeTypes(Stream &OS, ScalarPointerSpelling Spelling) {
  if (Spelling == ScalarPointerSpelling::StandardTypes) {
    OS << "/* Scalars are read and written through plain pointer casts at any "
          "address:\n   build for a target with unaligned access, with "
          "-fno-strict-aliasing. */\n\n";
    return;
  }
  OS << "#ifndef NEVERD_UNALIGNED_SCALARS\n"
        "#define NEVERD_UNALIGNED_SCALARS\n"
        "#if !defined(__clang__) && !defined(__GNUC__)\n"
        "#error \"unaligned pointer output requires Clang or GCC\"\n"
        "#endif\n";
  for (const auto &S : Scalars) {
    // 32-bit targets have no __int128; an access of that width could not be
    // spelled in either form there.
    const bool Wide = S.Bytes == 16;
    if (Wide)
      OS << "#if defined(__SIZEOF_INT128__)\n";
    OS << "typedef " << S.Type << " " << S.Alias
       << " __attribute__((aligned(1), may_alias));\n"
       << "_Static_assert(sizeof(" << S.Alias << ") == " << S.Bytes
       << " && _Alignof(" << S.Alias
       << ") == 1, \"unaligned scalar layout\");\n";
    if (Wide)
      OS << "#endif\n";
  }
  OS << "#endif\n\n";
}

/// \p Text as the operand of a cast.  A name, a number or one balanced
/// parenthesized whole binds as tightly as the cast already; anything else,
/// an embedded assignment included, is parenthesized.
inline std::string castOperand(llvm::StringRef Text) {
  if (!Text.empty() &&
      llvm::all_of(Text, [](char C) { return llvm::isAlnum(C) || C == '_'; }))
    return Text.str();
  bool Whole = Text.size() >= 2 && Text.front() == '(' && Text.back() == ')';
  int Depth = 0;
  char Quote = 0;
  for (size_t I = 0; Whole && I < Text.size(); ++I) {
    const char C = Text[I];
    if (Quote) {
      if (C == '\\')
        ++I;
      else if (C == Quote)
        Quote = 0;
    } else if (C == '"' || C == '\'') {
      Quote = C;
    } else if (C == '(') {
      ++Depth;
    } else if (C == ')' && --Depth == 0 && I + 1 != Text.size()) {
      Whole = false;
    }
  }
  if (Whole && Depth == 0 && !Quote)
    return Text.str();
  return "(" + Text.str() + ")";
}

/// The access to \p Alias at \p Address, a unary expression:
/// `*(_QWORD *)p`.  An operand of a postfix operator parenthesizes it.
inline std::string access(llvm::StringRef Alias, llvm::StringRef Address) {
  const std::string Pointer = "(" + Alias.str() + " *)";
  // A machine address is a pointer-width integer or an object pointer, and
  // C converts either to the access pointer directly.  An address-of may
  // designate a function, whose pointer converts only through an integer.
  if (Address.starts_with("&"))
    return "*" + Pointer + "(uintptr_t)(" + Address.str() + ")";
  return "*" + Pointer + castOperand(Address);
}

/// \p Text without the parentheses that enclose all of it, when there are
/// such parentheses.
inline std::string unparenthesized(llvm::StringRef Text) {
  if (Text.size() < 2 || Text.front() != '(' || Text.back() != ')')
    return Text.str();
  int Depth = 0;
  char Quote = 0;
  for (size_t I = 0; I < Text.size(); ++I) {
    const char C = Text[I];
    if (Quote) {
      if (C == '\\')
        ++I;
      else if (C == Quote)
        Quote = 0;
    } else if (C == '"' || C == '\'') {
      Quote = C;
    } else if (C == '(') {
      ++Depth;
    } else if (C == ')' && --Depth == 0 && I + 1 != Text.size()) {
      return Text.str();
    }
  }
  return Depth == 0 && !Quote ? Text.drop_front().drop_back().str()
                              : Text.str();
}

/// Whether \p Text is one conversion of an address to an integer, which a
/// cast applies to as it is: `(uintptr_t)p`, `(uintptr_t)&frame.var_m50`,
/// `(uintptr_t)((uint8_t *)&frame.var_m50 + 8)`.
inline bool integerConversion(llvm::StringRef Text) {
  if (!Text.consume_front("(uintptr_t)") || Text.empty())
    return false;
  if (Text.front() == '(')
    return castOperand(Text) == Text;
  Text.consume_front("&");
  return !Text.empty() &&
         (llvm::isAlpha(Text.front()) || Text.front() == '_') &&
         llvm::all_of(Text, [](char C) {
           return llvm::isAlnum(C) || C == '_' || C == '.' || C == '[' ||
                  C == ']';
         });
}

/// \p Address as the integer operand of a pointer cast.
inline std::string integerOperand(llvm::StringRef Address) {
  if (integerConversion(Address))
    return Address.str();
  return "(uintptr_t)(" + Address.str() + ")";
}

// The caller supplies a fresh, exact-type value carrier. In particular, a
// source local whose address escapes cannot be the destination of a load:
// memcpy forbids overlapping source and destination objects.
inline std::string loadCopy(llvm::StringRef Value, llvm::StringRef Address,
                            llvm::StringRef Size) {
  return "__builtin_memcpy(&" + Value.str() + ", (const void *)" +
         integerOperand(Address) + ", " + Size.str() + ")";
}

inline std::string storeCopy(llvm::StringRef Address, llvm::StringRef Value,
                             llvm::StringRef Size) {
  return "__builtin_memcpy((void *)" + integerOperand(Address) + ", &" +
         Value.str() + ", " + Size.str() + ")";
}

/// A byte count, or the byte a fill stores, as the C writers print numbers.
inline std::string byteNumber(uint64_t Value) {
  return Value < 10 ? std::to_string(Value) : "0x" + llvm::utohexstr(Value);
}

/// The byte each of \p Value's \p Bytes low bytes is, where they are all
/// alike: the byte memset stores for a fill of elements of \p Value.
inline std::optional<uint8_t> uniformByte(uint64_t Value, unsigned Bytes) {
  const auto Byte = static_cast<uint8_t>(Value);
  for (unsigned I = 1; I < Bytes && I < 8; ++I)
    if (static_cast<uint8_t>(Value >> (8 * I)) != Byte)
      return std::nullopt;
  return Byte;
}

/// The bytes \p Count elements of \p Element bytes take; none where a
/// string instruction would fault first, past 2^63 bytes.
inline std::optional<uint64_t> elementBytes(uint64_t Count, unsigned Element) {
  if (!Element || Count > (uint64_t{1} << 63) / Element)
    return std::nullopt;
  return Count * Element;
}

/// A forward fill (REP STOS) or copy (REP MOVS) of flat memory as the C
/// library's, which writes exactly the instruction's bytes where its caller
/// proved it does: a fill whose value's bytes are alike, a copy whose ranges
/// cannot overlap.  The addresses are integers.
inline std::string fillCall(llvm::StringRef Destination, llvm::StringRef Byte,
                            llvm::StringRef Size) {
  return "__builtin_memset((void *)" + integerOperand(Destination) + ", " +
         Byte.str() + ", " + Size.str() + ");\n";
}
inline std::string copyCall(llvm::StringRef Destination, llvm::StringRef Source,
                            llvm::StringRef Size) {
  return "__builtin_memcpy((void *)" + integerOperand(Destination) +
         ", (void *)" + integerOperand(Source) + ", " + Size.str() + ");\n";
}

} // namespace neverd::c_memory

#endif
