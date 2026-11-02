//===- ComparisonValidationDialog.cpp - Inspect scalar proof scope --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shows model equivalence separately from native-state and security claims.
//===----------------------------------------------------------------------===//

#include "ComparisonValidationDialog.h"

#include "../ResourceLimits.h"
#include "ComparisonSession.h"
#include "Icons.h"

#include "neverd/analysis/Limits.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

namespace neverd::gui {
ComparisonValidationDialog::ComparisonValidationDialog(ComparisonSession &S,
                                                       QWidget *Parent)
    : QDialog(Parent), Session(S), Revision(S.revision()) {
  namespace Limits = ::neverd::limits::gui;
  setObjectName("comparisonValidationDialog");
  setWindowTitle(tr("Validate scalar returns — NeverD"));
  setWindowIcon(icon("compare"));
  resize(Limits::kComparisonValidationInitialWidth,
         Limits::kComparisonValidationInitialHeight);
  setMinimumSize(Limits::kComparisonValidationMinimumWidth,
                 Limits::kComparisonValidationMinimumHeight);
  auto *Root = new QVBoxLayout(this);
  auto *Scope = new QLabel(
      tr("Check equal integer returns and defined termination in the lifted "
         "models, "
         "for all defined input bitvectors at matching register locations. "
         "Native-state equivalence and security are outside this check."),
      this);
  Scope->setWordWrap(true);
  Scope->setObjectName("validationScope");
  Root->addWidget(Scope);
  Target = new QLabel(this);
  Target->setObjectName("validationTarget");
  Target->setWordWrap(true);
  Root->addWidget(Target);
  auto *Actions = new QHBoxLayout;
  auto *TimeLabel = new QLabel(tr("Time limit (ms)"), this);
  Timeout = new QSpinBox(this);
  Timeout->setObjectName("validationTimeout");
  Timeout->setRange(1, ::neverd::limits::diff::kMaxTimeoutMilliseconds);
  Timeout->setValue(Limits::kComparisonValidationTimeoutMs);
  TimeLabel->setBuddy(Timeout);
  Actions->addWidget(TimeLabel);
  Actions->addWidget(Timeout);
  Actions->addStretch();
  Run = new QPushButton(tr("Check again"), this);
  Run->setObjectName("validationRun");
  Cancel = new QPushButton(tr("Cancel"), this);
  Cancel->setObjectName("validationCancel");
  Actions->addWidget(Run);
  Actions->addWidget(Cancel);
  Root->addLayout(Actions);
  auto *Budgets = new QHBoxLayout;
  auto *InstructionLabel = new QLabel(tr("Native instructions"), this);
  Instructions = new QSpinBox(this);
  Instructions->setObjectName("validationInstructions");
  Instructions->setRange(
      1, ::neverd::limits::diff::kMaxValidationNativeInstructions);
  Instructions->setValue(
      ::neverd::limits::diff::kDefaultValidationNativeInstructions);
  Instructions->setToolTip(tr("Maximum native instructions per function."));
  InstructionLabel->setBuddy(Instructions);
  Budgets->addWidget(InstructionLabel);
  Budgets->addWidget(Instructions);
  auto *WorkLabel = new QLabel(tr("Proof work"), this);
  Work = new QSpinBox(this);
  Work->setObjectName("validationWork");
  Work->setRange(1, ::neverd::limits::diff::kMaxValidationWork);
  Work->setValue(::neverd::limits::analysis::kLLVMScalarEquivalenceDefaultWork);
  Work->setToolTip(tr("Maximum work units for the scalar proof."));
  WorkLabel->setBuddy(Work);
  Budgets->addWidget(WorkLabel);
  Budgets->addWidget(Work);
  Budgets->addStretch();
  Root->addLayout(Budgets);
  Status = new QLabel(tr("Select an accepted function pair."), this);
  Status->setWordWrap(true);
  Status->setObjectName("validationStatus");
  Root->addWidget(Status);
  auto *Tabs = new QTabWidget(this);
  Tabs->setObjectName("validationTabs");
  Evidence = new QPlainTextEdit(Tabs);
  Evidence->setObjectName("validationEvidence");
  Evidence->setReadOnly(true);
  Report = new QPlainTextEdit(Tabs);
  Report->setObjectName("validationReport");
  Report->setReadOnly(true);
  Tabs->addTab(Evidence, tr("Result and assumptions"));
  Tabs->addTab(Report, tr("Full report"));
  Root->addWidget(Tabs, 1);
  connect(Run, &QPushButton::clicked, this, &ComparisonValidationDialog::run);
  connect(Cancel, &QPushButton::clicked, &Session, &ComparisonSession::cancel);
  for (auto *Limit : {Timeout, Instructions, Work})
    connect(Limit, &QSpinBox::valueChanged, this, [this] {
      if (Pending)
        return;
      clear();
      Status->setText(tr("Limits changed. Check again."));
      controls();
    });
  connect(&Session, &ComparisonSession::changed, this, [this] {
    if (Revision != Session.revision() || Session.summary()["stale"].toBool()) {
      clear();
      Query = {};
      Target->clear();
      Revision = Session.revision();
      Status->setText(
          tr("Inputs or pairing changed. Select the function pair again."));
    }
    controls();
  });
  connect(&Session, &ComparisonSession::mappingChanged, this, [this] {
    clear();
    Query = {};
    Target->clear();
    Status->setText(tr("Pairing changed. Select the function pair again."));
    controls();
  });
  controls();
}
void ComparisonValidationDialog::clear() {
  ++Generation;
  Pending = false;
  Evidence->clear();
  Report->clear();
}
void ComparisonValidationDialog::review(const QString &Side,
                                        const QString &Address) {
  clear();
  Revision = Session.revision();
  Query = {{"side", Side}, {"address", Address}};
  Target->setText(tr("Selected %1 function: %2").arg(Side, Address));
  run();
}
void ComparisonValidationDialog::controls() {
  const bool Idle = !Pending && !Session.busy() && !Session.cancellable();
  Run->setEnabled(Idle && !Query.isEmpty() &&
                  !Session.summary()["stale"].toBool());
  Timeout->setEnabled(Idle);
  Instructions->setEnabled(Idle);
  Work->setEnabled(Idle);
  Cancel->setEnabled(Pending && Session.cancellable() &&
                     !Session.cancelRequested());
}
void ComparisonValidationDialog::run() {
  if (Query.isEmpty() || Session.busy() || Session.cancellable()) {
    controls();
    return;
  }
  clear();
  Pending = true;
  const auto Current = Generation;
  auto Request = Query;
  Request["timeout_ms"] = Timeout->value();
  Request["max_native_instructions"] = Instructions->value();
  Request["max_work"] = Work->value();
  Status->setText(
      tr("Lifting and checking this pair… Cancellation is cooperative."));
  Session.request(
      "comparison_scalar_validation", Request, this,
      [this, Current](const QJsonObject &Message) {
        if (Current != Generation)
          return;
        Pending = false;
        if (Message["status"].toString() != "ok") {
          Status->setText(Message["error"].toObject()["message"].toString());
          controls();
          return;
        }
        const auto R = Message["payload"].toObject();
        const auto State = R["state"].toString(),
                   Outcome = R["status"].toString();
        if (State == "timed_out")
          Status->setText(tr("Time limit reached. No equivalence conclusion."));
        else if (State == "cancelled")
          Status->setText(tr("Cancelled. No equivalence conclusion."));
        else if (Outcome == "proved")
          Status->setText(
              tr("Equal returns proved in the lifted scalar models."));
        else if (Outcome == "budget_exceeded")
          Status->setText(tr("Validation limit reached. Adjust the limits and "
                             "check again."));
        else if (Outcome == "unsupported")
          Status->setText(
              tr("This pair is outside the supported scalar model."));
        else
          Status->setText(tr("Equivalence remains unproved; this does not "
                             "establish a difference."));
        const auto A = R["primary"].toObject(), B = R["secondary"].toObject();
        Target->setText(tr("Primary %1 (%2) ↔ Secondary %3 (%4)")
                            .arg(A["name"].toString(), A["address"].toString(),
                                 B["name"].toString(),
                                 B["address"].toString()));
        QString Text =
            tr("Input assumption: defined integer bitvectors at identical "
               "register locations.\n"
               "Lift correctness is assumed. Complete native state, memory and "
               "call effects are not proved.\n"
               "This result does not assess vulnerability or fix status.\n\n");
        if (!R["diagnostic"].toString().isEmpty())
          Text += tr("Detail: %1\n\n").arg(R["diagnostic"].toString());
        if (!R["result_bits"].isNull())
          Text += tr("Return width: %1 bits\n").arg(R["result_bits"].toInt());
        for (const auto &Value : R["arguments"].toArray()) {
          const auto Arg = Value.toObject();
          Text += tr("Argument %1: %2 bits, register offset %3\n")
                      .arg(Arg["ordinal"].toInt())
                      .arg(Arg["bits"].toInt())
                      .arg(Arg["register_offset"].toInt());
        }
        Text += tr("\nProof work: %1; completed partitions: %2; attempts: %3\n")
                    .arg(R["work"].toInteger())
                    .arg(R["completed_partitions"].toInteger())
                    .arg(R["attempts"].toInteger());
        if (R["input_source_binding"].toString() == "owned_sessions")
          Text += tr("Inputs: retained comparison sources.\n");
        else
          Text +=
              tr("Live input versions rechecked: %1\n")
                  .arg(R["inputs_revalidated"].toBool() ? tr("yes") : tr("no"));
        Text +=
            tr("Profile: %1\n\nPrimary SHA-256: %2\nSecondary SHA-256: %3\n")
                .arg(R["profile"].toString(), R["primary_sha256"].toString(),
                     R["secondary_sha256"].toString());
        Evidence->setPlainText(Text);
        Report->setPlainText(QString::fromUtf8(
            QJsonDocument(R).toJson(QJsonDocument::Indented)));
        controls();
      });
  controls();
}
} // namespace neverd::gui
