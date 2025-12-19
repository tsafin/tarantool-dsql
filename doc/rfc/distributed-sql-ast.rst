Distributed SQL: the first step - AST for parser
================================================

-  **Status**: V2, still is in progress
-  **Start date**: 11-2020
-  **Authors**: Timur Safin <tsafin@tarantool.org>
-  **Issues**: #5484, #5485, #5486

Changes
-------
- v1 this version with changed plans and more concise data structure 
     definitions. Now we do not serialize to SQL for remote nodes,
     but really operate with AST and nested objects.
- v0 initial version


Summary
-------

There is preliminary decision that we try to approach distributed SQL as
next big thing which come in 2021. This is a long project, which will be
approached gradually. Here we will try to observe all necessary steps to
be done both in long term and short term period of time. Longer terms goals
will be described briefly, but shorter term goal (extracting of AST from 
SQL parser) will be given in more details, we could do it because of current
set of Tarantool capabilities available and having some PoC already developed.

Bear in mind, that for longer term goals, later version of this RFC will try 
to collect wider relevant information, showing some (quiet random) industry
precedents (but that the list of industry examples will neither be scientific,
nor complete). 


Vocabulary:
-----------

We use standard MapReduce vocabulary when we talks about roles of
cluster nodes involved to the SQL queries processing.

+---------------+-----------------------------------------------------+
| Router(s)     | The node which processes queries and send to the    |
|               | corresponding storage nodes for local processing.   |
|               | It combines/reduces resultant data and sends it     |
|               | back to client                                      |
+===============+=====================================================+
| Combiner(s)   | Depending on the aggregation complexity needs there |
|               | may be several intermediate nodes which combine     |
|               | (aggregate) intermedate data and send it back       |
+---------------+-----------------------------------------------------+
| Storage nodes |                                                     |
+---------------+-----------------------------------------------------+

 

Distributed SQL scenario
------------------------

Once we move from single node case to multiple node case for SQL
execution all kinds of intra-node data exchange arise. Once we get
original SQL to the router node, it's expected that router would
preparse SQL query, (massage them appropriately) and then send some
command data to storage nodes for their local execution.
 
**The question is** - what format of data should we send to storage node?

We might try to send to the storage node the compiled binary VDBE
byte-code, but it looks to be a bad idea for several reasons:

1. Vdbe is not yet frozen and due to the technology used (lemon parser
   with on the fly generation of constants) it might differ very
   much between various versions even for builds from the same branch.
   Though, if we have to, we could take some extra measures to stabilize
   values of tokens generated;

2. But bigger problem is - different data distribution on different shard 
   nodes in the cluster. Which may require different query plans used 
   for the same SQL query. If we would generate blindly the single 
   byte code for received SQL then we may degrade performance comparing
   to the case when bytecode would be generated locally, for each modes 
   separately, taking local heuristics.

So at the moment simpler approach would be more preferable:

-  We simple transfer (modified) SQL query string to each of shard node 
   involved;

-  Or we could transfer AST serialized to some kind of binary form;

We suggest, that in the 1st stage, take the simplest approach - transfer 
simple SQL query in their textual form.

Distributed SQL in the InterSystems IRIS
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

(This is preliminary information, without further verification)

InterSystems IRIS uses ECP network for connecting cluster nodes, and
and building distributed cache of data in shards. It employs full-mesh
topology between nodes involved, thus any other node could cache 
locally data of a any other remote shard data.

This mesh topology eventually simplifies execution of SQL join execution,
which may be invoked on router node, regardless of a data locality. 
Preparsed and analyzed SQL queries were distributed to cluster nodes
via the same ECP mesh network.

**Question:** What would happen on router for gigantic results?

If router should use ECP for caching of reduced data on the router
then question is - how to proceed it effeciently without consuming 
entire block data memory? Do they use pagine for partial merge or what?

This is important question - and we will ask InterSystems experts for
comments.

 

Distributed SQL in MemSQL/SingleStore
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

https://docs.singlestore.com/v7.1/introduction/how-memsql-works/
SingleStore has builtin mode for local debugging of 4 nodes /
(TBD)
 

Distributed SQL in MariaDB SkySQL
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

https://mariadb.com/products/skysql/
(TBD)

Cluster JOIN...

 
Distributed SQL in Yugabyte
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Yugabyte uses PostgreSQL language front-end in their SQL parser and
optimizer, but has extended that optimizer with cluster aware specifics
and has added distributed execution.

For row data consistency they use RAFT consensus protocol, but has 
modified classical RAFT for faster execution if multiple regions
involved, where one needs to know locality of a data for effecient
processing.

Cluster JOIN...


Mike Siomkin' distributed SQL PoC
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

There is working and deployed proof-of-concept project which has been
implemented by Mike Siomkin, and which implements distributed SQL query
concept using currently available Tarantool facilities. 

.. note::

   There are some obvious limitations though, but it further proves 
   the point that with relatively small efforts, restricted distributed
   SQL processing might be implemented in current Tarantool within
   relatively short time frame.

For preliminary parsing of SQL queries Mike' code is using SQLParser
LuaRocks (https://github.com/tarantool/sqlparser) module which is
wrapping HyRise SQL parser implemented in C++
(https://github.com/hyrise/sql-parser) for parsing given SQL queries, and
building abstract-syntax trees (AST).

The intermediate part between cluster controller at the Tarantool side
and SQL parser is gridql.lua module. This is gridql responsibility to
parse SQL, analyze resultant AST, and enrich it appropriately for
aggregate functions, and pagination support. *I.e. queries sent to
storage node will be different to the original SQL query*, and will be
different to the query executed by combiner/reducer node.

 

The used sql-parser module exports only 2 methods: parse(query), and
tostring(ast).

-  `sqlparser.parse(q)` uses ffi function parseSql, which wraps hyrise SQL
   parser mechanics and returns AST tree as ffi structure
   LuaSQLParseResult, which in turns, composed of series of
   LuaSQLStatement-based objects, which might be of various types
   (e.g. kStmtSelect,  kStmtImport,  kStmtInsert,  kStmtUpdate,
   kStmtDelete, etc.), each of them could be attributed different
   set of data, including LuaExpr lists of various kinds;

-  `sqlparser.tostring(ast)` stringifies the passed AST object;

Despite the fact that Hyrise SQL parser has *no knowledge about builtin
SQL functions* supported by Tarantool SQL, it's parsing facilities are
enough for AST tree traversal, because any known name is marked as 
identifier of function, which is a good enough to start of SQL processing
in gridql module.

Local SQL execution is being done using builtin Tarantool SQL engine,
thus such lack of functions knowledge is not a problem, iff we pass
transparently SQL query down to the node processor.

Hyrise knowns all kinds of SQL queries, but at the moment gridql modules
*supports only `SELECT`s*, and not handles any other kinds of requests
(i.e. `UPDATE`).

Unfortunately, gridql, at their current form, could not be published due
to heavy usage of customer specific virtual tables, but there are claims
that it's possible to generalize and simplify code,
so it might be used elsewhere beyond current context.
 

Long-term goals
---------------

So, having many industry precendents we see that ideally, for distributed
SQL we have to have:

-  Some kind of router accepts SQL query, and then preparses it to some
   kind of intermediate representation (AST);

-  Topology aware query planner analyses parsed query and having knowledge
   of data distribution it sends parsed "AST" subqueries to only those
   nodes, which has relevant data. If there is no data locality known
   then all cluster involved via Map-Combine-Reduce operation;

-  Query might be split into inner subqueries for which stages would be
   planned and executed separately;

-  If transactions are not read only then cluster wide transaction
   manager / conflict manager to be involved for 2PC mechanics
   coordination;

-  And it would be easier if distributed SQL module should work even
   for single-node config (with or without vshard involved) for
   simpler debugging purposes;

Timings for these long-term plans are not yet known, but at the moment
we believe that the nearest subjects should be:

1. SQL parser refactoring to saving AST (with serialization and
   deserialization if necessary);

2. And transaction/conflict manager should be extended with cluster
   wide transaction support, to make possible next steps of queries
   beyond simple `SELECT`s;

2nd item is not SQL-specific, and will be handled elsewhere separately, 
this RFC we will continue to talk about SQL only plans;
 

Short-term distributed SQL plan
-------------------------------

At the moment parser, byte-code generation, and query execution is
tightly coupled in SQL code in Tarantool. This is side-effect of SQLite
architecture largely inherited by Tarantool from SQLite parser. And such
close coupling might become a road-blocker for us in the longer term, when
we would have to go to different nodes.

If we properly split query parser and query execution logics we will 
simplify configuration, making it easier to approach distributed SQL.

-  So, for the 1st, obvious step - we would need to create a
   separate/builtin module tarantool-sqlparser which would wrap SQL
   parsing in `parse(query)` method in a fashion similar to Mike
   Siomkin' `sqlparser` module above;

-  The `sql.parse(query)` method would need to return AST data structures,
   exported via ffi.

   -  At the moment only SELECT and VIEW queries build AST during
      parsing, this is major limitation, which would require some extra
      refactoring later, but it's ok for the 1st stage.

   -  For the 2nd stage we would need to extend AST with more SQL
      statement types, e.g. UPDATE / DELETE.

   -  Worth to mention, that current AST structures as defined in the
      `sqlint.h` are quite similar to that used in Mike' sqlparser
      module - for more details see comparison of LuaDataTypes.h to
      sqlint.h in the Appendix A below;

-  As we build AST we may enrich returned AST nodes with information
   about builtin functions kinds and expression data types, specific
   for our SQL parser;

-  So in addition to currently available ways to run SQL queries via:

   -  direct `box.execute`,

   -  Or 2 step `box.prepare + box.execute`

   -  we would add `sql.parse` method, similar to `box.prepare`,
      which should be similarly executable via `box.prepare + box.execute`;

-  This refactoring with separation of parse step, should still maintain
   fully working SQL execution cycle, i.e. with minimum code
   modifications all relevant SQL queries should pass whole
   Tarantool SQL test suite. (This might apply only to SELECT
   queries, for stage 1);

 

Distributed testing scenario
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

-  Assumption is that decoupled sql.parse method, should be powerful
   enough to be able replace HyRise SQL parser in the gridql
   proof-of-concept code. Once being published it will be another
   indicator of intermediate success if it will be working with
   Tarantool SQL parser module. (Mike is planning to publish cleaned
   up code soon);

   -  There is no need though in gridql for anything beyond simple
      SELECTs, which makes possible to create 1st implementation
      without major refactorings in Tarantool SQL parser, having only
      current data structures and code;

   -  Thus addition of AST support for DELETE, INSERT, UPDATE statements
      will be done at stage #2, probably the next quarter, and it's
      not a goal for current plan;

   -  i.e. we start with only read-only (SELECT and VIEW) queries, and
      not support RW operations yet;

-  INSERT/UPDATE/DELETE queries will be done afterward, once we have
   distributed conflict manager and transaction managers implemented.
   And it's subject to coordinated efforts with Alexander Lyapunov team;

 

Appendix A - AST data structures
--------------------------------

+-----------------------------------------------+-------------------------------+
| ``StatementType::kStmtSelect``                | ``ast_type::AST_TYPE_SELECT`` |
+===============================================+===============================+
|.. code:: c                                    |.. code:: c                    |
|                                               |                               |
| typedef struct {                              | struct sql_parsed_ast {       |
|    bool isValid;                              | const char* sql_query;        |
|    char* errorMsg;                            | enum ast_type ast_type;       |
|    int errorLine;                             | bool keep_ast;                |
|    int errorColumn;                           | union {                       |
|    size_t statementCount;                     |   struct Expr *expr;          |
|    struct LuaSQLStatement** statements;       |   struct Select *select;      |
| } LuaSQLParserResult;                         |   struct sql_trigger *trigger;|
|                                               |  };                           |
|                                               | };                            |
|                                               |                               |
+-----------------------------------------------+-------------------------------+
|.. code:: c                                    |.. code:: c                    |
|                                               |                               |
| typedef struct LuaSelectStatement {           |  struct Select {              | 
|    struct LuaSQLStatement base;               |        ExprList *pEList;      |
|                                               |        u8 op;                 |
|    struct LuaTableRef* fromTable;             |        LogEst nSelectRow;     |
|    bool selectDistinct;                       |        u32 selFlags;          |
|                                               |        int iLimit, iOffset;   |
|    size_t selectListSize;                     |        char zSelName[12];     |
|    struct LuaExpr** selectList;               |        int addrOpenEphm[2];   |
|                                               |        SrcList *pSrc;         |
|    struct LuaExpr* whereClause;               |        Expr *pWhere;          |
|                                               |        ExprList *pGroupBy;    |
|    struct LuaGroupByDescription* groupBy;     |        Expr *pHaving;         |
|                                               |        ExprList *pOrderBy;    |
|    size_t setOperationCount;                  |        Select *pPrior;        |
|    struct LuaSetOperation** setOperations;    |        Select *pNext;         |
|                                               |        Expr *pLimit;          |
|    size_t orderCount;                         |        Expr *pOffset;         |
|    struct LuaOrderDescription** order;        |        With *pWith;           |
|                                               |  };                           |
|    size_t withDescriptionCount;               |                               |
|    struct LuaWithDescription**                |                               |
|           withDescriptions;                   |                               |
|    struct LuaLimitDescription* limit;         |                               |
| } LuaSelectStatement;                         |                               |
|                                               |                               |
+-----------------------------------------------+-------------------------------+


Appendix B - programmatic interfaces
------------------------------------

Exporting AST as cdata is easy to do, but in this case it will be 
inconvenient to manipulate AST nodes, before sending to cluster nodes.
So there is 2nd, more idiomatic approach suggested - to convert AST
to nested Lua object/tables. Which should simplify some massaging
and provide natural way to serialization to msgpack.

SYNOPSIS
~~~~~~~~

.. code-block:: lua

   local sql = require `sqlparser`

   -- parse, return ast, pass it back unmodified for execution

   local ast = sql.parse [[ select * from "table" where id > 10 limit 10 ]]
   assert(type(ast) == 'cdata')
   local ok = sql.execute(ast)

   -- free allocated memory, like box.unprepare but for ast
   sql.unparse(ast)

   -- raw access to cdata structures
   local cdata = ast.as_cdata()
   if cdata.ast_type == ffi.C.AST_TYPE_SELECT
      handle_select_stmt(cdata.select)
   end

   -- Lua access to structurs as Lua tables
   local tree = ast.as_table()
   if tree.type == AST_TYPE_SELECT
      handle_columns_list(tree.select.columns)
      handle_where_clause(tree.select.where)
      limit = tree.select.limit
   end
   -- massaging with tree data

   -- serialization
   local msgpack = require 'msgpack'
   local to_wire = msgpack.encode(tree)

   -- networking magics ...
   -- ... deserialization
   local table = msgpack.decode(from_wire)
   ast.set_tree(tree)
   sql.execute(ast)


