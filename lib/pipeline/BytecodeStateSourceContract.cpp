//===- BytecodeStateSourceContract.cpp - Explicit source ABI input --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reject incomplete declarations; execution observations are not contracts.

#include "BytecodeStateSourceContract.h"

#include "neverd/pipeline/Limits.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Errc.h"

#include <map>

namespace neverd {
namespace {
llvm::Error invalid(llvm::StringRef Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "source_contracts: " + Message);
}
bool keys(const llvm::json::Object &O,
          std::initializer_list<llvm::StringRef> Allowed) {
  for (const auto &[K, V] : O)
    if (!llvm::is_contained(Allowed, llvm::StringRef(K)))
      return false;
  return true;
}
std::optional<uint64_t> number(const llvm::json::Object &O,
                               llvm::StringRef Key) {
  auto *V = O.get(Key);
  return V ? V->getAsUINT64() : std::nullopt;
}
llvm::Expected<StateSourceSlot> slot(const llvm::json::Value &V) {
  auto *O = V.getAsObject();
  if (!O || !keys(*O, {"offset", "bytes"}))
    return invalid("invalid state slot");
  auto Offset = O->getInteger("offset"), Bytes = O->getInteger("bytes");
  if (!Offset || !Bytes || *Offset < 0 ||
      *Offset > ::neverd::limits::pipeline::kMaxBytecodeSourceStateOffset ||
      (*Bytes != 1 && *Bytes != 2 && *Bytes != 4 && *Bytes != 8))
    return invalid("invalid slot offset or width");
  return StateSourceSlot{unsigned(*Offset), unsigned(*Bytes)};
}
llvm::Expected<StateSourceType> type(const llvm::json::Value &V) {
  static const std::map<llvm::StringRef, StateSourceType> Types = {
      {"void", StateSourceType::Void}, {"u32", StateSourceType::U32},
      {"u64", StateSourceType::U64},   {"i32", StateSourceType::I32},
      {"i64", StateSourceType::I64},   {"pointer", StateSourceType::Pointer},
      {"u8", StateSourceType::U8},     {"i8", StateSourceType::I8},
      {"u16", StateSourceType::U16},   {"i16", StateSourceType::I16}};
  auto Name = V.getAsString();
  if (!Name || !Types.count(*Name))
    return invalid("unsupported source type");
  return Types.at(*Name);
}
} // namespace
llvm::Expected<StateSourceContract>
readBytecodeStateSourceContract(const llvm::json::Value &V,
                                unsigned StateBytes) {
  auto *Root = V.getAsObject();
  if (!Root ||
      !keys(*Root,
            {"schemaVersion", "state_bytes", "isolated_bank",
             "zero_status_gateways", "localize_bank", "gateways", "images"}) ||
      Root->getInteger("schemaVersion") != 1 ||
      Root->getInteger("state_bytes") != StateBytes ||
      Root->getBoolean("isolated_bank") != true ||
      Root->getBoolean("zero_status_gateways") != true)
    return invalid("explicit isolation/status assertions and matching "
                   "state_bytes are required");
  auto *Gateways = Root->getArray("gateways");
  if (!Gateways ||
      Gateways->size() >
          ::neverd::limits::pipeline::
              kBytecodeStateSourceContractReadBytecodeStateSourceContractGatewaysSizeLimit)
    return invalid("invalid gateways array");
  StateSourceContract Result;
  Result.StateBytes = StateBytes;
  Result.IsolatedBank = true;
  Result.ZeroStatusGateways = true;
  if (const auto *Localize = Root->get("localize_bank")) {
    auto Enabled = Localize->getAsBoolean();
    if (!Enabled)
      return invalid("localize_bank must be an explicit boolean assertion");
    Result.LocalizeBank = *Enabled;
  }
  if (auto *ImagesValue = Root->get("images")) {
    auto *Images = ImagesValue->getAsArray();
    if (!Images || Images->size() > 16)
      return invalid("invalid image candidates");
    for (const auto &Value : *Images) {
      auto *Image = Value.getAsObject();
      if (!Image || !keys(*Image, {"base", "readable", "bytes"}) ||
          !Image->get("base") || Image->getBoolean("readable") != true ||
          !Image->getString("bytes"))
        return invalid("image requires a base, readable extent and hex bytes");
      auto Base = slot(*Image->get("base"));
      if (!Base)
        return Base.takeError();
      auto Hex = *Image->getString("bytes");
      if (Hex.empty() ||
          Hex.size() >
              ::neverd::limits::pipeline::
                  kBytecodeStateSourceContractReadBytecodeStateSourceContractHexSizeLimit ||
          Hex.size() % 2 ||
          !llvm::all_of(Hex, [](char C) { return llvm::isHexDigit(C); }))
        return invalid("invalid image hex bytes");
      StateSourceImage I;
      I.Base = *Base;
      I.Readable = true;
      for (size_t P = 0; P < Hex.size(); P += 2)
        I.Bytes.push_back((llvm::hexDigitValue(Hex[P]) << 4) |
                          llvm::hexDigitValue(Hex[P + 1]));
      Result.Images.push_back(std::move(I));
    }
  }
  for (const auto &Item : *Gateways) {
    auto *O = Item.getAsObject();
    if (!O ||
        !keys(*O, {"name", "selector", "function_pointer", "argument_count",
                   "arguments", "result_offset", "result_bytes", "writes",
                   "imports", "indirect_arguments", "strings",
                   "excluded_arguments", "fallback_writes"}) ||
        !O->getString("name") || !O->getArray("arguments") ||
        !O->getArray("writes") || !O->getArray("imports") ||
        !O->get("result_offset") || !O->get("result_bytes"))
      return invalid("incomplete gateway contract");
    StateSourceGateway G;
    G.Name = O->getString("name")->str();
    if (auto *Value = O->get("fallback_writes")) {
      auto *Effects = Value->getAsObject();
      if (!Effects || !keys(*Effects, {"result_bytes"}))
        return invalid("invalid fallback write assertion");
      G.FallbackWrites.emplace();
      if (auto *Width = Effects->get("result_bytes")) {
        auto Bytes = Width->getAsUINT64();
        if (!Bytes ||
            (*Bytes != 1 && *Bytes != 2 && *Bytes != 4 && *Bytes != 8))
          return invalid("invalid fixed fallback result width");
        G.FallbackWrites->ResultBytes = unsigned(*Bytes);
      }
    }
    for (auto [Name, Destination] :
         {std::pair{"selector", &G.Selector},
          std::pair{"function_pointer", &G.FunctionPointer},
          std::pair{"argument_count", &G.ArgumentCount}})
      if (auto *Value = O->get(Name)) {
        auto S = slot(*Value);
        if (!S)
          return S.takeError();
        *Destination = *S;
      }
    for (auto [Name, Destination] :
         {std::pair{"result_offset", &G.ResultOffset},
          std::pair{"result_bytes", &G.ResultBytes}}) {
      auto S = slot(*O->get(Name));
      if (!S)
        return S.takeError();
      *Destination = *S;
    }
    if (O->getArray("arguments")->size() > 16 ||
        O->getArray("writes")->size() >
            ::neverd::limits::pipeline::
                kBytecodeStateSourceContractReadBytecodeStateSourceContractOGetArrayWritesSizeLimit ||
        O->getArray("imports")->size() >
            ::neverd::limits::pipeline::
                kBytecodeStateSourceContractReadBytecodeStateSourceContractOGetArrayImportsSizeLimit)
      return invalid("gateway description exceeds work limit");
    for (const auto &A : *O->getArray("arguments")) {
      auto S = slot(A);
      if (!S)
        return S.takeError();
      G.Arguments.push_back(*S);
    }
    if (auto *Value = O->get("indirect_arguments")) {
      auto *Indices = Value->getAsArray();
      if (!Indices || Indices->size() > 16)
        return invalid("invalid indirect arguments");
      for (const auto &Index : *Indices) {
        auto I = Index.getAsUINT64();
        if (!I || *I >= G.Arguments.size())
          return invalid("invalid indirect argument index");
        G.IndirectArguments.push_back(unsigned(*I));
      }
    }
    if (auto *Value = O->get("strings")) {
      auto *Strings = Value->getAsArray();
      if (!Strings || Strings->size() > 16)
        return invalid("invalid string arguments");
      for (const auto &Value : *Strings) {
        auto *String = Value.getAsObject();
        if (!String ||
            !keys(*String, {"argument", "image", "by_value", "mode", "xor_key",
                            "encoded_mode"}) ||
            !number(*String, "argument") || !number(*String, "image") ||
            *number(*String, "argument") >= G.Arguments.size() ||
            *number(*String, "image") >= Result.Images.size() ||
            String->getBoolean("by_value") != true)
          return invalid("invalid by-value string declaration");
        StateSourceString S;
        S.Argument = unsigned(*number(*String, "argument"));
        S.Image = unsigned(*number(*String, "image"));
        S.ByValue = true;
        for (auto [Name, Destination] :
             {std::pair{"mode", &S.Mode}, std::pair{"xor_key", &S.XorKey}})
          if (auto *Field = String->get(Name)) {
            auto Slot = slot(*Field);
            if (!Slot)
              return Slot.takeError();
            *Destination = *Slot;
          }
        if (String->get("encoded_mode")) {
          auto Mode = number(*String, "encoded_mode");
          if (!Mode || !S.Mode)
            return invalid("invalid encoded string mode");
          S.EncodedMode = *Mode;
        }
        G.Strings.push_back(S);
      }
    }
    if (auto *Value = O->get("excluded_arguments")) {
      auto *Exclusions = Value->getAsArray();
      if (!Exclusions ||
          Exclusions->size() >
              ::neverd::limits::pipeline::
                  kBytecodeStateSourceContractReadBytecodeStateSourceContractExclusionsSizeLimit)
        return invalid("invalid argument exclusions");
      for (const auto &Value : *Exclusions) {
        auto *Exclusion = Value.getAsObject();
        if (!Exclusion || !keys(*Exclusion, {"argument", "value"}) ||
            !number(*Exclusion, "argument") || !number(*Exclusion, "value") ||
            *number(*Exclusion, "argument") >= G.Arguments.size())
          return invalid("invalid argument exclusion");
        G.ExcludedArguments.push_back(
            {unsigned(*number(*Exclusion, "argument")),
             *number(*Exclusion, "value")});
      }
    }
    for (const auto &W : *O->getArray("writes")) {
      auto *Write = W.getAsObject();
      if (!Write || !keys(*Write, {"slot", "value"}) || !Write->get("slot") ||
          !number(*Write, "value"))
        return invalid("invalid fixed bank write");
      auto S = slot(*Write->get("slot"));
      if (!S)
        return S.takeError();
      G.Writes.push_back({*S, *number(*Write, "value")});
    }
    for (const auto &I : *O->getArray("imports")) {
      auto *Import = I.getAsObject();
      if (!Import ||
          !keys(*Import,
                {"selector", "pointer_source", "name", "parameters", "result",
                 "variadic_fixed", "void_result", "defaults", "target_argument",
                 "pass_context", "void_result_argument"}) ||
          (G.FunctionPointer ? bool(Import->get("selector"))
                             : !number(*Import, "selector")) ||
          (bool(Import->get("name")) == bool(Import->get("target_argument"))) ||
          !Import->getArray("parameters") || !Import->get("result"))
        return invalid("incomplete imported signature");
      StateSourceImport Native;
      Native.Selector = number(*Import, "selector").value_or(0);
      if (auto *Value = Import->get("pass_context")) {
        auto Pass = Value->getAsBoolean();
        if (!Pass)
          return invalid("pass_context requires a boolean");
        Native.PassContext = *Pass;
      }
      if (Import->get("variadic_fixed")) {
        auto Fixed = number(*Import, "variadic_fixed");
        if (!Fixed || !*Fixed || *Fixed > 16)
          return invalid("invalid fixed variadic parameter count");
        Native.VariadicFixedParameters = unsigned(*Fixed);
      }
      if (auto *Value = Import->get("pointer_source")) {
        auto *Source = Value->getAsObject();
        if (!Source || !keys(*Source, {"image", "offset"}) ||
            !number(*Source, "image") || !number(*Source, "offset") ||
            *number(*Source, "image") >= Result.Images.size() ||
            *number(*Source, "offset") >
                ::neverd::limits::pipeline::kMaxBytecodeSourceStateOffset)
          return invalid("invalid imported pointer source");
        Native.PointerSource =
            StateSourcePointerSource{unsigned(*number(*Source, "image")),
                                     unsigned(*number(*Source, "offset"))};
      }
      if (Import->get("target_argument")) {
        auto Index = number(*Import, "target_argument");
        if (!Index || *Index >= 16)
          return invalid("invalid forwarded target argument");
        Native.TargetArgument = unsigned(*Index);
      } else {
        auto Name = Import->getString("name");
        if (!Name)
          return invalid("invalid import name");
        Native.Name = Name->str();
      }
      auto T = type(*Import->get("result"));
      if (!T)
        return T.takeError();
      Native.Result = *T;
      if (auto *Value = Import->get("defaults")) {
        auto *Defaults = Value->getAsArray();
        if (!Defaults || Defaults->size() > 16)
          return invalid("invalid default arguments");
        for (const auto &Default : *Defaults) {
          auto Bits = Default.getAsUINT64();
          if (!Bits)
            return invalid("default argument requires an unsigned bit pattern");
          Native.Defaults.push_back(*Bits);
        }
      }
      if (auto *Value = Import->get("void_result")) {
        auto Policy = Value->getAsString();
        if (Native.Result != StateSourceType::Void || !Policy ||
            (*Policy != "zero" && *Policy != "unspecified"))
          return invalid("invalid void result capture policy");
        if (*Policy == "unspecified")
          Native.VoidResult = StateSourceVoidResult::Unspecified;
      }
      if (auto *Value = Import->get("void_result_argument")) {
        auto Index = Value->getAsUINT64();
        if (!Index || *Index >= 16 || Import->get("void_result"))
          return invalid("invalid void result argument");
        Native.VoidResultArgument = unsigned(*Index);
      }
      if (Import->getArray("parameters")->size() > 16)
        return invalid("too many parameters");
      for (const auto &Parameter : *Import->getArray("parameters")) {
        auto P = type(Parameter);
        if (!P)
          return P.takeError();
        Native.Parameters.push_back(*P);
      }
      G.Imports.push_back(std::move(Native));
    }
    Result.Gateways.push_back(std::move(G));
  }
  return Result;
}
} // namespace neverd
