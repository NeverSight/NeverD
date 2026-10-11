//===- UnpackJSON.cpp - Unpack options and report wire format -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "UnpackInternal.h"

#ifdef NEVERD_UNPACK_EXECUTION
#include "neverd/emulation/ProcessReport.h"
#endif

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"

namespace neverd::unpack {
namespace {
/// Addresses are hexadecimal strings so that no consumer loses bits.
std::string bits(uint64_t Value) { return llvm::utohexstr(Value, true); }
} // namespace

llvm::Expected<UnpackOptions> unpackOptionsFromJSON(llvm::StringRef Text) {
#ifdef NEVERD_UNPACK_EXECUTION
  if (Text.size() > field::JSONLimit)
    return failure(text::TooLarge);
  auto Parsed = llvm::json::parse(Text);
  if (!Parsed)
    return failure(text::FieldType + llvm::toString(Parsed.takeError()));
  auto *Object = Parsed->getAsObject();
  if (!Object)
    return failure(text::ObjectRequired);
  UnpackOptions Options;
  if (const auto *Driver = Object->get("driver")) {
#ifdef NEVERD_UNPACK_DRIVER_EXECUTION
    std::string Scenario;
    llvm::raw_string_ostream(Scenario) << *Driver;
    auto ParsedDriver = emulation::driverOptionsFromScenarioJSON(Scenario);
    if (!ParsedDriver)
      return ParsedDriver.takeError();
    Options.Driver = std::move(*ParsedDriver);
    Object->erase("driver");
#else
    return failure("driver options require NEVERD_ENABLE_DRIVER_EMULATION");
#endif
  }
  if (const auto *Restore = Object->get(field::RestoreRuntime)) {
    auto Value = Restore->getAsBoolean();
    if (!Value)
      return failure(llvm::Twine(text::FieldType) + field::RestoreRuntime);
    Options.RestoreRuntime = *Value;
    Object->erase(field::RestoreRuntime);
  }
  if (const auto *Snapshot = Object->get(field::SnapshotOnly)) {
    auto Value = Snapshot->getAsBoolean();
    if (!Value)
      return failure(llvm::Twine(text::FieldType) + field::SnapshotOnly);
    Options.SnapshotOnly = *Value;
    Object->erase(field::SnapshotOnly);
  }
  if (Options.SnapshotOnly && Options.RestoreRuntime)
    return failure("snapshot_only and restore_runtime are mutually exclusive");
  if (const auto *Transfer = Object->get(field::Transfer)) {
    auto Number = Transfer->getAsUINT64();
    if (!Number || !*Number || *Number > defaults::MaxTransfers)
      return failure(llvm::Twine(text::FieldType) + field::Transfer);
    Options.Transfer = *Number;
    Object->erase(field::Transfer);
  }
  // The guest process decoder owns every remaining field and its validation.
  // Supply only the defaults that differ for unpacking; an option group the
  // caller wrote keeps every unpacking default it does not set itself.
#define NEVERD_UNPACK_PROCESS_DEFAULT(Field, Member, Default)                  \
  if (!Object->get(Field))                                                     \
    (*Object)[Field] = int64_t(defaults::Default);
#define NEVERD_UNPACK_PROFILE_DEFAULT(Group, Member, JSONGroup, JSONField,     \
                                      Value)                                   \
  if (!Object->get(JSONGroup))                                                 \
    (*Object)[JSONGroup] = llvm::json::Object{};                               \
  if (auto *Fields = Object->getObject(JSONGroup);                             \
      Fields && !Fields->get(JSONField))                                       \
    (*Fields)[JSONField] = Value;
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_PROFILE_DEFAULT
#undef NEVERD_UNPACK_PROCESS_DEFAULT
  std::string Forwarded;
  llvm::raw_string_ostream(Forwarded) << *Parsed;
  auto Process = emulation::processOptionsFromJSON(Forwarded);
  if (!Process)
    return Process.takeError();
  Options.Process = std::move(*Process);
  return Options;
#else
  return failure(text::Disabled);
#endif
}

std::string unpackResultJSON(const UnpackResult &Result,
                             llvm::StringRef OutputPath) {
  llvm::json::Array Evidence;
  for (auto E : Result.Packer.Evidence)
    Evidence.push_back(packerEvidenceName(E));
  llvm::json::Array Sections;
  for (const auto &S : Result.Sections)
    Sections.push_back(
        llvm::json::Object{{field::Name, S.Name},
                           {field::RVA, bits(S.RVA)},
                           {field::VirtualSize, S.VirtualSize},
                           {field::FileSize, S.FileSize},
                           {field::GeneratedBytes, S.GeneratedBytes}});
  llvm::json::Array Imports;
  for (const auto &I : Result.Imports)
    Imports.push_back(llvm::json::Object{
        {field::Module, I.Module},
        {field::Name, I.Name.empty() ? llvm::json::Value(nullptr)
                                     : llvm::json::Value(I.Name)},
        {field::Ordinal, I.Ordinal ? llvm::json::Value(int64_t(*I.Ordinal))
                                   : llvm::json::Value(nullptr)},
        {field::Slot, bits(I.SlotRVA)},
        {field::Origin, importOriginName(I.Origin)}});
  llvm::json::Array Transfers;
  for (const auto &T : Result.Transfers)
    Transfers.push_back(
        llvm::json::Object{{field::RVA, bits(T.RVA)},
                           {field::StackBalanced, T.StackBalanced},
                           {field::Generation, int64_t(T.Generation)},
                           {field::ProgramInvocation, T.ProgramInvocation}});
  llvm::json::Array HeapReferences;
  for (const auto &R : Result.RuntimeState.HeapReferences)
    HeapReferences.push_back(llvm::json::Object{
        {field::Storage, R.Location == UnpackHeapReference::Storage::Image
                             ? text::HeapImageStorage
                             : text::HeapTLSStorage},
        {field::Offset, bits(R.Offset)},
        {field::RVA, R.Location == UnpackHeapReference::Storage::Image
                         ? llvm::json::Value(bits(R.Offset))
                         : llvm::json::Value(nullptr)},
        {field::Address, bits(R.Address)},
        {field::AllocationAddress, bits(R.AllocationAddress)},
        {field::AllocationSize, R.AllocationSize}});
  llvm::json::Array EncodedPointers;
  for (const auto &R : Result.RuntimeState.EncodedPointerReferences)
    EncodedPointers.push_back(llvm::json::Object{
        {field::Storage, R.Location == UnpackHeapReference::Storage::Image
                             ? text::HeapImageStorage
                             : text::HeapTLSStorage},
        {field::Offset, bits(R.Offset)},
        {field::RVA, R.Location == UnpackHeapReference::Storage::Image
                         ? llvm::json::Value(bits(R.Offset))
                         : llvm::json::Value(nullptr)},
        {field::EncodedPointerValue, bits(R.Value)}});
  llvm::json::Value Output = nullptr;
  if (!Result.Image.empty()) {
    const auto Digest = llvm::SHA256::hash(Result.Image);
    Output = llvm::json::Object{
        {field::Path, OutputPath.empty() ? llvm::json::Value(nullptr)
                                         : llvm::json::Value(OutputPath)},
        {field::Size, int64_t(Result.Image.size())},
        {field::SHA256, llvm::toHex(Digest, true)}};
  }
  llvm::json::Object Object{
      {field::Version, int64_t(field::SchemaVersion)},
      {field::Format, formatKindName(Result.Format)},
      {field::Packer,
       llvm::json::Object{{field::Kind, packerKindName(Result.Packer.Kind)},
                          {field::Evidence, std::move(Evidence)}}},
      {field::Outcome, unpackOutcomeName(Result.Outcome)},
      {field::Diagnostic, Result.Diagnostic},
      {field::Architecture, Result.Architecture},
      {field::ImageBase, bits(Result.ImageBase)},
      {field::PackedEntry, bits(Result.PackedEntryRVA)},
      {field::Entry, Result.EntryRVA ? llvm::json::Value(bits(*Result.EntryRVA))
                                     : llvm::json::Value(nullptr)},
      {field::EntrySource,
       Result.EntryRVA ? llvm::json::Value(entrySourceName(Result.Source))
                       : llvm::json::Value(nullptr)},
      {field::Transfers, std::move(Transfers)},
      {field::RuntimeState,
       llvm::json::Object{
           {field::AdditionalStateKnown,
            Result.RuntimeState.AdditionalStateInventoryKnown},
           {field::AdditionalState,
            Result.RuntimeState.HasAdditionalDependencies},
           {field::HeapKnown, Result.RuntimeState.HeapInventoryKnown},
           {field::HeapReferenceCount,
            Result.RuntimeState.PossibleHeapReferences},
           {field::HeapReferences, std::move(HeapReferences)},
           {field::DirectServiceCalls, Result.RuntimeState.DirectServiceCalls},
           {field::EncodedPointerKnown,
            Result.RuntimeState.EncodedPointerInventoryKnown},
           {field::EncodedPointerCount,
            Result.RuntimeState.PossibleEncodedPointers},
           {field::EncodedPointerReferences, std::move(EncodedPointers)},
           {field::DynamicThreadLocalKnown,
            Result.RuntimeState.DynamicThreadLocalInventoryKnown},
           {field::DynamicTLSCount, Result.RuntimeState.LiveDynamicTLSSlots},
           {field::DynamicFLSCount, Result.RuntimeState.LiveDynamicFLSSlots}}},
      {field::MaterializedTLSCallbacks,
       int64_t(Result.MaterializedTLSCallbacks)},
      {field::Sections, std::move(Sections)},
      {field::Imports, std::move(Imports)},
      {field::ImportRepair,
       llvm::json::Object{
           {field::Stop, Result.ImportRepair.Stop},
           {field::Diagnostic, Result.ImportRepair.Diagnostic},
           {field::ObservedCalls, Result.ImportRepair.ObservedCalls},
           {field::RepairedCalls, Result.ImportRepair.RepairedCalls},
           {field::ConflictingCalls, Result.ImportRepair.ConflictingCalls},
           {field::ObservedLoads, Result.ImportRepair.ObservedLoads},
           {field::RepairedLoads, Result.ImportRepair.RepairedLoads},
           {field::Instructions, Result.ImportRepair.Instructions},
           {field::Events, Result.ImportRepair.Events}}},
      {field::Execution,
       llvm::json::Object{
           {field::Profile, Result.Profile},
           {field::Backend, Result.Backend},
           {field::SelectionReason, Result.BackendSelectionReason},
           {field::Stop, Result.ProcessStop},
           {field::ProcessDiagnostic, Result.ProcessDiagnostic},
           {field::PC, bits(Result.PC)},
           {field::Instructions, int64_t(Result.Instructions)},
           {field::Events, int64_t(Result.Events)}}},
      {field::Output, std::move(Output)}};
  std::string Text;
  llvm::raw_string_ostream(Text) << llvm::json::Value(std::move(Object));
  return Text;
}
} // namespace neverd::unpack
