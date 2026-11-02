//===- ObjCMetadataJSON.cpp - Objective-C declaration serialization -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Expose declarations and their analysis-only provenance explicitly.
//===----------------------------------------------------------------------===//

#include "neverd/loader/ObjC/ObjCMetadataJSON.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"

#include <map>

namespace neverd {
namespace {
#define NEVERD_OBJC_RELATIVE_LIST_TEXT(Name, Text)                             \
  inline constexpr char Name[] = Text;
#include "ObjCRelativeLists.def"
#define NEVERD_OBJC_STORAGE_TEXT(Name, Text)                                   \
  inline constexpr char Name[] = Text;
#include "ObjCStorage.def"

std::string addressText(va_t Address) {
  return "0x" + llvm::utohexstr(Address, /*LowerCase=*/true);
}
std::string jsonSafeText(llvm::StringRef Text) {
  return llvm::json::isUTF8(Text) ? Text.str() : llvm::json::fixUTF8(Text);
}
llvm::json::Value
cacheOriginJSON(const std::optional<ObjCCacheListOrigin> &Origin) {
  if (!Origin)
    return nullptr;
  return llvm::json::Object{
      {kImageIndexJSON, static_cast<int64_t>(Origin->ImageIndex)},
      {kImageAddressJSON, addressText(Origin->ImageAddress)},
      {kConditionJSON, kImageLoadedJSON}};
}

llvm::StringRef referenceKind(ObjCSourceReference::Kind Kind) {
  switch (Kind) {
#define NEVERD_OBJC_STORAGE_REFERENCE(Enumerator, Text)                        \
  case ObjCSourceReference::Kind::Enumerator:                                  \
    return Text;
#include "ObjCStorage.def"
  }
  llvm_unreachable(kUnresolvedReference);
}
} // namespace

llvm::json::Object objcMetadataJSON(const BinaryImage &Image) {
  llvm::json::Array References;
  for (const auto &[Address, Reference] : Image.ObjCMetadataReferences)
    References.push_back(
        llvm::json::Object{{kAddressJSON, addressText(Address)},
                           {kKindJSON, referenceKind(Reference.TheKind)},
                           {kSizeJSON, Reference.Size},
                           {kNameJSON, jsonSafeText(Reference.Name)},
                           {kClassNameJSON, jsonSafeText(Reference.ClassName)},
                           {kViewJSON, kMetadataOnly}});
  llvm::json::Array Classes;
  for (const ObjCClass &Class : Image.ObjCClasses) {
    llvm::json::Array Methods;
    for (const ObjCMethod &Method : Image.ObjCMethods) {
      if (Method.ClassAddress != Class.Address)
        continue;
      Methods.push_back(llvm::json::Object{
          {"selector", jsonSafeText(Method.Selector)},
          {"type_encoding", jsonSafeText(Method.TypeEncoding)},
          {"implementation", addressText(Method.Implementation)},
          {"class_method", Method.IsClassMethod},
          {kCacheOriginJSON, cacheOriginJSON(Method.CacheOrigin)},
          {"category_name", jsonSafeText(Method.CategoryName)},
          {"category_address", addressText(Method.CategoryAddress)}});
    }
    llvm::json::Array Ivars;
    for (const auto &Ivar : Class.Ivars)
      Ivars.push_back(llvm::json::Object{
          {"name", jsonSafeText(Ivar.Name)},
          {"type_encoding", jsonSafeText(Ivar.TypeEncoding)},
          {"offset", Ivar.Offset ? llvm::json::Value(*Ivar.Offset)
                                 : llvm::json::Value(nullptr)},
          {"size", static_cast<int64_t>(Ivar.Size)},
          {"alignment", static_cast<int64_t>(Ivar.Alignment)}});
    Classes.push_back(llvm::json::Object{
        {"name", jsonSafeText(Class.Name)},
        {"address", addressText(Class.Address)},
        {"superclass_address", addressText(Class.SuperclassAddress)},
        {"superclass",
         Class.SuperclassName.empty()
             ? llvm::json::Value(nullptr)
             : llvm::json::Value(jsonSafeText(Class.SuperclassName))},
        {"root_class", Class.RootClass},
        {"instance_start", static_cast<int64_t>(Class.InstanceStart)},
        {"instance_size", static_cast<int64_t>(Class.InstanceSize)},
        {"ivar_status", Class.IvarStatus},
        {"ivars", std::move(Ivars)},
        {"inheritance_status", jsonSafeText(Class.InheritanceStatus)},
        {"methods", std::move(Methods)}});
  }
  llvm::json::Array Categories;
  std::map<va_t, std::vector<const ObjCMethod *>> CategoryMethods;
  for (const auto &Method : Image.ObjCMethods)
    if (Method.CategoryAddress && !Method.CategoryName.empty())
      CategoryMethods[Method.CategoryAddress].push_back(&Method);
  for (const auto &[Address, Methods] : CategoryMethods) {
    llvm::json::Array Members;
    for (const auto *Method : Methods)
      Members.push_back(llvm::json::Object{
          {"selector", jsonSafeText(Method->Selector)},
          {"type_encoding", jsonSafeText(Method->TypeEncoding)},
          {"implementation", addressText(Method->Implementation)},
          {"class_method", Method->IsClassMethod},
          {kCacheOriginJSON, cacheOriginJSON(Method->CacheOrigin)},
          {"category_name", jsonSafeText(Method->CategoryName)},
          {"category_address", addressText(Address)}});
    Categories.push_back(llvm::json::Object{
        {"name", jsonSafeText(Methods.front()->CategoryName)},
        {"class_name", jsonSafeText(Methods.front()->ClassName)},
        {"address", addressText(Address)},
        {"methods", std::move(Members)}});
  }
  llvm::json::Array Protocols;
  for (const auto &Protocol : Image.ObjCProtocols) {
    llvm::json::Array Adopted;
    for (auto Address : Protocol.AdoptedProtocols)
      Adopted.push_back(addressText(Address));
    llvm::json::Array Methods;
    for (const auto &Method : Protocol.Methods)
      Methods.push_back(llvm::json::Object{
          {"metadata_address", addressText(Method.MetadataAddress)},
          {"selector", jsonSafeText(Method.Selector)},
          {"type_encoding", jsonSafeText(Method.TypeEncoding)},
          {"class_method", Method.IsClassMethod},
          {kCacheOriginJSON, cacheOriginJSON(Method.CacheOrigin)},
          {"optional", Method.IsOptional},
          {"status", Method.Status}});
    Protocols.push_back(
        llvm::json::Object{{"name", jsonSafeText(Protocol.Name)},
                           {"address", addressText(Protocol.Address)},
                           {"status", Protocol.Status},
                           {"adopted_protocols", std::move(Adopted)},
                           {"methods", std::move(Methods)}});
  }
  llvm::json::Array Properties;
  for (const auto &Property : Image.ObjCProperties)
    Properties.push_back(llvm::json::Object{
        {"name", jsonSafeText(Property.Name)},
        {"attributes", jsonSafeText(Property.Attributes)},
        {"type_encoding", jsonSafeText(Property.TypeEncoding)},
        {"metadata_address", addressText(Property.MetadataAddress)},
        {"owner_address", addressText(Property.OwnerAddress)},
        {"owner_name", jsonSafeText(Property.OwnerName)},
        {"owner_kind",
         Property.Owner == ObjCProperty::OwnerKind::Class      ? "class"
         : Property.Owner == ObjCProperty::OwnerKind::Category ? "category"
                                                               : "protocol"},
        {"class_name", jsonSafeText(Property.ClassName)},
        {"class_property", Property.IsClassProperty},
        {kCacheOriginJSON, cacheOriginJSON(Property.CacheOrigin)},
        {"readonly", Property.ReadOnly},
        {"optional", Property.IsOptional},
        {"getter", jsonSafeText(Property.Getter)},
        {"setter", jsonSafeText(Property.Setter)},
        {"status", Property.Status}});
  llvm::json::Array Limitations;
  Limitations.push_back("Runtime metadata does not recover class "
                        "protocol conformance or "
                        "dynamically registered classes.");
  for (const std::string &Diagnostic : Image.ObjCMetadataDiagnostics)
    Limitations.push_back(jsonSafeText(Diagnostic));
  const char *Status =
      !Image.ObjCMetadataDiagnostics.empty() ? "partial"
      : Image.ObjCClasses.empty() && Image.ObjCProtocols.empty() &&
              Categories.empty() && Image.ObjCProperties.empty()
          ? "section-absent"
          : "recovered";
  return llvm::json::Object{{"status", Status},
                            {"classes", std::move(Classes)},
                            {"categories", std::move(Categories)},
                            {"protocols", std::move(Protocols)},
                            {"properties", std::move(Properties)},
                            {kReferencesJSON, std::move(References)},
                            {"limitations", std::move(Limitations)}};
}

} // namespace neverd
