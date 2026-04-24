/*
 * vdbe_jit_perf.cc — C++ bridge that registers the LLVM PerfJITEventListener
 * with an existing MCJIT ExecutionEngine.
 *
 * The core JIT code lives in vdbe_jit.c (C) and uses the LLVM C API
 * (LLVMExecutionEngineRef).  PerfJITEventListener and RegisterJITEventListener
 * are only available in the C++ LLVM API, so they live here.
 *
 * Activation: set SQL_JIT_PERF_MAP=1 in the environment before starting
 * tarantool.  When set, every compiled MCJIT function is registered so that
 * `perf report` / `perf script` can symbolicate JIT frames.
 */

#include <cstdlib>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/JITEventListener.h>
#include <llvm-c/ExecutionEngine.h>

static bool
getenv_flag(const char *name)
{
	const char *v = getenv(name);
	return v != nullptr && v[0] == '1';
}

extern "C" void
vdbe_jit_register_perf_listener(LLVMExecutionEngineRef ee_ref)
{
	llvm::ExecutionEngine *EE = llvm::unwrap(ee_ref);
	if (EE == nullptr)
		return;

	/*
	 * GDB JIT registration: always active.
	 * Uses the GDB JIT interface (__jit_debug_register_code).  When gdb
	 * is not attached the function is a no-op; overhead is zero.
	 * When gdb is attached, each compiled function becomes visible in
	 * backtraces and can be stepped into.
	 */
	auto *gdb_l = llvm::JITEventListener::createGDBRegistrationListener();
	if (gdb_l)
		EE->RegisterJITEventListener(gdb_l);

	/*
	 * PerfJIT JITDUMP: opt-in via SQL_JIT_PERF_MAP=1.
	 * Writes /tmp/jit-PID.dump which `perf inject --jit` then uses to
	 * annotate perf report with per-function (and DWARF-line) attribution.
	 */
	if (getenv_flag("SQL_JIT_PERF_MAP")) {
		auto *perf_l = llvm::JITEventListener::createPerfJITEventListener();
		if (perf_l)
			EE->RegisterJITEventListener(perf_l);
	}
}
