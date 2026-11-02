//===- X64PagedSource.cpp - Imported paged operand ABI --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One explicit source-only callback signature, shared by both C routes.
//===----------------------------------------------------------------------===//

#include "neverd/analysis/arch/x86_64/X64PagedSource.h"

#include "neverd/lift/X86/X86Regs.h"

#include <utility>

namespace neverd::analysis {
SourceFunctionTypeHint x64PagedAccessSourceABI() {
  SourceFunctionTypeHint ABI;
  ABI.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
  ABI.Architecture = Arch::X64;
  ABI.HasExplicitABI = true;
  ABI.ReturnType = NdType::makeInt(8, false);
  ABI.ReturnLocation = {SourceABICarrierKind::IntegerRegister, x86reg::RAX, 0,
                        8};
  const uint64_t Registers[] = {x86reg::RDI, x86reg::RSI, x86reg::RDX,
                                x86reg::RCX};
  const char *Names[] = {"exchange", "address", "bytes", "flags"};
  for (unsigned I = 0; I != 4; ++I) {
    SourceParameterTypeHint P;
    P.Name = Names[I];
    P.Type = I ? NdType::makeInt(8, false) : NdType::makePtr();
    P.Location = {SourceABICarrierKind::IntegerRegister, Registers[I], 0, 8};
    ABI.Parameters.push_back(std::move(P));
  }
  return ABI;
}
} // namespace neverd::analysis
