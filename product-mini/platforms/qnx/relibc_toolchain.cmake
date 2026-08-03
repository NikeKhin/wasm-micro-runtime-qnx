# Copyright (C) 2019 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Cross-compiles for AArch64 QNX Neutrino 7.1 or 8.0 against relibc's QNX port
# instead of the QNX SDP, with an aarch64-none-elf GCC such as the Arm GNU
# Toolchain's. relibc's headers rely on GCC's predefined macros and atomic
# builtins. RELIBC_QNX_DIR is the relibc checkout, built for QNX_VERSION with
# its qnx/build-qnx.sh and with its generated headers added to the sysroot.

set (CMAKE_SYSTEM_NAME QNX)
set (CMAKE_SYSTEM_PROCESSOR aarch64)

# try_compile() projects read this file again without the -D options
list (APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES RELIBC_QNX_DIR QNX_VERSION)

if (NOT RELIBC_QNX_DIR)
  message (FATAL_ERROR "Set RELIBC_QNX_DIR to the relibc checkout")
endif ()
if (NOT QNX_VERSION)
  set (QNX_VERSION 7.1)
endif ()
if (QNX_VERSION VERSION_EQUAL 7.1)
  set (RELIBC_QNX_TARGET aarch64-unknown-nto-qnx710)
elseif (QNX_VERSION VERSION_EQUAL 8.0)
  set (RELIBC_QNX_TARGET aarch64-unknown-nto-qnx800)
else ()
  message (FATAL_ERROR "QNX_VERSION must be 7.1 or 8.0")
endif ()
set (CMAKE_SYSTEM_VERSION ${QNX_VERSION})

set (RELIBC_QNX_SYSROOT
     ${RELIBC_QNX_DIR}/target/${RELIBC_QNX_TARGET}/release/sysroot)
set (RELIBC_QNX_LIB_DIR ${RELIBC_QNX_SYSROOT}/aarch64le/lib)
# GNU ld's default script leaves the program headers unmapped, and relibc
# needs them to find the TLS segment
set (RELIBC_QNX_LD_SCRIPT ${RELIBC_QNX_DIR}/targets/aarch64-qnx-static.ld)
foreach (file ${RELIBC_QNX_LIB_DIR}/crt1.o ${RELIBC_QNX_LIB_DIR}/libc.a
              ${RELIBC_QNX_SYSROOT}/usr/include/stdio.h ${RELIBC_QNX_LD_SCRIPT})
  if (NOT EXISTS ${file})
    message (FATAL_ERROR "${file} not found")
  endif ()
endforeach ()

if (NOT CMAKE_C_COMPILER)
  set (CMAKE_C_COMPILER aarch64-none-elf-gcc)
endif ()
set (CMAKE_ASM_COMPILER ${CMAKE_C_COMPILER})

execute_process (COMMAND ${CMAKE_C_COMPILER} -print-file-name=include
                 OUTPUT_VARIABLE GCC_INCLUDE_DIR
                 OUTPUT_STRIP_TRAILING_WHITESPACE)

# relibc's headers come first, then GCC's own, and newlib's not at all.
# relibc's headers select their QNX definitions with __QNXNTO__. Atomics are
# inlined so that the libraries don't need libgcc's outline helpers, which
# can't detect LSE support on this target anyway.
set (CMAKE_C_FLAGS_INIT
     "-D__QNXNTO__ -nostdinc -isystem ${RELIBC_QNX_SYSROOT}/usr/include -isystem ${GCC_INCLUDE_DIR} -mno-outline-atomics")
set (CMAKE_ASM_FLAGS_INIT "${CMAKE_C_FLAGS_INIT}")

set (CMAKE_EXE_LINKER_FLAGS_INIT
     "-nostdlib -static -Wl,--eh-frame-hdr -Wl,-z,max-page-size=4096 -Wl,-z,common-page-size=4096 -T ${RELIBC_QNX_LD_SCRIPT} -L${RELIBC_QNX_LIB_DIR} ${RELIBC_QNX_LIB_DIR}/crt1.o ${RELIBC_QNX_LIB_DIR}/crti.o")
set (CMAKE_C_STANDARD_LIBRARIES_INIT
     "-Wl,--start-group ${RELIBC_QNX_LIB_DIR}/libc.a -lgcc -Wl,--end-group ${RELIBC_QNX_LIB_DIR}/crtn.o")

set (CMAKE_FIND_ROOT_PATH ${RELIBC_QNX_SYSROOT})
set (CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set (CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set (CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set (CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
