#include "WorkbenchStyle.h"

#include "../Config.h"
#include "Icons.h"
#include "Theme.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileSystemModel>
#include <QFontDialog>
#include <QGroupBox>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QStyleFactory>
#include <QStyleOption>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <algorithm>

namespace neverd::gui {
namespace {
/// The object name of Qt's file dialog places pane.
constexpr char FileDialogPlaces[] = "sidebar";
/// The object name of Qt's file dialog detail view.
constexpr char FileDialogDetails[] = "treeView";
/// Marks a detail view whose columns already follow its folders.
constexpr char FittedColumns[] = "neverdFittedColumns";

/// Lets a file dialog's names take the width its size, type and date leave.
/// Qt sizes those three from samples narrower than what it shows ("128.88
/// GB", "mp3Folder"), so sizes, types and dates lose their middles.
void fitFileColumns(QFileDialog &dialog, QTreeView &details) {
  auto *header = details.header();
  if (header->count() == 0)
    return;
  header->setStretchLastSection(false);
  header->setSectionResizeMode(0, QHeaderView::Stretch);
  auto *files = dialog.findChild<QFileSystemModel *>();
  if (!files || details.property(FittedColumns).toBool())
    return;
  details.setProperty(FittedColumns, true);
  // The columns follow the folder shown, once its entries have arrived.
  auto *fit = new QTimer(&details);
  fit->setSingleShot(true);
  QObject::connect(fit, &QTimer::timeout, &details, [&details] {
    for (int column = 1; column < details.header()->count(); ++column) {
      if (details.header()->isSectionHidden(column))
        continue;
      details.resizeColumnToContents(column);
      details.setColumnWidth(
          column, std::min(details.columnWidth(column),
                           ::neverd::limits::gui_config::
                               kWorkbenchStyleFileColumnMaximumWidth));
    }
  });
  const auto refit = [fit] { fit->start(0); };
  QObject::connect(files, &QFileSystemModel::rootPathChanged, fit, refit);
  QObject::connect(files, &QFileSystemModel::directoryLoaded, fit, refit);
  QObject::connect(files, &QAbstractItemModel::rowsInserted, fit, refit);
  refit();
}

/// Opens Qt's file and font dialogs with room for their lists; their
/// layouts' least sizes show only a few rows.
void openWithRoom(QDialog &dialog) {
  const QSize room =
      qobject_cast<QFileDialog *>(&dialog)
          ? ::neverd::limits::gui_config::kWorkbenchStyleFileDialogSize
          : ::neverd::limits::gui_config::kWorkbenchStyleFontDialogSize;
  QSize size = dialog.size().expandedTo(room.grownBy(dialog.contentsMargins()));
  if (const QScreen *screen = dialog.screen())
    size = size.boundedTo(screen->availableSize());
  dialog.resize(size);
}

/// Where keyboard focus frames a control: a button's whole face, a check
/// box's or radio button's box with its label, a tab; the rectangle Qt gives
/// hugs only the label.
QRectF focusFrame(const QStyleOption &option, const QWidget *widget) {
  QRect frame = option.rect;
  if (qobject_cast<const QPushButton *>(widget) ||
      qobject_cast<const QToolButton *>(widget)) {
    frame = widget->rect();
  } else if (qobject_cast<const QCheckBox *>(widget) ||
             qobject_cast<const QRadioButton *>(widget)) {
    const auto *box = static_cast<const QAbstractButton *>(widget);
    QStyleOptionButton button;
    button.initFrom(box);
    button.text = box->text();
    button.icon = box->icon();
    button.iconSize = box->iconSize();
    const QRect indicator =
        widget->style()->subElementRect(qobject_cast<const QRadioButton *>(box)
                                            ? QStyle::SE_RadioButtonIndicator
                                            : QStyle::SE_CheckBoxIndicator,
                                        &button, widget);
    const int margin =
        ::neverd::limits::gui_config::kWorkbenchStyleFocusFrameMargin;
    frame = frame.united(indicator).adjusted(-margin, -margin, margin, margin) &
            widget->rect();
  } else if (const auto *tabs = qobject_cast<const QTabBar *>(widget)) {
    if (const int tab = tabs->tabAt(option.rect.center()); tab >= 0)
      frame = tabs->tabRect(tab);
  }
  const qreal inset =
      ::neverd::limits::gui_config::kWorkbenchStyleFocusFrameWidth / 2;
  return QRectF(frame).adjusted(inset, inset, -inset, -inset);
}

bool drawsMnemonics(const QWidget *widget) {
  return qobject_cast<const QMenuBar *>(widget) ||
         qobject_cast<const QMenu *>(widget) ||
         qobject_cast<const QAbstractButton *>(widget) ||
         qobject_cast<const QLabel *>(widget) ||
         qobject_cast<const QGroupBox *>(widget) ||
         qobject_cast<const QTabBar *>(widget);
}
} // namespace

WorkbenchStyle::WorkbenchStyle()
    : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

int WorkbenchStyle::pixelMetric(PixelMetric metric, const QStyleOption *option,
                                const QWidget *widget) const {
  if (metric == PM_ToolBarIconSize)
    return ::neverd::limits::gui_config::kWorkbenchStyleSmallIconSize;
  return QProxyStyle::pixelMetric(metric, option, widget);
}

void WorkbenchStyle::drawPrimitive(PrimitiveElement element,
                                   const QStyleOption *option,
                                   QPainter *painter,
                                   const QWidget *widget) const {
  // Keyboard focus frames the control with a thin line of the focus color, as
  // Visual Studio Code draws its focus border, in place of Fusion's tinted box
  // behind the label.  The fields' focus border is the same color.  As in
  // Fusion, focus that the pointer gave draws none.
  if (element == PE_FrameFocusRect && option) {
    if (!(option->state & State_KeyboardFocusChange))
      return;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    QPen pen(Theme::instance().chrome(QStringLiteral("Focus")));
    pen.setWidthF(::neverd::limits::gui_config::kWorkbenchStyleFocusFrameWidth);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    const QRectF frame = focusFrame(*option, widget);
    const qreal radius =
        ::neverd::limits::gui_config::kWorkbenchStyleFocusFrameRadius;
    painter->drawRoundedRect(frame, radius, radius);
    // A push button keeps a line of the surface between the frame and its
    // face, as Visual Studio Code sets its focus border off a button, so the
    // frame shows on a primary button's accent too.
    if (qobject_cast<const QPushButton *>(widget)) {
      const qreal gap =
          ::neverd::limits::gui_config::kWorkbenchStyleFocusFrameGap;
      pen.setColor(Theme::instance().chrome(QStringLiteral("Window")));
      painter->setPen(pen);
      painter->drawRoundedRect(frame.adjusted(gap, gap, -gap, -gap),
                               std::max<qreal>(0, radius - gap),
                               std::max<qreal>(0, radius - gap));
    }
    painter->restore();
    return;
  }
  QProxyStyle::drawPrimitive(element, option, painter, widget);
}

int WorkbenchStyle::styleHint(StyleHint hint, const QStyleOption *option,
                              const QWidget *widget,
                              QStyleHintReturn *returnData) const {
  switch (hint) {
  case SH_UnderlineShortcut:
    return cues_;
  // A combo box's list highlights the item under the pointer, as a menu does.
  case SH_ComboBox_ListMouseTracking:
    return true;
  case SH_Menu_Scrollable:
    // Fusion otherwise wraps tall menus into columns. Keep one column with
    // Qt's scroll arrows, wheel handling and keyboard navigation on all hosts.
    return true;
  case SH_DialogButtonLayout:
    return QDialogButtonBox::GnomeLayout; // Cancel, then the primary action.
  case SH_DialogButtonBox_ButtonsHaveIcons:
  case SH_ComboBox_UseNativePopup:
  case SH_MessageBox_CenterButtons:
    return false;
  case SH_ToolButtonStyle:
    return Qt::ToolButtonIconOnly;
  case SH_Widget_Animation_Duration:
    // KDDockWidgets disables tab animations: Qt can otherwise paint a tab
    // after it was removed during a drag. Dock tabs inherit this style.
    if (qobject_cast<const QTabBar *>(widget))
      return 0;
    return QProxyStyle::styleHint(hint, option, widget, returnData);
  default:
    return QProxyStyle::styleHint(hint, option, widget, returnData);
  }
}

QIcon WorkbenchStyle::standardIcon(StandardPixmap pixmap, const QStyleOption *,
                                   const QWidget *) const {
  switch (pixmap) {
  // A file dialog's tool buttons and the places it lists.
  case SP_ArrowBack:
  case SP_ArrowLeft:
  case SP_FileDialogBack:
    return icon(QStringLiteral("back"));
  case SP_ArrowForward:
  case SP_ArrowRight:
  case SP_CommandLink:
    return icon(QStringLiteral("forward"));
  case SP_FileDialogToParent:
  case SP_ArrowUp:
    return icon(QStringLiteral("up"));
  case SP_FileDialogNewFolder:
    return icon(QStringLiteral("folder_new"));
  case SP_FileDialogListView:
  case SP_FileDialogContentsView:
    return icon(QStringLiteral("view_list"));
  case SP_FileDialogDetailedView:
  case SP_FileDialogInfoView:
    return icon(QStringLiteral("view_details"));
  case SP_DirIcon:
  case SP_DirClosedIcon:
  case SP_DirLinkIcon:
  case SP_DirHomeIcon:
    return icon(QStringLiteral("folder"));
  case SP_DirOpenIcon:
  case SP_DirLinkOpenIcon:
  case SP_DialogOpenButton:
    return icon(QStringLiteral("open"));
  case SP_FileIcon:
  case SP_FileLinkIcon:
    return icon(QStringLiteral("file"));
  case SP_ComputerIcon:
  case SP_DesktopIcon:
    return icon(QStringLiteral("computer"));
  case SP_DriveHDIcon:
  case SP_DriveFDIcon:
  case SP_DriveCDIcon:
  case SP_DriveDVDIcon:
  case SP_DriveNetIcon:
    return icon(QStringLiteral("drive"));
  case SP_MessageBoxInformation:
    return icon(QStringLiteral("message_information"));
  case SP_MessageBoxWarning:
    return icon(QStringLiteral("message_warning"));
  case SP_MessageBoxCritical:
    return icon(QStringLiteral("message_critical"));
  case SP_MessageBoxQuestion:
  case SP_DialogHelpButton:
  case SP_TitleBarContextHelpButton:
    return icon(QStringLiteral("help"));
  case SP_LineEditClearButton:
  case SP_TitleBarCloseButton:
  case SP_DockWidgetCloseButton:
  case SP_DialogCloseButton:
  case SP_DialogCancelButton:
  case SP_DialogNoButton:
  case SP_DialogNoToAllButton:
  case SP_TabCloseButton:
    return icon(QStringLiteral("close_small"));
  case SP_TitleBarMinButton:
    return icon(QStringLiteral("check_partial"));
  case SP_TitleBarMaxButton:
    return icon(QStringLiteral("maximize"));
  case SP_TitleBarNormalButton:
    return icon(QStringLiteral("restore"));
  case SP_TitleBarShadeButton:
    return icon(QStringLiteral("up"));
  case SP_TitleBarUnshadeButton:
  case SP_ArrowDown:
    return icon(QStringLiteral("down"));
  case SP_TitleBarMenuButton:
    return icon(QStringLiteral("app"));
  case SP_ToolBarHorizontalExtensionButton:
    return icon(QStringLiteral("forward"));
  case SP_ToolBarVerticalExtensionButton:
    return icon(QStringLiteral("down"));
  case SP_FileDialogStart:
    return icon(QStringLiteral("back"));
  case SP_FileDialogEnd:
    return icon(QStringLiteral("forward"));
  case SP_DialogOkButton:
  case SP_DialogApplyButton:
  case SP_DialogYesButton:
  case SP_DialogYesToAllButton:
    return icon(QStringLiteral("check"));
  case SP_DialogSaveButton:
  case SP_DialogSaveAllButton:
    return icon(QStringLiteral("save"));
  case SP_TrashIcon:
  case SP_DialogDiscardButton:
    return icon(QStringLiteral("local_types_delete"));
  case SP_DialogResetButton:
  case SP_RestoreDefaultsButton:
    return icon(QStringLiteral("undo"));
  case SP_DialogRetryButton:
  case SP_BrowserReload:
    return icon(QStringLiteral("refresh"));
  case SP_DialogAbortButton:
  case SP_BrowserStop:
    return icon(QStringLiteral("cancel"));
  case SP_DialogIgnoreButton:
    return icon(QStringLiteral("forward"));
  case SP_VistaShield:
    return icon(QStringLiteral("lock"));
  default:
    // Unused media/custom identifiers have no workbench glyph. Never ask
    // the desktop icon theme to supply a different visual language.
    return {};
  }
}

QPixmap WorkbenchStyle::standardPixmap(StandardPixmap pixmap,
                                       const QStyleOption *option,
                                       const QWidget *widget) const {
  const int size = ::neverd::limits::gui_config::kWorkbenchStyleSmallIconSize;
  return standardIcon(pixmap, option, widget)
      .pixmap(QSize(size, size), widget ? widget->devicePixelRatioF() : 1.0);
}

void WorkbenchStyle::polish(QWidget *widget) {
  QProxyStyle::polish(widget);
  if (auto *menus = qobject_cast<QMenuBar *>(widget))
    menus->setNativeMenuBar(false);
  if (widget->isWindow() && (widget->windowType() == Qt::Popup ||
                             widget->windowType() == Qt::ToolTip))
    widget->setWindowFlag(Qt::NoDropShadowWindowHint);
  // The combo box's own delegates draw items themselves; a styled delegate
  // draws them as the style sheet's item rules say, like a menu's.
  if (auto *combo = qobject_cast<QComboBox *>(widget);
      combo && !qobject_cast<QStyledItemDelegate *>(combo->itemDelegate()))
    combo->setItemDelegate(new QStyledItemDelegate(combo));
  // A heading too long for its column ends in an ellipsis; Qt's header
  // otherwise centers it and cuts off both of its ends.
  if (auto *header = qobject_cast<QHeaderView *>(widget))
    header->setTextElideMode(Qt::ElideRight);
  // Qt's own file dialog lists folders and files with the icon theme's
  // icons unless it is given others.
  if (auto *files = qobject_cast<QFileDialog *>(widget);
      files && files->iconProvider() != fileIcons())
    files->setIconProvider(fileIcons());
  // Its places pane draws them at 24 pixels, where icons drawn on the 16
  // pixel grid blur; at 16 they are as sharp as in the file list.
  if (auto *places = qobject_cast<QAbstractItemView *>(widget);
      places && places->objectName() == QLatin1String(FileDialogPlaces) &&
      qobject_cast<QFileDialog *>(places->window()))
    places->setIconSize(
        QSize(::neverd::limits::gui_config::kWorkbenchStyleSmallIconSize,
              ::neverd::limits::gui_config::kWorkbenchStyleSmallIconSize));
}

void WorkbenchStyle::polish(QApplication *application) {
  QProxyStyle::polish(application);
  application->installEventFilter(this);
}

void WorkbenchStyle::unpolish(QApplication *application) {
  application->removeEventFilter(this);
  QProxyStyle::unpolish(application);
}

bool WorkbenchStyle::eventFilter(QObject *watched, QEvent *event) {
  switch (event->type()) {
  case QEvent::KeyPress:
    if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Alt) {
      heldByMenu_ = false;
      setKeyboardCues(true);
      break;
    }
    [[fallthrough]];
  case QEvent::MouseButtonPress:
  case QEvent::MouseMove:
    // The release of an Alt that the window manager took never arrives.
    if (cues_ && !heldByMenu_ &&
        !(static_cast<QInputEvent *>(event)->modifiers() & Qt::AltModifier))
      setKeyboardCues(false);
    break;
  case QEvent::KeyRelease:
    if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Alt) {
      heldByMenu_ = QApplication::activePopupWidget() != nullptr;
      if (!heldByMenu_)
        setKeyboardCues(false);
    }
    break;
  case QEvent::Hide:
    // The popup list changes after the menu hides.
    if (heldByMenu_ && qobject_cast<QMenu *>(watched))
      QTimer::singleShot(0, this, &WorkbenchStyle::releaseAfterMenu);
    break;
  case QEvent::Show:
    // Before the dialog centers itself on its parent at its new size.  A
    // file dialog's detail view has its columns only once it shows.
    if (auto *dialog = qobject_cast<QDialog *>(watched);
        dialog && !event->spontaneous() &&
        (qobject_cast<QFileDialog *>(dialog) ||
         qobject_cast<QFontDialog *>(dialog))) {
      openWithRoom(*dialog);
      auto *files = qobject_cast<QFileDialog *>(dialog);
      if (auto *details = files ? files->findChild<QTreeView *>(
                                      QLatin1String(FileDialogDetails))
                                : nullptr)
        fitFileColumns(*files, *details);
    }
    break;
  case QEvent::ApplicationStateChange:
    if (QGuiApplication::applicationState() != Qt::ApplicationActive) {
      heldByMenu_ = false;
      setKeyboardCues(false);
    }
    break;
  default:
    break;
  }
  return QProxyStyle::eventFilter(watched, event);
}

void WorkbenchStyle::releaseAfterMenu() {
  if (!heldByMenu_ || QApplication::activePopupWidget())
    return;
  heldByMenu_ = false;
  setKeyboardCues(false);
}

void WorkbenchStyle::setKeyboardCues(bool on) {
  if (cues_ == on)
    return;
  cues_ = on;
  for (QWidget *widget : QApplication::allWidgets())
    if (widget->isVisible() && drawsMnemonics(widget))
      widget->update();
}

} // namespace neverd::gui
