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

extern "C" void
vdbe_jit_register_perf_listener(LLVMExecutionEngineRef ee_ref)
{
	const char *env = getenv("SQL_JIT_PERF_MAP");
	if (env == nullptr || env[0] != '1')
		return;

	llvm::ExecutionEngine *EE = llvm::unwrap(ee_ref);
	if (EE == nullptr)
		return;

	llvm::JITEventListener *listener =
		llvm::JITEventListener::createPerfJITEventListener();
	if (listener != nullptr)
		EE->RegisterJITEventListener(listener);
}
