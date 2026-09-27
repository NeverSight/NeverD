//===- MobileAndroidInventory.cpp - Bounded APK/DEX class queries ---------===//
#include "MobileAndroidQuery.h"

#include <algorithm>
#include <iterator>

namespace neverd::mobile {
void visitAndroidDexes(
    const fs::path &input, Budget &budget,
    const std::function<void(std::string_view, std::string_view)> &visit) {
  if (!visit)
    throw Error("Android bytecode query requires a visitor");
  auto suffix = lowerASCII(pathText(input.extension()));
  if (suffix == ".dex") {
    auto bytes = readFile(input, budget.limits.max_bytes);
    visit(pathText(input.filename()), bytes);
  } else if (suffix == ".apk") {
    uint64_t count = 0;
    visitZipMembers(
        input, budget,
        [](const fs::path &path) {
          return path.parent_path().empty() &&
                 androidDexName(pathText(path.filename()));
        },
        [&](const fs::path &path, std::string_view bytes) {
          ++count;
          visit(pathText(path), bytes);
        });
    if (!count)
      throw Error("APK contains no root classes.dex or classesN.dex bytecode");
  } else {
    throw Error("Android bytecode query requires an APK or DEX file");
  }
  budget.check();
}

void validateAndroidClassIdentities(const std::vector<std::string> &classes,
                                    Budget &budget) {
  if (classes.size() > budget.limits.max_files)
    throw Error("Android class inventory exceeds the file limit");
  uint64_t name_bytes = 0;
  for (const auto &name : classes) {
    budget.tick();
    if (!llvm::json::isUTF8(name))
      throw Error("Android class identity is not representable as UTF-8");
    if (name.size() > budget.limits.max_bytes - name_bytes)
      throw Error("Android class inventory exceeds the byte limit");
    name_bytes += name.size();
  }
  // Sorting views avoids copying strings or changing the publication order.
  std::vector<std::string_view> identities(classes.begin(), classes.end());
  std::sort(identities.begin(), identities.end());
  auto duplicate = std::adjacent_find(identities.begin(), identities.end());
  if (duplicate != identities.end())
    throw Error("duplicate Android class definition: " +
                std::string(*duplicate));
  budget.check();
}

AndroidClassInventory listAndroidClasses(const fs::path &input,
                                         std::string_view prefix,
                                         Budget &budget) {
  std::string normalized(prefix);
  if (!normalized.empty() && (!normalized.starts_with('L') ||
                              normalized.find('.') != std::string::npos)) {
    std::replace(normalized.begin(), normalized.end(), '.', '/');
    normalized.insert(0, "L");
  }
  AndroidClassInventory result;
  uint64_t name_bytes = 0;
  visitAndroidDexes(
      input, budget, [&](std::string_view, std::string_view bytes) {
        auto classes = dalvik::listDexClasses(bytes, budget);
        if (classes.size() > budget.limits.max_files - result.total_class_count)
          throw Error("Android class inventory exceeds the file limit");
        result.total_class_count += classes.size();
        for (const auto &name : classes) {
          if (name.size() > budget.limits.max_bytes - name_bytes)
            throw Error("Android class inventory exceeds the byte limit");
          name_bytes += name.size();
        }
        ++result.dex_count;
        result.classes.insert(result.classes.end(),
                              std::make_move_iterator(classes.begin()),
                              std::make_move_iterator(classes.end()));
      });
  validateAndroidClassIdentities(result.classes, budget);
  std::erase_if(result.classes, [&](const std::string &name) {
    budget.tick();
    if (!name.starts_with(normalized))
      return true;
    budget.output(name.size() + 1);
    return false;
  });
  budget.check();
  return result;
}
} // namespace neverd::mobile
