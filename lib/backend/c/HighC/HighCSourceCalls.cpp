#include "HighCWriter.h"

#include "neverd/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/libc/LibCObjC.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <stdexcept>

namespace neverd {
namespace {
std::string quotedRuntimeName(llvm::StringRef Name) {
  std::string Result = "\"";
  for (unsigned char Byte : Name.bytes()) {
    if (Byte == '\\' || Byte == '"' || Byte == '?') {
      Result += '\\';
      Result += char(Byte);
    } else if (Byte >= 0x20 && Byte <= 0x7e) {
      Result += char(Byte);
    } else {
      // Three octal digits cannot consume a following hex/decimal character,
      // and escaping '?' also prevents translation-phase trigraph surprises.
      Result += '\\';
      Result += char('0' + ((Byte >> 6) & 7));
      Result += char('0' + ((Byte >> 3) & 7));
      Result += char('0' + (Byte & 7));
    }
  }
  return Result + "\"";
}

bool scalar(const TypeRef &Type) {
  if (!Type)
    return false;
  if (Type->Kind == NdTypeKind::Float)
    return Type->Size == 4 || Type->Size == 8;
  if (Type->Kind == NdTypeKind::Ptr)
    return Type->Size == 8 && Type->Pointee;
  return Type->Kind == NdTypeKind::Int && (Type->Size == 1 || Type->Size == 2 ||
                                           Type->Size == 4 || Type->Size == 8);
}

bool integerPair(const TypeRef &Type) {
  return Type && Type->Kind == NdTypeKind::Int && Type->Size == 16;
}

std::string bad(llvm::StringRef Reason) {
  return "(0 /* bad source call: " + Reason.str() + " */)";
}

// HighIR retains scalar machine carriers. A floating register's bits must be
// reinterpreted at a source-call boundary, not numerically converted by C.
std::optional<std::string> sourceValue(llvm::StringRef Text,
                                       const TypeRef &Carrier,
                                       const TypeRef &Source) {
  if (Carrier && Source && Carrier->Kind == NdTypeKind::Struct &&
      Source->Kind == NdTypeKind::Struct && equalSourceTypes(Carrier, Source))
    return Text.str();
  if (integerPair(Carrier) && integerPair(Source))
    return "(" + typeToC(Source) + ")(" + Text.str() + ")";
  if (!scalar(Carrier) || !scalar(Source) || Carrier->Size != Source->Size)
    return std::nullopt;
  const bool CarrierFloat = Carrier->Kind == NdTypeKind::Float;
  const bool SourceFloat = Source->Kind == NdTypeKind::Float;
  if (CarrierFloat != SourceFloat) {
    if (Carrier->Kind == NdTypeKind::Ptr || Source->Kind == NdTypeKind::Ptr)
      return std::nullopt;
    return "__builtin_bit_cast(" + typeToC(Source) + ", (" + typeToC(Carrier) +
           ")(" + Text.str() + "))";
  }
  if (Carrier->Kind == NdTypeKind::Ptr || Source->Kind == NdTypeKind::Ptr)
    return "(" + typeToC(Source) + ")(uintptr_t)(" + Text.str() + ")";
  return "(" + typeToC(Source) + ")(" + Text.str() + ")";
}
} // namespace

llvm::StringRef HighCWriter::sourceConventionAttribute(
    SourceFunctionTypeHint::ConventionKind Convention) {
  switch (Convention) {
  case SourceFunctionTypeHint::ConventionKind::C:
    return "";
  case SourceFunctionTypeHint::ConventionKind::Swift:
    return "__attribute__((swiftcall)) ";
  }
  throw std::invalid_argument("Unsupported source calling convention");
}

std::string
HighCWriter::sourceParameterType(const SourceParameterTypeHint &Parameter) {
  std::string Result = typeToC(Parameter.Type);
  switch (Parameter.TheRole) {
  case SourceParameterTypeHint::Role::Ordinary:
    return Result;
  case SourceParameterTypeHint::Role::SwiftIndirectResult:
    return Result + " __attribute__((swift_indirect_result))";
  case SourceParameterTypeHint::Role::SwiftContext:
    return Result + " __attribute__((swift_context))";
  }
  throw std::invalid_argument("Unsupported source parameter role");
}

const HighFunc *
HighCWriter::sourceCallDefinition(const SourceCallTypeHint &Hint,
                                  llvm::StringRef Name) const {
  if (Hint.TargetAddress) {
    auto It = DefinedFunctionsByAddress.find(Hint.TargetAddress);
    return It == DefinedFunctionsByAddress.end() ? nullptr : It->second;
  }
  auto It = DefinedFuncs.find(Name.str());
  if (It != DefinedFuncs.end())
    return It->second;
  auto Identifier = DefinedFunctionsByIdentifier.find(Name.str());
  return Identifier == DefinedFunctionsByIdentifier.end() ? nullptr
                                                          : Identifier->second;
}

std::string HighCWriter::renderSourceCallExpr(const HighExpr &E) {
  const auto &Hint = *E.SourceCallHint;
  using Kind = SourceCallTypeHint::Kind;
  if (E.IntrinsicId != Intrinsic::None ||
      E.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      E.MemoryOrdering != NdMemoryOrdering::None)
    return bad("incompatible operation effects");
  const auto &Signature = Hint.Signature;
  if (Hint.WeakImport &&
      (Hint.CallKind != Kind::DarwinRuntimeCall ||
       Signature.Origin != SourceFunctionTypeHint::OriginKind::DarwinSDK))
    return bad("weak import belongs to another binding kind");
  if (Hint.ValueWitness && Hint.CallKind != Kind::SwiftValueWitness)
    return bad("value-witness operation belongs to another binding kind");
  if (Hint.CallKind == Kind::SwiftValueWitness) {
    if (!isSwiftValueWitnessSourceCallHint(Hint, Opts.TheArch) ||
        !E.IsIndirectCall || E.CallAddr || !Hint.ValueWitness ||
        E.Operands.size() != Signature.Parameters.size())
      return bad("invalid Swift value-witness binding");
    std::vector<std::string> Arguments;
    for (size_t I = 0; I < E.Operands.size(); ++I) {
      if (!E.Operands[I])
        return bad("missing Swift value-witness argument");
      auto Argument = sourceValue(exprStr(*E.Operands[I]), E.Operands[I]->Type,
                                  Signature.Parameters[I].Type);
      if (!Argument)
        return bad("Swift value-witness argument carrier disagrees with ABI");
      Arguments.push_back(std::move(*Argument));
    }
    const auto Slot = swiftValueWitnessSlot(*Hint.ValueWitness);
    if (!Slot)
      return bad("unsupported Swift value-witness operation");
    std::string Prototype = typeToC(Signature.ReturnType);
    Prototype += " (__attribute__((swiftcall)) *)(";
    for (size_t I = 0; I < Arguments.size(); ++I) {
      if (I)
        Prototype += ", ";
      Prototype += sourceParameterType(Signature.Parameters[I]);
    }
    const std::string Metadata = Arguments.back();
    const std::string Target =
        "((" + Prototype + "))((void **)*(void **)((char *)(uintptr_t)(" +
        Metadata + ") - sizeof(void *)))[" + std::to_string(*Slot) + "])";
    std::string Call = Target + "(";
    for (size_t I = 0; I < Arguments.size(); ++I) {
      if (I)
        Call += ", ";
      Call += Arguments[I];
    }
    Call += ")";
    if (Signature.ReturnType->Kind == NdTypeKind::Void)
      return Call;
    auto Result = sourceValue(Call, Signature.ReturnType, E.Type);
    return Result
               ? *Result
               : bad("Swift value-witness result carrier disagrees with ABI");
  }
  if ((Hint.CallKind == Kind::SwiftStringBridge ||
       Hint.CallKind == Kind::SwiftStringFromNSString) &&
      Signature.Convention != SourceFunctionTypeHint::ConventionKind::Swift)
    return bad("Swift string call has the wrong calling convention");
  if (Hint.CallKind == Kind::NativeAddress ||
      Hint.CallKind == Kind::RuntimeBlockIsa ||
      Hint.CallKind == Kind::RuntimeBlockDescriptor ||
      Hint.CallKind == Kind::RuntimeBlockLiteral ||
      Hint.CallKind == Kind::RuntimeAssociationKey ||
      Hint.CallKind == Kind::RuntimeKVOContext ||
      Hint.CallKind == Kind::RuntimeStaticIdentity ||
      Hint.CallKind == Kind::RuntimeLocalStorageAddress ||
      Hint.CallKind == Kind::RuntimeConstantString ||
      Hint.CallKind == Kind::RuntimeConstantObject ||
      Hint.CallKind == Kind::RuntimeBorrowedBytes ||
      Hint.CallKind == Kind::RuntimeReadOnlyBytes ||
      Hint.CallKind == Kind::RuntimeCStringStorage ||
      Hint.CallKind == Kind::RuntimeConstantObjectTable ||
      Hint.CallKind == Kind::DarwinRuntimeGlobalAddress ||
      Hint.CallKind == Kind::RuntimeProfileCounterStorage) {
    if (!E.Operands.empty() || !Signature.Parameters.empty() ||
        !Signature.ReturnType ||
        Signature.ReturnType->Kind != NdTypeKind::Ptr ||
        Signature.ReturnType->Size != 8)
      return bad("invalid source address declaration");
    std::string Value;
    if (Hint.CallKind == Kind::NativeAddress) {
      const auto *Definition =
          Hint.TargetAddress ? sourceCallDefinition(Hint, {}) : nullptr;
      if (!Definition)
        return bad("native address has no recovered definition");
      if (Definition->SourceTypeHint &&
          Definition->SourceTypeHint->Convention !=
              SourceFunctionTypeHint::ConventionKind::C)
        return bad("unsupported native callback calling convention");
      Value = "&" + functionIdentifier(*Definition);
    } else if (Hint.CallKind == Kind::DarwinRuntimeGlobalAddress) {
      if (Signature.Origin == SourceFunctionTypeHint::OriginKind::DarwinSDK ||
          Signature.Origin ==
              SourceFunctionTypeHint::OriginKind::SwiftRuntime) {
        auto It = SourceRuntimeDataIdentifiers.find(Hint.TargetName);
        if (It == SourceRuntimeDataIdentifiers.end())
          return bad("unknown runtime data identity");
        Value = It->second;
      } else if (Hint.TargetName == "__stack_chk_guard")
        Value = "__stack_chk_guard";
      else
        return bad("unknown runtime data identity");
    } else if (Hint.CallKind == Kind::RuntimeBlockIsa) {
      llvm::StringRef Name(Hint.TargetName);
      if (Name.starts_with("__"))
        Name = Name.drop_front();
      if (Name != "_NSConcreteStackBlock" && Name != "_NSConcreteGlobalBlock")
        return bad("unknown concrete block class");
      Value = Name.str();
    } else if (Hint.CallKind == Kind::RuntimeBorrowedBytes ||
               Hint.CallKind == Kind::RuntimeReadOnlyBytes) {
      if (!Hint.TargetAddress ||
          Hint.ByteCount > limits::kMaxSourceCallBorrowedBytes)
        return bad("borrowed bytes have no bounded source extent");
      Value = "neverd_borrowed_bytes_" +
              llvm::utohexstr(Hint.TargetAddress, true) + "_" +
              std::to_string(Hint.ByteCount) + "_address()";
    } else if (Hint.CallKind == Kind::RuntimeCStringStorage) {
      if (!Hint.TargetAddress || !Hint.ByteCount ||
          Hint.ByteCount > limits::kMaxSourceCallBorrowedBytes)
        return bad("C string storage has no complete bounded source extent");
      Value =
          std::string(Hint.ImmutablePointerSlot ? "neverd_cstring_pointer_"
                                                : "neverd_cstring_storage_") +
          llvm::utohexstr(Hint.ImmutablePointerSlot ? Hint.ImmutablePointerSlot
                                                    : Hint.TargetAddress,
                          true) +
          "_address()";
    } else if (Hint.CallKind == Kind::RuntimeConstantObjectTable) {
      if (!Hint.TargetAddress || !Hint.ByteCount || Hint.ByteCount > 65536 ||
          Hint.ByteCount % 8)
        return bad("constant-object table has no complete bounded extent");
      Value = "neverd_objc_constant_object_table_" +
              llvm::utohexstr(Hint.TargetAddress, true) + "_address()";
    } else if (Hint.CallKind == Kind::RuntimeConstantString ||
               Hint.CallKind == Kind::RuntimeConstantObject) {
      if (!Hint.TargetAddress)
        return bad("constant literal has no object identity");
      Value = std::string(Hint.CallKind == Kind::RuntimeConstantString
                              ? "neverd_objc_constant_string_"
                              : "neverd_objc_constant_object_") +
              llvm::utohexstr(Hint.TargetAddress, true) + "_address()";
    } else if (Hint.CallKind == Kind::RuntimeProfileCounterStorage) {
      if (!Hint.TargetAddress)
        return bad("profile counters have no storage identity");
      Value = "neverd_profile_counters_" +
              llvm::utohexstr(Hint.TargetAddress, true) + "_address()";
    } else if (Hint.CallKind == Kind::RuntimeAssociationKey) {
      if (!Hint.TargetAddress)
        return bad("association key has no source identity");
      Value = "neverd_objc_association_key_" +
              llvm::utohexstr(Hint.TargetAddress, true) + "_address()";
    } else if (Hint.CallKind == Kind::RuntimeKVOContext) {
      if (!Hint.TargetAddress)
        return bad("KVO context has no source identity");
      Value = "neverd_objc_kvo_context_" +
              llvm::utohexstr(Hint.TargetAddress, true) + "_address()";
    } else if (Hint.CallKind == Kind::RuntimeStaticIdentity) {
      if (!Hint.TargetAddress)
        return bad("static identity has no source identity");
      Value = "neverd_static_identity_" +
              llvm::utohexstr(Hint.TargetAddress, true) + "_address()";
    } else if (Hint.CallKind == Kind::RuntimeLocalStorageAddress) {
      if (!Hint.TargetAddress || !Hint.ByteCount)
        return bad("local storage has no source extent");
      Value = "neverd_local_storage_" +
              llvm::utohexstr(Hint.TargetAddress, true) + "_address()";
    } else {
      if (!Hint.TargetAddress)
        return bad("block source address has no runtime identity");
      Value = "neverd_block_" +
              std::string(Hint.CallKind == Kind::RuntimeBlockDescriptor
                              ? "descriptor_"
                              : "literal_") +
              llvm::utohexstr(Hint.TargetAddress, true) + "_address()";
    }
    auto Result = sourceValue(Value, Signature.ReturnType, E.Type);
    return Result ? *Result : bad("incompatible source address carrier");
  }
  const bool RuntimeReference = Hint.CallKind == Kind::RuntimeSelector ||
                                Hint.CallKind == Kind::RuntimeClass ||
                                Hint.CallKind == Kind::RuntimeMetaclass ||
                                Hint.CallKind == Kind::RuntimeIvarOffset ||
                                Hint.CallKind == Kind::RuntimeProtocol;
  if (RuntimeReference) {
    if (!E.Operands.empty() || !Signature.Parameters.empty() ||
        Hint.TargetName.empty() ||
        llvm::StringRef(Hint.TargetName).contains('\0'))
      return bad("invalid runtime reference");
    std::string Value;
    const auto Name = quotedRuntimeName(Hint.TargetName);
    if (Hint.CallKind == Kind::RuntimeIvarOffset) {
      if (Hint.OwnerClass.empty() ||
          llvm::StringRef(Hint.OwnerClass).contains('\0') ||
          !Signature.ReturnType ||
          Signature.ReturnType->Kind != NdTypeKind::Int ||
          (Signature.ReturnType->Size != 4 && Signature.ReturnType->Size != 8))
        return bad("invalid ivar offset identity");
      Value =
          "(" + typeToC(Signature.ReturnType) +
          ")ivar_getOffset(class_getInstanceVariable((Class)objc_getClass(" +
          quotedRuntimeName(Hint.OwnerClass) + "), " + Name + "))";
    } else {
      if (!Signature.ReturnType ||
          Signature.ReturnType->Kind != NdTypeKind::Ptr)
        return bad("runtime reference has a non-pointer type");
      const char *Function =
          Hint.CallKind == Kind::RuntimeSelector   ? "sel_registerName"
          : Hint.CallKind == Kind::RuntimeProtocol ? "objc_getProtocol"
          : Hint.CallKind == Kind::RuntimeClass    ? "objc_getClass"
                                                   : "objc_getMetaClass";
      Value = std::string(Function) + "(" + Name + ")";
    }
    auto Result = sourceValue(Value, Signature.ReturnType, E.Type);
    return Result ? *Result : bad("incompatible runtime result carrier");
  }
  if (Hint.CallKind != Kind::Native && Hint.CallKind != Kind::ObjCMessage &&
      Hint.CallKind != Kind::ObjCSuper2 && Hint.CallKind != Kind::BlockInvoke &&
      Hint.CallKind != Kind::ObjCRuntimeCall &&
      Hint.CallKind != Kind::SwiftRuntimeCall &&
      Hint.CallKind != Kind::SwiftStringBridge &&
      Hint.CallKind != Kind::SwiftStringFromNSString &&
      Hint.CallKind != Kind::DarwinRuntimeCall)
    return bad("unknown binding kind");
  if (Signature.Parameters.size() > 64 ||
      E.Operands.size() != Signature.Parameters.size())
    return bad("argument count disagrees with the source declaration");
  std::string ABIDiagnostic;
  if (Signature.HasExplicitABI && !validateSourceABI(Signature, ABIDiagnostic))
    return bad("invalid source ABI declaration");
  const bool PairResult = integerPair(Signature.ReturnType) &&
                          validateSourceABI(Signature, ABIDiagnostic);
  if (!Signature.ReturnType ||
      (Signature.ReturnType->Kind != NdTypeKind::Void &&
       !scalar(Signature.ReturnType) && !PairResult &&
       sourceAggregateMembers(Signature.ReturnType).empty()))
    return bad("unsupported result type");
  for (const auto &Parameter : Signature.Parameters)
    if (!scalar(Parameter.Type) &&
        sourceAggregateMembers(Parameter.Type).empty())
      return bad("unsupported parameter type");
  const bool Block = Hint.CallKind == Kind::BlockInvoke;
  if (Block &&
      (Signature.Parameters.empty() ||
       Signature.Parameters[0].Type->Kind != NdTypeKind::Ptr ||
       (Signature.Origin != SourceFunctionTypeHint::OriginKind::BlockRuntime &&
        Signature.Origin !=
            SourceFunctionTypeHint::OriginKind::NativeAnalysis)))
    return bad("block invocation has no complete source binding");
  const bool Message =
      Hint.CallKind == Kind::ObjCMessage || Hint.CallKind == Kind::ObjCSuper2;
  if (Hint.Format) {
    const auto &Format = *Hint.Format;
    const bool DeclaredMessage =
        Hint.CallKind == Kind::ObjCMessage &&
        Signature.Origin == SourceFunctionTypeHint::OriginKind::ObjCSDK;
    const bool DeclaredC =
        Hint.CallKind == Kind::DarwinRuntimeCall &&
        Signature.Origin == SourceFunctionTypeHint::OriginKind::DarwinSDK;
    if ((!DeclaredMessage && !DeclaredC) || !Format.FormatAddress ||
        Format.AlternativeFormatAddresses.size() >= 64 ||
        !std::is_sorted(Format.AlternativeFormatAddresses.begin(),
                        Format.AlternativeFormatAddresses.end()) ||
        std::adjacent_find(Format.AlternativeFormatAddresses.begin(),
                           Format.AlternativeFormatAddresses.end()) !=
            Format.AlternativeFormatAddresses.end() ||
        (!Format.AlternativeFormatAddresses.empty() &&
         Format.AlternativeFormatAddresses.front() <= Format.FormatAddress) ||
        Format.FixedCount < (Message ? 3U : 1U) ||
        Format.FixedCount > Signature.Parameters.size() ||
        (Message && Format.FormatParameter < 2) ||
        Format.FormatParameter >= Format.FixedCount)
      return bad("invalid variadic source declaration");
  }
  if (Message && (Signature.Parameters.size() < 2 || Hint.Selector.empty() ||
                  Signature.Parameters[0].Type->Kind != NdTypeKind::Ptr ||
                  Signature.Parameters[1].Type->Kind != NdTypeKind::Ptr))
    return bad("invalid message receiver or selector declaration");

  std::string Name = !E.CallTarget.empty() ? E.CallTarget : Hint.TargetName;
  if (Block) {
    if (!E.Operands[0])
      return bad("missing block receiver expression");
    auto Receiver = sourceValue(exprStr(*E.Operands[0]), E.Operands[0]->Type,
                                Signature.Parameters[0].Type);
    if (!Receiver)
      return bad("incompatible block receiver carrier");
    std::string Prototype = "(^)(";
    for (size_t I = 1; I < Signature.Parameters.size(); ++I) {
      if (I > 1)
        Prototype += ", ";
      Prototype += typeToC(Signature.Parameters[I].Type);
    }
    if (Signature.Parameters.size() == 1)
      Prototype += "void";
    Prototype = declarationToC(Signature.ReturnType, Prototype + ")");
    // The source-bound LowIR proof establishes that the original target was
    // loaded from this same receiver's invoke slot. Clang evaluates the block
    // expression once and supplies its hidden receiver itself.
    Name = "((" + Prototype + ")(" + *Receiver + "))";
  } else if (!Message) {
    const bool Runtime = Hint.CallKind == Kind::ObjCRuntimeCall ||
                         Hint.CallKind == Kind::SwiftRuntimeCall ||
                         Hint.CallKind == Kind::SwiftStringBridge ||
                         Hint.CallKind == Kind::SwiftStringFromNSString ||
                         Hint.CallKind == Kind::DarwinRuntimeCall;
    if (Runtime)
      Name = Hint.TargetName;
    if (Hint.CallKind == Kind::SwiftStringBridge)
      Name = "neverd_swift_string_to_nsstring";
    else if (Hint.CallKind == Kind::SwiftStringFromNSString)
      Name = "neverd_nsstring_to_swift_string";
    else if (Hint.CallKind == Kind::DarwinRuntimeCall &&
             Signature.Origin == SourceFunctionTypeHint::OriginKind::DarwinSDK)
      Name = "neverd_darwin_" + Hint.TargetName;
    const auto *Definition =
        Runtime ? nullptr : sourceCallDefinition(Hint, Name);
    if (!Runtime && Hint.TargetAddress && !Definition &&
        DefinedFuncs.count(Name))
      return bad("native target address disagrees with the source definition");
    if (Definition)
      Name = functionIdentifier(*Definition);
    llvm::StringRef NormalizedName(Name);
    NormalizedName.consume_front("_");
    if (ConflictingSourceNativeSignatures.count(NormalizedName.str()))
      return bad("conflicting native declarations");
    if (Definition) {
      const auto &Function = *Definition;
      const auto Convention = Function.SourceTypeHint
                                  ? Function.SourceTypeHint->Convention
                                  : SourceFunctionTypeHint::ConventionKind::C;
      if (Convention != Signature.Convention ||
          !equalSourceTypes(Function.ReturnType, Signature.ReturnType) ||
          Function.Params.size() != Signature.Parameters.size())
        return bad("native definition disagrees with the call declaration");
      for (size_t I = 0; I < Function.Params.size(); ++I)
        if (!equalSourceTypes(Function.Params[I].Type,
                              Signature.Parameters[I].Type))
          return bad("native parameter disagrees with the call declaration");
    }
    if (Name.empty())
      return bad("native binding has no source name");
    // These bindings carry exact C spellings; the remaining leading
    // underscores belong to the runtime API, not Mach-O decoration.
    const bool ExactRuntimeName =
        Hint.CallKind == Kind::DarwinRuntimeCall &&
        (Name == "__stack_chk_fail" || Name == "_Block_copy" ||
         Name == "_Block_release" || Name == "_Block_object_assign" ||
         Name == "_Block_object_dispose");
    if (!ExactRuntimeName) {
      if (Runtime) {
        const auto It = ExternalFunctionIdentifiers.find(Name);
        Name = It == ExternalFunctionIdentifiers.end()
                   ? functionIdentifier(Name)
                   : It->second;
      } else
        Name = Definition ? functionIdentifier(*Definition)
                          : functionIdentifier(Name);
    }
  } else {
    std::string Prototype = "(*)(";
    const auto FixedCount =
        Hint.Format ? Hint.Format->FixedCount : Signature.Parameters.size();
    for (size_t I = 0; I < FixedCount; ++I) {
      if (I)
        Prototype += ", ";
      if (I == 0)
        Prototype +=
            Hint.CallKind == Kind::ObjCSuper2 ? "struct objc_super *" : "id";
      else if (I == 1)
        Prototype += "SEL";
      else
        Prototype += typeToC(Signature.Parameters[I].Type);
    }
    if (Hint.Format)
      Prototype += ", ...";
    Prototype = declarationToC(Signature.ReturnType, Prototype + ")");
    Name = "((" + Prototype + ")" +
           (Hint.CallKind == Kind::ObjCSuper2 ? "objc_msgSendSuper2"
                                              : "objc_msgSend") +
           ")";
  }
  std::string Call = Name + "(";
  const size_t FirstArgument = Block ? 1 : 0;
  for (size_t I = FirstArgument; I < E.Operands.size(); ++I) {
    if (!E.Operands[I])
      return bad("missing argument expression");
    const auto &Argument = *E.Operands[I];
    TypeRef Carrier = Argument.Type;
    if (Argument.Kind == ExprKind::Var || Argument.Kind == ExprKind::Phi)
      if (auto Declared = declaredParamType(Argument.Var);
          Declared && Declared->Kind == NdTypeKind::Float)
        Carrier = Declared;
    const auto &SourceType = Signature.Parameters[I].Type;
    // A machine register may carry a null pointer through a subregister (for
    // example W0 on AArch64).  Preserve the exact zero as a C null pointer
    // constant instead of requiring the integer carrier to have pointer width.
    // Do not widen any nonzero integer here: those still require an explicit,
    // size-compatible pointer carrier.
    auto Value = Argument.Kind == ExprKind::Const && Carrier &&
                         Carrier->Kind == NdTypeKind::Int &&
                         Argument.ConstVal == 0 && SourceType &&
                         SourceType->Kind == NdTypeKind::Ptr &&
                         Carrier->Size != SourceType->Size
                     ? std::optional<std::string>("(" + typeToC(SourceType) +
                                                  ")0")
                     : sourceValue(exprStr(Argument), Carrier, SourceType);
    if (!Value)
      return bad("argument carrier disagrees with the source declaration");
    if (I != FirstArgument)
      Call += ", ";
    if (Message && I == 0)
      Call +=
          Hint.CallKind == Kind::ObjCSuper2 ? "(struct objc_super *)" : "(id)";
    else if (Message && I == 1)
      Call += "(SEL)";
    else if (I == 0 && Hint.CallKind == Kind::ObjCRuntimeCall &&
             libc::objcHasObjectStorageArgument(Hint.TargetName))
      Call += "(id *)";
    Call += *Value;
  }
  Call += ")";
  if (Signature.ReturnType->Kind == NdTypeKind::Void)
    return Call;
  auto Result = sourceValue(Call, Signature.ReturnType, E.Type);
  return Result ? *Result
                : bad("result carrier disagrees with the source declaration");
}
} // namespace neverd
