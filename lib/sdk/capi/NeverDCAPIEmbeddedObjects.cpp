//===- NeverDCAPIEmbeddedObjects.cpp - Embedded object catalogue ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Publishes validated file coordinates and optional outer-image addresses for
/// embedded ELF metadata, keeping them distinct from native code references.
//===----------------------------------------------------------------------===//

#include "EmbeddedObjects.h"
#include "ImageStrings.h"
#include "JSONText.h"
#include "SessionImpl.h"

#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

using namespace neverd;
using namespace neverd::sdk;

namespace neverd::sdk {
llvm::Expected<llvm::ArrayRef<EmbeddedELFObject>> embeddedObjects(Session &S) {
  if (!S.EmbeddedELFObjects) {
    if (S.BytePatches.empty()) {
      S.EmbeddedELFObjects = scanEmbeddedELF(S.Img.Raw);
    } else {
      // The overlay deliberately leaves Raw as the original file snapshot.
      // Reconstruct just this query's file view from its admitted patch rows.
      auto Bytes = S.Img.Raw;
      for (const auto &[FileOffset, Row] : S.BytePatches.rows()) {
        const uint64_t Offset = FileOffset - S.PatchInput->base();
        if (Offset > Bytes.size() || Row.Current.size() > Bytes.size() - Offset)
          return llvm::createStringError(
              llvm::inconvertibleErrorCode(),
              "patched file bytes are unavailable for embedded ELF scanning");
        std::copy(Row.Current.begin(), Row.Current.end(),
                  Bytes.begin() + Offset);
      }
      S.EmbeddedELFObjects = scanEmbeddedELF(Bytes);
    }
  }
  return llvm::ArrayRef(*S.EmbeddedELFObjects);
}

std::string embeddedReferenceText(const EmbeddedELFObject &Object,
                                  const EmbeddedELFReference &Reference) {
  const auto &From = Object.Sections[Reference.FromSection];
  const auto &To = Object.Sections[Reference.ToSection];
  return "embedded ELF file+" + vaHex(Object.Offset) + " " + From.Name + "+" +
         vaHex(Reference.From - From.Offset) + " -> " + To.Name + "+" +
         vaHex(Reference.To - To.Offset) + " (" +
         llvm::object::getELFRelocationTypeName(Object.Machine, Reference.Type)
             .str() +
         ")";
}
} // namespace neverd::sdk

const char *neverd_embedded_objects_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  auto Objects = embeddedObjects(*S);
  if (!Objects) {
    S->setError(llvm::toString(Objects.takeError()));
    return nullptr;
  }
  const auto &Strings = imageStrings(*S, strings::ScanOptions{});
  const auto address = [](va_t VA) -> llvm::json::Value {
    return VA == InvalidVA ? llvm::json::Value(nullptr)
                           : llvm::json::Value(vaHex(VA));
  };
  std::string Result;
  llvm::raw_string_ostream Stream(Result);
  llvm::json::OStream JSON(Stream);
  JSON.arrayBegin();
  for (const auto &Object : *Objects) {
    llvm::json::Object Row{
        {"file_offset", vaHex(Object.Offset)},
        {"size", vaHex(Object.Size)},
        {"bits", Object.Bits},
        {"big_endian", Object.BigEndian},
        {"machine", Object.Machine},
        {"elf_type", Object.Type},
        {"address", address(S->Img.fileOffsetToVA(Object.Offset))},
        {"relocations", static_cast<int64_t>(Object.Relocations)},
        {"unsupported_relocations",
         static_cast<int64_t>(Object.UnsupportedRelocations)},
        {"unresolved_relocations",
         static_cast<int64_t>(Object.UnresolvedRelocations)},
        {"diagnostic", jsonSafeText(Object.Diagnostic)}};
    JSON.objectBegin();
    for (const auto &[Key, Value] : Row)
      JSON.attribute(Key, Value);
    JSON.attributeBegin("refs");
    JSON.arrayBegin();
    for (const auto &R : Object.References) {
      const auto From = S->Img.fileOffsetToVA(Object.Offset + R.From);
      const auto To = S->Img.fileOffsetToVA(Object.Offset + R.To);
      const auto &Source = Object.Sections[R.FromSection];
      const auto &Target = Object.Sections[R.ToSection];
      llvm::json::Object Ref{
          {"from", address(From)},
          {"to", address(To)},
          {"from_file_offset", vaHex(Object.Offset + R.From)},
          {"to_file_offset", vaHex(Object.Offset + R.To)},
          {"record_file_offset", vaHex(Object.Offset + R.Record)},
          {"source_section", jsonSafeText(Source.Name)},
          {"source_offset", vaHex(R.From - Source.Offset)},
          {"target_section", jsonSafeText(Target.Name)},
          {"target_offset", vaHex(R.To - Target.Offset)},
          {"relocation_type", R.Type},
          {"addend", R.Addend},
          {"symbol", jsonSafeText(R.Symbol)},
          {"description", jsonSafeText(embeddedReferenceText(Object, R))}};
      if (To != InvalidVA)
        if (const auto Text = referredImageText(S->Img, Strings, 1, To)) {
          Ref["string_address"] = vaHex(Text->String->Address);
          Ref["text_offset"] = static_cast<int64_t>(Text->TextOffset);
        }
      JSON.value(std::move(Ref));
    }
    JSON.arrayEnd();
    JSON.attributeEnd();
    JSON.objectEnd();
  }
  JSON.arrayEnd();
  return dupStr(Result);
}
