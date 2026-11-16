//===- MachOTLSRuntime.cpp - Darwin per-thread initialization -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Assemble a position-independent TLV resolver and its table reference.
//===----------------------------------------------------------------------===//

#include "MachOTLSRuntime.h"

#include "MachOArm64eABI.h"

#include "neverd/backend/codegen/CodeGen.h"

#include "llvm/IR/Module.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Errc.h"

namespace neverd {
namespace {
#define MACHO_TLS_TEXT(Name, Text) constexpr char Name[] = Text;
#include "MachOTLSRuntime.def"
#undef MACHO_TLS_TEXT
} // namespace

llvm::Expected<MachOTLSResolver> buildMachOTLSResolver() {
  std::string Assembly =
      std::string(kResolverPrefix) + kRepair + kResult + kConfigWord;
  llvm::LLVMContext Context;
  llvm::Module Module(kRuntimeName, Context);
  Module.setModuleInlineAsm(Assembly);
  auto Result = Codegen().compile(Module, Arch::AArch64, BinaryFormat::MachO,
                                  macho_arm64e_abi::kTriple);
  if (!Result.Success)
    return llvm::createStringError(llvm::errc::not_supported, kResolverError);
  auto Object =
      llvm::object::ObjectFile::createObjectFile(llvm::MemoryBufferRef(
          llvm::toStringRef(llvm::ArrayRef(Result.ObjectData)), kRuntimeName));
  if (!Object)
    return Object.takeError();
  for (const auto &Section : (*Object)->sections()) {
    if (!Section.isText())
      continue;
    auto Bytes = Section.getContents();
    if (!Bytes)
      return Bytes.takeError();
    constexpr auto Trailer = sizeof(uint64_t);
    if (Bytes->size() <= Trailer ||
        Section.relocation_begin() != Section.relocation_end())
      break;
    return MachOTLSResolver{{Bytes->bytes_begin(), Bytes->bytes_end()},
                            Bytes->size() - Trailer};
  }
  return llvm::createStringError(llvm::errc::not_supported, kResolverError);
}
} // namespace neverd
