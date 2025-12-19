Full explicit casts table:

Recent RFC "Consistent Lua/SQL types" (#6009) defined ideal explicit and implicit conversion table we would liek to have for all current and
future Taranool SQL types. 

This patchset start from explict conversion tables (and implicit 
to come soon). The ideal picture would be as below:

              | 0 | 1 | 2 | 4 | 5 | 6 | 7 | 3 | 9 |10 |11 |12 | 8 |
              +---+---+---+---+---+---+---+---+---+---+---+---+---+
 0.       any |   |   |   |   |   |   |   |   |   |   |   |   |   |
 1.  unsigned |   | Y | Y | Y | Y |   |   | Y |   |   |   |   | Y |
 2.    string |   | S | Y | S | S | S | Y | S |   |   |   |   | Y |
 4.    double |   | S | Y | Y | S |   |   | Y |   |   |   |   | Y |
 5.   integer |   | S | Y | Y | Y |   |   | Y |   |   |   |   | Y |
 6.   boolean |   |   | Y |   |   | Y |   |   |   |   |   |   | Y |
 7. varbinary |   |   | Y |   |   |   | Y |   |   |   |   |   | Y |
 3.    number |   | S | Y | Y | S |   |   | Y |   |   |   |   | Y |
 9.   decimal |   |   |   |   |   |   |   |   |   |   |   |   |   |
10.      uuid |   |   |   |   |   |   |   |   |   |   |   |   |   |
11.     array |   |   |   |   |   |   |   |   |   |   |   |   |   |
12.       map |   |   |   |   |   |   |   |   |   |   |   |   |   |
 8.    scalar |   | S | Y | S | S | S | S | S |   |   |   |   | Y |
              +---+---+---+---+---+---+---+---+---+---+---+---+---+

Please pay attention that we omit DECIMAL, UUID, SCALAR and MAP rows
and columns, as they not yet fully supported by Tarantool SQL. Once 
their support will be landed we will modify conversion table and 
tests (which we also committing here).

Only booleans changes
---------------------

We need to modify explicit casts table according to the RFC from /doc/rfc/5910-consistent-sql-lua-types.md. This patch introduces changes for
BOOLEAN, thus, for simplicity sake, we mark unchanged cells as '.'

Since now on BOOLEAN will be only compatible with itself and STRINGs
(and recursively with SCALAR, which includes both those types). We
remove all other possible combinations which are defined now, these
cells marked with '-'.

              | 0 | 1 | 2 | 4 | 5 | 6 | 7 | 3 | 9 |10 |11 |12 | 8 |
              +---+---+---+---+---+---+---+---+---+---+---+---+---+
 0.       any |   |   |   |   |   |   |   |   |   |   |   |   |   |
 1.  unsigned |   | . | . | . | . | - |   | . |   |   |   |   | . |
 2.    string |   | . | . | . | . | S | . | . |   |   |   |   | . |
 4.    double |   | . | . | . | . | - |   | . |   |   |   |   | . |
 5.   integer |   | . | . | . | . | - |   | . |   |   |   |   | . |
 6.   boolean |   | - | Y | - | - | Y |   |   |   |   |   |   | . |
 7. varbinary |   |   | . |   |   | - | . |   |   |   |   |   | . |
 3.    number |   | . | . | . | . | - |   | . |   |   |   |   | . |
 9.   decimal |   |   |   |   |   |   |   |   |   |   |   |   |   |
10.      uuid |   |   |   |   |   |   |   |   |   |   |   |   |   |
11.     array |   |   |   |   |   |   |   |   |   |   |   |   |   |
12.       map |   |   |   |   |   |   |   |   |   |   |   |   |   |
 8.    scalar |   | . | . | . | . | S | . | . |   |   |   |   | . |
              +---+---+---+---+---+---+---+---+---+---+---+---+---+

ANY
---

              | 0 | 1 | 2 | 4 | 5 | 6 | 7 | 3 | 9 |10 |11 |12 | 8 |
              +---+---+---+---+---+---+---+---+---+---+---+---+---+
 0.       any |   |   |   |   |   |   |   |   |   |   |   |   |   |
 1.  unsigned |   | . | . | . | . |   |   | . |   |   |   |   | Y |
 2.    string |   | . | . | . | . | . | . | . |   |   |   |   | Y |
 4.    double |   | . | . | . | . |   |   | . |   |   |   |   | Y |
 5.   integer |   | . | . | . | . |   |   | . |   |   |   |   | Y |
 6.   boolean |   |   | . |   |   | . |   |   |   |   |   |   | Y |
 7. varbinary |   |   | . |   |   |   | . |   |   |   |   |   | Y |
 3.    number |   | . | . | . | . |   |   | . |   |   |   |   | Y |
 9.   decimal |   |   |   |   |   |   |   |   |   |   |   |   |   |
10.      uuid |   |   |   |   |   |   |   |   |   |   |   |   |   |
11.     array |   |   |   |   |   |   |   |   |   |   |   |   |   |
12.       map |   |   |   |   |   |   |   |   |   |   |   |   |   |
 8.    scalar |   | . | . | . | . | . | . | . |   |   |   |   | Y |
              +---+---+---+---+---+---+---+---+---+---+---+---+---+
