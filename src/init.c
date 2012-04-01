/*
  init.c
  system initialization and global state
*/
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <assert.h>
#if defined(__linux) || defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <errno.h>
#include <signal.h>
#include <libgen.h>
#include <getopt.h>
#include "julia.h"
#include <stdio.h>
#ifdef __WIN32__
# define WIN32_LEAN_AND_MEAN
# include <windows.h>
#else
#include "../external/libuv/include/uv-private/ev.h"
#endif

int jl_boot_file_loaded = 0;

char *jl_stack_lo;
char *jl_stack_hi;
size_t jl_page_size;

static void jl_find_stack_bottom(void)
{
    size_t stack_size;
#if defined(__linux) || defined(__APPLE__) || defined(__FreeBSD__)
    struct rlimit rl;
    getrlimit(RLIMIT_STACK, &rl);
    stack_size = rl.rlim_cur;
#else
    stack_size = 262144;  // guess
#endif
    jl_stack_hi = (char*)&stack_size;
    jl_stack_lo = jl_stack_hi - stack_size;
}

#ifndef __WIN32__
void fpe_handler(int arg)
{
    (void)arg;
    sigset_t sset;
    sigemptyset(&sset);
    sigaddset(&sset, SIGFPE);
    sigprocmask(SIG_UNBLOCK, &sset, NULL);

    jl_divide_by_zero_error();
}

void segv_handler(int sig, siginfo_t *info, void *context)
{
    sigset_t sset;
    sigemptyset(&sset);
    sigaddset(&sset, SIGSEGV);
    sigprocmask(SIG_UNBLOCK, &sset, NULL);

#ifdef COPY_STACKS
    if ((char*)info->si_addr > (char*)jl_stack_lo-3000000 &&
        (char*)info->si_addr < (char*)jl_stack_hi) {
#else
    if ((char*)info->si_addr > (char*)jl_current_task->stack-8192 &&
        (char*)info->si_addr <
        (char*)jl_current_task->stack+jl_current_task->ssize) {
#endif
        jl_raise(jl_stackovf_exception);
    }
    else {
        signal(SIGSEGV, SIG_DFL);
    }
}

#endif

volatile sig_atomic_t jl_signal_pending = 0;
volatile sig_atomic_t jl_defer_signal = 0;

#ifdef __WIN32__
void restore_signals() { }
void sigint_handler(int wsig)
{	
	//todo: switch to using windows custom handler instead of signal
	signal(SIGINT, sigint_handler);
	int sig;
	switch sig {
	//	case ...: usig = ...; break;
		default: sig = wsig;
	}
#else	
void restore_signals() {
	sigset_t sset;
	sigemptyset (&sset);
	sigprocmask (SIG_SETMASK, &sset, 0);
}
void sigint_handler(int sig, siginfo_t *info, void *context)
{	
#endif
	//printf("sigint\n");
    if (jl_defer_signal) {
        jl_signal_pending = sig;
    }
    else {
        jl_signal_pending = 0;
#ifndef __WIN32__
        ev_break(jl_local_event_loop()->ev,EVBREAK_CANCEL);
#endif
        jl_raise(jl_interrupt_exception);
    }
}

void jl_get_builtin_hooks(void);

uv_lib_t jl_dl_handle;
#ifdef __WIN32__
uv_lib_t jl_ntdll_handle;
uv_lib_t jl_kernel32_handle;
uv_lib_t jl_crtdll_handle;
uv_lib_t jl_winsock_handle;
#endif
uv_loop_t *jl_event_loop;
uv_loop_t *jl_io_loop;

#ifdef COPY_STACKS
void jl_switch_stack(jl_task_t *t, jmp_buf *where);
extern jmp_buf * volatile jl_jmp_target;
#endif

#ifdef __WIN32__
static long chachedPagesize = 0;
long getPageSize (void) {
	if (!chachedPagesize) {
        SYSTEM_INFO systemInfo;
        GetSystemInfo (&systemInfo);
        chachedPagesize = systemInfo.dwPageSize;
    }
    return chachedPagesize;
}
#else
long getPageSize (void) {
	return sysconf(_SC_PAGESIZE);
}
#endif


void julia_init(char *imageFile)
{
    jl_page_size = getPageSize();
    jl_find_stack_bottom();
    jl_dl_handle = jl_load_dynamic_library(NULL);
#ifdef __WIN32__
    uv_dlopen("ntdll.dll",&jl_ntdll_handle); //bypass julia's pathchecking for system dlls
    uv_dlopen("Kernel32.dll",&jl_kernel32_handle);
    uv_dlopen("msvcrt.dll",&jl_crtdll_handle);
    uv_dlopen("Ws2_32.dll",&jl_winsock_handle);
#endif
    jl_io_loop =  uv_loop_new(); //this loop will handle io/sockets - if not handled otherwise
    jl_event_loop = uv_default_loop(); //this loop will internal events (spawining process etc.) - this has to be the uv default loop as that's the only supported loop for processes ;(
    //init io
    jl_stdin_tty = malloc(sizeof(uv_tty_t));
    jl_stdout_tty = malloc(sizeof(uv_tty_t));
    jl_stderr_tty = malloc(sizeof(uv_tty_t));
    uv_tty_init(jl_io_loop,(uv_tty_t*)jl_stdin_tty,0,1);//stdin
    uv_tty_init(jl_io_loop,(uv_tty_t*)jl_stdout_tty,1,0);//stdout
    uv_tty_init(jl_io_loop,(uv_tty_t*)jl_stderr_tty,2,0);//stderr
    jl_stdin_tty->data=0;
    jl_stdout_tty->data=0;
    jl_stderr_tty->data=0;
    uv_tty_set_mode((uv_tty_t*)jl_stdin_tty,1); //raw input
    uv_tty_set_mode((uv_tty_t*)jl_stdout_tty,1); //raw output
#ifdef JL_GC_MARKSWEEP
    jl_gc_init();
    jl_gc_disable();
#endif
    jl_init_frontend();
    jl_init_types();
    jl_init_tasks(jl_stack_lo, jl_stack_hi-jl_stack_lo);
    jl_init_codegen();
    jl_an_empty_cell = (jl_value_t*)jl_alloc_cell_1d(0);

    jl_init_serializer();

    if (!imageFile) {
        jl_core_module = jl_new_module(jl_symbol("Core"));
        jl_current_module = jl_core_module;
        jl_init_intrinsic_functions();
        jl_init_primitives();
        jl_load("src/boot.jl");
        jl_get_builtin_hooks();
        jl_boot_file_loaded = 1;
        jl_init_box_caches();
    }

    if (imageFile) {
        JL_TRY {
            jl_restore_system_image(imageFile);
        }
        JL_CATCH {
            jl_printf(jl_stderr_tty, "error during init:\n");
            jl_show(jl_exception_in_transit);
            jl_printf(jl_stdout_tty, "\n");
            jl_exit(1);
        }
    }

#ifndef __WIN32__
	
    struct sigaction actf;
    memset(&actf, 0, sizeof(struct sigaction));
    sigemptyset(&actf.sa_mask);
    actf.sa_handler = fpe_handler;
    actf.sa_flags = 0;
    if (sigaction(SIGFPE, &actf, NULL) < 0) {
        jl_printf(jl_stderr_tty, "sigaction: %s\n", strerror(errno));
        jl_exit(1);
    }

    stack_t ss;
    ss.ss_flags = 0;
    ss.ss_size = SIGSTKSZ;
    ss.ss_sp = malloc(ss.ss_size);
    if (sigaltstack(&ss, NULL) < 0) {
        jl_printf(jl_stderr_tty, "sigaltstack: %s\n", strerror(errno));
        jl_exit(1);
    }
	
    struct sigaction act;
    memset(&act, 0, sizeof(struct sigaction));
    sigemptyset(&act.sa_mask);
    act.sa_sigaction = segv_handler;
    act.sa_flags = SA_ONSTACK | SA_SIGINFO;
    if (sigaction(SIGSEGV, &act, NULL) < 0) {
        jl_printf(jl_stderr_tty, "sigaction: %s\n", strerror(errno));
        jl_exit(1);
    }

#ifdef JL_GC_MARKSWEEP
    jl_gc_enable();
#endif
#endif
}

DLLEXPORT void jl_install_sigint_handler()
{
#ifdef __WIN32__
	//todo: switch to using SetConsoleCtrlHandler(sigint_handler, 1);
	signal(SIGINT, sigint_handler);
#else
    struct sigaction act;
    memset(&act, 0, sizeof(struct sigaction));
    sigemptyset(&act.sa_mask);
    act.sa_sigaction = sigint_handler;
    act.sa_flags = SA_SIGINFO;
    if (sigaction(SIGINT, &act, NULL) < 0) {
        jl_printf(jl_stderr_tty, "sigaction: %s\n", strerror(errno));
        jl_exit(1);
    }
#endif
	//printf("sigint installed\n");

}

DLLEXPORT
int julia_trampoline(int argc, char *argv[], int (*pmain)(int ac,char *av[]))
{
#ifdef COPY_STACKS
    // initialize base context of root task
    jl_root_task->stackbase = (char*)&argc;
    if (setjmp(jl_root_task->base_ctx)) {
        jl_switch_stack(jl_current_task, jl_jmp_target);
    }
#endif
    return pmain(argc, argv);
}

jl_function_t *jl_typeinf_func=NULL;

DLLEXPORT void jl_enable_inference(void)
{
    if (jl_typeinf_func != NULL) return;
    jl_typeinf_func = (jl_function_t*)jl_get_global(jl_base_module,
                                                    jl_symbol("typeinf_ext"));
}

static jl_value_t *core(char *name)
{
    return jl_get_global(jl_core_module, jl_symbol(name));
}

static jl_value_t *basemod(char *name)
{
    return jl_get_global(jl_base_module, jl_symbol(name));
}

jl_function_t *jl_method_missing_func=NULL;

// fetch references to things defined in boot.jl
void jl_get_builtin_hooks(void)
{
    jl_nothing      = core("nothing");
    jl_root_task->tls = jl_nothing;

    jl_char_type    = (jl_bits_type_t*)core("Char");
    jl_int8_type    = (jl_bits_type_t*)core("Int8");
    jl_uint8_type   = (jl_bits_type_t*)core("Uint8");
    jl_int16_type   = (jl_bits_type_t*)core("Int16");
    jl_uint16_type  = (jl_bits_type_t*)core("Uint16");
    jl_uint32_type  = (jl_bits_type_t*)core("Uint32");
    jl_uint64_type  = (jl_bits_type_t*)core("Uint64");

    jl_float32_type = (jl_bits_type_t*)core("Float32");
    jl_float64_type = (jl_bits_type_t*)core("Float64");

    jl_stackovf_exception =
        jl_apply((jl_function_t*)core("StackOverflowError"), NULL, 0);
    jl_divbyzero_exception =
        jl_apply((jl_function_t*)core("DivideByZeroError"), NULL, 0);
    jl_undefref_exception =
        jl_apply((jl_function_t*)core("UndefRefError"),NULL,0);
    jl_interrupt_exception =
        jl_apply((jl_function_t*)core("InterruptException"),NULL,0);
    jl_memory_exception =
        jl_apply((jl_function_t*)core("MemoryError"),NULL,0);

    jl_weakref_type = (jl_struct_type_t*)core("WeakRef");
    jl_ascii_string_type = (jl_struct_type_t*)core("ASCIIString");
    jl_utf8_string_type = (jl_struct_type_t*)core("UTF8String");
    jl_symbolnode_type = (jl_struct_type_t*)core("SymbolNode");

    jl_array_uint8_type =
        (jl_type_t*)jl_apply_type((jl_value_t*)jl_array_type,
                                  jl_tuple2(jl_uint8_type,
                                            jl_box_long(1)));
}

DLLEXPORT void jl_get_system_hooks(void)
{
    if (jl_method_missing_func) return; // only do this once

    jl_errorexception_type = (jl_struct_type_t*)basemod("ErrorException");
    jl_typeerror_type = (jl_struct_type_t*)basemod("TypeError");
    jl_loaderror_type = (jl_struct_type_t*)basemod("LoadError");
    jl_backtrace_type = (jl_struct_type_t*)basemod("BackTrace");

    jl_method_missing_func = (jl_function_t*)basemod("method_missing");
}
