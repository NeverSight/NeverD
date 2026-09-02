//===- EntryInputPolicy.cpp - Validate application entry inputs ---------===//

#include "neverd/safety/EntryInputPolicy.h"

#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/safety/SafetyContext.h"

#include <initializer_list>

using namespace neverd;
using namespace neverd::safety;

namespace {

std::optional<uint16_t> supportedPointerWidth(const BinaryImage &Image) {
  switch (Image.Arch) {
  case Arch::X64:
  case Arch::AArch64:
    if (Image.Bits == Bitness::Bits64)
      return uint16_t{8};
    return std::nullopt;
  case Arch::X86:
  case Arch::ARM:
    if (Image.Bits == Bitness::Bits32)
      return uint16_t{4};
    return std::nullopt;
  case Arch::EVM:
  case Arch::SBF:
  case Arch::Unknown:
    return std::nullopt;
  }
  return std::nullopt;
}

bool supportsCallingConvention(Arch TheArch, CallingConv CC) {
  switch (TheArch) {
  case Arch::X64:
    return CC == CallingConv::SysV_AMD64 || CC == CallingConv::Win64;
  case Arch::AArch64:
  case Arch::ARM:
    return CC == CallingConv::ARM_AAPCS;
  case Arch::X86:
    return CC == CallingConv::CDECL;
  case Arch::EVM:
  case Arch::SBF:
  case Arch::Unknown:
    return false;
  }
  return false;
}

bool widthsMatch(const MedFunc &Function,
                 std::initializer_list<uint16_t> Expected) {
  if (Function.Params.size() != Expected.size())
    return false;
  size_t Index = 0;
  for (uint16_t Width : Expected)
    if (Function.Params[Index++].Size != Width)
      return false;
  return true;
}

} // namespace

std::optional<llvm::StringRef>
neverd::safety::applicationEntryParameterSource(const AnalysisInput &Input,
                                                const MedFunc &Function,
                                                size_t ParameterIndex) {
  if (!Input.Img || ParameterIndex >= Function.Params.size())
    return std::nullopt;

  const std::optional<uint16_t> PointerWidth =
      supportedPointerWidth(*Input.Img);
  if (!PointerWidth || !supportsCallingConvention(Input.Img->Arch, Function.CC))
    return std::nullopt;

  const llvm::StringRef Name = stripLeadingUnderscores(Function.Name);
  if (Name == "main" || Name == "wmain") {
    const bool HasArgv = widthsMatch(Function, {4, *PointerWidth});
    const bool HasEnvp =
        widthsMatch(Function, {4, *PointerWidth, *PointerWidth});
    if (!HasArgv && !HasEnvp)
      return std::nullopt;
    switch (ParameterIndex) {
    case 0:
      return llvm::StringRef("argc");
    case 1:
      return llvm::StringRef("argv");
    case 2:
      return llvm::StringRef("envp");
    default:
      return std::nullopt;
    }
  }

  if (Name == "WinMain" || Name == "wWinMain") {
    if (!widthsMatch(Function, {*PointerWidth, *PointerWidth, *PointerWidth,
                                uint16_t{4}}))
      return std::nullopt;
    if (ParameterIndex == 2)
      return llvm::StringRef("command_line");
  }
  return std::nullopt;
}

bool neverd::safety::isApplicationEntryParameterSource(llvm::StringRef Source) {
  return Source == "argc" || Source == "argv" || Source == "envp" ||
         Source == "command_line";
}
