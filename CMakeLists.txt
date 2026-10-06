cmake_minimum_required(VERSION 3.29)

project(SharedStashPageText VERSION 0.1.22 LANGUAGES CXX RC)

include(FetchContent)

set(D2RLPLUGIN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(D2RLPLUGIN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    D2RLoaderPluginSDK
    GIT_REPOSITORY https://github.com/D2RLoader/PluginSDK.git
    GIT_TAG 6eb8f8b6192868214706bd6d528c5294f2f551b7
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(D2RLoaderPluginSDK)

add_library(shared_stash_page_text SHARED
    src/plugin.cpp
    src/plugin.rc
)
target_link_libraries(shared_stash_page_text PRIVATE D2RLPlugin::D2RLPlugin user32 gdi32)
target_compile_features(shared_stash_page_text PRIVATE cxx_std_20)
target_compile_definitions(shared_stash_page_text PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
)
set_target_properties(shared_stash_page_text PROPERTIES
    OUTPUT_NAME "d2rl-shared-stash-page-text"
    PREFIX ""
)

set(embedded_config "${CMAKE_CURRENT_BINARY_DIR}/d2rplugin/shared_stash_page_text/default-config.toml")
get_filename_component(embedded_config_dir "${embedded_config}" DIRECTORY)
file(MAKE_DIRECTORY "${embedded_config_dir}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/config/shared-stash-page-text.toml"
    "${embedded_config}"
    COPYONLY
)
set(default_config_header_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${default_config_header_dir}")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/config/shared-stash-page-text.toml" default_config_text)
string(HEX "${default_config_text}" default_config_hex)
string(LENGTH "${default_config_hex}" default_config_hex_length)
set(default_config_byte_literals "")
set(default_config_offset 0)
while(default_config_offset LESS default_config_hex_length)
    math(EXPR default_config_remaining "${default_config_hex_length} - ${default_config_offset}")
    if(default_config_remaining GREATER 512)
        set(default_config_chunk_length 512)
    else()
        set(default_config_chunk_length "${default_config_remaining}")
    endif()
    string(SUBSTRING "${default_config_hex}" ${default_config_offset} ${default_config_chunk_length} default_config_hex_chunk)
    string(REGEX REPLACE "(..)" "0x\\1, " default_config_chunk_literals "${default_config_hex_chunk}")
    string(APPEND default_config_byte_literals "    ${default_config_chunk_literals}\n")
    math(EXPR default_config_offset "${default_config_offset} + ${default_config_chunk_length}")
endwhile()
file(WRITE "${default_config_header_dir}/shared_stash_default_config.hpp"
    "constexpr unsigned char DefaultConfigBytes[] = {\n${default_config_byte_literals}    0x00\n};\n"
    "const std::string DefaultConfig(reinterpret_cast<const char*>(DefaultConfigBytes), sizeof(DefaultConfigBytes) - 1);\n"
)
target_include_directories(shared_stash_page_text PRIVATE "${default_config_header_dir}")
file(TO_CMAKE_PATH "${embedded_config}" embedded_config_resource_path)
set(config_resource "${embedded_config_dir}/default-config.rc")
file(WRITE "${config_resource}"
    "#include <D2RLPlugin/resource.h>\nD2RL_PLUGIN_CONFIG_RESOURCE_ID RCDATA \"${embedded_config_resource_path}\"\n"
)
set_property(SOURCE "${config_resource}" APPEND PROPERTY OBJECT_DEPENDS "${embedded_config}")
target_sources(shared_stash_page_text PRIVATE "${config_resource}")

if(MSVC)
    target_compile_options(shared_stash_page_text PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_options(shared_stash_page_text PRIVATE /INCREMENTAL:NO /Brepro)
endif()
