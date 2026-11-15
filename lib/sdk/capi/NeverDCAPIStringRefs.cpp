//===- NeverDCAPIStringRefs.cpp - C API: string references ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The instructions that refer to string text: each function's direct
/// references (InstructionFlow.h) meet the image's strings (ImageStrings.h).
///
//===----------------------------------------------------------------------===//

#include "EmbeddedObjects.h"
#include "ImageStrings.h"
#include "InstructionFlow.h"
#include "JSONText.h"
#include "SessionImpl.h"

#include "llvm/Support/JSON.h"

using namespace neverd;
using namespace neverd::sdk;

const char *neverd_string_refs_json(neverd_session_t Sess,
                                    const char *OptionsJson,
                                    neverd_va_t FirstEntry, int MaxFunctions) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  if (S->Img.Arch == Arch::EVM || S->Img.Arch == Arch::SBF) {
    S->setError("string references are only published for native images");
    return nullptr;
  }
  strings::ScanOptions Options;
  if (const auto Error = parseStringOptions(OptionsJson, Options);
      !Error.empty()) {
    S->setError(Error);
    return nullptr;
  }
  if (!S->synchronizeFunctions())
    return nullptr;
  const std::vector<ImageString> &Strings = imageStrings(*S, Options);
  const unsigned MinLength = std::max(1u, Options.MinLength);
  const FunctionPage Page = functionPage(*S, FirstEntry, MaxFunctions);
  struct Row {
    va_t From, To;
    ReferredImageText Text;
    llvm::StringRef Kind;
    std::optional<va_t> Via;
    std::string Instruction;
  };
  // Each function's rows, merged in entry order.
  std::vector<std::vector<Row>> Slots(Page.Functions.size());
  const BinaryImage &Img = S->Img;
  const bool Decoded = decodeFunctionReferences(
      *S, Page,
      [&](size_t Index, va_t Address, llvm::StringRef Instruction,
          const InstructionFlow &Flow, unsigned, InstructionMode) {
        for (const InstructionRef &Ref : Flow.Refs) {
          const va_t To = Ref.To;
          const llvm::StringRef Kind = Ref.Kind;
          if (const auto Text =
                  referredImageText(Img, Strings, MinLength, To)) {
            Slots[Index].push_back(
                {Address, To, *Text, Kind, std::nullopt, Instruction.str()});
            continue;
          }
          // A slot the loader relocated holds a pointer for certain.
          if (!Img.DataPtrRelocSlots.count(To) &&
              !Img.CodePtrRelocSlots.count(To))
            continue;
          if (const auto Target = relocatedPointer(Img, To))
            if (const auto Text =
                    referredImageText(Img, Strings, MinLength, *Target))
              Slots[Index].push_back(
                  {Address, *Target, *Text, Kind, To, Instruction.str()});
        }
      });
  if (!Decoded) {
    S->setError("failed to initialize a decoder for the image");
    return nullptr;
  }
  llvm::json::Array Refs;
  // The outer function cursor does not enumerate contained objects. Publish
  // their references once, on the first page, preserving original coordinates.
  if (FirstEntry == 0) {
    auto Objects = embeddedObjects(*S);
    if (!Objects) {
      S->setError(llvm::toString(Objects.takeError()));
      return nullptr;
    }
    for (const auto &Object : *Objects)
      for (const auto &R : Object.References) {
        const auto From = Img.fileOffsetToVA(Object.Offset + R.From);
        const auto To = Img.fileOffsetToVA(Object.Offset + R.To);
        if (From == InvalidVA || To == InvalidVA)
          continue;
        if (const auto Text = referredImageText(Img, Strings, MinLength, To))
          Refs.push_back(llvm::json::Array{
              vaHex(From), vaHex(To), vaHex(Text->String->Address),
              static_cast<int64_t>(Text->TextOffset), "embedded_offset",
              nullptr, jsonSafeText(embeddedReferenceText(Object, R))});
      }
  }
  for (const auto &Slot : Slots)
    for (const Row &R : Slot)
      Refs.push_back(llvm::json::Array{
          vaHex(R.From), vaHex(R.To), vaHex(R.Text.String->Address),
          static_cast<int64_t>(R.Text.TextOffset), R.Kind.str(),
          R.Via ? llvm::json::Value(vaHex(*R.Via)) : nullptr, R.Instruction});
  llvm::json::Object Result;
  Result["refs"] = std::move(Refs);
  Result["next_entry"] =
      Page.NextEntry ? llvm::json::Value(vaHex(*Page.NextEntry)) : nullptr;
  Result["function_count"] = static_cast<int64_t>(S->Functions.size());
  return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
}
