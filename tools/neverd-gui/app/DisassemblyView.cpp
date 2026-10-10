#include "DisassemblyView.h"

#include "AddressSpace.h"
#include "GraphView.h"
#include "ListingView.h"
#include "Session.h"
#include "ShrinkableRow.h"
#include "Theme.h"

#include <QLabel>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace neverd::gui {
namespace {
constexpr std::size_t MaxHistory = 512;
constexpr int FileOffsetDigits = 8;
} // namespace

DisassemblyView::DisassemblyView(Session &session, const AddressSpace &space,
                                 QWidget *parent)
    : QWidget(parent), session_(session), space_(space),
      stack_(new QStackedWidget(this)),
      listing_(new ListingView(session, space, this)),
      graph_(new GraphView(session, this)), status_(new QLabel(this)) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  stack_->addWidget(listing_);
  stack_->addWidget(graph_);
  layout->addWidget(stack_, 1);
  status_->setContentsMargins(6, 2, 6, 2);
  status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  makeRowShrinkable(*status_);
  layout->addWidget(status_);
  const auto applyFont = [this] {
    QFont font = Theme::instance().codeFont();
    status_->setFont(font);
  };
  applyFont();
  connect(&Theme::instance(), &Theme::changed, this, applyFont);
  connect(listing_, &ListingView::locationChanged, this, [this](Address a) {
    updateStatus();
    emit locationChanged(a);
  });
  connect(graph_, &GraphView::locationChanged, this, [this](Address a) {
    updateStatus();
    emit locationChanged(a);
  });
  connect(listing_, &ListingView::navigateRequested, this,
          &DisassemblyView::navigateRequested);
  connect(graph_, &GraphView::navigateRequested, this,
          &DisassemblyView::navigateRequested);
  connect(listing_, &ListingView::contextMenuRequested, this,
          &DisassemblyView::contextMenuRequested);
  connect(graph_, &GraphView::contextMenuRequested, this,
          &DisassemblyView::contextMenuRequested);
  connect(graph_, &GraphView::statusChanged, this, [this](const QString &text) {
    if (graphMode() && !text.isEmpty())
      status_->setText(text);
    else
      updateStatus();
  });
  connect(&session_, &Session::unloaded, this, [this] {
    disconnect(graphNavigation_);
    history_.clear();
    historyIndex_ = 0;
    status_->clear();
    emit historyChanged();
  });
}

bool DisassemblyView::graphMode() const {
  return stack_->currentWidget() == graph_;
}

void DisassemblyView::setGraphMode(bool graph) {
  if (graph == graphMode())
    return;
  disconnect(graphNavigation_);
  if (graph) {
    // A jump still loading decides the function: switch once it lands.
    if (listing_->jumpPending()) {
      if (!graphAfterJump_) {
        graphAfterJump_ = true;
        connect(
            listing_, &ListingView::jumpSettled, this,
            [this] {
              graphAfterJump_ = false;
              setGraphMode(true);
            },
            Qt::SingleShotConnection);
      }
      return;
    }
    const auto function = listing_->currentFunction();
    const auto address = listing_->currentAddress();
    if (!function) {
      emit session_.message(
          tr("Graph view requires a location inside a function."), 1);
      return;
    }
    stack_->setCurrentWidget(graph_);
    if (graph_->function() == function && graph_->loaded())
      graph_->setCursorAddress(address.value_or(*function));
    else
      graph_->showFunction(*function, address);
  } else {
    const auto address = graph_->currentAddress();
    stack_->setCurrentWidget(listing_);
    if (address)
      listing_->jumpTo(*address);
  }
  focusContent();
  updateStatus();
  emit graphModeChanged(graph);
}

void DisassemblyView::focusContent() {
  stack_->currentWidget()->setFocus(Qt::OtherFocusReason);
}

void DisassemblyView::remember(Address address) {
  if (restoring_)
    return;
  if (historyIndex_ < history_.size())
    history_.resize(historyIndex_ + 1);
  if (history_.empty() || history_.back() != address)
    history_.push_back(address);
  if (history_.size() > MaxHistory)
    history_.erase(history_.begin());
  historyIndex_ = history_.size() - 1;
  emit historyChanged();
}

void DisassemblyView::navigate(Address address, bool record,
                               std::optional<Address> from) {
  disconnect(graphNavigation_);
  if (record) {
    // Record where we were, then where we go, so Esc returns here.
    if (const auto current = from ? from : currentAddress()) {
      if (history_.empty() || history_[historyIndex_] != *current)
        remember(*current);
    }
    remember(address);
  }
  if (graphMode()) {
    if (!graph_->setCursorAddress(address)) {
      // Another function: follow it in the graph if it is code.
      graphNavigation_ = connect(
          listing_, &ListingView::locationChanged, this,
          [this, address](Address) {
            if (const auto function = listing_->currentFunction())
              graph_->showFunction(*function, address);
            else
              setGraphMode(false);
          },
          Qt::SingleShotConnection);
      listing_->jumpTo(address);
    } else {
      // Supersede any older function lookup, including the hidden listing's
      // selection, when the new address is already present in this graph.
      if (listing_->jumpPending())
        listing_->jumpTo(address);
      emit locationChanged(address);
    }
  } else {
    listing_->jumpTo(address);
  }
  updateStatus();
}

void DisassemblyView::goBack() {
  if (!canGoBack())
    return;
  // The newest entry is the current position after an unrecorded scroll.
  if (const auto current = currentAddress();
      current && historyIndex_ + 1 == history_.size() &&
      history_.back() != *current) {
    remember(*current);
  }
  --historyIndex_;
  restoring_ = true;
  navigate(history_[historyIndex_], false);
  restoring_ = false;
  emit historyChanged();
}

void DisassemblyView::goForward() {
  if (!canGoForward())
    return;
  ++historyIndex_;
  restoring_ = true;
  navigate(history_[historyIndex_], false);
  restoring_ = false;
  emit historyChanged();
}

std::optional<Address> DisassemblyView::currentAddress() const {
  return graphMode() ? graph_->currentAddress() : listing_->currentAddress();
}

std::optional<Address> DisassemblyView::currentItem() const {
  return graphMode() ? graph_->currentAddress() : listing_->currentItem();
}

std::optional<Address> DisassemblyView::currentFunction() const {
  return graphMode() ? graph_->function() : listing_->currentFunction();
}

QString DisassemblyView::currentFunctionName() const {
  return listing_->currentFunctionName();
}

std::optional<Address> DisassemblyView::operandTarget() const {
  return graphMode() ? graph_->operandTarget() : listing_->operandTarget();
}

std::optional<int> DisassemblyView::currentOperand() const {
  return graphMode() ? std::nullopt : listing_->currentOperand();
}

QString DisassemblyView::currentToken() const {
  return graphMode() ? graph_->currentToken() : listing_->currentToken();
}

QString DisassemblyView::selectedText() const {
  return graphMode() ? graph_->currentRowText() : listing_->selectedText();
}

void DisassemblyView::addressSpaceChanged() {
  listing_->viewport()->update();
  updateStatus();
}

void DisassemblyView::updateStatus() {
  const auto address = currentAddress();
  if (!address) {
    status_->clear();
    return;
  }
  const int digits = session_.bitness() == 64 ? 16 : 8;
  const auto offset = space_.fileOffsetOf(*address);
  QString location;
  const auto function = currentFunction();
  if (function) {
    const QString name = listing_->currentFunctionName();
    const QString base =
        name.isEmpty() ? QStringLiteral("sub_") + displayAddress(*function)
                       : name;
    location = *address == *function ? base
                                     : base + QStringLiteral("+") +
                                           displayAddress(*address - *function);
  } else if (const auto *region = space_.regionOf(*address)) {
    location =
        region->name + QLatin1Char(':') + displayAddress(*address, digits);
  }
  QString text = QStringLiteral("%1 %2: %3")
                     .arg(offset ? displayAddress(*offset, FileOffsetDigits)
                                 : QStringLiteral("--------"),
                          displayAddress(*address, digits), location);
  if (graphMode())
    text.prepend(QStringLiteral("%1% ").arg(graph_->zoom() * 100, 0, 'f', 0));
  if (!syncName_.isEmpty())
    text += tr(" (Synchronized with %1)").arg(syncName_);
  status_->setText(text);
}

} // namespace neverd::gui
