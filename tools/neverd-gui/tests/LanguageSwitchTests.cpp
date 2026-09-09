#include "Workbench.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>
#include <memory>

class LanguageSwitchTests final : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    QVERIFY(settingsDirectory_.isValid());
    // These tests never modify the real application's language preference.
    QCoreApplication::setOrganizationName("NeverDTests");
    QCoreApplication::setApplicationName("LanguageSwitchTests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDirectory_.path());
    hadLanguage_ = QSettings().contains("ui/language");
    previousLanguage_ = QSettings().value("ui/language");
  }

  void bundledCatalogsCoverEveryLocale() {
    Workbench workbench("/unused/neverd-worker");
    QCOMPARE(workbench.languages().size(), 11);
    for (auto locale : workbench.languages()) {
      locale.replace('-', '_');
      QTranslator translator;
      QVERIFY2(translator.load(":/i18n/neverd_" + locale + ".qm"),
               qPrintable(locale));
      QVERIFY2(!translator.translate("Main", "Open Binary").isEmpty(),
               qPrintable(locale));
      QVERIFY2(!translator.translate("McpConnectionManager", "Disconnected")
                    .isEmpty(),
               qPrintable(locale));
      QVERIFY2(!translator.translate("DockTitleBar", "Close Panel").isEmpty(),
               qPrintable(locale));
    }
  }

  void firstLaunchDefaultsToEnglish() {
    QSettings().remove("ui/language");
    Workbench workbench("/unused/neverd-worker");
    QCOMPARE(workbench.language(), "en");
  }

  void liveSwitchRetranslatesBindingsAndPersists() {
    QSettings().setValue("ui/language", "en");
    QQmlEngine engine;
    Workbench workbench("/unused/neverd-worker");
    workbench.setQmlEngine(&engine);
    McpConnectionManager manager;
    GuiSessionBroker broker;
    connect(&workbench, &Workbench::languageChanged, &manager,
            &McpConnectionManager::retranslate);
    connect(&workbench, &Workbench::languageChanged, &broker,
            &GuiSessionBroker::retranslate);
    QQmlComponent component(&engine);
    component.setData(R"(
            import QtQml
            QtObject {
                property string label: qsTranslate("Main", "Open Binary")
                property string missing: qsTranslate("NeverDTestMissingContext", "English fallback")
                property string address: "0xffffffffffffffff"
            }
        )",
                      QUrl());
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object != nullptr, qPrintable(component.errorString()));
    const auto originalSelection = workbench.selection();
    for (const auto &locale : workbench.languages()) {
      QString resourceLocale = locale;
      resourceLocale.replace('-', '_');
      QTranslator catalog;
      QVERIFY(catalog.load(":/i18n/neverd_" + resourceLocale + ".qm"));
      const auto expected = catalog.translate("Main", "Open Binary");
      QVERIFY(!expected.isEmpty());
      workbench.setLanguage(locale);
      QTRY_COMPARE(object->property("label").toString(), expected);
      QCOMPARE(QCoreApplication::translate("Main", "Open Binary"), expected);
      QCOMPARE(manager.status(),
               catalog.translate("McpConnectionManager", "Disconnected"));
      QCOMPARE(broker.status(), catalog.translate("GuiSessionBroker",
                                                  "Session sharing disabled"));
      QCOMPARE(object->property("missing").toString(), "English fallback");
      QCOMPARE(object->property("address").toString(), "0xffffffffffffffff");
      QCOMPARE(workbench.selection(), originalSelection);
      QCOMPARE(QSettings().value("ui/language").toString(), locale);
      Workbench restored("/unused/neverd-worker");
      QCOMPARE(restored.language(), locale);
    }
    workbench.setLanguage("unsupported-locale");
    QCOMPARE(workbench.language(), "ar");
    workbench.setLanguage("en");
    QTRY_COMPARE(object->property("label").toString(), "Open Binary");
  }

  void cleanupTestCase() {
    if (hadLanguage_)
      QSettings().setValue("ui/language", previousLanguage_);
    else
      QSettings().remove("ui/language");
  }

private:
  QTemporaryDir settingsDirectory_;
  QVariant previousLanguage_;
  bool hadLanguage_ = false;
};

QTEST_MAIN(LanguageSwitchTests)
#include "LanguageSwitchTests.moc"
