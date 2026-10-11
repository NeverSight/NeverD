//===- Platform.h - Instruction set and guest system selection --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNPACK_DYNAMIC_PLATFORM_H
#define NEVERD_UNPACK_DYNAMIC_PLATFORM_H

#include "../core/Image.h"

#include "neverd/emulation/ProcessObserver.h"

namespace neverd::unpack {
namespace text {
#define NEVERD_UNPACK_OBSERVATION_TEXT(Name, Text)                             \
  inline constexpr char Name[] = Text;
#include "Observation.def"
#undef NEVERD_UNPACK_OBSERVATION_TEXT
} // namespace text

/// What judging a transfer needs to know about an instruction set.
struct ArchitectureTraits {
  emulation::CPURegister StackPointer;
  /// The longest instruction, in bytes.
  uint64_t InstructionWindow;
};

/// The traits of \p Architecture, or an error that names it.
llvm::Expected<ArchitectureTraits>
architectureTraits(emulation::GuestArchitecture Architecture);
/// The guest process profile that executes \p Image, or an error that names
/// its container and instruction set.
llvm::Expected<emulation::ProcessProfile>
processProfile(const InputImage &Image);

/// Profile-neutral evidence used by the unpacking coordinator. Each owning
/// environment retains its own stop vocabulary and reports actual work.
struct ObservedExecution {
  std::string Profile, Stop, Diagnostic, BackendSelectionReason;
  emulation::ExecutionBackendKind Backend;
  uint64_t PC = 0, Instructions = 0, Events = 0, DirectServiceCalls = 0;
};
llvm::Expected<ObservedExecution>
observeImage(const std::filesystem::path &Path, const InputImage &Image,
             const UnpackOptions &Options,
             emulation::ProcessObserver &Observer);
} // namespace neverd::unpack
#endif
