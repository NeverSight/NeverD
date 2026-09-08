//===- MobileFile.h - Exclusive streaming output in owned staging ---------===//
#pragma once
#include "MobileCommon.h"

namespace neverd::mobile {
class OutputFile {
  intptr_t handle = -1;

public:
  explicit OutputFile(const fs::path &path);
  OutputFile(const OutputFile &) = delete;
  OutputFile &operator=(const OutputFile &) = delete;
  ~OutputFile();
  void write(std::string_view bytes);
  void close();
};
} // namespace neverd::mobile
