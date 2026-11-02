//===- MainWindowPatch.cpp - Interactive patch commands -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Adapt the current view to engine previews and journaled edits.
//===----------------------------------------------------------------------===//
#include "../ResourceLimits.h"
#include "CodeView.h"
#include "ComparisonDialog.h"
#include "DisassemblyView.h"
#include "GraphView.h"
#include "HexView.h"
#include "Icons.h"
#include "ListingView.h"
#include "MainWindow.h"
#include "OutputWindow.h"
#include "PatchEditor.h"
#include "PatchMenu.h"
#include "RewriteFunctionDialog.h"

#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QScopedValueRollback>
#include <QTreeView>
#include <algorithm>

namespace neverd::gui {
void MainWindow::connectPatchActions() {
  // Hex owns its own editing/navigation keys; only add the patch prefix here.
  hex_->addAction(actions_.action(ActionId::EditPatchMenu));
#define NEVERD_PATCH_ITEM(Id, Key, Kind)                                       \
  connect(actions_.action(ActionId::Id), &QAction::triggered, this,            \
          [this] { patchAction(QLatin1String(Kind)); });
#include "PatchMenu.def"
  connect(actions_.action(ActionId::EditPatchMenu), &QAction::triggered, this,
          [this] {
            const auto origin = patchOrigin();
            if (!origin.surface || origin.surface->width() <= 0 ||
                origin.surface->height() <= 0)
              return;
            auto *menu = new QMenu(commandWindow());
            preparePatchMenu(menu);
            connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
            const auto bounds = origin.surface->rect();
            const QPoint at(
                std::clamp(origin.anchor.left(), 0, bounds.right()),
                std::clamp(origin.anchor.bottom() + 1, 0, bounds.bottom()));
            menu->findChild<PatchMenu *>()->popupAt(
                origin.surface->mapToGlobal(at));
          });
  connect(hex_, &HexView::populateContextMenu, this, [this](QMenu *menu) {
    if (!session_.keepsBytePatches())
      return;
    auto *patch =
        menu->addMenu(icon(QStringLiteral("patch")), MainWindow::tr("Patch"));
    preparePatchMenu(patch);
  });
  connect(&session_, &Session::bytePatchesChanged, this,
          &MainWindow::updateActions);
}
MainWindow::PatchOrigin MainWindow::patchOrigin() const {
  if (auto *code = focusedCodeView()) {
    auto *text = code->text();
    return {text, text->viewport(), text->cursorViewportRect()};
  }
  for (auto *widget = commandFocusWidget(); widget;
       widget = widget->parentWidget()) {
    if (auto *hex = qobject_cast<HexView *>(widget))
      return {hex, hex->viewport(), hex->cursorViewportRect()};
    if (auto *listing = qobject_cast<ListingView *>(widget))
      return {listing, listing->viewport(), listing->cursorViewportRect()};
    if (auto *graph = qobject_cast<GraphView *>(widget))
      return {graph, graph, graph->cursorViewportRect()};
  }
  if (hexActive_)
    return {hex_, hex_->viewport(), hex_->cursorViewportRect()};
  if (disassembly_->graphMode()) {
    auto *graph = disassembly_->graph();
    return {graph, graph, graph->cursorViewportRect()};
  }
  auto *listing = disassembly_->listing();
  return {listing, listing->viewport(), listing->cursorViewportRect()};
}
QJsonObject MainWindow::patchRequest(const QString &kind) const {
  QJsonObject request{{"kind", kind}};
  const auto origin = patchOrigin();
  auto *hex = qobject_cast<HexView *>(origin.view.data());
  auto *listing = qobject_cast<ListingView *>(origin.view.data());
  auto *graph = qobject_cast<GraphView *>(origin.view.data());
  auto *code = qobject_cast<CodeText *>(origin.view.data());
  auto address = hex       ? hex->currentAddress()
                 : listing ? listing->currentAddress()
                 : graph   ? graph->currentAddress()
                 : code    ? code->currentAddress()
                           : std::nullopt;
  // Source rows need not have a displayed instruction mapping. The engine's
  // source proof, never this fallback, determines their editable bytes.
  if (code && !address)
    address = code->function();
  if (address)
    request["address"] = hexAddress(*address);
  if (kind == "fill_nops") {
    const auto selection = hex       ? hex->byteSelection()
                           : listing ? listing->byteSelection()
                                     : std::nullopt;
    if (selection) {
      request["address"] = hexAddress(selection->first);
      request["size"] = qint64(selection->second - selection->first + 1);
    }
  }
  if (auto *code = focusedCodeView(); code && code->text()->function()) {
    request["function"] = hexAddress(*code->text()->function());
    request["representation"] = code->representation();
    if (const auto number = code->text()->numberTarget();
        number && (kind == "constant" || kind == "bytes")) {
      request["line"] = number->line;
      request["literal"] = number->index;
      request["source_anchor"] = number->anchor;
    } else if (const auto row = code->text()->commentTarget();
               row && row->contains("line")) {
      request["line"] = row->value("line");
      request["literal"] = -1;
      request["source_anchor"] = row->value("anchor");
    }
  }
  return request;
}
void MainWindow::preparePatchMenu(QMenu *menu) {
  menu->clear();
  // Each opening has an independent lifetime and revision fence.
  for (auto *child :
       menu->findChildren<PatchMenu *>(QString(), Qt::FindDirectChildrenOnly))
    delete child;
  const QPointer<QWidget> focus = commandFocusWidget();
  auto *binding = new PatchMenu(menu, [this, focus](QAction *action) {
    if (!focus)
      return;
    // Popup focus and an asynchronous menu query must retain their source.
    QScopedValueRollback<QPointer<QWidget>> context(contextMenuFocus_, focus);
    action->trigger();
  });
  connect(&session_, &Session::revisionChanged, binding,
          [menu] { menu->close(); });
  connect(&session_, &Session::unloaded, binding, [menu] { menu->close(); });
  QSet<QString> base{"patches"};
  if (session_.hasBytePatches())
    base.insert("export");
  if (!session_.readOnly() && currentFunction() &&
      session_.keepsSourcePatches() && !session_.hasBytePatches())
    base.insert("rewrite");
  const auto request = patchRequest("bytes");
  if (!request.contains("address") || session_.readOnly()) {
    binding->fill(actions_, base);
    return;
  }
  menu->addAction(tr("Checking available patches…"))->setEnabled(false);
  const auto revision = session_.revision();
  const auto epoch = session_.epoch();
  const auto origin = patchOrigin();
  const auto *hex = qobject_cast<HexView *>(origin.view.data());
  const auto *listing = qobject_cast<ListingView *>(origin.view.data());
  const auto selection = hex       ? hex->selection()
                         : listing ? listing->byteSelection()
                                   : std::nullopt;
  const bool source = focusedCodeView() != nullptr;
  session_.read(
      source ? "patch_source_target" : "patch_context", request, binding,
      [this, binding, base, revision, epoch, selection,
       source](const QJsonObject &context) mutable {
        if (revision != session_.revision() || epoch != session_.epoch())
          return;
        // Source edits are offered only after the source-proof query succeeds.
        for (const auto &kind : context.value("actions").toArray())
          base.insert(kind.toString());
        if (!source && selection && selection->second > selection->first)
          base.insert("fill_nops");
        binding->fill(actions_, base);
      },
      [this, binding, base, menu](const QString &, const QString &reason) {
        binding->fill(actions_, base);
        menu->addSeparator();
        auto *refusal = menu->addAction(reason);
        refusal->setEnabled(false);
      });
}
void MainWindow::showPatchEditor(Address address, const QString &kind,
                                 const QString &initial,
                                 const PatchOrigin &origin) {
  if (!origin.view || !origin.surface || !origin.surface->isVisible())
    return;
  const auto view = origin.view;
  auto *surface = origin.surface.data();
  if (auto *old = surface->findChild<PatchEditor *>())
    old->deleteLater();
  auto *editor = new PatchEditor(session_, address, kind, initial, surface);
  connect(editor, &PatchEditor::cancelled, this, [editor, view] {
    editor->deleteLater();
    view->setFocus();
  });
  connect(editor, &PatchEditor::navigateRequested, this,
          [this, editor, address, view](int direction) {
            editor->hide();
            editor->deleteLater();
            view->setFocus();
            QKeyEvent move(QEvent::KeyPress,
                           direction > 0 ? Qt::Key_Down : Qt::Key_Up,
                           Qt::NoModifier);
            QApplication::sendEvent(view, &move);
            const auto target = patchRequest("assemble").value("address");
            if (addressValue(target) && *addressValue(target) != address)
              patchAction("assemble");
          });
  connect(editor, &PatchEditor::applied, this, [editor, view] {
    editor->deleteLater();
    view->setFocus();
  });
  editor->openAt(origin.anchor);
}
void MainWindow::patchAction(const QString &kind) {
  if (kind == "patches") {
    openDock(chooserDock(ChooserKind::PatchedBytes), true);
    return;
  }
  if (kind == "export") {
    exportBytePatches();
    return;
  }
  if (kind == "rewrite") {
    rewriteFunction();
    return;
  }
  auto request = patchRequest(kind);
  const auto target = addressValue(request.value("address"));
  if (!target || session_.readOnly())
    return;
  const auto address = *target;
  const auto epoch = session_.epoch();
  const auto revision = session_.revision();
  const auto origin = patchOrigin();
  if (kind == "revert") {
    session_.read(
        "patch_context", request, this,
        [this, request, epoch, revision](const QJsonObject &context) mutable {
          if (epoch != session_.epoch() || revision != session_.revision())
            return;
          // The opcode itself may be unchanged. Restore the engine-owned
          // changed runs intersecting this instruction, including operands.
          if (context.contains("revert_ranges")) {
            const auto ranges = context.value("revert_ranges").toArray();
            if (ranges.isEmpty())
              return;
            request["ranges"] = ranges;
          }
          session_.editBytePatch("patch_revert", request, epoch);
        });
    return;
  }
  if (kind == "assemble" || kind == "bytes") {
    session_.read(
        "patch_context", request, this,
        [this, address, kind, epoch, revision,
         origin](const QJsonObject &context) {
          if (epoch == session_.epoch() && revision == session_.revision())
            showPatchEditor(
                address, kind,
                context.value(kind == "assemble" ? "text" : "bytes").toString(),
                origin);
        });
    return;
  }
  if (kind == "constant" || kind == "skip_call_value") {
    QInputDialog dialog(origin.view);
    dialog.setCursor(Qt::ArrowCursor);
    dialog.setWindowTitle(tr("Patch value"));
    dialog.setLabelText(tr("Unsigned value (decimal or 0x hexadecimal):"));
    dialog.setTextValue("0");
    if (dialog.exec() != QDialog::Accepted)
      return;
    auto digits = dialog.textValue().trimmed();
    const bool hexadecimal = digits.startsWith("0x", Qt::CaseInsensitive);
    if (hexadecimal)
      digits.remove(0, 2);
    bool valid = false;
    const auto number = digits.toULongLong(&valid, hexadecimal ? 16 : 10);
    if (!valid || digits.startsWith('-')) {
      output_->append(tr("Invalid patch value."), 1);
      return;
    }
    request["value"] = hexAddress(number);
  }
  session_.read(
      "patch_preview", request, this,
      [this, request, epoch, revision,
       origin](const QJsonObject &plan) mutable {
        if (epoch != session_.epoch() || revision != session_.revision())
          return;
        request["expected"] = plan.value("original");
        const auto kind = request.value("kind").toString();
        // Choosing a single-instruction action is its confirmation. The
        // engine still previews and rechecks expected bytes, and the ordinary
        // project journal retains undo/redo. Source and range edits show their
        // wider consequences before committing.
        if (!request.contains("function") &&
            (kind == "nop" || kind == "branch_always" ||
             kind == "branch_never" || kind == "branch_invert" ||
             kind == "skip_call")) {
          session_.editBytePatch("patch_apply", request, epoch);
          return;
        }
        const auto shownBytes = [](const QJsonValue &value) {
          const auto full = value.toString();
          const auto limit =
              ::neverd::limits::gui::kPatchConfirmationHexCharacters;
          if (full.size() <= limit)
            return full;
          auto shortened = full.left(limit);
          // Byte boundaries let Qt wrap long ranges at fractional DPI too.
          for (auto at = shortened.size() - 2; at > 0; at -= 2)
            shortened.insert(at, QLatin1Char(' '));
          return shortened + QChar(0x2026);
        };
        QStringList lines{
            plan.value("address").toString() + QStringLiteral(" · ") +
                QString::number(plan.value("size").toInteger()) +
                QStringLiteral(" B"),
            tr("Original: %1").arg(shownBytes(plan.value("original"))),
            tr("Patched: %1").arg(shownBytes(plan.value("current")))};
        QStringList instructions;
        for (const auto &value : plan.value("decoded").toArray()) {
          const auto row = value.toObject();
          instructions << row.value("address").toString() + "  " +
                              row.value("text").toString();
        }
        const auto inlineCount =
            ::neverd::limits::gui::kPatchConfirmationInstructions;
        lines.append(instructions.mid(0, inlineCount));
        const auto instructionDetails = instructions.size() > inlineCount
                                            ? instructions.join('\n')
                                            : QString();
        if (!instructionDetails.isEmpty())
          lines << QString(QChar(0x2026));
        for (const auto &value : plan.value("warnings").toArray())
          lines << value.toString();
        const auto present = [this, request, epoch, revision, lines, origin,
                              instructionDetails](const QString &details) {
          if (!origin.view || epoch != session_.epoch() ||
              revision != session_.revision())
            return;
          QMessageBox dialog(
              QMessageBox::Question, tr("Apply byte patch"), lines.join('\n'),
              QMessageBox::Apply | QMessageBox::Cancel, origin.view);
          dialog.setCursor(Qt::ArrowCursor);
          dialog.setObjectName(QStringLiteral("bytePatchConfirmation"));
          dialog.setDefaultButton(QMessageBox::Apply);
          dialog.setEscapeButton(QMessageBox::Cancel);
          dialog.setTextFormat(Qt::PlainText);
          const auto combined = instructionDetails.isEmpty() ? details
                                : details.isEmpty()
                                    ? instructionDetails
                                    : instructionDetails + "\n\n" + details;
          if (!combined.isEmpty())
            dialog.setDetailedText(combined);
          if (dialog.exec() != QMessageBox::Apply ||
              epoch != session_.epoch() || revision != session_.revision())
            return;
          session_.editBytePatch("patch_apply", request, epoch);
        };
        if (request.contains("function")) {
          session_.read(
              "patch_source_preview", request, this,
              [this, present](const QJsonObject &source) {
                present(tr("Before:\n%1\nAfter:\n%2")
                            .arg(source.value("before").toString(),
                                 source.value("after").toString(
                                     source.value("after_error").toString())));
              },
              [present](const QString &, const QString &error) {
                present(error);
              });
        } else
          present({});
      });
}
void MainWindow::exportBytePatches() {
  const auto path = QFileDialog::getSaveFileName(
      commandWindow(), tr("Produce patched binary"),
      session_.filePath() + ".patched");
  if (path.isEmpty())
    return;
  const auto revision = session_.revision();
  const auto epoch = session_.epoch();
  session_.produceBinary(
      "patch_export", {{"path", path}}, this,
      [this, path](const QJsonObject &) {
        output_->append(tr("Patched binary written to %1").arg(path), 0);
      },
      [this, path, epoch, revision](const QString &, const QString &error) {
        if (epoch != session_.epoch() || revision != session_.revision())
          return;
        if (!error.contains("allow_unsigned")) {
          output_->append(error, 1);
          return;
        }
        if (QMessageBox::question(commandWindow(), tr("Export unsigned binary"),
                                  error + "\n\n" +
                                      tr("Remove the signature metadata and "
                                         "export an unsigned copy?")) !=
            QMessageBox::Yes)
          return;
        session_.produceBinary(
            "patch_export", {{"path", path}, {"allow_unsigned", true}}, this,
            [this, path](const QJsonObject &) {
              output_->append(
                  tr("Unsigned patched binary written to %1").arg(path), 0);
            });
      });
}
void MainWindow::connectPatchedBytes(ChooserView *view) {
  view->enableCommand(ChooserCommand::Delete, true);
  const auto revert = [this, view] {
    QJsonArray ranges;
    for (const auto &index : view->table()->selectionModel()->selectedRows()) {
      const auto row = view->model().rowObject(index.row());
      if (!row.isEmpty())
        ranges.append(QJsonObject{{"address", row.value("address")},
                                  {"size", row.value("size")}});
    }
    if (ranges.isEmpty())
      return;
    session_.editBytePatch(
        "patch_revert",
        {{"address", ranges.first().toObject().value("address")},
         {"ranges", ranges}},
        session_.epoch());
  };
  connect(view, &ChooserView::commandRequested, this,
          [revert](ChooserCommand command, Address) {
            if (command == ChooserCommand::Delete)
              revert();
          });
  view->table()->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(view->table(), &QWidget::customContextMenuRequested, this,
          [this, view, revert](const QPoint &at) {
            QMenu menu(view);
            auto *action =
                menu.addAction(tr("Restore selected original bytes"));
            action->setEnabled(!session_.readOnly());
            connect(action, &QAction::triggered, view, revert);
            menu.exec(view->table()->viewport()->mapToGlobal(at));
          });
}
void MainWindow::rewriteFunction() {
  const auto function = currentFunction();
  if (!function)
    return;
  auto *dialog = new RewriteFunctionDialog(session_, *function, commandView());
  dialog->setCursor(Qt::ArrowCursor);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  // A produced binary is a new input: openNow decides its PDB offer as for
  // any other, not from the previously opened file.
  connect(dialog, &RewriteFunctionDialog::openProduced, this,
          [this](const QString &path) { openNow(path, {}); });
  connect(dialog, &RewriteFunctionDialog::compareProduced, this,
          [this](const QString &path) {
            if (!comparison_)
              comparison_ = new ComparisonDialog(session_.workerPath(),
                                                 session_.filePath(), this);
            comparison_->session().start({{"path", session_.filePath()}},
                                         {{"path", path}}, {});
            comparison_->show();
            comparison_->raise();
          });
  dialog->show();
}
} // namespace neverd::gui
