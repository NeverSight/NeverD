//===- LibCRecordArguments.h - C library record parameters -------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The machine operands of a header-declared C library call with records.
/// The arity tables count source parameters, never a record's machine words.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIBC_LIBCRECORDARGUMENTS_H
#define NEVERD_LIBC_LIBCRECORDARGUMENTS_H

#include "neverd/debug/DebugContext.h"
#include "neverd/debug/UserPrototype.h"
#include "neverd/ir/IntegerCallArguments.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/libc/LibCFloatingArguments.h"
#include "neverd/libc/LibCNames.h"

#include <algorithm>

namespace neverd::libc {

inline bool isRecordCallParameter(const FunctionSym &Function, size_t Index,
                                  LibCVariant Variant) {
  if (Index >= Function.Params.size())
    return false;
  const auto &Type = Function.Params[Index].second;
  return Type && Type->Kind == NdTypeKind::Struct && !Type->IsEnum &&
         (Function.PrototypeLibrary == UserPrototypeLibrary ||
          !isObjectPointerParameter(Function.Name, static_cast<unsigned>(Index),
                                    Variant));
}

inline bool hasRecordCallParameters(const FunctionSym &Function,
                                    LibCVariant Variant) {
  for (size_t I = 0; I < Function.Params.size(); ++I)
    if (isRecordCallParameter(Function, I, Variant))
      return true;
  return false;
}

/// A scalar parameter's source index at a flat machine operand. Record pieces
/// have no scalar source parameter type, including an indirect copy carrier.
inline std::optional<unsigned>
recordCallScalarParameter(const FunctionSym &Function,
                          const IntegerCallArguments &Layout, unsigned Argument,
                          LibCVariant Variant) {
  for (unsigned I = 0; I < Layout.Parameters.size(); ++I)
    if (!isRecordCallParameter(Function, I, Variant) &&
        Layout.Parameters[I].Pieces.size() == 1 &&
        Layout.Parameters[I].Pieces[0].Argument == Argument)
      return I;
  return std::nullopt;
}

struct RecordCallArguments : IntegerCallArguments {
  /// The complete dense HFA is returned in the FP register bank, rather
  /// than through ResultPointer. Its source object is reinterpreted as the
  /// checked machine result carrier by the C emitter.
  bool FloatingResult = false;
};

/// Header declarations and checked local FP/HFA declarations retain their
/// original source types while their pieces take the places assigned by
/// SourceParameterPlacement. Other user declarations keep their own call model.
inline std::optional<RecordCallArguments>
recordCallArguments(const FunctionSym &Function, Arch Architecture,
                    BinaryFormat Format, LibCVariant Variant) {
  const auto Floating = floatingCallArguments(Function, Architecture, Format);
  if ((!Function.PrototypeFromHeader && !Function.PrototypeFromCLibrary &&
       !Floating) ||
      !Function.PlatformConvention || Function.IsVariadic ||
      Function.CallConv == DebugCallConv::Fastcall ||
      Function.CallConv == DebugCallConv::Thiscall)
    return std::nullopt;
  std::vector<TypeRef> Types;
  bool HasRecord = false;
  for (size_t I = 0; I < Function.Params.size(); ++I) {
    auto Type = Function.Params[I].second;
    if (!Type)
      return std::nullopt;
    if (isRecordCallParameter(Function, I, Variant)) {
      HasRecord = true;
    } else if (Type->Kind == NdTypeKind::Struct && !Type->IsEnum) {
      Type = NdType::makePtr();
      Type->Size = getTargetRegInfo(Architecture).PointerSize;
    }
    Types.push_back(std::move(Type));
  }
  if (HasRecord)
    if (auto Integer = integerCallArguments(Architecture, Format,
                                            Function.ReturnType, Types))
      return RecordCallArguments{std::move(*Integer), false};
  if (!Floating ||
      (Floating->ParameterElements.empty() && !Floating->ReturnElements))
    return std::nullopt;
  RecordCallArguments Result;
  Result.FloatingResult = Floating->ReturnElements != 0;
  size_t At = 0;
  for (uint16_t Count : Floating->ParameterElements) {
    IntegerCallParameter Parameter;
    uint16_t Offset = 0;
    for (uint16_t I = 0; I < std::max<uint16_t>(Count, 1); ++I, ++At) {
      const auto &Scalar = Floating->Scalars[At];
      Parameter.Pieces.push_back(
          {static_cast<unsigned>(At), Offset, Scalar.Bytes, Scalar.Floating});
      Offset += Scalar.Bytes;
    }
    Result.Parameters.push_back(std::move(Parameter));
  }
  Result.Positions = Floating->Scalars.size();
  return Result;
}

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCRECORDARGUMENTS_H
