# ── Core Engine — libsyncbar_core ────────────────────────────────────────────
#
# Compila como librería estática en macOS (.a) y como DLL en Windows (.dll).
# Todo el código aquí debe ser agnóstico del SO excepto dentro de los #ifdefs
# del HAL (RNF-02).

# ── SQLite3 amalgamation (vendor, sin dependencias externas) ─────────────────
# OBJECT: sus objetos quedan dentro de libsyncbar_core, así la app usa esta versión
# de SQLite y no la del sistema (-lsqlite3).
add_library(sqlite3 OBJECT
    vendor/sqlite3.c
)
target_include_directories(sqlite3 PUBLIC vendor)

# Flags específicos para SQLite (suprimir warnings del código de terceros)
if(MSVC)
    target_compile_options(sqlite3 PRIVATE /W0)
else()
    target_compile_options(sqlite3 PRIVATE -w)
endif()

# Activar WAL y threading seguro en SQLite
target_compile_definitions(sqlite3 PUBLIC
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_WAL_AUTOCHECKPOINT=1000
    SQLITE_ENABLE_COLUMN_METADATA=1
)

# ── Fuentes del Core ──────────────────────────────────────────────────────────
set(CORE_SOURCES
    src/database_mgr.cpp
    src/audio_engine.cpp
    src/bridge_api.cpp
)

# Ruteo de grupos (AudioRouter): Aggregate Device en macOS; nulo en el resto
if(APPLE)
    list(APPEND CORE_SOURCES src/router_coreaudio.cpp)
else()
    list(APPEND CORE_SOURCES src/router_null.cpp)
endif()

# HAL por plataforma (AudioHal): CoreAudio en macOS; nulo en el resto hasta WASAPI
if(APPLE)
    list(APPEND CORE_SOURCES src/hal_coreaudio.cpp)
else()
    list(APPEND CORE_SOURCES src/hal_null.cpp)
endif()

set(CORE_HEADERS
    include/database_mgr.h
    include/audio_engine.h
    include/audio_hal.h
    include/audio_router.h
    include/bridge_api.h
)

# ── Tipo de librería: estática en macOS, compartida en Windows ────────────────
if(APPLE)
    add_library(syncbar_core STATIC ${CORE_SOURCES} ${CORE_HEADERS})
elseif(WIN32)
    add_library(syncbar_core SHARED ${CORE_SOURCES} ${CORE_HEADERS})
    target_compile_definitions(syncbar_core PRIVATE SYNCBAR_BUILDING_DLL)
endif()

# ── Includes públicos y privados ──────────────────────────────────────────────
target_include_directories(syncbar_core
    PUBLIC  include
    PRIVATE src
)

# ── Enlace con SQLite y frameworks de plataforma ─────────────────────────────
target_link_libraries(syncbar_core
    PRIVATE sqlite3
)

if(APPLE)
    # CoreAudio + AudioToolbox para el HAL de macOS
    find_library(COREAUDIO_LIB CoreAudio REQUIRED)
    find_library(AUDIOTOOLBOX_LIB AudioToolbox REQUIRED)
    # CoreFoundation: hal_coreaudio.cpp usa CFString/CFRelease
    find_library(COREFOUNDATION_LIB CoreFoundation REQUIRED)
    target_link_libraries(syncbar_core PRIVATE
        ${COREAUDIO_LIB}
        ${AUDIOTOOLBOX_LIB}
        ${COREFOUNDATION_LIB}
    )
elseif(WIN32)
    # WASAPI vive en ole32 y mmdevapi
    target_link_libraries(syncbar_core PRIVATE
        ole32
        mmdevapi
    )
endif()

# ── Definiciones de compilación por plataforma ────────────────────────────────
if(APPLE)
    target_compile_definitions(syncbar_core PRIVATE SYNCBAR_PLATFORM_MACOS)
elseif(WIN32)
    target_compile_definitions(syncbar_core PRIVATE SYNCBAR_PLATFORM_WINDOWS)
endif()

# ── Propiedades de output ─────────────────────────────────────────────────────
set_target_properties(syncbar_core PROPERTIES
    OUTPUT_NAME "syncbar_core"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

message(STATUS "Core target : syncbar_core")
message(STATUS "Core type   : $<IF:$<BOOL:${APPLE}>,STATIC,SHARED>")

# ── Tests (solo en Debug) ─────────────────────────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_executable(test_database_mgr tests/test_database_mgr.cpp)
    target_link_libraries(test_database_mgr PRIVATE syncbar_core)
    target_include_directories(test_database_mgr PRIVATE include)

    add_executable(test_audio_logic tests/test_audio_logic.cpp)
    target_include_directories(test_audio_logic PRIVATE include)
    # audio_logic.h es header-only, no necesita linkear syncbar_core

    add_executable(test_audio_engine tests/test_audio_engine.cpp)
    target_link_libraries(test_audio_engine PRIVATE syncbar_core)
    target_include_directories(test_audio_engine PRIVATE include)
    if(APPLE)
        find_library(COREFOUNDATION_LIB CoreFoundation REQUIRED)
        target_link_libraries(test_audio_engine PRIVATE ${COREFOUNDATION_LIB})
    endif()

    add_executable(test_bridge_api tests/test_bridge_api.cpp)
    target_link_libraries(test_bridge_api PRIVATE syncbar_core)
    target_include_directories(test_bridge_api PRIVATE include)
    if(APPLE)
        target_link_libraries(test_bridge_api PRIVATE ${COREFOUNDATION_LIB})
    endif()

    # AudioEngine contra un HAL simulado (sin hardware)
    add_executable(test_engine_hal tests/test_engine_hal.cpp)
    target_link_libraries(test_engine_hal PRIVATE syncbar_core)
    target_include_directories(test_engine_hal PRIVATE include tests)

    # Ruteo de grupos con HAL y router simulados
    add_executable(test_engine_routing tests/test_engine_routing.cpp)
    target_link_libraries(test_engine_routing PRIVATE syncbar_core)
    target_include_directories(test_engine_routing PRIVATE include tests)

    # Registro en CTest: `ctest --test-dir build --output-on-failure`
    foreach(t test_database_mgr test_audio_logic test_audio_engine test_bridge_api test_engine_hal test_engine_routing)
        add_test(NAME ${t} COMMAND ${t})
        set_tests_properties(${t} PROPERTIES TIMEOUT 60)
    endforeach()
endif()
