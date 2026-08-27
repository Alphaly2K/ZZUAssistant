cmake_minimum_required(VERSION 3.25)
project(ZZUAssistant VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)

if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ZZU_USE_STATIC_MSVC_RUNTIME "Use /MT instead of the vcpkg-compatible /MD" OFF)
option(ZZU_USE_STATIC_OPENSSL "Prefer static OpenSSL libraries" OFF)
option(ZZU_BUILD_CLI "Build the ZZUAssistant command-line executable" ON)
option(ZZU_BUILD_FFI "Build the ZZUAssistant C ABI shared library" ON)
set(ZZU_LOCAL_DEPENDENCY_ROOT "" CACHE PATH
        "Optional non-vcpkg prefix containing OpenSSL, libpng and zlib")
set(BOOST_ROOT "" CACHE PATH "Optional unpacked or installed Boost tree")

# vcpkg manifest mode supplies package config files. An unpacked BOOST_ROOT
# has no config file, so retain FindBoost as a portable manual fallback.
set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_STATIC_RUNTIME ${ZZU_USE_STATIC_MSVC_RUNTIME})
find_package(Boost 1.84 CONFIG QUIET)
if(NOT Boost_FOUND)
    cmake_policy(PUSH)
    if(POLICY CMP0167)
        cmake_policy(SET CMP0167 OLD)
    endif()
    if(BOOST_ROOT)
        set(Boost_NO_SYSTEM_PATHS ON)
    endif()
    find_package(Boost 1.84 REQUIRED)
    cmake_policy(POP)
endif()
find_package(Threads REQUIRED)

set(OPENSSL_USE_STATIC_LIBS ${ZZU_USE_STATIC_OPENSSL})
if(ZZU_LOCAL_DEPENDENCY_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${ZZU_LOCAL_DEPENDENCY_ROOT}")
    set(OPENSSL_ROOT_DIR "${ZZU_LOCAL_DEPENDENCY_ROOT}" CACHE PATH
            "Path to an OpenSSL installation" FORCE)
    set(ZLIB_ROOT "${ZZU_LOCAL_DEPENDENCY_ROOT}" CACHE PATH
            "Path to a zlib installation" FORCE)
    set(PNG_ROOT "${ZZU_LOCAL_DEPENDENCY_ROOT}" CACHE PATH
            "Path to a libpng installation" FORCE)
endif()
find_package(OpenSSL 3 REQUIRED COMPONENTS SSL Crypto)
if(ZZU_BUILD_CLI)
    find_package(PNG REQUIRED)
    find_package(Python3 COMPONENTS Interpreter QUIET)
endif()

set(ZZUASSISTANT_CLIENT_SOURCES
        src/client/portal_client.cpp
        src/client/sso_client.cpp
        src/client/ecard_client.cpp
        src/client/app_client.cpp
        src/client/userinfo_client.cpp)

set(ZZUASSISTANT_PUBLIC_HEADERS
        include/zzuassistant/client.h
        include/client/options.h
        include/client/portal_client.h
        include/client/sso_client.h
        include/client/ecard_client.h
        include/client/app_client.h
        include/client/userinfo_client.h
        include/model/ecard.h
        include/model/electricity.h
        include/model/userinfo.h)

add_library(ZZUAssistantClient STATIC
        ${ZZUASSISTANT_CLIENT_SOURCES}
        ${ZZUASSISTANT_PUBLIC_HEADERS})
add_library(ZZUAssistant::Client ALIAS ZZUAssistantClient)
set_target_properties(ZZUAssistantClient PROPERTIES
        EXPORT_NAME Client
        POSITION_INDEPENDENT_CODE ON)
target_compile_features(ZZUAssistantClient PUBLIC cxx_std_23)
target_include_directories(ZZUAssistantClient PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(ZZUAssistantClient PRIVATE
        Boost::headers Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(ZZUAssistantClient PRIVATE
        BOOST_ALL_NO_LIB
        BOOST_ALL_STATIC_LINK)

if(ZZU_BUILD_FFI)
    add_library(ZZUAssistantFFI SHARED
            src/ffi/ffi.cpp
            include/zzuassistant/ffi.h)
    add_library(ZZUAssistant::FFI ALIAS ZZUAssistantFFI)
    set_target_properties(ZZUAssistantFFI PROPERTIES
            EXPORT_NAME FFI
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON)
    target_compile_features(ZZUAssistantFFI PRIVATE cxx_std_23)
    target_include_directories(ZZUAssistantFFI PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(ZZUAssistantFFI PRIVATE ZZUAssistant::Client Boost::headers)
    target_compile_definitions(ZZUAssistantFFI PRIVATE ZZUASSISTANT_FFI_BUILD)

    if(BUILD_TESTING)
        add_executable(ZZUAssistantFFISmoke tests/ffi_smoke.cpp)
        target_link_libraries(ZZUAssistantFFISmoke PRIVATE ZZUAssistant::FFI)
        add_test(NAME ffi_smoke COMMAND ZZUAssistantFFISmoke)
    endif()
endif()

if(ZZU_BUILD_CLI)
    add_executable(ZZUAssistant src/main.cpp
        src/cli/console.cpp
        src/cli/qr_renderer.cpp
        src/third_party/qrcodegen.cpp
        src/service/echo_service.cpp
        src/service/portal_service.cpp
        src/service/sso_service.cpp
        src/service/app_service.cpp
        src/service/completions_service.cpp
        src/service/course_service.cpp
        src/service/ecard_service.cpp
        src/service/electricity_service.cpp
        src/service/userinfo_service.cpp
        src/cli/session_store.cpp
        src/registry.cpp
        include/service/service.h
        include/service/echo_service.h
        include/client/portal_client.h
        include/service/portal_service.h
        include/client/sso_client.h
        include/service/sso_service.h
        include/client/ecard_client.h
        include/client/app_client.h
        include/service/app_service.h
        include/service/completions_service.h
        include/service/course_service.h
        include/service/ecard_service.h
        include/service/electricity_service.h
        include/client/userinfo_client.h
        include/service/userinfo_service.h
        include/cli/session_store.h
        include/auth/environment.h
        include/cli/console.h
        include/cli/qr_renderer.h
        include/third_party/qrcodegen.hpp
        include/registry.h
        include/model/sso.h
        include/model/portal.h
        include/model/constants.h
        include/model/app.h
        include/model/ecard.h
        include/model/electricity.h
        include/model/userinfo.h)

    target_include_directories(ZZUAssistant PRIVATE include)
    target_link_libraries(ZZUAssistant PRIVATE
            ZZUAssistant::Client Boost::headers OpenSSL::Crypto PNG::PNG)

    if(Python3_Interpreter_FOUND)
        file(TO_CMAKE_PATH "${Python3_EXECUTABLE}" ZZU_PYTHON_EXECUTABLE_PATH)
        target_compile_definitions(ZZUAssistant PRIVATE
                ZZU_PYTHON_EXECUTABLE="${ZZU_PYTHON_EXECUTABLE_PATH}")
    endif()

    add_custom_command(TARGET ZZUAssistant POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                $<TARGET_FILE_DIR:ZZUAssistant>/tool
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/tool/course.py
                $<TARGET_FILE_DIR:ZZUAssistant>/tool/course.py
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.md
                $<TARGET_FILE_DIR:ZZUAssistant>/LICENSE.md
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/NOTICE
                $<TARGET_FILE_DIR:ZZUAssistant>/NOTICE
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
                $<TARGET_FILE_DIR:ZZUAssistant>/THIRD_PARTY_NOTICES.md)
endif()

# SSL_CERT_FILE remains the runtime override. This optional build-time fallback
# also supports manually supplied distributions whose CA path is not compiled in.
if(OPENSSL_ROOT_DIR AND EXISTS "${OPENSSL_ROOT_DIR}/ssl/cacert.pem")
    file(TO_CMAKE_PATH "${OPENSSL_ROOT_DIR}/ssl/cacert.pem" ZZU_OPENSSL_CA_FILE)
    target_compile_definitions(ZZUAssistantClient PRIVATE
            ZZU_DEFAULT_CA_FILE="${ZZU_OPENSSL_CA_FILE}")
endif()

if(WIN32)
    target_link_libraries(ZZUAssistantClient PRIVATE ws2_32 mswsock)

    # Manual dependency prefixes might expose import libraries without placing
    # their DLL directory on PATH. vcpkg handles app-local DLL deployment itself.
    if(ZZU_BUILD_CLI AND EXISTS "${OPENSSL_ROOT_DIR}/bin/libcrypto-3-x64.dll")
        set(ZZU_RUNTIME_DLLS
                "${OPENSSL_ROOT_DIR}/bin/libcrypto-3-x64.dll"
                "${OPENSSL_ROOT_DIR}/bin/libssl-3-x64.dll"
                "${OPENSSL_ROOT_DIR}/bin/libpng16.dll"
                "${OPENSSL_ROOT_DIR}/bin/zlib.dll")
        add_custom_command(TARGET ZZUAssistant POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${ZZU_RUNTIME_DLLS} $<TARGET_FILE_DIR:ZZUAssistant>
                COMMAND_EXPAND_LISTS)
    endif()
endif()

# The default /MD matches vcpkg's x64-windows triplet and dynamic OpenSSL.
# Keep /MT available for custom/manual dependency builds.
if((MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC") AND
   ZZU_USE_STATIC_MSVC_RUNTIME)
    if(ZZU_BUILD_CLI)
        set_property(TARGET ZZUAssistant PROPERTY
                MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    set_property(TARGET ZZUAssistantClient PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CMakePackageConfigHelpers)

install(TARGETS ZZUAssistantClient
        EXPORT ZZUAssistantTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(ZZU_BUILD_FFI)
    install(TARGETS ZZUAssistantFFI
            EXPORT ZZUAssistantTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES include/zzuassistant/ffi.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zzuassistant)
endif()
install(FILES
        include/client/app_client.h
        include/client/ecard_client.h
        include/client/portal_client.h
        include/client/sso_client.h
        include/client/userinfo_client.h
        include/client/options.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/client)
install(FILES
        include/model/ecard.h
        include/model/electricity.h
        include/model/userinfo.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/model)
install(FILES include/zzuassistant/client.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zzuassistant)
install(FILES LICENSE.md NOTICE THIRD_PARTY_NOTICES.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/ZZUAssistant)
install(FILES docs/CLIENT_LIBRARY.md docs/API.md docs/FFI.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

configure_package_config_file(
        cmake/ZZUAssistantConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/ZZUAssistantConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ZZUAssistant)
write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/ZZUAssistantConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

install(EXPORT ZZUAssistantTargets
        FILE ZZUAssistantTargets.cmake
        NAMESPACE ZZUAssistant::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ZZUAssistant)
install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/ZZUAssistantConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/ZZUAssistantConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ZZUAssistant)
