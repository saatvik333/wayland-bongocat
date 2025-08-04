cmake_minimum_required(VERSION 3.24)
project(bongocat LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the build type." FORCE)
endif()

# TODO: add assets options and fully implement switching assets + optimization
#option(INCLUDE_DM_EMBEDDED_ASSETS "Include dm embedded assets (fully)" OFF)
#option(INCLUDE_DM20_EMBEDDED_ASSETS "Include dm20 embedded assets" OFF)
#option(INCLUDE_DMC_EMBEDDED_ASSETS "Include dmc embedded assets" OFF)
#option(INCLUDE_DMX_EMBEDDED_ASSETS "Include dmx embedded assets" OFF)
#option(INCLUDE_PEN20_EMBEDDED_ASSETS "Include pen20 embedded assets" OFF)

set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)
set(PROTOCOLS_DIR ${CMAKE_SOURCE_DIR}/protocols)
set(INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)
if(DEFINED ENV{WAYLAND_PROTOCOLS_DIR})
    set(WAYLAND_PROTOCOLS_DIR "$ENV{WAYLAND_PROTOCOLS_DIR}" CACHE PATH "Path to wayland-protocols")
else()
    set(WAYLAND_PROTOCOLS_DIR "/usr/share/wayland-protocols" CACHE PATH "Path to wayland-protocols")
endif()


set(CMAKE_CXX_STANDARD 23)
set(CMAKE_C_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)


set(PROTOCOL_XML_WLR ${PROTOCOLS_DIR}/wlr-layer-shell-unstable-v1.xml)
set(PROTOCOL_XML_XDG ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml)
set(PROTOCOL_XML_WLR_FOREIGN ${PROTOCOLS_DIR}/wlr-foreign-toplevel-management-unstable-v1.xml)
set(GENERATED_PROTOCOLS_SOURCES
    ${PROTOCOLS_DIR}/zwlr-layer-shell-v1-protocol.c
    ${PROTOCOLS_DIR}/xdg-shell-protocol.c
    ${PROTOCOLS_DIR}/wlr-foreign-toplevel-management-v1-protocol.c
)
set(GENERATED_PROTOCOLS_HEADERS
    ${PROTOCOLS_DIR}/zwlr-layer-shell-v1-client-protocol.h
    ${PROTOCOLS_DIR}/xdg-shell-client-protocol.h
    ${PROTOCOLS_DIR}/wlr-foreign-toplevel-management-v1-client-protocol.h
)
set(GENERATED_PROTOCOLS
    ${GENERATED_PROTOCOLS_SOURCES}
    ${GENERATED_PROTOCOLS_HEADERS}
)
add_custom_command(OUTPUT ${GENERATED_PROTOCOLS}
    COMMAND wayland-scanner client-header ${PROTOCOL_XML_XDG} ${PROTOCOLS_DIR}/xdg-shell-client-protocol.h
    COMMAND wayland-scanner private-code ${PROTOCOL_XML_XDG} ${PROTOCOLS_DIR}/xdg-shell-protocol.c
    COMMAND wayland-scanner client-header ${PROTOCOL_XML_WLR} ${PROTOCOLS_DIR}/zwlr-layer-shell-v1-client-protocol.h
    COMMAND wayland-scanner private-code ${PROTOCOL_XML_WLR} ${PROTOCOLS_DIR}/zwlr-layer-shell-v1-protocol.c
    COMMAND wayland-scanner private-code ${PROTOCOL_XML_WLR_FOREIGN} ${PROTOCOLS_DIR}/wlr-foreign-toplevel-management-v1-protocol.c
    COMMAND wayland-scanner client-header ${PROTOCOL_XML_WLR_FOREIGN} ${PROTOCOLS_DIR}/wlr-foreign-toplevel-management-v1-client-protocol.h
    DEPENDS ${PROTOCOL_XML_WLR} ${PROTOCOL_XML_XDG}
    COMMENT "Generating Wayland protocol files..."
)
add_custom_target(protocols DEPENDS ${GENERATED_PROTOCOLS})


add_executable(bongocat)
set(SOURCES
    ${SRC_DIR}/config/config.c
    ${SRC_DIR}/config/config_watcher.c
    ${SRC_DIR}/core/main.c
    ${SRC_DIR}/graphics/animation.c
    ${SRC_DIR}/graphics/embedded_assets.c
    ${SRC_DIR}/graphics/embedded_assets/bongocat.c
    ${SRC_DIR}/platform/input.c
    ${SRC_DIR}/platform/wayland.c
    ${SRC_DIR}/utils/error.c
    ${SRC_DIR}/utils/memory.c
    ${SRC_DIR}/utils/time.c
)
if (INCLUDE_DM_EMBEDDED_ASSETS)
    set(SOURCES ${SOURCES} "${SRC_DIR}/graphics/embedded_assets/dm.c")
    target_compile_definitions(bongocat PRIVATE FEATURE_INCLUDE_DM_EMBEDDED_ASSETS)
elseif (INCLUDE_DM20_EMBEDDED_ASSETS)
    set(SOURCES ${SOURCES} "${SRC_DIR}/graphics/embedded_assets/dm20.c")
    target_compile_definitions(bongocat PRIVATE FEATURE_INCLUDE_DM20_EMBEDDED_ASSETS)
elseif (INCLUDE_DMC_EMBEDDED_ASSETS)
    set(SOURCES ${SOURCES} "${SRC_DIR}/graphics/embedded_assets/dmc.c")
    target_compile_definitions(bongocat PRIVATE FEATURE_INCLUDE_DMC_EMBEDDED_ASSETS)
elseif (INCLUDE_DMX_EMBEDDED_ASSETS)
    set(SOURCES ${SOURCES} "${SRC_DIR}/graphics/embedded_assets/dmx.c")
    target_compile_definitions(bongocat PRIVATE FEATURE_INCLUDE_DMX_EMBEDDED_ASSETS)
elseif (INCLUDE_PEN20_EMBEDDED_ASSETS)
    set(SOURCES ${SOURCES} "${SRC_DIR}/graphics/embedded_assets/pen20.c")
    target_compile_definitions(bongocat PRIVATE FEATURE_INCLUDE_PEN20_EMBEDDED_ASSETS)
else()
    # Fallback digimon assets
    set(SOURCES ${SOURCES} "${SRC_DIR}/graphics/embedded_assets/min_dm.c")
    target_compile_definitions(bongocat PRIVATE FEATURE_INCLUDE_MIN_DM_EMBEDDED_ASSETS)
endif()

target_sources(bongocat PRIVATE ${SOURCES} ${GENERATED_PROTOCOLS_SOURCES})
add_dependencies(bongocat protocols)
target_include_directories(bongocat PRIVATE ${INCLUDE_DIR})
target_include_directories(bongocat SYSTEM PRIVATE ${PROTOCOLS_DIR} ${CMAKE_SOURCE_DIR}/lib)

set(WARNING_FLAGS -Wall -Wextra -Wpedantic -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition -Wredundant-decls -Wnested-externs -Wmissing-include-dirs -Wlogical-op -Wjump-misses-init -Wdouble-promotion -Wshadow)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(bongocat PRIVATE -fsanitize=address -fsanitize=undefined)
    set(EXTRA_LINK_FLAGS -fsanitize=address -fsanitize=undefined)
else()
    target_compile_definitions(bongocat PRIVATE _FORTIFY_SOURCE=2)
    target_compile_options(bongocat PRIVATE -flto -march=native -fomit-frame-pointer -funroll-loops -finline-functions)
    set(EXTRA_LINK_FLAGS -flto)
    #target_compile_definitions(bongocat PRIVATE DISABLE_MEMORY_STATISTICS)
endif()
target_compile_options(bongocat PRIVATE ${WARNING_FLAGS} -fstack-protector-strong)
target_link_libraries(bongocat PRIVATE wayland-client m rt pthread ${EXTRA_LINK_FLAGS})

install(TARGETS bongocat DESTINATION bin)
install(FILES bongocat.conf DESTINATION share/bongocat RENAME bongocat.conf.example)
install(PROGRAMS scripts/find_input_devices.sh DESTINATION bin RENAME bongocat-find-devices)

set_directory_properties(PROPERTIES ADDITIONAL_MAKE_CLEAN_FILES "${GENERATED_PROTOCOLS}")