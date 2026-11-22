# libkernel

## add Functional

- [signals](../../../core/libs/prx/libkernel/System/src/Signals.cpp) (libkernel) - an `SA_SIGINFO` handler receives a synthetic `siginfo` when the signal is delivered outside `sigwaitinfo`/`sigtimedwait`: a pending signal unblocked later and `raise` both build the record from the guest signal number, the `si_code` the call site knows (the raised code, or `SI_USER` for an unblocked pending one) and the process id, and leave the union (`si_addr`, `si_value`, the timer fields) zero, while a queued signal keeps the record `sigqueue` filled. The console's values for those fields on an unblocked pending signal and on `raise` are unmeasured. Closes when: those fields are measured on a PS5.
