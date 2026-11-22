# libSceAgcDriver

## add Functional

- [DCC key trace](../../../core/libs/prx/libSceAgcDriver/Graphics/src/DccMetadata.cpp) (libSceAgcDriver) - `APS5_TRACE_DCC_KEYS=1` prints from the key-store and key-fill paths, which run under `GuestMemory::GpuMutex`; an unbounded trace on a long run fills the log from under the lock and stalls the title while the log's reader does not drain it (measured at about 260 key stores per second, which stopped a live run at the loading-to-scene transition). Every such print stops after 64 lines per process (`TraceDccKeyLine`), after which the counters in the `[dcc]` line carry the totals; the remaining prints still block while the lock is held, so a blocked stderr still stalls the first ones. Closes when: the trace records into memory and writes it outside the lock.
