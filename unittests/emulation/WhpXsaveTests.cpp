//===- WhpXsaveTests.cpp - WHP complete-state packet protocol ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__)) &&             \
    defined(NEVERD_EMULATION_WHP)
#include "backends/whp/WhpXsaveState.h"
#include "gtest/gtest.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"

#include <cstdlib>
#include <cstring>

namespace neverd::emulation {
namespace {
#define NEVERD_WHP_FAILURE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "WhpHostFailureCases.def"
#undef NEVERD_WHP_FAILURE_TEXT
#define NEVERD_WHP_HOST_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_WHP_HOST_TEXT(Name, Text) constexpr char Name[] = Text;
#define NEVERD_WHP_BOUNDARY_BYTES(Name, ...)                                   \
  constexpr uint8_t Boundary##Name[] = {__VA_ARGS__};
#include "WhpHostFailureCases.def"
#undef NEVERD_WHP_BOUNDARY_BYTES
#undef NEVERD_WHP_HOST_TEXT
#undef NEVERD_WHP_HOST_VALUE
#define NEVERD_FP_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "x86_64/X64FPCases.def"
#undef NEVERD_FP_VALUE
constexpr RegisterValue Payloads[] = {
#define NEVERD_FP_PAYLOAD(Low, High) {Low, High},
#include "x86_64/X64FPCases.def"
#undef NEVERD_FP_PAYLOAD
};
namespace xsavecase {
#define NEVERD_XSAVE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "x86_64/X64XsaveCases.def"
#undef NEVERD_XSAVE_VALUE
} // namespace xsavecase
struct InvalidSize {
  UINT32 Written, Capacity;
  const char *Legacy, *Modern;
};
constexpr InvalidSize InvalidSizes[] = {
#define NEVERD_WHP_XSAVE_SIZE(Written, Capacity, Legacy, Modern)               \
  {Written, Capacity, Legacy, Modern},
#include "WhpHostFailureCases.def"
#undef NEVERD_WHP_XSAVE_SIZE
};
struct FakeXsave {
  WHV_PROCESSOR_XSAVE_FEATURES Features{};
  UINT32 FeatureBytes = sizeof(Features);
  HRESULT FeatureStatus = S_OK;
  std::vector<uint8_t> Packet = std::vector<uint8_t>(x64::fp::XsaveBytes);
  UINT32 Required = Packet.size(), Written = Packet.size();
  unsigned Queries = 0, Installs = 0, Captures = 0;
  unsigned MetadataInstalls = 0, MetadataCaptures = 0;
  inline static constexpr WHV_REGISTER_NAME MetadataNames[] = {
      WHvX64RegisterFpControlStatus, WHvX64RegisterXmmControlStatus};
  std::array<WHV_REGISTER_VALUE, std::size(MetadataNames)> Metadata{};
  HRESULT MetadataInstallStatus = S_OK, MetadataCaptureStatus = S_OK;
  HRESULT QueryStatus = HRESULT(InsufficientBuffer);
  HRESULT InstallStatus = S_OK, CaptureStatus = S_OK;
  static HRESULT WINAPI setRegisters(WHV_PARTITION_HANDLE Handle, UINT32 Index,
                                     const WHV_REGISTER_NAME *Names,
                                     UINT32 Count,
                                     const WHV_REGISTER_VALUE *Values) {
    auto &Self = *static_cast<FakeXsave *>(Handle);
    EXPECT_EQ(Index, 0u);
    EXPECT_EQ(Count, Self.Metadata.size());
    EXPECT_EQ(Names[0], WHvX64RegisterFpControlStatus);
    EXPECT_EQ(Names[1], WHvX64RegisterXmmControlStatus);
    ++Self.MetadataInstalls;
    if (FAILED(Self.MetadataInstallStatus))
      return Self.MetadataInstallStatus;
    std::copy_n(Values, Self.Metadata.size(), Self.Metadata.begin());
    return S_OK;
  }
  static HRESULT WINAPI getRegisters(WHV_PARTITION_HANDLE Handle, UINT32 Index,
                                     const WHV_REGISTER_NAME *Names,
                                     UINT32 Count, WHV_REGISTER_VALUE *Values) {
    auto &Self = *static_cast<FakeXsave *>(Handle);
    EXPECT_EQ(Index, 0u);
    EXPECT_EQ(Count, Self.Metadata.size());
    EXPECT_EQ(Names[0], WHvX64RegisterFpControlStatus);
    EXPECT_EQ(Names[1], WHvX64RegisterXmmControlStatus);
    ++Self.MetadataCaptures;
    if (FAILED(Self.MetadataCaptureStatus))
      return Self.MetadataCaptureStatus;
    std::copy(Self.Metadata.begin(), Self.Metadata.end(), Values);
    return S_OK;
  }
  static HRESULT WINAPI property(WHV_PARTITION_HANDLE Handle,
                                 WHV_PARTITION_PROPERTY_CODE Code, VOID *Bytes,
                                 UINT32 Size, UINT32 *Written) {
    auto &Self = *static_cast<FakeXsave *>(Handle);
    EXPECT_EQ(Code, WHvPartitionPropertyCodeProcessorXsaveFeatures);
    EXPECT_EQ(Size, sizeof(Self.Features));
    if (FAILED(Self.FeatureStatus))
      return Self.FeatureStatus;
    std::memcpy(Bytes, &Self.Features, sizeof(Self.Features));
    *Written = Self.FeatureBytes;
    return S_OK;
  }
  static HRESULT WINAPI get(WHV_PARTITION_HANDLE Handle, UINT32 Index,
                            VOID *Bytes, UINT32 Size, UINT32 *Written) {
    auto &Self = *static_cast<FakeXsave *>(Handle);
    EXPECT_EQ(Index, 0u);
    if (!Size) {
      ++Self.Queries;
      *Written = Self.Required;
      return Self.QueryStatus;
    }
    ++Self.Captures;
    if (FAILED(Self.CaptureStatus))
      return Self.CaptureStatus;
    EXPECT_GE(Size, Self.Packet.size());
    std::memcpy(Bytes, Self.Packet.data(), Self.Packet.size());
    *Written = Self.Written;
    return S_OK;
  }
  static HRESULT WINAPI set(WHV_PARTITION_HANDLE Handle, UINT32 Index,
                            const VOID *Bytes, UINT32 Size) {
    auto &Self = *static_cast<FakeXsave *>(Handle);
    EXPECT_EQ(Index, 0u);
    EXPECT_GE(Size, Self.Packet.size());
    ++Self.Installs;
    if (FAILED(Self.InstallStatus))
      return Self.InstallStatus;
    std::memcpy(Self.Packet.data(), Bytes, Self.Packet.size());
    return S_OK;
  }
  static HRESULT WINAPI getState(WHV_PARTITION_HANDLE Handle, UINT32 Index,
                                 WHV_VIRTUAL_PROCESSOR_STATE_TYPE Type,
                                 VOID *Bytes, UINT32 Size, UINT32 *Written) {
    EXPECT_EQ(Type, WHvVirtualProcessorStateTypeXsaveState);
    return get(Handle, Index, Bytes, Size, Written);
  }
  static HRESULT WINAPI setState(WHV_PARTITION_HANDLE Handle, UINT32 Index,
                                 WHV_VIRTUAL_PROCESSOR_STATE_TYPE Type,
                                 const VOID *Bytes, UINT32 Size) {
    EXPECT_EQ(Type, WHvVirtualProcessorStateTypeXsaveState);
    return set(Handle, Index, Bytes, Size);
  }
};
class WhpXsaveProtocol : public testing::TestWithParam<bool> {
protected:
  WhpAPI API;
  FakeXsave Target;
  WhpXsaveState Transfer;
  X64MachineState State;
  void SetUp() override {
    API.WHvGetVirtualProcessorRegisters = FakeXsave::getRegisters;
    API.WHvSetVirtualProcessorRegisters = FakeXsave::setRegisters;
    API.WHvGetPartitionProperty = FakeXsave::property;
    Target.Features.XsaveSupport = 1;
    Target.Features.AvxSupport = 1;
    if (GetParam()) {
      API.WHvGetVirtualProcessorState = FakeXsave::getState;
      API.WHvSetVirtualProcessorState = FakeXsave::setState;
    } else {
      API.WHvGetVirtualProcessorXsaveState = FakeXsave::get;
      API.WHvSetVirtualProcessorXsaveState = FakeXsave::set;
    }
    for (unsigned I = 0; I < State.FP.Registers.size(); ++I)
      State.FP.Registers[I] = Payloads[I];
    State.FP.Tag = UINT8_MAX;
    State.FP.Control = SeedControl;
    State.FP.Status = SeedStatus;
    State.FP.Opcode = SeedOpcode;
    State.FP.Instruction = SeedInstruction;
    State.FP.Data = SeedData;
    State.MXCSR = SeedMXCSR;
    for (unsigned I = 0; I < State.Xmm.size(); ++I)
      State.Xmm[I] = {SeedXmm + I, ~SeedXmm - I};
  }
  void initialize() {
    ASSERT_EQ(llvm::toString(Transfer.initialize(API, &Target)), "");
  }
};
TEST_P(WhpXsaveProtocol, CompleteCompactedPacketsRetainEveryPhysicalTOP) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  for (unsigned Top = 0; Top < x64::fp::RegisterCount; ++Top) {
    State.FP.Status = SeedStatus | (Top << x64::fp::TopShift);
    ASSERT_EQ(llvm::toString(Transfer.install(API, &Target, State)), "");
    EXPECT_EQ(llvm::support::endian::read64le(Target.Packet.data() +
                                              x64::fp::XCompOffset),
              x64::fp::Compacted | x64::fp::FPAndSSE);
    X64MachineState Next;
    ASSERT_EQ(llvm::toString(Transfer.capture(API, &Target, Next)), "");
    EXPECT_EQ(Next, State);
  }
  EXPECT_EQ(Target.Queries, 1u);
  EXPECT_EQ(Target.Installs, x64::fp::RegisterCount);
  EXPECT_EQ(Target.Captures, x64::fp::RegisterCount);
}
TEST_P(WhpXsaveProtocol, InitialCETComponentsDoNotHideFPState) {
  Target.Packet.resize(xsavecase::NativeBytes);
  Target.Required = Target.Written = Target.Packet.size();
  ASSERT_NO_FATAL_FAILURE(initialize());
  ASSERT_EQ(llvm::toString(Transfer.install(API, &Target, State)), "");
  llvm::support::endian::write64le(Target.Packet.data() +
                                       xsavecase::PresentOffset,
                                   xsavecase::NativePresent);
  llvm::support::endian::write64le(
      Target.Packet.data() + xsavecase::LayoutOffset, xsavecase::NativeLayout);
  X64MachineState Next;
  ASSERT_EQ(llvm::toString(Transfer.capture(API, &Target, Next)), "");
  EXPECT_EQ(Next, State);
  const auto Before = Next;
  Target.Packet.back() = xsavecase::StaleByte;
  auto E = Transfer.capture(API, &Target, Next);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  EXPECT_EQ(Next, Before);
}
TEST_P(WhpXsaveProtocol, InitialWideComponentsDoNotHideFPState) {
  Target.Packet.resize(xsavecase::WideBytes);
  Target.Required = Target.Written = Target.Packet.size();
  ASSERT_NO_FATAL_FAILURE(initialize());
  ASSERT_EQ(llvm::toString(Transfer.install(API, &Target, State)), "");
  llvm::support::endian::write64le(Target.Packet.data() +
                                       xsavecase::PresentOffset,
                                   xsavecase::NativePresent);
  llvm::support::endian::write64le(
      Target.Packet.data() + xsavecase::LayoutOffset, xsavecase::WideLayout);
  X64MachineState Next;
  ASSERT_EQ(llvm::toString(Transfer.capture(API, &Target, Next)), "");
  EXPECT_EQ(Next, State);
  const auto Before = Next;
  Target.Packet[xsavecase::WideCETEnd - 1] = xsavecase::StaleByte;
  auto E = Transfer.capture(API, &Target, Next);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  EXPECT_EQ(Next, Before);
}
TEST_P(WhpXsaveProtocol, NativeInstallRetainsFPStateBeforeAnyGuestExecution) {
  WhpVirtualProcessor Host;
  if (auto E = Host.API.load()) {
    const auto Text = llvm::toString(std::move(E));
    if (!std::getenv(XsaveRequireNative))
      GTEST_SKIP() << Text;
    FAIL() << Text;
  }
  auto &API = Host.API;
  const bool HasAPI = GetParam() ? API.WHvGetVirtualProcessorState &&
                                       API.WHvSetVirtualProcessorState
                                 : API.WHvGetVirtualProcessorXsaveState &&
                                       API.WHvSetVirtualProcessorXsaveState;
  if (!HasAPI) {
    if (!std::getenv(XsaveRequireNative))
      GTEST_SKIP() << diagnostic::WhpCapability;
    FAIL() << diagnostic::WhpCapability;
  }
  WHV_CAPABILITY Capability{};
  const auto Status =
      API.WHvGetCapability(WHvCapabilityCodeHypervisorPresent, &Capability,
                           sizeof(Capability), nullptr);
  if (FAILED(Status) || !Capability.HypervisorPresent) {
    const auto Text = whpFailure(diagnostic::WhpCapability, Status,
                                 whp::operation::WHvGetCapability);
    if (!std::getenv(XsaveRequireNative))
      GTEST_SKIP() << Text;
    FAIL() << Text;
  }
  ASSERT_EQ(API.WHvCreatePartition(&Host.Partition), S_OK);
  WHV_PARTITION_PROPERTY Property{};
  Property.ProcessorCount = 1;
  ASSERT_EQ(API.WHvSetPartitionProperty(Host.Partition,
                                        WHvPartitionPropertyCodeProcessorCount,
                                        &Property, sizeof(Property)),
            S_OK);
  ASSERT_EQ(API.WHvSetupPartition(Host.Partition), S_OK);
  ASSERT_EQ(API.WHvCreateVirtualProcessor(Host.Partition, 0, 0), S_OK);
  const WHV_REGISTER_NAME Names[] = {WHvX64RegisterCr0, WHvX64RegisterCr4,
                                     WHvX64RegisterEfer, WHvX64RegisterXCr0,
                                     WHvX64RegisterCs};
  WHV_REGISTER_VALUE Values[std::size(Names)]{};
  Values[0].Reg64 = x64::CR0;
  Values[1].Reg64 = x64::CR4 | x64::fp::OSXsave;
  Values[2].Reg64 = x64::EFER;
  Values[3].Reg64 = x64::fp::FPAndSSE;
  auto &Code = Values[4].Segment;
  Code.Selector = x64::CodeSelector;
  Code.Limit = x64::SegmentLimit;
  Code.Present = Code.NonSystemSegment = Code.Granularity = Code.Long = 1;
  Code.SegmentType = x64::CodeType;
  ASSERT_EQ(API.WHvSetVirtualProcessorRegisters(Host.Partition, 0, Names,
                                                std::size(Names), Values),
            S_OK);
  if (!GetParam()) {
    API.WHvGetVirtualProcessorState = nullptr;
    API.WHvSetVirtualProcessorState = nullptr;
  }
  WhpXsaveState Native;
  ASSERT_EQ(llvm::toString(Native.initialize(API, Host.Partition)), "");
  ASSERT_EQ(llvm::toString(Native.install(API, Host.Partition, State)), "");
  X64MachineState Actual;
  ASSERT_EQ(llvm::toString(Native.capture(API, Host.Partition, Actual)), "");
#define NEVERD_X64_FP_CONTROL(Name, Member, Type, UC, Offset)                  \
  EXPECT_EQ(Actual.FP.Member, State.FP.Member);
#include "arch/x86_64/X64FPState.def"
#undef NEVERD_X64_FP_CONTROL
  EXPECT_EQ(Actual.FP.Tag, State.FP.Tag);
  EXPECT_EQ(Actual.FP.Registers, State.FP.Registers);
  EXPECT_EQ(Actual.Xmm, State.Xmm);
  EXPECT_EQ(Actual.MXCSR, State.MXCSR);
  // Independently query the named host metadata registers. This distinguishes
  // installation loss from a complete-packet read that omits those fields.
  const WHV_REGISTER_NAME MetadataNames[] = {WHvX64RegisterFpControlStatus,
                                             WHvX64RegisterXmmControlStatus};
  WHV_REGISTER_VALUE Metadata[std::size(MetadataNames)]{};
  ASSERT_EQ(
      API.WHvGetVirtualProcessorRegisters(Host.Partition, 0, MetadataNames,
                                          std::size(MetadataNames), Metadata),
      S_OK);
  EXPECT_EQ(Metadata[0].FpControlStatus.LastFpOp, State.FP.Opcode);
  EXPECT_EQ(Metadata[0].FpControlStatus.LastFpRip, State.FP.Instruction);
  EXPECT_EQ(Metadata[1].XmmControlStatus.LastFpRdp, State.FP.Data);
}
TEST_P(WhpXsaveProtocol, NativeGuestRAMDistinguishesEntryFromCaptureLoss) {
  for (const bool Pending : {false, true})
    for (const bool ExplicitFeature : {false, true})
      for (const bool GuestRestore : {false, true})
        for (const bool DebugExit : {false, true}) {
          auto Expected = State;
          if (Pending) {
            Expected.FP.Control &= ~BoundaryInvalidException;
            Expected.FP.Status |=
                BoundaryPendingStatus | BoundaryInvalidException;
          } else {
            Expected.FP.Control |= x64::fp::ExceptionMask;
            Expected.FP.Status &= ~BoundaryPendingStatus;
          }
          auto Memory =
              llvm::cantFail(MemoryProjection::create(BoundaryRAMLimit));
          ASSERT_EQ(llvm::toString(Memory->map(BoundaryCode, x64::PageSize,
                                               Read | Write | Execute)),
                    "");
          ASSERT_EQ(llvm::toString(
                        Memory->map(BoundaryData, x64::PageSize, Read | Write)),
                    "");
          const auto Program =
              DebugExit ? (GuestRestore ? llvm::ArrayRef(BoundaryRestoreAndTrap)
                                        : llvm::ArrayRef(BoundaryTrap))
                        : (GuestRestore ? llvm::ArrayRef(BoundaryRestoreAndSave)
                                        : llvm::ArrayRef(BoundarySave));
          ASSERT_EQ(llvm::toString(Memory->write(BoundaryCode, Program)), "");
          alignas(x64::fp::RegisterSlotBytes)
              std::array<uint8_t, x64::fp::LegacyBytes>
                  Input{}, HostOutput{}, SavedHost{};
          ASSERT_EQ(llvm::toString(encodeX64FXState(Expected, Input)), "");
          std::error_code EC;
          auto Oracle = llvm::sys::Memory::allocateMappedMemory(
              x64::PageSize, nullptr,
              llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, EC);
          ASSERT_FALSE(bool(EC)) << EC.message();
          auto ReleaseOracle = llvm::scope_exit(
              [&] { (void)llvm::sys::Memory::releaseMappedMemory(Oracle); });
          std::memcpy(Oracle.base(), BoundaryHostRoundTrip,
                      sizeof(BoundaryHostRoundTrip));
          EC = llvm::sys::Memory::protectMappedMemory(
              Oracle, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_EXEC);
          ASSERT_FALSE(bool(EC)) << EC.message();
          llvm::sys::Memory::InvalidateInstructionCache(Oracle.base(),
                                                        Oracle.allocatedSize());
          using HostRoundTrip = void (*)(void *, void *, void *);
          reinterpret_cast<HostRoundTrip>(Oracle.base())(
              Input.data(), HostOutput.data(), SavedHost.data());
          X64MachineState HostState;
          ASSERT_EQ(llvm::toString(decodeX64FXState(HostState, HostOutput)),
                    "");
          ASSERT_EQ(llvm::toString(Memory->write(
                        BoundaryData + BoundaryInputOffset, Input)),
                    "");
          const auto Root = llvm::cantFail(buildX64PageTables(*Memory));
          WhpVirtualProcessor Host;
          if (auto E = Host.API.load()) {
            const auto Text = llvm::toString(std::move(E));
            if (!std::getenv(XsaveRequireNative))
              GTEST_SKIP() << Text;
            FAIL() << Text;
          }
          auto &API = Host.API;
          WHV_CAPABILITY Capability{};
          ASSERT_EQ(API.WHvGetCapability(WHvCapabilityCodeHypervisorPresent,
                                         &Capability, sizeof(Capability),
                                         nullptr),
                    S_OK);
          if (!Capability.HypervisorPresent) {
            if (!std::getenv(XsaveRequireNative))
              GTEST_SKIP() << diagnostic::WhpCapability;
            FAIL() << diagnostic::WhpCapability;
          }
          ASSERT_EQ(API.WHvGetCapability(WHvCapabilityCodeProcessorVendor,
                                         &Capability, sizeof(Capability),
                                         nullptr),
                    S_OK);
          const auto Vendor = Capability.ProcessorVendor;
          ASSERT_EQ(API.WHvGetCapability(WHvCapabilityCodeProcessorFeatures,
                                         &Capability, sizeof(Capability),
                                         nullptr),
                    S_OK);
          const bool HostPointers =
              Capability.ProcessorFeatures.X87PointersSavedSupport;
          ASSERT_EQ(API.WHvCreatePartition(&Host.Partition), S_OK);
          WHV_PARTITION_PROPERTY Property{};
          Property.ProcessorCount = 1;
          ASSERT_EQ(API.WHvSetPartitionProperty(
                        Host.Partition, WHvPartitionPropertyCodeProcessorCount,
                        &Property, sizeof(Property)),
                    S_OK);
          WHV_PROCESSOR_FEATURES Features{};
          UINT32 FeatureBytes = 0;
          ASSERT_EQ(API.WHvGetPartitionProperty(
                        Host.Partition,
                        WHvPartitionPropertyCodeProcessorFeatures, &Features,
                        sizeof(Features), &FeatureBytes),
                    S_OK);
          ASSERT_EQ(FeatureBytes, sizeof(Features));
          SCOPED_TRACE(llvm::formatv(BoundaryTrace, GuestRestore, HostPointers,
                                     bool(Features.X87PointersSavedSupport),
                                     ExplicitFeature, DebugExit, Pending,
                                     unsigned(Vendor), HostState.FP.Opcode,
                                     HostState.FP.Instruction,
                                     HostState.FP.Data)
                           .str());
          if (ExplicitFeature) {
            Features.X87PointersSavedSupport = HostPointers;
            ASSERT_EQ(API.WHvSetPartitionProperty(
                          Host.Partition,
                          WHvPartitionPropertyCodeProcessorFeatures, &Features,
                          sizeof(Features)),
                      S_OK);
          }
          if (DebugExit) {
            Property = {};
            Property.ExtendedVmExits.ExceptionExit = 1;
            ASSERT_EQ(API.WHvSetPartitionProperty(
                          Host.Partition,
                          WHvPartitionPropertyCodeExtendedVmExits, &Property,
                          sizeof(Property)),
                      S_OK);
            Property = {};
            Property.ExceptionExitBitmap = uint64_t(1) << x64::DebugVector;
            ASSERT_EQ(API.WHvSetPartitionProperty(
                          Host.Partition,
                          WHvPartitionPropertyCodeExceptionExitBitmap,
                          &Property, sizeof(Property)),
                      S_OK);
          }
          ASSERT_EQ(API.WHvSetupPartition(Host.Partition), S_OK);
          ASSERT_EQ(llvm::toString(Host.mapMemory(*Memory)), "");
          ASSERT_EQ(API.WHvCreateVirtualProcessor(Host.Partition, 0, 0), S_OK);
          ASSERT_EQ(llvm::toString(Host.initializeRunControl()), "");
          std::vector<WHV_REGISTER_NAME> Names;
          std::vector<WHV_REGISTER_VALUE> Values;
          auto Add = [&](WHV_REGISTER_NAME Name, uint64_t Value) {
            WHV_REGISTER_VALUE V{};
            V.Reg64 = Value;
            Names.push_back(Name);
            Values.push_back(V);
          };
          Add(WHvX64RegisterCr0, x64::CR0);
          Add(WHvX64RegisterCr3, Root);
          Add(WHvX64RegisterCr4, x64::CR4 | x64::fp::OSXsave);
          Add(WHvX64RegisterEfer, x64::EFER);
          Add(WHvX64RegisterXCr0, x64::fp::FPAndSSE);
          Add(WHvX64RegisterRip, BoundaryCode);
          Add(WHvX64RegisterRflags, x64::ReservedFlag);
          Add(WHvX64RegisterRsp, BoundaryData + x64::PageSize);
          Add(WHvX64RegisterRcx, BoundaryData);
          Add(WHvX64RegisterRdx, BoundaryData + BoundaryInputOffset);
          for (const auto Name : {WHvX64RegisterCs, WHvX64RegisterSs,
                                  WHvX64RegisterDs, WHvX64RegisterEs}) {
            WHV_REGISTER_VALUE V{};
            const bool Code = Name == WHvX64RegisterCs;
            V.Segment.Selector = Code ? x64::CodeSelector : x64::DataSelector;
            V.Segment.Limit = x64::SegmentLimit;
            V.Segment.Present = V.Segment.NonSystemSegment =
                V.Segment.Granularity = 1;
            V.Segment.SegmentType = Code ? x64::CodeType : x64::DataType;
            V.Segment.Long = Code;
            V.Segment.Default = !Code;
            Names.push_back(Name);
            Values.push_back(V);
          }
          ASSERT_EQ(
              API.WHvSetVirtualProcessorRegisters(
                  Host.Partition, 0, Names.data(), Names.size(), Values.data()),
              S_OK);
          if (!GetParam()) {
            API.WHvGetVirtualProcessorState = nullptr;
            API.WHvSetVirtualProcessorState = nullptr;
          }
          WhpXsaveState Native;
          ASSERT_EQ(llvm::toString(Native.initialize(API, Host.Partition)), "");
          ASSERT_EQ(
              llvm::toString(Native.install(API, Host.Partition, Expected)),
              "");
          ASSERT_EQ(llvm::toString(Memory->beginRun()), "");
          auto Release = llvm::scope_exit([&] { Memory->endRun(); });
          WHV_RUN_VP_EXIT_CONTEXT Exit{};
          const MachineRunControl Control{
              std::chrono::steady_clock::now() +
              std::chrono::milliseconds(BoundaryTimeoutMilliseconds)};
          ASSERT_EQ(llvm::toString(Host.run(Exit, Control)), "");
          ASSERT_EQ(Exit.ExitReason, DebugExit ? WHvRunVpExitReasonException
                                               : WHvRunVpExitReasonX64Halt);
          if (DebugExit)
            ASSERT_EQ(Exit.VpException.ExceptionType, x64::DebugVector);
          std::array<uint8_t, x64::fp::LegacyBytes> Output{};
          ASSERT_EQ(llvm::toString(Memory->read(BoundaryData, Output)), "");
          X64MachineState Guest, Captured;
          ASSERT_EQ(llvm::toString(decodeX64FXState(Guest, Output)), "");
          ASSERT_EQ(
              llvm::toString(Native.capture(API, Host.Partition, Captured)),
              "");
          auto Compare = [](const X64MachineState &Actual,
                            const X64MachineState &Reference,
                            const char *Boundary, bool Metadata = true) {
            SCOPED_TRACE(Boundary);
            EXPECT_EQ(Actual.FP.Control, Reference.FP.Control);
            EXPECT_EQ(Actual.FP.Status, Reference.FP.Status);
            EXPECT_EQ(Actual.FP.Tag, Reference.FP.Tag);
            EXPECT_EQ(Actual.FP.Registers, Reference.FP.Registers);
            EXPECT_EQ(Actual.Xmm, Reference.Xmm);
            EXPECT_EQ(Actual.MXCSR, Reference.MXCSR);
            if (Metadata) {
              EXPECT_EQ(Actual.FP.Opcode, Reference.FP.Opcode);
              EXPECT_EQ(Actual.FP.Instruction, Reference.FP.Instruction);
              EXPECT_EQ(Actual.FP.Data, Reference.FP.Data);
            }
          };
          // Without an unmasked pending exception, AMD can clear metadata.
          // Every guest field is still checked against independent execution;
          // the pending case additionally proves nonzero metadata retention.
          Compare(HostState, Expected, BoundaryHostState, Pending);
          Compare(Guest, HostState, BoundaryGuestState);
          Compare(Captured, HostState, BoundaryAPIState);
        }
}
TEST_P(WhpXsaveProtocol, NamedMetadataRestoresOmittedPacketFields) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  ASSERT_EQ(llvm::toString(Transfer.install(API, &Target, State)), "");
  llvm::support::endian::write16le(Target.Packet.data() + x64::fp::OpcodeOffset,
                                   0);
  llvm::support::endian::write64le(
      Target.Packet.data() + x64::fp::InstructionOffset, 0);
  llvm::support::endian::write64le(Target.Packet.data() + x64::fp::DataOffset,
                                   0);
  X64MachineState Next;
  ASSERT_EQ(llvm::toString(Transfer.capture(API, &Target, Next)), "");
  EXPECT_EQ(Next, State);
  EXPECT_EQ(Target.MetadataInstalls, 1u);
  EXPECT_EQ(Target.MetadataCaptures, 1u);
}
TEST_P(WhpXsaveProtocol, MetadataFailuresAndConflictsCannotPublishState) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  Target.MetadataInstallStatus = E_FAIL;
  EXPECT_EQ(llvm::toString(Transfer.install(API, &Target, State)),
            MetadataInstall);
  Target.MetadataInstallStatus = S_OK;
  ASSERT_EQ(llvm::toString(Transfer.install(API, &Target, State)), "");
  X64MachineState Next;
  const auto Before = Next;
  Target.MetadataCaptureStatus = E_FAIL;
  EXPECT_EQ(llvm::toString(Transfer.capture(API, &Target, Next)),
            MetadataCapture);
  EXPECT_EQ(Next, Before);
  Target.MetadataCaptureStatus = S_OK;
  --Target.Metadata[0].FpControlStatus.LastFpOp;
  EXPECT_EQ(llvm::toString(Transfer.capture(API, &Target, Next)),
            MetadataOpcodeConflict);
  EXPECT_EQ(Next, Before);
}
TEST_P(WhpXsaveProtocol, InconsistentSharedControlsCannotPublishState) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  ASSERT_EQ(llvm::toString(Transfer.install(API, &Target, State)), "");
  using Change = void (*)(FakeXsave &);
  const Change Changes[] = {
      [](FakeXsave &T) { --T.Metadata[0].FpControlStatus.FpControl; },
      [](FakeXsave &T) { --T.Metadata[0].FpControlStatus.FpStatus; },
      [](FakeXsave &T) { --T.Metadata[0].FpControlStatus.FpTag; },
      [](FakeXsave &T) { --T.Metadata[1].XmmControlStatus.XmmStatusControl; }};
  const auto Original = Target.Metadata;
  for (const auto Mutate : Changes) {
    Target.Metadata = Original;
    Mutate(Target);
    X64MachineState Next;
    const auto Before = Next;
    auto E = Transfer.capture(API, &Target, Next);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    EXPECT_EQ(Next, Before);
  }
}
TEST_P(WhpXsaveProtocol, InvalidQuerySizesCannotAllocateOrInstallState) {
  for (const auto &Case : InvalidSizes) {
    if (Case.Capacity)
      continue;
    Target.Required = Case.Written;
    EXPECT_EQ(llvm::toString(Transfer.initialize(API, &Target)),
              GetParam() ? Case.Modern : Case.Legacy);
  }
  EXPECT_EQ(Target.Installs, 0u);
}
TEST_P(WhpXsaveProtocol, TruncatedAndOversizedCapturesCannotPublishState) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  ASSERT_EQ(llvm::toString(Transfer.install(API, &Target, State)), "");
  for (const auto &Case : InvalidSizes) {
    if (!Case.Capacity)
      continue;
    ASSERT_EQ(Target.Required, Case.Capacity);
    Target.Written = Case.Written;
    auto Next = State;
    EXPECT_EQ(llvm::toString(Transfer.capture(API, &Target, Next)),
              GetParam() ? Case.Modern : Case.Legacy);
    EXPECT_EQ(Next, State);
  }
}
TEST_P(WhpXsaveProtocol, MalformedHostHeaderCannotPublishState) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  ASSERT_EQ(llvm::toString(Transfer.install(API, &Target, State)), "");
  llvm::support::endian::write64le(Target.Packet.data() + x64::fp::XStateOffset,
                                   InvalidPadding);
  auto Next = State;
  EXPECT_EQ(llvm::toString(Transfer.capture(API, &Target, Next)),
            GetParam() ? ModernMalformed : LegacyMalformed);
  EXPECT_EQ(Next, State);
}
TEST_P(WhpXsaveProtocol, InvalidInputReportsPreparationWithoutHostMutation) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  const auto Before = Target.Packet;
  State.MXCSR = InvalidPadding;
  EXPECT_EQ(llvm::toString(Transfer.install(API, &Target, State)),
            GetParam() ? ModernInvalidInput : LegacyInvalidInput);
  EXPECT_EQ(Target.Installs, 0u);
  EXPECT_EQ(Target.Packet, Before);
}
TEST_P(WhpXsaveProtocol, HostFailureRetainsItsStatusAndOriginalState) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  Target.CaptureStatus = E_FAIL;
  auto Next = State;
  EXPECT_EQ(llvm::toString(Transfer.capture(API, &Target, Next)),
            GetParam() ? ModernCapture : LegacyCapture);
  EXPECT_EQ(Next, State);
}
TEST_P(WhpXsaveProtocol, QueryFailureRetainsStatusBeforeAllocatingState) {
  Target.QueryStatus = E_FAIL;
  EXPECT_EQ(llvm::toString(Transfer.initialize(API, &Target)),
            GetParam() ? ModernCapture : LegacyCapture);
  EXPECT_EQ(Target.Queries, 1u);
  EXPECT_EQ(Target.Installs, 0u);
  EXPECT_EQ(Target.Captures, 0u);
}
TEST_P(WhpXsaveProtocol,
       EffectiveFeaturesAreValidatedWithoutNarrowingDefaults) {
  const auto Before = Target.Features.AsUINT64;
  ASSERT_NO_FATAL_FAILURE(initialize());
  EXPECT_EQ(Target.Features.AsUINT64, Before);
  EXPECT_EQ(Target.Queries, 1u);
}
TEST_P(WhpXsaveProtocol, FeatureQueryFailureRetainsStatusBeforeStateQuery) {
  Target.FeatureStatus = E_FAIL;
  EXPECT_EQ(llvm::toString(Transfer.initialize(API, &Target)), FeatureQuery);
  EXPECT_EQ(Target.Queries, 0u);
}
TEST_P(WhpXsaveProtocol, MissingEffectiveXsaveIsTypedUnavailable) {
  Target.Features.XsaveSupport = 0;
  auto E = Transfer.initialize(API, &Target);
  EXPECT_TRUE(E.isA<BackendUnavailableError>());
  llvm::consumeError(std::move(E));
  EXPECT_EQ(Target.Queries, 0u);
}
TEST_P(WhpXsaveProtocol, TruncatedFeatureQueryCannotAllocateState) {
  Target.FeatureBytes -= 1;
  EXPECT_EQ(llvm::toString(Transfer.initialize(API, &Target)),
            diagnostic::WhpState);
  EXPECT_EQ(Target.Queries, 0u);
}
TEST_P(WhpXsaveProtocol, InstallFailureRetainsStatusAndHostPacket) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  Target.InstallStatus = E_FAIL;
  const auto Before = Target.Packet;
  EXPECT_EQ(llvm::toString(Transfer.install(API, &Target, State)),
            GetParam() ? ModernInstall : LegacyInstall);
  EXPECT_EQ(Target.Installs, 1u);
  EXPECT_EQ(Target.Packet, Before);
}
TEST_P(WhpXsaveProtocol, DuplicateInitializationKeepsTheOwnedPacket) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  EXPECT_EQ(llvm::toString(Transfer.initialize(API, &Target)),
            diagnostic::WhpState);
  ASSERT_EQ(llvm::toString(Transfer.install(API, &Target, State)), "");
  X64MachineState Next;
  ASSERT_EQ(llvm::toString(Transfer.capture(API, &Target, Next)), "");
  EXPECT_EQ(Next, State);
}
TEST(WhpXsaveAvailability, MissingCompleteStateAPIIsTypedUnavailable) {
  WhpAPI API;
  WhpXsaveState Transfer;
  auto E = Transfer.initialize(API, nullptr);
  EXPECT_TRUE(E.isA<BackendUnavailableError>());
  llvm::consumeError(std::move(E));
}
INSTANTIATE_TEST_SUITE_P(StateAPIs, WhpXsaveProtocol,
                         testing::Values(false, true));
} // namespace
} // namespace neverd::emulation
#endif
