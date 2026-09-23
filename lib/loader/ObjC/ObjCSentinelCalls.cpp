#include "neverd/loader/ObjC/ObjCSentinelCalls.h"

#include "../MachO/DarwinSourceDeclarations.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCConstantStrings.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"
#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"

#include <algorithm>

namespace neverd {
bool objcSentinelReceiverValid(const BinaryImage &Image,
                               llvm::StringRef Selector,
                               const ObjCReceiverTypeHint &Receiver) {
  if (Image.Arch != Arch::AArch64 || Image.Format != BinaryFormat::MachO ||
      Image.Bits != Bitness::Bits64 || Image.IsRelocatable ||
      Selector != "setWithObjects:" ||
      Receiver.Origin != ObjCReceiverTypeHint::OriginKind::ClassReference ||
      Receiver.ClassName != "NSSet" || !Receiver.IsClassMethod ||
      !Receiver.Steps.empty() || !objcReceiverTypeHintValid(Image, Receiver))
    return false;
  // The method declaration and the class-object export are independent facts.
  // Complete SDK TBDs from Actions run 35642673269 export NSSet through
  // CoreFoundation and explicitly reexport CoreFoundation from Foundation.
  // The device TBD target is arm64e-ios; simulator targets include arm64.
  // CoreFoundation SHA256 (device, simulator):
  // 09c3d615bf5cd50d5c579113d8db3c9b2fd8b75406111b9e6b0956868207844b
  // 580fbfd9dd800631181b48764f6672ef6b2537052cde1b5763f35b0b985e7ffb
  // Foundation SHA256 (device, simulator):
  // c30d1c4b6df415d47b53e63786ce70f65fe697b8b0ae9b9f733af3bb0793c3d1
  // f8deb2651ce28d190c16c38dce45c22ec1cee36fc1d3c4061f897bec3d3f32ba
  constexpr llvm::StringLiteral Foundation =
      "/System/Library/Frameworks/Foundation.framework/Foundation";
  constexpr llvm::StringLiteral CoreFoundation =
      "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation";
  if (std::find(Image.DynInfo.NeededLibs.begin(),
                Image.DynInfo.NeededLibs.end(),
                Foundation) == Image.DynInfo.NeededLibs.end())
    return false;
  const auto Bind = Image.DyldBindSlots.find(Receiver.Address);
  if (Bind == Image.DyldBindSlots.end() ||
      Bind->second.Name != "_OBJC_CLASS_$_NSSet" || Bind->second.Addend ||
      Bind->second.WeakImport ||
      Image.ConflictingImportStorageSlots.count(Receiver.Address) ||
      std::find(Image.DynInfo.NeededLibs.begin(),
                Image.DynInfo.NeededLibs.end(),
                Bind->second.Module) == Image.DynInfo.NeededLibs.end() ||
      (Bind->second.Module != Foundation &&
       Bind->second.Module != CoreFoundation))
    return false;
  const auto Storage = Image.ImportStorageSlots.find(Receiver.Address);
  if (Storage != Image.ImportStorageSlots.end() &&
      (Storage->second.Name != Bind->second.Name || Storage->second.Addend))
    return false;
  const auto Import = Image.ImportPtrSlots.find(Receiver.Address);
  if (Import != Image.ImportPtrSlots.end() &&
      Import->second != Bind->second.Name)
    return false;
  for (const auto &Class : Image.ObjCClasses)
    if (Class.Name == "NSSet")
      return false;
  // Runtime encodings omit variadic and sentinel attributes. A local override
  // cannot inherit the SDK's tail contract merely by sharing its fixed types.
  for (const auto &Method : Image.ObjCMethods)
    if (Method.ClassName == "NSSet" && Method.IsClassMethod &&
        Method.Selector == Selector)
      return false;
  for (const auto &Property : Image.ObjCProperties)
    if (Property.ClassName == "NSSet" && Property.IsClassProperty &&
        (Property.Getter == Selector || Property.Setter == Selector))
      return false;
  return true;
}

std::optional<SourceCallTypeHint>
objcSentinelSourceCallHint(const BinaryImage &Image, llvm::StringRef Selector,
                           const ObjCReceiverTypeHint &Receiver,
                           llvm::ArrayRef<va_t> Objects) {
  if (!objcSentinelReceiverValid(Image, Selector, Receiver) ||
      Objects.size() > 61)
    return std::nullopt;
  for (const auto Address : Objects)
    if (!Address || (!readObjCConstantString(Image, Address) &&
                     !darwinDeclaredSourceDataObject(Image, Address)))
      return std::nullopt;
  // Complete device/simulator Messages ASTs agree on NSSet(Creation)'s
  // +setWithObjects:(id)firstObj, ... and SentinelAttr. Their accompanying
  // NS_REQUIRES_NIL_TERMINATION macro is __attribute__((sentinel(0,1))).
  // AST SHA256:
  // dc99c06a9e4aab8015f8675b08ac55e042201401c21d39a4df4535aa63e0303f
  // c443f389aae208205f42b4a592859ffb45766cc6e2f663b6ef919528e2676b61
  // Complete macro dump SHA256 (device, simulator):
  // 6812f2c893d183b6584e8dab85a33ad73359753ecefb530333c101049b51d0a5
  // b07e8d80da623fa63c31dc790180f460706a572a0ecf6b20e4b95941292469ef
  auto Signature = parseObjCMethodEncoding(Selector, "@24@0:8@16");
  if (!Signature)
    return std::nullopt;
  Signature->Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
  // firstObject is either Objects.front() or nil. Each nonempty sequence has
  // its remaining objects followed by a separate nil variadic argument.
  for (size_t I = 0; I < Objects.size(); ++I)
    Signature->Parameters.push_back(
        {"object", NdType::makePtr(NdType::makeVoid())});
  std::string Diagnostic;
  if (!assignDarwinVariadicSourceABI(*Signature, 3, Image.Arch, Diagnostic))
    return std::nullopt;
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Hint.Signature = std::move(*Signature);
  Hint.TargetName = "objc_msgSend";
  Hint.Selector = Selector.str();
  Hint.Receiver = Receiver;
  Hint.NilTerminated = SourceCallTypeHint::NilTerminatedArguments{
      std::vector<va_t>(Objects.begin(), Objects.end())};
  return Hint;
}
} // namespace neverd
