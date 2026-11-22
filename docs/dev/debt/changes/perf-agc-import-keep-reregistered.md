# libSceAgcDriver

## add Functional

- [Host import over a released range](../../../core/libs/prx/libSceAgcDriver/Graphics/src/GuestBufferMemory.cpp) (libSceAgcDriver) - with `APS5_IMPORT_KEEP_REREGISTERED=1` an import of a large mapping may cover a range the guest released, until the mapping changes. A multi-piece import is compared against the lease by the extent and flags of the pieces that are still registered and keeps the ones that went away, and a stale registry reuses any existing import that covers the request and passes that comparison, not only a joined one. The released range keeps being served from the mapping the import pins, so a title that releases a range inside such an import and registers other content over it can be served the import's bytes there until the composition changes and the registry is reconciled. Only titles whose memory is a shared backing (direct memory is one memfd on Linux) are intended; RADV and the console are not measured.
