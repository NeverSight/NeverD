#include "neverd/loader/Swift/SwiftRuntimeCalls.h"

#include "../MachO/DarwinRuntimeImport.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <iterator>

namespace neverd {
namespace {
struct SwiftRuntimeDeclaration {
  const char *Name;
  const char *Signature;
  bool DoesNotReturn;
  bool UsesSwiftConvention;
};
#include "SwiftRuntimeDeclarations.inc"

struct SwiftMetadataDeclaration {
  const char *Name;
  const char *AArch64Modules;
  const char *X64Modules;
};
constexpr SwiftMetadataDeclaration SwiftMetadataDeclarations[] = {
#include "SwiftMetadataDeclarations.inc"
};

struct SwiftSDKDeclaration {
  const char *Name;
  const char *Modules;
  const char *Signature;
};

// Compiler-observed public Foundation bridge entry points. The compact
// signature alphabet records only physical scalar carriers: p is a pointer,
// z is an unsigned word, I is swift_indirect_result, and C is swift_context.
// A parenthesized pair is returned in the two integer result registers.
constexpr SwiftSDKDeclaration SwiftSDKDeclarations[] = {
    {"$s10Foundation10URLRequestV19_bridgeToObjectiveCSo12NSURLRequestCyF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pC"},
    {"$s10Foundation10URLRequestV36_unconditionallyBridgeFromObjectiveCyACSo12"
     "NSURLRequestCSgFZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vIp"},
    {"$s10Foundation12NotificationV19_bridgeToObjectiveCSo14NSNotificationCyF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pC"},
    {"$s10Foundation12NotificationV36_unconditionallyBridgeFromObjectiveCyACSo"
     "14NSNotificationCSgFZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vIp"},
    {"$s10Foundation13URLComponentsV19_bridgeToObjectiveCSo15NSURLComponentsCyF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pC"},
    {"$s10Foundation14DateComponentsV36_unconditionallyBridgeFromObjectiveCyAC"
     "So06NSDateC0CSgFZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vIp"},
    // NSKeyValueObservation.invalidate() passes its receiver in swiftself.
    {"$s10Foundation21NSKeyValueObservationC10invalidateyyFTj",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vC"},
    {"$s10Foundation22_convertErrorToNSErrorySo0E0Cs0C0_pF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pp"},
    // URL.pathExtension reads the URL value through swiftself and returns
    // both words of the String value.
    {"$s10Foundation3URLV13pathExtensionSSvg",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "(zz)C"},
    {"$s10Foundation3URLV19_bridgeToObjectiveCSo5NSURLCyF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pC"},
    // Swift 6.1.2 arm64 client IR shows URL.appendingPathComponent(String)
    // constructing its URL result through the Swift indirect-result pointer
    // and reading the receiver through swiftself.
    {"$s10Foundation3URLV22appendingPathComponentyACSSF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vIzpC"},
    {"$s10Foundation3URLV36_"
     "unconditionallyBridgeFromObjectiveCyACSo5NSURLCSgFZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vIp"},
    {"$s10Foundation4DataV19_bridgeToObjectiveCSo6NSDataCyF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pzz"},
    {"$s10Foundation4DataV36_"
     "unconditionallyBridgeFromObjectiveCyACSo6NSDataCSgFZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "(zz)p"},
    {"$s10Foundation4DateV19_bridgeToObjectiveCSo6NSDateCyF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pC"},
    {"$s10Foundation4DateV36_"
     "unconditionallyBridgeFromObjectiveCyACSo6NSDateCSgFZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vIp"},
    // Locale.preferredLanguages returns the Array object in one register.
    {"$s10Foundation6LocaleV18preferredLanguagesSaySSGvgZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "p"},
    {"$s10Foundation6LocaleV19_bridgeToObjectiveCSo8NSLocaleCyF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pC"},
    {"$s10Foundation6LocaleV36_unconditionallyBridgeFromObjectiveCyACSo8NSLocale"
     "CSgFZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vIp"},
    {"$s10Foundation9IndexPathV19_bridgeToObjectiveCSo07NSIndexC0CyF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pC"},
    {"$s10Foundation9IndexPathV36_unconditionallyBridgeFromObjectiveCyACSo07NS"
     "IndexC0CSgFZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vIp"},
    // Binding.wrappedValue's generic setter receives the value address,
    // Binding metadata, and the mutable Binding in swiftself.
    {"$s7SwiftUI7BindingV12wrappedValuexvs",
     "/System/Library/Frameworks/SwiftUI.framework/SwiftUI", "vppC"},
    {"$sSD10FoundationE19_bridgeToObjectiveCSo12NSDictionaryCyF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "ppppp"},
    {"$sSD10FoundationE36_unconditionallyBridgeFromObjectiveCySDyxq_"
     "GSo12NSDictionaryCSgFZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "ppppp"},
    {"$sSS10lowercasedSSyF", "/usr/lib/swift/libswiftCore.dylib", "(zz)zp"},
    // Swift 6.1.2 arm64 client IR passes the inout Hasher address followed
    // by the two String words to String.hash(into:).
    {"$sSS4hash4intoys6HasherVz_tF", "/usr/lib/swift/libswiftCore.dylib",
     "vpzp"},
    {"$sSS5index5afterSS5IndexVAD_tF", "/usr/lib/swift/libswiftCore.dylib",
     "zzzp"},
    // Swift String is passed as its two scalar carriers; the mutable
    // destination is the swiftself pointer.
    {"$sSS6appendyySSF", "/usr/lib/swift/libswiftCore.dylib", "vzpC"},
    // Character occupies the same two scalar result carriers as String.
    {"$sSSySJSS5IndexVcig", "/usr/lib/swift/libswiftCore.dylib", "(zz)zzp"},
    {"$sSa10FoundationE19_bridgeToObjectiveCSo7NSArrayCyF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "ppp"},
    {"$sSa10FoundationE36_unconditionallyBridgeFromObjectiveCySayxGSo7NSArrayC"
     "SgFZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "ppp"},
    {"$sSo21OS_dispatch_semaphoreC8DispatchE4waityyF",
     "/usr/lib/swift/libswiftDispatch.dylib", "vC"},
    {"$sSo21OS_dispatch_semaphoreC8DispatchE6signalSiyF",
     "/usr/lib/swift/libswiftDispatch.dylib", "zC"},
    // The concrete UIImage initializer consumes the two String words in x0/x1
    // and returns an object in x0. WMF's arm64 call uses those carriers and
    // links the exact Swift overlay symbol from UIKit.
    // https://developer.apple.com/documentation/uikit/uiimage/init(imageliteralresourcename:)
    {"$sSo7UIImageC5UIKitE24imageLiteralResourceNameABSS_tcfC",
     "/System/Library/Frameworks/UIKit.framework/UIKit", "pzp"},
    // NSNumber(integerLiteral:) takes the integer in the first argument
    // register and the NSNumber metatype in swiftself.
    {"$sSo8NSNumberC10FoundationE14integerLiteralABSi_tcfC",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pzC"},
    // StringProtocol.caseInsensitiveCompare<String> carries five generic
    // pointers and the String value address in swiftself.
    {"$sSy10FoundationE22caseInsensitiveCompareySo18NSComparisonResultVqd__"
     "SyRd__lF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "zpppppC"},
    // Swift 6.1.2 emits StringProtocol.contains<String> as five ordinary
    // pointer carriers plus the haystack value in swiftself. The generic
    // conformance and metadata arguments remain explicit runtime inputs.
    {"$sSy10FoundationE8containsySbqd__SyRd__lF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "bpppppC"},
    {"$ss018_bridgeAnyObjectToB0yypyXlSgF",
     "/usr/lib/swift/libswiftCore.dylib", "vIp"},
    // The mutating _StringGuts.grow(Int) entry takes the capacity in the
    // first integer register and the two-word guts address in swiftself.
    {"$ss11_StringGutsV4growyySiF", "/usr/lib/swift/libswiftCore.dylib", "vzC"},
    {"$ss18_CocoaArrayWrapperV8endIndexSivg",
     "/usr/lib/swift/libswiftCore.dylib", "zz"},
    {"$ss27_bridgeAnythingToObjectiveCyyXlxlF",
     "/usr/lib/swift/libswiftCore.dylib", "ppp"},
};

bool declaredSDKABI(const BinaryImage &Image, va_t Slot,
                    SourceCallTypeHint &Hint) {
  const auto Found = std::lower_bound(
      std::begin(SwiftSDKDeclarations), std::end(SwiftSDKDeclarations),
      Hint.TargetName, [](const auto &Row, llvm::StringRef Name) {
        return llvm::StringRef(Row.Name) < Name;
      });
  const auto Bind = Image.DyldBindSlots.find(Slot);
  if (Found == std::end(SwiftSDKDeclarations) ||
      Hint.TargetName != Found->Name || Bind == Image.DyldBindSlots.end() ||
      !darwinExportModuleMatches(Found->Modules, Bind->second.Module))
    return false;

  const auto Word = NdType::makeInt(8, false);
  const auto Byte = NdType::makeInt(1, false);
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto &Signature = Hint.Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftSDK;
  llvm::StringRef Encoding(Found->Signature);
  if (Encoding.consume_front("(zz)"))
    Signature.ReturnType = NdType::makeInt(16, false);
  else if (Encoding.consume_front("p"))
    Signature.ReturnType = Pointer;
  else if (Encoding.consume_front("z"))
    Signature.ReturnType = Word;
  else if (Encoding.consume_front("b"))
    Signature.ReturnType = Byte;
  else if (Encoding.consume_front("v"))
    Signature.ReturnType = NdType::makeVoid();
  else
    return false;
  for (char Code : Encoding) {
    SourceParameterTypeHint Parameter;
    Parameter.Name = "arg" + std::to_string(Signature.Parameters.size());
    Parameter.Type = Code == 'z' ? Word : Pointer;
    if (Code == 'I')
      Parameter.TheRole = SourceParameterTypeHint::Role::SwiftIndirectResult;
    else if (Code == 'C')
      Parameter.TheRole = SourceParameterTypeHint::Role::SwiftContext;
    else if (Code != 'p' && Code != 'z')
      return false;
    Signature.Parameters.push_back(std::move(Parameter));
  }
  std::string Diagnostic;
  return assignDarwinSwiftSourceABI(Signature, Image.Arch, Diagnostic);
}

bool declaredMetadataABI(const BinaryImage &Image, va_t Slot,
                         SourceCallTypeHint &Hint) {
  const auto Found =
      std::lower_bound(std::begin(SwiftMetadataDeclarations),
                       std::end(SwiftMetadataDeclarations), Hint.TargetName,
                       [](const auto &Row, llvm::StringRef Name) {
                         return llvm::StringRef(Row.Name) < Name;
                       });
  const auto Bind = Image.DyldBindSlots.find(Slot);
  if (Found == std::end(SwiftMetadataDeclarations) ||
      Hint.TargetName != Found->Name || Bind == Image.DyldBindSlots.end() ||
      !darwinExportModuleMatches(Image.Arch == Arch::AArch64
                                     ? Found->AArch64Modules
                                     : Found->X64Modules,
                                 Bind->second.Module))
    return false;
  auto &Signature = Hint.Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftSDK;
  Signature.ReturnType = NdType::makeStruct(
      {NdType::makePtr(NdType::makeVoid()), NdType::makeInt(8, false)});
  Signature.Parameters = {{"request", NdType::makeInt(8, false)}};
  std::string Diagnostic;
  return assignDarwinSwiftSourceABI(Signature, Image.Arch, Diagnostic);
}

bool declaredStdlibABI(const BinaryImage &Image, va_t Slot,
                       llvm::StringRef Name, SourceCallTypeHint &Hint) {
  constexpr llvm::StringLiteral AssertionFailure =
      "$ss17_assertionFailure__4file4line5flagss5NeverOs12StaticStringV_"
      "SSAHSus6UInt32VtF";
  const auto Bind = Image.DyldBindSlots.find(Slot);
  if (Name != AssertionFailure || Bind == Image.DyldBindSlots.end() ||
      Bind->second.Module != "/usr/lib/swift/libswiftCore.dylib")
    return false;

  auto &Signature = Hint.Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftSDK;
  const auto Word = NdType::makeInt(8, false);
  const auto Byte = NdType::makeInt(1, false);
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  Signature.ReturnType = NdType::makeVoid();
  // Compiler IR lowers the two StaticString values to (i64, i64, i8), the
  // String value to (i64, ptr), followed by UInt and UInt32. This is a fixed
  // transport declaration; it does not expose or reconstruct either layout.
  Signature.Parameters = {{"message_address", Word},
                          {"message_count", Word},
                          {"message_flags", Byte},
                          {"detail_bits", Word},
                          {"detail_storage", Pointer},
                          {"file_address", Word},
                          {"file_count", Word},
                          {"file_flags", Byte},
                          {"line", Word},
                          {"flags", NdType::makeInt(4, false)}};
  // Both StaticString byte ranges are read synchronously and the call never
  // returns. Rebuild their immutable contents rather than preserving image
  // addresses; String remains an opaque two-word value with its ownership.
  Hint.BorrowedByteInputs = {{0, 1}, {5, 6}};
  Hint.SwiftStringInputs = {{3, 4}};
  Hint.DoesNotReturn = true;
  std::string Diagnostic;
  return assignDarwinSwiftSourceABI(Signature, Image.Arch, Diagnostic);
}

bool declaredFixedABI(const BinaryImage &Image, va_t Slot, llvm::StringRef Name,
                      SourceCallTypeHint &Hint) {
  const auto *Found = std::lower_bound(
      std::begin(SwiftRuntimeDeclarations), std::end(SwiftRuntimeDeclarations),
      Name, [](const SwiftRuntimeDeclaration &Row, llvm::StringRef Value) {
        return llvm::StringRef(Row.Name) < Value;
      });
  if (Found == std::end(SwiftRuntimeDeclarations) || Name != Found->Name)
    return false;
  if (Found->UsesSwiftConvention) {
    // Versioned runtime entries carry the same fixed ABI when strongly
    // imported from their declared provider. A weak or absent import cannot
    // establish availability; a same-named user function supplies no evidence.
    const auto Bind = Image.DyldBindSlots.find(Slot);
    if (Bind == Image.DyldBindSlots.end() ||
        Bind->second.Module != "/usr/lib/swift/libswiftCore.dylib")
      return false;
  }
  const auto Type = [](char Encoding) -> TypeRef {
    switch (Encoding) {
    case 'v':
      return NdType::makeVoid();
    case 'p':
      return NdType::makePtr(NdType::makeVoid());
    case 'z':
      return NdType::makeInt(8, false);
    case 'u':
      return NdType::makeInt(4, false);
    case 'b':
      // Catalog boolean results have an explicit zero-extension contract.
      // Keep their complete low byte as an unsigned source carrier.
      return NdType::makeInt(1, false);
    default:
      return {};
    }
  };
  llvm::StringRef Encoding(Found->Signature);
  if (Encoding.empty() || Encoding.size() > 20)
    return false;
  auto &Signature = Hint.Signature;
  if (Encoding.consume_front("(")) {
    // The catalog records only complete, explicitly declared two-word
    // Swift results. Their physical carriers belong to the shared ABI layer.
    if (!Found->UsesSwiftConvention || Encoding.size() < 3 ||
        Encoding[2] != ')' || (Encoding[0] != 'p' && Encoding[0] != 'z') ||
        (Encoding[1] != 'p' && Encoding[1] != 'z'))
      return false;
    Signature.ReturnType =
        NdType::makeStruct({Type(Encoding[0]), Type(Encoding[1])});
    Encoding = Encoding.drop_front(3);
  } else {
    Signature.ReturnType = Type(Encoding.front());
    Encoding = Encoding.drop_front();
  }
  if (!Signature.ReturnType)
    return false;
  Signature.Parameters.clear();
  for (char Code : Encoding) {
    const auto Parameter = Type(Code);
    if (!Parameter || Parameter->Kind == NdTypeKind::Void)
      return false;
    Signature.Parameters.push_back(
        {"arg" + std::to_string(Signature.Parameters.size()), Parameter});
  }
  Signature.Convention = Found->UsesSwiftConvention
                             ? SourceFunctionTypeHint::ConventionKind::Swift
                             : SourceFunctionTypeHint::ConventionKind::C;
  Hint.DoesNotReturn = Found->DoesNotReturn;
  return !Hint.DoesNotReturn || Signature.ReturnType->Kind == NdTypeKind::Void;
}
} // namespace

std::optional<SourceCallTypeHint>
swiftRuntimeSourceCallHint(const BinaryImage &Image, va_t ImportSlot) {
  const auto Import = darwinRuntimeImport(Image, ImportSlot);
  if (!Import)
    return std::nullopt;
  llvm::StringRef Name(*Import);
  if (!Name.consume_front("_"))
    return std::nullopt;

  SourceCallTypeHint Result;
  Result.CallKind = SourceCallTypeHint::Kind::SwiftRuntimeCall;
  Result.TargetAddress = ImportSlot;
  Result.TargetName = Name.str();
  if (declaredMetadataABI(Image, ImportSlot, Result))
    return Result;
  auto &Signature = Result.Signature;
  if (declaredSDKABI(Image, ImportSlot, Result))
    return Result;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
  if (declaredStdlibABI(Image, ImportSlot, Name, Result))
    return Result;
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  const bool ReportInFile =
      Name == "_swift_stdlib_reportFatalErrorInFile" ||
      Name == "_swift_stdlib_reportUnimplementedInitializerInFile";
  const bool ReportInitializer =
      Name == "_swift_stdlib_reportUnimplementedInitializer" ||
      Name == "_swift_stdlib_reportUnimplementedInitializerInFile";
  if (ReportInFile || ReportInitializer ||
      Name == "_swift_stdlib_reportFatalError") {
    // SwiftShims/AssertionReporting.h declares ordinary C calls. Reporting
    // returns; the compiler emits a separate trap. Each string is consumed
    // through a bounded precision and copied into the diagnostic message.
    const auto Bytes = NdType::makePtr(NdType::makeInt(1, false));
    const auto Length = NdType::makeInt(4, true);
    const auto Unsigned = NdType::makeInt(4, false);
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"first", Bytes},
                            {"first_length", Length},
                            {"second", Bytes},
                            {"second_length", Length}};
    Result.BorrowedByteInputs = {{0, 1}, {2, 3}};
    if (ReportInFile) {
      Signature.Parameters.push_back({"file", Bytes});
      Signature.Parameters.push_back({"file_length", Length});
      Signature.Parameters.push_back({"line", Unsigned});
      Result.BorrowedByteInputs.push_back({4, 5});
      if (ReportInitializer)
        Signature.Parameters.push_back({"column", Unsigned});
    }
    Signature.Parameters.push_back({"flags", Unsigned});
    std::string Diagnostic;
    if (!assignDarwinScalarSourceABI(Signature, Image.Arch, Diagnostic))
      return std::nullopt;
    return Result;
  }
  // These entries have C_CC declarations in the Swift runtime ABI. In
  // particular, Direct refcount entries use SwiftDirectRR_CC and must not be
  // accepted by prefix matching. Keep calls and their memory effects intact.
  // https://github.com/swiftlang/swift/blob/main/include/swift/Runtime/RuntimeFunctions.def
  if (Name == "swift_retain" || Name == "swift_nonatomic_retain" ||
      Name == "swift_unknownObjectRetain" ||
      Name == "swift_nonatomic_unknownObjectRetain" ||
      Name == "swift_bridgeObjectRetain" ||
      Name == "swift_nonatomic_bridgeObjectRetain" ||
      Name == "swift_getObjectType") {
    Signature.ReturnType = Pointer;
    Signature.Parameters = {{"object", Pointer}};
  } else if (Name == "swift_release" || Name == "swift_nonatomic_release" ||
             Name == "swift_unknownObjectRelease" ||
             Name == "swift_nonatomic_unknownObjectRelease" ||
             Name == "swift_bridgeObjectRelease" ||
             Name == "swift_nonatomic_bridgeObjectRelease") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"object", Pointer}};
  // RuntimeFunctions.def instantiates this exact C-ABI storage family for
  // native and unknown-object weak/unowned references. Keep explicit names so
  // private suffix variants cannot acquire a public runtime contract.
  } else if (Name == "swift_weakInit" || Name == "swift_weakAssign" ||
             Name == "swift_unknownObjectWeakInit" ||
             Name == "swift_unknownObjectWeakAssign" ||
             Name == "swift_unownedInit" || Name == "swift_unownedAssign" ||
             Name == "swift_unknownObjectUnownedInit" ||
             Name == "swift_unknownObjectUnownedAssign") {
    Signature.ReturnType = Pointer;
    Signature.Parameters = {{"reference", Pointer}, {"object", Pointer}};
  } else if (Name == "swift_weakCopyInit" || Name == "swift_weakTakeInit" ||
             Name == "swift_weakCopyAssign" || Name == "swift_weakTakeAssign" ||
             Name == "swift_unknownObjectWeakCopyInit" ||
             Name == "swift_unknownObjectWeakTakeInit" ||
             Name == "swift_unknownObjectWeakCopyAssign" ||
             Name == "swift_unknownObjectWeakTakeAssign" ||
             Name == "swift_unownedCopyInit" ||
             Name == "swift_unownedTakeInit" ||
             Name == "swift_unownedCopyAssign" ||
             Name == "swift_unownedTakeAssign" ||
             Name == "swift_unknownObjectUnownedCopyInit" ||
             Name == "swift_unknownObjectUnownedTakeInit" ||
             Name == "swift_unknownObjectUnownedCopyAssign" ||
             Name == "swift_unknownObjectUnownedTakeAssign") {
    Signature.ReturnType = Pointer;
    Signature.Parameters = {{"destination", Pointer}, {"source", Pointer}};
  } else if (Name == "swift_weakLoadStrong" || Name == "swift_weakTakeStrong" ||
             Name == "swift_unknownObjectWeakLoadStrong" ||
             Name == "swift_unknownObjectWeakTakeStrong" ||
             Name == "swift_unownedLoadStrong" ||
             Name == "swift_unownedTakeStrong" ||
             Name == "swift_unknownObjectUnownedLoadStrong" ||
             Name == "swift_unknownObjectUnownedTakeStrong") {
    Signature.ReturnType = Pointer;
    Signature.Parameters = {{"reference", Pointer}};
  } else if (Name == "swift_weakDestroy" ||
             Name == "swift_unknownObjectWeakDestroy" ||
             Name == "swift_unownedDestroy" ||
             Name == "swift_unknownObjectUnownedDestroy") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"reference", Pointer}};
  } else if (Name == "swift_once") {
    // Runtime/Once.h uses C_CC, including the context argument passed to the
    // callback. A source binding preserves the runtime call and its predicate;
    // it does not prove ownership or permit eager/omitted initialization.
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"predicate", Pointer},
                            {"function", NdType::makePtr(NdType::makeFunc(
                                             NdType::makeVoid(), {Pointer}))},
                            {"context", Pointer}};
  } else if (Name == "swift_beginAccess") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"address", Pointer},
                            {"scratch", Pointer},
                            {"flags", NdType::makeInt(8, false)},
                            {"pc", Pointer}};
  } else if (Name == "swift_endAccess") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"scratch", Pointer}};
  } else if (!declaredFixedABI(Image, ImportSlot, Name, Result)) {
    return std::nullopt;
  }
  std::string Diagnostic;
  const bool Assigned =
      Signature.Convention == SourceFunctionTypeHint::ConventionKind::Swift
          ? assignDarwinSwiftSourceABI(Signature, Image.Arch, Diagnostic)
          : assignDarwinScalarSourceABI(Signature, Image.Arch, Diagnostic);
  if (!Assigned)
    return std::nullopt;
  return Result;
}

} // namespace neverd
