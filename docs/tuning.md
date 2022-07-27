# Performance Tuning TCMalloc

There are three user accessible controls that we can use to performance tune
TCMalloc:

*   The logical page size for TCMalloc (4KiB, 8KiB, 32KiB, 256KiB)
*   The per-thread or per-cpu cache sizes
*   The rate at which memory is released to the OS

None of these tuning parameters are clear wins, otherwise they would be the
default. We'll discuss the advantages and disadvantages of changing them.

## The Logical Page Size for TCMalloc:

This is determined at compile time by linking in the appropriate version of
TCMalloc. The page size indicates the unit in which TCMalloc manages memory. The
default is in 8KiB chunks, there are larger options of 32KiB and 256KiB. There
is also the 4KiB page size used by the small-but-slow allocator.

A smaller page size allows TCMalloc to provide memory to an application with
less waste. Waste comes about through two issues:

*   Left-over memory when rounding larger requests to the page size (eg a
    request for 62 KiB might get rounded to 64 KiB).
*   Pages of memory that are stuck because they have a single in use allocation
    on the page, and therefore cannot be repurposed to hold a different size of
    allocation.

The second of these points is worth elucidating. For small allocations TCMalloc
will fit multiple objects onto a single page.

So if you request 512 bytes, then an entire page will be devoted to 512 byte
objects. If the size of that page is 4KiB we get 8 objects, if the size of that
page is 256KiB we get 512 objects. That page can only be used for 512 byte
objects until all the objects on the page have been freed.

If you have 8 objects on a page, there's a reasonable chance that all 8 will
become free at the same time, and we can repurpose the page for objects of a
different size. If there's 512 objects on that page, then it is very unlikely
that all the objects will become freed at the same time, so that page will
probably never become entirely free and will probably hang around, potentially
containing only a few in-use objects.

The consequence of this is that large pages tend to lead to a larger memory
footprint. There's also the issue that if you want one object of a size, you
need to allocate a whole page.

The advantage of managing objects using larger page sizes are:

*   Objects of the same size are better clustered in memory. If you need 512 KiB
    of 8 byte objects, then that's two 256 KiB pages, or 128 x 4 KiB pages. If
    memory is largely backed by hugepages, then with large pages in the worst
    case we can map the entire demand with two large pages, whereas small pages
    could take up to 128 entries in the TLB.
*   There's a structure called the `PageMap` which enables TCMalloc to lookup
    information about any allocated memory. If we use large pages the pagemap
    needs fewer entries and can be much smaller. This makes it more likely that
    it is cache resident. However, sized delete substantially reduced the number
    of times that we need to consult the pagemap, so the benefit from larger
    pages is reduced.

**Suggestion:** The default of 8KiB page sizes is probably good enough for most
applications. However, if an application has a heap measured in GiB it may be
worth looking at using large page sizes.

**Suggestion:** Consider small-but-slow if it is more important to minimise
memory footprint over performance.

**Note:** Class sizes are determined on a per-page-size basis. So changing the
page size will implicitly change the class sizes used. Class sizes are selected
to be memory-efficient for the applications using that page size. If an
application changes page size, there may be a performance or memory impact from
the different selection of class sizes.

## Per-thread/per-cpu Cache Sizes

The default is for TCMalloc to run in per-cpu mode as this is faster; however,
there are few applications which have not yet transitioned. The plan is to move
these across at some point soon.

Increasing the size of the cache is an obvious way to improve performance. The
larger the cache the less frequently memory needs to be fetched from the central
caches. Returning memory from the cache is substantially faster than fetching
from the central cache.

The size of the per-cpu caches is controlled by
`tcmalloc::MallocExtension::SetMaxPerCpuCacheSize`. This controls the limit for
each CPU, so the total amount of memory for application could be much larger
than this. Memory on CPUs where the application is no longer able to run can be
freed by calling `tcmalloc::MallocExtension::ReleaseCpuMemory`.

In contrast `tcmalloc::MallocExtension::SetMaxTotalThreadCacheBytes` controls
the _total_ size of all thread caches in the application.

**Suggestion:** The default cache size is typically sufficient, but cache size
can be increased (or decreased) depending on the amount of time spent in
TCMalloc code, and depending on the overall size of the application (a larger
application can afford to cache more memory without noticeably increasing its
overall size).

## Memory Releasing

`tcmalloc::MallocExtension::ReleaseMemoryToSystem` makes a request to release
`n` bytes of memory to TCMalloc. This can keep the memory footprint of the
application down to a minimal amount, however it should be considered that this
just reduces the application down from it's peak memory footprint, and does not
make that peak memory footprint smaller.

There are two disadvantages of releasing memory aggressively:

*   Memory that is unmapped may be immediately needed, and there is a cost to
    faulting unmapped memory back into the application.
*   Memory that is unmapped at small granularity will break up hugepages, and
    this will cause some performance loss due to increased TLB misses.

**Note:** Release rate is not a panacea for memory usage. Jobs should be
provisioned for peak memory usage to avoid OOM errors. Setting a release rate
may enable an application to exceed the memory limit for short periods of
time without triggering an OOM. A release rate is also a good citizen behavior
as it will enable the system to use spare capacity memory for applications
which are are under provisioned. However, it is not a substitute for setting
appropriate memory requirements for the job.

**Note:** Memory is released from the `PageHeap` and stranded per-cpu caches.
It is not possible to release memory from other internal structures, like
the `CentralFreeList`.

**Suggestion:** The default release rate is probably appropriate for most
applications. In situations where it is tempting to set a faster rate it is
worth considering why there are memory spikes, since those spikes are likely to
cause an OOM at some point.
