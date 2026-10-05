// Host floating-point exception guard.
//
// ReXGlue v0.10.0 can leave host FP exceptions unmasked on threads whose
// PPCContext never ran fpscr.InitHost() (see handler below), after which
// ordinary float math raises STATUS_FLOAT_INEXACT_RESULT and kills the
// process. Xbox 360 titles never rely on these traps, so re-mask them and
// resume. The first few hits are logged to fpe.log for diagnosis.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>

#include <rex/ppc/context.h>
#include <rex/system/thread_state.h>

namespace {

constexpr DWORD kMxcsrMaskAll = 0x1F80;  // IM|DM|ZM|OM|UM|PM
constexpr WORD kX87MaskAll = 0x3F;

bool IsFloatException(DWORD code) {
  switch (code) {
    case STATUS_FLOAT_DENORMAL_OPERAND:
    case STATUS_FLOAT_DIVIDE_BY_ZERO:
    case STATUS_FLOAT_INEXACT_RESULT:
    case STATUS_FLOAT_INVALID_OPERATION:
    case STATUS_FLOAT_OVERFLOW:
    case STATUS_FLOAT_STACK_CHECK:
    case STATUS_FLOAT_UNDERFLOW:
    case STATUS_FLOAT_MULTIPLE_FAULTS:
    case STATUS_FLOAT_MULTIPLE_TRAPS:
      return true;
    default:
      return false;
  }
}

LONG CALLBACK FloatExceptionHandler(EXCEPTION_POINTERS* ep) {
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (!IsFloatException(code)) return EXCEPTION_CONTINUE_SEARCH;

  static std::atomic<int> hits{0};
  if (hits.fetch_add(1) < 16) {
    const auto rip = static_cast<uintptr_t>(ep->ContextRecord->Rip);
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(rip), &mod);
    char mod_name[MAX_PATH] = "?";
    if (mod) GetModuleFileNameA(mod, mod_name, MAX_PATH);
    if (FILE* f = std::fopen("fpe.log", "a")) {
      std::fprintf(f, "code=0x%08lX rip=%p module=%s rva=0x%llX mxcsr=0x%08lX\n", code,
                   reinterpret_cast<void*>(rip), mod_name,
                   static_cast<unsigned long long>(rip - reinterpret_cast<uintptr_t>(mod)),
                   ep->ContextRecord->MxCsr);
      std::fclose(f);
    }
  }

  // The runtime only calls fpscr.InitHost() in XThread::Execute, so contexts
  // that run guest callbacks on host threads (audio worker, GPU interrupts)
  // keep a cached csr of 0 and every enable/disableFlushMode re-unmasks the
  // host. Repair the cached value so this thread doesn't trap again.
  if (auto* ts = rex::runtime::ThreadState::Get()) {
    if (auto* ctx = ts->context()) ctx->fpscr.csr |= kMxcsrMaskAll;
  }

  ep->ContextRecord->MxCsr |= kMxcsrMaskAll;
  ep->ContextRecord->FltSave.MxCsr |= kMxcsrMaskAll;
  ep->ContextRecord->FltSave.ControlWord |= kX87MaskAll;
  ep->ContextRecord->FltSave.StatusWord &= ~kX87MaskAll;
  return EXCEPTION_CONTINUE_EXECUTION;
}

const PVOID g_handler = AddVectoredExceptionHandler(1, FloatExceptionHandler);

}  // namespace

#endif  // _WIN32

#if defined(__linux__)

#include <csignal>
#include <ucontext.h>

#include <rex/ppc/context.h>
#include <rex/system/thread_state.h>

namespace {

constexpr unsigned kMxcsrMaskAll = 0x1F80;  // IM|DM|ZM|OM|UM|PM
constexpr unsigned kMxcsrFlags = 0x3F;

// Linux counterpart of the handler above: the same unmasked traps arrive as
// SIGFPE. Re-mask them in the interrupted context and resume.
void FloatSignalHandler(int, siginfo_t*, void* uctx) {
  if (auto* ts = rex::runtime::ThreadState::Get()) {
    if (auto* ctx = ts->context()) ctx->fpscr.csr |= kMxcsrMaskAll;
  }
  auto* uc = static_cast<ucontext_t*>(uctx);
  if (uc->uc_mcontext.fpregs) {
    uc->uc_mcontext.fpregs->mxcsr = (uc->uc_mcontext.fpregs->mxcsr | kMxcsrMaskAll) & ~kMxcsrFlags;
    uc->uc_mcontext.fpregs->cwd |= 0x3F;
    uc->uc_mcontext.fpregs->swd &= ~0x3F;
  }
}

const bool g_installed = [] {
  struct sigaction sa {};
  sa.sa_sigaction = FloatSignalHandler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  return sigaction(SIGFPE, &sa, nullptr) == 0;
}();

}  // namespace

#endif  // __linux__
