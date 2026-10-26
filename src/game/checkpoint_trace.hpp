#pragma once
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#ifdef _WIN32
#include <windows.h>
#include <system_error>
#else
#include <sys/resource.h>
#endif

// Wall time locates the slow stage; thread CPU and task-wide scheduler/fault
// deltas distinguish computation from blocking without claiming a kernel cause.
struct CheckpointTrace {
    using Clock=std::chrono::steady_clock;
    struct Stage { const char* name; double wall, cpu; };
    bool enabled;
    const char* operation;
    uint64_t frame;
    size_t slot;
    Clock::time_point start, previous;
    double cpu_start, cpu_previous;
#ifndef _WIN32
    rusage usage{};
#endif
    std::array<Stage,8> stages{};
    size_t count=0;
    static double cpu_ms() {
#ifdef _WIN32
        FILETIME created, exited, kernel, user;
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
            throw std::system_error(GetLastError(), std::system_category(), "checkpoint CPU timing");
        auto ticks = [](FILETIME value) { return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
        return (ticks(kernel) + ticks(user)) / 1e4;
#else
        timespec t{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);
        return t.tv_sec*1000.0+t.tv_nsec/1e6;
#endif
    }
    CheckpointTrace(bool enabled,const char* operation,uint64_t frame,size_t slot):enabled(enabled),operation(operation),frame(frame),slot(slot) {
        if(enabled) {
#ifndef _WIN32
            getrusage(RUSAGE_SELF,&usage);
#endif
            start=previous=Clock::now(); cpu_start=cpu_previous=cpu_ms();
        }
    }
    void mark(const char* name) {
        if(!enabled) return;
        auto now=Clock::now(); auto cpu=cpu_ms();
        stages[count++]={name,std::chrono::duration<double,std::milli>(now-previous).count(),cpu-cpu_previous};
        previous=now; cpu_previous=cpu;
    }
    void report(double clear_ms,double remap_ms,size_t bytes) const {
        if(!enabled) return;
        double total=std::chrono::duration<double,std::milli>(previous-start).count();
        if(total<2.0) return;
        fprintf(stderr,"CHECKPOINT_SLOW: operation=%s frame=%llu slot=%zu wall_ms=%.3f thread_cpu_ms=%.3f vm_clear_ms=%.3f vm_map_ms=%.3f native_bytes=%zu",operation,frame,slot,total,cpu_previous-cpu_start,clear_ms,remap_ms,bytes);
#ifndef _WIN32
        rusage after{}; getrusage(RUSAGE_SELF,&after);
        fprintf(stderr," minor_faults=%ld major_faults=%ld voluntary_switches=%ld involuntary_switches=%ld",after.ru_minflt-usage.ru_minflt,after.ru_majflt-usage.ru_majflt,after.ru_nvcsw-usage.ru_nvcsw,after.ru_nivcsw-usage.ru_nivcsw);
#endif
        for(size_t i=0;i<count;++i) fprintf(stderr," %s_wall_ms=%.3f %s_cpu_ms=%.3f",stages[i].name,stages[i].wall,stages[i].name,stages[i].cpu);
        fputc('\n',stderr);
    }
};
