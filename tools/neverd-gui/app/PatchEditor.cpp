//===- PatchEditor.cpp - Live machine-byte preview ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Display engine-produced bytes and covered instructions before commit.
//===----------------------------------------------------------------------===//
#include "PatchEditor.h"

#include "../ResourceLimits.h"
#include "Icons.h"
#include "Session.h"
#include "SettingsKeys.h"
#include "Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace neverd::gui {
PatchEditor::PatchEditor(Session &session, Address address, QString kind,
                         QString initial, QWidget *parent, bool nibble)
    : QFrame(parent), session_(session), address_(address),
      kind_(std::move(kind)), revision_(session.revision()),
      epoch_(session.epoch()) {
  setObjectName(QStringLiteral("patchEditor"));
  setFrameShape(QFrame::StyledPanel);
  setAutoFillBackground(true);
  // The owner is a text viewport. Chrome must not inherit its I-beam cursor.
  setCursor(Qt::ArrowCursor);
  auto *layout = new QVBoxLayout(this);
  header_ = new QWidget(this);
  header_->setObjectName(QStringLiteral("patchHeader"));
  auto *header = new QHBoxLayout(header_);
  header->setContentsMargins(0, 0, 0, 0);
  auto *caption = new QLabel(tr("Patch at %1 — Enter applies, Esc cancels")
                                 .arg(displayAddress(address)),
                             header_);
  caption->setWordWrap(true);
  caption->setAttribute(Qt::WA_TransparentForMouseEvents);
  header->addWidget(caption, 1);
  auto *close = new QToolButton(header_);
  close->setObjectName(QStringLiteral("patchClose"));
  close->setIcon(icon(QStringLiteral("close_small")));
  close->setToolTip(tr("Close"));
  close->setAccessibleName(tr("Close"));
  header->addWidget(close, 0, Qt::AlignTop);
  layout->addWidget(header_);
  input_ = new QLineEdit(std::move(initial), this);
  input_->setObjectName(QStringLiteral("patchInput"));
  input_->setCursor(Qt::IBeamCursor);
  input_->setAccessibleName(kind_ == "assemble" ? tr("Assembly instruction")
                                                : tr("Hexadecimal bytes"));
  input_->setMaxLength(kind_ == "assemble"
                           ? ::neverd::limits::patch::kMaxAssemblyTextBytes
                           : ::neverd::limits::patch::kMaxApplyBytes * 2);
  if (nibble)
    input_->setInputMask(QStringLiteral("HH"));
  layout->addWidget(input_);
  auto *options = new QHBoxLayout;
  fill_ = new QCheckBox(tr("Fill with NOPs"), this);
  fill_->setChecked(QSettings().value(settings::PatchFillNops, true).toBool());
  overlap_ = new QCheckBox(tr("Replace following instructions"), this);
  dialect_ = new QComboBox(this);
  dialect_->addItem(tr("Intel"), "intel");
  dialect_->addItem(tr("AT&T"), "att");
  dialect_->setCurrentIndex(dialect_->findData(
      QSettings().value(settings::PatchAssemblerDialect, "intel").toString()));
  dialect_->setAccessibleName(tr("Assembly dialect"));
  options->addWidget(fill_);
  options->addWidget(overlap_);
  options->addWidget(dialect_);
  layout->addLayout(options);
  fill_->setVisible(kind_ == "assemble");
  overlap_->setVisible(kind_ == "assemble");
  dialect_->setVisible(kind_ == "assemble" &&
                       (session.architecture().contains("x86") ||
                        session.architecture() == "x64"));
  preview_ = new QPlainTextEdit(this);
  preview_->setFont(Theme::instance().codeFont());
  showPreview(tr("Preparing preview…"));
  preview_->setObjectName(QStringLiteral("patchPreview"));
  preview_->setReadOnly(true);
  preview_->setFocusPolicy(Qt::StrongFocus);
  layout->addWidget(preview_);
  auto *buttons = new QDialogButtonBox(this);
  apply_ = buttons->addButton(tr("Apply patch"), QDialogButtonBox::AcceptRole);
  apply_->setDefault(true);
  apply_->setEnabled(false);
  apply_->setObjectName(QStringLiteral("patchApply"));
  auto *cancel = buttons->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
  cancel->setObjectName(QStringLiteral("patchCancel"));
  layout->addWidget(buttons);
  debounce_.setSingleShot(true);
  debounce_.setInterval(::neverd::limits::gui::kPatchPreviewDelayMs);
  connect(&debounce_, &QTimer::timeout, this, &PatchEditor::preview);
  const auto edited = [this] {
    ++serial_;
    planned_ = {};
    apply_->setEnabled(false);
    debounce_.start();
  };
  connect(input_, &QLineEdit::textChanged, this, edited);
  connect(fill_, &QCheckBox::toggled, this, edited);
  connect(fill_, &QCheckBox::toggled, this, [](bool value) {
    QSettings().setValue(settings::PatchFillNops, value);
  });
  connect(overlap_, &QCheckBox::toggled, this, edited);
  connect(dialect_, &QComboBox::currentIndexChanged, this, edited);
  connect(dialect_, &QComboBox::currentIndexChanged, this, [this] {
    QSettings().setValue(settings::PatchAssemblerDialect,
                         dialect_->currentData());
  });
  connect(buttons, &QDialogButtonBox::accepted, this, &PatchEditor::apply);
  connect(buttons, &QDialogButtonBox::rejected, this, &PatchEditor::cancelled);
  connect(close, &QToolButton::clicked, this, &PatchEditor::cancelled);
  for (auto *child : findChildren<QWidget *>())
    child->installEventFilter(this);
  if (parent)
    parent->installEventFilter(this);
  connect(&session_, &Session::unloaded, this, &PatchEditor::cancelled);
  input_->setFocus();
  if (nibble)
    input_->setCursorPosition(0);
  else
    input_->selectAll();
  // Reserve the final preview area before the first paint. Neither a loading
  // row nor a later result may resize the panel and flip its caret anchor.
  ensurePolished();
  preview_->ensurePolished();
  const int lines = kind_ == "assemble"
                        ? ::neverd::limits::gui::kPatchAssemblyPreviewLines
                        : ::neverd::limits::gui::kPatchBytePreviewLines;
  const int height = lines * preview_->fontMetrics().lineSpacing() +
                     int(preview_->document()->documentMargin() * 2) +
                     preview_->frameWidth() * 2;
  preview_->setFixedHeight(
      std::min(height, ::neverd::limits::gui::kPatchPreviewHeight));
  adjustSize();
  debounce_.start();
}
void PatchEditor::openAt(const QRect &anchor) {
  anchor_ = anchor;
  placed_ = true;
  moved_ = false;
  setFixedWidth(std::min(parentWidget()->width(),
                         ::neverd::limits::gui::kPatchEditorWidth));
  adjustSize();
  place();
  show();
  raise();
  input_->setFocus();
}
void PatchEditor::place() {
  if (!placed_ || !parentWidget())
    return;
  const auto bounds = parentWidget()->rect();
  QPoint position = pos();
  if (!moved_) {
    const int gap = ::neverd::limits::gui::kPatchAnchorGap;
    const int below = anchor_.bottom() + 1 + gap;
    const int above = anchor_.top() - gap - height();
    // Keep the edited row visible, preferring the next line when it fits.
    position = QPoint(anchor_.left(),
                      below + height() <= bounds.height() || above < 0 ? below
                                                                       : above);
  }
  position.setX(
      std::clamp(position.x(), 0, std::max(0, bounds.width() - width())));
  position.setY(
      std::clamp(position.y(), 0, std::max(0, bounds.height() - height())));
  move(position);
}
void PatchEditor::resizeEvent(QResizeEvent *event) {
  QFrame::resizeEvent(event);
  place();
}
void PatchEditor::showPreview(const QString &text) {
  preview_->setPlainText(text);
}
QJsonObject PatchEditor::request() const {
  auto text = input_->text().trimmed();
  if (kind_ == "bytes") {
    text.remove(' ');
    text.remove('\t');
  }
  return {{"address", hexAddress(address_)},
          {"kind", kind_},
          {kind_ == "assemble" ? "text" : "bytes", text},
          {"fill_nops", fill_->isChecked()},
          {"allow_overlap", overlap_->isChecked()},
          {"dialect", dialect_->currentData().toString()}};
}
void PatchEditor::preview() {
  const auto serial = ++serial_;
  auto payload = request();
  payload["allow_overlap"] = true;
  session_.read(
      "patch_preview", payload, this,
      [this, serial](const QJsonObject &plan) {
        if (serial != serial_ || session_.epoch() != epoch_ ||
            session_.revision() != revision_)
          return;
        planned_ = plan;
        QStringList lines{
            tr("Original: %1")
                .arg(
                    plan.value("original")
                        .toString()
                        .left(
                            ::neverd::limits::gui::kPatchPreviewHexCharacters)),
            tr("New: %1").arg(plan.value("current").toString().left(
                ::neverd::limits::gui::kPatchPreviewHexCharacters))};
        const auto covers = plan.value("covers").toArray();
        for (const auto &entry : covers) {
          const auto row = entry.toObject();
          lines << row.value("address").toString() + "  " +
                       row.value("text").toString();
        }
        for (const auto &warning : plan.value("warnings").toArray())
          lines << warning.toString();
        const bool extra = kind_ == "assemble" && covers.size() > 1;
        if (extra && !overlap_->isChecked())
          lines << tr("Review the covered instructions and enable replacement "
                      "to apply.");
        if (plan.value("original").toString().size() >
            ::neverd::limits::gui::kPatchPreviewHexCharacters)
          lines << tr("Byte display truncated; the full range is %1 bytes.")
                       .arg(plan.value("size").toInteger());
        showPreview(lines.join('\n'));
        apply_->setEnabled(!extra || overlap_->isChecked());
      },
      [this, serial](const QString &, const QString &error) {
        if (serial != serial_)
          return;
        showPreview(error);
        apply_->setEnabled(false);
        planned_ = {};
      });
}
void PatchEditor::apply() {
  if (!apply_->isEnabled() || planned_.isEmpty())
    return;
  if (session_.epoch() != epoch_ || session_.revision() != revision_) {
    showPreview(tr("The input changed. Reopen the patch editor."));
    apply_->setEnabled(false);
    return;
  }
  auto payload = request();
  payload["expected"] = planned_.value("original");
  debounce_.stop();
  ++serial_;
  setEnabled(false);
  apply_->setEnabled(false);
  const QPointer<PatchEditor> self(this);
  session_.editBytePatch(
      "patch_apply", payload, epoch_,
      [self] {
        if (self)
          emit self->applied();
      },
      [self](const QString &, const QString &error) {
        if (!self)
          return;
        self->setEnabled(true);
        self->showPreview(error);
        self->apply_->setEnabled(self->session_.epoch() == self->epoch_ &&
                                 self->session_.revision() == self->revision_);
      });
}
bool PatchEditor::eventFilter(QObject *object, QEvent *event) {
  if (object == parentWidget()) {
    if (event->type() == QEvent::Resize && placed_) {
      setFixedWidth(std::min(parentWidget()->width(),
                             ::neverd::limits::gui::kPatchEditorWidth));
      adjustSize();
      place();
    }
    return QFrame::eventFilter(object, event);
  }
  if (object == header_) {
    if (event->type() == QEvent::MouseButtonPress) {
      const auto *mouse = static_cast<QMouseEvent *>(event);
      if (mouse->button() == Qt::LeftButton) {
        dragStart_ = mouse->globalPosition().toPoint();
        dragPosition_ = pos();
        pressed_ = true;
        dragging_ = false;
        return true;
      }
    } else if (event->type() == QEvent::MouseMove && pressed_) {
      const auto *mouse = static_cast<QMouseEvent *>(event);
      const auto delta = mouse->globalPosition().toPoint() - dragStart_;
      if (dragging_ ||
          delta.manhattanLength() >= QApplication::startDragDistance()) {
        dragging_ = true;
        moved_ = true;
        move(dragPosition_ + delta);
        place();
      }
      return true;
    } else if (event->type() == QEvent::MouseButtonRelease && pressed_) {
      pressed_ = false;
      return true;
    }
  }
  if (event->type() == QEvent::ShortcutOverride) {
    event->accept();
    return true;
  }
  if (event->type() == QEvent::KeyPress) {
    const auto key = static_cast<QKeyEvent *>(event)->key();
    if (kind_ == "assemble" && (key == Qt::Key_Up || key == Qt::Key_Down) &&
        object == input_) {
      emit navigateRequested(key == Qt::Key_Down ? 1 : -1);
      return true;
    }
    if (key == Qt::Key_Escape) {
      emit cancelled();
      return true;
    }
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
      // A focused Cancel or close button must dismiss the editor, just as a
      // click does. Return in the fields still applies the preview.
      if (auto *button = qobject_cast<QPushButton *>(object))
        button->click();
      else if (auto *button = qobject_cast<QToolButton *>(object))
        button->click();
      else
        apply();
      return true;
    }
    if (key == Qt::Key_F2) {
      apply();
      return true;
    }
  }
  return QFrame::eventFilter(object, event);
}
} // namespace neverd::gui
