#pragma once

#include <int.h>

#include <locks/event.h>
#include <locks/mutex.h>

enum signal_default_action
{
    // When this is the default, the signal runner returns normally.
    SIGNAL_DEFAULT_IGNORE,
    // When this is the default, the current thread is exited.
    SIGNAL_DEFAULT_TERMINATE_PROC,
    // Blocks the thread.
    SIGNAL_DEFAULT_STOP,
    // Readies the thread.
    SIGNAL_DEFAULT_CONTINUE,
};

// Public
enum {
    SIGHUP = 1 , SIGINT,       SIGQUIT,     SIGILL,       SIGTRAP,
    SIGABRT    , SIGBUS,       SIGFPE,      SIGKILL,      SIGUSR1,
    SIGSEGV    , SIGUSR2,      SIGPIPE,     SIGALRM,      SIGTERM,
    SIGSTKFLT  , SIGCHLD,      SIGCONT,     SIGSTOP,      SIGTSTP,
    SIGTTIN    , SIGTTOU,      SIGURG,      SIGXCPU,      SIGXFSZ,
    SIGVTALRM  , SIGSYS,       SIGMAX = 64,
};

// Public
typedef uint64_t sigset_t;

// Public
enum {
    SA_SIGINFO   = BIT(0), 
    SA_ONSTACK   = BIT(1),
    SA_RESETHAND = BIT(2),
    SA_NODEFER   = BIT(3),
    SA_NOCLDWAIT = BIT(4), // unimplemented
    SA_NOCLDSTOP = BIT(5), // unimplemented
};
// Public
typedef struct siginfo_t
{
    int signum;
    int sigcode;
    struct thread* sender;
    void* addr;
    int status;
    union {
        void* ptr;
        uintptr_t integer;
    } udata;
} siginfo_t;
// Public
typedef struct user_sigaction {
    union {
        void(*handler)(int signum);
        void(*sa_sigaction)(int signum, siginfo_t* info, void* unknown);
    } un;
    // NOTE(oberrow): Set to __mlibc_restorer in the mlibc sysdeps.
    uintptr_t trampoline_base; // required
    uint32_t  flags;
} user_sigaction;
typedef struct sigaction {
    union {
        void(*handler)(int signum);
        void(*sa_sigaction)(int signum, siginfo_t* info, void* unknown);
    } un;
    // NOTE(oberrow): Set to __mlibc_restorer in the mlibc sysdeps.
    uintptr_t trampoline_base; // required
    uint32_t  flags;
    // The following fields are not to be carried to userspace.
    // Fields of 'siginfo_t' are set with this
    uintptr_t udata;
    void* addr;
    int status;
    int sigcode;
    struct thread* sender;
} sigaction;
// Internal
typedef struct signal_header {
    // NOTE: To get the first signal to dispatch, use __builtin_ctzll(pending & ~masked)
    sigset_t  pending;
    sigset_t  mask;
    uintptr_t sp;
    size_t    stack_size; // Unused in the actual signal implemtation, only exists for Sys_SigAltStack.
    mutex     lock; // take when modifying this structure.
    event     event; // set when a signal runs, clear when it exits (sigreturn)
} signal_header;