# vdbe_jit.gdb — GDB init script for debugging CnP and MCJIT SQL execution.
#
# Loaded automatically by gdb_jit.sh.  Can also be sourced manually:
#   (gdb) source /path/to/tools/jit_bench/vdbe_jit.gdb
#
# Provides:
#   cnp-info  <vdbe*>   — dump CnP compilation state for a Vdbe
#   jit-info  <vdbe*>   — dump MCJIT compilation state for a Vdbe
#   sql-vdbes           — list active VDBEs with SQL and JIT/CnP addresses
#   cnp-find <addr>     — find the CnP Vdbe owning a code address
#   jit-find <addr>     — find the MCJIT Vdbe owning a jit_func address
#   cnp-break <vdbe*>   — break on CnP code entry
#   jit-break <vdbe*>   — break on MCJIT function entry
#   cnp-disas <vdbe*> [pc] — disassemble CnP code or one stencil by PC
#   jit-disas <vdbe*>   — disassemble MCJIT function
#   sql-break-compile   — break at every CnP + MCJIT compile, log SQL text
#   sql-break-exec      — break at every CnP + MCJIT exec entry
#   sql-break-off       — delete all SQL JIT breakpoints

set pagination off
set print pretty on
set print array-indexes on

# ---------------------------------------------------------------------------
# User-defined commands
# ---------------------------------------------------------------------------

define cnp-info
    if $argc != 1
        printf "Usage: cnp-info <vdbe_ptr>\n"
    else
        set $p = (struct Vdbe *)$arg0
        printf "=== CnP state for Vdbe %p ===\n", $p
        printf "  SQL:          %s\n", $p->zSql
        printf "  nOp:          %d\n", $p->nOp
        printf "  cnp_compiled: %d  (0=NOT_COMPILED 1=COMPILED 2=FAILED)\n", $p->cnp_compiled
        printf "  cnp_code:     %p\n", $p->cnp_code
        printf "  cnp_size:     %u bytes\n", $p->cnp_size
        printf "  cnp_nop:      %d\n", $p->cnp_nop
        printf "  cnp_ehframe:  %p\n", $p->cnp_ehframe
        printf "  resume_func:  %p\n", $p->cnp_resume_func
        printf "  row_ready:    %d\n", $p->cnp_row_ready
        printf "  perf symbol:  vdbe_cnp_stmt_%08x_ops_%d\n", $p->stmt_id, $p->nOp
    end
end
document cnp-info
Dump CnP compilation state for a Vdbe*.
Usage: cnp-info <vdbe_ptr>
end

define jit-info
    if $argc != 1
        printf "Usage: jit-info <vdbe_ptr>\n"
    else
        set $p = (struct Vdbe *)$arg0
        printf "=== MCJIT state for Vdbe %p ===\n", $p
        printf "  SQL:          %s\n", $p->zSql
        printf "  nOp:          %d\n", $p->nOp
        printf "  jit_compiled: %d\n", $p->jit_compiled
        printf "  jit_func:     %p\n", $p->jit_func
        if $p->jit_func != 0
            printf "  symbol:       "
            info symbol $p->jit_func
        end
    end
end
document jit-info
Dump MCJIT compilation state for a Vdbe*.
Usage: jit-info <vdbe_ptr>
end

# Print the opcode table for a Vdbe
define cnp-opcodes
    if $argc != 1
        printf "Usage: cnp-opcodes <vdbe_ptr>\n"
    else
        set $p = (struct Vdbe *)$arg0
        set $i = 0
        printf "PC  opcode  p1      p2      p3\n"
        while $i < $p->nOp
            printf "%3d  %3d     %6d  %6d  %6d\n", $i, \
                $p->aOp[$i].opcode, $p->aOp[$i].p1, \
                $p->aOp[$i].p2, $p->aOp[$i].p3
            set $i = $i + 1
        end
    end
end
document cnp-opcodes
Print the opcode table for a Vdbe*.
Usage: cnp-opcodes <vdbe_ptr>
end

define sql-vdbes
    set $db = sql_get()
    if $db == 0
        printf "sql_get() returned NULL\n"
    else
        set $p = $db->pVdbe
        while $p != 0
            printf "Vdbe %p  stmt_id=%08x  sql=%s\n", $p, $p->stmt_id, $p->zSql
            printf "  CnP : compiled=%d code=%p size=%u name=vdbe_cnp_stmt_%08x_ops_%d\n", \
                $p->cnp_compiled, $p->cnp_code, $p->cnp_size, $p->stmt_id, $p->nOp
            printf "  JIT : compiled=%d func=%p\n", $p->jit_compiled, $p->jit_func
            if $p->jit_func != 0
                printf "        "
                info symbol $p->jit_func
            end
            set $p = $p->pNext
        end
    end
end
document sql-vdbes
List active VDBEs with SQL text and JIT/CnP code pointers.
Usage: sql-vdbes
end

define cnp-find
    if $argc != 1
        printf "Usage: cnp-find <addr>\n"
    else
        set $addr = (char *)$arg0
        set $db = sql_get()
        set $p = $db->pVdbe
        set $found = 0
        while $p != 0
            if $p->cnp_code != 0
                set $start = (char *)$p->cnp_code
                set $end = $start + $p->cnp_size
                if $addr >= $start && $addr < $end
                    printf "CnP addr %p belongs to Vdbe %p\n", $addr, $p
                    cnp-info $p
                    set $found = 1
                end
            end
            set $p = $p->pNext
        end
        if $found == 0
            printf "No active CnP Vdbe owns %p\n", $addr
        end
    end
end
document cnp-find
Find the active Vdbe whose CnP code buffer owns an address.
Usage: cnp-find <addr>
end

define jit-find
    if $argc != 1
        printf "Usage: jit-find <addr>\n"
    else
        set $addr = (void *)$arg0
        set $db = sql_get()
        set $p = $db->pVdbe
        set $found = 0
        while $p != 0
            if $p->jit_func == $addr
                printf "MCJIT addr %p belongs to Vdbe %p\n", $addr, $p
                jit-info $p
                printf "  SQL: %s\n", $p->zSql
                set $found = 1
            end
            set $p = $p->pNext
        end
        if $found == 0
            printf "No active MCJIT Vdbe owns %p\n", $addr
        end
    end
end
document jit-find
Find the active Vdbe whose MCJIT function entry matches an address.
Usage: jit-find <addr>
end

define cnp-break
    if $argc != 1
        printf "Usage: cnp-break <vdbe_ptr>\n"
    else
        set $p = (struct Vdbe *)$arg0
        if $p->cnp_code == 0 || $p->cnp_compiled != 1
            printf "CnP code is not compiled for %p\n", $p
        else
            break *$p->cnp_code
        end
    end
end
document cnp-break
Set a breakpoint on the CnP code entry address for a Vdbe*.
Usage: cnp-break <vdbe_ptr>
end

define jit-break
    if $argc != 1
        printf "Usage: jit-break <vdbe_ptr>\n"
    else
        set $p = (struct Vdbe *)$arg0
        if $p->jit_func == 0 || $p->jit_compiled == 0
            printf "MCJIT function is not compiled for %p\n", $p
        else
            break *$p->jit_func
        end
    end
end
document jit-break
Set a breakpoint on the MCJIT function entry for a Vdbe*.
Usage: jit-break <vdbe_ptr>
end

define cnp-disas
    if $argc < 1 || $argc > 2
        printf "Usage: cnp-disas <vdbe_ptr> [pc]\n"
    else
        set $p = (struct Vdbe *)$arg0
        if $p->cnp_code == 0 || $p->cnp_compiled != 1
            printf "CnP code is not compiled for %p\n", $p
        else
            if $argc == 1
                set $start = (char *)$p->cnp_code
                set $end = $start + $p->cnp_size
                printf "Disassembly for vdbe_cnp_stmt_%08x_ops_%d [%p, %p)\n", \
                    $p->stmt_id, $p->nOp, $start, $end
                eval "disassemble 0x%lx, 0x%lx", \
                    (unsigned long)$start, (unsigned long)$end
            else
                set $pc = (int)$arg1
                if $pc < 0 || $pc >= $p->cnp_nop
                    printf "PC %d out of range [0, %d)\n", $pc, $p->cnp_nop
                else
                    set $start = (char *)$p->cnp_pc_stencil[$pc]
                    if $pc + 1 < $p->cnp_nop
                        set $end = (char *)$p->cnp_pc_stencil[$pc + 1]
                    else
                        set $end = (char *)$p->cnp_code + $p->cnp_size
                    end
                    printf "Disassembly for vdbe_cnp_stmt_%08x_ops_%d pc=%d [%p, %p)\n", \
                        $p->stmt_id, $p->nOp, $pc, $start, $end
                    eval "disassemble 0x%lx, 0x%lx", \
                        (unsigned long)$start, (unsigned long)$end
                end
            end
        end
    end
end
document cnp-disas
Disassemble CnP native code for a Vdbe*.
Usage: cnp-disas <vdbe_ptr> [pc]
Without pc, disassembles the whole code buffer.
With pc, disassembles one opcode stencil.
end

define jit-disas
    if $argc != 1
        printf "Usage: jit-disas <vdbe_ptr>\n"
    else
        set $p = (struct Vdbe *)$arg0
        if $p->jit_func == 0 || $p->jit_compiled == 0
            printf "MCJIT function is not compiled for %p\n", $p
        else
            printf "Disassembly for MCJIT function at %p\n", $p->jit_func
            info symbol $p->jit_func
            eval "disassemble 0x%lx", (unsigned long)$p->jit_func
        end
    end
end
document jit-disas
Disassemble the MCJIT native function for a Vdbe*.
Usage: jit-disas <vdbe_ptr>
end

# ---------------------------------------------------------------------------
# Breakpoint sets
# ---------------------------------------------------------------------------

define sql-break-compile
    break vdbe_cnp_compile
    commands
        silent
        printf "[CnP] compile: %d ops  sql=%s\n", p->nOp, p->zSql
        continue
    end
    break vdbe_jit_compile
    commands
        silent
        printf "[JIT] compile: %d ops  sql=%s\n", p->nOp, p->zSql
        continue
    end
    printf "SQL compile breakpoints set (silent, auto-continue).\n"
    printf "Remove with: sql-break-off\n"
end
document sql-break-compile
Set silent logging breakpoints at vdbe_cnp_compile and vdbe_jit_compile.
Each hit prints the SQL text and op count and continues.
end

define sql-break-exec
    break vdbe_cnp_exec
    commands
        silent
        printf "[CnP] exec: sql=%s  code=%p\n", p->zSql, p->cnp_code
        bt 6
        continue
    end
    break sqlVdbeExec
    commands
        silent
        printf "[VDBE] exec: sql=%s\n", p->zSql
        continue
    end
    printf "SQL exec breakpoints set (silent, auto-continue).\n"
end
document sql-break-exec
Set silent logging breakpoints at vdbe_cnp_exec and sqlVdbeExec.
end

define sql-break-off
    delete breakpoints
    printf "All breakpoints deleted.\n"
end
document sql-break-off
Delete all breakpoints.
end

# ---------------------------------------------------------------------------
# Startup message
# ---------------------------------------------------------------------------
printf "\n"
printf "=== VDBE JIT debug helpers loaded ===\n"
printf "  cnp-info   <vdbe*>  — CnP compile state\n"
printf "  jit-info   <vdbe*>  — MCJIT compile state\n"
printf "  sql-vdbes  — list active VDBEs\n"
printf "  cnp-find   <addr>   — resolve CnP address to SQL/Vdbe\n"
printf "  jit-find   <addr>   — resolve MCJIT address to SQL/Vdbe\n"
printf "  cnp-break  <vdbe*>  — break on CnP code entry\n"
printf "  jit-break  <vdbe*>  — break on MCJIT function entry\n"
printf "  cnp-opcodes <vdbe*> — opcode table\n"
printf "  cnp-disas  <vdbe*> [pc] — CnP disassembly\n"
printf "  jit-disas  <vdbe*>  — MCJIT disassembly\n"
printf "  sql-break-compile   — log every CnP+MCJIT compile\n"
printf "  sql-break-exec      — log every CnP+VDBE exec entry\n"
printf "  sql-break-off       — delete all breakpoints\n"
printf "\n"
