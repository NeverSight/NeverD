//===- ComparisonModel.cpp - On-demand comparison rows --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keeps only recently displayed pages. Generation checks discard late replies
/// after navigation, filtering or edits without repainting stale evidence.
//===----------------------------------------------------------------------===//

#include "ComparisonModel.h"

#include "../ResourceLimits.h"
#include "ComparisonSession.h"
#include "Theme.h"

#include <QHash>
#include <QJsonDocument>
#include <QTextDocument>
#include <QTimer>
#include <utility>

namespace neverd::gui {
namespace {
constexpr int PageSize = ::neverd::limits::gui::kComparisonPageSize;
QString number(double Value) {
  return QString::number(Value * 100, 'f', 1) + '%';
}
QString code(const QJsonObject &I) {
  return I["mnemonic"].toString() + ' ' + I["operands"].toString();
}
QString originAddresses(const QJsonObject &Node) {
  QStringList Addresses;
  for (const auto &Origin : Node["origins"].toArray())
    Addresses << Origin.toObject()["address"].toString();
  if (Addresses.isEmpty() && Node["origin_sequence"].toInt(-1) >= 0)
    Addresses << Node["address"].toString();
  Addresses.removeDuplicates();
  return Addresses.join(", ");
}
QString names(const QJsonArray &Functions) {
  QStringList Names;
  for (const auto &F : Functions)
    Names << F.toObject()["name"].toString();
  return Names.join(", ");
}
} // namespace
ComparisonModel::ComparisonModel(ComparisonSession &S, Kind K, QObject *Parent)
    : QAbstractTableModel(Parent), Session(S), Type(K),
      Pages(::neverd::limits::gui::kComparisonCachedPages) {}
QString ComparisonModel::changeStateText(const QString &State) {
  if (State == "unmeasured")
    return tr("Unmeasured");
  if (State == "identity_only")
    return tr("Identity only");
  if (State == "changed")
    return tr("Changed");
  if (State == "incomplete")
    return tr("Incomplete");
  if (State == "uncertain")
    return tr("Uncertain");
  if (State == "unchanged")
    return tr("Unchanged");
  return State;
}

QString ComparisonModel::functionNamesText(const QJsonObject &Function) {
  const auto Display = Function["name"].toString();
  const auto Analyzed = Function["evidence_name"].toString();
  return Analyzed.isEmpty() || Analyzed == Display
             ? Display
             : tr("%1 (analyzed name: %2)").arg(Display, Analyzed);
}
QString ComparisonModel::parameterViewText(const QJsonObject &V) {
  if (!V["elided_truncations"].toInt())
    return {};
  return tr("param %1: low %2 of %3 bits")
      .arg(V["parameter"].toInt())
      .arg(V["view_bits"].toInt())
      .arg(V["source_bits"].toInt());
}
QString ComparisonModel::parameterViewsText(const QJsonObject &N) {
  QStringList Views;
  for (const auto &V : N["parameter_views"].toArray()) {
    const auto Text = parameterViewText(V.toObject());
    if (!Text.isEmpty())
      Views << Text;
  }
  Views.removeDuplicates();
  return Views.join(", ");
}
QString ComparisonModel::callEffectText(const QJsonObject &Effect) {
  auto Status = Effect["status"].toString();
  if (Status == "qualified_contract") {
    auto Family = Effect["family"].toString().replace('_', ' ');
    return Effect["release_may_fail"].toBool()
               ? tr("Qualified %1 contract; release may fail").arg(Family)
               : tr("Qualified %1 contract").arg(Family);
  }
  for (const auto &[Key, Label] :
       {std::pair{"unresolved_target", tr("Unresolved call target")},
        {"internal_target", tr("Internal callee; review its body")},
        {"unqualified_identity", tr("No qualified import identity")},
        {"unsupported_environment", tr("Unsupported ABI or input profile")},
        {"ssa_unavailable", tr("SSA unavailable in collected scope")},
        {"missing_ssa_call", tr("No native-associated SSA call")},
        {"ambiguous_ssa_call", tr("Ambiguous SSA call association")},
        {"missing_arguments", tr("Recovered arguments unavailable")},
        {"unsupported_contract", tr("No applicable call contract")}})
    if (Status == Key)
      return Label;
  return Status.replace('_', ' ');
}
int ComparisonModel::rowCount(const QModelIndex &Parent) const {
  return Parent.isValid() ? 0 : Total;
}
int ComparisonModel::columnCount(const QModelIndex &Parent) const {
  return Parent.isValid()             ? 0
         : Type == Kind::Instructions ? 7
         : Type == Kind::Candidates   ? 5
                                      : 6;
}
QVariant ComparisonModel::headerData(int Section, Qt::Orientation Direction,
                                     int Role) const {
  if (Direction == Qt::Horizontal && Type == Kind::FlowAccesses &&
      Section == 4 && Role == Qt::ToolTipRole)
    return tr("A same-width COPY/PHI path to a RETURN operand. That operand "
              "may be a control target; this does not establish an ABI return "
              "value or execution order.");
  if (Role != Qt::DisplayRole || Direction != Qt::Horizontal)
    return {};
  if (Type == Kind::Candidates)
    return QStringList{tr("Address"), tr("Function"), tr("Bytes"),
                       tr("Pairing"), tr("Current counterpart")}
        .value(Section);
  const QStringList Functions{tr("Address"),     tr("Function"),
                              tr("Counterpart"), tr("Similarity"),
                              tr("Confidence"),  tr("State")};
  const QStringList Instructions{tr("Primary address"),       tr("Bytes"),
                                 tr("Primary instruction"),   tr("Change"),
                                 tr("Secondary instruction"), tr("Bytes"),
                                 tr("Secondary address")};
  const QStringList Regions{tr("Hypothesis"),     tr("Primary"),
                            tr("Secondary"),      tr("Shared instructions"),
                            tr("Coverage A / B"), tr("Confidence")};
  const QStringList Anchors{tr("Primary function"),   tr("Primary span"),
                            tr("Secondary function"), tr("Secondary span"),
                            tr("Instructions"),       tr("Seed evidence")};
  if (Type == Kind::FlowFunctions)
    return QStringList{tr("Function entry"), tr("Availability"), tr("Nodes"),
                       tr("Blocks"),         tr("Reason"),       tr("Evidence")}
        .value(Section);
  if (Type == Kind::Reviews)
    return QStringList{tr("Event"),
                       tr("Case"),
                       tr("Assessment"),
                       tr("Author"),
                       tr("Binding / history"),
                       tr("Note")}
        .value(Section);
  if (Type == Kind::Changes)
    return QStringList{tr("Priority"),
                       tr("Change"),
                       tr("Context"),
                       tr("Primary instruction"),
                       tr("Secondary instruction"),
                       tr("Association")}
        .value(Section);
  if (Type == Kind::ImpactFunctions)
    return QStringList{tr("Function entry"),
                       tr("Function"),
                       tr("Depth"),
                       tr("Entry evidence"),
                       tr("Witness instruction"),
                       tr("Gaps")}
        .value(Section);
  if (Type == Kind::ImpactSites)
    return QStringList{tr("Caller"),      tr("Original address"),
                       tr("Instruction"), tr("Callee"),
                       tr("Bytes"),       tr("Contract / gaps")}
        .value(Section);
  if (Type == Kind::FlowRegions)
    return QStringList{tr("Hypothesis"),     tr("Primary"),
                       tr("Secondary"),      tr("Shared nodes"),
                       tr("Coverage A / B"), tr("Boundaries")}
        .value(Section);
  if (Type == Kind::FlowRegionAnchors)
    return QStringList{tr("Primary function"),   tr("Primary root"),
                       tr("Secondary function"), tr("Secondary root"),
                       tr("Node pairs"),         tr("Seed evidence")}
        .value(Section);
  if (Type == Kind::FlowRegionNodes)
    return QStringList{tr("Primary node"),      tr("Primary origin"),
                       tr("Primary operation"), tr("Secondary node"),
                       tr("Secondary origin"),  tr("Secondary operation")}
        .value(Section);
  if (Type == Kind::FlowAccesses)
    return QStringList{tr("Argument"), tr("Original address"), tr("Access"),
                       tr("Width"),    tr("RETURN input"),     tr("Gaps")}
        .value(Section);
  if (Type == Kind::FlowNodes || Type == Kind::FlowSlice)
    return QStringList{tr("Node"),  tr("Original address"), tr("Operation"),
                       tr("Width"), tr("Inputs"),           tr("Gaps")}
        .value(Section);
  return (Type == Kind::Functions       ? Functions
          : Type == Kind::Regions       ? Regions
          : Type == Kind::RegionAnchors ? Anchors
                                        : Instructions)
      .value(Section);
}
QJsonObject ComparisonModel::row(int Index) const {
  if (Index < 0 || Index >= Total)
    return {};
  const int Page = Index / PageSize;
  if (const auto *Rows = Pages.object(Page))
    return Index % PageSize < Rows->size()
               ? Rows->at(Index % PageSize).toObject()
               : QJsonObject{};
  fetch(Page);
  return {};
}
QVariant ComparisonModel::data(const QModelIndex &Index, int Role) const {
  if (!Index.isValid())
    return {};
  const auto R = row(Index.row());
  if (R.isEmpty())
    return Role == Qt::DisplayRole && Index.column() == 1
               ? QVariant(tr("Loading…"))
               : QVariant();
  const auto E = R["evidence"].toObject();
  const auto K = R["kind"].toString();
  if (Role == Qt::ToolTipRole) {
    if (Type == Kind::FlowRegionNodes && Query["coordinates"] == "frontier")
      return tr("External input binding; the producers have not been "
                "compared.\n%1")
          .arg(QString::fromUtf8(
              QJsonDocument(R).toJson(QJsonDocument::Indented)));
    if (Type == Kind::Changes || Type == Kind::Reviews)
      return QString::fromUtf8(
          QJsonDocument(R).toJson(QJsonDocument::Indented));
    if (Type == Kind::ImpactFunctions || Type == Kind::ImpactSites)
      return tr("Observed call paths; feasibility and data effects remain "
                "unverified.\n%1")
          .arg(QString::fromUtf8(
              QJsonDocument(R).toJson(QJsonDocument::Indented)));
    if (Type == Kind::FlowRegionNodes &&
        (!parameterViewsText(R["primary"].toObject()).isEmpty() ||
         !parameterViewsText(R["secondary"].toObject()).isEmpty()))
      return tr("Primary: %1\nSecondary: %2\nEvery use of a projected "
                "parameter "
                "reads the same low bits. Whole-function behavior remains "
                "unverified.\n%3")
          .arg(parameterViewsText(R["primary"].toObject()),
               parameterViewsText(R["secondary"].toObject()),
               QString::fromUtf8(
                   QJsonDocument(R).toJson(QJsonDocument::Indented)));
    if (Type == Kind::FlowRegions || Type == Kind::FlowRegionAnchors ||
        Type == Kind::FlowRegionNodes)
      return tr("Finite SSA correspondence; open effects and control require "
                "review.\n%1")
          .arg(QString::fromUtf8(
              QJsonDocument(R).toJson(QJsonDocument::Indented)));
    if (Type == Kind::FlowFunctions)
      return R["reason"].toString();
    if ((Type == Kind::FlowNodes || Type == Kind::FlowSlice ||
         Type == Kind::FlowAccesses) &&
        R.contains("origins"))
      return (parameterViewsText(R).isEmpty()
                  ? QString()
                  : parameterViewsText(R) + '\n' +
                        tr("Every use of this parameter reads the same low "
                           "bits; "
                           "the original width and native site are retained.") +
                        '\n') +
             tr("Block %1 · Original addresses: %2\n%3")
                 .arg(R["block"].toInt())
                 .arg(originAddresses(R).isEmpty() ? tr("Synthetic")
                                                   : originAddresses(R))
                 .arg(QString::fromUtf8(
                     QJsonDocument(R).toJson(QJsonDocument::Indented)));
    if (Type == Kind::FlowNodes || Type == Kind::FlowSlice ||
        Type == Kind::FlowAccesses)
      return tr("Block %1 · Original operation %2\n%3")
          .arg(R["block"].toInt())
          .arg(R["origin_sequence"].toInt(-1))
          .arg(QString::fromUtf8(
              QJsonDocument(R).toJson(QJsonDocument::Indented)));
    if (Type == Kind::Regions)
      return tr("Primary functions: %1\nSecondary functions: %2\nHeuristic "
                "explanations, not validated behavior.\n%3")
          .arg(names(R["primary_functions"].toArray()),
               names(R["secondary_functions"].toArray()), R["id"].toString());
    if (Type == Kind::Regions || Type == Kind::RegionAnchors)
      return tr("Shared normalized instruction runs are evidence for competing "
                "transformation hypotheses. "
                "They do not validate behavior, calls, memory effects or patch "
                "presence.") +
             '\n' + R["id"].toString();
    if (Type == Kind::Functions || Type == Kind::Candidates) {
      QString Names = functionNamesText(R);
      const auto Counterpart = R["counterpart"].toObject();
      if (!Counterpart.isEmpty())
        Names +=
            '\n' + tr("Counterpart: %1").arg(functionNamesText(Counterpart));
      return Qt::convertFromPlainText(
          Names + "\n\n" + E["explanation"].toString() + '\n' +
          tr("Heuristic confidence; structural similarity does not prove "
             "semantic equivalence.") +
          '\n' + R["identity"].toString());
    }
    return code(R["primary"].toObject()) + '\n' +
           code(R["secondary"].toObject());
  }
  if (Role == Qt::FontRole &&
      (Type == Kind::Instructions || Index.column() == 0))
    return Theme::instance().codeFont();
  if (Role == Qt::ForegroundRole && Type == Kind::Instructions &&
      Index.column() == 3) {
    if (K == "inserted")
      return Theme::instance().color(ColorRole::DiffAdded);
    if (K == "deleted")
      return Theme::instance().color(ColorRole::DiffRemoved);
    if (K == "uncertain")
      return Theme::instance().chrome(QStringLiteral("SecondaryText"));
    if (K != "identical" && K != "moved")
      return Theme::instance().color(ColorRole::DiffChanged);
  }
  if (Role != Qt::DisplayRole)
    return {};
  if (Type == Kind::ImpactFunctions || Type == Kind::ImpactSites) {
    QStringList Gaps;
    for (const auto &G : R["gaps"].toArray()) {
      const auto Key = G.toString();
      Gaps << (Key == "unresolved_indirect_call" ? tr("Unknown indirect call")
               : Key == "unresolved_indirect_transfer"
                   ? tr("Unknown indirect transfer")
               : Key == "unresolved_direct_call"
                   ? tr("Direct callee unavailable")
               : Key == "native_coverage"     ? tr("Incomplete native coverage")
               : Key == "exceptional_control" ? tr("Exception flow")
               : Key == "external_body"       ? tr("External function body")
               : Key == "incomplete_discovery" ? tr("Incomplete discovery")
                                               : Key);
    }
    if (Type == Kind::ImpactFunctions) {
      if (!R["scan_complete"].toBool())
        Gaps << tr("Scan incomplete");
      QStringList Entries;
      if (R["entry_point"].toBool())
        Entries << tr("Program entry");
      if (R["exported"].toBool())
        Entries << tr("Export");
      switch (Index.column()) {
      case 0:
        return R["address"].toString();
      case 1:
        return R["name"].toString();
      case 2:
        return R["depth"].toInt();
      case 3:
        return Entries.join(", ");
      case 4:
        return R["witness"]
            .toObject()["instruction"]
            .toObject()["address"]
            .toString();
      case 5:
        return Gaps.join(", ");
      }
    } else {
      const auto I = R["instruction"].toObject();
      const auto Effect = R["effect"].toObject();
      if (!Effect.isEmpty())
        Gaps.prepend(callEffectText(Effect));
      switch (Index.column()) {
      case 0:
        return R["caller"].toObject()["name"].toString();
      case 1:
        return I["address"].toString();
      case 2:
        return code(I);
      case 3:
        return R["callee"].isNull() ? tr("Unresolved / boundary")
                                    : R["callee"].toObject()["name"].toString();
      case 4:
        return I["bytes"].toString();
      case 5:
        return Gaps.join(", ");
      }
    }
  }
  if (Type == Kind::Reviews) {
    switch (Index.column()) {
    case 0:
      return R["sequence"].toInt();
    case 1:
      return R["case"].toString();
    case 2:
      return R["assessment"].toString().replace('_', ' ');
    case 3:
      return R["author"].toString();
    case 4:
      return (R["pair_active"].toBool() ? tr("Accepted pair")
                                        : tr("Pair inactive")) +
             " · " + (R["latest"].toBool() ? tr("Latest") : tr("Earlier"));
    case 5:
      return R["note"].toString().section('\n', 0, 0);
    }
  }
  if (Type == Kind::Changes) {
    const auto Join = [](const QJsonArray &Values) {
      QStringList Names;
      for (const auto &Value : Values)
        Names << Value.toString().replace('_', ' ');
      return Names.join(", ");
    };
    const auto Instruction = [&](const char *Side) {
      const auto I = R[Side].toObject();
      return I.isEmpty() ? tr("—") : I["address"].toString() + "  " + code(I);
    };
    switch (Index.column()) {
    case 0:
      return R["priority"].toInt();
    case 1:
      return Join(R["kinds"].toArray());
    case 2: {
      auto Contexts = R["primary_context"].toArray();
      for (const auto &C : R["secondary_context"].toArray())
        if (!Contexts.contains(C))
          Contexts.append(C);
      return Join(Contexts);
    }
    case 3:
      return Instruction("primary");
    case 4:
      return Instruction("secondary");
    case 5:
      return R["association"].toString().replace('_', ' ');
    }
  }
  if (Type == Kind::FlowFunctions) {
    switch (Index.column()) {
    case 0:
      return R["function"].toString();
    case 1:
      return R["availability"].toString().replace('_', ' ');
    case 2:
      return R["nodes"].toInt();
    case 3:
      return R["blocks"].toInt();
    case 4:
      return R["reason"].toString();
    case 5:
      return tr("Observed SSA");
    }
  }
  if (Type == Kind::FlowNodes || Type == Kind::FlowSlice ||
      Type == Kind::FlowAccesses) {
    switch (Index.column()) {
    case 0:
      return Type == Kind::FlowAccesses ? R["argument"].toInt() + 1
                                        : R["id"].toInt();
    case 1:
      return originAddresses(R).isEmpty() ? tr("Synthetic")
                                          : originAddresses(R);
    case 2:
      if (Type == Kind::FlowAccesses)
        return R["access"] == QJsonValue("write") ? tr("Write") : tr("Read");
      return R["opcode"].toString();
    case 3:
      return tr("%1 B").arg(
          R[Type == Kind::FlowAccesses ? "access_bytes" : "result_bytes"]
              .toInt());
    case 4: {
      if (Type == Kind::FlowAccesses)
        return R["return_operand_path"].toBool() ? tr("COPY / PHI path")
                                                 : tr("Not observed");
      QStringList Inputs;
      const auto Values = R["inputs"].toArray();
      const auto ArgumentBegin = R["call_argument_begin"].toInt(-1);
      const auto ArgumentCount = R["call_argument_count"].toInt();
      QHash<int, QString> ParameterViews;
      for (const auto &V : R["parameter_views"].toArray()) {
        const auto View = V.toObject();
        const auto Text = parameterViewText(View);
        ParameterViews.insert(
            View["input"].toInt(),
            Text.isEmpty() ? tr("param %1").arg(View["parameter"].toInt())
                           : Text);
      }
      for (qsizetype N = 0; N < Values.size(); ++N) {
        const auto I = Values[N].toArray();
        if (I.size() != 7 && I.size() != 8)
          continue;
        const auto Kind = I[0].toInt();
        QString Text = Kind == 0   ? tr("n%1").arg(I[1].toInt())
                       : Kind == 1 ? I[3].toString()
                       : Kind == 2 ? tr("param %1").arg(I[3].toString())
                                   : tr("unknown");
        if (ParameterViews.contains(N))
          Text = ParameterViews.value(N);
        if (!I[6].isNull())
          Text += tr(" @b%1").arg(I[6].toInt());
        if (ArgumentBegin >= 0 && N >= ArgumentBegin &&
            N - ArgumentBegin < ArgumentCount)
          Text = tr("arg %1: %2").arg(N - ArgumentBegin + 1).arg(Text);
        Inputs << Text;
      }
      return Inputs.join(", ");
    }
    case 5: {
      QStringList Gaps;
      for (const auto &G : R["gaps"].toArray())
        Gaps << G.toString().replace('_', ' ');
      return Gaps.join(", ");
    }
    }
  }
  if (Type == Kind::FlowRegionAnchors) {
    switch (Index.column()) {
    case 0:
      return R["primary_function"].toString();
    case 1:
      return R["primary_root"].toInt();
    case 2:
      return R["secondary_function"].toString();
    case 3:
      return R["secondary_root"].toInt();
    case 4:
      return R["node_count"].toInt();
    case 5:
      return R["unique_seed"].toBool() ? tr("Unique seed")
                                       : tr("Repeated seed");
    }
  }
  if (Type == Kind::FlowRegionNodes) {
    const auto N = R[Index.column() < 3 ? "primary" : "secondary"].toObject();
    switch (Index.column() % 3) {
    case 0:
      return N["id"].toInt();
    case 1:
      return originAddresses(N).isEmpty() ? tr("Synthetic")
                                          : originAddresses(N);
    case 2: {
      const auto Original = N["original"].toObject();
      if (N.contains("originals")) {
        QStringList Instructions;
        for (const auto &O : N["originals"].toArray())
          Instructions << code(O.toObject());
        const auto Views = parameterViewsText(N);
        return N["opcode"].toString() +
               (Views.isEmpty() ? QString() : " · " + Views) +
               (Instructions.isEmpty() ? QString()
                                       : " · " + Instructions.join("; "));
      }
      return N["opcode"].toString() +
             (Original.isEmpty() ? QString()
                                 : " · " + Original["mnemonic"].toString() +
                                       " " + Original["operands"].toString());
    }
    }
  }
  if (Type == Kind::Regions || Type == Kind::FlowRegions) {
    QStringList Explanations;
    for (const auto &K : R["explanations"].toArray())
      Explanations << K.toString().replace('_', ' ');
    switch (Index.column()) {
    case 0:
      return Explanations.join(", ");
    case 1:
      return R["primary_function_count"].toInt();
    case 2:
      return R["secondary_function_count"].toInt();
    case 3:
      return tr("%1 / %2")
          .arg(R[Type == Kind::FlowRegions ? "primary_nodes"
                                           : "primary_instructions"]
                   .toInt())
          .arg(R[Type == Kind::FlowRegions ? "secondary_nodes"
                                           : "secondary_instructions"]
                   .toInt());
    case 4:
      return number(R["primary_coverage"].toDouble()) + " / " +
             number(R["secondary_coverage"].toDouble());
    case 5:
      if (Type == Kind::FlowRegions)
        return R["open_data_frontier"].toBool() ? tr("Data + control")
                                                : tr("Control");
      return number(R["confidence"].toDouble());
    }
  }
  if (Type == Kind::RegionAnchors) {
    const auto A = R["primary"].toObject(), B = R["secondary"].toObject();
    switch (Index.column()) {
    case 0:
      return A["name"].toString();
    case 1:
      return A["begin"].toString() + " – " + A["end"].toString();
    case 2:
      return B["name"].toString();
    case 3:
      return B["begin"].toString() + " – " + B["end"].toString();
    case 4:
      return A["instructions"].toInt();
    case 5:
      return R["unique_seed"].toBool() ? tr("Unique seed")
                                       : tr("Repeated seed");
    }
  }
  if (Type == Kind::Candidates) {
    const auto Counterpart = R["counterpart"].toObject();
    switch (Index.column()) {
    case 0:
      return R["address"].toString();
    case 1:
      return R["name"].toString();
    case 2:
      return R["size"].toInteger() ? R["size"].toVariant()
                                   : QVariant(tr("Unknown"));
    case 3:
      return !Counterpart.isEmpty()                  ? tr("Paired")
             : R["state"] == QJsonValue("ambiguous") ? tr("Ambiguous")
                                                     : tr("Unpaired");
    case 4:
      return Counterpart.isEmpty() ? tr("None")
                                   : Counterpart["name"].toString() + " · " +
                                         Counterpart["address"].toString();
    }
    return {};
  }
  if (Type == Kind::Functions) {
    switch (Index.column()) {
    case 0:
      return R["address"].toString();
    case 1:
      return R["name"].toString();
    case 2:
      return R["counterpart"].toObject()["name"].toString();
    case 3:
      return E.isEmpty() || E["identity_only"].toBool()
                 ? QString()
                 : number(E["similarity"].toDouble());
    case 4:
      return E.isEmpty() ? QString() : number(E["confidence"].toDouble());
    case 5:
      if (const auto State = E["change_state"].toString(); !State.isEmpty())
        return changeStateText(State);
      if (E["identity_only"].toBool())
        return tr("Identity only");
      if (!E.isEmpty() && !E["measured"].toBool())
        return tr("Unmeasured");
      if (R["incomplete"].toBool() ||
          (!E.isEmpty() && !E["analysis_complete"].toBool()))
        return tr("Incomplete");
      if (E["normalized_unchanged"].toBool())
        return tr("Unchanged");
      if (E["measured"].toBool())
        return tr("Changed");
      return R["state"].toString();
    }
  } else {
    const auto A = R["primary"].toObject(), B = R["secondary"].toObject();
    switch (Index.column()) {
    case 0:
      return A["address"].toString();
    case 1:
      return A["bytes"].toString();
    case 2:
      return code(A);
    case 3:
      if (K == "uncertain")
        return tr("Uncertain");
      return K;
    case 4:
      return code(B);
    case 5:
      return B["bytes"].toString();
    case 6:
      return B["address"].toString();
    }
  }
  return {};
}
void ComparisonModel::setQuery(const QJsonObject &Value) {
  Query = Value;
  QueryActive = true;
  reload();
}
void ComparisonModel::clear() {
  QueryActive = false;
  Query = {};
  reload();
}
void ComparisonModel::reload() {
  beginResetModel();
  ++Generation;
  Pages.clear();
  Pending.clear();
  Total = 0;
  endResetModel();
  if (QueryActive && Session.ready() && !Session.busy() &&
      !Session.revision().isEmpty() &&
      (Type == Kind::Functions || Type == Kind::Candidates ||
       Type == Kind::FlowFunctions || Type == Kind::Reviews ||
       ((Type == Kind::ImpactFunctions || Type == Kind::ImpactSites) &&
        Query.contains("seeds")) ||
       Query.contains("address") || Query.contains("region_id") ||
       Query.contains("all_anchors") || Query.contains("anchor") ||
       (Type == Kind::Regions &&
        Session.summary()["regions"].toObject()["state"].isString()) ||
       (Type == Kind::FlowRegions &&
        Session.summary()["flow_regions"].toObject()["state"].isString())))
    fetch(0);
}
void ComparisonModel::sort(int Column, Qt::SortOrder Order) {
  if (Type != Kind::Functions)
    return;
  const QStringList Keys{"address",    "name",       "address",
                         "similarity", "confidence", "changes"};
  Query["sort"] = Keys.value(Column, "address");
  Query["descending"] = Order == Qt::DescendingOrder;
  reload();
}
void ComparisonModel::fetch(int Page) const {
  if (Pending.contains(Page) ||
      Pending.size() >= ::neverd::limits::gui::kComparisonPendingPages ||
      Session.busy())
    return;
  Pending.insert(Page);
  auto *Self = const_cast<ComparisonModel *>(this);
  const auto Serial = Generation;
  QTimer::singleShot(0, Self, [Self, Page, Serial] {
    if (Self->Generation != Serial)
      return;
    auto Request = Self->Query;
    if (Self->Type == Kind::Instructions || Self->Type == Kind::FlowSlice ||
        Self->Type == Kind::FlowAccesses ||
        Self->Type == Kind::ImpactFunctions ||
        Self->Type == Kind::ImpactSites || Self->Type == Kind::Changes)
      Self->Query.remove("refresh");
    Request["offset"] = Page * PageSize;
    Request["limit"] = PageSize;
    const auto Operation =
        Self->Type == Kind::Reviews   ? "comparison_reviews"
        : Self->Type == Kind::Changes ? "comparison_observations"
        : Self->Type == Kind::ImpactFunctions || Self->Type == Kind::ImpactSites
            ? "comparison_impact"
        : Self->Type == Kind::FlowRegions ||
                Self->Type == Kind::FlowRegionAnchors ||
                Self->Type == Kind::FlowRegionNodes
            ? "comparison_flow_regions"
        : Self->Type == Kind::FlowFunctions || Self->Type == Kind::FlowNodes ||
                Self->Type == Kind::FlowAccesses
            ? "comparison_dataflow"
        : Self->Type == Kind::FlowSlice ? "comparison_slice"
        : Self->Type == Kind::Functions || Self->Type == Kind::Candidates
            ? "comparison_functions"
        : Self->Type != Kind::Instructions ? "comparison_regions"
        : Self->Query.contains("anchor")   ? "comparison_region_page"
                                           : "comparison_function_page";
    Self->Session.request(
        Operation, Request, Self, [Self, Page, Serial](const auto &Message) {
          if (Self->Generation != Serial)
            return;
          Self->Pending.remove(Page);
          if (Message["status"] != QJsonValue("ok")) {
            emit Self->failed(
                Message["error"].toObject()["message"].toString());
            return;
          }
          const auto Result = Message["payload"].toObject();
          const int Total = Result["total"].toInt();
          if (Self->Total != Total) {
            Self->beginResetModel();
            Self->Total = Total;
            Self->endResetModel();
          }
          const auto Rows =
              Result[Self->Type == Kind::Instructions ||
                             Self->Type == Kind::FlowFunctions ||
                             Self->Type == Kind::FlowNodes ||
                             Self->Type == Kind::FlowSlice ||
                             Self->Type == Kind::FlowAccesses ||
                             Self->Type == Kind::FlowRegionNodes ||
                             Self->Type == Kind::ImpactFunctions ||
                             Self->Type == Kind::ImpactSites ||
                             Self->Type == Kind::Changes ||
                             Self->Type == Kind::Reviews
                         ? "rows"
                         : "items"]
                  .toArray();
          Self->Pages.insert(Page, new QJsonArray(Rows));
          if (!Rows.isEmpty())
            emit Self->dataChanged(
                Self->index(Page * PageSize, 0),
                Self->index(Page * PageSize + Rows.size() - 1,
                            Self->columnCount() - 1));
          emit Self->pageReady(Result);
        });
  });
}
} // namespace neverd::gui
