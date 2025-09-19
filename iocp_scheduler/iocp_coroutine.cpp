#include <coroutine>
#include <unordered_map>
#include <windows.h>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <sstream>
#include <memory>

struct Task {
    struct promise_type {
        Task get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

class IocpScheduler {
private:
    HANDLE iocp_handle;
    std::unordered_map<HANDLE, std::coroutine_handle<>> io_handles;

public:
    IocpScheduler() {
        iocp_handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
        if (iocp_handle == NULL) {
            throw std::runtime_error("CreateIoCompletionPort failed");
        }
    }

    ~IocpScheduler() {
        CloseHandle(iocp_handle);
    }

    void register_io(HANDLE file_handle, std::coroutine_handle<> handle) {
        if (io_handles.find(file_handle) == io_handles.end()) {
            io_handles[file_handle] = handle;

            if (CreateIoCompletionPort(file_handle, iocp_handle, (ULONG_PTR)file_handle, 0) == NULL) {
                throw std::runtime_error("CreateIoCompletionPort failed to associate file handle");
            }
        }
    }

    void run() {
        while (true) {
            DWORD bytes_transferred = 0;
            ULONG_PTR completion_key = 0;
            LPOVERLAPPED overlapped = nullptr;

            BOOL success = GetQueuedCompletionStatus(
                iocp_handle,
                &bytes_transferred,
                &completion_key,
                &overlapped,
                INFINITE);

            if (completion_key != 0) {
                HANDLE ready_handle = (HANDLE)completion_key;
                if (auto it = io_handles.find(ready_handle); it != io_handles.end()) {
                    it->second.resume();
                }
            }
        }
    }
};

struct AsyncReadAwaiter {
    IocpScheduler& sched;
    HANDLE file_handle;
    std::unique_ptr<char[]> buffer;
    DWORD buffer_size;
    OVERLAPPED overlapped;
    DWORD bytes_read;

    AsyncReadAwaiter(IocpScheduler& s, HANDLE file, DWORD size)
        : sched(s), file_handle(file), buffer_size(size), bytes_read(0) {
        buffer = std::make_unique<char[]>(size);
        ZeroMemory(&overlapped, sizeof(OVERLAPPED));
    }

    bool await_ready() const {
        return false;
    }

    void await_suspend(std::coroutine_handle<> h) {
        sched.register_io(file_handle, h);
        
        if (!ReadFile(file_handle, buffer.get(), buffer_size, &bytes_read, &overlapped)) {
            DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                std::stringstream ss;
                ss << "ReadFile failed, error " << error;
                throw std::runtime_error(ss.str());
            }
        }
    }

    std::string await_resume() {
        DWORD bytes_transferred = 0;
        if (!GetOverlappedResult(file_handle, &overlapped, &bytes_transferred, FALSE)) {
            DWORD error = GetLastError();
            std::stringstream ss;
            ss << "GetOverlappedResult failed, error " << error;
            throw std::runtime_error(ss.str());
        }

        return std::string(buffer.get(), bytes_transferred);
    }
};

Task async_read_file(IocpScheduler& sched, const char* path) {
    HANDLE file_handle = CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        NULL);

    if (file_handle == INVALID_HANDLE_VALUE) {
        std::stringstream ss;
        ss << "CreateFile failed, error " << GetLastError();
        throw std::runtime_error(ss.str());
    }

    while (true) {
        auto data = co_await AsyncReadAwaiter(sched, file_handle, 4096);
        std::cout << "Read " << data.size() << " bytes\n";
        if (data.size() == 0) {
            break;
        }
    }

    CloseHandle(file_handle);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: sample file_path" << std::endl;
        return 1;
    }

    IocpScheduler scheduler;
    async_read_file(scheduler, argv[1]);
    scheduler.run();
    return 0;
}
