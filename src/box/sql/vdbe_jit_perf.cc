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

	if (getenv_flag("SQL_JIT_PERF_MAP")) {
		auto *l = llvm::JITEventListener::createPerfJITEventListener();
		if (l)
			EE->RegisterJITEventListener(l);
	}
	if (getenv_flag("SQL_JIT_GDB")) {
		auto *l = llvm::JITEventListener::createGDBRegistrationListener();
		if (l)
			EE->RegisterJITEventListener(l);
	}
}
