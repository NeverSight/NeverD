///
/// \file
/// DriverNetworkInterfaces.
///

#include "neverd/emulation/DriverNetworkInterfaces.h"

#include "../WindowsUnicode.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <set>
#include <tuple>
namespace neverd::emulation {
namespace {
llvm::Error invalid(const llvm::Twine &T) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "network_interfaces: " + T);
}
#define NEVERD_NETWORK_ROW_FIELD(N, O, S)                                      \
  [[maybe_unused]] constexpr unsigned N##Offset = O;
#include "../kernel/KernelNetworkRow.def"
#undef NEVERD_NETWORK_ROW_FIELD
} // namespace
llvm::Error validateDriverNetworkInterfaces(const DriverOptions &O) {
  if (!O.NetworkInterfaces)
    return llvm::Error::success();
  if (!O.Version || O.Version->Major != 10 || O.Version->Minor != 0 ||
      O.Version->Build != 19041)
    return invalid("requires windows_version 10.0.19041");
  using InterfaceIdentity =
      std::tuple<uint64_t, uint32_t, std::array<uint8_t, 16>>;
  std::optional<std::set<InterfaceIdentity>> Identity;
  bool Any = false;
  for (const auto &Level : O.NetworkInterfaces->Levels) {
    if (!Level)
      continue;
    Any = true;
    if (Level->size() > DriverNetworkMaxRows)
      return invalid("exceeds 64 interface rows");
    std::set<uint64_t> Luids;
    std::set<uint32_t> Indices;
    std::set<InterfaceIdentity> Current;
    for (const auto &Row : *Level) {
      if (Row.size() != DriverNetworkRowSize)
        return invalid("row must contain exactly 1352 public ABI bytes");
      auto U32 = [&](unsigned At) {
        return llvm::support::endian::read32le(Row.data() + At);
      };
      const auto Luid = llvm::support::endian::read64le(Row.data());
      const auto Index = U32(InterfaceIndexOffset);
      if (!Luid || !Index || (Luid & 0xffffff) || !((Luid >> 24) & 0xffffff) ||
          !U32(TypeOffset) || U32(TypeOffset) > 0xffff ||
          (Luid >> 48) != U32(TypeOffset) || !Luids.insert(Luid).second ||
          !Indices.insert(Index).second)
        return invalid("requires distinct valid LUID/index identities and "
                       "matching interface types");
      std::array<uint8_t, 16> Guid;
      std::copy_n(Row.begin() + InterfaceGuidOffset, Guid.size(), Guid.begin());
      Current.emplace(Luid, Index, Guid);
      if (U32(PhysicalAddressLengthOffset) > 32)
        return invalid("physical address exceeds 32 bytes");
      for (auto At : {AliasOffset, DescriptionOffset}) {
        std::vector<uint16_t> Name;
        bool Terminated = false;
        for (unsigned I = 0; I < 257; ++I) {
          auto C = llvm::support::endian::read16le(Row.data() + At + I * 2);
          if (!C) {
            Terminated = true;
            break;
          }
          Name.push_back(C);
        }
        if (!Terminated)
          return invalid("interface names require a terminated WCHAR array");
        auto UTF8 = windows_unicode::utf8(Name);
        if (!UTF8)
          return invalid(llvm::toString(UTF8.takeError()));
      }
      const auto Tunnel = U32(TunnelTypeOffset);
      if ((Tunnel != 0 && Tunnel != 1 && Tunnel != 2 && Tunnel != 11 &&
           Tunnel != 13 && Tunnel != 14 && Tunnel != 15) ||
          (U32(TypeOffset) != 131 && Tunnel) || U32(MediaTypeOffset) >= 20 ||
          U32(PhysicalMediumTypeOffset) >= 21 || U32(AccessTypeOffset) < 1 ||
          U32(AccessTypeOffset) > 4 || U32(DirectionTypeOffset) > 2 ||
          U32(OperStatusOffset) < 1 || U32(OperStatusOffset) > 7 ||
          U32(AdminStatusOffset) < 1 || U32(AdminStatusOffset) > 3 ||
          U32(MediaConnectStateOffset) > 2 || U32(ConnectionTypeOffset) < 1 ||
          U32(ConnectionTypeOffset) > 3)
        return invalid("row contains an invalid 19041 interface enum value");
    }
    if (Identity && *Identity != Current)
      return invalid(
          "observed levels must describe the same interface identities");
    Identity = std::move(Current);
  }
  if (!Any)
    return invalid("requires at least one explicitly observed level");
  return llvm::Error::success();
}
} // namespace neverd::emulation
