//===- KernelReferenceThread.cpp - Original System-thread construction ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original System-thread construction.
///

#include "KernelReferenceThread.h"

#include "KernelProcessorTopology.h"
#include "KernelReferenceImage.h"
#include "KernelReferenceInitializer.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
bool hasKernelThreadConstruction(const KernelReferenceImage &Reference) {
  if (Reference.threadInitializationSymbols().empty() || !Reference.debugger())
    return false;
  const auto &F = *Reference.debugger();
  return F.ThreadSize == 0x898 && F.ProcessSize == 0xa40 &&
         F.ThreadIDOffset == 0x480 && F.ThreadProcessIDOffset == 0x478 &&
         F.ThreadProcessOffset == 0x220 && F.ThreadWin32StartOffset == 0x4d0 &&
         F.ProcessIDOffset == 0x440;
}
llvm::Expected<KernelReferenceThreadResult>
constructKernelReferenceThread(const KernelReferenceImage &K,
                               const KernelReferenceThreadInput &Spec) {
  using namespace llvm;
  auto invalid = [](const char *Text) {
    return createStringError(inconvertibleErrorCode(), Text);
  };
  if (!hasKernelThreadConstruction(K))
    return invalid("missing authenticated 19041 thread construction profile");
  if (Spec.Object < 0x30 || Spec.Object % 16 || !Spec.Process ||
      Spec.Process % 16 || !Spec.StartAddress || Spec.ThreadID < 8 ||
      Spec.ThreadID > UINT32_MAX || Spec.ThreadID % 4 || !Spec.StackBase ||
      Spec.StackBase % 4096 || Spec.StackSize < 0x10000 ||
      Spec.StackSize > 0x100000 || Spec.StackSize % 4096 ||
      !Spec.AuxiliaryBase || Spec.AuxiliaryBase % 4096 ||
      Spec.Object > UINT64_MAX - 0x898 || Spec.Process > UINT64_MAX - 0xa40 ||
      Spec.StackBase > UINT64_MAX - Spec.StackSize ||
      Spec.AuxiliaryBase > UINT64_MAX - Spec.AuxiliaryBytes)
    return invalid("invalid thread construction address or identity contract");
  std::vector<std::pair<uint64_t, uint64_t>> Ranges{
      {Spec.Object - 0x30, Spec.Object + 0x898},
      {Spec.Process, Spec.Process + 0xa40},
      {Spec.StackBase, Spec.StackBase + Spec.StackSize},
      {Spec.AuxiliaryBase, Spec.AuxiliaryBase + Spec.AuxiliaryBytes},
      {K.base(), K.base() + K.size()}};
  std::sort(Ranges.begin(), Ranges.end());
  for (unsigned I = 1; I < Ranges.size(); ++I)
    if (Ranges[I].first < Ranges[I - 1].second)
      return invalid("overlapping thread construction owners");
  const auto &S = K.threadInitializationSymbols();
  const uint64_t Thread = Spec.Object, Process = Spec.Process,
                 Start = Spec.StartAddress;
  const uint64_t Bootstrap = Spec.AuxiliaryBase + 0x2000,
                 Type = Spec.AuxiliaryBase + 0x6000,
                 CIDTable = Spec.AuxiliaryBase + 0x7000,
                 Node = Spec.AuxiliaryBase + 0x8000,
                 PRCB = Spec.AuxiliaryBase + 0x9000,
                 Input = Spec.AuxiliaryBase + 0x14000,
                 GS = Spec.AuxiliaryBase + 0x16000,
                 Token = Spec.AuxiliaryBase + 0x18000,
                 TargetStack = Spec.StackBase,
                 BootstrapStack = Spec.AuxiliaryBase + 0x20000;
  KernelInitializerPlan P;
  P.TimestampCounter = 0;
  P.StackBase = Spec.AuxiliaryBase + 0x30000;
  P.StackSize = 65536;
  P.ReturnPC = Spec.AuxiliaryBase + 0x40000;
  P.GSBase = GS;
  P.Writable = {{Thread, 0x898},
                {Process, 0xa40},
                {Bootstrap, 0x898},
                {TargetStack, Spec.StackSize},
                {BootstrapStack, 65536},
                {Input, 4096},
                {S.at("KiProcessListLock"), 8},
                {S.at("KiProcessListHead"), 16}};
  auto zero = [&](uint64_t A, uint64_t N) {
    P.Inputs.push_back({A, std::vector<uint8_t>(N)});
  };
  auto bytes = [](uint64_t A, uint64_t V, unsigned N) {
    KernelInitializerBytes B{A, std::vector<uint8_t>(N)};
    for (unsigned I = 0; I < N; ++I)
      B.Bytes[I] = V >> (I * 8);
    return B;
  };
  auto put = [&](uint64_t A, uint64_t V, unsigned N) {
    P.Inputs.push_back(bytes(A, V, N));
  };
  auto global = [&](const char *Name, uint64_t V, unsigned N) {
    put(S.at(Name), V, N);
  };
  // The private object allocation provider creates one owned pointer reference.
  P.Writable.push_back({Thread - 0x30, 8});
  put(Thread - 0x30, 1, 8);
  put(Process + 0x4b8, (Token | 15),
      8); // Private primary token with fifteen cached fast references.
  global("PspDamExtensionHost", 0, 8);
  global("PspBamExtensionHost", 0, 8);
  put(Process + 0x8c0, 0, 8);
  global("PspNotifyEnableMask", 0,
         4); // No process/thread notification registrations at creation.
  global("SeTokenLeakTracking", 0,
         4); // Explicit private profile: token leak tracing disabled.
  put((Token + 0x40), 0, 8);
  put((Token + 0x48), 0, 8);
  put((Token + 0xc8), 0, 4); // No enabled privileges or extra token flags.
  global("ExpLuidIncrement", 1, 8);
  global("ExpLuid", 0x3e9, 8);
  P.Writable.push_back({S.at("ExpLuid"), 8});
  zero(Type + 0x4c, 16); // Thread generic mapping: no generic bits requested in
                         // this experiment.
  global("ObpTraceFlags", 0,
         4); // Explicit private profile: object tracing disabled.
  // The private execution CPU binds only scalar fields of its kernel loader
  // context. It is never a guest-visible ETHREAD or a second constructed
  // object.
  put(Bootstrap + 0xb8, Process, 8);
  put(Bootstrap + 0x1e4, 0, 4);
  put(Bootstrap + 0xc3, 15, 1);
  put(Bootstrap + 0x184, 2, 1);
  put(Bootstrap + 0x232, 0, 1);
  put(Bootstrap + 0x318, 0, 4); // Empty AutoBoost allocation and entry state.
  put(Bootstrap + 0x74, 0x400, 4);
  put(Bootstrap + 0x510, 0, 4);
  zero(Input, 4096);
  put(Process + 0x8e8, 0,
      8); // No process energy-accounting context in the bounded profile.
  put(Process + 0x510, 0, 8);
  put(Process + 0x3e0, 0, 8);
  put(Process + 0x87c, 0, 4);
  put(Process + 0x3d8, 0, 8);
  put(Process + 0x860, 0, 8);
  put(Process + 0x858, 0, 4);
  put(Process + 0x460, 0, 4);
  put(Process + 0x438, 0, 8);
  put(Process + 0x464, 0x4000000, 4);
  put(Process + 0x5e0, Process + 0x5e0, 8);
  put(Process + 0x5e8, Process + 0x5e0, 8);
  put(Process + 0x5f0, 0, 4);
  put(Process + 0x278, 0, 4);
  put(Process + 0x440, 4, 8);
  put(Process + 0x304, 0, 2);
  put(Process + 0x40, 0, 8);
  put(Process + 0x280, 8, 1);
  put(Process + 0x281, 6, 1);
  put(Process + 0x58, 1, 8);
  put(Process + 0x284, 0, 2);
  put(Process + 0x2c4, 0, 2);
  put(Process + 0x30, Process + 0x30, 8);
  put(Process + 0x38, Process + 0x30, 8);
  put(Process + 0x370, 0, 8);
  put(Process + 0x73a, 0, 1);
  put(Process, 3, 4);
  put(Process + 0x348, 0, 4);
  put(GS + 0x188, Bootstrap, 8);
  put(GS + 0x20, PRCB, 8);
  P.Writable.push_back({Node, 0x180});
  put(Node, 0, 8);
  put(Node + 8, 0, 8);
  put(Node + 0x10, 1, 8);
  put(Node + 0x18, 0, 8);
  put(Node + 0x88, 1, 8);
  put(Node + 0xac, 0, 4);
  put(Node + 0xb0, 0, 4);
  put(Node + 0x92, 0, 2);
  put(Node + 0x94, 0, 2);
  put(Node + 0x50, 1, 8);
  put(Node + 0x98, 0, 8);
  put(PRCB + 0x38, GS + 0x1000, 8);
  put(GS + 0x1000, 0, 1);
  P.Writable.push_back({GS + 0x1000, 1});
  put(PRCB + 0x7c18, 0, 4);
  put(PRCB + 0x7eb4, 0, 4);
  put(PRCB + 0x7eb8, 0, 8);
  for (unsigned I = 0; I < 32; ++I) {
    auto Head = PRCB + 0x7c80 + I * 16;
    put(Head, Head, 8);
    put(Head + 8, Head, 8);
  }
  put(PRCB + 0x7ec0, 0, 8);
  put(PRCB + 0x8458, 1, 8);
  put(PRCB + 7, processor::CurrentNumber, 1);
  put(PRCB + 0xd0,
      processor::CurrentGroup | (uint16_t(processor::CurrentNumber) << 8), 2);
  put(PRCB + 0x18, 0, 8);
  put(PRCB + 0xc0, Node, 8);
  put(PRCB + 0xc8, processor::ActiveMask, 8);
  put(PRCB + 0x24, processor::CurrentIndex, 4);
  put(PRCB + 8, Bootstrap, 8);
  put(PRCB + 0x10, 0, 8);
  P.Writable.push_back({PRCB, 0xaf00});
  put(PRCB + 0x30, 0, 8);
  put(PRCB + 0x23, 0, 1);
  put(PRCB + 0x84b8, 0, 8);
  put(PRCB + 0x8440, 0, 8);
  zero(
      windows::SharedClockDataBase + windows::SharedTickCountOffset,
      windows::SharedTickCountSize); // Isolated bootstrap at virtual time zero.
  put(0xfffff780000003d8ULL, 0, 8);
  put(0xfffff78000000014ULL, 0, 8);
  global("KiShortExecutionCycles", 0, 4);
  global("KiWaitAlways", 0x4e4457414954414c, 8);
  global("KiWaitNever", 0x4e44574149544e45, 8);
  global("KeKernelStackSize", Spec.StackSize, 4);
  global("KeHeteroSystem", 0, 4);
  global("__security_cookie", 0x2b992ddfa232, 8);
  global("KiXSaveAreaLength", 512, 4);
  global("PopEnergyEstimationEnabled", 0, 1);
  global("KiMaximumGroups", processor::GroupCount, 2);
  global("KiSchedulerAssistThreadFlagEnabled", 0, 4);
  global("PsThreadType", Type, 8);
  global("PspCidTable", CIDTable, 8);
  global("KeNodeBlock", Node, 8);
  global("KiProcessorBlock", PRCB, 8);
  global("KiIrqlFlags", 0, 4);
  global("PerfGlobalGroupMask", 0, 8);
  put(S.at("PerfGlobalGroupMask") + 8, 0, 8);
  put(S.at("PerfGlobalGroupMask") + 16, 0, 8);
  put(S.at("PerfGlobalGroupMask") + 24, 0, 8);
  global("KiRebalanceMinPriority", 1,
         4); // Compiled default, single-CPU balance policy.
  global("KiForegrounBoostVelocityFlag", 0, 1);
  global("KiVelocityFlags", 0, 4);
  global("KiProcessorNumberToIndexMappingTable", 0, 4);
  global("KiForceIdleDisabled", 0, 4);
  global("KiForceIdleState", 0, 4);
  global("KiCpuSetSequence", 0, 8);
  global("KiCacheAwareScheduling", 0, 4);
  global("KiCyclesPerClockQuantum", 1, 4);
  global("KiProcessListLock", 0, 8);
  global("KiProcessListHead", S.at("KiProcessListHead"), 8);
  put(S.at("KiProcessListHead") + 8, S.at("KiProcessListHead"), 8);
  global("KeActiveProcessors", processor::NativeAffinityHeader19041, 8);
  put(S.at("KeActiveProcessors") + 8, processor::ActiveMask, 8);
  for (const auto Name :
       {"ObCreateObjectEx", "ExCreateHandleEx", "MmCreateKernelStack"}) {
    const unsigned Arity = std::string(Name) == "ObCreateObjectEx"   ? 10
                           : std::string(Name) == "ExCreateHandleEx" ? 5
                                                                     : 3;
    P.Services.push_back(
        {S.at(Name), Arity,
         [&, Name](const KernelInitializerServiceInput &InputData)
             -> Expected<KernelInitializerServiceResult> {
           auto A = InputData.Arguments;
           KernelInitializerServiceResult R;
           if (std::string(Name) == "ObCreateObjectEx") {
             if (A[1] != Type || uint32_t(A[5]) != 0x898)
               return createStringError(inconvertibleErrorCode(),
                                        "unsupported object allocation");
             R.Writes.push_back(bytes(A[8], Thread, 8));
           } else if (std::string(Name) == "ExCreateHandleEx") {
             if (A[0] != CIDTable || A[1] != Thread)
               return createStringError(inconvertibleErrorCode(),
                                        "unsupported CID allocation");
             R.Value = Spec.ThreadID;
           } else {
             if (uint32_t(A[0]) != 8 || uint32_t(A[1]) || A[2] != Thread)
               return createStringError(inconvertibleErrorCode(),
                                        "unsupported stack allocation");
             R.Value = TargetStack + Spec.StackSize;
           }
           return R;
         }});
  }
  bool Inserted = false;
  unsigned HandleCount = 0;
  P.Services.push_back(
      {S.at("ObInsertObjectEx"), 7,
       [&](const KernelInitializerServiceInput &I)
           -> Expected<KernelInitializerServiceResult> {
         auto A = I.Arguments;
         if (Inserted || A[0] != Thread || A[1] != Input + 0x100 ||
             uint32_t(A[2]) != 0x1fffff || uint32_t(A[3]) ||
             uint32_t(A[4]) != 1 || A[5] || A[6])
           return createStringError(
               inconvertibleErrorCode(),
               "unexpected unnamed thread insertion arguments");
         auto Access = I.Read(A[1], 0xa0);
         if (!Access)
           return Access.takeError();
         auto Header = I.Read(Thread - 0x30, 8);
         if (!Header)
           return Header.takeError();
         auto &B = *Access;
         if (support::endian::read64le(B.data() + 0x20) ||
             support::endian::read64le(B.data() + 0x30) != Token ||
             support::endian::read64le(B.data() + 0x38) != 4 ||
             support::endian::read32le(B.data() + 0x18) != 0x1fffff ||
             support::endian::read64le(Header->data()) != 3)
           return createStringError(
               inconvertibleErrorCode(),
               "unexpected security subject or creation reference state");
         Inserted = true;
         return KernelInitializerServiceResult{
             0, {bytes(A[1] + 0x10, 0, 4), bytes(A[1] + 0x14, 0x1fffff, 4)}};
       }});
  P.Services.push_back(
      {S.at("ObOpenObjectByPointer"), 7,
       [&](const KernelInitializerServiceInput &I)
           -> Expected<KernelInitializerServiceResult> {
         auto A = I.Arguments;
         if (!Inserted || HandleCount || A[0] != Thread || uint32_t(A[1]) ||
             A[2] != Input + 0x100 || uint32_t(A[3]) || A[4] != Type ||
             uint8_t(A[5]) || A[6] != Input + 0x288)
           return createStringError(
               inconvertibleErrorCode(),
               "unexpected inserted thread handle request");
         auto Header = I.Read(Thread - 0x30, 8);
         if (!Header)
           return Header.takeError();
         auto Count = support::endian::read64le(Header->data());
         if (Count != 2)
           return createStringError(
               inconvertibleErrorCode(),
               "unexpected insertion references before handle");
         ++HandleCount;
         return KernelInitializerServiceResult{
             0,
             {bytes(A[6], 0x60000000, 8), bytes(Thread - 0x30, Count + 1, 8)}};
       }});
  P.Services.push_back(
      {S.at("KiSwapContext"), 3,
       [&](const KernelInitializerServiceInput &I)
           -> Expected<KernelInitializerServiceResult> {
         auto A = I.Arguments;
         if (!Inserted || HandleCount != 1 || A[0] != Bootstrap ||
             A[1] != Thread || uint8_t(A[2]) != 0)
           return createStringError(inconvertibleErrorCode(),
                                    "unexpected scheduler CPU handoff");
         auto Body = I.Read(Thread, 0x898);
         if (!Body)
           return Body.takeError();
         auto CPU = I.Read(PRCB + 8, 16);
         if (!CPU)
           return CPU.takeError();
         auto ProcessHead = I.Read(Process + 0x5e0, 16);
         if (!ProcessHead)
           return ProcessHead.takeError();
         auto Active = I.Read(Process + 0x5f0, 4);
         if (!Active)
           return Active.takeError();
         if ((*Body)[0x184] != 2 ||
             support::endian::read64le(CPU->data()) != Thread ||
             support::endian::read64le(CPU->data() + 8) ||
             support::endian::read64le(ProcessHead->data()) != Thread + 0x4e8 ||
             support::endian::read64le(ProcessHead->data() + 8) !=
                 Thread + 0x4e8 ||
             support::endian::read32le(Active->data()) != 1)
           return createStringError(
               inconvertibleErrorCode(),
               "thread not inserted and selected by original scheduler");
         return KernelInitializerServiceResult{0, {}, true};
       }});
  // Prepare the private CPU loader context with original KeInitThread. Its
  // running state and priority belong to this isolated CPU profile, not guest
  // output. No target-thread byte is patched by this preparation.
  KernelInitializerPlan H = P;
  H.Inputs.erase(std::remove_if(H.Inputs.begin(), H.Inputs.end(),
                                [&](auto &R) {
                                  return R.Address >= Bootstrap &&
                                         R.Address < Bootstrap + 0x898;
                                }),
                 H.Inputs.end());
  H.Inputs.push_back({Bootstrap, std::vector<uint8_t>(0x898)});
  H.Calls = {{S.at("KeInitThread"),
              {Bootstrap, BootstrapStack + 65536,
               S.at("PspSystemThreadStartup"), Start, 0, 0, 0, Process, 0},
              true}};
  H.Outputs = {{Bootstrap, 0x898}};
  auto Helper = initializeFromKernelReference(K, H);
  if (!Helper)
    return Helper.takeError();
  auto Loader = Helper->Outputs[0];
  Loader.Bytes[0xc3] = 15;
  Loader.Bytes[0x184] = 2;
  P.Inputs.erase(std::remove_if(P.Inputs.begin(), P.Inputs.end(),
                                [&](auto &R) {
                                  return R.Address >= Bootstrap &&
                                         R.Address < Bootstrap + 0x898;
                                }),
                 P.Inputs.end());
  P.Inputs.push_back(std::move(Loader));
  P.Calls = {{S.at("PspAllocateThread"),
              {Process, 0, 0, 0, 0, Input, Start, 0, Input + 0x80, Input + 0x88,
               0, Input + 0x100},
              true},
             {S.at("PspInsertThread"),
              {Thread, Process, 0, Input + 0x80, 0x1fffff, 0, 0, 0,
               Input + 0x100, Input + 0x90, 0},
              false}};
  P.Outputs = {
      {Thread, 0x898},
      {Process + KernelReferenceThreadInput::ProcessThreadListHeadOffset, 16}};
  auto Result = initializeFromKernelReference(K, P);
  if (!Result)
    return Result.takeError();
  const auto &B = Result->Outputs[0].Bytes;
  auto get = [&](size_t I) { return support::endian::read64le(B.data() + I); };
  if (!Result->Handoff || Result->Handoff->Entry != S.at("KiSwapContext") ||
      Result->ReturnValues.size() != 1 || Result->ReturnValues[0] != 0 ||
      B[0] != 6 || B[0x184] != 2 || get(0x480) != Spec.ThreadID ||
      get(0x478) != 4 || get(0x220) != Process || get(0x450) != Start ||
      get(0x4d0) != Start || get(0x2f8) != Process + 0x30 || B[0xc3] != 8 ||
      B[0x233] != 8 || get(0x240) != 1)
    return invalid(
        "original constructor did not establish the requested thread");
  KernelReferenceThreadResult Constructed{std::move(Result->Outputs[0].Bytes),
                                          Result->Instructions,
                                          Helper->Instructions,
                                          {}};
  std::copy_n(Result->Outputs[1].Bytes.begin(), 16,
              Constructed.ProcessThreadListHead.begin());
  const auto Head = Thread + 0x2f8;
  if (support::endian::read64le(Constructed.ProcessThreadListHead.data()) !=
          Head ||
      support::endian::read64le(Constructed.ProcessThreadListHead.data() + 8) !=
          Head)
    return invalid("original constructor did not link its kernel thread");
  return Constructed;
}
} // namespace neverd::emulation
