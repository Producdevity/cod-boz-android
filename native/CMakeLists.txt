cmake_minimum_required(VERSION 3.24)

project(codboz_runtime LANGUAGES CXX C)

set(CMAKE_POLICY_DEFAULT_CMP0144 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0167 OLD)
set(CMAKE_POLICY_DEFAULT_CMP0218 OLD)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

option(CODBOZ_ENABLE_SANITIZERS "Enable host AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(codboz_sanitizers INTERFACE)
if(CODBOZ_ENABLE_SANITIZERS)
    if(ANDROID OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "CODBOZ_ENABLE_SANITIZERS requires a Clang or GCC host build")
    endif()
    target_compile_options(codboz_sanitizers INTERFACE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(codboz_sanitizers INTERFACE -fsanitize=address,undefined)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(SOURCE_PROFILE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../config/source-profile.json")
file(READ "${SOURCE_PROFILE_PATH}" SOURCE_PROFILE_JSON)

string(JSON SOURCE_APK_SHA256 GET "${SOURCE_PROFILE_JSON}" source_apk sha256)
string(JSON SOURCE_APK_PACKAGE GET "${SOURCE_PROFILE_JSON}" source_apk package)
string(JSON SOURCE_APK_VERSION_CODE GET "${SOURCE_PROFILE_JSON}" source_apk version_code)
string(JSON SOURCE_APK_VERSION_NAME GET "${SOURCE_PROFILE_JSON}" source_apk version_name)
string(JSON SOURCE_APK_MIN_SDK GET "${SOURCE_PROFILE_JSON}" source_apk min_sdk)
string(JSON SOURCE_APK_TARGET_SDK GET "${SOURCE_PROFILE_JSON}" source_apk target_sdk)
string(JSON SOURCE_APK_SIGNER_SHA256 GET "${SOURCE_PROFILE_JSON}" source_apk signer_certificate_sha256)
string(JSON SOURCE_GUEST_S3E_ARCH GET "${SOURCE_PROFILE_JSON}" guest s3e_arch)
string(JSON SOURCE_GUEST_ARCHITECTURE GET "${SOURCE_PROFILE_JSON}" guest architecture)
string(JSON SOURCE_GUEST_PACKED_SIZE GET "${SOURCE_PROFILE_JSON}" guest packed_size)
string(JSON SOURCE_GUEST_PACKED_SHA256 GET "${SOURCE_PROFILE_JSON}" guest packed_sha256)
string(JSON SOURCE_GUEST_UNPACKED_SIZE GET "${SOURCE_PROFILE_JSON}" guest unpacked_size)
string(JSON SOURCE_GUEST_UNPACKED_SHA256 GET "${SOURCE_PROFILE_JSON}" guest unpacked_sha256)
string(JSON SOURCE_RESOURCE_BASE_URL GET "${SOURCE_PROFILE_JSON}" resources base_url)

foreach(digest IN ITEMS
        SOURCE_APK_SHA256
        SOURCE_APK_SIGNER_SHA256
        SOURCE_GUEST_PACKED_SHA256
        SOURCE_GUEST_UNPACKED_SHA256)
    string(LENGTH "${${digest}}" digest_length)
    if(NOT digest_length EQUAL 64 OR NOT "${${digest}}" MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "Invalid SHA-256 in ${SOURCE_PROFILE_PATH}: ${digest}")
    endif()
endforeach()

string(JSON SOURCE_ARCHIVE_COUNT LENGTH "${SOURCE_PROFILE_JSON}" resources archives)
set(SOURCE_ARCHIVE_ENTRIES "")
if(SOURCE_ARCHIVE_COUNT GREATER 0)
    math(EXPR SOURCE_ARCHIVE_LAST "${SOURCE_ARCHIVE_COUNT} - 1")
    foreach(index RANGE 0 ${SOURCE_ARCHIVE_LAST})
        string(JSON archive_name GET "${SOURCE_PROFILE_JSON}" resources archives ${index} name)
        string(JSON archive_size GET "${SOURCE_PROFILE_JSON}" resources archives ${index} size)
        string(JSON archive_sha256 GET "${SOURCE_PROFILE_JSON}" resources archives ${index} sha256)
        string(LENGTH "${archive_sha256}" archive_sha256_length)
        if(NOT archive_name MATCHES "^[A-Za-z0-9_.-]+$" OR
           NOT archive_sha256_length EQUAL 64 OR
           NOT archive_sha256 MATCHES "^[0-9a-f]+$")
            message(FATAL_ERROR "Invalid resource archive in ${SOURCE_PROFILE_PATH}")
        endif()
        string(APPEND SOURCE_ARCHIVE_ENTRIES
            "    ResourceArchiveProfile{\"${archive_name}\", ${archive_size}u, \"${archive_sha256}\"},\n")
    endforeach()
endif()

string(JSON SOURCE_EXTENSION_COUNT LENGTH "${SOURCE_PROFILE_JSON}" extensions)
set(SOURCE_EXTENSION_ENTRIES "")
if(SOURCE_EXTENSION_COUNT GREATER 0)
    math(EXPR SOURCE_EXTENSION_LAST "${SOURCE_EXTENSION_COUNT} - 1")
    foreach(index RANGE 0 ${SOURCE_EXTENSION_LAST})
        string(JSON extension_name GET "${SOURCE_PROFILE_JSON}" extensions ${index} name)
        string(JSON extension_hash GET "${SOURCE_PROFILE_JSON}" extensions ${index} hash)
        string(LENGTH "${extension_hash}" extension_hash_length)
        if(NOT extension_name MATCHES "^[A-Za-z0-9_.-]+$" OR
           NOT extension_hash_length EQUAL 8 OR
           NOT extension_hash MATCHES "^[0-9a-fA-F]+$")
            message(FATAL_ERROR "Invalid extension profile in ${SOURCE_PROFILE_PATH}")
        endif()
        string(APPEND SOURCE_EXTENSION_ENTRIES
            "    ExtensionProfile{\"${extension_name}\", 0x${extension_hash}u},\n")
    endforeach()
endif()

configure_file(
    loader/source_profile_data.inc.in
    generated/loader/source_profile_data.inc
    @ONLY
)

set(IMPORT_DESCRIPTOR_PATH "${CMAKE_CURRENT_SOURCE_DIR}/bridge/imports.json")
file(READ "${IMPORT_DESCRIPTOR_PATH}" IMPORT_DESCRIPTOR_JSON)

string(JSON IMPORT_SCHEMA_VERSION GET "${IMPORT_DESCRIPTOR_JSON}" schema_version)
if(NOT IMPORT_SCHEMA_VERSION EQUAL 1)
    message(FATAL_ERROR "Unsupported import descriptor schema in ${IMPORT_DESCRIPTOR_PATH}")
endif()
string(JSON SOURCE_IMPORT_COUNT LENGTH "${IMPORT_DESCRIPTOR_JSON}" imports)
if(SOURCE_IMPORT_COUNT LESS 1)
    message(FATAL_ERROR "Import descriptor has no imports: ${IMPORT_DESCRIPTOR_PATH}")
endif()
set(SOURCE_IMPORT_ENTRIES "")
set(SOURCE_IMPORT_INVENTORY "")
set(SOURCE_IMPORT_NAMES "")
math(EXPR SOURCE_IMPORT_LAST "${SOURCE_IMPORT_COUNT} - 1")
foreach(index RANGE 0 ${SOURCE_IMPORT_LAST})
    string(JSON import_name GET "${IMPORT_DESCRIPTOR_JSON}" imports ${index})
    if(NOT import_name MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "Invalid import name in ${IMPORT_DESCRIPTOR_PATH}: ${import_name}")
    endif()
    if(import_name IN_LIST SOURCE_IMPORT_NAMES)
        message(FATAL_ERROR "Duplicate import name in ${IMPORT_DESCRIPTOR_PATH}: ${import_name}")
    endif()
    list(APPEND SOURCE_IMPORT_NAMES "${import_name}")
    string(APPEND SOURCE_IMPORT_INVENTORY "${import_name}\n")
    string(APPEND SOURCE_IMPORT_ENTRIES "    \"${import_name}\",\n")
endforeach()
string(SHA256 SOURCE_IMPORT_SHA256 "${SOURCE_IMPORT_INVENTORY}")
string(JSON EXPECTED_IMPORT_SHA256 GET "${IMPORT_DESCRIPTOR_JSON}" source_inventory_sha256)
if(NOT SOURCE_IMPORT_SHA256 STREQUAL EXPECTED_IMPORT_SHA256)
    message(FATAL_ERROR "Import inventory digest does not match ${IMPORT_DESCRIPTOR_PATH}")
endif()

set(GENERATED_IMPORTS_PATH "${CMAKE_CURRENT_BINARY_DIR}/generated/bridge/generated_imports.hpp")
configure_file(
    bridge/generated_imports.hpp.in
    "${GENERATED_IMPORTS_PATH}"
    @ONLY
)

set(LOADER_SOURCES
    ${GENERATED_IMPORTS_PATH}
    gl/gl_compat.cpp
    loader/s3e_image.cpp
    loader/apk_reader.cpp
    loader/lzma.cpp
    loader/sha256.cpp
    loader/crc32.cpp
    loader/file_io.cpp
    loader/importer.cpp
    loader/source_profile.cpp
    bridge/import_inventory.cpp
    loader/guest_loader.cpp
    loader/dtrz_archive.cpp
    s3e/config.cpp
    s3e/control_config.cpp
    s3e/file_system.cpp
    game/codboz_player_name.cpp
)

set(GUEST_SOURCES
    guest/guest_memory.cpp
    guest/guest_heap.cpp
)

set(CPU_SOURCES
    cpu/dynarmic_cpu.cpp
    guest/guest_call.cpp
    bridge/dispatcher.cpp
    game/codboz_controller_bridge.cpp
    game/codboz_fixups.cpp
    game/codboz_frame_interpolation.cpp
    network/socket_system.cpp
)

if(ANDROID)
    list(APPEND CPU_SOURCES gl/gl_bridge_android.cpp)
else()
    list(APPEND CPU_SOURCES gl/gl_bridge_stub.cpp)
endif()

# Vendored LZMA SDK decoder (public domain). Compiled without -Werror.
add_library(lzma_sdk STATIC third_party/lzma/LzmaDec.c)
target_include_directories(lzma_sdk PUBLIC third_party/lzma)

# Project loader and guest model, with no Android dependencies.
add_library(codboz_loader STATIC ${LOADER_SOURCES} ${GUEST_SOURCES})
target_include_directories(codboz_loader PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(codboz_loader PUBLIC lzma_sdk codboz_sanitizers)

# robin-map predates CMake 3.5 compatibility, so this policy floor lets the
# vendored Dynarmic dependency configure under current CMake.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_NO_BINDINGS ON CACHE BOOL "" FORCE)
set(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT ON CACHE BOOL "" FORCE)

# Dynarmic links header-only Boost (variant, icl). Point FindBoost at the vendored
# subset so the build is hermetic (host and CI do not need a system Boost).
set(BOOST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost CACHE PATH "" FORCE)
set(Boost_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost/include CACHE PATH "" FORCE)
set(Boost_NO_SYSTEM_PATHS ON CACHE BOOL "" FORCE)
set(Boost_NO_BOOST_CMAKE ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/dynarmic EXCLUDE_FROM_ALL)

# Guest CPU bridge: Dynarmic A32 over GuestMemory with trap-based imports.
add_library(codboz_cpu STATIC ${CPU_SOURCES})
target_link_libraries(codboz_cpu
    PUBLIC codboz_loader dynarmic codboz_sanitizers
    PRIVATE merry::mcl
)
target_include_directories(codboz_cpu PRIVATE ${Boost_INCLUDE_DIR})

if(ANDROID)
    set(CODBOZ_ANDROID_COMPILE_OPTIONS
        -Wall
        -Wextra
        -fvisibility=hidden
        -fvisibility-inlines-hidden
        -fstack-protector-strong
        $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-D_FORTIFY_SOURCE=2>
    )
    target_compile_options(codboz_loader PRIVATE ${CODBOZ_ANDROID_COMPILE_OPTIONS})
    target_compile_options(codboz_cpu PRIVATE ${CODBOZ_ANDROID_COMPILE_OPTIONS})

    # EGL/GLES for the GL bridge.
    find_library(egl-lib EGL)
    find_library(glesv2-lib GLESv2)
    find_library(android-lib android)
    target_link_libraries(codboz_cpu PUBLIC ${egl-lib} ${glesv2-lib} ${android-lib})

    add_library(codboz_runtime SHARED
        audio/android_audio_backend.cpp
        network/android_nsd_backend.cpp
        runtime/runtime.cpp
        runtime/jni_bridge.cpp
    )
    target_include_directories(codboz_runtime PRIVATE runtime ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(codboz_runtime PRIVATE codboz_loader codboz_cpu)
    target_compile_options(codboz_runtime PRIVATE ${CODBOZ_ANDROID_COMPILE_OPTIONS})
    target_link_options(codboz_runtime PRIVATE
        -Wl,-z,relro
        -Wl,-z,now
        -Wl,-z,noexecstack
    )

    find_library(log-lib log)
    target_link_libraries(codboz_runtime PRIVATE ${log-lib})
else()
    target_compile_options(codboz_loader PRIVATE -Wall -Wextra -Werror -pedantic)
    target_compile_options(codboz_cpu PRIVATE -Wall -Wextra -Werror -pedantic)

    add_executable(s3e_inspect tools/s3e_inspect.cpp)
    target_link_libraries(s3e_inspect PRIVATE codboz_loader)

    add_executable(apk_inspect tools/apk_inspect.cpp)
    target_link_libraries(apk_inspect PRIVATE codboz_loader)

    enable_testing()
    add_subdirectory(tests)
endif()
