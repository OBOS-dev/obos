#include <obos/syscall.h>
#include <obos/error.h>
#include <stdio.h>
#include <assert.h>

typedef struct memstat
{
    // The size of all allocated (committed) memory.
    size_t committedMemory;
    // The size of all memory within this context which has been paged out.
    size_t paged;
    // The size of all pageable memory (memory that can be paged out).
    size_t pageable;
    // The size of all non-pageable memory (memory that cannot be paged out).
    size_t nonPaged;
    size_t resv;
    // The amount of total page faults on this context.
    size_t pageFaultCount;
    // The amount of soft page faults on this context.
    size_t softPageFaultCount;
    // The amount of hard page faults on this context.
    size_t hardPageFaultCount;
    // The amount of page faults on this context since the last sampling interval.
    size_t pageFaultCountSinceSample;
    // The amount of soft page faults on this context since the last sampling interval.
    size_t softPageFaultCountSinceSample;
    // The amount of hard page faults on this context since the last sampling interval.
    size_t hardPageFaultCountSinceSample;
} memstat;

void get_div_and_unit(size_t val, char* unit, size_t *divisor)
{
    if (val > 1024)
        *unit = 'K';
    if (val > 1024*1024)
        *unit = 'M';
    if (val > 1024*1024*1024)
        *unit = 'G';
    switch (*unit) {
        case 'B': *divisor = 1; break;
        case 'K': *divisor = 1024; break;
        case 'M': *divisor = 1024*1024; break;
        case 'G': *divisor = 1024*1024*1024; break;
        default: assert(!"invalid unit");
    }
}

int main()
{
    size_t pmem = syscall0(Sys_GetUsedPhysicalMemoryCount);
    size_t cmem = syscall0(Sys_GetCachedByteCount);
    char unit = 'B';
    size_t divisor = 0;
    get_div_and_unit(pmem, &unit, &divisor);
    printf("Physical memory usage: %f%c\n", pmem/(float)divisor, unit);
    if (cmem != OBOS_STATUS_UNIMPLEMENTED)
    {
        get_div_and_unit(cmem, &unit, &divisor);
        printf("Cached Memory: %f%c\n", cmem/(float)divisor, unit);
    }
    memstat global_memory_usage = {};
    syscall2(Sys_ContextGetStat, HANDLE_INVALID, &global_memory_usage);
    get_div_and_unit(global_memory_usage.committedMemory, &unit, &divisor);
    printf("Total committed memory: %f%c\n", global_memory_usage.committedMemory/(float)divisor, unit);
    get_div_and_unit(global_memory_usage.paged, &unit, &divisor);
    printf("Total paged memory: %f%c\n", global_memory_usage.paged/(float)divisor, unit);
    get_div_and_unit(global_memory_usage.pageable, &unit, &divisor);
    printf("Total pageable memory: %f%c\n", global_memory_usage.pageable/(float)divisor, unit);
    get_div_and_unit(global_memory_usage.nonPaged, &unit, &divisor);
    printf("Total non paged memory: %f%c\n", global_memory_usage.nonPaged/(float)divisor, unit);
    return 0;
}
