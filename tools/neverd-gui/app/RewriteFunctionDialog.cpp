//===- RewriteFunctionDialog.cpp - Source replacement UI -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Compile explicit C through the engine and display its publication receipt.
//===----------------------------------------------------------------------===//
#include "RewriteFunctionDialog.h"

#include "../ResourceLimits.h"
#include "Session.h"
#include "SettingsKeys.h"
#include "Theme.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>
namespace neverd::gui {
RewriteFunctionDialog::RewriteFunctionDialog(Session &session, Address function,
                                             QWidget *parent)
    : QDialog(parent), session_(session), function_(function),
      epoch_(session.epoch()), revision_(session.revision()) {
  setWindowTitle(tr("Rewrite function in C"));
  setObjectName(QStringLiteral("rewriteFunctionDialog"));
  setSizeGripEnabled(true);
  auto *layout = new QVBoxLayout(this);
  auto *form = new QFormLayout;
  compiler_ = new QComboBox(this);
  for (const auto &compiler : {"clang", "neverc", "gcc"})
    compiler_->addItem(compiler, compiler);
  const auto selected =
      QSettings().value(settings::PatchCompiler, "clang").toString();
  compiler_->setCurrentIndex(std::max(0, compiler_->findData(selected)));
  compilerPath_ = new QLineEdit(this);
  compilerPath_->setText(QSettings()
                             .value(settings::PatchCompilerPaths)
                             .toMap()
                             .value(compiler_->currentData().toString())
                             .toString());
  connect(compiler_, &QComboBox::currentIndexChanged, this, [this] {
    const auto kind = compiler_->currentData().toString();
    QSettings().setValue(settings::PatchCompiler, kind);
    compilerPath_->setText(QSettings()
                               .value(settings::PatchCompilerPaths)
                               .toMap()
                               .value(kind)
                               .toString());
  });
  connect(
      compilerPath_, &QLineEdit::textEdited, this, [this](const QString &path) {
        auto paths = QSettings().value(settings::PatchCompilerPaths).toMap();
        paths[compiler_->currentData().toString()] = path;
        QSettings().setValue(settings::PatchCompilerPaths, paths);
      });
  compilerPath_->setPlaceholderText(tr("Executable name or full path"));
  symbol_ = new QLineEdit(this);
  symbol_->setPlaceholderText(tr("Empty selects the sole external definition"));
  original_ = new QLineEdit(this);
  original_->setPlaceholderText(
      tr("Optional external function name to call the saved original body"));
  form->addRow(tr("Compiler:"), compiler_);
  form->addRow(tr("Compiler path:"), compilerPath_);
  form->addRow(tr("Replacement function:"), symbol_);
  form->addRow(tr("Original-body binding:"), original_);
  layout->addLayout(form);
  source_ = new QPlainTextEdit(this);
  source_->setFont(Theme::instance().codeFont());
  source_->setAccessibleName(tr("Replacement C source"));
  receipt_ = new QPlainTextEdit(this);
  receipt_->setReadOnly(true);
  receipt_->setAccessibleName(tr("Publication receipt"));
  auto *split = new QSplitter(Qt::Vertical, this);
  split->addWidget(source_);
  split->addWidget(receipt_);
  split->setStretchFactor(0, 3);
  split->setStretchFactor(1, 1);
  layout->addWidget(split);
  status_ =
      new QLabel(tr("Preparing compilable C and original bindings…"), this);
  status_->setWordWrap(true);
  layout->addWidget(status_);
  auto *buttons = new QDialogButtonBox(this);
  compile_ = buttons->addButton(tr("Compile to new binary…"),
                                QDialogButtonBox::ActionRole);
  compile_->setDefault(true);
  compile_->setEnabled(false);
  open_ =
      buttons->addButton(tr("Open new binary"), QDialogButtonBox::ActionRole);
  open_->setEnabled(false);
  compare_ = buttons->addButton(tr("Compare with original"),
                                QDialogButtonBox::ActionRole);
  compare_->setEnabled(false);
  buttons->addButton(QDialogButtonBox::Close);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(compile_, &QPushButton::clicked, this,
          &RewriteFunctionDialog::compile);
  connect(open_, &QPushButton::clicked, this, [this] {
    emit openProduced(produced_);
    accept();
  });
  connect(compare_, &QPushButton::clicked, this,
          [this] { emit compareProduced(produced_); });
  resize(::neverd::limits::gui::kRewriteDialogWidth,
         ::neverd::limits::gui::kRewriteDialogHeight);
  session_.read(
      "source_patch_context", {{"address", hexAddress(function)}}, this,
      [this](const QJsonObject &context) {
        source_->setPlainText(context.value("source").toString());
        symbol_->setText(context.value("symbol").toString());
        bindings_ = context.value("bindings").toArray();
        status_->setText(
            tr("%1 original symbol bindings prepared. Output uses a new file.")
                .arg(bindings_.size()));
        compile_->setEnabled(true);
      },
      [this](const QString &, const QString &error) {
        status_->setText(error);
      });
}
void RewriteFunctionDialog::compile() {
  if (epoch_ != session_.epoch() || revision_ != session_.revision()) {
    status_->setText(
        tr("The analysis changed. Reopen this editor before compiling."));
    compile_->setEnabled(false);
    return;
  }
  const auto path =
      QFileDialog::getSaveFileName(this, tr("Produce replacement binary"),
                                   session_.filePath() + ".rewritten");
  if (path.isEmpty())
    return;
  auto bindings = bindings_;
  if (!original_->text().trimmed().isEmpty())
    bindings.append(QJsonObject{{"symbol", original_->text().trimmed()},
                                {"kind", "original"},
                                {"address", hexAddress(function_)},
                                {"size", 0}});
  compile_->setEnabled(false);
  open_->setEnabled(false);
  compare_->setEnabled(false);
  status_->setText(tr("Compiling and validating the replacement…"));
  session_.produceBinary(
      "source_patch",
      {{"address", hexAddress(function_)},
       {"source", source_->toPlainText()},
       {"symbol", symbol_->text().trimmed()},
       {"compiler", compiler_->currentData().toString()},
       {"compiler_path", compilerPath_->text().trimmed()},
       {"bindings", bindings},
       {"path", path}},
      this,
      [this, path](const QJsonObject &report) {
        receipt_->setPlainText(QString::fromUtf8(
            QJsonDocument(report).toJson(QJsonDocument::Indented)));
        produced_ = path;
        status_->setText(tr("Replacement published to %1").arg(path));
        compile_->setEnabled(true);
        open_->setEnabled(true);
        compare_->setEnabled(true);
      },
      [this](const QString &, const QString &error) {
        status_->setText(error);
        compile_->setEnabled(true);
      });
}
} // namespace neverd::gui
