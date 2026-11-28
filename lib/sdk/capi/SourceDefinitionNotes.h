//===- SourceDefinitionNotes.h - Refused C definitions ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_SOURCEDEFINITIONNOTES_H
#define NEVERD_SDK_CAPI_SOURCEDEFINITIONNOTES_H

#include "neverd/backend/c/CEmitterOptions.h"

#include "llvm/ADT/StringExtras.h"

#include <string>
#include <vector>

namespace neverd::sdk {

/// Identify a function omitted from a source unit and the reason it was
/// refused.
inline std::string notDecompiledNote(llvm::StringRef Name, va_t Entry,
                                     llvm::StringRef Reason) {
  // Neither a name nor a reason, which may quote one, can end the comment
  // it stands in.
  auto Commented = [](std::string Text) {
    for (size_t At = Text.find("*/"); At != std::string::npos;
         At = Text.find("*/", At))
      Text.replace(At, 2, "* /");
    return Text;
  };
  return "/* neverd.not-decompiled: " + Commented(Name.str()) + " at 0x" +
         llvm::utohexstr(Entry) + ": " + Commented(Reason.str()) + " */\n";
}

/// Render the C writer's refusals in unit order.
inline std::string refusedDefinitionNotes(
    const std::vector<CEmitterOptions::RefusedDefinition> &Refused) {
  std::string Notes;
  for (const auto &Definition : Refused)
    Notes += notDecompiledNote(
        Definition.Name.empty() ? "sub_" + llvm::utohexstr(Definition.Entry)
                                : Definition.Name,
        Definition.Entry, "c-emission-failed; " + Definition.Reason);
  return Notes.empty() ? Notes : Notes + "\n";
}

} // namespace neverd::sdk

#endif
