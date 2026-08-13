//===- PythonPluginMarshal.cpp - Python/C value marshalling ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Conversions between Python objects and the C values the plugin host works
/// in: plugin metadata attributes read out as validated std::strings, and
/// optional C strings decoded into Python text.
///
//===----------------------------------------------------------------------===//

#include "PythonPluginRuntimeDetail.h"

#include <cstddef>
#include <cstring>
#include <string>

namespace neverd::sdk::python_plugin {

bool metadataString(PyObject *Spec, const char *Field, bool AllowEmpty,
                    std::string &Result) {
  PyRef Value(PyObject_GetAttrString(Spec, Field));
  if (!Value || !PyUnicode_Check(Value.get())) {
    if (!PyErr_Occurred())
      PyErr_Format(PyExc_TypeError, "plugin metadata '%s' must be a string",
                   Field);
    return false;
  }
  Py_ssize_t Length = 0;
  const char *Data = PyUnicode_AsUTF8AndSize(Value.get(), &Length);
  if (!Data)
    return false;
  Result.assign(Data, static_cast<size_t>(Length));
  if ((!AllowEmpty && Result.empty()) ||
      Result.find('\0') != std::string::npos) {
    PyErr_Format(PyExc_ValueError,
                 "plugin metadata '%s' must be non-empty and NUL-free", Field);
    return false;
  }
  return true;
}

PyObject *unicodeFromOptionalCString(const char *Value, const char *Field) {
  if (!Value)
    return Py_NewRef(Py_None);
  PyObject *Result = PyUnicode_DecodeUTF8(
      Value, static_cast<Py_ssize_t>(strlen(Value)), "strict");
  if (!Result && PyErr_ExceptionMatches(PyExc_UnicodeDecodeError)) {
    PyErr_Clear();
    PyErr_Format(PyExc_ValueError, "event %s is not valid UTF-8", Field);
  }
  return Result;
}

} // namespace neverd::sdk::python_plugin
