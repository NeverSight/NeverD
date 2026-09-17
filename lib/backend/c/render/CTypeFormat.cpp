//===- CTypeFormat.cpp - Type to C string formatting ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Type-to-C-string formatting implementation.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/render/CTypeFormat.h"

#include "neverd/Limits.h"
#include "neverd/ir/SourceABI.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Type.h"

#include <cctype>
#include <functional>
#include <stdexcept>
#include <string>

namespace neverd {

std::string escapeCString(llvm::StringRef Str) {
  std::string Result;
  Result.reserve(Str.size());
  for (unsigned char Ch : Str) {
    switch (Ch) {
    case '\n':
      Result += "\\n";
      break;
    case '\t':
      Result += "\\t";
      break;
    case '\r':
      Result += "\\r";
      break;
    case '\\':
      Result += "\\\\";
      break;
    case '"':
      Result += "\\\"";
      break;
    case '\0':
      Result += "\\0";
      break;
    default:
      if (Ch >= 32 && Ch < 127) {
        Result += static_cast<char>(Ch);
      } else {
        Result += "\\x";
        Result += llvm::hexdigit(Ch >> 4);
        Result += llvm::hexdigit(Ch & 0xF);
      }
      break;
    }
  }
  return Result;
}

namespace {
std::string extendedIntegerType(unsigned Bytes, bool Signed) {
  if (Bytes == 32 || Bytes == 64)
    return std::string(Signed ? "int" : "uint") + std::to_string(Bytes * 8) +
           "_t";
  if (Bytes == 0 || Bytes > 16)
    throw std::invalid_argument("C integer exceeds the supported bit width");
  return std::string(Signed ? "" : "unsigned ") + "_BitInt(" +
         std::to_string(Bytes * 8) + ")";
}

bool containsFunction(const TypeRef &Type) {
  auto Current = Type;
  for (unsigned Depth = 0; Current && Depth <= limits::kMaxCPointerNesting;
       ++Depth) {
    if (Current->Kind == NdTypeKind::Func)
      return true;
    if (Current->Kind != NdTypeKind::Ptr)
      return false;
    Current = Current->Pointee;
  }
  if (Current)
    throw std::invalid_argument("C type exceeds the pointer nesting limit");
  return false;
}
} // namespace

std::string declarationToC(const TypeRef &Ty, llvm::StringRef Declarator) {
  if (!containsFunction(Ty))
    return typeToC(Ty) + (Declarator.empty() ? "" : " " + Declarator.str());
  if (!equalSourceTypes(Ty, Ty))
    throw std::invalid_argument("C callback type has no supported signature");
  if (Ty->Kind == NdTypeKind::Ptr) {
    std::string Pointer = "*" + Declarator.str();
    if (Ty->Pointee->Kind == NdTypeKind::Func)
      Pointer = "(" + Pointer + ")";
    return declarationToC(Ty->Pointee, Pointer);
  }
  std::string Function = Declarator.str() + "(";
  for (size_t I = 0; I < Ty->ParamTypes.size(); ++I) {
    if (I)
      Function += ", ";
    Function += typeToC(Ty->ParamTypes[I]);
  }
  if (Ty->ParamTypes.empty())
    Function += "void";
  return declarationToC(Ty->RetType, Function + ")");
}

std::string typeToC(const TypeRef &Ty) {
  if (!Ty)
    return "uint32_t";
  if (containsFunction(Ty))
    return declarationToC(Ty, {});
  switch (Ty->Kind) {
  case NdTypeKind::Void:
    return "void";
  case NdTypeKind::Int:
    if (Ty->IsSigned) {
      switch (Ty->Size) {
      case 1:
        return "int8_t";
      case 2:
        return "int16_t";
      case 4:
        return "int32_t";
      case 8:
        return "int64_t";
      case 16:
        return "__int128";
      default:
        return extendedIntegerType(Ty->Size, true);
      }
    } else {
      switch (Ty->Size) {
      case 1:
        return "uint8_t";
      case 2:
        return "uint16_t";
      case 4:
        return "uint32_t";
      case 8:
        return "uint64_t";
      case 16:
        return "unsigned __int128";
      default:
        return extendedIntegerType(Ty->Size, false);
      }
    }
  case NdTypeKind::Float:
    return Ty->Size == 4 ? "float" : "double";
  case NdTypeKind::Ptr:
    if (!Ty->Pointee || Ty->Pointee->Kind == NdTypeKind::Void)
      return "void*";
    return typeToC(Ty->Pointee) + "*";
  case NdTypeKind::Struct: {
    if (sourceAggregateMembers(Ty).empty())
      throw std::invalid_argument("C record has no supported source layout");
    std::function<std::string(const TypeRef &)> Code = [&](const TypeRef &T) {
      if (T->Kind == NdTypeKind::Float)
        return std::string(T->Size == 4 ? "f" : "d");
      std::string Result = "r" + std::to_string(T->Fields.size());
      for (const auto &Field : T->Fields)
        Result += "_" + Code(Field);
      return Result + "_e";
    };
    return "struct nd_record_" + Code(Ty);
  }
  case NdTypeKind::Array:
    return typeToC(Ty->ElemType) + "*";
  case NdTypeKind::Unknown:
    if (Ty->Size > 0) {
      switch (Ty->Size) {
      case 1:
        return "uint8_t";
      case 2:
        return "uint16_t";
      case 4:
        return "uint32_t";
      case 8:
        return "uint64_t";
      case 16:
        return "unsigned __int128";
      default:
        return extendedIntegerType(Ty->Size, false);
      }
    }
    return "uint32_t";
  default:
    return "uint32_t";
  }
}

std::string llvmStructName(llvm::StructType *ST) {
  if (ST->hasName()) {
    std::string Raw = ST->getName().str();
    std::string Clean;
    for (char Ch : Raw) {
      if (std::isalnum(static_cast<unsigned char>(Ch)) || Ch == '_')
        Clean += Ch;
      else
        Clean += '_';
    }
    if (Clean.empty() || std::isdigit(static_cast<unsigned char>(Clean[0])))
      Clean = "nd_" + Clean;
    return "struct " + Clean;
  }
  return "struct anon_" +
         std::to_string(reinterpret_cast<uintptr_t>(ST) & 0xFFFF);
}

std::string typeToCLLVM(llvm::Type *Ty) {
  if (!Ty)
    return "void";

  if (Ty->isVoidTy())
    return "void";
  if (Ty->isIntegerTy(1))
    return "uint8_t";
  if (Ty->isIntegerTy(8))
    return "uint8_t";
  if (Ty->isIntegerTy(16))
    return "uint16_t";
  if (Ty->isIntegerTy(32))
    return "uint32_t";
  if (Ty->isIntegerTy(64))
    return "uint64_t";
  if (Ty->isIntegerTy()) {
    unsigned Bits = Ty->getIntegerBitWidth();
    if (Bits <= 8)
      return "uint8_t";
    if (Bits <= 16)
      return "uint16_t";
    if (Bits <= 32)
      return "uint32_t";
    if (Bits <= 64)
      return "uint64_t";
    if (Bits <= 128)
      return "__uint128_t";
    return "uint64_t";
  }
  if (Ty->isFloatTy())
    return "float";
  if (Ty->isDoubleTy())
    return "double";
  if (Ty->isPointerTy())
    return "void*";

  if (auto *ST = llvm::dyn_cast<llvm::StructType>(Ty))
    return llvmStructName(ST);

  if (auto *VT = llvm::dyn_cast<llvm::FixedVectorType>(Ty)) {
    unsigned Total = VT->getNumElements() *
                     VT->getElementType()->getPrimitiveSizeInBits() / 8;
    if (Total == 16)
      return "__int128";
    if (Total == 32)
      return "struct { __int128 lo; __int128 hi; }";
    return "uint64_t";
  }

  if (auto *AT = llvm::dyn_cast<llvm::ArrayType>(Ty))
    return typeToCLLVM(AT->getElementType()) + "*";

  if (Ty->isFunctionTy())
    return "void*";

  return "void";
}

llvm::SmallVector<const char *, 3> getArchIntrinsicHeaders(Arch TheArch) {
  if (TheArch == Arch::X86 || TheArch == Arch::X64)
    return getX86IntrinsicHeaders();
  auto Headers = getARMIntrinsicHeaders();
  if (TheArch == Arch::AArch64)
    Headers.push_back("arm_sve.h");
  return Headers;
}

void emitCIndent(llvm::raw_ostream &OS, int Level) {
  for (int I = 0; I < Level; ++I)
    OS << "    ";
}

} // namespace neverd
