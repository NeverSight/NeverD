//===- WhpMachine.cpp - Windows x64 execution----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../arch/x86_64/X64Machine.h"
#include "../../core/ExecutionDiagnostics.h"
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__)) &&             \
    defined(NEVERD_EMULATION_WHP)
#include <vector>
#include <windows.h>
#include <winhvplatform.h>

namespace neverd::emulation {
namespace {
#define NEVERD_WHP_STRING(Name, Text) constexpr auto Name = Text;
#include "WhpProtocol.def"
#undef NEVERD_WHP_STRING
struct WhpAPI {
  HMODULE Module = nullptr;
#define NEVERD_WHP_FUNCTION(Name) decltype(&::Name) Name = nullptr;
#include "WhpProtocol.def"
#undef NEVERD_WHP_FUNCTION
  ~WhpAPI() {
    if (Module)
      FreeLibrary(Module);
  }
  llvm::Error load() {
    Module = LoadLibraryExW(Library, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!Module)
      return diagnostic::unavailable(diagnostic::WhpCapability);
#define NEVERD_WHP_FUNCTION(Name)                                              \
  Name = reinterpret_cast<decltype(Name)>(GetProcAddress(Module, #Name));      \
  if (!Name)                                                                   \
    return diagnostic::unavailable(diagnostic::WhpCapability);
#include "WhpProtocol.def"
#undef NEVERD_WHP_FUNCTION
    return llvm::Error::success();
  }
};
class WhpMachine final : public X64Machine {
public:
  WhpAPI API;
  WHV_PARTITION_HANDLE Partition = nullptr;
  ~WhpMachine() override {
    if (Partition)
      API.WHvDeletePartition(Partition);
  }
  llvm::Error step(X64MachineState &State, uint64_t Root) override {
    std::vector<WHV_REGISTER_NAME> Names;
    std::vector<WHV_REGISTER_VALUE> Values;
    auto Add = [&](WHV_REGISTER_NAME Name, uint64_t Value) {
      WHV_REGISTER_VALUE V{};
      V.Reg64 = Value;
      Names.push_back(Name);
      Values.push_back(V);
    };
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  Add(WHvX64Register##WHP, State.reg(X64Register::Name));
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
    const size_t GeneralCount = Names.size();
    // The admitted ISA excludes all instructions observing or modifying TF.
    Values.back().Reg64 |= x64::TrapFlag;
    Add(WHvX64RegisterCr0, x64::CR0);
    Add(WHvX64RegisterCr3, Root);
    Add(WHvX64RegisterCr4, x64::CR4);
    Add(WHvX64RegisterEfer, x64::EFER);
    Add(WHvX64RegisterCr8, State.reg(X64Register::CR8));
    for (auto Name : {WHvX64RegisterCs, WHvX64RegisterSs, WHvX64RegisterDs,
                      WHvX64RegisterEs, WHvX64RegisterFs, WHvX64RegisterGs}) {
      WHV_REGISTER_VALUE V{};
      const bool Code = Name == WHvX64RegisterCs;
      V.Segment.Selector = Code ? x64::CodeSelector : x64::DataSelector;
      V.Segment.Limit = x64::SegmentLimit;
      V.Segment.Present = V.Segment.NonSystemSegment = V.Segment.Granularity =
          1;
      V.Segment.SegmentType = Code ? x64::CodeType : x64::DataType;
      V.Segment.Long = Code;
      V.Segment.Default = !Code;
      if (Name == WHvX64RegisterGs)
        V.Segment.Base = State.GSBase;
      Names.push_back(Name);
      Values.push_back(V);
    }
    if (FAILED(API.WHvSetVirtualProcessorRegisters(
            Partition, 0, Names.data(), Names.size(), Values.data())))
      return diagnostic::error(diagnostic::WhpState);
    WHV_RUN_VP_EXIT_CONTEXT Exit{};
    if (FAILED(API.WHvRunVirtualProcessor(Partition, 0, &Exit, sizeof(Exit))))
      return diagnostic::error(diagnostic::WhpRun);
    if (Exit.ExitReason != WHvRunVpExitReasonException ||
        Exit.VpException.ExceptionType != x64::DebugVector)
      return diagnostic::error(diagnostic::WhpExit);
    if (FAILED(API.WHvGetVirtualProcessorRegisters(
            Partition, 0, Names.data(), GeneralCount, Values.data())))
      return diagnostic::error(diagnostic::WhpState);
    size_t I = 0;
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  State.reg(X64Register::Name) = Values[I++].Reg64;
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
    State.reg(X64Register::FLAGS) &= ~x64::TrapFlag;
    return llvm::Error::success();
  }
};
} // namespace
llvm::Expected<std::unique_ptr<X64Machine>> createWhpMachine(uint8_t *Backing,
                                                             uint64_t Size) {
  auto M = std::make_unique<WhpMachine>();
  if (auto E = M->API.load())
    return E;
  WHV_CAPABILITY C{};
  if (FAILED(M->API.WHvGetCapability(WHvCapabilityCodeHypervisorPresent, &C,
                                     sizeof(C), nullptr)) ||
      !C.HypervisorPresent)
    return diagnostic::unavailable(diagnostic::WhpCapability);
  if (FAILED(M->API.WHvGetCapability(WHvCapabilityCodeExtendedVmExits, &C,
                                     sizeof(C), nullptr)) ||
      !C.ExtendedVmExits.ExceptionExit)
    return diagnostic::unavailable(diagnostic::WhpCapability);
  if (FAILED(M->API.WHvCreatePartition(&M->Partition)))
    return diagnostic::error(diagnostic::WhpCreate);
  WHV_PARTITION_PROPERTY P{};
  P.ProcessorCount = 1;
  if (FAILED(M->API.WHvSetPartitionProperty(
          M->Partition, WHvPartitionPropertyCodeProcessorCount, &P, sizeof(P))))
    return diagnostic::error(diagnostic::WhpCreate);
  P = {};
  P.ExtendedVmExits.ExceptionExit = 1;
  if (FAILED(M->API.WHvSetPartitionProperty(
          M->Partition, WHvPartitionPropertyCodeExtendedVmExits, &P,
          sizeof(P))))
    return diagnostic::error(diagnostic::WhpCreate);
  P = {};
  P.ExceptionExitBitmap = uint64_t(1) << x64::DebugVector;
  if (FAILED(M->API.WHvSetPartitionProperty(
          M->Partition, WHvPartitionPropertyCodeExceptionExitBitmap, &P,
          sizeof(P))) ||
      FAILED(M->API.WHvSetupPartition(M->Partition)))
    return diagnostic::error(diagnostic::WhpCreate);
  if (FAILED(M->API.WHvMapGpaRange(M->Partition, Backing, 0, Size,
                                   WHvMapGpaRangeFlagRead |
                                       WHvMapGpaRangeFlagWrite |
                                       WHvMapGpaRangeFlagExecute)))
    return diagnostic::error(diagnostic::WhpMap);
  if (FAILED(M->API.WHvCreateVirtualProcessor(M->Partition, 0, 0)))
    return diagnostic::error(diagnostic::WhpCreate);
  return std::unique_ptr<X64Machine>(std::move(M));
}
} // namespace neverd::emulation
#else

namespace neverd::emulation {
llvm::Expected<std::unique_ptr<X64Machine>> createWhpMachine(uint8_t *,
                                                             uint64_t) {
  return diagnostic::unavailable(diagnostic::Unavailable);
}
} // namespace neverd::emulation
#endif
