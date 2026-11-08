//===- PageDelta.cpp - A kept page with what changed applied ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The operations neverd::worker::pageDelta writes: a field set or taken away,
/// lines of a text replaced where the text has as many, and runs over a list
/// that keep, shift the numbers of, replace, drop or add its elements.
//===----------------------------------------------------------------------===//

#include "PageDelta.h"

#include <QStringList>

namespace page_delta {
namespace {

std::optional<qint64> count(const QJsonValue &value) {
  if (!value.isDouble())
    return std::nullopt;
  const double number = value.toDouble();
  const auto whole = static_cast<qint64>(number);
  if (number < 0 || static_cast<double>(whole) != number)
    return std::nullopt;
  return whole;
}

std::optional<qint64> integer(const QJsonValue &value) {
  if (!value.isDouble())
    return std::nullopt;
  const double number = value.toDouble();
  const auto whole = static_cast<qint64>(number);
  if (static_cast<double>(whole) != number)
    return std::nullopt;
  return whole;
}

} // namespace

std::optional<QJsonObject> apply(QJsonObject page, const QJsonArray &delta) {
  for (const auto &entry : delta) {
    const auto operation = entry.toArray();
    if (operation.size() < 2 || !operation[0].isString() ||
        !operation[1].isString())
      return std::nullopt;
    const auto kind = operation[0].toString();
    const auto field = operation[1].toString();
    if (kind == QLatin1String("set") && operation.size() == 3) {
      page[field] = operation[2];
      continue;
    }
    if (kind == QLatin1String("unset") && operation.size() == 2) {
      if (!page.contains(field))
        return std::nullopt;
      page.remove(field);
      continue;
    }
    if (!page.contains(field))
      return std::nullopt;
    if (kind == QLatin1String("lines") && operation.size() == 4 &&
        page[field].isString() && operation[3].isArray()) {
      auto lines =
          page[field].toString().split(QLatin1Char('\n'), Qt::KeepEmptyParts);
      const auto total = count(operation[2]);
      if (!total || lines.size() != *total)
        return std::nullopt;
      for (const auto &change : operation[3].toArray()) {
        const auto pair = change.toArray();
        const auto line = pair.size() == 2 ? count(pair[0]) : std::nullopt;
        if (!line || *line >= lines.size() || !pair[1].isString())
          return std::nullopt;
        lines[*line] = pair[1].toString();
      }
      page[field] = lines.join(QLatin1Char('\n'));
      continue;
    }
    if (kind == QLatin1String("items") && operation.size() == 3 &&
        page[field].isArray() && operation[2].isArray()) {
      const auto old = page[field].toArray();
      QJsonArray items;
      qsizetype used = 0;
      for (const auto &runValue : operation[2].toArray()) {
        const auto run = runValue.toArray();
        if (run.size() < 2 || !run[0].isString())
          return std::nullopt;
        const auto how = run[0].toString();
        const bool shift = how == QLatin1String("shift");
        if (how == QLatin1String("same") || how == QLatin1String("skip") ||
            shift) {
          const auto runLength = count(run[1]);
          if (!runLength || *runLength > old.size() - used ||
              run.size() != (shift ? 3 : 2) || (shift && !run[2].isObject()))
            return std::nullopt;
          const auto by = shift ? run[2].toObject() : QJsonObject();
          for (qint64 item = 0;
               how != QLatin1String("skip") && item < *runLength; ++item) {
            QJsonValue value = old[used + item];
            if (shift) {
              if (!value.isObject())
                return std::nullopt;
              auto object = value.toObject();
              for (auto add = by.begin(); add != by.end(); ++add) {
                const auto from = object.contains(add.key())
                                      ? integer(object[add.key()])
                                      : std::nullopt;
                const auto plus = integer(add.value());
                if (!from || !plus)
                  return std::nullopt;
                object[add.key()] = *from + *plus;
              }
              value = object;
            }
            items.append(value);
          }
          used += *runLength;
        } else if ((how == QLatin1String("put") ||
                    how == QLatin1String("new")) &&
                   run.size() == 2 && run[1].isArray()) {
          const auto values = run[1].toArray();
          if (how == QLatin1String("put")) {
            if (values.size() > old.size() - used)
              return std::nullopt;
            used += values.size();
          }
          for (const auto &value : values)
            items.append(value);
        } else {
          return std::nullopt;
        }
      }
      if (used != old.size())
        return std::nullopt;
      page[field] = items;
      continue;
    }
    return std::nullopt;
  }
  return page;
}

} // namespace page_delta
