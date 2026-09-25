cmake_minimum_required(VERSION 3.20)
project(asio_bridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W3 /utf-8)
  # 静态链接 MSVC 运行库(/MT): 产物免装 VC++ 运行库, 单文件自包含便于分发
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  # 资源编译器(RC)：必须在使用它的 target 之前启用，否则 .rc 不参与链接、产物无图标
  enable_language(RC)
endif()

set(ASIO_SDK ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ASIOSDK2.3)

add_executable(asio_bridge
  src/main.cpp
  src/wasapi_process_capture.cpp
  src/wasapi_endpoint_capture.cpp
  src/volume_map.cpp
  src/asio_render.cpp
  src/wasapi_output.cpp
  src/device_scan.cpp
  src/web_console.cpp
  src/update_check.cpp
  ${ASIO_SDK}/common/asio.cpp
  ${ASIO_SDK}/host/asiodrivers.cpp
  ${ASIO_SDK}/host/pc/asiolist.cpp
)

target_include_directories(asio_bridge PRIVATE
  src
  ${ASIO_SDK}/common
  ${ASIO_SDK}/host
  ${ASIO_SDK}/host/pc
)

target_link_libraries(asio_bridge PRIVATE ole32 oleaut32 uuid avrt ws2_32 mmdevapi winhttp bcrypt)

# 应用图标：资源脚本必须参与编译，否则产物没有 RT_ICON，
# 资源管理器/任务栏/开始菜单会显示默认图标（这是「每次更新都丢图标」的根因——
# 图标此前只在 build/build_bridge.sh 里用 rc.exe 链入，而打包取的是 CMake 产物）。
if(MSVC)
  target_sources(asio_bridge PRIVATE packaging/asio_bridge.rc)
endif()

# 供 MSVC 命令行环境使用
if(MSVC)
  target_compile_definitions(asio_bridge PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# ASIO SDK 老代码修复：
# asiodrivers.cpp 使用 IASIO 前未包含 windows.h，强制前置包含
set_source_files_properties(${ASIO_SDK}/host/asiodrivers.cpp PROPERTIES COMPILE_OPTIONS "/FIwindows.h")
