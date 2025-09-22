// iocp_coroutine.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//
#include <coroutine>
#include <unordered_map>
#include <windows.h>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <sstream>
#include <memory>
#include <signal.h>

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
        for(auto handle : io_handles) {
            std::cout << "coroutine destroy" << std::endl;
            handle.second.destroy();
        }
        CloseHandle(iocp_handle);
    }

    void register_io(HANDLE file_handle, std::coroutine_handle<> handle) {
        if (io_handles.find(file_handle) == io_handles.end()) {
            io_handles[file_handle] = handle;

            if (CreateIoCompletionPort(file_handle, iocp_handle, (ULONG_PTR)file_handle, 0) == NULL) {
                std::string errmsg = "CreateIoCompletionPort failed to associate file handle"; 
                std::cerr << errmsg << std::endl; 
                throw std::runtime_error(errmsg);
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

            if (completion_key == 0) {
                std::cout << "IOCP ready to quit" << std::endl; 
                break; 
            }
            else {
                HANDLE ready_handle = (HANDLE)completion_key;
                if (auto it = io_handles.find(ready_handle); it != io_handles.end()) {
                    it->second.resume();
                }
            }
        }
    }

    void exit(int signo) {
        std::cout << "caught signal " << signo << ", prepare to quit!" << std::endl; 
        PostQueuedCompletionStatus(iocp_handle, 0, (ULONG_PTR)0, NULL);
    }
};


struct AsyncReadAwaiter {
    IocpScheduler& sched;
    HANDLE file_handle;
    std::unique_ptr<char[]> buffer;
    DWORD buffer_size;
    LARGE_INTEGER &offset; 
    OVERLAPPED overlapped;
    DWORD bytes_read;

    AsyncReadAwaiter(IocpScheduler& s, HANDLE file, LARGE_INTEGER &off, DWORD size)
        : sched(s), file_handle(file), buffer_size(size), offset(off), bytes_read(0) {
        buffer = std::make_unique<char[]>(size);
        ZeroMemory(&overlapped, sizeof(OVERLAPPED));
    }

    bool await_ready() const {
        return false;
    }

    bool await_suspend(std::coroutine_handle<> h) {
        sched.register_io(file_handle, h);

        overlapped.Offset = offset.LowPart;
        overlapped.OffsetHigh = offset.HighPart;
        //std::cout << "ReadFile from " << offset.QuadPart << std::endl;
        if (!ReadFile(file_handle, buffer.get(), buffer_size, &bytes_read, &overlapped)) {
            DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                std::stringstream ss;
                ss << "ReadFile failed, error " << error;
                std::cerr << ss.str() << std::endl; 
                throw std::runtime_error(ss.str());
            }
        }
        else {
            // if immediately success, not hangup
            std::cout << "immediately success, read = " << bytes_read << std::endl; 
        }
        return bytes_read > 0 ? false : true;
    }

    std::string await_resume() {
        DWORD bytes_transferred = 0;
        if (bytes_read > 0) {
            bytes_transferred = bytes_read;
        }
        else {
            if (!GetOverlappedResult(file_handle, &overlapped, &bytes_transferred, FALSE)) {
                DWORD error = GetLastError();
                if (error != ERROR_HANDLE_EOF) {
                    std::stringstream ss;
                    ss << "GetOverlappedResult failed, error " << error;
                    std::cerr << ss.str() << std::endl;
                    throw std::runtime_error(ss.str());
                }
                else {
                    return "";
                }
            }
        }

        offset.QuadPart += bytes_transferred; 
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
        std::cerr << ss.str() << std::endl; 
        throw std::runtime_error(ss.str());
    }

    LARGE_INTEGER offset = { 0 };
    while (true) {
        auto data = co_await AsyncReadAwaiter(sched, file_handle, offset, 1024);
        std::cout << "Read [" << file_handle << "] " << data.size() << " bytes" << std::endl;
        if (data.size() == 0) {
            break;
        }
    }

    CloseHandle(file_handle);
}

IocpScheduler g_scheduler;

void on_user_exit(int signo) {
    g_scheduler.exit(signo); 
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: sample file" << std::endl;
        return 1;
    }

    signal(SIGINT, on_user_exit); 
    async_read_file(g_scheduler, argv[1]);
    // async_read_file(scheduler, argv[2]);
    g_scheduler.run();
    return 0;
}


