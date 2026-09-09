#pragma once
#include "Protocol.h"

#include <map>

namespace neverd::worker {
class Contributions {
public:
  Contributions();
  Json listing() const;
  Json registerFile(const std::string &path);
  Json remove(const std::string &nameSpace);
  Json query(const std::string &id, const std::string &address) const;

private:
  std::map<std::string, Json> manifests_;
  std::uint64_t revision_ = 1;
};
} // namespace neverd::worker
