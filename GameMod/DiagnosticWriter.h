#pragma once

#include <windows.h>
#include <array>
#include <cstring>
#include <cwchar>

namespace diagnostics
{
    // Only the writer thread touches files. Records own their complete path
    // and formatted data; the sink never reads live game or worker state.
    struct Record
    {
        static constexpr size_t kLineBytes = 1024;
        std::array<wchar_t, MAX_PATH> path{};
        std::array<char, kLineBytes> line{};
        DWORD length = 0;
    };

    class Writer
    {
    public:
        using Sink = void (*)(const Record&, void*);
        static constexpr size_t kCapacity = 64;

        Writer() = default;
        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;

        // The owner must keep this object and the module alive until
        // FinishShutdown succeeds. A stalled write cannot authorize unload.
        bool Start(Sink sink = AppendFile, void* context = nullptr)
        {
            if (thread_ != nullptr) return false;
            wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (wake_ == nullptr) return false;
            head_ = count_ = 0;
            InterlockedExchange(&stopping_, 0);
            sink_ = sink;
            context_ = context;
            thread_ = CreateThread(nullptr, 0, Run, this, 0, nullptr);
            if (thread_ == nullptr)
            {
                CloseHandle(wake_);
                wake_ = nullptr;
                return false;
            }
            return true;
        }

        // Called by the worker, never by the sink. Full/contended queues or
        // startup failure discard optional diagnostics instead of delaying input.
        bool Submit(const wchar_t* path, const char* line, size_t length)
        {
            if (thread_ == nullptr || path == nullptr || line == nullptr || length == 0 ||
                length >= Record::kLineBytes) return false;
            const size_t pathLength = std::wcslen(path);
            if (pathLength == 0 || pathLength >= MAX_PATH || !TryAcquireSRWLockExclusive(&lock_)) return false;
            const bool accepted = count_ < kCapacity && InterlockedCompareExchange(&stopping_, 0, 0) == 0;
            if (accepted)
            {
                Record& record = records_[(head_ + count_) % kCapacity];
                std::memcpy(record.path.data(), path, (pathLength + 1) * sizeof(wchar_t));
                std::memcpy(record.line.data(), line, length);
                record.line[length] = 0;
                record.length = static_cast<DWORD>(length);
                ++count_;
            }
            ReleaseSRWLockExclusive(&lock_);
            if (accepted) SetEvent(wake_);
            return accepted;
        }

        void BeginShutdown()
        {
            InterlockedExchange(&stopping_, 1);
            if (wake_ != nullptr) SetEvent(wake_);
        }

        bool FinishShutdown()
        {
            if (thread_ == nullptr) return true;
            if (WaitForSingleObject(thread_, 0) != WAIT_OBJECT_0) return false;
            CloseHandle(thread_);
            CloseHandle(wake_);
            thread_ = wake_ = nullptr;
            return true;
        }

    private:
        HANDLE thread_ = nullptr, wake_ = nullptr;
        SRWLOCK lock_ = SRWLOCK_INIT;
        volatile LONG stopping_ = 0;
        std::array<Record, kCapacity> records_{};
        size_t head_ = 0, count_ = 0;
        Sink sink_ = nullptr;
        void* context_ = nullptr;

        bool Take(Record& record)
        {
            AcquireSRWLockExclusive(&lock_);
            const bool available = count_ != 0;
            if (available)
            {
                record = records_[head_];
                head_ = (head_ + 1) % kCapacity;
                --count_;
            }
            ReleaseSRWLockExclusive(&lock_);
            return available;
        }

        static DWORD WINAPI Run(void* context)
        {
            auto& writer = *static_cast<Writer*>(context);
            for (;;)
            {
                WaitForSingleObject(writer.wake_, INFINITE);
                Record record{};
                while (writer.Take(record)) writer.sink_(record, writer.context_);
                // Recheck the queue after observing shutdown: a producer may
                // have queued its last record just before shutdown began.
                if (InterlockedCompareExchange(&writer.stopping_, 0, 0) != 0)
                {
                    while (writer.Take(record)) writer.sink_(record, writer.context_);
                    return 0;
                }
            }
        }

        static void AppendFile(const Record& record, void*)
        {
            const HANDLE file = CreateFileW(record.path.data(), FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return;
            DWORD written = 0;
            WriteFile(file, record.line.data(), record.length, &written, nullptr);
            CloseHandle(file);
        }
    };
}
