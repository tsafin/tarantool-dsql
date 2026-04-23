# M5 Phase 2 — DWARF / GDB / Unwind Support for CnP and MCJIT

## Current State (M5 Phase 2 — done)

| Backend | Feature | Activation | What you get |
|---------|---------|------------|--------------|
| CnP | `/tmp/perf-PID.map` | `SQL_CNP_PERF_MAP=1` | Function-level perf symbolication |
| CnP | `.eh_frame` CFI | `SQL_CNP_EH_FRAME=1` | Stack unwinding through CnP frames; `gdb bt` shows CnP frames |
| CnP | JITDUMP | `SQL_CNP_JITDUMP=1` | Per-opcode perf attribution after `perf inject --jit` |
| MCJIT | JITDUMP (PerfJITEventListener) | `SQL_JIT_PERF_MAP=1` | Function-level perf + DWARF via `perf inject --jit` |
| MCJIT | GDB registration listener | `SQL_JIT_GDB=1` | `gdb bt` shows MCJIT function names, breakpoints inside JIT |

## Tasks Completed (M5 Phase 2)

| Task | Lines | File | Status |
|------|-------|------|--------|
| A — MCJIT GDB listener | ~15 | `vdbe_jit_perf.cc` | ✅ Done |
| B — CnP `.eh_frame` CFI | ~100 | `vdbe_cnp.c` | ✅ Done |
| C — CnP JITDUMP | ~200 | `vdbe_cnp.c` | ✅ Done |

### Implementation notes

**Task A**: `createGDBRegistrationListener()` builds a minimal in-memory ELF
for each compiled MCJIT function and notifies gdb via the JIT interface
`__jit_debug_register_code`.  Activated by `SQL_JIT_GDB=1`.

**Task B**: CIE encodes `CFA = RSP+8, RA = [RSP]` — the uniform CnP calling
convention (no prologue, no callee-saves).  Without augmentation `zR`, libgcc
expects 8-byte absolute addresses in the FDE's `pc_begin`/`pc_range` fields
on x86-64.  Includes a 4-byte zero section terminator required by libgcc's
`__register_frame`.  Guarded by `HAVE_REGISTER_FRAME` (CMake check).
Activated by `SQL_CNP_EH_FRAME=1`.

**Task C**: Emits `JIT_CODE_LOAD` + `JIT_CODE_DEBUG_INFO` records into a
mmap'd `/tmp/jit-PID.dump` file.  Each opcode gets one debug entry with its
stencil address, PC index as line number, and opcode mnemonic as filename.
Uses `mremap` to grow the file as needed.  Activated by `SQL_CNP_JITDUMP=1`.

---

## Task A — MCJIT: GDB registration listener

**Effort:** ~10 lines of C++
**File:** `src/box/sql/vdbe_jit_perf.cc`
**Activation:** `SQL_JIT_GDB=1`

LLVM 11 provides `JITEventListener::createGDBRegistrationListener()`.  It
implements the [GDB JIT interface][gdb-jit]: builds a minimal in-memory ELF
object for each compiled function and notifies gdb via `__jit_debug_register_code`.

Hook it alongside the existing `createPerfJITEventListener()` call:

```cpp
extern "C" void
vdbe_jit_register_perf_listener(LLVMExecutionEngineRef ee_ref)
{
    llvm::ExecutionEngine *EE = llvm::unwrap(ee_ref);
    if (EE == nullptr)
        return;

    if (getenv_flag("SQL_JIT_PERF_MAP")) {
        auto *l = llvm::JITEventListener::createPerfJITEventListener();
        if (l) EE->RegisterJITEventListener(l);
    }
    if (getenv_flag("SQL_JIT_GDB")) {
        auto *l = llvm::JITEventListener::createGDBRegistrationListener();
        if (l) EE->RegisterJITEventListener(l);
    }
}
```

After this change, `gdb` will show MCJIT frames by name in backtraces and
can set breakpoints inside compiled SQL functions.

[gdb-jit]: https://sourceware.org/gdb/current/onlinedocs/gdb/JIT-Interface.html

---

## Task B — CnP: `.eh_frame` CFI registration

**Effort:** ~80 lines of C
**File:** `src/box/sql/vdbe_cnp.c`
**Activation:** consider always-on (overhead is negligible)

### Problem

CnP stencils are called as plain function pointers with no prologue/epilogue.
The CPU's default frame unwinding (RBP chain or `.eh_frame` DWARF CFI) has no
information about them.  Consequences:

- `backtrace()` / `backtrace_symbols()` produce garbage or stop at the CnP
  dispatch loop.
- gdb `bt` shows `?? ()` for all CnP frames.
- C++ exceptions cannot propagate through CnP frames (not a current concern
  since CnP code is plain C, but relevant if the call stack passes through
  CnP into Lua or C++ code that throws).

### Solution: DWARF CFI via `__register_frame`

The CnP calling convention is uniform and trivially describable in DWARF CFI:

- No function prologue — RSP is not adjusted by the stencil itself.
- No callee-saved registers pushed.
- The return address is at `[RSP]` (i.e. `CFA = RSP + 8`, `RA = [CFA-8]`).

This means a single static **CIE** (Common Information Entry) covers every
compiled program, and each program only needs a short **FDE** (Frame
Description Entry) recording its start address and byte length.

```
CIE:
  version = 1
  augmentation = ""       (no LSB/personality/LSDA)
  code_align = 1
  data_align = -8         (x86-64 standard)
  return_address_register = 16  (RIP)
  DW_CFA_def_cfa: register=RSP (7), offset=8
  DW_CFA_offset: register=RIP (16), offset=-8/data_align = 1

FDE per program:
  initial_location = <code pointer>
  address_range    = <code_size>
  (no additional instructions — CIE rules apply everywhere)
```

Total size: ~28-byte CIE + ~20-byte FDE per program.

### Implementation sketch

```c
/* Static CIE bytes (x86-64, computed once). */
static const uint8_t cnp_cie_bytes[] = { /* pre-encoded */ };

static void
cnp_register_frame(struct Vdbe *p)
{
    size_t sz = sizeof(cnp_cie_bytes) + CNP_FDE_SIZE;
    uint8_t *ehframe = malloc(sz);
    if (!ehframe) return;
    memcpy(ehframe, cnp_cie_bytes, sizeof(cnp_cie_bytes));
    cnp_encode_fde(ehframe + sizeof(cnp_cie_bytes),
                   (uintptr_t)p->cnp_code, p->cnp_size,
                   /* CIE offset */ sizeof(cnp_cie_bytes));
    __register_frame(ehframe);
    p->cnp_ehframe = ehframe;   /* new field on Vdbe */
}

static void
cnp_deregister_frame(struct Vdbe *p)
{
    if (p->cnp_ehframe) {
        __deregister_frame(p->cnp_ehframe);
        free(p->cnp_ehframe);
        p->cnp_ehframe = NULL;
    }
}
```

Call `cnp_register_frame(p)` at the end of `vdbe_cnp_compile()`, and
`cnp_deregister_frame(p)` in `vdbe_cnp_release()` (arena wrap path that
sets `cnp_compiled = CNP_NOT_COMPILED`).

### Platform notes

- `__register_frame` / `__deregister_frame` live in `libgcc_s.so.1`
  (present on all Debian/Ubuntu targets).  On musl they live in `libgcc.a`.
  Add a CMake check:
  ```cmake
  check_function_exists(__register_frame HAVE_REGISTER_FRAME)
  ```
  and guard the feature with `#ifdef HAVE_REGISTER_FRAME`.

- On macOS the function is `__register_frame` in `libSystem` but it takes
  a single FDE, not a whole `.eh_frame` section.  Guard with
  `#if defined(__linux__)` for now.

- The DWARF `.eh_frame` encoding is little-endian, DW_EH_PE_pcrel for
  addresses (to make the FDE relocatable in the mmap'd arena).

---

## Task C — CnP: JITDUMP format for per-opcode perf attribution

**Effort:** ~200 lines of C
**File:** `src/box/sql/vdbe_cnp.c`
**Activation:** `SQL_CNP_JITDUMP=1`

### Problem

The current `/tmp/perf-PID.map` gives function-level attribution:

```
perf report:
  42.3%  tarantool  [JIT] vdbe_cnp_17
```

With JITDUMP we get opcode-level attribution:

```
perf report:
  18.1%  tarantool  [JIT] OP_Column       (vdbe_cnp_17:3)
   9.4%  tarantool  [JIT] OP_Compare      (vdbe_cnp_17:7)
   7.2%  tarantool  [JIT] OP_MakeRecord   (vdbe_cnp_17:11)
```

### The JITDUMP format

Documented in `linux/tools/perf/Documentation/jit-interface.txt`.

Key records written once per compiled program:

1. **`JIT_CODE_LOAD`** — registers the code address+size with a symbol name.
2. **`JIT_CODE_DEBUG_INFO`** — N entries, one per opcode:
   - `addr` = address of that opcode's stencil in the code buffer
     (`p->cnp_code + pc_offset[i]`)
   - `lineno` = opcode index `i` (used as "line number" by perf)
   - `filename` = opcode mnemonic (e.g. `"OP_Column"`)

The dump file must be **mmap'd** (perf reads it via mmap while the process
runs), not written with `fprintf`.  Use `ftruncate` to grow it as needed.

```c
struct jitdump_file_header {
    uint32_t magic;       /* 0x4A695444 "JiTD" */
    uint32_t version;     /* 1 */
    uint32_t total_size;  /* sizeof(header) */
    uint32_t elf_mach;    /* EM_X86_64 = 62 */
    uint32_t pad1;        /* 0 */
    uint32_t pid;
    uint64_t timestamp;   /* CLOCK_MONOTONIC ns */
    uint64_t flags;       /* 0 */
};
```

### What to emit per `vdbe_cnp_compile()` call

At the end of `vdbe_cnp_compile()`, after `pc_offset[]` is built and before
it is freed:

1. Write a `JIT_CODE_LOAD` record with the compiled code bytes.
2. Write a `JIT_CODE_DEBUG_INFO` record with `nOp` entries:
   ```c
   for (int i = 0; i < nOp; i++) {
       entry[i].addr     = (uint64_t)(uintptr_t)(code + pc_offset[i]);
       entry[i].lineno   = i;
       entry[i].discrim  = 0;
       entry[i].name     = opcode_name(aOp[i].opcode);  /* "OP_Column" etc */
   }
   ```

### Workflow

```bash
# Record
SQL_CNP_JITDUMP=1 VDBE_DISPATCHER=cnp perf record -k mono ./src/tarantool bench.lua

# Inject DWARF
perf inject --jit -i perf.data -o perf.jit.data

# Report with opcode-level attribution
perf report -i perf.jit.data --stdio
```

### Implementation notes

- Keep `SQL_CNP_PERF_MAP` and `SQL_CNP_JITDUMP` as independent env vars;
  they write different files (`perf-PID.map` vs `jit-PID.dump`) and serve
  different workflows.  JITDUMP is strictly more informative but requires
  the extra `perf inject` step.

- The JITDUMP file must be opened before the first `mmap`, with a fixed
  initial size (e.g. 1 MB), grown with `ftruncate` + `mremap` as needed.

- Use `clock_gettime(CLOCK_MONOTONIC, ...)` for all timestamps.

- The `opcode_name()` helper can use the existing `sqlite3OpcodeName()`
  function already present in `vdbeaux.c`.

---

## Summary

| Task | Lines | Effort | Backend | What you gain |
|------|-------|--------|---------|---------------|
| A — GDB listener | ~10 | Trivial | MCJIT | `gdb bt` shows JIT function names |
| B — `.eh_frame` CFI | ~80 | Medium | CnP | Stack unwinding through CnP frames; `gdb bt` shows CnP frames |
| C — JITDUMP | ~200 | Medium | CnP | Per-opcode perf attribution after `perf inject --jit` |

Recommended order: A → B → C.  All three are independent.
