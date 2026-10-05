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

struct sigaction g_previous {};

// Linux counterpart of the handler above: the same unmasked float traps arrive
// as SIGFPE. Re-mask them in the interrupted context and resume. Anything else
// (integer division by zero) goes to whoever handled SIGFPE before.
void FloatSignalHandler(int sig, siginfo_t* info, void* uctx) {
  const bool float_trap = info && info->si_code >= FPE_FLTDIV && info->si_code <= FPE_FLTSUB;
  auto* uc = static_cast<ucontext_t*>(uctx);
  if (!float_trap || !uc || !uc->uc_mcontext.fpregs) {
    if (g_previous.sa_flags & SA_SIGINFO) {
      if (g_previous.sa_sigaction) return g_previous.sa_sigaction(sig, info, uctx);
    } else if (g_previous.sa_handler != SIG_DFL && g_previous.sa_handler != SIG_IGN && g_previous.sa_handler) {
      return g_previous.sa_handler(sig);
    }
    signal(SIGFPE, SIG_DFL);
    raise(SIGFPE);
    return;
  }
  // As on Windows, repair the context's cached csr so this thread doesn't trap again.
  if (auto* ts = rex::runtime::ThreadState::Get()) {
    if (auto* ctx = ts->context()) ctx->fpscr.csr |= kMxcsrMaskAll;
  }
  uc->uc_mcontext.fpregs->mxcsr = (uc->uc_mcontext.fpregs->mxcsr | kMxcsrMaskAll) & ~kMxcsrFlags;
  uc->uc_mcontext.fpregs->cwd |= 0x3F;
  uc->uc_mcontext.fpregs->swd &= ~0x3F;
}

}  // namespace

namespace kk {
// The runtime installs its own signal handlers during setup, so this is called
// again afterwards (OnPostSetup) to make sure the float guard is in front.
void InstallFpeGuard() {
  struct sigaction sa {};
  sa.sa_sigaction = FloatSignalHandler;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  struct sigaction old {};
  if (sigaction(SIGFPE, &sa, &old) == 0 && old.sa_sigaction != FloatSignalHandler) g_previous = old;
}
}  // namespace kk

namespace {
const bool g_installed = (kk::InstallFpeGuard(), true);
}  // namespace

#else

namespace kk {
void InstallFpeGuard() {}  // Windows: the vectored handler above is installed at startup.
}  // namespace kk

#endif  // __linux__
