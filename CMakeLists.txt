cmake_minimum_required(VERSION 3.27)
cmake_policy(SET CMP0144 NEW)
project(ulog VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0" CACHE STRING "Debug flags" FORCE)
set(CMAKE_C_FLAGS_DEBUG "-g -O0" CACHE STRING "Debug flags" FORCE)

include(FetchContent)

FetchContent_Declare(
        uvent
        GIT_REPOSITORY https://github.com/Usub-development/uvent.git
        GIT_TAG main
)
FetchContent_Declare(
        ureflect
        GIT_REPOSITORY https://github.com/Usub-development/ureflect.git
        GIT_TAG main
        OVERRIDE_FIND_PACKAGE
)

FetchContent_MakeAvailable(uvent ureflect)

file(GLOB_RECURSE ULOG_HEADERS CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/include/ulog/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/ulog/*.hpp
)
file(GLOB_RECURSE ULOG_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ulog/*.cpp
)

add_library(ulog ${ULOG_SOURCES} ${ULOG_HEADERS})
add_library(usub::ulog ALIAS ulog)

target_include_directories(ulog
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ulog>
)

target_link_libraries(ulog
        PUBLIC
        usub::uvent
        usub::ureflect
)

set_target_properties(ulog PROPERTIES
        EXPORT_NAME ulog
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
)

option(ULOG_BUILD_EXAMPLES "Build ulog example executables" ON)
if (ULOG_BUILD_EXAMPLES)
    add_executable(ulog_example examples/main.cpp)
    target_link_libraries(ulog_example PRIVATE ulog)

    add_executable(ulog_mutex_fallback_example examples/main_mutex_fallback.cpp)
    target_link_libraries(ulog_mutex_fallback_example PRIVATE ulog)

    add_executable(ulog_reflection_example examples/main_reflection.cpp)
    target_link_libraries(ulog_reflection_example PRIVATE ulog)
endif ()

install(TARGETS ulog
        EXPORT ulogTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ulog
)

install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ulog
        FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.hpp"
)

configure_package_config_file(
        cmake/ulogConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/ulogConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ulog
        NO_CHECK_REQUIRED_COMPONENTS_MACRO
)

write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/ulogConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/ulogConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/ulogConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ulog
)

install(EXPORT ulogTargets
        NAMESPACE ulog::
        FILE ulogTargets.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ulog
)