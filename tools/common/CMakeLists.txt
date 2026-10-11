# Shared native transports depend only on JSON and the public C ABI.
if(NOT TARGET neverd_tool_json)
  add_subdirectory(transport)
endif()

add_library(neverd_tool_web STATIC web/Backend.cpp)
target_include_directories(neverd_tool_web PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
# Consumers choose the real or mock C ABI. Never add LLVM or a worker process.
target_link_libraries(neverd_tool_web PUBLIC neverd_tool_json PRIVATE ${CMAKE_DL_LIBS})
