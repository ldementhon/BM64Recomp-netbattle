namespace Probe = RT64::RollbackProbe;
struct Profile {
    uint64_t graphics_wait, graphics, native, native_wait, copy, bytes, stored, screen, acquire, present_gpu_wait, pacing, present_wait, present_call, dependency;
    uint64_t upload, texture, submit, fence, gpu, schedule, completion, wakeup, samples, invalid, noise_pixels, other_pixels;
    std::array<uint64_t,Probe::PrepCount> prep_wall{},prep_cpu{},prep_calls{};
};
static Profile profile() {
    Profile p{graphics_wait_ns,Probe::graphics,Probe::native,Probe::native_wait,Probe::copy,Probe::copy_bytes,Probe::stored_bytes,Probe::screen,Probe::acquire,Probe::present_gpu_wait,Probe::pacing,Probe::present_wait,Probe::present_call,Probe::workload_dependency, Probe::native_upload_wait, Probe::native_texture_wait, Probe::native_submit, Probe::native_fence_wait, Probe::native_gpu, Probe::native_schedule, Probe::native_complete_delay, Probe::native_wakeup, Probe::native_samples, Probe::native_invalid, Probe::native_noise_pixels, Probe::native_other_pixels};
    for(unsigned i=0;i<Probe::PrepCount;++i) { p.prep_wall[i]=Probe::prep_wall[i]; p.prep_cpu[i]=Probe::prep_cpu[i]; p.prep_calls[i]=Probe::prep_calls[i]; }
    return p;
}
static void profile_report(const Profile& a, const Profile& b, unsigned updates) {
    fprintf(stderr,"PROFILE: updates=%u graphics_fence_ms=%.3f graphics_worker_ms=%.3f native_stage_ms=%.3f native_gpu_upload_wait_ms=%.3f RAM_copy_ms=%.3f RAM_copy_bytes=%llu RAM_stored_bytes=%llu screen_ms=%.3f acquire_ms=%.3f present_gpu_wait_ms=%.3f pacing_ms=%.3f swapchain_wait_ms=%.3f present_call_ms=%.3f present_dependency_ms=%.3f\n",updates,
        (b.graphics_wait-a.graphics_wait)/1e6,(b.graphics-a.graphics)/1e6,(b.native-a.native)/1e6,(b.native_wait-a.native_wait)/1e6,(b.copy-a.copy)/1e6,b.bytes-a.bytes,b.stored-a.stored,(b.screen-a.screen)/1e6,(b.acquire-a.acquire)/1e6,(b.present_gpu_wait-a.present_gpu_wait)/1e6,(b.pacing-a.pacing)/1e6,(b.present_wait-a.present_wait)/1e6,(b.present_call-a.present_call)/1e6,(b.dependency-a.dependency)/1e6);
    fprintf(stderr,"NATIVE_PROFILE: updates=%u samples=%llu invalid=%llu noise_pixels=%llu other_pixels=%llu CPU_upload_wait_ms=%.3f texture_wait_ms=%.3f submit_ms=%.3f fence_wait_ms=%.3f GPU_execute_ms=%.3f submit_to_GPU_ms=%.3f GPU_to_callback_ms=%.3f callback_to_wake_ms=%.3f\n",updates,b.samples-a.samples,b.invalid-a.invalid,b.noise_pixels-a.noise_pixels,b.other_pixels-a.other_pixels,(b.upload-a.upload)/1e6,(b.texture-a.texture)/1e6,(b.submit-a.submit)/1e6,(b.fence-a.fence)/1e6,(b.gpu-a.gpu)/1e6,(b.schedule-a.schedule)/1e6,(b.completion-a.completion)/1e6,(b.wakeup-a.wakeup)/1e6);
    if(Probe::prep_profile) {
        fprintf(stderr,"PREP_PROFILE: updates=%u",updates);
        for(unsigned i=0;i<Probe::PrepCount;++i) fprintf(stderr," %s_wall_ms=%.3f %s_CPU_ms=%.3f %s_calls=%llu",Probe::prep_names[i],(b.prep_wall[i]-a.prep_wall[i])/1e6,Probe::prep_names[i],(b.prep_cpu[i]-a.prep_cpu[i])/1e6,Probe::prep_names[i],b.prep_calls[i]-a.prep_calls[i]);
        fprintf(stderr,"\n");
    }

}
struct Presentation { uint64_t frame, deadline, time, callback; };
static std::mutex presentation_mutex;
static std::vector<Presentation> presentations;
static std::vector<Probe::PresentTrace> present_traces;
static void present_trace(const Probe::PresentTrace& trace) {
    std::lock_guard lock(presentation_mutex);
    present_traces.push_back(trace);
}
void displayed(uint64_t id, uint64_t deadline, uint64_t time) {
    const auto callback=Probe::now();
    std::lock_guard lock(presentation_mutex);
    presentations.push_back({id,deadline,time,callback});
    if(id) Probe::observeDisplay(deadline,time);
}

static void loop_wait_until(uint64_t deadline) {
    if(!Probe::aligned_pacing) {
        auto now=Probe::now();
        if(now<deadline) std::this_thread::sleep_for(std::chrono::nanoseconds(deadline-now));
        return;
    }
#ifdef __APPLE__
    static const auto scale=[] { mach_timebase_info_data_t t; mach_timebase_info(&t); return double(t.denom)/t.numer; }();
    constexpr uint64_t spin_ns=1500000;
    if(Probe::now()+spin_ns<deadline) mach_wait_until(uint64_t((deadline-spin_ns)*scale));
#endif
    while(Probe::now()<deadline) std::atomic_signal_fence(std::memory_order_seq_cst);
}
static uint64_t display_aligned_epoch(uint64_t& period) {
    std::vector<uint64_t> gaps;
    uint64_t last=0;
    { std::lock_guard lock(presentation_mutex);
      for(auto p:presentations) if(p.time>last) {
          if(last && p.time-last>12000000 && p.time-last<20000000) gaps.push_back(p.time-last);
          last=p.time;
      }
    }
    require(gaps.size()>=10,"not enough display samples to align pacing");
    if(gaps.size()>60) gaps.erase(gaps.begin(),gaps.end()-60);
    std::sort(gaps.begin(),gaps.end()); period=gaps[gaps.size()/2];
    auto now=Probe::now();
    // Diagnostic phase sweep; latency is always measured from this actual release.
    const char* value=std::getenv("BM64_RELEASE_LEAD_US");
    auto lead=value ? std::stoll(value)*1000 : 0;
    require(lead<int64_t(period) && lead>-int64_t(period),"release phase must be within one refresh");
    return int64_t(last+((now-last)/period+2)*period)-lead;
}
