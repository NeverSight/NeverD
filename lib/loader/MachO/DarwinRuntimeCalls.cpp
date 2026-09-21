#include "neverd/loader/MachO/DarwinRuntimeCalls.h"

#include "DarwinRuntimeImport.h"
#include "DarwinSourceDeclarations.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringRef.h"

namespace neverd {
namespace {
std::optional<llvm::StringRef> darwinWeakRuntimeImport(const BinaryImage &Image,
                                                       va_t Slot) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      Image.ConflictingImportStorageSlots.count(Slot))
    return std::nullopt;
  const auto Import = Image.ImportPtrSlots.find(Slot);
  const auto Bind = Image.DyldBindSlots.find(Slot);
  if (Import == Image.ImportPtrSlots.end() ||
      Bind == Image.DyldBindSlots.end() ||
      Bind->second.Name != Import->second || Bind->second.Addend ||
      !Bind->second.WeakImport)
    return std::nullopt;
  if (auto I = Image.ImportStorageSlots.find(Slot);
      I != Image.ImportStorageSlots.end() &&
      (I->second.Name != Import->second || I->second.Addend))
    return std::nullopt;
  return Import->second;
}
} // namespace

std::optional<SourceCallTypeHint>
darwinRuntimeSourceCallHint(const BinaryImage &Image, va_t ImportSlot) {
  // compiler-rt probes this optional libSystem entry before calling it. Keep
  // the exact weak linkage and fixed ABI so the recovered guard remains valid.
  if (const auto Weak = darwinWeakRuntimeImport(Image, ImportSlot);
      Weak && *Weak == "__availability_version_check") {
    const auto &Bind = Image.DyldBindSlots.at(ImportSlot);
    if (!darwinExportModuleMatches("/usr/lib/libSystem.B.dylib", Bind.Module))
      return std::nullopt;
    SourceCallTypeHint Result;
    Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
    Result.WeakImport = true;
    Result.TargetAddress = ImportSlot;
    Result.TargetName = "_availability_version_check";
    auto &Signature = Result.Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
    Signature.ReturnType = NdType::makeInt(1, false);
    Signature.Parameters = {
        {"count", NdType::makeInt(4, false)},
        {"versions", NdType::makePtr(NdType::makeVoid())},
    };
    std::string Diagnostic;
    if (!assignDarwinFixedSourceABI(Signature, Image.Arch, Diagnostic))
      return std::nullopt;
    return Result;
  }

  const auto Import = darwinRuntimeImport(Image, ImportSlot);
  if (!Import)
    return std::nullopt;
  llvm::StringRef Name(*Import);
  if (!Name.consume_front("_"))
    return std::nullopt;

  // UIKit declares these fixed functions with by-value CGSize parameters
  // and results. The command-line-tools SDK used for DarwinSourceDeclarations.inc
  // has no UIKit headers or binary, so retain the public contracts at the same
  // exact symbol/provider boundary as the UIKit external storage below.
  // https://developer.apple.com/documentation/uikit/nsstringfromcgsize
  // https://developer.apple.com/documentation/uikit/uigraphicsbeginimagecontext(_:)
  const bool UIKitFixedFunction =
      Name == "NSStringFromCGSize" || Name == "CGSizeFromString" ||
      Name == "UIGraphicsBeginImageContext" ||
      Name == "UIGraphicsBeginImageContextWithOptions" ||
      Name == "UIGraphicsGetCurrentContext" ||
      Name == "UIGraphicsGetImageFromCurrentImageContext" ||
      Name == "UIGraphicsEndImageContext";
  if (UIKitFixedFunction) {
    const auto Bind = Image.DyldBindSlots.find(ImportSlot);
    if (Image.Arch != Arch::AArch64 || Bind == Image.DyldBindSlots.end() ||
        !darwinExportModuleMatches(
            "/System/Library/Frameworks/UIKit.framework/UIKit",
            Bind->second.Module))
      return std::nullopt;
    SourceCallTypeHint Result;
    Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
    Result.TargetAddress = ImportSlot;
    Result.TargetName = Name.str();
    auto &Signature = Result.Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
    const bool ReturnsPointer =
        Name == "NSStringFromCGSize" || Name == "UIGraphicsGetCurrentContext" ||
        Name == "UIGraphicsGetImageFromCurrentImageContext";
    Signature.ReturnType = ReturnsPointer ? NdType::makePtr(NdType::makeVoid())
                                          : NdType::makeVoid();
    if (Name == "NSStringFromCGSize" || Name == "UIGraphicsBeginImageContext" ||
        Name == "UIGraphicsBeginImageContextWithOptions") {
      const auto Size =
          NdType::makeStruct({NdType::makeFloat(8), NdType::makeFloat(8)});
      if (!Size)
        return std::nullopt;
      Signature.Parameters = {{"size", Size}};
    }
    if (Name == "CGSizeFromString") {
      // The same complete device/simulator SDK ASTs and UIKit reexport map
      // establish CGSize(NSString *). The shared ABI assigns both FP results.
      Signature.ReturnType =
          NdType::makeStruct({NdType::makeFloat(8), NdType::makeFloat(8)});
      if (!Signature.ReturnType)
        return std::nullopt;
      Signature.Parameters = {{"string", NdType::makePtr(NdType::makeVoid())}};
    }
    if (Name == "UIGraphicsBeginImageContextWithOptions") {
      // Complete Xcode 26.5 device and arm64 simulator ASTs agree on
      // void(CGSize, BOOL=bool, CGFloat=double). Their UIKit TBDs reexport
      // this exact symbol from the embedded UIKitCore export map.
      // SDK evidence: NeverSight/NeverD Actions run 35648792995.
      Signature.Parameters.push_back({"opaque", NdType::makeInt(1, false)});
      Signature.Parameters.push_back({"scale", NdType::makeFloat(8)});
    }
    std::string Diagnostic;
    if (!assignDarwinFixedSourceABI(Signature, Image.Arch, Diagnostic))
      return std::nullopt;
    return Result;
  }

  // dispatch_once_f has a fixed callback contract that the generated Clang
  // encoding can only spell as the intentionally unsupported opaque `^?`.
  // Preserve the complete public prototype here rather than accepting unknown
  // callback signatures throughout the declaration parser.
  if (Name == "dispatch_once_f") {
    const auto Bind = Image.DyldBindSlots.find(ImportSlot);
    if (Bind == Image.DyldBindSlots.end() ||
        !darwinExportModuleMatches(
            "/usr/lib/libSystem.B.dylib|/usr/lib/system/libdispatch.dylib",
            Bind->second.Module))
      return std::nullopt;
    SourceCallTypeHint Result;
    Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
    Result.TargetAddress = ImportSlot;
    Result.TargetName = Name.str();
    auto &Signature = Result.Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
    Signature.ReturnType = NdType::makeVoid();
    const auto Context = NdType::makePtr(NdType::makeVoid());
    const auto Callback = NdType::makePtr(
        NdType::makeFunc(NdType::makeVoid(), {Context}));
    Signature.Parameters = {
        {"predicate", NdType::makePtr(NdType::makeInt(8, true))},
        {"context", Context},
        {"function", Callback},
    };
    std::string Diagnostic;
    if (!assignDarwinFixedSourceABI(Signature, Image.Arch, Diagnostic))
      return std::nullopt;
    return Result;
  }

  // dispatch_queue_set_specific has a fixed destructor callback contract that
  // the generated Clang encoding spells as the intentionally unsupported
  // opaque `^?`. Keep the public void (*)(void *) prototype at the same exact
  // libdispatch export boundary instead of accepting unknown callbacks in the
  // declaration parser.
  if (Name == "dispatch_queue_set_specific") {
    const auto Bind = Image.DyldBindSlots.find(ImportSlot);
    if (Bind == Image.DyldBindSlots.end() ||
        !darwinExportModuleMatches(
            "/usr/lib/libSystem.B.dylib|/usr/lib/system/libdispatch.dylib",
            Bind->second.Module))
      return std::nullopt;
    SourceCallTypeHint Result;
    Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
    Result.TargetAddress = ImportSlot;
    Result.TargetName = Name.str();
    auto &Signature = Result.Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
    Signature.ReturnType = NdType::makeVoid();
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    const auto Destructor =
        NdType::makePtr(NdType::makeFunc(NdType::makeVoid(), {Pointer}));
    Signature.Parameters = {
        {"queue", Pointer},
        {"key", Pointer},
        {"context", Pointer},
        {"destructor", Destructor},
    };
    std::string Diagnostic;
    if (!assignDarwinScalarSourceABI(Signature, Image.Arch, Diagnostic))
      return std::nullopt;
    return Result;
  }

  // The public lock routines use one pointer to stable, opaque lock storage.
  // Call the real platform implementation, including its ownership checks.
  // https://github.com/apple-oss-distributions/libplatform/blob/main/include/os/lock.h
  const bool TryLock = Name == "os_unfair_lock_trylock";
  const bool StackFailure = Name == "__stack_chk_fail";
  // Block.h declares these fixed C ABIs. Ownership work stays in the real
  // runtime and, for captured objects, the recovered descriptor helpers.
  const bool BlockCopy = Name == "_Block_copy";
  const bool BlockRelease = Name == "_Block_release";
  const bool BlockAssign = Name == "_Block_object_assign";
  const bool BlockDispose = Name == "_Block_object_dispose";
  const bool BlockRuntime =
      BlockCopy || BlockRelease || BlockAssign || BlockDispose;
  if (!StackFailure && !BlockRuntime && !TryLock &&
      Name != "os_unfair_lock_lock" && Name != "os_unfair_lock_unlock" &&
      Name != "os_unfair_lock_assert_owner" &&
      Name != "os_unfair_lock_assert_not_owner")
    return darwinDeclaredSourceCallHint(Image, ImportSlot);
  SourceCallTypeHint Result;
  Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
  Result.DoesNotReturn = StackFailure;
  Result.TargetAddress = ImportSlot;
  Result.TargetName = Name.str();
  auto &Signature = Result.Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinRuntime;
  Signature.ReturnType =
      TryLock ? NdType::makeInt(1, false) : NdType::makeVoid();
  if (BlockRuntime) {
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    if (BlockCopy)
      Signature.ReturnType = Pointer;
    if (BlockAssign)
      Signature.Parameters.push_back({"destination", Pointer});
    Signature.Parameters.push_back({"object", Pointer});
    if (BlockAssign || BlockDispose)
      Signature.Parameters.push_back({"flags", NdType::makeInt(4, true)});
  } else if (!StackFailure)
    Signature.Parameters = {{"lock", NdType::makePtr(NdType::makeVoid())}};
  std::string Diagnostic;
  if (!assignDarwinScalarSourceABI(Signature, Image.Arch, Diagnostic))
    return std::nullopt;
  return Result;
}

std::optional<SourceCallTypeHint>
darwinRuntimeGlobalAddressHint(const BinaryImage &Image, va_t ImportSlot) {
  const auto Import = darwinRuntimeImport(Image, ImportSlot);
  if (!Import)
    return std::nullopt;

  // These UIKit constants are public external storage, rather than functions
  // or implementation-owned objects. The command-line-tools SDK used to
  // generate DarwinSourceDataDeclarations.inc has no UIKit headers or binary,
  // so retain the same exact symbol/provider proof here.
  // https://developer.apple.com/documentation/uikit/uiapplicationdidreceivememorywarningnotification
  llvm::StringRef FrameworkData;
  const auto Bind = Image.DyldBindSlots.find(ImportSlot);
  auto MatchFrameworkData = [&](llvm::StringRef Name, llvm::StringRef Module) {
    if (Import->starts_with("_") && Import->drop_front() == Name &&
        Bind != Image.DyldBindSlots.end() &&
        darwinExportModuleMatches(Module, Bind->second.Module))
      FrameworkData = Name;
  };
  for (llvm::StringRef Name :
       {"UIApplicationDidReceiveMemoryWarningNotification",
        "UIApplicationWillTerminateNotification", "UIBackgroundTaskInvalid",
        "UIAccessibilityTraitButton", "UIEdgeInsetsZero",
        "UIViewNoIntrinsicMetric"})
    MatchFrameworkData(Name,
                       "/System/Library/Frameworks/UIKit.framework/UIKit");
  // Both complete Xcode 26.5 ARM64 SDK ASTs declare this notification as
  // external, non-TLS NSString pointer storage. Keep the original load.
  // https://developer.apple.com/documentation/uikit/uiapplication/didenterbackgroundnotification
  if (Image.Arch == Arch::AArch64)
    MatchFrameworkData(
        "UIApplicationDidEnterBackgroundNotification",
        "/System/Library/Frameworks/UIKit.framework/UIKit");
  // CIContext.h imports OpenGLES on iOS, unavailable in the CLT SDK used by
  // the generated catalog. Complete Xcode 26.5 iPhoneOS and arm64 simulator
  // ASTs agree that these are external, non-TLS NSString pointer objects.
  // The CoreImage export authenticates their storage, not their contents.
  // https://developer.apple.com/documentation/coreimage/kcicontextpriorityrequestlow
  // https://developer.apple.com/documentation/coreimage/kcicontextusesoftwarerenderer
  if (Image.Arch == Arch::AArch64)
    for (llvm::StringRef Name :
         {"kCIContextPriorityRequestLow", "kCIContextUseSoftwareRenderer"})
      MatchFrameworkData(
          Name, "/System/Library/Frameworks/CoreImage.framework/CoreImage|"
                "/System/Library/Frameworks/CoreImage.framework/Versions/A/"
                "CoreImage");
  // Swift's inlinable collection implementations take these singletons'
  // addresses, making their external storage identities part of the
  // stdlib/runtime ABI. Apple Swift 6.1.2 emits all three as external globals.
  // https://github.com/swiftlang/swift/blob/main/stdlib/public/core/ContiguousArrayBuffer.swift
  // https://github.com/swiftlang/swift/blob/main/stdlib/public/core/Dictionary.swift
  // https://github.com/swiftlang/swift/blob/main/stdlib/public/core/Set.swift
  llvm::StringRef SwiftEmptyCollection;
  if (*Import == "__swiftEmptyArrayStorage")
    SwiftEmptyCollection = "_swiftEmptyArrayStorage";
  else if (*Import == "__swiftEmptyDictionarySingleton")
    SwiftEmptyCollection = "_swiftEmptyDictionarySingleton";
  else if (*Import == "__swiftEmptySetSingleton")
    SwiftEmptyCollection = "_swiftEmptySetSingleton";
  const bool SwiftEmptyStorage =
      !SwiftEmptyCollection.empty() && Bind != Image.DyldBindSlots.end() &&
      darwinExportModuleMatches("/usr/lib/swift/libswiftCore.dylib",
                                Bind->second.Module);
  // Compiler .self queries prove these are external non-TLS data addresses,
  // not metadata accessors. Exact per-architecture exports authenticate the
  // provider; neither a mangled-name suffix nor metadata contents are guessed.
  static constexpr struct {
    const char *Name;
    const char *AArch64Modules;
    const char *X64Modules;
  } SwiftData[] = {
#include "SwiftSourceDataDeclarations.inc"
  };
  llvm::StringRef SwiftMetadata;
  if (Import->starts_with("_") && Bind != Image.DyldBindSlots.end())
    for (const auto &D : SwiftData)
      if (Import->drop_front() == D.Name &&
          darwinExportModuleMatches(
              Image.Arch == Arch::AArch64 ? D.AArch64Modules : D.X64Modules,
              Bind->second.Module))
        SwiftMetadata = D.Name;
  if (FrameworkData.empty() && !SwiftEmptyStorage && SwiftMetadata.empty() &&
      *Import != "___stack_chk_guard")
    return darwinDeclaredSourceGlobalAddressHint(Image, ImportSlot);

  SourceCallTypeHint Result;
  Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeGlobalAddress;
  Result.TargetAddress = ImportSlot;
  if (!FrameworkData.empty()) {
    Result.TargetName = FrameworkData.str();
    Result.Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
    Result.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  } else if (SwiftEmptyStorage || !SwiftMetadata.empty()) {
    Result.TargetName =
        (SwiftEmptyStorage ? SwiftEmptyCollection : SwiftMetadata).str();
    Result.Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
    Result.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  } else {
    // Darwin exports long __stack_chk_guard[8]. Bind its address and preserve
    // every native memory access; a guard is not a constant or private storage.
    // https://github.com/apple-oss-distributions/Libc/blob/main/sys/OpenBSD/stack_protector.c
    Result.TargetName = "__stack_chk_guard";
    Result.Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinRuntime;
    Result.Signature.ReturnType = NdType::makePtr(NdType::makeInt(8, true));
  }
  std::string Diagnostic;
  if (!assignDarwinScalarSourceABI(Result.Signature, Image.Arch, Diagnostic))
    return std::nullopt;
  return Result;
}
} // namespace neverd
