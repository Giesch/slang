#
# setup the standard module configuration
#

#
# Neural module configuration
# SLANG_STANDARD_MODULE_DIR_NAME is the output directory name for all the standard modules
set(SLANG_STANDARD_MODULE_DIR_NAME "slang-standard-module-${SLANG_VERSION_NUMERIC}" CACHE STRING "Directory name for standard module")

# SLANG_NEURAL_MODULE_FILE_NAME is the file name for the neural module
set(SLANG_NEURAL_MODULE_FILE_NAME "neural.slang-module" CACHE STRING "module file name for neural module")

# SLANG_WORKGRAPH_MODULE_FILE_NAME is the file name for the experimental workgraph module
set(SLANG_WORKGRAPH_MODULE_FILE_NAME "workgraph.slang-module" CACHE STRING "module file name for workgraph module")

# Determine the library directory based on platform
# On Windows: shared libraries (.dll) go to bin/
# On Linux/Mac: shared libraries (.so/.dylib) go to lib/
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(slang_library_dir "bin")
else()
    set(slang_library_dir "lib")
endif()

# Set install directory using the platform-specific library directory
set(SLANG_STANDARD_MODULE_INSTALL_DIR "${slang_library_dir}/${SLANG_STANDARD_MODULE_DIR_NAME}" CACHE STRING "Installation directory for standard modules")

#
# Generate the standard module configuration header, this is used by the slang compiler to find the standard module
#

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/slang-standard-module-config.h.in
    slang-standard-module-config-header/slang-standard-module-config.h
)

# Create a target to ensure the standard module config header is generated
add_custom_target(
    generate_standard_module_config_header
    DEPENDS
        "${CMAKE_CURRENT_BINARY_DIR}/slang-standard-module-config-header/slang-standard-module-config.h"
)
set_target_properties(generate_standard_module_config_header PROPERTIES FOLDER "generated")

#
# Embedding the compiled modules into the slang library
#

# Resolve the host slang-embed tool. When cross-compiling, SLANG_GENERATORS_PATH
# holds host-platform tools; otherwise the slang-embed target from this build is
# used.
if(SLANG_GENERATORS_PATH)
    if(CMAKE_HOST_WIN32)
        set(CMAKE_HOST_EXECUTABLE_SUFFIX ".exe")
    else()
        set(CMAKE_HOST_EXECUTABLE_SUFFIX "")
    endif()
    set(SLANG_EMBED_TOOL
        "${SLANG_GENERATORS_PATH}/slang-embed${CMAKE_HOST_EXECUTABLE_SUFFIX}"
    )
    set(SLANG_EMBED_TOOL_DEPENDENCY)
else()
    set(SLANG_EMBED_TOOL slang-embed)
    set(SLANG_EMBED_TOOL_DEPENDENCY slang-embed)
endif()

# Generate a header holding the bytes of a compiled `.slang-module` file as an
# array initializer list, so the slang library can embed the module.
#
# `module_file` is the `.slang-module` to embed, `header_name` is the file name
# of the header under the current binary directory, and `target_name` names the
# custom target that produces the header.
function(
    slang_add_embedded_standard_module_header
    module_file
    header_name
    target_name
)
    set(header "${CMAKE_CURRENT_BINARY_DIR}/${header_name}")
    add_custom_command(
        OUTPUT ${header}
        COMMAND ${SLANG_EMBED_TOOL} -binary ${module_file} ${header}
        DEPENDS ${module_file} ${SLANG_EMBED_TOOL_DEPENDENCY}
        VERBATIM
    )
    add_custom_target(${target_name} DEPENDS ${header})
    set_target_properties(${target_name} PROPERTIES FOLDER generated)
endfunction()

# Build the neural module
add_subdirectory(neural)

# Build experimental modules
add_subdirectory(experimental)

# The slang library links exactly one of these two object libraries, selected by
# SLANG_EMBED_STANDARD_MODULES. The embedded variant carries the module bytes;
# the other returns nullptr for every module.
set(embedded_standard_modules_common_args
    .
    OBJECT
    EXCLUDE_FROM_ALL
    EXPORT_MACRO_PREFIX
    SLANG
    EXPORT_TYPE_AS
    ${SLANG_LIB_TYPE}
    FOLDER
    generated
    LINK_WITH_PRIVATE
    core
    USE_EXTRA_WARNINGS
    INCLUDE_DIRECTORIES_PRIVATE
    ${slang_SOURCE_DIR}/include
    EXPLICIT_SOURCE
    ./slang-embedded-standard-modules.cpp
)

if(SLANG_EMBED_STANDARD_MODULES)
    add_custom_target(generate_standard_module_headers)
    add_dependencies(
        generate_standard_module_headers
        slang-neural-module-header
        slang-workgraph-module-header
    )
    set_target_properties(
        generate_standard_module_headers
        PROPERTIES FOLDER generated
    )

    slang_add_target(
        ${embedded_standard_modules_common_args}
        TARGET_NAME slang-embedded-standard-modules
        REQUIRES generate_standard_module_headers
        EXTRA_COMPILE_DEFINITIONS_PRIVATE SLANG_EMBED_STANDARD_MODULES
        INCLUDE_DIRECTORIES_PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR}/neural
            ${CMAKE_CURRENT_BINARY_DIR}/experimental
    )
endif()

slang_add_target(
    ${embedded_standard_modules_common_args}
    TARGET_NAME slang-no-embedded-standard-modules
)
