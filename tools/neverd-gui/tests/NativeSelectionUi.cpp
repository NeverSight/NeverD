//===- NativeSelectionUi.cpp - Independent native SDK example ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "GuiPluginUi.h"

#include <QApplication>
#include <QLabel>

using namespace neverd::gui::pluginui;

class SelectionModule final : public QObject, public Module {
  Q_OBJECT
  Q_PLUGIN_METADATA(IID NEVERD_GUI_PLUGIN_UI_IID)
  Q_INTERFACES(neverd::gui::pluginui::Module)

public:
  bool activate(Context &context, QObject &lifetime, QString &error) override {
    auto *view = context.mountView(
        "selection", {},
        [](QWidget *parent) {
          auto *label = new QLabel(parent);
          label->setObjectName("NativeSelectionInspector");
          label->setTextFormat(Qt::PlainText);
          return label;
        },
        error);
    if (!view)
      return false;
    // What a module keeps below the activation's lifetime may watch its
    // view.  This one records a view that the host destroyed before it.
    auto *keeper = new QObject(&lifetime);
    connect(view, &QObject::destroyed, keeper,
            [] { qApp->setProperty("nativeSelectionViewWentFirst", true); });
    auto *label = static_cast<QLabel *>(view);
    const auto update = [label](const QJsonObject &selection) {
      label->setText(selection.value("address").toString(
          QStringLiteral("No address selected")));
    };
    context.watchContext(label, update);
    update(context.captureContext());
    label->show();
    return true;
  }
};

#include "NativeSelectionUi.moc"
