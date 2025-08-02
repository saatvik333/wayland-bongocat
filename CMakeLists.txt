cmake_minimum_required(VERSION 3.24)
project(bongocat LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the build type." FORCE)
endif()

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


set(SOURCES
    ${SRC_DIR}/config/config.c
    ${SRC_DIR}/config/config_watcher.c
    ${SRC_DIR}/core/main.c
    ${SRC_DIR}/graphics/animation.c
    ${SRC_DIR}/graphics/embedded_assets.c
    ${SRC_DIR}/graphics/embedded_assets_bongocat.c
    ${SRC_DIR}/platform/input.c
    ${SRC_DIR}/platform/wayland.c
    ${SRC_DIR}/utils/error.c
    ${SRC_DIR}/utils/memory.c
)
add_executable(bongocat ${SOURCES} ${GENERATED_PROTOCOLS_SOURCES})
add_dependencies(bongocat protocols)
target_include_directories(bongocat PRIVATE ${INCLUDE_DIR})
target_include_directories(bongocat SYSTEM PRIVATE ${PROTOCOLS_DIR} ${CMAKE_SOURCE_DIR}/lib)

set(WARNING_FLAGS -Wall -Wextra -Wpedantic -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition -Wredundant-decls -Wnested-externs -Wmissing-include-dirs -Wlogical-op -Wjump-misses-init -Wdouble-promotion -Wshadow)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(bongocat PRIVATE ${WARNING_FLAGS} -fsanitize=address -fsanitize=undefined)
    set(EXTRA_LINK_FLAGS -fsanitize=address -fsanitize=undefined)
else()
    target_compile_definitions(bongocat PRIVATE _FORTIFY_SOURCE=2)
    target_compile_options(bongocat PRIVATE ${WARNING_FLAGS} -flto -march=native -fomit-frame-pointer -funroll-loops -finline-functions -fstack-protector-strong)
    set(EXTRA_LINK_FLAGS -flto)
endif()
target_link_libraries(bongocat PRIVATE wayland-client m pthread ${EXTRA_LINK_FLAGS})

install(TARGETS bongocat DESTINATION bin)
install(FILES bongocat.conf DESTINATION share/bongocat RENAME bongocat.conf.example)
install(PROGRAMS scripts/find_input_devices.sh DESTINATION bin RENAME bongocat-find-devices)

set_directory_properties(PROPERTIES ADDITIONAL_MAKE_CLEAN_FILES "${GENERATED_PROTOCOLS}")