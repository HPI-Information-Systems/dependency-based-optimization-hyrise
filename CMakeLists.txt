# consider updating DEPENDENCIES.md when you touch this line
cmake_minimum_required(VERSION 3.18 FATAL_ERROR)

project(Hyrise)

if(APPLE)
    set(CMAKE_EXE_LINKER_FLAGS -Wl,-export_dynamic)
else()
    set(CMAKE_EXE_LINKER_FLAGS -Wl,--export-dynamic)
endif()

option(ENABLE_UNSUPPORTED_COMPILER "Set to ON to build Hyrise even if the compiler is not supported. Default: OFF" OFF)
function(compiler_not_supported message)
    if (${ENABLE_UNSUPPORTED_COMPILER})
        message(WARNING ${message})
    else()
        message(FATAL_ERROR "${message} You can ignore this error by setting -DENABLE_UNSUPPORTED_COMPILER=ON.")
    endif()
endfunction(compiler_not_supported)

if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
        compiler_not_supported("Your GCC version ${CMAKE_CXX_COMPILER_VERSION} is too old.")
    endif()
    if (APPLE)
        # https://software.intel.com/en-us/forums/intel-threading-building-blocks/topic/749200
        compiler_not_supported("We had to drop support for GCC on OS X because it caused segfaults when used with tbb. You can continue, but don't hold us responsible for any segmentation faults.")
    endif()
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
    if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
        compiler_not_supported("Your clang version ${CMAKE_CXX_COMPILER_VERSION} is too old.")
    endif()
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "AppleClang")
    if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14.0)
        compiler_not_supported("Hyrise does not support Apple clang compilers older than version 14.")
    endif()
    # In release mode, we found issues on ARM with loop unrolling (macOS Ventura, Apple Clang 14.0).
    # As we do not benchmark on macOS, we can safely ignore this issue for now.
    add_compile_options(-Wno-pass-failed)
    execute_process(
        COMMAND brew --prefix
        RESULT_VARIABLE BREW
        OUTPUT_VARIABLE BREW_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT BREW EQUAL 0)
        set(BREW_PREFIX "<brew_library_path>")
        set(CMAKE_OSX_BREW_TEXT "Homebrew (see https://brew.sh) and ")
    endif()
    if (NOT EXISTS "${BREW_PREFIX}/opt/llvm/bin/clang")
        set(CMAKE_OSX_INSTALL_TEXT "install ${CMAKE_OSX_BREW_TEXT}the official llvm/clang package using `install_dependencies.sh` or `brew install llvm --with-toolchain` and then ")
    endif()
    message(WARNING "On macOS, we recommend a recent clang compiler instead of Apple's clang compiler. Please ${CMAKE_OSX_INSTALL_TEXT}run cmake with `-DCMAKE_C_COMPILER=${BREW_PREFIX}/opt/llvm/bin/clang -DCMAKE_CXX_COMPILER=${BREW_PREFIX}/opt/llvm/bin/clang++`.")
else()
    compiler_not_supported("You are using an unsupported compiler (${CMAKE_CXX_COMPILER_ID})! Compilation has only been tested with Clang (Linux + OS X) and GCC (Linux).")
endif()

# Enable address and undefined behavior sanitization if requested
option(ENABLE_ADDR_UB_SANITIZATION "Set to ON to build Hyrise with ASAN and UBSAN enabled. Default: OFF" OFF)
if (${ENABLE_ADDR_UB_SANITIZATION})
    # add_compile_options() wants list, CMAKE_EXE_LINKER_FLAGS a string. There are probably cleverer ways than
    # duplicating the flags, but this is probably the simplest solution.
    # -fno-sanitize-recover=all is used to make UBsan fail (i.e., return a non-zero exit code) when an error is found.
    # -mllvm -asan-use-private-alias=1 fixes false positive ODR violation alerts for shared libraries, see https://github.com/google/sanitizers/issues/1017 and https://reviews.llvm.org/D137227
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fsanitize-blacklist=${CMAKE_CURRENT_SOURCE_DIR}/resources/.ubsan-blacklist.txt -fno-omit-frame-pointer -mllvm -asan-use-private-alias=1)
    add_link_options(-fsanitize=address,undefined)
    set(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address,undefined -fno-sanitize-recover=all -fsanitize-blacklist=${CMAKE_CURRENT_SOURCE_DIR}/resources/.ubsan-blacklist.txt -fno-omit-frame-pointer")
endif()

# Enable thread sanitization if requested
option(ENABLE_THREAD_SANITIZATION "Set to ON to build Hyrise with TSAN enabled. Default: OFF" OFF)
if (${ENABLE_THREAD_SANITIZATION})
    # add_compile_options() wants list, CMAKE_EXE_LINKER_FLAGS a string. There are probably cleverer ways than
    # duplicating the flags, but this is probably the simplest solution.
    add_compile_options(-fsanitize=thread -O1)
    add_link_options(-fsanitize=thread)
    set(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
endif()

# Set default build type if none was passed on the command line
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()
set(BUILD_TYPES Debug RelWithDebInfo Release)
if(NOT CMAKE_BUILD_TYPE IN_LIST BUILD_TYPES)
    message(FATAL_ERROR "Unknown Build Type: ${CMAKE_BUILD_TYPE}")
endif()

# CMake settings
set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake ${CMAKE_MODULE_PATH}) # To allow CMake to locate our Find*.cmake files
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}) # Put binaries into root of build tree
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib) # Put libraries into their own dir

# C(++) Flags
set(FLAGS_ALL "-fopenmp-simd")  # enables loop vectorization hints, but does not include the OpenMP runtime
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} ${FLAGS_ALL}")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${FLAGS_ALL}")

# Build the binary optimized for the current system, ignoring older systems.
# Not all environments support march=native - check before we use it. Otherwise use mcpu.
include(CheckCXXCompilerFlag)
CHECK_CXX_COMPILER_FLAG("-march=native" COMPILER_SUPPORTS_MARCH_NATIVE)
if(COMPILER_SUPPORTS_MARCH_NATIVE)
    set(FLAGS_RELEASE "${FLAGS_RELEASE} -march=native")
else()
    set(FLAGS_RELEASE "${FLAGS_RELEASE} -mcpu=native")
endif()

set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} ${FLAGS_ALL} ${FLAGS_RELEASE}")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${FLAGS_ALL} ${FLAGS_RELEASE}")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} ${FLAGS_ALL} ${FLAGS_RELEASE}")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} ${FLAGS_ALL} ${FLAGS_RELEASE}")

include(CheckLinkerFlag)
if (HYRISE_LINKER)
    check_linker_flag(CXX "-fuse-ld=${HYRISE_LINKER}" HAS_HYRISE_LINKER)
    if (NOT HAS_HYRISE_LINKER)
        message(FATAL_ERROR "Got HYRISE_LINKER=${HYRISE_LINKER}, but -fuse-ld=${HYRISE_LINKER} does not work.")
    endif()
    message(STATUS "Using HYRISE_LINKER (${HYRISE_LINKER}).")
    add_link_options("-fuse-ld=${HYRISE_LINKER}")
else()
    check_linker_flag(CXX "-fuse-ld=lld" HAS_LLD)
    if (HAS_LLD)
        message(STATUS "Found LLD, using as linker.")
        add_link_options("-fuse-ld=lld")
    else()
        message(STATUS "Using default linker.")
        if (NOT APPLE)
            message(WARNING "Consider installing the LLD linker for faster incremental builds on Linux.")
        endif()
    endif()
endif()

# Require NCurses over Curses
set(CURSES_NEED_NCURSES TRUE)

# Surprisingly, this makes the linking process on Mac faster by 5x
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0")
set(CMAKE_SHARED_LINKER_FLAGS_DEBUG "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -O0")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -O0")

# Dependencies
set(DEFAULT_LIB_DIRS $ENV{HOME}/local /opt/local /usr/local /usr)
find_package(Numa QUIET)
find_package(Tbb REQUIRED)
find_package(Readline REQUIRED)
find_package(Curses REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Boost REQUIRED COMPONENTS container date_time NO_MODULE)

add_definitions(-DBOOST_THREAD_VERSION=5)
set(BOOST_LIBS Boost::container Boost::date_time)

# If we are building Hyrise for the CI server, we want to make sure that all optional features are available and can be tested
if(CI_BUILD)
    if(NOT ${NUMA_FOUND})
        message(FATAL_ERROR "-DCI_BUILD=ON was set, but libnuma was not found.")
    endif()
endif()

# Link Time Optimization (LTO)
cmake_policy(SET CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
option(ENABLE_LTO "Set to ON to build Hyrise with enabled Link Time Optimization (LTO). Default: OFF" OFF)
if (${ENABLE_LTO})
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_output)
    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        add_definitions(-DWITH_LTO)
        message(STATUS "Building with Link-Time Optimization")
    else()
        message(WARNING "LTO not supported: ${ipo_output}")
    endif()
endif()

# Include sub-CMakeLists.txt
add_subdirectory(third_party/ EXCLUDE_FROM_ALL)

# Some third-party libs don't support LTO (if enabled above):
foreach(no_lto_target gtest gtest_main gmock gmock_main)
    if(TARGET no_lto_target)
        set_property(TARGET ${no_lto_target} PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
    endif()
endforeach(no_lto_target)
add_subdirectory(src)


# Useful for printing all c++ files in a directory:
# find . -type f -name "*.cpp" -o -name "*.hpp" | cut -c 3- | sort
