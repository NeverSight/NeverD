// Original public fixture: the C exports below are declared by the official
// C++ UCRT/VCRuntime headers, including a function returning a callback.
#include <corecrt_terminate.h>
#include <vcruntime_typeinfo.h>

extern "C" __declspec(dllexport) terminate_handler
UseCRTTerminator(terminate_handler Handler) {
  return set_terminate(Handler);
}

extern "C" __declspec(dllexport) void
UseCRTExceptionCopy(const __std_exception_data *Source,
                    __std_exception_data *Destination) {
  __std_exception_copy(Source, Destination);
}

extern "C" __declspec(dllexport) const char *
UseCRTTypeName(__std_type_info_data *Data, __type_info_node *Root) {
  return __std_type_info_name(Data, Root);
}
