#include "WorkbenchStyle.h"

#include "../Config.h"
#include "DevicePixels.h"
#include "Icons.h"
#include "Theme.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
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
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStatusBar>
#include <QStyleFactory>
#include <QStyleOption>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <algorithm>
#include <optional>

namespace neverd::gui {
namespace {
/// The object name of Qt's file dialog places pane.
constexpr char FileDialogPlaces[] = "sidebar";
/// The object name of Qt's file dialog detail view.
constexpr char FileDialogDetails[] = "treeView";
/// Marks a detail view whose columns already follow its folders.
constexpr char FittedColumns[] = "neverdFittedColumns";
/// Marks a header whose columns keep their headings whole.
constexpr char FittedHeadings[] = "neverdFittedHeadings";
/// The object names of the widgets whose boxes the theme style sheet names:
/// the Quick Start dialog's actions, which paint themselves, the patch
/// editor, the card a file's loading shows on, and the command palette.
constexpr char QuickStartAction[] = "quickStartAction";
constexpr char PatchEditor[] = "patchEditor";
constexpr char LoadProgressCard[] = "loadProgressCard";
constexpr char CommandPalette[] = "commandPalette";

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
/// hugs only the label.  The frame's line runs just inside it.
QRect focusFrame(const QStyleOption &option, const QWidget *widget) {
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
  return frame;
}

/// Keeps each column a person can resize at least as wide as its heading:
/// when the header first shows, when its columns or headings change and when
/// the language changes.  A person can still narrow a column afterwards.
class HeadingFitter final : public QObject {
public:
  explicit HeadingFitter(QHeaderView *header)
      : QObject(header), header_(header) {
    header->installEventFilter(this);
    connect(header, &QHeaderView::sectionCountChanged, this,
            &HeadingFitter::schedule);
    schedule();
  }

  bool eventFilter(QObject *, QEvent *event) override {
    switch (event->type()) {
    case QEvent::Show:
      if (!shown_) {
        shown_ = true;
        schedule();
      }
      break;
    case QEvent::LanguageChange:
    case QEvent::FontChange:
    case QEvent::StyleChange:
      schedule();
      break;
    default:
      break;
    }
    return false;
  }

private:
  void schedule() {
    if (pending_)
      return;
    pending_ = true;
    // After the models answer in the new language and the style sheet sets
    // the headings' font and padding.
    QTimer::singleShot(0, this, [this] {
      pending_ = false;
      fit();
    });
  }

  void fit() {
    if (model_ != header_->model()) {
      if (model_)
        disconnect(model_, nullptr, this, nullptr);
      model_ = header_->model();
      if (model_) {
        connect(model_, &QAbstractItemModel::headerDataChanged, this,
                &HeadingFitter::schedule);
        connect(model_, &QAbstractItemModel::modelReset, this,
                &HeadingFitter::schedule);
      }
    }
    if (!model_)
      return;
    for (int logical = 0; logical < header_->count(); ++logical) {
      if (header_->isSectionHidden(logical) ||
          header_->sectionResizeMode(logical) != QHeaderView::Interactive)
        continue;
      // A last column that stretches takes the size it is given as the least
      // it shrinks to, so it keeps its heading too, and a table narrower than
      // its headings scrolls sideways.
      if (const int heading = header_->sectionSizeHint(logical);
          header_->sectionSize(logical) < heading)
        header_->resizeSection(logical, heading);
    }
  }

  QHeaderView *header_;
  QPointer<QAbstractItemModel> model_;
  bool shown_ = false;
  bool pending_ = false;
};

/// The color of the surface \p widget stands on: its nearest ancestor that
/// paints an opaque background.  A style sheet's background reaches the
/// palette of the widget it styles.
QColor surfaceBehind(const QWidget *widget) {
  for (const QWidget *ancestor = widget ? widget->parentWidget() : nullptr;
       ancestor; ancestor = ancestor->parentWidget()) {
    const QColor color = ancestor->palette().color(ancestor->backgroundRole());
    if (color.alpha() == 255 &&
        (ancestor->autoFillBackground() ||
         ancestor->testAttribute(Qt::WA_StyledBackground) ||
         ancestor->isWindow()))
      return color;
  }
  return Theme::instance().chrome(QStringLiteral("Window"));
}

/// A theme glyph of the chrome, in the current theme's variant.
QIcon chromeGlyph(const char *name) {
  return icon(QLatin1String(name) + QLatin1Char('-') +
              Theme::modeName(Theme::instance().mode()));
}

/// A menu item's padding box, before its direction mirrors it: room before
/// the icon column, after the shortcut, and above and below the text.
QRect menuItemContents(const QRect &rect) {
  return rect.adjusted(
      ::neverd::limits::gui_config::kWorkbenchStyleMenuItemPaddingLeft,
      ::neverd::limits::gui_config::kWorkbenchStyleMenuItemPaddingVertical,
      -::neverd::limits::gui_config::kWorkbenchStyleMenuItemPaddingRight,
      -::neverd::limits::gui_config::kWorkbenchStyleMenuItemPaddingVertical);
}

/// A square of \p size at the leading edge of \p rect, \p inset in from it.
QRect leadingSquare(Qt::LayoutDirection direction, const QRect &rect, int size,
                    int inset) {
  return QStyle::alignedRect(direction, Qt::AlignLeft | Qt::AlignVCenter,
                             QSize(size, size), rect)
      .translated(direction == Qt::LeftToRight ? inset : -inset, 0);
}

bool drawsMnemonics(const QWidget *widget) {
  return qobject_cast<const QMenuBar *>(widget) ||
         qobject_cast<const QMenu *>(widget) ||
         qobject_cast<const QAbstractButton *>(widget) ||
         qobject_cast<const QLabel *>(widget) ||
         qobject_cast<const QGroupBox *>(widget) ||
         qobject_cast<const QTabBar *>(widget);
}

/// A color of the theme's chrome (ChromeColors.def).
QColor chrome(const char *name) {
  return Theme::instance().chrome(QLatin1String(name));
}

/// The chrome color \p name where the theme draws it, as a high contrast
/// theme outlines what the others fill, or else \p otherwise.
QColor chromeOr(const char *name, const QColor &otherwise) {
  const QColor color = chrome(name);
  return color.alpha() > 0 ? color : otherwise;
}

/// The style option \p widget hands the style as it paints itself, with the
/// state the style sheet's pseudo-states read: a tool button counts as
/// pressed while its menu shows, a push button as a dialog's default while it
/// has focus.
template <typename Widget, typename Option>
Option paintedOption(const Widget &widget) {
  // Qt lets only the widget's subclasses fill one in.
  struct Access : Widget {
    using Widget::initStyleOption;
  };
  Option option;
  (widget.*(&Access::initStyleOption))(&option);
  return option;
}

/// Whether \p widget stands inside an \p Ancestor, as a style sheet's
/// descendant selector matches it.
template <typename Ancestor> bool inside(const QWidget &widget) {
  for (const QWidget *parent = widget.parentWidget(); parent;
       parent = parent->parentWidget())
    if (qobject_cast<const Ancestor *>(parent))
      return true;
  return false;
}

/// Whether \p widget is a field: a line edit other than a combo box's or spin
/// box's own editor, a combo box, a spin box, or a text edit that takes
/// typing.  A text edit that only shows text has no box.
bool isField(const QWidget &widget) {
  if (qobject_cast<const QComboBox *>(&widget) ||
      qobject_cast<const QAbstractSpinBox *>(&widget))
    return true;
  if (qobject_cast<const QLineEdit *>(&widget))
    return !inside<QComboBox>(widget) &&
           !qobject_cast<const QAbstractSpinBox *>(widget.parentWidget());
  if (const auto *plain = qobject_cast<const QPlainTextEdit *>(&widget))
    return !plain->isReadOnly();
  if (const auto *rich = qobject_cast<const QTextEdit *>(&widget))
    return !rich->isReadOnly();
  return false;
}

/// A field's box: the input fill and gray outline, the focus color's outline
/// while it has focus, and only a quieter outline while it cannot take input,
/// as Windows 11 draws a disabled text box.  A text edit, which the style
/// sheet frames as it frames every scroll area, takes its outline only with
/// focus and shows nothing while it cannot take input.
DeviceBox fieldFace(const QWidget &field) {
  DeviceBox face;
  face.radius = ::neverd::limits::gui_config::kWorkbenchStyleControlRadius;
  const bool editor = qobject_cast<const QAbstractScrollArea *>(&field);
  if (!field.isEnabled()) {
    face.border = editor ? QColor() : chrome("DisabledBorder");
    face.borderWidth = editor ? 0 : 1;
    return face;
  }
  face.fill = chrome("Input");
  if (editor && !field.hasFocus())
    face.borderWidth = 0;
  else
    face.border = chrome(field.hasFocus() ? "Focus" : "InputBorder");
  return face;
}

/// A push button's face: a dialog's default in the accent, any other in the
/// button gray, each a shade darker under the pointer.  One that cannot act
/// gives up its fill and keeps its outline; a default one keeps a faint
/// accent instead, so the dialog's default action stays recognizable.  A
/// high contrast theme outlines a button under the pointer in place of the
/// darker shade.
DeviceBox pushButtonFace(const QPushButton &button) {
  const auto option = paintedOption<QPushButton, QStyleOptionButton>(button);
  const bool enabled = option.state & QStyle::State_Enabled;
  const bool hover = enabled && (option.state & QStyle::State_MouseOver);
  DeviceBox face;
  face.radius = ::neverd::limits::gui_config::kWorkbenchStyleControlRadius;
  if (option.features & QStyleOptionButton::DefaultButton) {
    if (!enabled) {
      face.fill = chrome("ButtonPrimaryDisabled");
      face.border = chrome("ButtonPrimaryDisabledBorder");
      return face;
    }
    face.fill = chrome(hover ? "ButtonPrimaryHover" : "ButtonPrimary");
    face.border =
        hover ? chromeOr("ButtonHoverBorder", chrome("ButtonPrimaryBorder"))
              : chrome("ButtonPrimaryBorder");
    return face;
  }
  if (!enabled) {
    face.border = chrome("DisabledBorder");
    return face;
  }
  face.fill = chrome(hover ? "ButtonHover" : "Button");
  face.border = hover ? chromeOr("ButtonHoverBorder", chrome("Border"))
                      : chrome("Border");
  return face;
}

/// A tool button's face, flat until the pointer is over it: the hover gray
/// then, the selection color while it is pressed or on, and an outline of the
/// focus color while it has focus.  A toggle whose glyph shows its state, as
/// a disclosure's chevron points, takes no fill for it; one that is on but
/// cannot act keeps an outline in place of its fill.  A tab bar's scroll
/// arrows are square and fill with the bar.
DeviceBox toolButtonFace(const QToolButton &button) {
  const auto option =
      paintedOption<QToolButton, QStyleOptionToolButton>(button);
  const bool enabled = option.state & QStyle::State_Enabled;
  const bool hover = enabled && (option.state & QStyle::State_MouseOver);
  const bool pressed = option.state & QStyle::State_Sunken;
  const bool on = option.state & QStyle::State_On;
  DeviceBox face;
  if (inside<QTabBar>(button)) {
    face.fill = chrome(hover             ? "ToolBarHover"
                       : (pressed || on) ? "ToolButtonChecked"
                                         : "TabBar");
    // Inside the button; the style sheet gives these arrows no border.
    if (hover)
      face.border = chrome("ToolBarHoverBorder");
    return face;
  }
  face.radius = ::neverd::limits::gui_config::kWorkbenchStyleToolButtonRadius;
  const bool glyph = button.property("glyphToggle").toBool();
  if (glyph && hover)
    face.fill = chrome("ToolBarHover");
  else if (!glyph && (pressed || on) && enabled)
    face.fill = chrome("ToolButtonChecked");
  else if (!glyph && hover)
    face.fill = chrome("ToolBarHover");
  if (on && !enabled)
    face.border = glyph ? QColor() : chrome("DisabledBorder");
  else if (option.state & QStyle::State_HasFocus)
    face.border = chrome("Focus");
  else if (hover)
    face.border = chrome("ToolBarHoverBorder");
  return face;
}

/// The box the workbench paints under \p widget's own painting, in place of
/// the one its style sheet rule describes: a field's, a button's and a tool
/// button's face, and the frames of a combo box's list, the patch editor, the
/// load card and the command palette.  Their rules keep the box model, with a
/// transparent border of the same width, and the colors are the rules' that
/// came before.  Qt would draw the box across device pixels at a fractional
/// scale (DevicePixels).  None: the widget's own rule paints it, if any.
std::optional<DeviceBox> faceOf(const QWidget &widget) {
  namespace config = ::neverd::limits::gui_config;
  if (isField(widget))
    return fieldFace(widget);
  if (const auto *button = qobject_cast<const QPushButton *>(&widget)) {
    if (button->objectName() == QLatin1String(QuickStartAction))
      return std::nullopt;
    return pushButtonFace(*button);
  }
  if (const auto *button = qobject_cast<const QToolButton *>(&widget)) {
    // A line edit's clear button paints only its glyph.
    if (button->inherits("QLineEditIconButton"))
      return std::nullopt;
    return toolButtonFace(*button);
  }
  if (qobject_cast<const QAbstractItemView *>(&widget) &&
      inside<QComboBox>(widget))
    return DeviceBox{chrome("Menu"), chrome("MenuSeparator")};
  const QString name = widget.objectName();
  if (name == QLatin1String(PatchEditor))
    return DeviceBox{chrome("Window"), chrome("Border")};
  if (name == QLatin1String(LoadProgressCard))
    return DeviceBox{chrome("Menu"), chrome("Border"), 1, DeviceBox().edges,
                     config::kWorkbenchStyleCardRadius};
  // The palette is a window; its own fill is the window's.
  if (name == QLatin1String(CommandPalette))
    return DeviceBox{QColor(), chrome("MenuSeparator")};
  return std::nullopt;
}

/// A check box's, radio button's or list item's box and its mark, as the
/// theme drew them through the style sheet: the accent and a white mark while
/// on, the focus color's outline under the pointer, and only an outline while
/// it cannot act.  A list's boxes neither answer the pointer nor dim.
void paintIndicator(QStyle::PrimitiveElement element,
                    const QStyleOption &option, QPainter &painter) {
  namespace config = ::neverd::limits::gui_config;
  const bool radio = element == QStyle::PE_IndicatorRadioButton;
  const bool item = element == QStyle::PE_IndicatorItemViewItemCheck;
  const bool enabled = item || (option.state & QStyle::State_Enabled);
  const bool partial = !radio && (option.state & QStyle::State_NoChange);
  const bool marked = partial || (option.state & QStyle::State_On);
  const bool hover =
      !item && enabled && (option.state & QStyle::State_MouseOver);
  DeviceBox box;
  box.radius =
      radio ? option.rect.width() / 2.0 : config::kWorkbenchStyleCheckBoxRadius;
  if (!enabled) {
    box.fill = marked ? chrome("DisabledText") : QColor();
    box.border = chrome(marked ? "DisabledText" : "DisabledBorder");
  } else if (marked) {
    box.fill = chrome("CheckboxChecked");
    box.border = chrome("CheckboxCheckedBorder");
  } else {
    box.fill = chrome("CheckboxBackground");
    box.border = chrome(hover ? "Focus" : "CheckboxBorder");
  }
  paintDeviceBox(painter, option.rect, box);
  if (!marked)
    return;
  // The style sheet drew the mark from its image inside the box's border.
  const QIcon mark(QStringLiteral(":/neverd/icons/%1.svg")
                       .arg(QLatin1String(radio     ? "radio_dot"
                                          : partial ? "check_partial"
                                                    : "check")));
  DevicePixels device(painter);
  const QRect area = device.rect(option.rect);
  const int width = device.width(box.borderWidth);
  const QRect inner = area.adjusted(width, width, -width, -width);
  if (!device.snaps()) {
    mark.paint(&painter, inner, Qt::AlignCenter);
    return;
  }
  // On device pixels the mark is drawn at their size, one to one.
  painter.drawPixmap(inner.topLeft(), mark.pixmap(inner.size(), 1.0));
}

/// Where a tab's label goes: inside the line along its top and the
/// separator at its end, and inside its padding.
QRect tabContents(const QRect &tab) {
  namespace config = ::neverd::limits::gui_config;
  return tab.adjusted(config::kWorkbenchStyleTabPadding.left(),
                      1 + config::kWorkbenchStyleTabPadding.top(),
                      -1 - config::kWorkbenchStyleTabPadding.right(),
                      -config::kWorkbenchStyleTabPadding.bottom());
}

/// A tab's label color: the active text on the selected tab and under the
/// pointer, the inactive text on the others, and the disabled text on one
/// that cannot be chosen.
QColor tabText(const QStyleOptionTab &tab) {
  if (!(tab.state & QStyle::State_Enabled))
    return chrome("DisabledText");
  return chrome(tab.state & (QStyle::State_Selected | QStyle::State_MouseOver)
                    ? "TabActiveText"
                    : "TabInactiveText");
}

bool verticalTab(const QStyleOptionTab &tab) {
  return tab.shape == QTabBar::RoundedEast ||
         tab.shape == QTabBar::RoundedWest ||
         tab.shape == QTabBar::TriangularEast ||
         tab.shape == QTabBar::TriangularWest;
}
} // namespace

WorkbenchStyle::WorkbenchStyle()
    : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

int WorkbenchStyle::pixelMetric(PixelMetric metric, const QStyleOption *option,
                                const QWidget *widget) const {
  switch (metric) {
  case PM_ToolBarIconSize:
    return ::neverd::limits::gui_config::kWorkbenchStyleSmallIconSize;
  // A tab's padding is its room (CT_TabBarTab), and its label stays put when
  // it is selected.
  case PM_TabBarTabHSpace:
  case PM_TabBarTabVSpace:
  case PM_TabBarTabShiftHorizontal:
  case PM_TabBarTabShiftVertical:
    return 0;
  default:
    return QProxyStyle::pixelMetric(metric, option, widget);
  }
}

QRect WorkbenchStyle::subElementRect(SubElement element,
                                     const QStyleOption *option,
                                     const QWidget *widget) const {
  // A tab's label fits inside its padding, measured on the tab where it rests
  // in the bar, as Qt elides it.
  if (const auto *tab = qstyleoption_cast<const QStyleOptionTab *>(option);
      tab && element == SE_TabBarTabText) {
    const auto *bar = qobject_cast<const QTabBar *>(widget);
    QStyleOptionTab label(*tab);
    label.rect = tabContents(
        bar && tab->tabIndex != -1 ? bar->tabRect(tab->tabIndex) : tab->rect);
    return QProxyStyle::subElementRect(element, &label, widget);
  }
  return QProxyStyle::subElementRect(element, option, widget);
}

void WorkbenchStyle::drawPrimitive(PrimitiveElement element,
                                   const QStyleOption *option,
                                   QPainter *painter,
                                   const QWidget *widget) const {
  namespace config = ::neverd::limits::gui_config;
  // The style paints these boxes on device pixels, in the theme's colors,
  // where the style sheet's rules keep only their size: its native border
  // hands them to the style.
  if (option) {
    switch (element) {
    case PE_IndicatorCheckBox:
    case PE_IndicatorRadioButton:
    case PE_IndicatorItemViewItemCheck:
      paintIndicator(element, *option, *painter);
      return;
    case PE_FrameGroupBox:
      paintDeviceBox(*painter, option->rect,
                     DeviceBox{QColor(), chrome("Border"), 1, DeviceBox().edges,
                               config::kWorkbenchStyleGroupBoxRadius});
      return;
    case PE_PanelMenu:
    case PE_FrameMenu:
      // A menu fills first and frames last, over its items.
      if (qobject_cast<const QMenu *>(widget)) {
        paintDeviceBox(*painter, option->rect,
                       element == PE_PanelMenu
                           ? DeviceBox{chrome("Menu"), QColor()}
                           : DeviceBox{QColor(), chrome("MenuSeparator")});
        return;
      }
      break;
    case PE_PanelTipLabel:
      // The style sheet fills a tooltip.
      paintDeviceBox(*painter, option->rect,
                     DeviceBox{QColor(), chrome("MenuSeparator")});
      return;
    case PE_PanelStatusBar:
      paintDeviceBox(*painter, option->rect,
                     DeviceBox{chrome("StatusBar"), chrome("StatusBarBorder"),
                               1, Qt::TopEdge});
      return;
    case PE_IndicatorToolBarSeparator: {
      // A hairline across the bar, in the middle of the room the style sheet
      // gives it and short of the bar's edges.
      const QRect &room = option->rect;
      const int inset = config::kWorkbenchStyleToolBarSeparatorInset;
      const bool across = option->state & State_Horizontal;
      fillDeviceLine(
          *painter,
          across
              ? QRect(room.left() + (room.width() - 1) / 2, room.top() + inset,
                      1, room.height() - 2 * inset)
              : QRect(room.left() + inset, room.top() + (room.height() - 1) / 2,
                      room.width() - 2 * inset, 1),
          across ? Qt::Vertical : Qt::Horizontal, chrome("Separator"));
      return;
    }
    default:
      break;
    }
  }
  // Keyboard focus frames the control with a thin line of the focus color, as
  // Visual Studio Code draws its focus border, in place of Fusion's tinted box
  // behind the label.  The fields' focus border is the same color.  As in
  // Fusion, focus that the pointer gave draws none.
  if (element == PE_FrameFocusRect && option) {
    if (!(option->state & State_KeyboardFocusChange))
      return;
    DevicePixels device(*painter);
    painter->setRenderHint(QPainter::Antialiasing);
    const qreal width = device.width(config::kWorkbenchStyleFocusFrameWidth);
    QPen pen(Theme::instance().chrome(QStringLiteral("Focus")));
    pen.setWidthF(width);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    const qreal inset = width / 2;
    const QRectF frame = QRectF(device.rect(focusFrame(*option, widget)))
                             .adjusted(inset, inset, -inset, -inset);
    const qreal radius = device.length(config::kWorkbenchStyleFocusFrameRadius);
    painter->drawRoundedRect(frame, radius, radius);
    // A push button keeps a line of the surface it stands on between the
    // frame and its face, as Visual Studio Code sets its focus border off a
    // button, so the frame shows on a primary button's accent too.
    if (qobject_cast<const QPushButton *>(widget)) {
      const qreal gap = device.width(config::kWorkbenchStyleFocusFrameGap);
      pen.setColor(surfaceBehind(widget));
      painter->setPen(pen);
      painter->drawRoundedRect(frame.adjusted(gap, gap, -gap, -gap),
                               std::max<qreal>(0, radius - gap),
                               std::max<qreal>(0, radius - gap));
    }
    return;
  }
  QProxyStyle::drawPrimitive(element, option, painter, widget);
}

void WorkbenchStyle::drawControl(ControlElement element,
                                 const QStyleOption *option, QPainter *painter,
                                 const QWidget *widget) const {
  namespace config = ::neverd::limits::gui_config;
  // A tab as Visual Studio Code draws an editor tab: the selected one in the
  // editor's color with the accent along its top, the others a shade off the
  // bar, each ending in a line of the bar's color.  The style draws tabs whole,
  // on device pixels, with the geometry the style sheet gave them before.
  if (const auto *tab = qstyleoption_cast<const QStyleOptionTab *>(option)) {
    if (element == CE_TabBarTabShape) {
      const bool selected = tab->state & State_Selected;
      paintDeviceBox(*painter, tab->rect,
                     DeviceBox{chrome(selected ? "TabActive" : "TabInactive"),
                               chrome("TabSeparator"), 1, Qt::RightEdge});
      if (selected)
        paintDeviceBox(
            *painter, tab->rect,
            DeviceBox{QColor(), chrome("TabActiveBorder"), 1, Qt::TopEdge});
      return;
    }
    if (element == CE_TabBarTabLabel) {
      QStyleOptionTab label(*tab);
      label.rect = tabContents(tab->rect);
      label.palette.setColor(widget ? widget->foregroundRole()
                                    : QPalette::WindowText,
                             tabText(*tab));
      // Qt's own label code would frame focus with the base style's frame.
      label.state &= ~State_HasFocus;
      QProxyStyle::drawControl(element, &label, painter, widget);
      if (tab->state & State_HasFocus) {
        QStyleOptionFocusRect focus;
        focus.QStyleOption::operator=(label);
        focus.rect = label.rect;
        proxy()->drawPrimitive(PE_FrameFocusRect, &focus, painter, widget);
      }
      return;
    }
  }
  // A heading fills with the header and ends in a line along its end and its
  // foot, on device pixels.  The style sheet gives the fill.
  if (element == CE_HeaderSection && option &&
      qobject_cast<const QHeaderView *>(widget)) {
    paintDeviceBox(*painter, option->rect,
                   DeviceBox{option->palette.color(QPalette::Button),
                             chrome("Border"), 1,
                             Qt::RightEdge | Qt::BottomEdge});
    return;
  }
  // A splitter's handle is a hairline, the focus color under the pointer.  A
  // handle paints itself on behalf of its splitter.
  if (element == CE_Splitter && option &&
      (qobject_cast<const QSplitter *>(widget) ||
       qobject_cast<const QSplitterHandle *>(widget))) {
    fillDeviceLine(*painter, option->rect,
                   option->state & State_Horizontal ? Qt::Vertical
                                                    : Qt::Horizontal,
                   chrome(option->state & (State_MouseOver | State_Sunken)
                              ? "DockSeparatorHover"
                              : "DockSeparator"));
    return;
  }
  // The style paints a menu's items, so a shortcut can read quieter than its
  // command, as in Visual Studio Code; a style sheet paints both with one
  // pen.  The geometry is the style sheet's that came before: the padding,
  // the icon and check mark columns, and the chevron.
  const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option);
  if (element == CE_MenuItem && item && qobject_cast<const QMenu *>(widget) &&
      item->menuItemType == QStyleOptionMenuItem::Separator) {
    const int inset = config::kWorkbenchStyleMenuSeparatorInset;
    const int margin = config::kWorkbenchStyleMenuSeparatorMargin;
    fillDeviceLine(*painter,
                   item->rect.adjusted(inset, margin, -inset, -margin),
                   Qt::Horizontal, chrome("MenuSeparator"));
    return;
  }
  if (element != CE_MenuItem || !item || !qobject_cast<const QMenu *>(widget) ||
      (item->menuItemType != QStyleOptionMenuItem::Normal &&
       item->menuItemType != QStyleOptionMenuItem::DefaultItem &&
       item->menuItemType != QStyleOptionMenuItem::SubMenu)) {
    QProxyStyle::drawControl(element, option, painter, widget);
    return;
  }
  const Theme &theme = Theme::instance();
  const Qt::LayoutDirection direction = item->direction;
  const QRect rect = item->rect;
  const bool enabled = item->state & State_Enabled;
  const bool selected = item->state & State_Selected;
  painter->save();
  painter->setFont(item->font);
  if (selected) {
    DevicePixels device(*painter);
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    painter->setBrush(theme.chrome(QStringLiteral("MenuSelection")));
    const qreal radius = device.length(config::kWorkbenchStyleMenuItemRadius);
    painter->drawRoundedRect(QRectF(device.rect(rect)), radius, radius);
    // A high contrast theme outlines the item it does not fill.
    if (const QColor outline =
            theme.chrome(QStringLiteral("MenuSelectionBorder"));
        outline.alpha() > 0) {
      const qreal width = device.width(1);
      painter->setPen(QPen(outline, width));
      painter->setBrush(Qt::NoBrush);
      painter->drawRoundedRect(device.outline(rect, 1), radius - width / 2,
                               radius - width / 2);
    }
  }
  const bool checked =
      item->checkType != QStyleOptionMenuItem::NotCheckable && item->checked;
  int textOffset = item->maxIconWidth;
  if (!item->icon.isNull()) {
    // A toggle with an icon shows it boxed while on, as Visual Studio Code
    // shows an active input option.
    const int size = pixelMetric(PM_SmallIconSize, item, widget);
    const QRect box = leadingSquare(
        direction, rect, size + 2 * config::kWorkbenchStyleMenuIconBorder,
        config::kWorkbenchStyleMenuIconInset);
    if (checked) {
      DevicePixels device(*painter);
      painter->setRenderHint(QPainter::Antialiasing);
      const qreal width = device.width(config::kWorkbenchStyleMenuIconBorder);
      QPen pen(theme.chrome(QStringLiteral("OptionActiveBorder")));
      pen.setWidthF(width);
      painter->setPen(pen);
      painter->setBrush(theme.chrome(QStringLiteral("OptionActiveBackground")));
      const qreal inset = width / 2;
      const qreal radius =
          device.length(config::kWorkbenchStyleMenuItemRadius) - inset;
      painter->drawRoundedRect(
          QRectF(device.rect(box)).adjusted(inset, inset, -inset, -inset),
          radius, radius);
    }
    const QIcon::Mode mode = !enabled   ? QIcon::Disabled
                             : selected ? QIcon::Active
                                        : QIcon::Normal;
    const QPixmap pixmap = item->icon.pixmap(
        QSize(size, size), painter->device()->devicePixelRatio(), mode,
        checked ? QIcon::On : QIcon::Off);
    QRect place(QPoint(), pixmap.deviceIndependentSize().toSize());
    place.moveCenter(box.center());
    painter->drawPixmap(place.topLeft(), pixmap);
  } else if (item->menuHasCheckableItems) {
    const QRect mark =
        leadingSquare(direction, rect, config::kWorkbenchStyleMenuCheckSize,
                      config::kWorkbenchStyleMenuCheckInset);
    if (checked)
      chromeGlyph("menu_check").paint(painter, mark);
    textOffset = std::max(textOffset, mark.width());
  }
  const QRect contents = menuItemContents(rect);
  QRect text = contents;
  text.setLeft(text.left() + textOffset);
  text.setWidth(text.width() - item->reservedShortcutWidth);
  int flags = Qt::AlignLeft | Qt::AlignVCenter | Qt::TextShowMnemonic |
              Qt::TextDontClip | Qt::TextSingleLine;
  if (!styleHint(SH_UnderlineShortcut, item, widget))
    flags |= Qt::TextHideMnemonic;
  painter->setPen(theme.chrome(!enabled   ? QStringLiteral("DisabledText")
                               : selected ? QStringLiteral("MenuSelectionText")
                                          : QStringLiteral("MenuText")));
  QStringView label(item->text);
  // Quieter until the item is highlighted; a disabled item's shortcut is
  // already the disabled color.
  const qreal secondary =
      enabled && !selected ? config::kWorkbenchStyleMenuSecondaryOpacity : 1.0;
  if (const qsizetype tab = label.indexOf(u'\t'); tab >= 0) {
    const QRect shortcut(text.topRight(),
                         QPoint(contents.right(), text.bottom()));
    painter->setOpacity(secondary);
    painter->drawText(visualRect(direction, contents, shortcut), flags,
                      label.mid(tab + 1).toString());
    painter->setOpacity(1.0);
    label = label.left(tab);
  }
  painter->drawText(visualRect(direction, rect, text), flags, label.toString());
  if (item->menuItemType == QStyleOptionMenuItem::SubMenu) {
    const int size = config::kWorkbenchStyleMenuArrowSize;
    const QRect arrow = alignedRect(
        direction, Qt::AlignRight | Qt::AlignVCenter, QSize(size, size), rect);
    painter->setOpacity(secondary);
    chromeGlyph(direction == Qt::LeftToRight ? "chevron_right" : "chevron_left")
        .paint(painter, arrow, Qt::AlignCenter,
               enabled ? QIcon::Normal : QIcon::Disabled);
  }
  painter->restore();
}

QSize WorkbenchStyle::sizeFromContents(ContentsType type,
                                       const QStyleOption *option,
                                       const QSize &contents,
                                       const QWidget *widget) const {
  namespace config = ::neverd::limits::gui_config;
  // A tab holds its label and padding, the line along its top and the
  // separator at its end, and room for its icon as Qt's common style leaves.
  if (const auto *tab = qstyleoption_cast<const QStyleOptionTab *>(option);
      tab && type == CT_TabBarTab) {
    QSize size = contents;
    if (!tab->icon.isNull())
      (verticalTab(*tab) ? size.rheight() : size.rwidth()) +=
          config::kWorkbenchStyleTabIconRoom;
    const QMargins &padding = config::kWorkbenchStyleTabPadding;
    return size + QSize(padding.left() + padding.right() + 1,
                        padding.top() + padding.bottom() + 1);
  }
  const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option);
  if (type != CT_MenuItem || !item || !qobject_cast<const QMenu *>(widget))
    return QProxyStyle::sizeFromContents(type, option, contents, widget);
  if (item->menuItemType == QStyleOptionMenuItem::Separator)
    return QSize(contents.width(),
                 1 + 2 * config::kWorkbenchStyleMenuSeparatorMargin);
  // As the style sheet sized an item it painted: the shortcut's gap, the
  // icon or check mark column, and the padding.
  QSize size = contents;
  if (item->text.contains(u'\t'))
    size.rwidth() += config::kWorkbenchStyleMenuShortcutGap;
  if (!item->icon.isNull()) {
    const int small = pixelMetric(PM_SmallIconSize, nullptr, widget);
    size.rwidth() +=
        std::max(item->maxIconWidth,
                 item->icon.actualSize(QSize(small, small)).width()) +
        config::kWorkbenchStyleMenuColumnGap;
  } else if (item->menuHasCheckableItems) {
    size.rwidth() +=
        std::max(item->maxIconWidth, config::kWorkbenchStyleMenuCheckSize) +
        config::kWorkbenchStyleMenuColumnGap;
  } else {
    size.rwidth() += item->maxIconWidth;
  }
  return size + QSize(config::kWorkbenchStyleMenuItemPaddingLeft +
                          config::kWorkbenchStyleMenuItemPaddingRight,
                      2 * config::kWorkbenchStyleMenuItemPaddingVertical);
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
  // A button or a check box lights under the pointer only over the part that
  // takes a click, which Qt tracks only while the mouse is tracked; the style
  // sheet's hover rules, which these boxes no longer have, turned it on.
  if (qobject_cast<QPushButton *>(widget) ||
      qobject_cast<QCheckBox *>(widget) ||
      qobject_cast<QRadioButton *>(widget) ||
      qobject_cast<QGroupBox *>(widget)) {
    widget->setAttribute(Qt::WA_Hover);
    widget->setMouseTracking(true);
  }
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
  // otherwise centers it and cuts off both of its ends.  A column across a
  // table starts as wide as its heading, in every language.
  if (auto *header = qobject_cast<QHeaderView *>(widget)) {
    header->setTextElideMode(Qt::ElideRight);
    if (header->orientation() == Qt::Horizontal &&
        !header->property(FittedHeadings).toBool()) {
      header->setProperty(FittedHeadings, true);
      new HeadingFitter(header);
    }
  }
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
  case QEvent::LanguageChange:
    // Qt words a tab's close button only as it builds it.
    if (auto *tabs = qobject_cast<QTabBar *>(watched))
      for (int tab = 0; tab < tabs->count(); ++tab)
        for (const auto side : {QTabBar::LeftSide, QTabBar::RightSide})
          if (QWidget *button = tabs->tabButton(tab, side);
              button && button->inherits("CloseButton"))
            button->setToolTip(
                QCoreApplication::translate("CloseButton", "Close Tab"));
    break;
  case QEvent::Paint:
    // Before the widget paints what lies on its box.
    if (watched->isWidgetType()) {
      auto *widget = static_cast<QWidget *>(watched);
      if (const std::optional<DeviceBox> face = faceOf(*widget)) {
        QPainter painter(widget);
        paintDeviceBox(painter, widget->rect(), *face);
      }
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
