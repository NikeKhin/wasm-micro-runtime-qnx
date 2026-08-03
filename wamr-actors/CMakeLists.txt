# Copyright (C) 2019 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required (VERSION 3.14)

project (wamr-actors C)

set (WAMR_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

if (NOT DEFINED WAMR_BUILD_PLATFORM)
  string (TOLOWER ${CMAKE_SYSTEM_NAME} WAMR_BUILD_PLATFORM)
endif ()

if (NOT CMAKE_BUILD_TYPE)
  set (CMAKE_BUILD_TYPE Release)
endif ()

set (CMAKE_C_STANDARD 99)

if (NOT DEFINED WAMR_BUILD_INTERP)
  set (WAMR_BUILD_INTERP 1)
endif ()
if (NOT DEFINED WAMR_BUILD_FAST_INTERP)
  set (WAMR_BUILD_FAST_INTERP 1)
endif ()
if (NOT DEFINED WAMR_BUILD_AOT)
  set (WAMR_BUILD_AOT 1)
endif ()
if (NOT DEFINED WAMR_BUILD_LIBC_BUILTIN)
  set (WAMR_BUILD_LIBC_BUILTIN 0)
endif ()
if (NOT DEFINED WAMR_BUILD_LIBC_WASI)
  set (WAMR_BUILD_LIBC_WASI 0)
endif ()
if (NOT DEFINED WAMR_BUILD_SIMD)
  set (WAMR_BUILD_SIMD 0)
endif ()
# Lets the per-message time budgets interrupt running code
if (NOT DEFINED WAMR_BUILD_THREAD_MGR)
  set (WAMR_BUILD_THREAD_MGR 1)
endif ()
if (WAMR_BUILD_PLATFORM STREQUAL "qnx"
    AND NOT DEFINED WAMR_DISABLE_STACK_HW_BOUND_CHECK)
  set (WAMR_DISABLE_STACK_HW_BOUND_CHECK 1)
endif ()

include (${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

find_package (Threads REQUIRED)

add_library (vmlib STATIC ${WAMR_RUNTIME_LIB_SOURCE})
set_target_properties (vmlib PROPERTIES OUTPUT_NAME iwasm)
target_link_libraries (vmlib Threads::Threads -lm)

add_library (wamr_actors STATIC
  src/actor_runtime.c
  src/actor_supervisor.c
  src/actor_timer.c
  src/actor_wasm.c
)
target_include_directories (wamr_actors PUBLIC include)
target_link_libraries (wamr_actors PUBLIC vmlib)
if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options (wamr_actors PRIVATE -Wall -Wextra)
endif ()

add_executable (wamr-actors launcher/main.c)
target_link_libraries (wamr-actors wamr_actors)

option (WAMR_ACTORS_BUILD_TESTS "Build the tests" ON)
set (WAMR_ACTORS_WASM_CC "" CACHE FILEPATH
     "clang that targets wasm32, to compile the test actors")

if (WAMR_ACTORS_BUILD_TESTS)
  add_executable (actor_test test/actor_test.c)
  target_include_directories (actor_test PRIVATE test/wasm)
  target_link_libraries (actor_test wamr_actors)

  if (WAMR_ACTORS_WASM_CC)
    set (TEST_ACTOR_DIR ${CMAKE_CURRENT_BINARY_DIR}/test)
    file (MAKE_DIRECTORY ${TEST_ACTOR_DIR})
    set (TEST_ACTORS
      "ping:ping.c" "pong:pong.c" "crasher:crasher.c" "spinner:spinner.c"
      "ticker:ticker.c" "worker:worker.c" "counter_v1:counter.c"
      "counter_v2:counter.c:-DCOUNTER_VERSION=2")
    foreach (entry ${TEST_ACTORS})
      string (REPLACE ":" ";" entry ${entry})
      list (GET entry 0 actor)
      list (GET entry 1 source)
      list (LENGTH entry fields)
      set (defines "")
      if (fields GREATER 2)
        list (GET entry 2 defines)
      endif ()
      add_custom_command (
        OUTPUT ${TEST_ACTOR_DIR}/${actor}.wasm
        COMMAND ${WAMR_ACTORS_WASM_CC} --target=wasm32 -mcpu=mvp
                -mbulk-memory -O2 -nostdlib -Wl,--no-entry ${defines}
                -I${CMAKE_CURRENT_LIST_DIR}/include
                -o ${TEST_ACTOR_DIR}/${actor}.wasm
                ${CMAKE_CURRENT_LIST_DIR}/test/wasm/${source}
        DEPENDS test/wasm/${source} test/wasm/test_tags.h
                include/wamr_actors_guest.h
        VERBATIM)
      list (APPEND TEST_ACTOR_FILES ${TEST_ACTOR_DIR}/${actor}.wasm)
    endforeach ()
    add_custom_target (test_actors ALL DEPENDS ${TEST_ACTOR_FILES})
    add_dependencies (actor_test test_actors)
  endif ()

  enable_testing ()
  add_test (NAME actor_test
            COMMAND actor_test ${CMAKE_CURRENT_BINARY_DIR}/test)
endif ()
