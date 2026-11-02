//===- ProjectInput.h - Session-owned saved project input ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Isolated NDDB working copies and their stable comparison provenance.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_PROJECTINPUT_H
#define NEVERD_SDK_CAPI_PROJECTINPUT_H

#include "neverd/debug/DebugInfoDiscovery.h"
#include "neverd/support/FileStamp.h"
#include "neverd/support/LoadProgress.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace neverd::sdk {

struct ProjectInput final {
  std::filesystem::path Directory, Source, Input;
  std::string InputName, StateSHA256, DebugChoices;
  ~ProjectInput();
  ProjectInput() = default;
  ProjectInput(const ProjectInput &) = delete;
  ProjectInput &operator=(const ProjectInput &) = delete;
};

/// Empty for a normal binary. A database is validated and extracted into a
/// new private temporary directory. Failure/cancellation removes it; successful
/// session loading transfers ownership. The original project is never unpacked
/// into its normal workbench/recovery directory or written back.
std::shared_ptr<ProjectInput>
prepareProjectInput(const std::filesystem::path &Source,
                    const LoadProgress &Progress);

struct ProjectLoadSettings {
  DebugInfoRequest Debug;
  bool ReadStaticSymbols = true;
  std::vector<std::pair<std::filesystem::path, FileStamp>> ExternalFiles;
};
/// Apply stored debug choices to this load only. External files are not packed;
/// checked files reauthenticate through the normal debug loader, while saved
/// unchecked choices must also retain their original file stamp.
ProjectLoadSettings projectLoadSettings(const ProjectInput &Project,
                                        const DebugInfoRequest &Requested,
                                        bool ReadStaticSymbols,
                                        const LoadProgress &Progress);
void checkProjectDebugFiles(const ProjectLoadSettings &Settings,
                            const LoadProgress &Progress);

} // namespace neverd::sdk

#endif // NEVERD_SDK_CAPI_PROJECTINPUT_H
