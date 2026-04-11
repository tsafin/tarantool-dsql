include(utils)
include(ProcessorCount)

# Build a third-party project that uses autotools to generate build system.
# Basically it generates configure script, run configure and then make and
# rerun them according to their dependants in case of incremental build.
#
# DIR - relative path to project directory in main project tree.
# CONFIGURE - options for configure script.
# BYPRODUCTS - products caller targets depend upon.
#
# Note that incremental rebuild in case autotools files or config files are
# changed is available only if CMake version is at least 3.12. Otherwise
# rebuild will not be triggered. Rebuild is only required for developers
# which presumably has newer CMake versions.
function(ext_project_autotools name)
    set(sv_keywords
        DIR
    )
    set(mv_keywords
        CONFIGURE
        BYPRODUCTS
    )
    set(options
        ""
    )
    cmake_parse_arguments(
        ARGS "${options}" "${sv_keywords}" "${mv_keywords}" ${ARGN}
    )
    if (NOT DEFINED ARGS_DIR)
        message(FATAL_ERROR "DIR argument is mandatory")
    endif()

    # CONFIGURE_DEPENDS available since 3.12
    # VERSION_GREATER_EQUAL is only since 3.7
    if (${CMAKE_VERSION} VERSION_GREATER "3.12" OR
        ${CMAKE_VERSION} VERSION_EQUAL "3.12")
        set(autotool_files_standard
            configure.ac
            acinclude.m4
        )
        list_add_prefix(
            autotool_files_standard "${ARGS_DIR}/" autotool_files_standard
        )

        file(
            GLOB_RECURSE autotool_files_am
            CONFIGURE_DEPENDS
            RELATIVE "${PROJECT_SOURCE_DIR}"
            "${ARGS_DIR}/*.am"
        )

        set(autotool_files ${autotool_files_am} ${autotool_files_standard})

        file(
            GLOB_RECURSE config_files_in
            CONFIGURE_DEPENDS
            RELATIVE "${PROJECT_SOURCE_DIR}"
            "${ARGS_DIR}/*.in"
        )
    endif()

    set(autotools_source_dir "${PROJECT_SOURCE_DIR}/${ARGS_DIR}")
    set(autotools_build_dir "${PROJECT_BINARY_DIR}/${ARGS_DIR}")
    list_add_prefix(ARGS_BYPRODUCTS "${autotools_build_dir}/" byproducts)

    file(MAKE_DIRECTORY ${autotools_build_dir})

    add_custom_command(
        OUTPUT ${autotools_source_dir}/configure
        WORKING_DIRECTORY ${autotools_source_dir}
        COMMAND
            autoreconf -i
        COMMAND
            # autoreconf do not touch configure on all paths. For example if
            # only *.am are changed. We don't aim to follow precise dependency
            # path of autotools thus let's just update configure in this case.
            touch configure
        DEPENDS ${autotool_files}
    )

    add_custom_command(
        OUTPUT ${autotools_build_dir}/Makefile
        WORKING_DIRECTORY ${autotools_build_dir}
        COMMAND ${autotools_source_dir}/configure ${ARGS_CONFIGURE}
        DEPENDS ${autotools_source_dir}/configure
    )

    # Some *.in files are products of *.am files and some are not. For the
    # latter we don't need to run autoreconf && configure as it will be
    # overkill. We only need to run config.status.
    add_custom_command(
        OUTPUT
            ${autotools_build_dir}/config.log
        WORKING_DIRECTORY ${autotools_build_dir}
        COMMAND ./config.status
        DEPENDS
            ${config_files_in}
            # This dependency reflects dependency on config.status.
            # config.status and Makefile are both updated on configure run.
            ${autotools_build_dir}/Makefile
    )

    ProcessorCount(nproc)
    if (nproc)
        set(make make -j${nproc})
    else()
        set(make make)
    endif()

    add_custom_target(${name}
        WORKING_DIRECTORY ${autotools_build_dir}
        COMMAND
            ${make}
        DEPENDS
            ${autotools_build_dir}/Makefile
            ${autotools_build_dir}/config.log
        BYPRODUCTS
            ${byproducts}
    )
endfunction()
