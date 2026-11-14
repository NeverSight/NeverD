//===- GuiPluginUiHost.cpp - Native plugin view ownership -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "GuiPluginUiHost.h"

#include "GuiPluginUi.h"
#include "PluginServices.h"
#include "Session.h"
#include "Theme.h"
#include "mcp/GuiSessionBroker.h"

#include <QAction>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QPluginLoader>
#include <QPointer>
#include <QStyle>
#include <QTimer>
#include <QVariant>
#include <QWidget>
#include <algorithm>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace neverd::gui {
namespace {
bool sameProject(const QJsonObject &a, const QJsonObject &b) {
  return a.value("loaded").toBool() && b.value("loaded").toBool() &&
         a.value("epoch") == b.value("epoch") &&
         a.value("project_id") == b.value("project_id");
}

QString pathText(const std::filesystem::path &path) {
  const auto text = path.u8string();
  return QString::fromUtf8(reinterpret_cast<const char *>(text.data()),
                           qsizetype(text.size()));
}

std::optional<QPoint> requestedPosition(const QWidget &widget) {
  const auto position = widget.property(pluginui::ViewPositionProperty);
  if (position.metaType().id() == QMetaType::QPoint)
    return position.toPoint();
  return std::nullopt;
}

QRect adjacentSpace(QRect area, const QRect &anchor, Qt::Edge edge, int gap) {
  switch (edge) {
  case Qt::TopEdge:
    area.setBottom(std::min(area.bottom(), anchor.top() - gap - 1));
    break;
  case Qt::BottomEdge:
    area.setTop(std::max(area.top(), anchor.bottom() + gap + 1));
    break;
  case Qt::LeftEdge:
    area.setRight(std::min(area.right(), anchor.left() - gap - 1));
    break;
  case Qt::RightEdge:
    area.setLeft(std::max(area.left(), anchor.right() + gap + 1));
    break;
  }
  return area;
}
} // namespace

class GuiPluginUiHost::Binding final : public QObject,
                                       public pluginui::Context,
                                       public pluginui::MenuContext,
                                       public pluginui::ThemeContext {
public:
  struct View {
    QString Id;
    pluginui::Placement Place;
    QPointer<QWidget> Widget;
    plugin::Owner Scope;
  };

  Binding(GuiPluginUiHost &host, plugin::Owner scope, QString pluginId)
      : QObject(&host), Host(host), Scope(scope), PluginId(std::move(pluginId)),
        Epoch(host.Jobs.context().value("epoch")) {
    Host.Bindings.append(this);
  }

  ~Binding() override {
    Host.Bindings.removeOne(this);
    const auto views = std::exchange(Views, {});
    for (const auto &view : views)
      delete view.Widget.data();
  }

  QWidget *mountView(const QString &id, const pluginui::Placement &place,
                     Factory factory, QString &error) override {
    if (Retiring || !plugin::validIdentifier(id.toStdString()) || find(id) ||
        (!place.Anchor.isEmpty() && !find(place.Anchor))) {
      error = tr("Invalid, duplicate or unavailable plugin view: %1").arg(id);
      return nullptr;
    }
    auto &host = Host.Jobs.host();
    plugin::Diagnostic issue;
    const auto scope = host.createOwner(Scope, plugin::OwnerKind::View,
                                        id.toStdString(), issue);
    if (!scope) {
      error = QString::fromStdString(issue.Message);
      return nullptr;
    }
    QWidget *widget = factory(&Host.Workbench);
    if (!widget || widget->parentWidget() != &Host.Workbench ||
        widget->isWindow()) {
      host.close(*scope, [&host, scope](const auto &) { host.forget(*scope); });
      error = tr("The view factory must create a workbench child widget.");
      return nullptr;
    }
    const auto guard = std::make_shared<QPointer<QWidget>>(widget);
    if (!host.registerResource(
            *scope, {"gui_view", id.toStdString()},
            [guard] {
              if (*guard)
                (*guard)->hide();
            },
            [guard](auto done) {
              delete guard->data();
              done({});
            },
            issue)) {
      delete widget;
      host.close(*scope, [&host, scope](const auto &) { host.forget(*scope); });
      error = QString::fromStdString(issue.Message);
      return nullptr;
    }
    Views.append({id, place, widget, *scope});
    widget->setAttribute(Qt::WA_DeleteOnClose);
    widget->installEventFilter(&Host);
    connect(widget, &QObject::destroyed, this, [this, scope, guard] {
      // QWidget emits destroyed before its QObject base clears QPointer.
      // Retiring the view here must not touch the widget's destructor again.
      guard->clear();
      auto &host = Host.Jobs.host();
      Views.removeIf(
          [scope](const auto &view) { return view.Scope == *scope; });
      host.close(*scope, [&host, scope](const auto &) { host.forget(*scope); });
      Host.scheduleLayout();
    });
    if (Active && !host.activate(*scope, issue)) {
      widget->close();
      error = QString::fromStdString(issue.Message);
      return nullptr;
    }
    Host.scheduleLayout();
    return widget;
  }

  bool mountMenuAction(const QString &id, ActionFactory factory,
                       QString &error) override {
    if (Active || Retiring || !plugin::validIdentifier(id.toStdString()) ||
        MenuActions.contains(id)) {
      error = tr("Invalid, duplicate or unavailable plugin menu action: %1")
                  .arg(id);
      return false;
    }
    auto *action = factory(this);
    if (!action || action->parent() != this) {
      error = tr("The menu action factory must use the supplied parent.");
      return false;
    }
    MenuActions.insert(id, action);
    return true;
  }

  const View *find(const QString &id) const {
    for (const auto &view : Views)
      if (view.Id == id && view.Widget)
        return &view;
    return nullptr;
  }

  void start() {
    Active = true;
    for (const auto &view : Views) {
      plugin::Diagnostic error;
      if (!Host.Jobs.host().activate(view.Scope, error) && view.Widget)
        view.Widget->close();
    }
    Host.scheduleLayout();
  }

  void retire() {
    Retiring = true;
    Active = false;
    for (auto *action : MenuActions)
      action->setEnabled(false);
    if (Broker)
      Broker->stop();
  }

  QJsonObject captureContext() const override {
    const auto context = Host.Project.pluginServices().currentContext();
    return !Retiring && context.value("epoch") == Epoch ? context
                                                        : QJsonObject{};
  }

  void watchContext(QObject *receiver, Observer observer) override {
    const auto connection =
        connect(&Host.Jobs, &PluginJobs::contextChanged, this,
                [this, observer = std::move(observer)](const auto &context) {
                  if (Active && context.value("epoch") == Epoch)
                    observer(context);
                });
    connect(receiver, &QObject::destroyed, this,
            [connection] { QObject::disconnect(connection); });
  }

  QString attachSession(const QJsonObject &binding, QString &error) override {
    const auto current = captureContext();
    if (!Active || !sameProject(current, binding)) {
      error = tr("The analysis session has changed.");
      return {};
    }
    if (!Broker) {
      Broker = new GuiSessionBroker(this);
      connect(Broker, &GuiSessionBroker::queryRequested, &Host.Project,
              &Session::externalQuery);
      connect(&Host.Project, &Session::externalResponse, Broker,
              &GuiSessionBroker::reply);
      connect(&Host.Jobs, &PluginJobs::contextChanged, Broker,
              &GuiSessionBroker::setSelection);
    }
    Broker->setSelection(current);
    if (!Broker->start()) {
      error = Broker->status();
      return {};
    }
    return Broker->credentialFile();
  }

  QColor themeColor(const QString &role) const override {
    return Theme::instance().chrome(role);
  }

  bool navigate(const QString &address, const QJsonObject &binding) override {
    if (!Active || !sameProject(captureContext(), binding) ||
        !parseAddress(address))
      return false;
    Host.Jump(address);
    return true;
  }

  GuiPluginUiHost &Host;
  plugin::Owner Scope;
  QString PluginId;
  QJsonValue Epoch;
  QList<View> Views;
  QMap<QString, QAction *> MenuActions;
  GuiSessionBroker *Broker = nullptr;
  bool Active = false;
  bool Retiring = false;
};

GuiPluginUiHost::GuiPluginUiHost(QWidget &workbench, Session &session,
                                 std::function<QRect()> available,
                                 Navigate navigate)
    : QObject(&workbench), Workbench(workbench), Project(session),
      Jobs(session.pluginServices().jobs()), Available(std::move(available)),
      Jump(std::move(navigate)) {
  Workbench.installEventFilter(this);
  connect(&Jobs, &PluginJobs::componentBound, this, [this](const QString &id) {
    const auto owner = Jobs.componentOwner(id);
    for (auto *binding : Bindings)
      if (owner && binding->Scope == *owner)
        binding->start();
  });
  Jobs.setGuiActivator([this](auto scope, const auto &manifest, auto &error) {
    const auto fail = [&](const QString &why) {
      error = {"gui_activation_failed", why.toStdString()};
      return false;
    };
    const QFileInfo file(pathText(manifest.GuiLibrary));
    const QDir package(QFileInfo(pathText(manifest.Path)).canonicalPath());
    const auto path = file.canonicalFilePath();
    const auto relative = package.relativeFilePath(path);
    if (!file.isFile() || path.isEmpty() || relative.startsWith("../") ||
        QDir::isAbsolutePath(relative))
      return fail(tr("The GUI module is missing or outside the package: %1")
                      .arg(file.filePath()));
    QPluginLoader loader(path);
    if (loader.metaData().value("IID") != NEVERD_GUI_PLUGIN_UI_IID)
      return fail(
          tr("The module does not provide the required GUI interface."));
    auto *module = qobject_cast<pluginui::Module *>(loader.instance());
    if (!module)
      return fail(loader.errorString());
    auto *binding =
        new Binding(*this, scope, QString::fromStdString(manifest.Id));
    const QPointer<Binding> guard(binding);
    if (!Jobs.host().registerResource(
            scope, {"gui_binding", manifest.Id},
            [guard] {
              if (guard)
                guard->retire();
            },
            [guard](auto done) {
              delete guard.data();
              done({});
            },
            error)) {
      delete binding;
      return false;
    }
    QString issue;
    try {
      if (!module->activate(*binding, *binding, issue))
        return fail(issue.isEmpty() ? tr("The GUI module failed to activate.")
                                    : issue);
    } catch (const std::exception &exception) {
      return fail(QString::fromUtf8(exception.what()));
    } catch (...) {
      return fail(tr("The GUI module failed during activation."));
    }
    return true;
  });
}

GuiPluginUiHost::~GuiPluginUiHost() {
  Jobs.setGuiActivator({});
  Workbench.removeEventFilter(this);
  const auto bindings = Bindings;
  for (auto *binding : bindings)
    Jobs.host().close(binding->Scope);
  Jobs.drainClosures();
  while (!Bindings.isEmpty())
    delete Bindings.back();
}

bool GuiPluginUiHost::contains(const QWidget *widget) const {
  if (!widget)
    return false;
  for (const auto *binding : Bindings)
    for (const auto &view : binding->Views)
      if (view.Widget &&
          (view.Widget == widget || view.Widget->isAncestorOf(widget)))
        return true;
  return false;
}

QList<QAction *> GuiPluginUiHost::menuActions(const QString &pluginId) const {
  for (const auto *binding : Bindings)
    if (binding->Active && binding->PluginId == pluginId)
      return binding->MenuActions.values();
  return {};
}

bool GuiPluginUiHost::eventFilter(QObject *, QEvent *event) {
  switch (event->type()) {
  case QEvent::Resize:
  case QEvent::Show:
  case QEvent::Hide:
  case QEvent::LayoutRequest:
  case QEvent::StyleChange:
    scheduleLayout();
    break;
  case QEvent::DynamicPropertyChange:
    if (static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName() ==
        pluginui::ViewPositionProperty)
      scheduleLayout();
    break;
  default:
    break;
  }
  return false;
}

void GuiPluginUiHost::scheduleLayout() {
  if (std::exchange(LayoutQueued, true))
    return;
  QTimer::singleShot(0, this, [this] {
    LayoutQueued = false;
    layoutViews();
  });
}

void GuiPluginUiHost::layoutViews() {
  const int gap =
      std::max(0, Workbench.style()->pixelMetric(QStyle::PM_LayoutRightMargin));
  const auto area = Available().adjusted(gap, gap, -gap, -gap);
  if (area.isEmpty())
    return;
  QHash<Qt::Corner, int> offsets;
  for (const auto *binding : Bindings) {
    if (!binding->Active)
      continue;
    for (const auto &view : binding->Views) {
      auto *widget = view.Widget.data();
      if (!widget || widget->isHidden())
        continue;
      const bool right = view.Place.Corner == Qt::BottomRightCorner ||
                         view.Place.Corner == Qt::TopRightCorner;
      const bool bottom = view.Place.Corner == Qt::BottomLeftCorner ||
                          view.Place.Corner == Qt::BottomRightCorner;
      const auto hint = widget->sizeHint();
      const auto desired = (hint.isValid() ? hint : widget->size())
                               .expandedTo(widget->minimumSizeHint())
                               .expandedTo(QSize(1, 1));
      const auto *anchor = binding->find(view.Place.Anchor);
      const auto requested = view.Place.Anchor.isEmpty()
                                 ? requestedPosition(*widget)
                                 : std::nullopt;
      const bool movingAnchor = anchor && anchor->Place.Anchor.isEmpty() &&
                                requestedPosition(*anchor->Widget).has_value();
      QRect bounds = area;
      QRect anchorRect;
      Qt::Edge side = bottom ? Qt::TopEdge : Qt::BottomEdge;
      if (anchor) {
        anchorRect = anchor->Widget->geometry();
        bounds = adjacentSpace(area, anchorRect, side, gap);
        if (movingAnchor) {
          const auto visibleArea = [desired](const QRect &space) {
            if (space.isEmpty())
              return qint64(0);
            const auto size = desired.boundedTo(space.size());
            return qint64(size.width()) * size.height();
          };
          auto best = visibleArea(bounds);
          for (const auto edge : {bottom ? Qt::BottomEdge : Qt::TopEdge,
                                  right ? Qt::LeftEdge : Qt::RightEdge,
                                  right ? Qt::RightEdge : Qt::LeftEdge}) {
            const auto candidate = adjacentSpace(area, anchorRect, edge, gap);
            const auto score = visibleArea(candidate);
            if (score > best) {
              best = score;
              bounds = candidate;
              side = edge;
            }
          }
        }
      }
      if (bounds.isEmpty()) {
        widget->resize(0, 0);
        continue;
      }
      const auto size = desired.boundedTo(bounds.size());
      const int offset = offsets.value(view.Place.Corner);
      const auto alignment = anchor ? anchorRect : bounds;
      int x = right ? alignment.right() - size.width() + 1 : alignment.left();
      int y = side == Qt::TopEdge ? bounds.bottom() - size.height() + 1
                                  : bounds.top();
      if (requested) {
        x = requested->x();
        y = requested->y();
      } else if (!anchor) {
        y += bottom ? -offset : offset;
      } else if (side == Qt::LeftEdge || side == Qt::RightEdge) {
        x = side == Qt::LeftEdge ? bounds.right() - size.width() + 1
                                 : bounds.left();
        y = anchorRect.center().y() - size.height() / 2;
      }
      const QRect geometry(
          QPoint(
              std::clamp(x, bounds.left(), bounds.right() - size.width() + 1),
              std::clamp(y, bounds.top(), bounds.bottom() - size.height() + 1)),
          size);
      if (!requested && !movingAnchor) {
        const int occupied = bottom ? area.bottom() - geometry.top() + 1
                                    : geometry.bottom() - area.top() + 1;
        offsets[view.Place.Corner] = std::max(offset, occupied + gap);
      }
      if (widget->geometry() != geometry)
        widget->setGeometry(geometry);
      widget->raise();
    }
  }
}
} // namespace neverd::gui
