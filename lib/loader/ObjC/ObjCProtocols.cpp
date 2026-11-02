#include "ObjCProtocols.h"

#include "ObjCMethodLists.h"
#include "ObjCProperties.h"
#include "ObjCRuntimeData.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/Limits.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd::objc {
namespace {

class ProtocolReader {
  BinaryImage &Image;
  RuntimeData Data;
  ImportStorageSlotCollection Imports;
  size_t MethodsRemaining = ::neverd::limits::loader::kObjCProtocolsMaxRecords;
  size_t EdgesRemaining = ::neverd::limits::loader::kObjCProtocolsMaxRecords;
  std::map<va_t, ObjCProtocol> Protocols;
  std::vector<va_t> Work;

  void diagnostic(llvm::StringRef Message) {
    auto &Messages = Image.ObjCMetadataDiagnostics;
    if (Messages.size() < ::neverd::limits::loader::
                              kObjCProtocolsDiagnosticMessagesSizeLimit &&
        std::find(Messages.begin(), Messages.end(), Message) == Messages.end())
      Messages.push_back(Message.str());
  }

  bool enqueue(va_t Address) {
    if (Protocols.count(Address))
      return true;
    if (Protocols.size() >=
        ::neverd::limits::loader::kObjCProtocolsMaxRecords) {
      diagnostic("Objective-C protocol count exceeds the parsing budget");
      return false;
    }
    Protocols[Address].Address = Address;
    Work.push_back(Address);
    return true;
  }

  bool inheritance(ObjCProtocol &Protocol) {
    const auto List = Data.localPointer(Protocol.Address + 16);
    if (!List)
      return false;
    if (!*List)
      return true;
    const auto *Bytes = Data.bytes(*List, 8);
    if (!Bytes)
      return false;
    const auto Count = llvm::support::endian::read64le(Bytes);
    if (Count > EdgesRemaining || !Data.bytes(*List, 8 + Count * 8))
      return false;
    EdgesRemaining -= Count;
    bool Valid = true;
    std::set<va_t> Seen;
    for (size_t I = 0; I < Count; ++I) {
      const auto Parent = Data.localPointer(*List + 8 + I * 8);
      if (!Parent || !*Parent || !enqueue(*Parent)) {
        Valid = false;
        continue;
      }
      if (Seen.insert(*Parent).second)
        Protocol.AdoptedProtocols.push_back(*Parent);
    }
    return Valid;
  }

  bool methods(ObjCProtocol &Protocol, unsigned Kind) {
    const auto List = Data.localPointer(Protocol.Address + 24 + Kind * 8);
    if (!List)
      return false;
    if (!*List)
      return true;
    std::string Diagnostic;
    auto Records = readMethodList(Image, *List, MethodsRemaining, Diagnostic);
    if (!Records) {
      diagnostic(Diagnostic);
      return false;
    }
    bool Valid = true;
    for (auto &Record : *Records) {
      ObjCProtocolMethod Method;
      Method.MetadataAddress = Record.Address;
      Method.CacheOrigin = Record.CacheOrigin;
      Method.Selector = std::move(Record.Selector);
      Method.TypeEncoding = std::move(Record.TypeEncoding);
      Method.IsClassMethod = (Kind & 1) != 0;
      Method.IsOptional = Kind >= 2;
      if (!Record.Implementation || *Record.Implementation ||
          Method.Selector.empty() || Method.TypeEncoding.empty()) {
        Method.Status = "invalid_metadata";
        Valid = false;
      } else {
        Method.TypeHint =
            parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
        if (!Method.TypeHint) {
          Method.Status = "unsupported_encoding";
        } else if (!assignDarwinObjCSourceABI(*Method.TypeHint, Image.Arch,
                                              Diagnostic)) {
          Method.TypeHint.reset();
          Method.Status = "unsupported_abi";
        } else {
          Method.Status = "supported";
        }
      }
      Protocol.Methods.push_back(std::move(Method));
    }
    return Valid;
  }

  void read(ObjCProtocol &Protocol) {
    Protocol.Status = "invalid_metadata";
    const auto Address = Protocol.Address;
    // The non-fragile 64-bit record has a 72-byte common prefix. Later fields
    // are optional; the on-disk size determines their presence.
    if (!Data.bytes(Address, 72)) {
      diagnostic("Truncated or non-file-backed Objective-C protocol record");
      return;
    }
    const auto Size = Data.u32(Address + 64);
    if (!Size || *Size < 72 ||
        *Size > ::neverd::limits::loader::kObjCProtocolsReadSizeLimit ||
        !Data.bytes(Address, *Size)) {
      diagnostic("Invalid Objective-C protocol record size");
      return;
    }
    const auto Name = Data.localPointer(Address + 8);
    const auto Text = Name ? Data.string(*Name) : std::nullopt;
    if (!Text) {
      diagnostic("Objective-C protocol name is unavailable");
      return;
    }
    Protocol.Name = *Text;
    bool Valid = inheritance(Protocol);
    for (unsigned Kind = 0; Kind < 4; ++Kind)
      Valid = methods(Protocol, Kind) && Valid;
    ObjCProperty Owner;
    Owner.Owner = ObjCProperty::OwnerKind::Protocol;
    Owner.OwnerAddress = Address;
    Owner.OwnerName = Protocol.Name;
    Valid =
        readPropertyList(Image, Address + 56, Owner, MethodsRemaining) && Valid;
    if (*Size >= 96) {
      Owner.IsClassProperty = true;
      Valid = readPropertyList(Image, Address + 88, Owner, MethodsRemaining) &&
              Valid;
    }
    if (!Valid)
      diagnostic(
          "Objective-C protocol declarations or inheritance are incomplete");
    else
      Protocol.Status = "recovered";
  }

  void validateInheritance() {
    // Process parents before dependents without recursive traversal. Nodes
    // left over belong to a cycle or depend on one, so none can supply hints.
    std::map<va_t, size_t> Pending;
    std::map<va_t, std::vector<va_t>> Dependents;
    std::vector<va_t> Ready;
    for (const auto &[Address, Protocol] : Protocols) {
      Pending[Address] = Protocol.AdoptedProtocols.size();
      if (Protocol.AdoptedProtocols.empty())
        Ready.push_back(Address);
      for (auto Parent : Protocol.AdoptedProtocols)
        Dependents[Parent].push_back(Address);
    }
    for (size_t I = 0; I < Ready.size(); ++I) {
      const auto Address = Ready[I];
      for (auto Child : Dependents[Address]) {
        if (Protocols[Address].Status != "recovered")
          Protocols[Child].Status = "invalid_inheritance";
        if (--Pending[Child] == 0)
          Ready.push_back(Child);
      }
    }
    for (auto &[Address, Protocol] : Protocols) {
      if (Pending[Address]) {
        Protocol.Status = "invalid_inheritance";
        diagnostic("Objective-C protocol inheritance contains a cycle");
      }
      if (Protocol.Status != "recovered")
        for (auto &Method : Protocol.Methods) {
          Method.TypeHint.reset();
          if (Method.Status == "supported")
            Method.Status = Protocol.Status;
        }
    }
    for (auto &Property : Image.ObjCProperties)
      if (Property.Owner == ObjCProperty::OwnerKind::Protocol) {
        const auto &Protocol = Protocols.at(Property.OwnerAddress);
        if (Protocol.Status != "recovered") {
          Property.GetterTypeHint.reset();
          Property.SetterTypeHint.reset();
          Property.Status = Protocol.Status;
        }
      }
  }

public:
  explicit ProtocolReader(BinaryImage &Image)
      : Image(Image), Data(Image, PointerView::MetadataTarget),
        Imports(Image.collectImportStorageSlots()) {}

  void run() {
    size_t RootsRemaining = ::neverd::limits::loader::kObjCProtocolsMaxRecords;
    for (const auto &Section : Image.Sections) {
      if (Section.Name != "__objc_protolist" &&
          Section.Name != "__objc_protorefs")
        continue;
      if (Section.Size % 8 || Section.Size / 8 > RootsRemaining ||
          Image.getSectionFor(Section.VA) != &Section ||
          !Data.bytes(Section.VA, Section.Size)) {
        diagnostic("Invalid or non-file-backed Objective-C protocol list");
        continue;
      }
      RootsRemaining -= Section.Size / 8;
      for (size_t I = 0; I < Section.Size / 8; ++I) {
        const auto Slot = Section.VA + I * 8;
        // A reference to an external protocol carries no local declaration.
        if (Section.Name == "__objc_protorefs" && Imports.Slots.count(Slot) &&
            !Imports.Conflicts.count(Slot))
          continue;
        const auto Address = Data.localPointer(Slot);
        if (!Address || !*Address) {
          diagnostic("Objective-C protocol address is unavailable");
          continue;
        }
        enqueue(*Address);
      }
    }
    for (size_t I = 0; I < Work.size(); ++I)
      read(Protocols.at(Work[I]));
    validateInheritance();
    for (auto &[Address, Protocol] : Protocols)
      Image.ObjCProtocols.push_back(std::move(Protocol));
  }
};
} // namespace

void readProtocolDeclarations(BinaryImage &Image) {
  ProtocolReader(Image).run();
}
} // namespace neverd::objc
