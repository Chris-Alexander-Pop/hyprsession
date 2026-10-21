cmake_minimum_required(VERSION 3.19)
project(hyprsession VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)

add_compile_options(
  -Wall
  -Wextra
  -Wno-unused-parameter
  -Wno-missing-field-initializers
  --no-gnu-unique
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HYPR_DEPS REQUIRED hyprland)
pkg_check_modules(LUA REQUIRED lua)

add_library(hyprsession SHARED
  src/main.cpp
  src/session.cpp
  src/capture.cpp
  src/restore.cpp
  src/snapshot.cpp
  src/store.cpp
)

target_include_directories(hyprsession PRIVATE src ${HYPR_DEPS_INCLUDE_DIRS} ${LUA_INCLUDE_DIRS})
foreach(inc ${HYPR_DEPS_INCLUDE_DIRS})
  if(EXISTS "${inc}/hyprland/src")
    target_include_directories(hyprsession PRIVATE "${inc}/hyprland/src")
  endif()
endforeach()
target_link_directories(hyprsession PRIVATE ${HYPR_DEPS_LIBRARY_DIRS} ${LUA_LIBRARY_DIRS})
target_link_libraries(hyprsession PRIVATE ${HYPR_DEPS_LIBRARIES} ${LUA_LIBRARIES})

add_executable(hyprsession-snapshot-test
  tools/snapshot_test.cpp
  src/snapshot.cpp
  src/store.cpp
)
target_include_directories(hyprsession-snapshot-test PRIVATE src)
add_test(NAME hyprsession-snapshot-test COMMAND hyprsession-snapshot-test)
