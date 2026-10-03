//===- WindowsProcessExports.cpp - Bounded guest export resolution -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcessModules.h"

#include "neverd/emulation/CPU.h"

#include "llvm/ADT/StringExtras.h"

#include <array>
#include <set>

namespace neverd::emulation::windows_process {
namespace {
using namespace value;

llvm::Error validateMetadata(Program &Program, size_t Index,
                             const ExecutionBudget &Budget,
                             ExecutionBackend &CPU) {
  const auto &Module = Program.Modules[Index];
  const auto &Image = Module.Loaded;
  std::array<uint8_t, PageSize> Bytes;
  for (const auto &Range : Module.ExportMetadata) {
    uint64_t Address = Image.Base + Range.RVA, Remaining = Range.Size;
    while (Remaining) {
      if (!Budget.remainingMicroseconds())
        return failure(text::ModuleTimeout);
      auto Region = llvm::find_if(Image.Regions, [&](const auto &R) {
        return Address >= R.Address && Address - R.Address < R.Bytes.size();
      });
      if (Region == Image.Regions.end())
        return failure(text::Metadata);
      const uint64_t Offset = Address - Region->Address;
      const uint64_t Size = std::min<uint64_t>(
          {Remaining, Bytes.size(), Region->Bytes.size() - Offset});
      if (Size > Program.Reads.MetadataBytes)
        return failure(text::ExportBudget);
      Program.Reads.MetadataBytes -= Size;
      auto Access = CPU.canAccess(Address, Size, Read | UserAccessible);
      if (!Access)
        return Access.takeError();
      if (!*Access)
        return failure(text::Access);
      if (auto E =
              CPU.read(Address, llvm::MutableArrayRef(Bytes).take_front(Size)))
        return E;
      if (!std::equal(Bytes.begin(), Bytes.begin() + Size,
                      Region->Bytes.begin() + Offset))
        return failure(text::ExportChanged);
      Address += Size;
      Remaining -= Size;
    }
  }
  return llvm::Error::success();
}
} // namespace

llvm::Expected<ExportResolution>
resolveExport(Program &Program, size_t Index, llvm::StringRef Name,
              std::optional<uint16_t> Ordinal, const ExecutionBudget &Budget,
              ExecutionBackend *CPU, ForwardModule Load) {
  std::string Symbol = Name.str();
  std::set<std::pair<size_t, size_t>> Visited;
  std::set<size_t> Validated;
  for (uint64_t Depth = 0; Depth < MaxForwarders; ++Depth) {
    if (!Budget.remainingMicroseconds())
      return failure(text::ModuleTimeout);
    if (!Program.Reads.Records)
      return failure(text::ExportBudget);
    --Program.Reads.Records;
    if (Index >= Program.Modules.size())
      return failure(text::ModuleExport);
    if (Ordinal && !*Ordinal)
      return ExportResolution{std::nullopt, ErrorInvalidParameter};
    if (CPU &&
        (!Load || Program.Modules[Index].State != ModuleState::Prepared) &&
        Validated.insert(Index).second)
      if (auto E = validateMetadata(Program, Index, Budget, *CPU))
        return std::move(E);
    const auto &Module = Program.Modules[Index];
    const uint32_t MissingError =
        Ordinal ? ErrorInvalidOrdinal : ErrorProcedureNotFound;
    std::optional<size_t> Entry;
    if (Ordinal) {
      auto I = Module.Ordinals.find(*Ordinal);
      if (I != Module.Ordinals.end())
        Entry = I->second;
    } else {
      auto I = Module.Names.find(Symbol);
      if (I != Module.Names.end())
        Entry = I->second;
    }
    if (!Entry)
      return ExportResolution{std::nullopt, MissingError};
    if (!Visited.emplace(Index, *Entry).second)
      return failure(text::ForwarderCycle);
    const auto &Export = Module.Loaded.Exports.Entries[*Entry];
    if (Export.Kind == PEExportKind::Hole && !Depth)
      return ExportResolution{std::nullopt, MissingError};
    // A forwarded zero RVA yields the image base on native Windows. Preserve
    // that pointer without treating the header as executable code.
    if (Export.Kind == PEExportKind::Address ||
        Export.Kind == PEExportKind::Hole)
      return ExportResolution{Module.Loaded.Base + Export.RVA, 0};

    // Own these strings before Load can append and relocate the module vector.
    const auto [Library, Target] =
        llvm::StringRef(Export.Forwarder).rsplit(text::ForwarderSeparator);
    if (Library.empty() || Target.empty())
      return failure(text::ModuleForwarder);
    std::string LibraryName = Library.str();
    if (!Library.ends_with_insensitive(text::DLLExtension))
      LibraryName += text::DLLExtension;
    auto Key = moduleName(LibraryName);
    if (!Key)
      return Key.takeError();
    Symbol = Target.str();
    Ordinal.reset();
    if (llvm::StringRef(Symbol).starts_with(text::ForwarderOrdinal)) {
      const auto Digits = llvm::StringRef(Symbol).drop_front();
      uint16_t Number;
      if (Digits.empty() || !llvm::all_of(Digits, llvm::isDigit) ||
          Digits.getAsInteger(ForwarderOrdinalRadix, Number))
        return failure(text::ModuleForwarder);
      Ordinal = Number;
    }
    if (findProvider(*Key)) {
      if (Ordinal || !findService(*Key, Symbol))
        return failure(text::ModuleForwarder);
      auto Gate = Program.ServiceGates.find({*Key, Symbol});
      if (Gate == Program.ServiceGates.end())
        return failure(text::Service);
      return ExportResolution{Gate->second, 0};
    }
    if (Load) {
      auto Next = Load(Index, *Key);
      if (!Next) {
        auto E = Next.takeError();
        // Native lookup reports a missing forwarded library as a missing
        // procedure, while an explicit LoadLibrary still reports error 126.
        uint32_t Code = 0;
        E = llvm::handleErrors(std::move(E), [&](const ModuleLoadError &F) {
          Code =
              F.Code == ErrorModuleNotFound ? ErrorProcedureNotFound : F.Code;
        });
        if (E)
          return std::move(E);
        return ExportResolution{std::nullopt, Code};
      }
      Index = *Next;
    } else {
      auto Next = findModule(Program, *Key);
      if (!Next)
        return failure(text::ForwarderLoad + *Key);
      Index = *Next;
    }
  }
  return failure(text::ForwarderDepth);
}
} // namespace neverd::emulation::windows_process
