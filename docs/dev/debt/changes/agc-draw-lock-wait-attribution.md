# libSceAgcDriver

## add Draw-phase lock attribution

- [Draw lock-wait phase](../../../core/libs/prx/libSceAgcDriver/Execution/src/Driver/Draw/Draw.cpp) (libSceAgcDriver) - on the pipelined path the `lock wait` bucket of the `[draw] driver phases` line is not the GPU mutex wait: `DrawPhaseTiming::Phase(DrawRowLockWait)` there runs after `drawWriteRanges` and `DrawPipeline::Queue0().Enqueue` without taking `GuestMemory::GpuMutex()`, so it credits write-range collection and the producer's wait for pipeline room to the lock. Only the inline `lockForDraw()` and `lockedPrepare` sites credit a real `gpuLock.lock()` to that bucket. `DrawPipeline::report` counts the enqueue's room wait separately, but the per-packet phase line does not. Closes when: the pipelined path reports its enqueue and room wait under their own phase.
