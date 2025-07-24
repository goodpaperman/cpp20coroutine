#include <coroutine>
#include <unordered_map>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <sstream>
#include <signal.h>
#include <sys/signalfd.h>

#define MAX_EVENTS 10

struct Task {
    struct promise_type {
        Task get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

class EpollScheduler {
private:
    int epoll_fd;
    int signal_fd; 
    std::unordered_map<int, std::coroutine_handle<>> io_handles;
public:
    EpollScheduler(int signum) {
        epoll_fd = epoll_create(MAX_EVENTS);
        if (epoll_fd == -1) {
            std::stringstream ss;
            ss << "epoll_create failed, error " << errno; 
            throw std::runtime_error(ss.str());
        }

        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, signum);
        sigprocmask(SIG_BLOCK, &mask, NULL);
        signal_fd = signalfd(-1, &mask, SFD_NONBLOCK);
        if (signal_fd == -1) { 
            std::stringstream ss;
            ss << "signalfd failed, error " << errno; 
            throw std::runtime_error(ss.str());
        }

        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = signal_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, signal_fd, &ev) == -1) {
            std::stringstream ss;
            ss << "epoll_ctl failed, error " << errno; 
            throw std::runtime_error(ss.str());
        }

        std::cout << "register signal " << signum << " as fd " << signal_fd << std::endl; 
    }

    ~EpollScheduler() {
        for(auto handle : io_handles) {
            std::cout << "coroutine destroy" << std::endl; 
            handle.second.destroy(); 
        }
        close(signal_fd); 
        close(epoll_fd);
    }

    void register_io(int fd, std::coroutine_handle<> handle) {
        if (io_handles.find(fd) == io_handles.end()) {
            io_handles[fd] = handle;

            epoll_event event{};
            event.events = EPOLLIN | EPOLLET; 
            event.data.fd = fd;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == -1) {
                std::stringstream ss;
                ss << "epoll_ctl failed, error " << errno; 
                throw std::runtime_error(ss.str());
            }
        }
    }

    void run() {
        while (true) {
            epoll_event events[MAX_EVENTS] = { 0 };
            int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
            for (int i = 0; i < n; ++i) {
                int ready_fd = events[i].data.fd;
                if (ready_fd == signal_fd) {
                    struct signalfd_siginfo fdsi = { 0 };
                    read(signal_fd, &fdsi, sizeof(fdsi));
                    std::cout << "signal " << fdsi.ssi_signo << " detected, exit..." << std::endl; 
                    return; 
                }

                if (auto it = io_handles.find(ready_fd); it != io_handles.end()) {
                    it->second.resume(); 
                }
            }
        }
    }
};

struct AsyncReadAwaiter {
    EpollScheduler& sched;
    int fd;
    int len; 
    std::string buffer; 

    AsyncReadAwaiter(EpollScheduler& s, int file_fd, size_t buf_size) 
        : sched(s), fd(file_fd), len(0), buffer(buf_size, '\0') { }

#ifdef PREREAD_IN_AWAIT_READY
    bool await_ready()  { 
        len = 0; 
        ssize_t n = read(fd, buffer.data(), buffer.size());
        if (n > 0) { 
            len = n; 
            return true; 
        } else if (n == -1 && errno != EAGAIN) {
            std::stringstream ss;
            ss << "pre read failed, error " << errno; 
            throw std::runtime_error(ss.str());
        }

        return false;
    }

    void await_suspend(std::coroutine_handle<> h) {
        sched.register_io(fd, h); 
    }
#else 
    bool await_ready() const { 
        return false;
    }

    bool await_suspend(std::coroutine_handle<> h) {
        len = 0; 
        ssize_t n = read(fd, buffer.data(), buffer.size());
        if (n > 0) { 
            len = n; 
            return false; 
        } else if (n == 0 || (n == -1 && errno == EAGAIN)) {
            sched.register_io(fd, h); 
            return true; 
        } else {
            std::stringstream ss;
            ss << "pre read failed, error " << errno; 
            throw std::runtime_error(ss.str());
        }
    }
#endif

    std::string await_resume() {
        ssize_t n = read(fd, buffer.data() + len, buffer.size() - len);
        if (n == -1) {
            if (len > 0) { 
                buffer.resize(len); 
                return std::move(buffer);
            }

                std::stringstream ss;
                ss << "read failed, error " << errno; 
                throw std::runtime_error(ss.str());
        }

        buffer.resize(n + len);
        if (len > 0) {
            std::cout << "pre-read " << len << ", read " << n << std::endl; 
        }
        return std::move(buffer);
    }
};

Task async_read_file(EpollScheduler& sched, const char* path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
        std::stringstream ss;
        ss << "open failed, error " << errno; 
        throw std::runtime_error(ss.str());
    }

    while (true) {
        auto data = co_await AsyncReadAwaiter(sched, fd, 4096);
        std::cout << "Read [" << data.size() << "] " << data;
        if (data.size() == 0)
            std::cout << std::endl; 

        // if (data.size() == 0)
        //     break; 
    }
    close(fd);
}

int main(int argc, char* argv[]) {
    if (argc < 3) { 
        std::cout << "Usage: sample pipe1 pipe2" << std::endl; 
        return 1; 
    }

    EpollScheduler scheduler(SIGINT);
    async_read_file(scheduler, argv[1]);
    async_read_file(scheduler, argv[2]);
    scheduler.run();
    return 0;
}
