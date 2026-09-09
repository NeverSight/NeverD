#pragma once

class QQmlApplicationEngine;
class Workbench;

// Exercises the actual packaged QML tree and KDDockWidgets frontend.
// The caller supplies an isolated layout path and starts the application loop.
void startDockingProbe(QQmlApplicationEngine &engine, Workbench &workbench);
