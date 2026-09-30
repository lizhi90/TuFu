// rt_util.h —— 实时线程辅助（绑核 / SCHED_FIFO / 内存锁定 / 绝对周期唤醒）
// 用法：RT 线程入口先调 rt_setup(cpu, prio) 与 DmaLatencyGuard，再进 1ms 循环。
#pragma once

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sched.h>
#include <pthread.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>

namespace kx {

inline long long ts_to_ns(const timespec& ts) {
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

inline timespec ns_to_ts(long long ns) {
    timespec ts;
    ts.tv_sec  = (time_t)(ns / 1000000000LL);
    ts.tv_nsec = (long)(ns % 1000000000LL);
    if (ts.tv_nsec < 0) { ts.tv_nsec += 1000000000L; ts.tv_sec -= 1; }
    return ts;
}

// 读取 CLOCK_MONOTONIC（与 IgH 周期同步用的时钟一致）
inline long long mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts_to_ns(ts);
}

// 绑定 CPU + SCHED_FIFO。cpu<0 表示不绑核；prio<=0 表示不改调度策略。
inline bool rt_setup(int cpu, int prio) {
    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
            std::fprintf(stderr, "[rt] 绑核 CPU%d 失败: %s\n", cpu, std::strerror(errno));
            return false;
        }
    }
    if (prio > 0) {
        sched_param sp{};
        sp.sched_priority = prio;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
            std::fprintf(stderr, "[rt] 设 SCHED_FIFO/%d 失败(需 root 或 LimitRTPRIO): %s\n",
                         prio, std::strerror(errno));
            return false;
        }
    }
    // 锁定内存，避免运行时缺页抖动
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::fprintf(stderr, "[rt] mlockall 失败: %s\n", std::strerror(errno));
    }
    return true;
}

// 持有 /dev/cpu_dma_latency 的 fd，阻止 CPU 进入深度 C-state
class DmaLatencyGuard {
public:
    DmaLatencyGuard() {
        fd_ = ::open("/dev/cpu_dma_latency", O_WRONLY);
        if (fd_ >= 0) {
            int32_t latency = 0;
            if (::write(fd_, &latency, sizeof(latency)) < 0) {
                ::close(fd_);
                fd_ = -1;
            }
        }
    }
    ~DmaLatencyGuard() {
        if (fd_ >= 0) {
            int32_t l = 1000000;
            if (::write(fd_, &l, sizeof(l)) < 0) { /* 退出阶段失败可忽略 */ }
            ::close(fd_);
        }
    }
    DmaLatencyGuard(const DmaLatencyGuard&) = delete;
    DmaLatencyGuard& operator=(const DmaLatencyGuard&) = delete;
private:
    int fd_ = -1;
};

// 绝对时间周期休眠：wake 由调用方按周期递增，避免累积漂移
inline void sleep_until(long long wake_ns) {
    timespec ts = ns_to_ts(wake_ns);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
}

} // namespace kx
