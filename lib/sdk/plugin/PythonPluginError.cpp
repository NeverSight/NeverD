//===- PythonPluginError.cpp - Python exception translation ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Turns a pending Python exception into a host diagnostic string, preferring
/// a formatted traceback and degrading to str(value) when the traceback
/// machinery itself is unavailable.
///
//===----------------------------------------------------------------------===//

#include "PythonPluginRuntimeDetail.h"

#include <cstddef>
#include <string>

namespace neverd::sdk::python_plugin {

namespace {

std::string fallbackPythonException(PyObject *Value) {
  if (!Value)
    return "unknown Python exception";
  PyRef Text(PyObject_Str(Value));
  if (!Text) {
    PyErr_Clear();
    return "unprintable Python exception";
  }
  Py_ssize_t Length = 0;
  const char *Data = PyUnicode_AsUTF8AndSize(Text.get(), &Length);
  if (!Data) {
    PyErr_Clear();
    return "unprintable Python exception";
  }
  return std::string(Data, static_cast<size_t>(Length));
}

} // namespace

std::string formatPythonException() {
  if (!PyErr_Occurred())
    return "unknown Python exception";

  PyObject *RawType = nullptr;
  PyObject *RawValue = nullptr;
  PyObject *RawTraceback = nullptr;
  PyErr_Fetch(&RawType, &RawValue, &RawTraceback);
  PyErr_NormalizeException(&RawType, &RawValue, &RawTraceback);
  PyRef Type(RawType);
  PyRef Value(RawValue);
  PyRef Traceback(RawTraceback);

  PyRef TracebackModule(PyImport_ImportModule("traceback"));
  PyRef Formatter(
      TracebackModule
          ? PyObject_GetAttrString(TracebackModule.get(), "format_exception")
          : nullptr);
  PyObject *TracebackArgument = Traceback ? Traceback.get() : Py_None;
  PyRef Lines(Formatter ? PyObject_CallFunctionObjArgs(
                              Formatter.get(), Type ? Type.get() : Py_None,
                              Value ? Value.get() : Py_None, TracebackArgument,
                              nullptr)
                        : nullptr);
  PyRef Separator(Lines ? PyUnicode_FromString("") : nullptr);
  PyRef Joined(Separator ? PyUnicode_Join(Separator.get(), Lines.get())
                         : nullptr);
  if (!Joined) {
    PyErr_Clear();
    return fallbackPythonException(Value.get());
  }
  Py_ssize_t Length = 0;
  const char *Data = PyUnicode_AsUTF8AndSize(Joined.get(), &Length);
  if (!Data) {
    PyErr_Clear();
    return fallbackPythonException(Value.get());
  }
  return std::string(Data, static_cast<size_t>(Length));
}

} // namespace neverd::sdk::python_plugin
