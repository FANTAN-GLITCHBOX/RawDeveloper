cmake_minimum_required(VERSION 3.21)
project(RawDeveloper VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(MSVC)
  # ソースは UTF-8（日本語の文字列を含むため）
  add_compile_options(/utf-8 /W3 /permissive-)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Concurrent)

# LibRaw：Windows では vcpkg、Linux（開発用）では pkg-config
find_package(libraw CONFIG QUIET)
if(TARGET libraw::raw_r)
  set(LIBRAW_TARGET libraw::raw_r)
elseif(TARGET libraw::raw)
  set(LIBRAW_TARGET libraw::raw)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBRAW REQUIRED IMPORTED_TARGET libraw_r)
  set(LIBRAW_TARGET PkgConfig::LIBRAW)
endif()

# 現像エンジン（画面に依存しない部分）
add_library(rdengine STATIC
  src/engine/Image.h
  src/engine/Parallel.h
  src/engine/Color.h src/engine/Color.cpp
  src/engine/Params.h src/engine/Params.cpp
  src/engine/Pipeline.h src/engine/Pipeline.cpp
  src/engine/Auto.h src/engine/Auto.cpp
  src/engine/RawLoader.h src/engine/RawLoader.cpp
)
target_include_directories(rdengine PUBLIC src)
target_link_libraries(rdengine PUBLIC ${LIBRAW_TARGET} Qt6::Core)
if(WIN32)
  target_compile_definitions(rdengine PUBLIC NOMINMAX)
endif()

# アプリ本体
qt_add_executable(RawDeveloper WIN32
  src/main.cpp
  src/ui/Theme.h src/ui/Theme.cpp
  src/ui/Widgets.h src/ui/Widgets.cpp
  src/ui/CurveEditor.h src/ui/CurveEditor.cpp
  src/ui/Scopes.h src/ui/Scopes.cpp
  src/ui/Viewer.h src/ui/Viewer.cpp
  src/ui/DevelopPanel.h src/ui/DevelopPanel.cpp
  src/ui/MainWindow.h src/ui/MainWindow.cpp
)
target_link_libraries(RawDeveloper PRIVATE rdengine Qt6::Widgets Qt6::Concurrent)

install(TARGETS RawDeveloper RUNTIME DESTINATION .)

# エンジンの動作確認用（コンソール）
add_executable(rd_selftest tests/engine_selftest.cpp)
target_link_libraries(rd_selftest PRIVATE rdengine)
install(TARGETS rd_selftest RUNTIME DESTINATION .)

# Python 版との一致確認用：リニア画像(.f32)＋パラメータ(JSON) → 現像結果(.f32)
add_executable(rd_develop tests/develop_tool.cpp)
target_link_libraries(rd_develop PRIVATE rdengine)
