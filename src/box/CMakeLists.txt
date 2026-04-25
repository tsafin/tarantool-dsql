file(MAKE_DIRECTORY ${PROJECT_BINARY_DIR}/src/box/lua)
file(MAKE_DIRECTORY ${PROJECT_BINARY_DIR}/src/box/sql)

# Sometimes 'spying' code is not acceptable even if it would be
# disabled by default. That option allows to remove the feedback
# daemon from the build completely.
option(ENABLE_FEEDBACK_DAEMON "Feedback daemon which reports debug data to the Tarantool team" ON)

set(lua_sources)
lua_source(lua_sources lua/load_cfg.lua load_cfg_lua)
lua_source(lua_sources lua/schema.lua schema_lua)
lua_source(lua_sources lua/tuple.lua tuple_lua)
lua_source(lua_sources lua/tuple_format.lua tuple_format_lua)
lua_source(lua_sources lua/session.lua session_lua)
if (ENABLE_FEEDBACK_DAEMON)
    lua_source(lua_sources lua/feedback_daemon.lua feedback_daemon_lua)
endif()
if(ENABLE_AUDIT_LOG)
    lua_source(lua_sources ${AUDIT_LOG_LUA_SOURCE} audit_lua)
endif()
if(ENABLE_SPACE_UPGRADE)
    lua_source(lua_sources ${SPACE_UPGRADE_LUA_SOURCE} space_upgrade_lua)
endif()
if(ENABLE_FLIGHT_RECORDER)
    lua_source(lua_sources ${FLIGHT_RECORDER_LUA_SOURCE} flightrec_lua)
endif()
if(ENABLE_READ_VIEW)
    lua_source(lua_sources ${READ_VIEW_LUA_SOURCE} read_view_lua)
endif()
if(ENABLE_SECURITY)
    lua_source(lua_sources ${SECURITY_LUA_SOURCE} security_lua)
endif()
if(ENABLE_INTEGRITY)
    lua_source(lua_sources ${INTEGRITY_LUA_SOURCE} integrity_lua)
endif()
lua_source(lua_sources lua/net_box.lua net_box_lua)
lua_source(lua_sources lua/upgrade.lua upgrade_lua)
lua_source(lua_sources lua/console.lua console_lua)
lua_source(lua_sources lua/xlog.lua xlog_lua)
lua_source(lua_sources lua/key_def.lua key_def_lua)
lua_source(lua_sources lua/merger.lua merger_lua)
lua_source(lua_sources lua/iproto.lua iproto_lua)
lua_source(lua_sources lua/mkversion.lua mkversion_lua)

# {{{ config
lua_source(lua_sources lua/config/applier/app.lua         config_applier_app_lua)
lua_source(lua_sources lua/config/applier/box_cfg.lua     config_applier_box_cfg_lua)
lua_source(lua_sources lua/config/applier/compat.lua      config_applier_compat_lua)
lua_source(lua_sources lua/config/applier/console.lua     config_applier_console_lua)
lua_source(lua_sources lua/config/applier/credentials.lua config_applier_credentials_lua)
lua_source(lua_sources lua/config/applier/fiber.lua       config_applier_fiber_lua)
lua_source(lua_sources lua/config/applier/mkdir.lua       config_applier_mkdir_lua)
lua_source(lua_sources lua/config/applier/roles.lua       config_applier_roles_lua)
lua_source(lua_sources lua/config/applier/sharding.lua    config_applier_sharding_lua)
lua_source(lua_sources lua/config/cluster_config.lua      config_cluster_config_lua)
lua_source(lua_sources lua/config/configdata.lua          config_configdata_lua)
lua_source(lua_sources lua/config/init.lua                config_init_lua)
lua_source(lua_sources lua/config/instance_config.lua     config_instance_config_lua)
lua_source(lua_sources lua/config/source/env.lua          config_source_env_lua)
lua_source(lua_sources lua/config/source/file.lua         config_source_file_lua)
lua_source(lua_sources lua/config/utils/aboard.lua        config_utils_aboard_lua)
lua_source(lua_sources lua/config/utils/expression.lua    config_utils_expression_lua)
lua_source(lua_sources lua/config/utils/file.lua          config_utils_file_lua)
lua_source(lua_sources lua/config/utils/log.lua           config_utils_log_lua)
lua_source(lua_sources lua/config/utils/schema.lua        config_utils_schema_lua)
lua_source(lua_sources lua/config/utils/snapshot.lua      config_utils_snapshot_lua)
lua_source(lua_sources lua/config/utils/tabulate.lua      config_utils_tabulate_lua)

if (ENABLE_CONFIG_EXTRAS)
    lua_source(lua_sources ${CONFIG_EXTRAS_DIR}/source/etcd.lua config_source_etcd_lua)
    lua_source(lua_sources ${CONFIG_EXTRAS_DIR}/storage/init.lua config_storage_init_lua)
    lua_source(lua_sources ${CONFIG_EXTRAS_DIR}/source/storage.lua config_source_storage_lua)
    lua_source(lua_sources ${CONFIG_EXTRAS_DIR}/extras.lua config_extras_lua)
endif()
# }}} config

if (ENABLE_FAILOVER)
    lua_multi_source(lua_sources ${FAILOVER_LUA_SOURCES})
endif()

set(bin_sources)
bin_source(bin_sources bootstrap.snap bootstrap.h bootstrap_bin)

check_c_source_compiles("
int main() {
  void *labels[] = {&&label}; goto *labels[0];
label:
  return 1;

  ;
  return 0;
}" CC_HAS_COMPUTED_GOTO)

if (CC_HAS_COMPUTED_GOTO)
    add_definitions(-DHAVE_COMPUTED_GOTO=1)
endif()

set(sql_sources
    sql/opcodes.c
    sql/parse.c
    sql/alter.c
    sql/cursor.c
    sql/build.c
    sql/delete.c
    sql/expr.c
    sql/func.c
    sql/global.c
    sql/hash.c
    sql/insert.c
    sql/main.c
    sql/malloc.c
    sql/mem.c
    sql/os.c
    sql/os_unix.c
    sql/parse_def.c
    sql/pragma.c
    sql/prepare.c
    sql/printf.c
    sql/random.c
    sql/resolve.c
    sql/port.c
    sql/select.c
    sql/show.c
    sql/tokenize.c
    sql/treeview.c
    sql/trigger.c
    sql/update.c
    sql/util.c
    sql/vdbe.c
    sql/vdbe_field_ref.c
    sql/vdbe_blob.c
    sql/vdbe_ops_arith.c
    sql/vdbe_ops_data.c
    sql/vdbe_ops_compare.c
    sql/vdbe_ops_control.c
    sql/vdbe_ops_logical.c
    sql/vdbe_ops_string.c
    sql/vdbe_ops_type.c
    sql/vdbe_ops_aggregate.c
    sql/vdbe_ops_functions.c
    sql/vdbe_ops_cursor_data.c
    sql/vdbe_ops_cursor_nav.c
    sql/vdbe_ops_cursor_seek.c
    sql/vdbe_ops_index.c
    sql/vdbe_ops_modify.c
    sql/vdbe_ops_limit.c
    sql/vdbe_ops_sorter.c
    sql/vdbe_ops_inline_simple.c
    sql/vdbe_ops_inline_medium_1.c
    sql/vdbe_ops_inline_medium_2.c
    sql/vdbe_ops_inline_medium_3.c
    sql/vdbe_ops_inline_medium_4.c
    sql/vdbe_ops_inline_medium_5.c
    sql/vdbe_ops_inline_medium_6.c
    sql/vdbe_ops_inline_medium_7.c
    sql/vdbe_ops_inline_medium_8.c
    sql/vdbe_dispatch_validate.c
    sql/vdbe_dispatch_wrapper.c
    sql/vdbe_helpers.c
    sql/vdbeapi.c
    sql/vdbeaux.c
    sql/vdbesort.c
    sql/vdbetrace.c
    sql/walker.c
    sql/where.c
    sql/wherecode.c
    sql/whereexpr.c
)

# Add JIT source file only when JIT is enabled
if(ENABLE_SQL_JIT)
    list(APPEND sql_sources sql/vdbe_jit.c)
    list(APPEND sql_sources sql/vdbe_jit_perf.cc)
    # vdbe_jit_perf.cc includes LLVM C++ headers that require C++17 with GCC 8.
    # Suppress -Wunused-parameter from LLVM system headers in debug builds.
    set_source_files_properties(sql/vdbe_jit_perf.cc PROPERTIES
        COMPILE_FLAGS "-std=c++17 -Wno-unused-parameter")
endif()

# Add CnP source file only when CnP is enabled
if(ENABLE_SQL_CNP)
    list(APPEND sql_sources sql/vdbe_cnp.c)
endif()

#
# SQL JIT: Generate LLVM bitcode for handler files
#
if(ENABLE_SQL_JIT)
    # List of handler files to compile to bitcode
    set(sql_jit_handler_defs
        -DSQL_TEST=1
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND sql_jit_handler_defs -DSQL_DEBUG=1)
    endif()
    set(sql_jit_handlers
        sql/vdbe_ops_arith.c
        sql/vdbe_ops_compare.c
        sql/vdbe_ops_logical.c
        sql/vdbe_ops_data.c
        sql/vdbe_ops_cursor_data.c
        sql/vdbe_ops_index.c
        sql/vdbe_ops_string.c
        sql/vdbe_ops_type.c
    )
    
    set(sql_jit_bitcode_files)
    
    # Generate bitcode for each handler file
    foreach(handler_source ${sql_jit_handlers})
        get_filename_component(handler_name ${handler_source} NAME_WE)
        set(bitcode_file "${PROJECT_BINARY_DIR}/src/box/${handler_name}.bc")
        
        add_custom_command(
            OUTPUT ${bitcode_file}
            COMMAND clang 
                -emit-llvm -c -O2
                -fno-emulated-tls
                ${sql_jit_handler_defs}
                -I${PROJECT_SOURCE_DIR}/src
                -I${PROJECT_SOURCE_DIR}/src/lib
                -I${PROJECT_SOURCE_DIR}/src/lib/core
                -I${PROJECT_SOURCE_DIR}/src/lib/small/include
                -I${PROJECT_SOURCE_DIR}/src/lib/small/third_party
                -I${PROJECT_SOURCE_DIR}/src/lib/msgpuck
                -I${PROJECT_SOURCE_DIR}/src/box
                -I${PROJECT_SOURCE_DIR}/third_party
                -I${PROJECT_SOURCE_DIR}/third_party/coro
                -I${PROJECT_SOURCE_DIR}/third_party/luajit/src
                -I${PROJECT_SOURCE_DIR}/third_party/xxHash
                -I${PROJECT_SOURCE_DIR}/third_party/zstd/lib
                -I${PROJECT_SOURCE_DIR}/third_party/zstd/lib/common
                -I${PROJECT_SOURCE_DIR}/third_party/libutil_freebsd
                -I${PROJECT_BINARY_DIR}/src
                -I${PROJECT_BINARY_DIR}/src/box
                -I${PROJECT_BINARY_DIR}/src/box/sql
                -I${PROJECT_BINARY_DIR}/src/lib/small/small/include
                -I${PROJECT_BINARY_DIR}/third_party
                -I${PROJECT_BINARY_DIR}/third_party/luajit/src
                -I${PROJECT_BINARY_DIR}/third_party/decNumber
                ${LLVM_DEFINITIONS}
                ${PROJECT_SOURCE_DIR}/src/box/${handler_source}
                -o ${bitcode_file}
            DEPENDS ${PROJECT_SOURCE_DIR}/src/box/${handler_source}
            COMMENT "Generating LLVM bitcode for ${handler_name}"
        )
        
        list(APPEND sql_jit_bitcode_files ${bitcode_file})
    endforeach()
    
    # Create target for all bitcode files
    add_custom_target(sql_jit_bitcode ALL
        DEPENDS ${sql_jit_bitcode_files}
    )
    
    message(STATUS "SQL JIT: Will generate bitcode for ${sql_jit_handlers}")
endif()

#
# SQL Copy-and-Patch: Build-time stencil extraction pipeline
#
if(ENABLE_SQL_CNP)
    # Require clang-16+ for stencil compilation
    find_program(CLANG_CNP clang-16 clang)
    if(NOT CLANG_CNP)
        message(FATAL_ERROR "ENABLE_SQL_CNP requires clang-16 (or clang)")
    endif()
    message(STATUS "SQL CnP: Using ${CLANG_CNP} for stencil compilation")

    set(CNP_GENERATED_DIR "${PROJECT_BINARY_DIR}/src/box/sql/generated")
    file(MAKE_DIRECTORY ${CNP_GENERATED_DIR})

    set(CNP_STUBS_C "${CNP_GENERATED_DIR}/vdbe_cnp_stubs.c")
    set(CNP_STUBS_O "${CNP_GENERATED_DIR}/vdbe_cnp_stubs.o")
    set(CNP_STENCILS_H "${CNP_GENERATED_DIR}/vdbe_cnp_stencils.h")
    set(CNP_FRAGMENTS_C "${CNP_GENERATED_DIR}/vdbe_cnp_fragments.c")
    set(CNP_FRAGMENTS_O "${CNP_GENERATED_DIR}/vdbe_cnp_fragments.o")
    set(CNP_FRAGMENTS_H "${CNP_GENERATED_DIR}/vdbe_cnp_fragments.h")
    set(CNP_GENSTUBS "${PROJECT_SOURCE_DIR}/tools/vdbe_cnp_genstubs.py")
    set(CNP_GENFRAGS "${PROJECT_SOURCE_DIR}/tools/vdbe_cnp_genfrags.py")
    set(CNP_EXTRACT "${PROJECT_SOURCE_DIR}/tools/vdbe_cnp_extract.py")
    set(CNP_EXTRACT_FRAGS
        "${PROJECT_SOURCE_DIR}/tools/vdbe_cnp_extract_fragments.py")
    set(CNP_OPCODES_YAML "${PROJECT_SOURCE_DIR}/tools/vdbe_dsl/opcodes.yaml")
    set(CNP_OPCODES_H "${PROJECT_BINARY_DIR}/src/box/sql/opcodes.h")

    # Include paths for stencil compilation (same as JIT handlers)
    set(CNP_INCLUDE_FLAGS
        -I${PROJECT_SOURCE_DIR}/src
        -I${PROJECT_SOURCE_DIR}/src/lib
        -I${PROJECT_SOURCE_DIR}/src/lib/core
        -I${PROJECT_SOURCE_DIR}/src/lib/small/include
        -I${PROJECT_SOURCE_DIR}/src/lib/small/third_party
        -I${PROJECT_SOURCE_DIR}/src/lib/msgpuck
        -I${PROJECT_SOURCE_DIR}/src/box
        -I${PROJECT_SOURCE_DIR}/src/box/sql
        -I${PROJECT_SOURCE_DIR}/third_party
        -I${PROJECT_SOURCE_DIR}/third_party/coro
        -I${PROJECT_SOURCE_DIR}/third_party/luajit/src
        -I${PROJECT_SOURCE_DIR}/third_party/xxHash
        -I${PROJECT_BINARY_DIR}/src
        -I${PROJECT_BINARY_DIR}/src/box
        -I${PROJECT_BINARY_DIR}/src/box/sql
        -I${PROJECT_BINARY_DIR}/src/lib/small/small/include
        -I${PROJECT_BINARY_DIR}/third_party
        -I${PROJECT_BINARY_DIR}/third_party/luajit/src
    )

    # Step 1: Generate stencil stubs from opcodes.yaml
    add_custom_command(
        OUTPUT ${CNP_STUBS_C}
        COMMAND ${CMAKE_COMMAND} -E env python3 ${CNP_GENSTUBS}
            --yaml ${CNP_OPCODES_YAML}
            --output ${CNP_STUBS_C}
        DEPENDS ${CNP_GENSTUBS} ${CNP_OPCODES_YAML}
        COMMENT "Generating CnP stencil stubs"
    )

    # Dedicated threaded fragment artifact for future extraction.
    add_custom_command(
        OUTPUT ${CNP_FRAGMENTS_C}
        COMMAND ${CMAKE_COMMAND} -E env python3 ${CNP_GENFRAGS}
            --output ${CNP_FRAGMENTS_C}
        DEPENDS ${CNP_GENFRAGS}
        COMMENT "Generating CnP fragment source"
    )

    # Step 2: Compile stubs to relocatable object
    add_custom_command(
        OUTPUT ${CNP_STUBS_O}
        COMMAND ${CLANG_CNP}
            -O2 -fno-pic -fno-asynchronous-unwind-tables -fno-stack-protector
            -fno-omit-frame-pointer
            ${CNP_INCLUDE_FLAGS}
            -c ${CNP_STUBS_C}
            -o ${CNP_STUBS_O}
        DEPENDS ${CNP_STUBS_C}
        COMMENT "Compiling CnP stencil stubs to object file"
    )

    add_custom_command(
        OUTPUT ${CNP_FRAGMENTS_O}
        COMMAND ${CLANG_CNP}
            -O2 -fPIC -fno-asynchronous-unwind-tables -fno-stack-protector
            -fno-omit-frame-pointer
            -D_GNU_SOURCE
            ${CNP_INCLUDE_FLAGS}
            -c ${CNP_FRAGMENTS_C}
            -o ${CNP_FRAGMENTS_O}
        DEPENDS ${CNP_FRAGMENTS_C}
        COMMENT "Compiling CnP fragment source to object file"
    )

    # Step 3: Extract stencils from object file
    add_custom_command(
        OUTPUT ${CNP_STENCILS_H}
        COMMAND python3 ${CNP_EXTRACT}
            --input ${CNP_STUBS_O}
            --output ${CNP_STENCILS_H}
            --opcodes-header ${CNP_OPCODES_H}
        DEPENDS ${CNP_STUBS_O} ${CNP_EXTRACT} ${CNP_OPCODES_H}
        COMMENT "Extracting CnP stencils from object file"
    )

    add_custom_command(
        OUTPUT ${CNP_FRAGMENTS_H}
        COMMAND python3 ${CNP_EXTRACT_FRAGS}
            --input ${CNP_FRAGMENTS_O}
            --output ${CNP_FRAGMENTS_H}
            --opcodes-header ${CNP_OPCODES_H}
        DEPENDS ${CNP_FRAGMENTS_O} ${CNP_EXTRACT_FRAGS} ${CNP_OPCODES_H}
        COMMENT "Extracting CnP threaded fragments from object file"
    )

    # Convenience target
    add_custom_target(cnp_stencils
        DEPENDS ${CNP_STENCILS_H}
    )

    add_custom_target(cnp_fragments
        DEPENDS ${CNP_FRAGMENTS_O}
    )

    add_custom_target(cnp_fragment_headers
        DEPENDS ${CNP_FRAGMENTS_H}
    )

    # Make sure stencils are built before the box library
    add_definitions(-DENABLE_SQL_CNP)
    message(STATUS "SQL CnP: Stencil pipeline enabled")
endif()

add_custom_target(box_generate_lua_sources
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}/src/box
    DEPENDS ${lua_sources})
set_property(DIRECTORY PROPERTY ADDITIONAL_MAKE_CLEAN_FILES ${lua_sources})

include_directories(${ZSTD_INCLUDE_DIRS})
include_directories(${PROJECT_BINARY_DIR}/src/box/sql)
include_directories(${PROJECT_BINARY_DIR}/src/box)
include_directories(${PROJECT_SOURCE_DIR}/src/box)
include_directories(${EXTRA_CORE_INCLUDE_DIRS})
include_directories(${EXTRA_BOX_INCLUDE_DIRS})

add_library(box_error STATIC error.cc errcode.c mp_error.cc)
target_link_libraries(box_error core stat mpstream vclock)

add_library(node_name STATIC node_name.c)

add_library(xrow STATIC xrow.c iproto_constants.c iproto_features.c)
target_link_libraries(xrow server core small vclock misc box_error node_name
                      ${MSGPUCK_LIBRARIES})

set(tuple_sources
    tuple.c
    field_map.c
    tuple_format.c
    tuple_format_map.c
    tuple_constraint_def.c
    tuple_constraint.c
    tuple_builder.c
    xrow_update.c
    xrow_update_field.c
    xrow_update_array.c
    xrow_update_bar.c
    xrow_update_route.c
    xrow_update_map.c
    tuple_compare.cc
    tuple_extract_key.cc
    tuple_hash.cc
    tuple_bloom.c
    tuple_dictionary.c
    key_def.c
    coll_id_def.c
    coll_id.c
    coll_id_cache.c
    field_def.c
    opt_def.c
    identifier.c
    mp_tuple.c
)

if(ENABLE_TUPLE_COMPRESSION)
    list(APPEND tuple_sources ${TUPLE_COMPRESSION_BOX_SOURCES})
endif()

add_library(tuple STATIC ${tuple_sources})
target_link_libraries(tuple json box_error core ${MSGPUCK_LIBRARIES} misc bit coll)

set(xlog_sources xlog.c)
if(ENABLE_RETENTION_PERIOD)
    list(APPEND xlog_sources ${RETENTION_PERIOD_SOURCES})
endif()

add_library(xlog STATIC ${xlog_sources})
target_link_libraries(xlog core box_error crc32 ${ZSTD_LIBRARIES})

set(box_sources
    allocator.cc
    memtx_allocator.cc
    msgpack.c
    iproto.cc
    xrow_io.cc
    tuple_convert.c
    index.cc
    index_def.c
    index_weak_ref.c
    iterator_type.c
    memtx_hash.cc
    memtx_tree.cc
    memtx_rtree.cc
    memtx_bitset.cc
    memtx_tx.c
    module_cache.c
    engine.c
    memtx_engine.cc
    memtx_space.c
    sysview.c
    sysalloc.c
    blackhole.c
    service_engine.c
    session_settings.c
    vinyl.c
    vy_stmt.c
    vy_mem.c
    vy_run.c
    vy_range.c
    vy_lsm.c
    vy_tx.c
    vy_write_iterator.c
    vy_read_iterator.c
    vy_point_lookup.c
    vy_cache.c
    vy_log.c
    vy_upsert.c
    vy_history.c
    vy_read_set.c
    vy_scheduler.c
    vy_regulator.c
    vy_quota.c
    request.c
    space.c
    space_cache.c
    space_def.c
    sequence.c
    func.c
    func_cache.c
    func_def.c
    field_default_func.c
    tuple_constraint_func.c
    tuple_constraint_fkey.c
    key_list.c
    alter.cc
    schema.cc
    schema_def.c
    session.c
    port.c
    txn.c
    txn_limbo.c
    txn_event_trigger.c
    raft.c
    box.cc
    gc.c
    checkpoint_schedule.c
    user_def.c
    user.cc
    authentication.c
    auth_chap_sha1.c
    replication.cc
    recovery.cc
    xstream.cc
    applier.cc
    relay.cc
    journal.c
    sql.c
    bind.c
    execute.c
    sql_stmt_cache.c
    wal.c
    call.c
    merger.c
    ibuf.c
    watcher.c
    decimal.c
    read_view.c
    mp_box_ctx.c
    ${sql_sources}
    ${lua_sources}
    lua/init.c
    lua/call.c
    lua/cfg.cc
    lua/console.c
    lua/lib.c
    lua/serialize_lua.c
    lua/tuple.c
    lua/slab.cc
    lua/index.c
    lua/space.cc
    lua/sequence.c
    lua/misc.cc
    lua/info.c
    lua/stat.c
    lua/ctl.c
    lua/error.cc
    lua/session.c
    lua/net_box.c
    lua/xlog.c
    lua/execute.c
    lua/key_def.c
    lua/merger.c
    lua/watcher.c
    lua/iproto.c
    lua/func_adapter.c
    lua/tuple_format.c
    lua/trigger.c
    lua/config/utils/expression_lexer.c
    ${bin_sources})

if(ENABLE_AUDIT_LOG)
    list(APPEND box_sources ${AUDIT_LOG_SOURCES})
endif()

if(ENABLE_SPACE_UPGRADE)
    list(APPEND box_sources ${SPACE_UPGRADE_SOURCES})
else()
    list(APPEND box_sources space_upgrade.c memtx_space_upgrade.c)
endif()

if(ENABLE_FLIGHT_RECORDER)
    list(APPEND box_sources ${FLIGHT_RECORDER_SOURCES})
endif()

if(ENABLE_WAL_EXT)
    list(APPEND box_sources ${WAL_EXT_SOURCES})
endif()

if(ENABLE_READ_VIEW)
    list(APPEND box_sources ${READ_VIEW_SOURCES})
endif()

if(ENABLE_SECURITY)
    list(APPEND box_sources ${SECURITY_SOURCES})
endif()

if(ENABLE_INTEGRITY)
    list(APPEND box_sources ${INTEGRITY_SOURCE})
endif()

if(ENABLE_MEMCS_ENGINE)
    list(APPEND box_sources ${MEMCS_ENGINE_SOURCES})
endif()

add_library(box STATIC ${box_sources})

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_definitions(-DSQL_DEBUG=1)
  add_definitions(-DSQL_VDBE_OP_PROFILE=1)
endif()
add_definitions(-DSQL_TEST=1)

if (ENABLE_VDBE_GOTO_DISPATCH)
  add_definitions(-DSQL_USE_GOTO=1)
endif()

if (VDBE_USE_GENERATED_DISPATCH)
  add_definitions(-DVDBE_USE_GENERATED_DISPATCH=1)
endif()

if(ENABLE_SQL_CNP)
    add_dependencies(box cnp_stencils)
endif()

set(EXT_SRC_DIR ${PROJECT_SOURCE_DIR}/extra)
set(EXT_BIN_DIR ${PROJECT_BINARY_DIR}/extra)
set(SQL_SRC_DIR ${PROJECT_SOURCE_DIR}/src/box/sql)
set(SQL_BIN_DIR ${PROJECT_BINARY_DIR}/src/box/sql)

include_directories(${SQL_SRC_DIR})
include_directories(${SQL_BIN_DIR})
set(VDBE_DSL ${PROJECT_SOURCE_DIR}/tools/vdbe_dsl/opcodes.yaml)
set(VDBE_CODEGEN ${PROJECT_SOURCE_DIR}/tools/vdbe_codegen.py)
set(VDBE_GEN_DIR ${SQL_BIN_DIR}/generated)
set(VDBE_GEN_C ${VDBE_GEN_DIR}/vdbe_dispatch_generated.c)
set(VDBE_GEN_H ${VDBE_GEN_DIR}/vdbe_opcodes_generated.h)

# Generate VDBE opcode sources from DSL
# Regenerate when opcodes.h changes (after mkopcodeh.sh runs) to keep IDs in sync
add_custom_command(
    OUTPUT ${VDBE_GEN_C} ${VDBE_GEN_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${VDBE_GEN_DIR}
    COMMAND python3 ${VDBE_CODEGEN} --in ${VDBE_DSL} --out ${VDBE_GEN_DIR}
    DEPENDS ${VDBE_CODEGEN} ${VDBE_DSL} ${SQL_BIN_DIR}/opcodes.h
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generating VDBE opcode headers and dispatch from DSL"
)

add_custom_target(vdbe_codegen DEPENDS ${VDBE_GEN_C} ${VDBE_GEN_H})
add_dependencies(vdbe_codegen generate_sql_files)
include_directories(${SQL_SRC_DIR}/generated)

#
# SQL JIT: Install bitcode files
#
if(ENABLE_SQL_JIT)
    install(FILES ${sql_jit_bitcode_files}
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/tarantool/sql_handlers)
endif()

# Generated dispatch code is included directly in vdbe.c, not compiled separately
# (Phase 5.3: kept in build for reference, will be integrated into vdbe.c)
# list(APPEND sql_sources ${VDBE_GEN_C})

add_custom_target(generate_sql_files DEPENDS
    sql/parse.h
    sql/keywordhash.h
    sql/dispatchtable.h
    sql/parse.y
    sql/parse.c
    sql/opcodes.h
    sql/opcodes.c)

add_custom_command(OUTPUT ${SQL_BIN_DIR}/keywordhash.h
    COMMAND ${EXT_BIN_DIR}/mkkeywordhash > keywordhash.h.tmp
    COMMAND ${CMAKE_COMMAND} -E copy_if_different keywordhash.h.tmp keywordhash.h
    COMMAND ${CMAKE_COMMAND} -E remove keywordhash.h.tmp
    WORKING_DIRECTORY "${SQL_BIN_DIR}"
    DEPENDS mkkeywordhash)

add_custom_command(OUTPUT ${SQL_BIN_DIR}/parse.h ${SQL_BIN_DIR}/parse.c
    COMMAND ${EXT_BIN_DIR}/lemon -T${EXT_SRC_DIR}/lempar.c -o${SQL_BIN_DIR}/parse.c ${SQL_SRC_DIR}/parse.y
    COMMAND ${CMAKE_COMMAND} -E copy parse.h parse.h.tmp
    COMMAND ${EXT_SRC_DIR}/addopcodes.sh parse.h.tmp > parse.h
    COMMAND ${CMAKE_COMMAND} -E remove parse.h.tmp parse.out
    WORKING_DIRECTORY "${SQL_BIN_DIR}"
    DEPENDS lemon ${SQL_SRC_DIR}/parse.y)

add_custom_command(OUTPUT ${SQL_BIN_DIR}/opcodes.h
    COMMAND cat parse.h ${SQL_SRC_DIR}/vdbe.c | ${EXT_SRC_DIR}/mkopcodeh.sh > opcodes.h
    WORKING_DIRECTORY "${SQL_BIN_DIR}"
    DEPENDS ${SQL_SRC_DIR}/vdbe.c ${EXT_SRC_DIR}/mkopcodeh.sh ${SQL_BIN_DIR}/parse.h)

add_custom_command(OUTPUT ${SQL_BIN_DIR}/opcodes.c
    COMMAND ${EXT_SRC_DIR}/mkopcodec.sh opcodes.h > opcodes.c
    WORKING_DIRECTORY "${SQL_BIN_DIR}"
    DEPENDS ${SQL_SRC_DIR}/vdbe.c ${EXT_SRC_DIR}/mkopcodec.sh ${SQL_BIN_DIR}/parse.h
        ${SQL_BIN_DIR}/opcodes.h)

add_custom_command(OUTPUT ${SQL_BIN_DIR}/dispatchtable.h
    COMMAND ${EXT_SRC_DIR}/mkdispatchtableh.sh opcodes.h > dispatchtable.h
    WORKING_DIRECTORY "${SQL_BIN_DIR}"
    DEPENDS ${SQL_SRC_DIR}/vdbe.c ${EXT_SRC_DIR}/mkdispatchtableh.sh ${SQL_BIN_DIR}/parse.h
            ${SQL_BIN_DIR}/opcodes.h)

target_link_libraries(box box_error tuple stat xrow xlog vclock crc32 raft
                      node_name ${common_libraries})

# Link LLVM libraries and set JIT compile definitions
if(ENABLE_SQL_JIT)
    target_link_libraries(box PUBLIC ${LLVM_LIBS})
    target_compile_definitions(box PRIVATE
        ENABLE_SQL_JIT
        SQL_JIT_BITCODE_DIR="${PROJECT_BINARY_DIR}/src/box"
    )
endif()

add_dependencies(box build_bundled_libs vdbe_codegen)
if(ENABLE_SQL_JIT)
    add_dependencies(box sql_jit_bitcode)
endif()
