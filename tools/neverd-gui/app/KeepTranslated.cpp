//===- KeepTranslated.cpp - Texts that follow the language ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Set a view's texts again on each language change it receives.
//===----------------------------------------------------------------------===//

#include "KeepTranslated.h"

#include <QEvent>
#include <QObject>
#include <QWidget>
#include <utility>

namespace neverd::gui {
namespace {

/// Sets a view's texts again on each language change the view receives.
class Retranslator final : public QObject {
public:
  Retranslator(QWidget &owner, std::function<void()> apply)
      : QObject(&owner), apply_(std::move(apply)) {
    owner.installEventFilter(this);
  }

protected:
  bool eventFilter(QObject *, QEvent *event) override {
    if (event->type() == QEvent::LanguageChange)
      apply_();
    return false;
  }

private:
  std::function<void()> apply_;
};

} // namespace

void keepTranslated(QWidget &owner, std::function<void()> apply) {
  apply();
  new Retranslator(owner, std::move(apply));
}

} // namespace neverd::gui
