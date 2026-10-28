//===- AndroidDex.cpp - Explicit DEX bodies in the workload VM ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared decoder admission and execution budgets for JNI-supplied DEX code.
//===----------------------------------------------------------------------===//
#include "../kernel/LinuxTime.h"
#include "AndroidInternal.h"

#include "neverd/dalvik/AndroidProperties.h"
#include "neverd/dalvik/Runtime.h"
#include "neverd/emulation/Limits.h"

#include <cstdint>
#include <utility>

namespace neverd::emulation::android_model {
llvm::Error Bionic::initializeDex() {
  const auto &Native = *Options.Android;
  JNI.Heap = std::make_shared<mobile::dalvik::Heap>(Options.MemoryLimit / 2,
                                                    jni_abi::ObjectLimit);
  if (!Native.JNI && !Native.JNIClassLookupFailures.empty())
    return failure("JNI class lookup failures require JNI");
  if (!Native.JNI && Native.Application)
    return failure("Android application snapshot requires JNI");
  if (!Native.JNI && Native.ObserveJavaState)
    return failure("Android Java state observation requires JNI");
  if (Native.Application && Native.Application->files_directory &&
      (!Options.LinuxFiles ||
       !linux_model::isDeclaredDirectory(*Options.LinuxFiles,
                                         *Native.Application->files_directory)))
    return failure(
        "Android files directory requires a declared Linux directory");
  if (Native.DexImages.empty() && !Native.JNI)
    return llvm::Error::success();
  if (!Native.JNI || Native.DexImages.size() > 16)
    return failure("DEX images require JNI and at most 16 explicit images");
  uint64_t Total = 0;
  for (const auto &Image : Native.DexImages) {
    if (Image.empty() ||
        Image.size() >
            ::neverd::limits::emulation::kMaxAndroidDexInputBytes - Total)
      return failure("DEX image input exceeds the 1 MiB budget");
    Total += Image.size();
    if (Image.size() < 8 || Image[7] != 0 ||
        (std::string_view(reinterpret_cast<const char *>(Image.data()), 7) !=
             "dex\n035" &&
         std::string_view(reinterpret_cast<const char *>(Image.data()), 7) !=
             "dex\n037" &&
         std::string_view(reinterpret_cast<const char *>(Image.data()), 7) !=
             "dex\n038" &&
         std::string_view(reinterpret_cast<const char *>(Image.data()), 7) !=
             "dex\n039"))
      return failure(
          "Android API 28 requires DEX version 035, 037, 038 or 039");
  }
  // Materialized strings/instructions are larger than their compressed DEX
  // representation. Reserve a conservative bounded share before decoding.
  if (Total > Options.MemoryLimit / 256)
    return failure("DEX input exceeds the process model storage budget");
  try {
    // max_files bounds the shared reader's logical class inventory, not the
    // number of input DEX images. Debit it across all images in this workload.
    mobile::Budget DecodeBudget({1, 4096, std::max<uint64_t>(1, Total)});
    DecodeBudget.deadline =
        std::chrono::steady_clock::now() +
        std::chrono::microseconds(Budget.remainingMicroseconds());
    std::vector<mobile::dalvik::Class> Classes;
    for (size_t I = 0; I < Native.DexImages.size(); ++I) {
      const auto &Image = Native.DexImages[I];
      auto Part = mobile::dalvik::parseDex(
          std::string_view(reinterpret_cast<const char *>(Image.data()),
                           Image.size()),
          "supplied-dex-" + std::to_string(I), DecodeBudget);
      DecodeBudget.limits.max_files -= Part.size();
      Classes.insert(Classes.end(), std::make_move_iterator(Part.begin()),
                     std::make_move_iterator(Part.end()));
    }
    JNI.Dex = std::make_shared<mobile::dalvik::Program>(std::move(Classes),
                                                        Native.Properties);
    if (Native.JNIClassLookupFailures.size() > jni_abi::ReferenceLimit)
      return failure("JNI class lookup failure inventory exceeds its budget");
    for (const auto &[Name, Exception] : Native.JNIClassLookupFailures) {
      if (Name.empty() ||
          Name.size() > ::neverd::limits::emulation::
                            kAndroidDexBionicInitializeDexNameSizeLimit ||
          Name.front() == '/' || Name.back() == '/' ||
          Name.find("//") != std::string::npos ||
          Name.find_first_of(".;[\0", 0, 4) != std::string::npos ||
          (Exception != "java/lang/ClassNotFoundException" &&
           Exception != "java/lang/NoClassDefFoundError"))
        return failure("invalid JNI class lookup failure declaration");
      const auto Type = "L" + Name + ";";
      mobile::dalvik::descriptor(Type);
      if (mobile::dalvik::platformClass(Name) || JNI.Dex->containsClass(Type))
        return failure("JNI class lookup failure shadows a known class");
    }
    auto ReadClock = [this](int32_t ID, uint64_t Divisor,
                            const char *RangeError) {
      using mobile::dalvik::ExecutionResult;
      using mobile::dalvik::ExecutionStop;
      // Native and modeled Java work use the same cumulative clock owner.
      // Accounting a repeated total never advances time twice.
      if (!Kernel.clock().accountInstructions(Budget.instructions(), Result))
        return ExecutionResult{
            ExecutionStop::Unsupported, 0, {}, Result.Diagnostic};
      auto Now = Kernel.clock().read(ID, Result);
      if (!Now)
        return ExecutionResult{
            ExecutionStop::Unsupported, 0, {}, Result.Diagnostic};
      // Both API 28 clock paths use signed nanoseconds before conversion.
      if (Now->Seconds < 0 ||
          uint64_t(Now->Seconds) >
              (uint64_t(INT64_MAX) - Now->Nanoseconds) / 1000000000)
        return ExecutionResult{ExecutionStop::Unsupported, 0, {}, RangeError};
      return ExecutionResult{
          ExecutionStop::Returned,
          (uint64_t(Now->Seconds) * 1000000000 + Now->Nanoseconds) / Divisor};
    };
    mobile::dalvik::RuntimeServices Services;
    Services.binder_thread_policy = [this] {
      return mobile::dalvik::ExecutionResult{
          mobile::dalvik::ExecutionStop::Returned,
          binderThread().StrictModePolicy};
    };
    Services.set_binder_thread_policy = [this](uint32_t Policy) {
      binderThread().StrictModePolicy = Policy;
      return mobile::dalvik::ExecutionResult{};
    };
    Services.event_log_write = [this](std::span<const uint8_t> Payload) {
      using mobile::dalvik::ExecutionResult;
      using mobile::dalvik::ExecutionStop;
      auto Written = eventLogWrite({Payload.data(), Payload.size()});
      return Written
                 ? ExecutionResult{ExecutionStop::Returned, *Written}
                 : ExecutionResult{
                       ExecutionStop::Unsupported, 0, {}, Result.Diagnostic};
    };
    Services.uptime_milliseconds = [ReadClock] {
      return ReadClock(linux_model::ClockMonotonic, 1000000,
                       "Android monotonic time exceeds its range");
    };
    Services.elapsed_realtime_nanoseconds = [ReadClock] {
      return ReadClock(linux_model::ClockBootTime, 1,
                       "Android boot time exceeds its range");
    };
    Services.process_uid = [this] {
      using mobile::dalvik::ExecutionResult;
      using mobile::dalvik::ExecutionStop;
      auto UID = kernelService(linux_model::ServiceKind::GetUID, {});
      if (!UID)
        return ExecutionResult{
            ExecutionStop::Invalid, 0, {}, llvm::toString(UID.takeError())};
      if (!*UID)
        return ExecutionResult{
            ExecutionStop::Unsupported, 0, {}, Result.Diagnostic};
      return ExecutionResult{ExecutionStop::Returned, **UID};
    };
    Services.property_integer = [this](std::string_view Key, int32_t Default) {
      using mobile::dalvik::ExecutionResult;
      using mobile::dalvik::ExecutionStop;
      // Use the native record owner, including its guest mapping/integrity
      // checks. A changed record must not fall back to the input dictionary.
      auto Property = propertyNamed(llvm::StringRef(Key.data(), Key.size()));
      if (!Property)
        return ExecutionResult{ExecutionStop::Invalid,
                               0,
                               {},
                               llvm::toString(Property.takeError())};
      return ExecutionResult{ExecutionStop::Returned,
                             uint32_t(*Property
                                          ? mobile::dalvik::androidPropertyInt(
                                                (*Property)->Value, Default)
                                          : Default)};
    };
    JNI.DexRuntime = std::make_shared<mobile::dalvik::Runtime>(
        *JNI.Dex, JNI.Heap, Native.Application, std::move(Services));
  } catch (const mobile::Error &E) {
    return failure(llvm::Twine("invalid supplied DEX: ") + E.what());
  }
  return llvm::Error::success();
}
BionicResult Bionic::executeDex(const mobile::dalvik::Method *Method,
                                llvm::ArrayRef<uint64_t> Arguments,
                                llvm::StringRef Class, DexCompletion Complete) {
  return runDex(
      [&](const auto &Admit, uint64_t Stack) {
        return Method ? JNI.DexRuntime->execute(
                            *Method, {Arguments.data(), Arguments.size()},
                            Admit, Stack, threadID())
                      : JNI.DexRuntime->initialize(Class.str(), Admit, Stack,
                                                   threadID());
      },
      std::move(Complete));
}
BionicResult Bionic::allocateDex(llvm::StringRef Class, bool Initialized,
                                 DexCompletion Complete) {
  return runDex(
      [&](const auto &Admit, uint64_t Stack) {
        return JNI.DexRuntime->allocate(Class.str(), Initialized, Admit, Stack,
                                        threadID());
      },
      std::move(Complete));
}
BionicResult
Bionic::runDex(const std::function<mobile::dalvik::ExecutionResult(
                   const mobile::dalvik::AdmitInstruction &, uint64_t)> &Body,
               DexCompletion Complete) {
  using mobile::dalvik::ExecutionStop;
  auto Admit = [this] {
    if (!Budget.remainingMicroseconds())
      return ExecutionStop::Timeout;
    return Budget.consumeInstructions() ? ExecutionStop::Returned
                                        : ExecutionStop::InstructionLimit;
  };
  auto Executed = Body(
      Admit, std::min<uint64_t>(Options.StackSize, Options.MemoryLimit / 256));
  return finishDex(std::move(Executed), std::move(Complete));
}
BionicResult Bionic::resumeDex(std::shared_ptr<DexContinuation> Continuation,
                               mobile::dalvik::ExecutionResult Result) {
  if (Continuation->Thread != threadID())
    return failure("Java continuation thread mismatch");
  return finishDex(
      JNI.DexRuntime->resume(Continuation->ID, threadID(), std::move(Result)),
      std::move(Continuation->Complete));
}
BionicResult Bionic::dexReference(uint64_t Identity) {
  auto Ref = jniReference(JNI.Heap->get(Identity));
  if (!Ref)
    return Ref.takeError();
  return value(*Ref);
}
BionicResult Bionic::finishDex(mobile::dalvik::ExecutionResult Executed,
                               DexCompletion Complete) {
  using mobile::dalvik::ExecutionStop;
  while (Executed.stop == ExecutionStop::Suspended) {
    if (!Executed.external || !Executed.continuation)
      return failure("missing Java continuation request");
    auto Continuation = std::make_shared<DexContinuation>(DexContinuation{
        Executed.continuation, threadID(), std::move(Complete)});
    if (Executed.external->kind == mobile::dalvik::ExternalCall::LoadLibrary) {
      auto Load = loadJavaLibrary(*Executed.external, Continuation);
      if (!Load)
        return Load.takeError();
      if (auto Callback = std::get_if<GuestCallback>(&*Load))
        return std::optional<BionicValue>(std::move(*Callback));
      Executed = JNI.DexRuntime->resume(
          Continuation->ID, threadID(),
          std::move(std::get<mobile::dalvik::ExecutionResult>(*Load)));
      Complete = std::move(Continuation->Complete);
      continue;
    }
    if (!Executed.external->method)
      return failure("missing Java native method");
    return callNativeJNI(*Executed.external->method,
                         Executed.external->arguments, std::move(Continuation));
  }
  if (Executed.stop == ExecutionStop::Returned)
    return Complete ? Complete(Executed.value) : value(Executed.value);
  if (Executed.stop == ExecutionStop::Thrown) {
    if (Executed.throwable)
      JNI.Threads.at(threadID()).Exception = JNI.Heap->get(Executed.throwable);
    else if (auto E = jniException(Executed.exception))
      return std::move(E);
    return value(0);
  }
  if (Executed.stop == ExecutionStop::Invalid)
    return failure(Executed.diagnostic);
  Result.Diagnostic = Executed.diagnostic;
  if (Executed.stop == ExecutionStop::Timeout) {
    Expired = true;
    Result.Stop = ProcessStopReason::Timeout;
  } else if (Executed.stop == ExecutionStop::InstructionLimit)
    Result.Stop = ProcessStopReason::InstructionLimit;
  else
    Result.Stop = ProcessStopReason::UnsupportedService;
  return std::nullopt;
}
} // namespace neverd::emulation::android_model
