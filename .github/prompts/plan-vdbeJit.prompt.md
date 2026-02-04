# Plan: Statement-Level AOT JIT Using Compiled Handler IR

**TL;DR**: Add ahead-of-time JIT compilation for SQL prepared statements by directly reusing LLVM IR from compiled C handlers. At build time, compile handler code ([vdbe_ops_arithmetic.c](src/box/sql/vdbe_ops_arithmetic.c), etc.) to LLVM bitcode. At PREPARE time, link handler IR into JIT function and inline for arithmetic/comparisons. No manual IR templates needed - leverage existing C code that LLVM already compiles. Focus on expressions (arithmetic, comparisons, filters) with smooth fallback to interpreter for I/O operations.

## Steps

### 1. Capture handler IR at build time (~1-2 weeks)

- Modify [src/box/sql/CMakeLists.txt](src/box/sql/CMakeLists.txt) to compile handler files to LLVM bitcode when `ENABLE_SQL_JIT=ON`
- Add CMake commands:
  ```cmake
  add_custom_command(
    OUTPUT vdbe_ops_arithmetic.bc
    COMMAND clang -emit-llvm -c -O2 ${SQL_SRC_DIR}/vdbe_ops_arithmetic.c -o ${SQL_BIN_DIR}/vdbe_ops_arithmetic.bc
    DEPENDS vdbe_ops_arithmetic.c
  )
  ```
- Generate bitcode for all handler files: [vdbe_ops_arithmetic.c](src/box/sql/vdbe_ops_arithmetic.c), [vdbe_ops_comparison.c](src/box/sql/vdbe_ops_comparison.c), [vdbe_ops_register.c](src/box/sql/vdbe_ops_register.c), [vdbe_ops_logical.c](src/box/sql/vdbe_ops_logical.c)
- Install `.bc` files alongside library: `${CMAKE_INSTALL_PREFIX}/share/tarantool/sql_handlers.bc`

### 2. Add JIT compiler infrastructure (~1-2 weeks)

- Create [src/box/sql/vdbe_jit.c](src/box/sql/vdbe_jit.c) with LLVM C++ API wrappers
- Implement `vdbe_jit_init()` → loads handler bitcode modules into `llvm::Module`, caches for reuse
- Add to `struct Vdbe` in [vdbe.h](src/box/sql/vdbe.h): `void *jit_func`, `bool jit_compiled`, `LLVMModuleRef jit_module`
- Add configuration: `box.cfg{sql = {jit = {enable = false}}}` with C API `box_sql_jit_enabled()` (default disabled for safety)

### 3. Generate JIT function by linking handler IR (~3-4 weeks)

- Implement `vdbe_jit_compile(Vdbe *p)` in [vdbe_jit.c](src/box/sql/vdbe_jit.c):
  - Create new LLVM function: `i32 @vdbe_jit_exec_%d(ptr %vdbe_ptr, i32 %start_pc)`
  - Scan VDBE program (`p->aOp[]`) and classify opcodes: `jitable` (arithmetic/comparison), `callable` (I/O handlers), `unsupported` (fallback immediately)
  - For each opcode in program:
    - If `jitable`: Use LLVM `CloneFunctionInto()` to copy handler function from bitcode, then inline with `InlineFunctionInfo` API
    - If `callable`: Emit `call` instruction to handler function (FFI)
    - If `unsupported`: Emit `ret %current_pc` to return control to interpreter
  - Apply LLVM optimization passes: `createFunctionInliningPass()`, `createInstructionCombiningPass()`, dead code elimination
  - Compile optimized IR to native code using LLVM ORC JIT, store function pointer in `p->jit_func`

### 4. Integrate JIT execution with fallback (~1 week)

- Modify [vdbe.c](src/box/sql/vdbe.c) `sqlVdbeExec()` entry point:
  ```c
  if (p->jit_compiled && box_sql_jit_enabled()) {
      int next_pc = ((VdbeJitFunc)p->jit_func)(p, p->pc);
      if (next_pc >= 0) {
          p->pc = next_pc;
          // JIT stopped at unsupported opcode, continue in interpreter
      } else {
          return next_pc;  // JIT completed: -1=done, <-1=error
      }
  }
  // Interpreter fallback
  return vdbe_exec_generated_dispatcher(p, p->aOp, p->aMem);
  ```
- JIT and interpreter can hand off seamlessly at any PC boundary
- Add debug logging when fallback occurs (controlled by `VDBE_PROFILE` flag)

### 5. Extend YAML DSL with JIT metadata (minimal) (~1 week)

- Add `jit_inline: true/false` field to [opcodes.yaml](tools/vdbe_dsl/opcodes.yaml) indicating if handler should be inlined
- Mark for inlining: `OP_Add, OP_Subtract, OP_Multiply, OP_Divide, OP_Remainder` (arithmetic), `OP_Eq, OP_Ne, OP_Lt, OP_Le, OP_Gt, OP_Ge` (comparisons), `OP_And, OP_Or, OP_Not` (logical), `OP_Move, OP_Copy` (register ops)
- Mark for FFI call: `OP_Column, OP_Next, OP_SeekGE, OP_IdxInsert` (I/O operations)
- Mark unsupported: `OP_Init, OP_Halt, OP_Program` (control flow requiring special handling)
- Update [vdbe_codegen.py](tools/vdbe_codegen.py) to emit `const char *vdbe_opcode_jit_mode[176]` table used by JIT compiler

## Implementation Decisions

### 1. Handler function specialization strategy

All handlers use `int vdbe_op_xxx(Vdbe *p, Op *pOp, Mem *aMem)`. When inlining into JIT code:

**Decision**: Specialize by replacing `pOp->p1/p2/p3` with compile-time constants (Option B), AND attempt type specialization where beneficial.

**Rationale**: 
- Constant propagation will optimize away parameter passing overhead and enable aggressive inlining
- For frequently-executed arithmetic/comparison operations, eliminating runtime type checks provides additional speedup
- Type specialization will be attempted opportunistically - if type information is available at PREPARE time, generate specialized variants

**Implementation approach**:
- When compiling JIT function, analyze opcode parameters and inline handler with constant values substituted
- Track type information during PREPARE phase (e.g., from schema knowledge or literal constants)
- Where types are known statically, specialize handler to single type path
- Where types are dynamic, inline full handler and rely on LLVM dead code elimination

### 2. Type specialization implementation

Handlers have type switches (e.g., `switch(mem_type(p1))`). For generating specialized variants:

**Decision**: Emit specialized versions per type using templatized C++ handlers (Option B approach).

**Rationale**:
- More maintainable than manual specialization - C++ templates allow writing handler logic once
- Compiler generates optimized variants automatically for INTEGER, REAL, TEXT, BLOB types
- Template specialization provides type-specific optimizations (e.g., integer fast path)
- Cleaner code structure: `template<MemType T> int vdbe_op_add(Vdbe *p, Mem *p1, Mem *p2, Mem *pOut)`

**Implementation approach**:
- Convert critical handlers ([vdbe_ops_arithmetic.c](src/box/sql/vdbe_ops_arithmetic.c), [vdbe_ops_comparison.c](src/box/sql/vdbe_ops_comparison.c)) to C++ with templates
- Compile template instantiations to separate bitcode files: `vdbe_ops_arithmetic_int.bc`, `vdbe_ops_arithmetic_real.bc`, etc.
- At JIT compile time, select appropriate specialized bitcode based on type analysis
- Fallback to generic handler when type cannot be determined statically

### 3. LLVM version compatibility

Tarantool built with LLVM 12, but runtime environment may have LLVM 14.

**Decision**: Version mismatch is not a concern - both bitcode generation and JIT compilation happen at build time.

**Rationale**:
- Build process: Developer builds Tarantool with LLVM X → generates `.bc` files with LLVM X → links JIT runtime with LLVM X
- Runtime: Tarantool binary includes embedded LLVM ORC JIT (same version used during build) → loads `.bc` files → compiles to native code
- No runtime LLVM dependency - JIT compilation occurs within Tarantool process using embedded LLVM components
- Bitcode files never cross LLVM version boundaries (unlike pre-compiled shared libraries)

**Implementation approach**:
- Detect LLVM version at CMake configuration time
- Compile handler `.bc` files using same LLVM version as JIT runtime
- Ship `.bc` files alongside Tarantool binary in installation bundle
- Document LLVM version requirement in build documentation
