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

#include <stdint.h>
#include <atomic>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#include <Windows.h>
#include <synchapi.h>
#else
#include <vector>
#include <shared_mutex>
#include <mutex>
#include <utility> // std::move
#endif

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
    TinyRingBuffer() {}
    explicit TinyRingBuffer(size_t length)
    {
        init(int64_t(length));
    }
    TinyRingBuffer(const TinyRingBuffer & other) = delete;
    TinyRingBuffer(TinyRingBuffer && other)
    {
#ifdef _WIN32
        
#else
        std::scoped_lock lock{other.m_mutex};
        std::swap(m_head, other.m_head);
        std::swap(m_tail, other.m_tail);
        std::swap(m_count, other.m_count);
        m_buffer = std::move(other.m_buffer);
#endif
    }
    virtual ~TinyRingBuffer()
    {
        free();
    }
    TinyRingBuffer& operator=(const TinyRingBuffer & other) = delete;
    TinyRingBuffer& operator=(TinyRingBuffer && other)
    {
        std::scoped_lock lock{m_mutex, other.m_mutex};
        m_head = other.m_head;
        other.m_head = 0;
        m_tail = other.m_tail;
        other.m_tail = 0;
        m_count = other.m_count;
        other.m_count = 0;
        m_buffer = std::move(other.m_buffer);
        return *this;
    }

    TinyRingBufferStatus enqueue(const T& src)
    {
#ifdef _WIN32
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
#else
        std::scoped_lock lock(m_mutex);
        if (m_count >= m_buffer.size())
            return TinyRingBufferStatus::BUFFER_FULL;

        int64_t i = m_head;
        m_head = (m_head + 1) % m_buffer.size();
        m_buffer[i] = src;
        m_count++;
#endif
        return TinyRingBufferStatus::SUCCESS;
    }

    TinyRingBufferStatus enqueue(const T* src, int64_t elements)
    {
#ifdef _WIN32
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
#else
        std::scoped_lock lock(m_mutex);
        if (m_count + elements > m_buffer.size())
            return TinyRingBufferStatus::BUFFER_FULL;
        for (int64_t i = 0; i < elements; ++i)
        {
            auto sts = enqueue(src[i]);
            if (sts != TinyRingBufferStatus::SUCCESS)
                return sts;
        }
#endif
        return TinyRingBufferStatus::SUCCESS;
    }

    TinyRingBufferStatus dequeue(T* dst)
    {
#ifdef _WIN32
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
#else
        std::scoped_lock lock(m_mutex);
        if (m_count <= 0)
            return TinyRingBufferStatus::BUFFER_EMPTY;

        int64_t i = m_tail;
        m_tail = (m_tail + 1) % m_buffer.size();
        if (dst)
            *dst = m_buffer[i];
        m_count--;
#endif
        return TinyRingBufferStatus::SUCCESS;
    }

    TinyRingBufferStatus dequeue(T* dst, int64_t elements, int64_t* elements_dequeued)
    {
#ifdef _WIN32
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
#else
        std::scoped_lock lock(m_mutex);
      
        if (elements_dequeued != nullptr)
          *elements_dequeued = 0; 

        if (m_count <= 0)
            return TinyRingBufferStatus::BUFFER_EMPTY;

        int64_t try_dequeue = std::min(m_count, elements);

        for (int64_t n = 0; n < try_dequeue; ++n)
        {
            int64_t i = m_tail;
            m_tail = (m_tail + 1) % m_buffer.size();
            if (dst)
                dst[n] = m_buffer[i];
            m_count--;
        }

        if (elements_dequeued != nullptr)
            *elements_dequeued = try_dequeue;
#endif
        return TinyRingBufferStatus::SUCCESS;
    }

    [[nodiscard]]
    int64_t length() const
    {
#ifdef _WIN32
        return m_buffer_size / sizeof(T);
#else
        std::shared_lock lock(m_mutex);
        return m_buffer.size();
#endif
    }

    [[nodiscard]]
    bool empty() const
    {
#ifdef _WIN32
        return m_used_bytes == 0;
#else
        std::shared_lock lock(m_mutex);
        return m_head == m_tail;
#endif
    }

    [[nodiscard]]
    int64_t count() const
    {
#ifdef _WIN32
        return m_used_bytes / sizeof(T);
#else
        std::shared_lock lock(m_mutex);
        return m_count;
#endif
    }

private:

    TinyRingBufferStatus init(int64_t buffer_length)
    {
#ifdef _WIN32
        int64_t buffer_size = buffer_length * sizeof(T);
        buffer_size = (buffer_size + (0xffff-1)) & ~0xffff;
        if (buffer_size & 0xffff)
            return TinyRingBufferStatus::INVALID_ARGUMENT;

        m_buffer_size = buffer_size; // Make it a multiple of a page size, 64K  

        int tries = 0;
        while (tries < 5 && m_buffer == nullptr)
        {
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
#else
        std::scoped_lock lock(m_mutex);
        m_buffer.resize(buffer_length);
#endif
        return TinyRingBufferStatus::SUCCESS;
    }

    TinyRingBufferStatus free()
    {
#ifdef _WIN32
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
#else
        std::scoped_lock lock(m_mutex);
        m_head = 0;
        m_tail = 0;
        m_count = 0;
        std::vector<T>().swap(m_buffer);
#endif
        return TinyRingBufferStatus::SUCCESS;
    }

    int64_t m_head = 0;
    int64_t m_tail = 0;
#ifdef _WIN32
    std::atomic_int64_t m_used_bytes = 0;
    int64_t m_buffer_size = 0;
    uint8_t* m_buffer = nullptr;
    SRWLOCK m_lock = SRWLOCK_INIT;
    void* m_map = nullptr;
    HANDLE m_handle = NULL;
#else
    int64_t m_count = 0; // tail and head can not be used. is the buffer empty or full?
    std::vector<T> m_buffer;
    mutable std::shared_mutex m_mutex;
#endif
};
} // namespace utils
