//===- KernelFrameworkTests.cpp - KMDF guest identity and lifetime --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Public x64 WDF ABI records and independently observable lifecycle contracts.
/// Layouts: Microsoft Windows-Driver-Frameworks publicinc/wdf/kmdf/1.33 and
/// shared/inc/private/common/fxldr.h at
/// b6191d9543441329154da32f7ab9bdd97228dd3c. Ordering:
/// shared/object/fxobjectstatemachine.cpp and shared/core/fxdriver.cpp. The
/// fake memory supplies bytes and allocation failures, never API semantics.
///
//===----------------------------------------------------------------------===//

#include "KernelFrameworkTestSupport.h"

namespace neverd::emulation {
namespace {
using namespace framework_test;

TEST_F(DriverKernelFramework, VersionOnePublishesDistinctGlobalsAndExactTable) {
  bind();
  EXPECT_TRUE(Model.hasLiveBinding());
  const uint64_t Table = get(TableSlot);
  EXPECT_NE(Table, Globals);
  EXPECT_NE(get(Info + 40), Globals);
  EXPECT_EQ(get(Globals), 0u);
  std::set<uint64_t> Addresses;
  for (unsigned I = 0; I < 458; ++I) {
    const auto Address = get(Table + 8 * I);
    const auto *Export = Exports.lookup(Address);
    ASSERT_NE(Export, nullptr);
    EXPECT_EQ(Export->Binding, Globals);
    EXPECT_EQ(Export->Kind,
              KernelExportRegistry::ExportKind::FrameworkFunction);
    EXPECT_EQ(Export->Module, "wdf01000.sys");
    EXPECT_TRUE(Addresses.insert(Address).second);
  }
  EXPECT_EQ(Exports.lookup(get(Table + 116 * 8))->Name, "WdfDriverCreate");
  success(Model.validateGuestAccess(Globals, 56, false));
  expectError(Model.validateGuestAccess(Globals, 1, true), "read-only");
  expectError(Model.validateGuestAccess(Table, 8, true), "read-only");
  expectError(Model.validateGuestAccess(get(Info + 40), 1, false), "opaque");
}

TEST_F(DriverKernelFramework,
       VersionTwoExactProviderClearsOnlyHigherIndicator) {
  bind(true);
  EXPECT_EQ(get(Higher, 1), 0u);
  EXPECT_EQ(get(FunctionCount, 4), 458u);
  EXPECT_EQ(get(StructureCount, 4), 81u);
  EXPECT_EQ(get(StructureTable), Sentinel);
}

TEST_F(DriverKernelFramework,
       UnsupportedVersionAndCountLeaveBindingUnpublished) {
  for (const auto &[Offset, Value] : std::vector<std::pair<unsigned, unsigned>>{
           {16, 2}, {20, 32}, {28, 457}}) {
    initializeInfo();
    put(Info + Offset, Value, 4);
    expectError(loader("WdfVersionBind", {Driver, Registry, Info, GlobalsSlot}),
                "version or function count");
    EXPECT_FALSE(Model.hasLiveBinding());
    EXPECT_EQ(liveAllocations(), 0u);
    EXPECT_EQ(get(GlobalsSlot), Sentinel);
    EXPECT_EQ(get(TableSlot), Sentinel);
    EXPECT_EQ(get(Info + 40), 0u);
  }
}

TEST_F(DriverKernelFramework, MalformedOutputIsValidatedBeforeAllocating) {
  DenyWriteAt = GlobalsSlot;
  expectError(loader("WdfVersionBind", {Driver, Registry, Info, GlobalsSlot}),
              "write validation");
  EXPECT_EQ(AllocationAttempts, 0u);
  EXPECT_EQ(get(TableSlot), Sentinel);
  EXPECT_EQ(get(Info + 40), 0u);
  DenyWriteAt = 0;
  bind();
}

TEST_F(DriverKernelFramework, AllocationFailureRollsBackUnpublishedBinding) {
  FailAllocation = 2;
  expectError(loader("WdfVersionBind", {Driver, Registry, Info, GlobalsSlot}),
              "allocation");
  EXPECT_FALSE(Model.hasLiveBinding());
  EXPECT_EQ(liveAllocations(), 0u);
  EXPECT_EQ(get(GlobalsSlot), Sentinel);
  EXPECT_EQ(get(TableSlot), Sentinel);
  EXPECT_EQ(get(Info + 40), 0u);
  FailAllocation = 0;
  bind();
}

TEST_F(DriverKernelFramework,
       AliasedBindingOutputsCannotPublishBrokenIdentity) {
  put(Info + 32, GlobalsSlot);
  expectError(loader("WdfVersionBind", {Driver, Registry, Info, GlobalsSlot}),
              "overlap");
  EXPECT_FALSE(Model.hasLiveBinding());
  EXPECT_EQ(liveAllocations(), 0u);
  EXPECT_EQ(get(GlobalsSlot), Sentinel);
}

TEST_F(DriverKernelFramework,
       BindingIdentityRejectsForeignGlobalsAndStaleTable) {
  bind();
  const auto OldEntry = entry("WdfWdmDriverGetWdfDriverHandle");
  expectError(Model.call(OldEntry, {Globals + 8, Driver}, 0), "globals");
  EXPECT_EQ(take(loader("WdfVersionUnbind", {Registry, Info, Globals})), 0u);
  EXPECT_FALSE(Model.hasLiveBinding());
  EXPECT_EQ(liveAllocations(), 0u);
  expectError(Model.call(OldEntry, {Globals, Driver}, 0), "unbound");
  expectError(Model.validateGuestAccess(Globals, 1, false), "freed");
  const auto OldGlobals = Globals;
  bind();
  EXPECT_NE(Globals, OldGlobals);
  EXPECT_NE(entry("WdfWdmDriverGetWdfDriverHandle").Address, OldEntry.Address);
  expectError(Model.call(OldEntry, {Globals, Driver}, 0), "unbound");
}

TEST_F(DriverKernelFramework, UnbindMustMatchRegistryPathBeforeCleaningDriver) {
  bind();
  expectError(loader("WdfVersionUnbind", {Registry + 16, Info, Globals}),
              "registry");
  EXPECT_TRUE(Model.hasLiveBinding());
  createDriver();
  EXPECT_EQ(take(loader("WdfVersionUnbind", {Registry, Info, Globals})), 0u);
  EXPECT_FALSE(Model.takeGuestCall());
  EXPECT_FALSE(Model.hasLiveBinding());
  EXPECT_EQ(liveAllocations(), 0u);
}

TEST_F(DriverKernelFramework, UnbindAcceptsCopiedRegistryPathFromEntryStub) {
  bind();
  const uint64_t Copy = Driver + 0xe00;
  const uint64_t CopyText = Copy + 0x40;
  std::vector<uint8_t> Text(get(Registry, 2));
  success(Memory.read(RegistryText, Text));
  success(Memory.write(CopyText, Text));
  put(Copy, Text.size(), 2);
  put(Copy + 2, Text.size(), 2);
  put(Copy + 8, CopyText);
  put(CopyText + 2, 'X', 2);
  expectError(loader("WdfVersionUnbind", {Copy, Info, Globals}),
              "registry path");
  EXPECT_TRUE(Model.hasLiveBinding());
  success(Memory.write(CopyText, Text));
  EXPECT_EQ(take(loader("WdfVersionUnbind", {Copy, Info, Globals})), 0u);
  EXPECT_FALSE(Model.hasLiveBinding());
  EXPECT_EQ(liveAllocations(), 0u);
}

TEST_F(DriverKernelFramework, DriverConfigurationReturnsDocumentedStatuses) {
  bind();
  put(Config, 31, 4);
  EXPECT_EQ(take(invoke("WdfDriverCreate",
                        {Globals, Driver, Registry, 0, Config, DriverSlot})),
            0xc0000004u);
  put(Config, 32, 4);
  put(Config + 8, ParentCleanup);
  EXPECT_EQ(take(invoke("WdfDriverCreate",
                        {Globals, Driver, Registry, 0, Config, DriverSlot})),
            0xc000000du);
  put(Config + 8, 0);
  const auto Handle = createDriver();
  EXPECT_EQ(get(Globals), Handle);
  EXPECT_EQ(take(invoke("WdfDriverCreate",
                        {Globals, Driver, Registry, 0, Config, DriverSlot})),
            0xc0000183u);
  EXPECT_EQ(get(DriverSlot), Handle);
  EXPECT_EQ(take(invoke("WdfDriverWdmGetDriverObject", {Globals, Handle})),
            Driver);
  EXPECT_EQ(take(invoke("WdfWdmDriverGetWdfDriverHandle", {Globals, Driver})),
            Handle);
  const auto Path = take(invoke("WdfDriverGetRegistryPath", {Globals, Handle}));
  EXPECT_NE(Path, RegistryText);
  EXPECT_EQ(get(Path, 2), '\\');
  expectError(Model.validateGuestAccess(Path, 2, true), "read-only");
}

TEST_F(DriverKernelFramework,
       TypedContextIsZeroedAndDuplicatePreservesIdentity) {
  bind();
  createDriver();
  type();
  type(TypeOther);
  attributes(0, Type);
  const auto Handle = object(Attrs);
  const auto Context =
      take(invoke("WdfObjectGetTypedContextWorker", {Globals, Handle, Type}));
  ASSERT_NE(Context, 0u);
  EXPECT_EQ(get(Context), 0u);
  EXPECT_EQ(get(Context + 16), 0u);
  put(Context, Sentinel);
  EXPECT_EQ(take(invoke("WdfObjectAllocateContext",
                        {Globals, Handle, Attrs, ContextOutput})),
            0x40000000u);
  EXPECT_EQ(get(ContextOutput), Context);
  EXPECT_EQ(get(Context), Sentinel);
  EXPECT_EQ(take(invoke("WdfObjectContextGetObject", {Globals, Context})),
            Handle);
  expectError(invoke("WdfObjectContextGetObject", {Globals, Context + 1}),
              "context pointer");
  EXPECT_EQ(take(invoke("WdfObjectGetTypedContextWorker",
                        {Globals, Handle, TypeOther})),
            0u);
  attributes(0, TypeOther);
  EXPECT_EQ(take(invoke("WdfObjectAllocateContext",
                        {Globals, Handle, Attrs, ContextOutput})),
            0u);
  EXPECT_NE(get(ContextOutput), Context);
}

TEST_F(DriverKernelFramework, ContextIdentityIsTheAlreadyCanonicalAPIPointer) {
  bind();
  createDriver();
  type(Type, TypeOther);
  type(TypeOther, TypeOther);
  attributes(0, Type);
  const auto Handle = object(Attrs);
  EXPECT_NE(
      take(invoke("WdfObjectGetTypedContextWorker", {Globals, Handle, Type})),
      0u);
  EXPECT_EQ(take(invoke("WdfObjectGetTypedContextWorker",
                        {Globals, Handle, TypeOther})),
            0u);
  attributes(0, TypeOther);
  EXPECT_EQ(take(invoke("WdfObjectAllocateContext",
                        {Globals, Handle, Attrs, ContextOutput})),
            0u);
}

TEST_F(DriverKernelFramework, AdditionalContextRequiresAType) {
  bind();
  createDriver();
  const auto Handle = object();
  attributes();
  put(ContextOutput, Sentinel);
  EXPECT_EQ(take(invoke("WdfObjectAllocateContext",
                        {Globals, Handle, Attrs, ContextOutput})),
            0xc0000033u);
  EXPECT_EQ(get(ContextOutput), Sentinel);
}

TEST_F(DriverKernelFramework, CleanupPrecedesDestructionAcrossObjectHierarchy) {
  bind();
  createDriver();
  attributes(0, 0, ParentCleanup, ParentDestroy);
  const auto Parent = object(Attrs);
  attributes(Parent, 0, ChildCleanup, ChildDestroy);
  const auto Child = object(Attrs);
  EXPECT_EQ(take(invoke("WdfObjectDelete", {Globals, Parent})), 0u);
  auto Call = callback();
  EXPECT_EQ(Call.PC, ChildCleanup);
  EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{Child}));
  finish(Call);
  Call = callback();
  EXPECT_EQ(Call.PC, ParentCleanup);
  EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{Parent}));
  finish(Call);
  EXPECT_EQ(drain(), (std::vector<uint64_t>{ChildDestroy, ParentDestroy}));
  expectError(invoke("WdfObjectDelete", {Globals, Child}), "handle");
  expectError(invoke("WdfObjectDelete", {Globals, Parent}), "handle");
}

TEST_F(DriverKernelFramework, ReferenceDelaysDestroyAndContextRemainsReadable) {
  bind();
  createDriver();
  type();
  attributes(0, Type, ChildCleanup, ChildDestroy);
  const auto Handle = object(Attrs);
  const auto Context =
      take(invoke("WdfObjectGetTypedContextWorker", {Globals, Handle, Type}));
  EXPECT_EQ(
      take(invoke("WdfObjectReferenceActual", {Globals, Handle, 0, 0, 0})), 0u);
  EXPECT_EQ(take(invoke("WdfObjectDelete", {Globals, Handle})), 0u);
  EXPECT_EQ(drain(), (std::vector<uint64_t>{ChildCleanup}));
  EXPECT_EQ(take(invoke("WdfObjectAllocateContext",
                        {Globals, Handle, Attrs, ContextOutput})),
            0xc0000056u);
  success(Model.validateGuestAccess(Context, 24, false));
  EXPECT_EQ(
      take(invoke("WdfObjectDereferenceActual", {Globals, Handle, 0, 0, 0})),
      0u);
  auto Call = callback();
  EXPECT_EQ(Call.PC, ChildDestroy);
  EXPECT_EQ(
      take(invoke("WdfObjectGetTypedContextWorker", {Globals, Handle, Type})),
      Context);
  finish(Call);
  EXPECT_FALSE(Model.takeGuestCall());
  expectError(Model.validateGuestAccess(Context, 1, false), "freed");
  expectError(invoke("WdfObjectContextGetObject", {Globals, Context}),
              "context pointer");
}

TEST_F(DriverKernelFramework,
       DereferenceInsideCleanupDoesNotDestroyBeforeReturn) {
  bind();
  createDriver();
  attributes(0, 0, ChildCleanup, ChildDestroy);
  const auto Handle = object(Attrs);
  take(invoke("WdfObjectReferenceActual", {Globals, Handle, 0, 0, 0}));
  take(invoke("WdfObjectDelete", {Globals, Handle}));
  auto Call = callback();
  EXPECT_EQ(Call.PC, ChildCleanup);
  take(invoke("WdfObjectDereferenceActual", {Globals, Handle, 0, 0, 0}));
  EXPECT_FALSE(Model.takeGuestCall());
  EXPECT_FALSE(Released.count(Handle));
  finish(Call);
  EXPECT_EQ(drain(), (std::vector<uint64_t>{ChildDestroy}));
  EXPECT_TRUE(Released.count(Handle));
}

TEST_F(DriverKernelFramework,
       ParentCleanupReleasingChildReferenceWaitsUntilCleanupReturns) {
  bind();
  createDriver();
  attributes(0, 0, ParentCleanup, ParentDestroy);
  const auto Parent = object(Attrs);
  attributes(Parent, 0, ChildCleanup, ChildDestroy);
  const auto Child = object(Attrs);
  take(invoke("WdfObjectReferenceActual", {Globals, Child, 0, 0, 0}));
  take(invoke("WdfObjectDelete", {Globals, Parent}));
  auto Call = callback();
  EXPECT_EQ(Call.PC, ChildCleanup);
  finish(Call);
  Call = callback();
  EXPECT_EQ(Call.PC, ParentCleanup);
  take(invoke("WdfObjectDereferenceActual", {Globals, Child, 0, 0, 0}));
  EXPECT_FALSE(Model.takeGuestCall());
  EXPECT_FALSE(Released.count(Child));
  finish(Call);
  EXPECT_EQ(drain(), (std::vector<uint64_t>{ChildDestroy, ParentDestroy}));
}

TEST_F(DriverKernelFramework,
       UnloadCallbackCanDeleteItsObjectsBeforeDriverCleanup) {
  bind();
  createDriver();
  attributes(0, 0, ChildCleanup, ChildDestroy);
  const auto Child = object(Attrs);
  unload();
  const auto Outer = callback();
  EXPECT_EQ(Outer.PC, UnloadPC);
  EXPECT_EQ(take(invoke("WdfObjectDelete", {Globals, Child})), 0u);
  EXPECT_EQ(drain(), (std::vector<uint64_t>{ChildCleanup, ChildDestroy}));
  finish(Outer);
  EXPECT_TRUE(drain().empty());
  EXPECT_EQ(take(loader("WdfVersionUnbind", {Registry, Info, Globals})), 0u);
  EXPECT_FALSE(Model.hasLiveBinding());
  EXPECT_EQ(liveAllocations(), 0u);
}

TEST_F(DriverKernelFramework, OrdinaryUnloadCleansChildrenAndRetiresBinding) {
  bind(true);
  attributes(0, 0, ParentCleanup, ParentDestroy);
  const auto Handle = createDriver(Attrs);
  attributes(0, 0, ChildCleanup, ChildDestroy);
  object(Attrs);
  unload();
  auto Call = callback();
  EXPECT_EQ(Call.PC, UnloadPC);
  EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{Handle}));
  finish(Call);
  EXPECT_EQ(drain(), (std::vector<uint64_t>{ChildCleanup, ParentCleanup,
                                            ChildDestroy, ParentDestroy}));
  EXPECT_EQ(take(loader("WdfVersionUnbind", {Registry, Info, Globals})), 0u);
  EXPECT_FALSE(Model.hasLiveBinding());
  EXPECT_EQ(liveAllocations(), 0u);
}

TEST_F(DriverKernelFramework, BindAcceptsEntryStubCopiedRegistryDescriptor) {
  const uint64_t Copy = Driver + 0xe00;
  std::vector<uint8_t> Descriptor(16);
  success(Memory.read(Registry, Descriptor));
  success(Memory.write(Copy, Descriptor));
  EXPECT_EQ(take(loader("WdfVersionBind", {Driver, Copy, Info, GlobalsSlot})),
            0u);
  Globals = get(GlobalsSlot);
  EXPECT_NE(Globals, Sentinel);
  EXPECT_TRUE(Model.hasLiveBinding());
  EXPECT_EQ(take(invoke("WdfDriverCreate",
                        {Globals, Driver, Copy, 0, Config, DriverSlot})),
            0u);
  EXPECT_NE(get(DriverSlot), 0u);
}

TEST_F(DriverKernelFramework,
       BindingReservesTableAndUnloadThunkBeforeAllocating) {
  take(Exports.bindImport({0, "WDFLDR.SYS", "WdfVersionBind"}));
  unsigned Index = 0;
  while (Exports.availableThunkCount() > 458)
    take(Exports.bindImport(
        {0, "ntoskrnl.exe", "Capacity" + std::to_string(Index++)}));
  const size_t Remaining = Exports.availableThunkCount();
  expectError(loader("WdfVersionBind", {Driver, Registry, Info, GlobalsSlot}),
              "capacity");
  EXPECT_FALSE(Model.hasLiveBinding());
  EXPECT_EQ(AllocationAttempts, 0u);
  EXPECT_EQ(Exports.availableThunkCount(), Remaining);
  EXPECT_EQ(get(GlobalsSlot), Sentinel);
  EXPECT_EQ(get(TableSlot), Sentinel);
  EXPECT_EQ(get(Info + 40), 0u);
}

TEST_F(DriverKernelFramework,
       AttributeFailuresReturnStatusesWithoutAllocating) {
  bind();
  struct InvalidField {
    uint64_t Address;
    uint64_t Value;
    unsigned Width;
    uint32_t Status;
  };
  const InvalidField Cases[] = {
      {Attrs, 55, 4, 0xc0000004},     {Type, 39, 4, 0xc0000004},
      {Type + 8, 0, 8, 0xc0200209},   {Attrs + 40, 23, 8, 0xc0200209},
      {Attrs + 24, 0, 4, 0xc0200209}, {Attrs + 28, 5, 4, 0xc0200209},
  };
  uint64_t Handle = 0;
  for (unsigned API = 0; API < 3; ++API) {
    if (API == 1) {
      createDriver();
      Handle = object();
    }
    for (const auto &Case : Cases) {
      SCOPED_TRACE(API);
      SCOPED_TRACE(Case.Address);
      type();
      attributes(0, Type);
      put(Case.Address, Case.Value, Case.Width);
      put(Output, Sentinel);
      const size_t Attempts = AllocationAttempts;
      uint64_t Status = 0;
      if (API == 0)
        Status = take(invoke("WdfDriverCreate", {Globals, Driver, Registry,
                                                 Attrs, Config, Output}));
      else if (API == 1)
        Status = take(invoke("WdfObjectCreate", {Globals, Attrs, Output}));
      else
        Status = take(invoke("WdfObjectAllocateContext",
                             {Globals, Handle, Attrs, Output}));
      EXPECT_EQ(Status, Case.Status);
      EXPECT_EQ(get(Output), Sentinel);
      EXPECT_EQ(AllocationAttempts, Attempts);
      EXPECT_FALSE(Model.takeGuestCall());
    }
  }
}

TEST_F(DriverKernelFramework,
       ContextSizeOverrideRequiresATypeAndAllowsExtension) {
  bind();
  createDriver();
  attributes();
  put(Attrs + 40, 32);
  put(Output, Sentinel);
  const size_t Attempts = AllocationAttempts;
  EXPECT_EQ(take(invoke("WdfObjectCreate", {Globals, Attrs, Output})),
            0xc0200209u);
  EXPECT_EQ(get(Output), Sentinel);
  EXPECT_EQ(AllocationAttempts, Attempts);
  type();
  attributes(0, Type);
  put(Attrs + 40, 32);
  const auto Handle = object(Attrs);
  const auto Context =
      take(invoke("WdfObjectGetTypedContextWorker", {Globals, Handle, Type}));
  EXPECT_EQ(Allocations.at(Context), 32u);
  EXPECT_EQ(get(Context + 24), 0u);
}

TEST_F(DriverKernelFramework, DriverAndAdditionalContextRejectExplicitParents) {
  bind();
  attributes(Driver);
  put(Output, Sentinel);
  const size_t Attempts = AllocationAttempts;
  EXPECT_EQ(take(invoke("WdfDriverCreate",
                        {Globals, Driver, Registry, Attrs, Config, Output})),
            0xc020020fu);
  EXPECT_EQ(get(Output), Sentinel);
  EXPECT_EQ(AllocationAttempts, Attempts);
  const auto DriverHandle = createDriver();
  const auto Handle = object();
  type();
  attributes(DriverHandle, Type);
  put(ContextOutput, Sentinel);
  EXPECT_EQ(take(invoke("WdfObjectAllocateContext",
                        {Globals, Handle, Attrs, ContextOutput})),
            0xc020020fu);
  EXPECT_EQ(get(ContextOutput), Sentinel);
  EXPECT_EQ(take(invoke("WdfObjectAllocateContext",
                        {Globals, Handle, 0, ContextOutput})),
            0xc0200212u);
  EXPECT_EQ(get(ContextOutput), Sentinel);
}

TEST_F(DriverKernelFramework,
       LegacyContextDescriptorUsesItsActualPublicExtent) {
  bind();
  createDriver();
  type();
  put(Type, 24, 4);
  attributes(0, Type);
  const auto Handle = object(Attrs);
  EXPECT_NE(
      take(invoke("WdfObjectGetTypedContextWorker", {Globals, Handle, Type})),
      0u);
}

TEST_F(DriverKernelFramework, UnsupportedSchedulingAttributesRemainExplicit) {
  bind();
  createDriver();
  attributes();
  put(Attrs + 24, 3, 4);
  const size_t Attempts = AllocationAttempts;
  expectError(invoke("WdfObjectCreate", {Globals, Attrs, Output}),
              "unsupported framework object execution level");
  attributes();
  put(Attrs + 28, 2, 4);
  expectError(invoke("WdfObjectCreate", {Globals, Attrs, Output}),
              "unsupported framework object synchronization scope");
  EXPECT_EQ(AllocationAttempts, Attempts);
}

TEST_F(DriverKernelFramework, ZeroSizeTypedContextHasOpaqueOwnedIdentity) {
  bind();
  createDriver();
  type();
  put(Type + 16, 0);
  attributes(0, Type);
  const auto Handle = object(Attrs);
  const auto Context =
      take(invoke("WdfObjectGetTypedContextWorker", {Globals, Handle, Type}));
  ASSERT_NE(Context, 0u);
  EXPECT_NE(Context, Handle);
  EXPECT_EQ(take(invoke("WdfObjectContextGetObject", {Globals, Context})),
            Handle);
  const size_t Attempts = AllocationAttempts;
  EXPECT_EQ(take(invoke("WdfObjectAllocateContext",
                        {Globals, Handle, Attrs, ContextOutput})),
            0x40000000u);
  EXPECT_EQ(get(ContextOutput), Context);
  EXPECT_EQ(AllocationAttempts, Attempts);
  expectError(Model.validateGuestAccess(Context, 1, false), "opaque");
  expectError(Model.validateGuestAccess(Context, 1, true), "opaque");
  type(TypeOther);
  EXPECT_EQ(take(invoke("WdfObjectGetTypedContextWorker",
                        {Globals, Handle, TypeOther})),
            0u);
  take(invoke("WdfObjectDelete", {Globals, Handle}));
  EXPECT_TRUE(Released.count(Context));
  expectError(Model.validateGuestAccess(Context, 1, false), "freed");
  expectError(invoke("WdfObjectContextGetObject", {Globals, Context}),
              "context pointer");
}

TEST_F(DriverKernelFramework,
       FailedEntryUnbindCleansObjectsWithoutDriverUnload) {
  bind();
  attributes(0, 0, ParentCleanup, ParentDestroy);
  const auto DriverHandle = createDriver(Attrs);
  attributes(0, 0, ChildCleanup, ChildDestroy);
  const auto Child = object(Attrs);
  const auto StaleEntry = entry("WdfDriverWdmGetDriverObject");
  EXPECT_EQ(take(loader("WdfVersionUnbind", {Registry, Info, Globals})), 0u);
  const std::pair<uint64_t, uint64_t> Expected[] = {
      {ChildCleanup, Child},
      {ParentCleanup, DriverHandle},
      {ChildDestroy, Child},
      {ParentDestroy, DriverHandle},
  };
  for (const auto &[PC, Handle] : Expected) {
    const auto Call = callback();
    EXPECT_EQ(Call.PC, PC);
    EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{Handle}));
    EXPECT_TRUE(Model.hasLiveBinding());
    success(Model.validateGuestAccess(Globals, 56, false));
    finish(Call);
  }
  EXPECT_FALSE(Model.takeGuestCall());
  EXPECT_FALSE(Model.hasLiveBinding());
  EXPECT_EQ(liveAllocations(), 0u);
  expectError(Model.call(StaleEntry, {Globals, DriverHandle}, 0), "unbound");
  expectError(Model.validateGuestAccess(Globals, 1, false), "freed");
}

TEST_F(DriverKernelFramework,
       UnbindKeepsGlobalsAliveWhenExternalReferenceRemains) {
  bind();
  createDriver();
  type();
  attributes(0, Type, ChildCleanup, ChildDestroy);
  const auto Child = object(Attrs);
  const auto Context =
      take(invoke("WdfObjectGetTypedContextWorker", {Globals, Child, Type}));
  take(invoke("WdfObjectReferenceActual", {Globals, Child, 0, 0, 0}));
  EXPECT_EQ(take(loader("WdfVersionUnbind", {Registry, Info, Globals})), 0u);
  const auto Call = callback();
  EXPECT_EQ(Call.PC, ChildCleanup);
  expectError(Model.finishGuestCall(Call.Token, 0), "references");
  EXPECT_FALSE(Model.takeGuestCall());
  EXPECT_TRUE(Model.hasLiveBinding());
  EXPECT_FALSE(Released.count(Globals));
  EXPECT_FALSE(Released.count(get(TableSlot)));
  EXPECT_FALSE(Released.count(get(Info + 40)));
  EXPECT_FALSE(Released.count(Context));
  success(Model.validateGuestAccess(Globals, 56, false));
}

} // namespace
} // namespace neverd::emulation
