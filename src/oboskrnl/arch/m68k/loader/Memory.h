#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Npl
{
    constexpr uintptr_t DontCare = 0;
    constexpr uintptr_t HhdmBase = 0x8000'0000;

    enum class MemoryType
    {
        Usable,
        Reclaimable,
        KernelModules,
    };

    void InitMemoryManager();
    void EnableMmu();
    size_t HhdmLimit();
    size_t GenerateLbpMemoryMap(void* store, size_t count);

    uintptr_t AllocPages(size_t count, MemoryType type = MemoryType::Reclaimable);
    void* AllocGeneral(size_t size);
    void* MapMemory(size_t length, uintptr_t vaddr, uintptr_t paddr = DontCare);
    uintptr_t GetMap(uintptr_t vaddr);
}

// NOTE(oberrow):
// Taken from https://github.com/DeanoBurrito/northport/blob/730f3c531f6190c238801f1abe180c1e0c8c61de/libs/np-syslib/include/Memory.h
namespace sl
{
    template<typename T>
    void memsetT(void* const start, T value, size_t valueCount)
    {
        T* const si = reinterpret_cast<T* const>(start);
        
        for (size_t i = 0; i < valueCount; i++)
            si[i] = value;
    }

    void* memset(void* const start, uint8_t val, size_t count);

    void* memcopy(const void* const source, void* const destination, size_t count);
    void* memcopy(const void* const source, size_t sourceOffset, void* const destination, size_t destOffset, size_t count);

    int memcmp(const void* const a, const void* const b, size_t count);
    int memcmp(const void* const a, size_t offsetA, const void* const b, size_t offsetB, size_t count);

    size_t memfirst(const void* const buff, uint8_t target, size_t upperLimit);
    size_t memfirst(const void* const buff, size_t offset, uint8_t target, size_t upperLimit);
}

//These MUST be provided by the program, we'll forward declare them here to make them available.
extern "C"
{
    void* malloc(size_t length);
    void free(void* ptr, size_t length);

    //clang requires these to exist for __builtin_xyz, while GCC provides its own.
    void* memcpy(void* dest, const void* src, size_t len);
    void* memset(void* dest, int value, size_t len);
    void* memmove(void* dest, const void* src, size_t len);
}