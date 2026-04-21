# Tarantool

[![Actions Status][actions-badge]][actions-url]
[![Code Coverage][coverage-badge]][coverage-url]
[![OSS Fuzz][oss-fuzz-badge]][oss-fuzz-url]
[![Telegram][telegram-badge]][telegram-url]
[![GitHub Discussions][discussions-badge]][discussions-url]
[![Stack Overflow][stackoverflow-badge]][stackoverflow-url]

[Tarantool][tarantool-url] is an in-memory computing platform consisting of a
database and an application server.

> This branch carries substantial SQL VDBE execution engine work:
>
> * **Generated threaded interpreter** — a DSL-driven code generator
>   (`tools/vdbe_codegen.py`) produces a fully-covered dispatch loop
>   (`src/box/sql/generated/`) from `tools/vdbe_dsl/opcodes.yaml`. All 142/142
>   opcodes are handled. Selectable at runtime via `VDBE_DISPATCHER=generated`.
>
> * **LLVM MCJIT** — an experimental ahead-of-time compiler that translates VDBE
>   programs to native code at prepare time using the LLVM ExecutionEngine API
>   (requires `ENABLE_SQL_JIT=ON`). 141/142 opcodes supported (OP_Program falls
>   back to the interpreter). Enabled at runtime via `SQL_JIT_ENABLE=1`.
>   Prepared-statement execution is `1.25x–1.58x` faster than the interpreter;
>   one-shot execution is dominated by compile cost.
>
> * **Copy-and-Patch (CnP) JIT** — a lightweight JIT that copies pre-compiled
>   opcode stencils and patches in runtime addresses, with no LLVM dependency.
>   All 142/142 opcodes covered. Selectable via `VDBE_DISPATCHER=cnp`.
>   Uses a shared 8 MB RWX arena to eliminate per-compile mmap overhead.
>
> * **Auto statement cache with JIT integration** — `box.execute()` caches the
>   last 256 prepared statements and triggers JIT compilation on cache insertion,
>   so repeated `box.execute()` calls for the same query use native code without
>   explicit `box.prepare()`.
>
> Benchmark results and methodology: [`tools/jit_bench/SQL_JIT_BENCHMARK.md`](tools/jit_bench/SQL_JIT_BENCHMARK.md).  
> Implementation plan and design notes: [`docs/sql-vdbe/COPY_AND_PATCH_PLAN.md`](docs/sql-vdbe/COPY_AND_PATCH_PLAN.md).

It is distributed under [BSD 2-Clause][license] terms.

Key features of the application server:

* Heavily optimized Lua interpreter with incredibly fast tracing JIT compiler,
  based on LuaJIT 2.1.
* Cooperative multitasking, non-blocking IO.
* [Persistent queues][queue].
* [Sharding][vshard].
* [Cluster and application management framework][cartridge].
* Access to external databases such as [MySQL][mysql] and [PostgreSQL][pg].
* A rich set of built-in and standalone [modules][modules].

Key features of the database:

* MessagePack data format and MessagePack based client-server protocol.
* Two data engines: 100% in-memory with complete WAL-based persistence and an
  own implementation of LSM-tree, to use with large data sets.
* Multiple index types: HASH, TREE, RTREE, BITSET.
* Document oriented JSON path indexes.
* Asynchronous master-master replication.
* Synchronous quorum-based replication.
* RAFT-based automatic leader election for the single-leader configuration.
* Authentication and access control.
* ANSI SQL, including views, joins, referential and check constraints.
* [Connectors][connectors] for many programming languages.
* The database is a C extension of the application server and can be turned
  off.

Supported platforms are Linux (x86_64, aarch64), Mac OS X (x86_64, M1), FreeBSD
(x86_64).

Tarantool is ideal for data-enriched components of scalable Web architecture:
queue servers, caches, stateful Web applications.

To download and install Tarantool as a binary package for your OS or using
Docker, please see the [download instructions][download].

To build Tarantool from source, see detailed [instructions][building] in the
Tarantool documentation.

To find modules, connectors and tools for Tarantool, check out our [Awesome
Tarantool][awesome-list] list.

Please report bugs to our [issue tracker][issue-tracker]. We also warmly
welcome your feedback on the [discussions][discussions-url] page and questions
on [Stack Overflow][stackoverflow-url].

We accept contributions via pull requests. Check out our [contributing
guide][contributing].

Thank you for your interest in Tarantool!

[actions-badge]: https://github.com/tarantool/tarantool/workflows/release/badge.svg
[actions-url]: https://github.com/tarantool/tarantool/actions
[coverage-badge]: https://coveralls.io/repos/github/tarantool/tarantool/badge.svg?branch=master
[coverage-url]: https://coveralls.io/github/tarantool/tarantool?branch=master
[telegram-badge]: https://img.shields.io/badge/Telegram-join%20chat-blue.svg
[telegram-url]: http://telegram.me/tarantool
[discussions-badge]: https://img.shields.io/github/discussions/tarantool/tarantool
[discussions-url]: https://github.com/tarantool/tarantool/discussions
[stackoverflow-badge]: https://img.shields.io/badge/stackoverflow-tarantool-orange.svg
[stackoverflow-url]: https://stackoverflow.com/questions/tagged/tarantool
[oss-fuzz-badge]: https://oss-fuzz-build-logs.storage.googleapis.com/badges/tarantool.svg
[oss-fuzz-url]: https://oss-fuzz.com/coverage-report/job/libfuzzer_asan_tarantool/latest
[tarantool-url]: https://www.tarantool.io/en/
[license]: LICENSE
[modules]: https://www.tarantool.io/en/download/rocks
[queue]: https://github.com/tarantool/queue
[vshard]: https://github.com/tarantool/vshard
[cartridge]: https://github.com/tarantool/cartridge
[mysql]: https://github.com/tarantool/mysql
[pg]: https://github.com/tarantool/pg
[connectors]: https://www.tarantool.io/en/download/connectors
[download]: https://www.tarantool.io/en/download/
[building]: https://www.tarantool.io/en/doc/latest/dev_guide/building_from_source/
[issue-tracker]: https://github.com/tarantool/tarantool/issues
[contributing]: CONTRIBUTING.md
[awesome-list]: https://github.com/tarantool/awesome-tarantool/
