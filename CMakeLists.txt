cmake_minimum_required(VERSION 3.21)

project(SyncBarAudio
    VERSION 1.0.0
    DESCRIPTION "SyncBar Audio — Shared Core Engine"
    LANGUAGES C CXX
)

# ── Estándar C++17 obligatorio ────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Configuración de build types ──────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

message(STATUS "Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "System     : ${CMAKE_SYSTEM_NAME}")
message(STATUS "Processor  : ${CMAKE_SYSTEM_PROCESSOR}")

# ── Validación de plataforma soportada ────────────────────────────────────────
if(NOT (APPLE OR WIN32))
    message(FATAL_ERROR "SyncBar Audio only supports macOS and Windows.")
endif()

# ── Flags de compilación por plataforma ──────────────────────────────────────
if(APPLE)
    # Apple Silicon + x86_64 universal binary (opcional, activar con -DUNIVERSAL=ON)
    option(UNIVERSAL "Build universal binary (arm64 + x86_64)" OFF)
    if(UNIVERSAL)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
        message(STATUS "Universal binary: arm64 + x86_64")
    endif()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
endif()

if(MSVC)
    add_compile_options(/W4 /WX /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# ── Subdirectorios ────────────────────────────────────────────────────────────
add_subdirectory(core)
