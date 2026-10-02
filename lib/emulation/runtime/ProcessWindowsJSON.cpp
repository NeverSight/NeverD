//===- ProcessWindowsJSON.cpp - Bounded explicit module inputs -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessWindowsJSON.h"

#include "neverd/emulation/ProcessReportFields.h"
namespace neverd::emulation {
llvm::Expected<WindowsProcessOptions>
windowsOptionsFromJSON(const llvm::json::Value &Value) {
  namespace field = process_report;
  auto Invalid = [](llvm::StringRef Field) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   llvm::Twine(field::WindowsOptions) + Field);
  };
  const auto *Object = Value.getAsObject();
  if (!Object)
    return Invalid(field::Windows);
  WindowsProcessOptions Out;
  for (const auto &[Key, V] : *Object) {
    if (Key != field::Modules)
      return Invalid(Key);
    const auto *Modules = V.getAsArray();
    if (!Modules || Modules->size() > windows_process_limits::Modules)
      return Invalid(Key);
    for (const auto &Item : *Modules) {
      const auto *Module = Item.getAsObject();
      if (!Module || Module->size() != 2)
        return Invalid(Key);
      auto Name = Module->getString(field::Name);
      auto Path = Module->getString(field::Path);
      if (!Name || Name->empty() || Name->contains('\0') ||
          Name->size() >= windows_process_limits::NameBytes || !Path ||
          Path->empty() || Path->contains('\0'))
        return Invalid(Key);
      Out.Modules.push_back(
          {Name->str(), std::filesystem::path(std::u8string(
                            reinterpret_cast<const char8_t *>(Path->data()),
                            Path->size()))});
    }
  }
  return Out;
}
} // namespace neverd::emulation
