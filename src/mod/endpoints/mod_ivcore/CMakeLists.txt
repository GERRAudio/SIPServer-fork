cmake_minimum_required(VERSION 3.16)
project(mod_ivcore VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# FreeSWITCH paths
# Override on the command line:
#   cmake -DFS_PREFIX=/usr/local/freeswitch ..
# ---------------------------------------------------------------------------
set(FS_PREFIX "/usr/local/freeswitch"
    CACHE PATH "FreeSWITCH installation prefix")

set(FS_INCLUDE_DIR "${FS_PREFIX}/include/freeswitch"
    CACHE PATH "FreeSWITCH headers directory")

set(FS_LIB_DIR "${FS_PREFIX}/lib"
    CACHE PATH "FreeSWITCH library directory")

set(FS_MOD_DIR "${FS_PREFIX}/mod"
    CACHE PATH "FreeSWITCH modules directory (install target)")

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
set(MOD_SOURCES
    mod_ivcore.cpp
    ivp_transport.cpp
)

# ---------------------------------------------------------------------------
# Shared library target (.so)
# FreeSWITCH modules are plain shared libraries loaded at runtime.
# ---------------------------------------------------------------------------
add_library(mod_ivcore MODULE ${MOD_SOURCES})

# No "lib" prefix — FreeSWITCH expects "mod_ivcore.so"
set_target_properties(mod_ivcore PROPERTIES PREFIX "")

target_include_directories(mod_ivcore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FS_INCLUDE_DIR}
)

target_compile_options(mod_ivcore PRIVATE
    -Wall -Wextra -O2 -fPIC
)

target_compile_definitions(mod_ivcore PRIVATE
    SWITCH_API_VERSION=1
)

# Link against the FreeSWITCH core library
find_library(FREESWITCH_LIB
    NAMES freeswitch
    PATHS ${FS_LIB_DIR}
    NO_DEFAULT_PATH
)

if(NOT FREESWITCH_LIB)
    message(WARNING "libfreeswitch not found in ${FS_LIB_DIR} — "
                    "set FS_LIB_DIR or FS_PREFIX appropriately. "
                    "Continuing without explicit link (may work if FS headers are in RPATH).")
else()
    target_link_libraries(mod_ivcore PRIVATE ${FREESWITCH_LIB})
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS mod_ivcore
    LIBRARY DESTINATION ${FS_MOD_DIR}
)

install(FILES conf/ivcore.conf.xml
    DESTINATION "${FS_PREFIX}/conf/autoload_configs"
    OPTIONAL
)

# ---------------------------------------------------------------------------
# Convenience message
# ---------------------------------------------------------------------------
message(STATUS "mod_ivcore build configuration:")
message(STATUS "  FS_PREFIX     = ${FS_PREFIX}")
message(STATUS "  FS_INCLUDE    = ${FS_INCLUDE_DIR}")
message(STATUS "  FS_LIB        = ${FS_LIB_DIR}")
message(STATUS "  Install to    = ${FS_MOD_DIR}")
message(STATUS "")
message(STATUS "After 'cmake --build . --target install' add to modules.conf.xml:")
message(STATUS "  <load module=\"mod_ivcore\"/>")
