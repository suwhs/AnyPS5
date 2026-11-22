# libkernel

## add [Aliased direct memory stays readable]

- [DoMprotect](../../../core/libs/prx/libkernel/DirectMemory/DirectMemory.cpp) (libkernel) - a request that clears `PROT_READ` over a range whose physical pages another mapping of the same direct memory shares keeps `PROT_READ`, because the emulator's own reads and the other alias must keep working; the guest's protection is then not honoured on such a range: a read-only alias cannot be made unreadable, a guard page never faults, and a title that relies on either behaves differently from a console. The general model - a protection belongs to the mapping, not to the physical pages - is not implemented, and an internal read that would otherwise fault is not reported. Unmeasured on a console; a title that decommits aliased direct memory without this aborts in the guest
