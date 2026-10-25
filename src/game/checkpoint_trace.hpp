#pragma once
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <sys/resource.h>

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
    rusage usage{};
    std::array<Stage,8> stages{};
    size_t count=0;
    static double cpu_ms() {
        timespec t{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);
        return t.tv_sec*1000.0+t.tv_nsec/1e6;
    }
    CheckpointTrace(bool enabled,const char* operation,uint64_t frame,size_t slot):enabled(enabled),operation(operation),frame(frame),slot(slot) {
        if(enabled) { getrusage(RUSAGE_SELF,&usage); start=previous=Clock::now(); cpu_start=cpu_previous=cpu_ms(); }
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
        rusage after{}; getrusage(RUSAGE_SELF,&after);
        fprintf(stderr,"CHECKPOINT_SLOW: operation=%s frame=%llu slot=%zu wall_ms=%.3f thread_cpu_ms=%.3f vm_clear_ms=%.3f vm_map_ms=%.3f native_bytes=%zu minor_faults=%ld major_faults=%ld voluntary_switches=%ld involuntary_switches=%ld",operation,frame,slot,total,cpu_previous-cpu_start,clear_ms,remap_ms,bytes,after.ru_minflt-usage.ru_minflt,after.ru_majflt-usage.ru_majflt,after.ru_nvcsw-usage.ru_nvcsw,after.ru_nivcsw-usage.ru_nivcsw);
        for(size_t i=0;i<count;++i) fprintf(stderr," %s_wall_ms=%.3f %s_cpu_ms=%.3f",stages[i].name,stages[i].wall,stages[i].name,stages[i].cpu);
        fputc('\n',stderr);
    }
};
