//===- SafetyScanDialog.cpp - Native memory-safety scan -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Display the shared scan report without reclassifying its findings.
//===----------------------------------------------------------------------===//

#include "SafetyScanDialog.h"

#include "DialogReturnKey.h"
#include "Icons.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace neverd::gui {
SafetyScanDialog::SafetyScanDialog(QueryService &Service, QWidget *Parent)
    : QDialog(Parent), Queries(Service) {
  setObjectName("memorySafetyScan");
  setWindowTitle(tr("Memory Safety Scan"));
  setWindowIcon(icon("analyze"));
  resize(fontMetrics().averageCharWidth() * 100, fontMetrics().height() * 32);
  auto *layout = new QVBoxLayout(this);
  auto *controls = new QHBoxLayout;
  auto *label = new QLabel(tr("&Analyzers"), this);
  Analyzers = new QComboBox(this);
  Analyzers->addItem(tr("Audit and Hunt"), "both");
  Analyzers->addItem(tr("Audit"), "audit");
  Analyzers->addItem(tr("Hunt"), "hunt");
  label->setBuddy(Analyzers);
  controls->addWidget(label);
  controls->addWidget(Analyzers);
  controls->addStretch();
  layout->addLayout(controls);
  Status = new QLabel(this);
  Status->setObjectName("safetyScanStatus");
  Status->setTextFormat(Qt::PlainText);
  Status->setWordWrap(true);
  Status->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(Status);
  auto *split = new QSplitter(Qt::Vertical, this);
  Findings = new QTreeWidget(split);
  Findings->setAccessibleName(tr("Findings"));
  Findings->setRootIsDecorated(false);
  Findings->setAlternatingRowColors(true);
  Findings->setHeaderLabels({tr("Verdict"), tr("Property"), tr("Address"),
                             tr("Function"), tr("Analyzers")});
  Findings->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
  Details = new QPlainTextEdit(split);
  Details->setAccessibleName(tr("Finding evidence"));
  Details->setReadOnly(true);
  layout->addWidget(split, 1);
  connect(Findings, &QTreeWidget::currentItemChanged, this,
          [this](QTreeWidgetItem *item) {
            if (item)
              Details->setPlainText(QString::fromUtf8(
                  QJsonDocument(
                      Rows.at(Findings->indexOfTopLevelItem(item)).toObject())
                      .toJson(QJsonDocument::Indented)));
            else
              Details->clear();
          });
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  Run = buttons->addButton(tr("&Scan"), QDialogButtonBox::ActionRole);
  Cancel = buttons->addButton(tr("Cancel scan"), QDialogButtonBox::ActionRole);
  Run->setObjectName("runSafetyScan");
  Cancel->setObjectName("cancelSafetyScan");
  Cancel->setEnabled(false);
  connect(Run, &QPushButton::clicked, this, &SafetyScanDialog::start);
  connect(Cancel, &QPushButton::clicked, this, [this] {
    Queries.cancelCommand(Request);
    Cancel->setEnabled(false);
    Status->setText(tr("Canceling scan…"));
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
  leaveWithoutDefaultButton(*this);
  showAsMainAction(*Run);
  // A report belongs to the exact project revision on which it was run.
  connect(&Queries, &QueryService::contextChanged, this, &QDialog::reject);
}

SafetyScanDialog::~SafetyScanDialog() {
  Queries.cancelCommand(Request);
  Queries.unsubscribeOwner(this);
}

void SafetyScanDialog::start() {
  Findings->clear();
  Rows = {};
  Details->clear();
  Status->setText(tr("Scanning…"));
  Analyzers->setEnabled(false);
  Run->setEnabled(false);
  Cancel->setEnabled(true);
  Request = Queries.enqueueCommand(
      "memory_safety_scan", {{"analyzer", Analyzers->currentData().toString()}},
      this,
      [this](const QJsonObject &reply) {
        Request = 0;
        Analyzers->setEnabled(true);
        Run->setEnabled(true);
        Cancel->setEnabled(false);
        if (reply.value("status") == "ok")
          showReport(reply.value("payload").toObject());
        else if (reply.value("status") == "cancelled")
          Status->setText(tr("Scan canceled before it started."));
        else
          Status->setText(
              reply.value("error").toObject().value("message").toString());
      },
      Queries.revision());
}

void SafetyScanDialog::showReport(const QJsonObject &Report) {
  QStringList status{tr("Verdict: %1").arg(Report.value("verdict").toString())};
  const auto analyzers = Report.value("analyzers").toObject();
  for (auto it = analyzers.begin(); it != analyzers.end(); ++it) {
    const auto track = it.value().toObject();
    status << tr("%1: %2 (%3 scanned, %4 skipped, %5 inconclusive)")
                  .arg(it.key(), track.value("state").toString())
                  .arg(track.value("scanned").toInteger())
                  .arg(track.value("skipped").toInteger())
                  .arg(track.value("unknown").toInteger());
    if (track.contains("error"))
      status << track.value("error").toString();
  }
  Status->setText(status.join('\n'));
  Rows = Report.value("findings").toArray();
  for (const auto &value : Rows) {
    const auto row = value.toObject();
    const auto observations = row.value("observations").toArray();
    QStringList sources;
    for (const auto &observation : observations) {
      const QString name = observation.toObject().value("analyzer").toString();
      if (!sources.contains(name))
        sources.append(name);
    }
    const auto observation = observations.first().toObject();
    new QTreeWidgetItem(
        Findings,
        {row.value("verdict").toString(), row.value("property").toString(),
         row.value("location").toObject().value("call_va").toString(),
         observation.value("finding").toObject().value("function").toString(),
         sources.join(", ")});
  }
  if (Findings->topLevelItemCount())
    Findings->setCurrentItem(Findings->topLevelItem(0));
}
} // namespace neverd::gui
