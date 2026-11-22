# libSceAgcDriver

## add Guest memory read lifetime

- [Import page hashes over read traps](../../../core/libs/prx/libSceAgcDriver/Execution/src/GuestMemory.cpp) (libSceAgcDriver, Linux) - an import leaves the pages a read trap covers out of its page hashes and treats them as changed, so it does not read them and does not wait for their resolver. A page check before the hashes still reads the trap as inaccessible when its range is not already in the cached page state, since on Linux the trap is a `PROT_NONE` mapping and no logical protection is queried, and an import of such a range throws `guest memory is not readable`; a trap armed between the query and the hashing threads still faults them into the resolver, which waits for the `GpuMutex` the import holds. Closes when: the page check leaves trapped pages out as well and no trap armed after the query reaches the hashing threads.
