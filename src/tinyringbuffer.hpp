/*
MIT License

Copyright (c) 2020 Markus Lindelöw

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN

#include <stdint.h>
#include <Windows.h>
#include <synchapi.h>
#include <atomic>

namespace utils
{
enum class TinyRingBufferStatus
{
    SUCCESS = 0,
    BUFFER_EMPTY = 1,
    BUFFER_FULL = 2,
    MEMORY_ERROR = 3,
    INVALID_ARGUMENT = 4
};

template <typename T>
class TinyRingBuffer
{
public:
    TinyRingBuffer()
        : m_head(0)
        , m_tail(0)
        , m_used_bytes(0)
        , m_buffer_size(0) // Make it a multiple of a page size, 64K
        , m_lock(SRWLOCK_INIT)
        , m_buffer(nullptr)
        , m_handle()
        , m_map()
    {
    }

    ~TinyRingBuffer()
    {
        free();
    }

    // To make it easier to use BSS
    TinyRingBufferStatus init(int64_t buffer_size)
    {
        if (buffer_size & 0xffff)
            return TinyRingBufferStatus::INVALID_ARGUMENT;

        int tries = 0;
        while (tries < 5 && m_buffer == nullptr)
        {
            m_buffer_size = buffer_size;
            int64_t virtual_size = m_buffer_size * 3; // todo(markusl): why 3 and not 2 ?
            m_buffer = (uint8_t*)VirtualAlloc(nullptr, (SIZE_T)virtual_size, MEM_RESERVE, PAGE_NOACCESS);
            VirtualFree(m_buffer, 0, MEM_RELEASE);

            if (m_buffer != nullptr)
            {
                m_handle = CreateFileMappingA(
                    INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD)(virtual_size >> 32), (DWORD)(virtual_size & 0xffffffffu), nullptr);
                if (m_handle == nullptr)
                {
                    free();
                }
                else
                {
                    m_map = MapViewOfFileEx(m_handle, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)m_buffer_size, m_buffer);
                    void* map2 = MapViewOfFileEx(m_handle, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)m_buffer_size, m_buffer + m_buffer_size);

                    if (m_map == nullptr || map2 == nullptr)
                        free();
                }

                tries++;
            }
        }

        if (m_buffer == nullptr)
            return TinyRingBufferStatus::MEMORY_ERROR;
        return TinyRingBufferStatus::SUCCESS;
    }

    TinyRingBufferStatus free()
    {
        bool a = UnmapViewOfFile(m_map);
        bool b = UnmapViewOfFile((uint8_t*)m_map + m_buffer_size);
        if (a == 0 || b == 0)
        {
            return TinyRingBufferStatus::MEMORY_ERROR;
        }
        m_map = nullptr;
        m_buffer = 0;
        m_buffer_size = 0;
        m_head = 0;
        m_tail = 0;
        m_used_bytes = 0;

        if (m_handle != nullptr)
        {
            CloseHandle(m_handle);
            m_handle = nullptr;
        }

        return TinyRingBufferStatus::SUCCESS;
    }

    // Non-thread safe ptr parameter
    TinyRingBufferStatus allocate(int64_t elements, T** ptr)
    {
        T* p = nullptr;
        if (ptr != nullptr)
        {
            p = data();
        }

        const int64_t byte_size = (int64_t)sizeof(T) * elements;
        AcquireSRWLockExclusive(&m_lock);
        if (m_used_bytes + byte_size > m_buffer_size)
        {
            ReleaseSRWLockExclusive(&m_lock);
            if (ptr != nullptr)
                *ptr = nullptr;
            return TinyRingBufferStatus::BUFFER_FULL;
        }

        m_head = (m_head + byte_size) % m_buffer_size;
        m_used_bytes += byte_size;

        ReleaseSRWLockExclusive(&m_lock);
        if (ptr != nullptr)
            *ptr = p;
        return TinyRingBufferStatus::SUCCESS;
    }

    TinyRingBufferStatus enqueue(const T& src)
    {
        const int64_t byte_size = (int64_t)sizeof(T);
        AcquireSRWLockExclusive(&m_lock);
        if (m_used_bytes + byte_size > m_buffer_size)
        {
            ReleaseSRWLockExclusive(&m_lock);
            return TinyRingBufferStatus::BUFFER_FULL;
        }

        *reinterpret_cast<T*>(m_buffer + m_head) = src;
        m_head = (m_head + byte_size) % m_buffer_size;
        m_used_bytes += byte_size;

        ReleaseSRWLockExclusive(&m_lock);
        return TinyRingBufferStatus::SUCCESS;
    }

    TinyRingBufferStatus enqueue(const T* src, int64_t elements)
    {
        const int64_t byte_size = (int64_t)sizeof(T) * elements;
        AcquireSRWLockExclusive(&m_lock);
        if (m_used_bytes + byte_size > m_buffer_size)
        {
            ReleaseSRWLockExclusive(&m_lock);
            return TinyRingBufferStatus::BUFFER_FULL;
        }

        for (int n = 0; n < elements; ++n)
            reinterpret_cast<T*>(m_buffer + m_head)[n] = src[n];

        m_head = (m_head + byte_size) % m_buffer_size;
        m_used_bytes += byte_size;

        ReleaseSRWLockExclusive(&m_lock);
        return TinyRingBufferStatus::SUCCESS;
    }

    TinyRingBufferStatus dequeue(T* dst)
    {
        const int64_t byte_size = (int64_t)sizeof(T);
        AcquireSRWLockExclusive(&m_lock);

        if (m_used_bytes - byte_size < 0)
        {
            ReleaseSRWLockExclusive(&m_lock);
            return TinyRingBufferStatus::BUFFER_EMPTY;
        }
        *dst = *reinterpret_cast<T*>(m_buffer + m_tail);
        memset(m_buffer + m_tail, 0xAB, sizeof(T));

        m_tail = (m_tail + byte_size) % m_buffer_size;
        m_used_bytes -= byte_size;

        ReleaseSRWLockExclusive(&m_lock);
        return TinyRingBufferStatus::SUCCESS;
    }

    TinyRingBufferStatus dequeue(T* dst, int64_t elements)
    {
        const int64_t byte_size = (int64_t)sizeof(T) * elements;
        AcquireSRWLockExclusive(&m_lock);
        if (m_used_bytes - byte_size < 0)
        {
            ReleaseSRWLockExclusive(&m_lock);
            return BUFFER_EMPTY;
        }

        for (int n = 0; n < elements; ++n)
            dst[n] = reinterpret_cast<T*>(m_buffer + m_head)[n];

        memset(m_buffer + m_tail, 0xAB, sizeof(T) * elements);

        m_tail = (m_tail + byte_size) % m_buffer_size;
        m_used_bytes -= byte_size;

        ReleaseSRWLockExclusive(&m_lock);
        return SUCCESS;
    }

    int64_t buffer_size() const
    {
        return m_buffer_size;
    }

    bool empty() const
    {
        return m_used_bytes == 0;
    }

    bool is_inited() const
    {
        return m_buffer != nullptr;
    }

    // non-thread safe access
    T* data()
    {
        return reinterpret_cast<T*>(m_buffer + m_tail);
    }

    int64_t count() const
    {
        return m_used_bytes / sizeof(T);
    }

protected:
    int64_t m_head;
    int64_t m_tail;
    std::atomic_int64_t m_used_bytes;
    int64_t m_buffer_size;
    uint8_t* m_buffer;

    SRWLOCK m_lock;
    void* m_map;
    HANDLE m_handle;
};
} // namespace utils
