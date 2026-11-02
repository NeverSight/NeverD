// Interface languages: bundled catalogs, the first-launch default and live
// switching of the production window.
#include "Docking.h"
#include "Language.h"
#include "MainWindow.h"
#include "Session.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"

#include <QApplication>
#include <QMenuBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>
#include <kddockwidgets/qtwidgets/views/DockWidget.h>

using namespace neverd::gui;

namespace {
QString resourceLocale(QString code) { return code.replace('-', '_'); }
constexpr char WindowContext[] = "neverd::gui::MainWindow";
} // namespace

class LanguageSwitchTests final : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    QVERIFY(settingsDirectory_.isValid());
    // These tests never touch the real application's preferences.
    QCoreApplication::setOrganizationName(QStringLiteral("NeverDTests"));
    QCoreApplication::setApplicationName(QStringLiteral("LanguageSwitchTests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDirectory_.path());
    configureDocking();
  }

  void bundledCatalogsCoverEveryLocale() {
    QCOMPARE(languages().size(), 11);
    QCOMPARE(languages().front().code, QStringLiteral("en"));
    for (const auto &language : languages()) {
      QTranslator translator;
      QVERIFY2(translator.load(QStringLiteral(":/i18n/neverd_") +
                               resourceLocale(language.code) +
                               QStringLiteral(".qm")),
               qPrintable(language.code));
      if (language.code == QLatin1String("en"))
        continue;
      for (const auto &[context, source] :
           {std::pair{"Menus", "&File"}, std::pair{"Actions", "&Open..."},
            std::pair{WindowContext, "Functions"},
            std::pair{"McpConnectionManager", "Disconnected"},
            std::pair{"Actions", "Version comparison &graph…"},
            std::pair{"neverd::gui::LineageDialog", "Version comparison graph"},
            std::pair{"neverd::gui::LineagePageView", "Rows per page"},
            std::pair{"neverd::gui::LineageSession",
                      "Version graph loaded."}}) {
        const auto text = translator.translate(context, source);
        QVERIFY2(!text.isEmpty(),
                 qPrintable(language.code + QStringLiteral(": ") +
                            QLatin1String(source)));
      }
    }
  }

  void firstLaunchDefaultsToEnglish() {
    QSettings().remove(QStringLiteral("ui/language"));
    QCOMPARE(currentLanguage(), QStringLiteral("en"));
  }

  void liveSwitchRetranslatesTheWindowAndPersists() {
    Session session(QStringLiteral("/unused/neverd-worker"));
    McpConnectionManager mcp;
    GuiSessionBroker broker;
    QTemporaryDir layout;
    MainWindow window(session, mcp, broker);
    window.setLayoutPath(layout.filePath(QStringLiteral("layout.json")));
    window.initializeLayout();
    const auto fileMenu = [&] {
      return window.menuBar()->actions().value(0)->text();
    };
    const auto functionsTitle = [&] {
      for (auto *dock :
           window.findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
        if (dock->uniqueName() == QLatin1String("functions"))
          return dock->title();
      return QString();
    };
    for (const auto &language : languages()) {
      QTranslator catalog;
      QVERIFY(catalog.load(QStringLiteral(":/i18n/neverd_") +
                           resourceLocale(language.code) +
                           QStringLiteral(".qm")));
      const auto translated = [&](const char *context, const char *source) {
        const auto text = catalog.translate(context, source);
        return text.isEmpty() ? QString::fromUtf8(source) : text;
      };
      applyLanguage(language.code);
      mcp.retranslate();
      QTRY_COMPARE(fileMenu(), translated("Menus", "&File"));
      QCOMPARE(functionsTitle(), translated(WindowContext, "Functions"));
      QCOMPARE(mcp.status(),
               translated("McpConnectionManager", "Disconnected"));
      QCOMPARE(QSettings().value(QStringLiteral("ui/language")).toString(),
               language.code);
      QCOMPARE(currentLanguage(), language.code);
      QCOMPARE(QApplication::layoutDirection(),
               language.code == QLatin1String("ar") ? Qt::RightToLeft
                                                    : Qt::LeftToRight);
    }
    applyLanguage(QStringLiteral("unsupported-locale"));
    QCOMPARE(currentLanguage(), QStringLiteral("ar"));
    applyLanguage(QStringLiteral("en"));
    QTRY_COMPARE(fileMenu(), QStringLiteral("&File"));
    QCOMPARE(functionsTitle(), QStringLiteral("Functions"));
  }

private:
  QTemporaryDir settingsDirectory_;
};

QTEST_MAIN(LanguageSwitchTests)
#include "LanguageSwitchTests.moc"
