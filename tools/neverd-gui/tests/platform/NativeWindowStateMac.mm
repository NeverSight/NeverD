//===- NativeWindowStateMac.mm - Cocoa transition completion tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Observe AppKit completion instead of an eagerly updated Qt state flag.
///
//===----------------------------------------------------------------------===//

#include "../NativeWindowStates.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <QGuiApplication>
#include <QTest>
#include <QWidget>
#include <cstdint>

namespace neverd::gui::test {

bool nativeWindowIsMinimized(QWidget &Window) {
  return reinterpret_cast<NSView *>(Window.winId()).window.miniaturized;
}

bool hasNativeApplicationModalWindow() { return NSApp.modalWindow != nil; }

void quitNativeApplication() { [NSApp terminate:nil]; }

bool canPostNativePointerEvents() { return CGPreflightPostEventAccess(); }

bool moveNativePointer(QPoint Position) {
  if (!canPostNativePointerEvents())
    return false;
  CGEventRef Event = CGEventCreateMouseEvent(
      nullptr, kCGEventMouseMoved, CGPointMake(Position.x(), Position.y()),
      kCGMouseButtonLeft);
  if (!Event)
    return false;
  // Posting is asynchronous. A predicate such as !underMouse() can already
  // be true before this move reaches AppKit, letting the next move overtake
  // the native enter/leave transition the test intends to observe.
  bool Delivered = false;
  bool *Completion = &Delivered;
  const auto Token =
      static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(Completion));
  CGEventSetIntegerValueField(Event, kCGEventSourceUserData, Token);
  id Monitor = [NSEvent
      addLocalMonitorForEventsMatchingMask:NSEventMaskMouseMoved
                                   handler:^NSEvent *(NSEvent *NativeEvent) {
                                     if (NativeEvent.CGEvent &&
                                         CGEventGetIntegerValueField(
                                             NativeEvent.CGEvent,
                                             kCGEventSourceUserData) == Token)
                                       *Completion = true;
                                     return NativeEvent;
                                   }];
  if (!Monitor) {
    CFRelease(Event);
    return false;
  }
  CGEventPost(kCGHIDEventTap, Event);
  CFRelease(Event);
  const bool Result = QTest::qWaitFor([&] { return Delivered; });
  [NSEvent removeMonitor:Monitor];
  if (!Result)
    qWarning("AppKit did not dispatch the pointer move to (%d, %d)",
             Position.x(), Position.y());
  return Result;
}

bool reopenNativeApplication() {
  auto *Event = [NSAppleEventDescriptor
      appleEventWithEventClass:kCoreEventClass
                       eventID:kAEReopenApplication
              targetDescriptor:
                  [NSAppleEventDescriptor
                      descriptorWithProcessIdentifier:NSProcessInfo.processInfo
                                                          .processIdentifier]
                      returnID:kAutoGenerateReturnID
                 transactionID:kAnyTransactionID];
  NSError *Error = nil;
  [Event sendEventWithOptions:NSAppleEventSendNoReply timeout:1 error:&Error];
  return Error == nil;
}

bool setNativeFullScreen(QWidget &Window, bool FullScreen) {
  return setNativeWindowState(Window, FullScreen ? Qt::WindowFullScreen
                                                 : Qt::WindowNoState);
}

bool setNativeWindowState(QWidget &Window, Qt::WindowStates States,
                          const std::function<void()> &Apply) {
  if (QGuiApplication::platformName() != QLatin1String("cocoa"))
    return false;
  NSWindow *Native = reinterpret_cast<NSView *>(Window.winId()).window;
  if (!Native)
    return false;
  const bool FullScreen = States.testFlag(Qt::WindowFullScreen);
  const auto Request = [&] {
    if (Apply)
      Apply();
    else
      Window.setWindowState(States);
  };
  if (FullScreen == bool(Native.styleMask & NSWindowStyleMaskFullScreen)) {
    Request();
    return true;
  }
  bool Finished = false;
  bool *Completion = &Finished;
  NSNotificationCenter *Center = NSNotificationCenter.defaultCenter;
  id Observer = [Center
      addObserverForName:FullScreen ? NSWindowDidEnterFullScreenNotification
                                    : NSWindowDidExitFullScreenNotification
                  object:Native
                   queue:nil
              usingBlock:^(NSNotification *) {
                *Completion = true;
              }];
  Request();
  const bool Result = QTest::qWaitFor([&] { return Finished; });
  [Center removeObserver:Observer];
  return Result;
}

} // namespace neverd::gui::test
