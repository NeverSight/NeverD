//===- CRecordCall.h - Reassemble C record call arguments --------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared HighC/LLVM-C spelling of a record carried in machine-word operands.
/// Each operand is evaluated once and copied as bits, including pointers.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_CRECORDCALL_H
#define NEVERD_BACKEND_C_CRECORDCALL_H

#include "CIdentifier.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/ir/IntegerCallArguments.h"
#include "neverd/ir/SourceABI.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/FormatVariadic.h"

#include <stdexcept>
#include <string>

namespace neverd::c_detail {
#define NEVERD_C_RECORD_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "CRecordCall.def"

/// Copy each logical member once between complete C value representations.
/// The validated source layout owns the offsets. Padding never becomes an
/// integer value, including when a C record result uses raw LLVM word fields.
inline std::string sourceRecordValue(const TypeRef &Record,
                                     llvm::StringRef From, llvm::StringRef To,
                                     llvm::StringRef Expression) {
  const auto Temporary = [&](llvm::StringRef Stem) {
    std::string Name = Stem.str();
    while (From.contains(Name) || To.contains(Name) ||
           Expression.contains(Name))
      Name += '_';
    return Name;
  };
  const auto Destination = Temporary(kSourceRecordDestination);
  const auto Source = Temporary(kSourceRecordSource);
  std::string Copies;
  for (const auto &Member : sourceAggregateMembers(Record))
    Copies += llvm::formatv(kSourceRecordFieldCopy, Member.ByteOffset,
                            Member.Type->Size, Destination, Source)
                  .str();
  return llvm::formatv(kSourceRecordValue, To, From, Expression, Copies,
                       Destination, Source)
      .str();
}

inline std::string recordCallType(const TypeRef &Type,
                                  const CEmitterOptions &Options) {
  if (!Type || !Options.RecordDefinitions)
    throw std::runtime_error("A record call needs its header's complete type");
  const auto Definitions = Options.RecordDefinitions({Type});
  const std::string Name = cNamedTypeSpelling(Type->SourceName);
  if (llvm::is_contained(Definitions.HeaderTypedefs, Name))
    return Name;
  if (const auto Tag = Definitions.Tags.find(Name);
      Tag != Definitions.Tags.end())
    return std::string(Tag->second ? "union " : "struct ") + Name;
  if (llvm::is_contained(Definitions.Types, Name))
    return Name;
  throw std::runtime_error("A record call's header type cannot be spelled");
}

inline std::string
recordCallArgument(const std::string &Type,
                   const IntegerCallParameter &Parameter,
                   llvm::function_ref<std::string(unsigned)> Operand) {
  if (Type.empty() || Parameter.Pieces.empty())
    throw std::runtime_error("A record call argument has no complete type");
  if (Parameter.Indirect)
    return "*(const " + Type + " *)(uintptr_t)(" +
           Operand(Parameter.Pieces[0].Argument) + ")";
  std::string Text = "({ " + Type + " __neverd_record; ";
  for (size_t I = 0; I < Parameter.Pieces.size(); ++I) {
    const auto &Piece = Parameter.Pieces[I];
    unsigned Word = 1;
    while (Word < Piece.Bytes)
      Word *= 2;
    if (Word > 8)
      throw std::runtime_error("A record call carrier is wider than a word");
    const std::string Integer = "uint" + std::to_string(Word * 8) + "_t";
    const std::string Name = "__neverd_piece" + std::to_string(I);
    if (Piece.Floating) {
      if (Piece.Bytes != 4 && Piece.Bytes != 8)
        throw std::runtime_error(
            "A floating record piece has unsupported width");
      const std::string Float = Piece.Bytes == 4 ? "float" : "double";
      Text += Integer + " " + Name + " = __builtin_bit_cast(" + Integer +
              ", (" + Float + ")(" + Operand(Piece.Argument) + ")); ";
    } else {
      Text += Integer + " " + Name + " = (" + Integer + ")(uintptr_t)(" +
              Operand(Piece.Argument) + "); ";
    }
    Text += "__builtin_memcpy((unsigned char *)&__neverd_record + " +
            std::to_string(Piece.Offset) + ", &" + Name + ", " +
            std::to_string(Piece.Bytes) + "); ";
  }
  return Text + "__neverd_record; })";
}

/// Reproduce the hidden-result write and integer pointer result together.
/// Capturing the pointer once also keeps a source operand with side effects
/// from being evaluated again after the call.
inline std::string recordCallResult(const std::string &Type,
                                    const std::string &Address,
                                    const std::string &Call) {
  return "({ uintptr_t __neverd_result_address = (uintptr_t)(" + Address +
         "); " + Type + " __neverd_result = " + Call +
         "; __builtin_memcpy((void *)__neverd_result_address, "
         "&__neverd_result, sizeof(__neverd_result)); "
         "__neverd_result_address; })";
}

} // namespace neverd::c_detail

#endif // NEVERD_BACKEND_C_CRECORDCALL_H
