Branch - https://github.com/tarantool/tarantool/tree/tsafin/gh-4470-explicit-implicit-conversions-V2

This patchset containes of 2 major part and 1 bonus. Major parts
are changes in explicit and implicit conversion tables, with all
accompanying tests we have so far (and 1 extensive test introduced).
The bonus part is for improvements in the testing system tap.lua
which became apparent during debugging of this patches.

Relates to #5910, #6009
Part of #4470
Fixes #6010

Explicit table
==============

As we discovered harder way, there is no need to introduce so much changes
to the current explicit conversion table, because it's mostly compliant
already:
- most of changes we had to do was about `BOOLEAN` conversions, which are
  stricter now than before;
- there are addition of to `ANY` conversion, which we have decided to make
  behaving similar to `SCALAR` conversions;

Those changes could be visualized via this table:


                  | 0 | 1 | 2 | 4 | 5 | 6 | 7 | 3 | 9 |10 |11 |12 | 8 |
                  +---+---+---+---+---+---+---+---+---+---+---+---+---+
     0.       any |   |   |   |   |   |   |   |   |   |   |   |   |   |
     1.  unsigned |   | . | . | . | . | - |   | . |   |   |   |   | Y |
     2.    string |   | . | . | . | . | S | . | . |   |   |   |   | Y |
     4.    double |   | . | . | . | . | - |   | . |   |   |   |   | Y |
     5.   integer |   | . | . | . | . | - |   | . |   |   |   |   | Y |
     6.   boolean |   | - | Y | - | - | Y |   |   |   |   |   |   | Y |
     7. varbinary |   |   | . |   |   | - | . |   |   |   |   |   | Y |
     3.    number |   | . | . | . | . | - |   | . |   |   |   |   | Y |
     9.   decimal |   |   |   |   |   |   |   |   |   |   |   |   |   |
    10.      uuid |   |   |   |   |   |   |   |   |   |   |   |   |   |
    11.     array |   |   |   |   |   |   |   |   |   |   |   |   |   |
    12.       map |   |   |   |   |   |   |   |   |   |   |   |   |   |
     8.    scalar |   | . | . | . | . | S | . | . |   |   |   |   | Y |
                  +---+---+---+---+---+---+---+---+---+---+---+---+---+

This table above represent all possible combinations of explicit cast
from type (row) to another type (column). Numbering of types might look
confusing (it's not sequential) but the order is what we see in the
consistency specification RFC, and we should live with that. Values are
actual enum values from the code. Unfortunately, changing of order is
almost impossible because of massive rework for the spec it would require.

How to interpret this table, e.g.:
- for the explicit cast from `BOOLEAN` (6) to `BOOLEAN` (6) we should
  always allow cast (and make it noop), thus intersection is `Y` (yes);

- `STRING` (2) to `BOOLEAN` (6) may work sometimes (if string represents
  TRUE or FALSE literals), but may fail otherwise, thus there is `S` (sometimes);

- We have prohibited majority of ther directions for `BOOLEAN` thus there are
  `-` (minus) in such cells, i.e. `BOOLEAN` (6) to `INTEGER` (5);

- Untouched entries in the table marked with '.'. Assumption is -
  we already have correct conversion rule here activated well before;

- Worth to mention that empty cell means that this conversion
  direction is prohibited.

For obvious reasons those chnaged conversion rules made us modify 
expected results for many tests - they are fixed.

But to cover all possible conversion combinations (minus those, not yet
implemented in SQL types like DECIMAL, ARRAY and MAP) we have created
special test which check _all combinations of CASTs()_ and verifies
results against table rules defined in the RFC.

There is e_casts.test.lua created for that purpose.

Implicit table
==============

Similarly to explicit conversion rules we have update implicit conversion
rules, and this is 2nd part of patchset.

We will not draw changed values of implicit conversion table (please see
it's final state in the consistemcy types RFC), but verbally we have 
modified following directions of conversions:
  - string to/from double
  - double to/from integer
  - varbinary to/from string

In addition to modifciation of all relevant test results, we have also
extended e_casts.test.lua introduced above for checking of all possible
directions of implicit conversions.

Approach to check it though is different than for explicit cast, we do
not use simple expression with implicit cast activated (like we might be 
using `CAST()` for explicit casts), but rather we insert value of original
type into columns of expected target type.

Relates to #5910, #6009
Closes #4470


Bonus - better error reporting in tap tests
===========================================

Also, during debugging of explicit/implicitit conversions test it was
discovered the harder way, that TAP infrastructure does not report correctly
origin source line of and error, but rather report :0 line as location.
This has been fixed.

Closes #6134

