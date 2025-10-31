#include <cppcoro/task.hpp>
#include <cppcoro/task.hpp> 
#include <cppcoro/io_service.hpp>
#include <cppcoro/read_only_file.hpp>
#include <cppcoro/when_all_ready.hpp>
#include <cppcoro/sync_wait.hpp>


#include <filesystem>
#include <memory>
#include <algorithm>
#include <iostream>

namespace fs = std::filesystem;

cppcoro::task<std::uint64_t> count_lines(cppcoro::io_service& ioService, fs::path path)
{
    auto file = cppcoro::read_only_file::open(ioService, path);
    constexpr size_t bufferSize = 4096;
    auto buffer = std::make_unique<std::uint8_t[]>(bufferSize);
    std::uint64_t newlineCount = 0;
    for (std::uint64_t offset = 0, fileSize = file.size(); offset < fileSize;)
    {
        const auto bytesToRead = static_cast<size_t>(
                std::min<std::uint64_t>(bufferSize, fileSize - offset));
        const auto bytesRead = co_await file.read(offset, buffer.get(), bytesToRead);
        newlineCount += std::count(buffer.get(), buffer.get() + bytesRead, '\n');
        offset += bytesRead;
    }

    co_return newlineCount;
}

cppcoro::task<> run(cppcoro::io_service& ioService)
{
    cppcoro::io_work_scope ioScope(ioService);
    auto lineCount = co_await count_lines(ioService, fs::path{"countline.log"});
    std::cout << "foo.txt has " << lineCount << " lines." << std::endl;;
}

cppcoro::task<> process_events(cppcoro::io_service& ioService)
{
    // 处理事件至 io_service 被停止时
    // 比如：当最后一个 io_work_scope 退出作用域时
    ioService.process_events();
    co_return;
}

int main(int argc, char* argv[])
{
    cppcoro::io_service ioService;
    cppcoro::sync_wait(cppcoro::when_all_ready(
                run(ioService),
                process_events(ioService)));

    return 0;
}
