//===- InterpreterRecoveryDialog.cpp - Recovery options ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Use the worker's versioned recovery adapter without inventing memory facts.
//===----------------------------------------------------------------------===//

#include "InterpreterRecoveryDialog.h"

#include "../ResourceLimits.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QVariant>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace neverd::gui {

QJsonObject InterpreterRecoveryDialog::defaults() {
  // Storage selection only: all resource defaults and semantic decisions stay
  // in the SDK. No frame, alignment or separation premise is implied.
  return {{"source_abi", "xmm"}};
}

InterpreterRecoveryDialog::InterpreterRecoveryDialog(const QJsonObject &Current,
                                                     QWidget *Parent)
    : QDialog(Parent), Options(Current.isEmpty() ? defaults() : Current) {
  setObjectName(QStringLiteral("interpreterRecoveryOptions"));
  setWindowTitle(tr("Recover interpreter function"));
  setSizeGripEnabled(true);
  resize(limits::gui::kInterpreterRecoveryDialogWidth,
         limits::gui::kInterpreterRecoveryDialogHeight);
  auto *Layout = new QVBoxLayout(this);
  auto *Note = new QLabel(
      tr("Recover reachable control flow and generate C with explicit x64 "
         "register state. The source describes normal, nonfaulting execution "
         "with fixed image mappings. Memory assumptions below apply only "
         "when you enter them."),
      this);
  Note->setWordWrap(true);
  Layout->addWidget(Note);
  auto *Tabs = new QTabWidget(this);
  Layout->addWidget(Tabs, 1);
  const auto Page = [&](const QString &Title) {
    auto *Scroll = new QScrollArea(Tabs);
    Scroll->setWidgetResizable(true);
    auto *Body = new QWidget(Scroll);
    auto *Form = new QFormLayout(Body);
    Form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    Scroll->setWidget(Body);
    Tabs->addTab(Scroll, Title);
    return Form;
  };
  auto *State = Page(tr("State and memory"));
  auto *Work = Page(tr("Work limits"));
  auto *ABI = new QComboBox(this);
  ABI->setObjectName(QStringLiteral("recoverySourceABI"));
  ABI->addItem(tr("General registers"), "scalar");
  ABI->addItem(tr("General registers and CPU queries"), "queries");
  ABI->addItem(tr("General registers, CPU queries and XMM"), "xmm");
  ABI->setCurrentIndex(
      ABI->findData(Options.value("source_abi").toString("xmm")));
  State->addRow(tr("Source state"), ABI);
  struct Field {
    QString Key, Label;
    QLineEdit *Edit;
    bool Signed, Wide;
  };
  std::vector<Field> Fields;
  const auto Number = [&](QFormLayout *Form, const char *Key,
                          const QString &Label, bool Signed = false,
                          bool Endpoint = false, bool Wide = false) {
    auto *Edit = new QLineEdit(this);
    Edit->setObjectName(QLatin1String(Key));
    if (Options.contains(QLatin1String(Key)))
      Edit->setText(Options.value(QLatin1String(Key)).toVariant().toString());
    Edit->setPlaceholderText(Endpoint ? tr("Unspecified")
                                      : tr("Engine default"));
    Form->addRow(Label, Edit);
    Fields.push_back({QLatin1String(Key), Label, Edit, Signed, Wide});
  };
  Number(State, "entry_frame_begin", tr("Entry RSP range start"), true, true);
  Number(State, "entry_frame_end", tr("Entry RSP range end (exclusive)"), true,
         true);
  auto *FrameNote =
      new QLabel(tr("Signed offsets from entry RSP. Declaring this range "
                    "assumes address arithmetic does not wrap; it does "
                    "not prove accessibility or separation."),
                 this);
  FrameNote->setWordWrap(true);
  State->addRow(FrameNote);
  Number(State, "entry_frame_alignment", tr("Entry RSP alignment"));
  Number(State, "entry_frame_residue", tr("Entry RSP residue"));
  std::vector<std::pair<QString, QCheckBox *>> Switches;
  const auto Switch = [&](QFormLayout *Form, const char *Key,
                          const QString &Label) {
    auto *Box = new QCheckBox(Label, this);
    Box->setObjectName(QLatin1String(Key));
    Box->setChecked(Options.value(QLatin1String(Key)).toBool());
    Form->addRow(Box);
    Switches.emplace_back(QLatin1String(Key), Box);
  };
  Switch(State, "external_stores_disjoint_frame",
         tr("Assume external stores avoid the declared entry frame"));
  Switch(State, "prove_store_return_separation",
         tr("Prove stores avoid return slots and saved frame cells"));
  Switch(State, "prove_store_frame_separation",
         tr("Prove stores avoid the declared entry frame"));
  auto *WorkNote = new QLabel(
      tr("Blank or zero keeps the engine default. More work "
         "does not permit incomplete source or add memory assumptions."),
      this);
  WorkNote->setWordWrap(true);
  Work->addRow(WorkNote);
  Number(Work, "max_nodes", tr("Control-flow nodes"));
  Number(Work, "max_contexts_per_address", tr("Contexts per address"));
  Number(Work, "max_operations", tr("Evaluated operations"), false, false,
         true);
  Number(Work, "max_solver_queries", tr("Solver queries"));
  Number(Work, "max_control_fields", tr("Control fields"));
  Number(Work, "max_control_refinements", tr("Control refinements"));
  Number(Work, "max_symbolic_nodes", tr("Symbolic expression nodes"));
  Number(Work, "max_node_evaluations", tr("Node evaluations"));
  Number(Work, "max_discovery_visits", tr("Dependency visits"));
  Number(Work, "max_chained_transfers", tr("Chained transfers"));
  Number(Work, "max_chained_visits_per_destination",
         tr("Chained visits per destination"));
  Switch(Work, "stop_chain_at_repeat",
         tr("End a chain at a repeated destination"));
  auto *Error = new QLabel(this);
  Error->setObjectName(QStringLiteral("recoveryOptionsError"));
  Error->setWordWrap(true);
  for (const auto &Field : Fields)
    connect(Field.Edit, &QLineEdit::textChanged, Error, &QLabel::clear);
  Layout->addWidget(Error);
  auto *Buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  Buttons->button(QDialogButtonBox::Ok)->setText(tr("Recover"));
  Layout->addWidget(Buttons);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(Buttons, &QDialogButtonBox::accepted, this,
          [this, Fields, Switches, ABI, Error] {
            auto Next = Options;
            Next["source_abi"] = ABI->currentData().toString();
            for (const auto &Field : Fields) {
              const auto Text = Field.Edit->text().trimmed();
              Next.remove(Field.Key);
              if (Text.isEmpty())
                continue;
              bool Valid = false;
              const auto Value = Text.toLongLong(&Valid, 0);
              if (!Valid || (!Field.Signed && Value < 0) ||
                  (!Field.Signed && !Field.Wide &&
                   uint64_t(Value) > std::numeric_limits<uint32_t>::max())) {
                Error->setText(
                    tr("Enter a valid integer for %1.").arg(Field.Label));
                Field.Edit->setFocus();
                return;
              }
              Next[Field.Key] = Value;
            }
            if (Next.contains("entry_frame_begin") !=
                Next.contains("entry_frame_end")) {
              Error->setText(
                  tr("Enter both entry-frame endpoints, or leave both blank."));
              return;
            }
            for (const auto &[Key, Box] : Switches)
              Next[Key] = Box->isChecked();
            Options = std::move(Next);
            accept();
          });
}

void InterpreterRecoveryDialog::showReport(const QJsonObject &Report,
                                           QWidget *Parent) {
  QDialog Dialog(Parent);
  Dialog.setObjectName(QStringLiteral("interpreterRecoveryReport"));
  Dialog.setWindowTitle(tr("Interpreter recovery report"));
  Dialog.setSizeGripEnabled(true);
  Dialog.resize(limits::gui::kInterpreterRecoveryReportWidth,
                limits::gui::kInterpreterRecoveryReportHeight);
  auto *Layout = new QVBoxLayout(&Dialog);
  auto *Text = new QPlainTextEdit(&Dialog);
  Text->setReadOnly(true);
  Text->setPlainText(
      QString::fromUtf8(QJsonDocument(Report).toJson(QJsonDocument::Indented)));
  Layout->addWidget(Text);
  auto *Buttons = new QDialogButtonBox(QDialogButtonBox::Close, &Dialog);
  Layout->addWidget(Buttons);
  connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
  Dialog.exec();
}

} // namespace neverd::gui
