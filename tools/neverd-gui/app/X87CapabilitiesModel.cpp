//===- X87CapabilitiesModel.cpp - Shared x87 inventory view
//----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Joins instruction forms to the report's explicit coverage dimensions.
//===----------------------------------------------------------------------===//

#include "X87CapabilitiesModel.h"

#include <QJsonDocument>
#include <QSet>

namespace neverd::gui {
namespace {
const QStringList CoverageDimensions{"coverage_class", "source_mode",
                                     "state_path",     "consumer",
                                     "backend",        "destination"};
const QStringList SourceDimensions{"source_mode", "abi", "state_path"};

bool strings(const QJsonObject &Object, const QStringList &Keys) {
  for (const auto &Key : Keys)
    if (!Object.value(Key).isString() || Object.value(Key).toString().isEmpty())
      return false;
  return true;
}
bool unique(const QJsonObject &Object, const QStringList &Keys,
            QSet<QByteArray> &Seen) {
  QJsonArray Key;
  for (const auto &Name : Keys)
    Key.append(Object.value(Name));
  const auto Bytes = QJsonDocument(Key).toJson(QJsonDocument::Compact);
  if (Seen.contains(Bytes))
    return false;
  Seen.insert(Bytes);
  return true;
}
bool matches(const QJsonObject &Cell, const QJsonObject &Dimensions,
             const QStringList &Keys) {
  for (const auto &Key : Keys)
    if (Key != QLatin1String("coverage_class") &&
        Cell.value(Key) != Dimensions.value(Key))
      return false;
  return true;
}
} // namespace

X87CapabilitiesModel::X87CapabilitiesModel(QObject *Parent)
    : QAbstractTableModel(Parent) {}

bool X87CapabilitiesModel::setReport(const QJsonObject &Value, QString &Error) {
  clear();
  Error = tr("The engine returned an incompatible x87 capability report.");
  // Schema 1 is the shared public report contract, not a GUI capability list.
  if (Value.value("schema_version") != QJsonValue(1) ||
      !strings(Value, {"scope", "matrix_contract", "precision", "evidence"}))
    return false;
  for (const auto *Key : {"forms", "coverage", "source_paths"})
    if (!Value.value(Key).isArray() || Value.value(Key).toArray().isEmpty())
      return false;
  QSet<QByteArray> Seen;
  for (const auto &Item : Value.value("forms").toArray()) {
    const auto F = Item.toObject();
    if (!strings(F, {"id", "mnemonic", "coverage_class"}) ||
        !F.value("operands").isString() || !unique(F, {"id"}, Seen))
      return false;
  }
  Seen.clear();
  for (const auto &Item : Value.value("coverage").toArray()) {
    const auto C = Item.toObject();
    if (!strings(C, CoverageDimensions) ||
        !strings(C,
                 {"numerical", "architectural_state", "profile", "reason"}) ||
        !C.value("implemented").isBool() ||
        !C.value("requirements").isString() ||
        !C.value("test_sources").isString() ||
        !unique(C, CoverageDimensions, Seen))
      return false;
  }
  Seen.clear();
  for (const auto &Item : Value.value("source_paths").toArray()) {
    const auto S = Item.toObject();
    if (!strings(S, SourceDimensions) ||
        !strings(S, {"long_double", "call_contract"}) ||
        !S.value("available").isBool() || !S.value("default_path").isBool() ||
        !unique(S, SourceDimensions, Seen))
      return false;
  }
  beginResetModel();
  Report = Value;
  Forms = Value.value("forms").toArray();
  selectCells();
  endResetModel();
  Error.clear();
  return true;
}

void X87CapabilitiesModel::clear() {
  beginResetModel();
  Report = {};
  Forms = {};
  Cells.clear();
  endResetModel();
}

void X87CapabilitiesModel::setDimensions(const QJsonObject &Value) {
  Dimensions = Value;
  selectCells();
  if (!Forms.isEmpty())
    emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
}

void X87CapabilitiesModel::selectCells() {
  Cells.clear();
  for (const auto &Item : Report.value("coverage").toArray()) {
    const auto C = Item.toObject();
    if (matches(C, Dimensions, CoverageDimensions))
      Cells.insert(C.value("coverage_class").toString(), C);
  }
}

QStringList X87CapabilitiesModel::values(const QString &Dimension) const {
  QSet<QString> Values;
  for (const auto &Item :
       Report
           .value(Dimension == QLatin1String("abi") ? "source_paths"
                                                    : "coverage")
           .toArray())
    Values.insert(Item.toObject().value(Dimension).toString());
  Values.remove(QString());
  QStringList Result(Values.begin(), Values.end());
  Result.sort();
  return Result;
}

QJsonObject X87CapabilitiesModel::form(int Row) const {
  return Row >= 0 && Row < Forms.size() ? Forms.at(Row).toObject()
                                        : QJsonObject{};
}

QJsonObject X87CapabilitiesModel::capability(int Row) const {
  return Cells.value(form(Row).value("coverage_class").toString());
}

QJsonObject X87CapabilitiesModel::sourcePath() const {
  for (const auto &Item : Report.value("source_paths").toArray())
    if (matches(Item.toObject(), Dimensions, SourceDimensions))
      return Item.toObject();
  return {};
}

int X87CapabilitiesModel::rowCount(const QModelIndex &Parent) const {
  return Parent.isValid() ? 0 : Forms.size();
}
int X87CapabilitiesModel::columnCount(const QModelIndex &Parent) const {
  return Parent.isValid() ? 0 : 6;
}

QVariant X87CapabilitiesModel::data(const QModelIndex &Index, int Role) const {
  if (!Index.isValid() || Index.row() >= rowCount() ||
      (Role != Qt::DisplayRole && Role != Qt::ToolTipRole))
    return {};
  const auto F = form(Index.row());
  const auto C = capability(Index.row());
  if (Role == Qt::ToolTipRole)
    return C.isEmpty()
               ? tr("This combination is not listed; it is unsupported.")
               : C.value("reason").toString();
  switch (Index.column()) {
  case 0:
    return F.value("mnemonic").toString();
  case 1:
    return F.value("operands").toString();
  case 2:
    return C.isEmpty()                       ? tr("Unlisted")
           : C.value("implemented").toBool() ? tr("Yes")
                                             : tr("No");
  case 3:
    return C.value("numerical").toString(tr("unsupported"));
  case 4:
    return C.value("architectural_state").toString(tr("unsupported"));
  case 5:
    return C.value("profile").toString(tr("unqualified"));
  default:
    return {};
  }
}

QVariant X87CapabilitiesModel::headerData(int Section,
                                          Qt::Orientation Orientation,
                                          int Role) const {
  if (Orientation != Qt::Horizontal || Role != Qt::DisplayRole)
    return {};
  switch (Section) {
  case 0:
    return tr("Instruction");
  case 1:
    return tr("Operands");
  case 2:
    return tr("Implemented");
  case 3:
    return tr("Numerical result");
  case 4:
    return tr("Architectural state");
  case 5:
    return tr("Profile");
  default:
    return {};
  }
}
} // namespace neverd::gui
