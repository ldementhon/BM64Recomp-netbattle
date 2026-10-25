// Included inside checkpoint's namespace. Verification and timing use the same path.
static bool catchup(unsigned at) {
    const unsigned length = setting("BM64_CATCHUP_FRAMES", 0);
    if (!length) return false;
    const unsigned samples = setting("BM64_CATCHUP_SAMPLES", 100);
    const bool verify = std::getenv("BM64_CATCHUP_VERIFY") != nullptr;
    const bool render_all = std::getenv("BM64_CATCHUP_RENDER_ALL") != nullptr;
    require(at > 0 && (length == 4 || length == 8 || length == 12) && samples > 0, "invalid catch-up window/sample count");
    require(!std::getenv("BM64_BENCHMARK_FRAMES") && !std::getenv("BM64_SUPPRESS_REPLAY"), "conflicting catch-up modes");
    if (frame < at) return true;
    static uint64_t end;
    static unsigned completed;
    static std::vector<Digest> reference;
    static std::vector<double> times, waits, restores, work_times;
    static RenderCounts baseline_start{}, baseline_previous{}, final_update{}, replay_start{};
    static RenderCounts totals{};
    static auto started = std::chrono::steady_clock::now();
    static uint64_t wait_start;
    static Profile profile_start;
    static double capture_cost;
    if (!end) {
        save(0, true);
        save(1 + frame % 16, true);
        end = frame + length;
        reference.clear();
        baseline_start = baseline_previous = render_counts();
        return true;
    }
    // Refresh corrected history as real rollback would, including the final update.
    auto capture_ms=save(1 + frame % 16, true);
    if(replay_pass) capture_cost+=capture_ms;
    if (verify) {
        auto actual = digest();
        size_t index = frame + length - end - 1;
        if (!replay_pass) {
            reference.push_back(actual);
            fprintf(stderr,"CATCHUP: baseline frame=%llu ram_hash=%016llx\n",frame,actual.ram);
        } else if (actual.ram != reference[index].ram || actual.registers != reference[index].registers) {
            fprintf(stderr,"CHECKPOINT FAIL: catch-up frame=%llu expected=%016llx actual=%016llx registers=%016llx/%016llx\n",frame,reference[index].ram,actual.ram,reference[index].registers,actual.registers);
            if (std::getenv("BM64_NEGATIVE_INPUT") || std::getenv("BM64_NEGATIVE_STACKS")) {
                fprintf(stderr,"PASS: negative control detected divergence\n"); fflush(stderr); std::_Exit(0);
            }
            std::_Exit(2);
        }
    }
    if (!replay_pass) {
        auto counts = render_counts();
        final_update = {counts.drawn-baseline_previous.drawn, 0, counts.presented-baseline_previous.presented, 0};
        baseline_previous = counts;
        if (frame == end) {
            replay_pass = 1;
            replay_start = counts;
            wait_start = presentation_wait_ns;
            profile_start=profile(); capture_cost=0;
            catchup_active = true;
            started = std::chrono::steady_clock::now();
            restores.push_back(restore(0, true));
            suppress_render = !render_all;
        }
        return true;
    }
    suppress_render = !render_all && frame < end - 1;
    if (frame != end) return true;
    finish_presentation();
    auto elapsed = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    times.push_back(elapsed);
    waits.push_back(double(presentation_wait_ns.load()-wait_start)/1e6);
    work_times.push_back(elapsed - waits.back());
    if(profiling) {
        auto p=profile(); profile_report(profile_start,p,length);
        fprintf(stderr,"PROFILE: capture_ms=%.3f restore_ms=%.3f simulation_runtime_residual_ms=%.3f\n",capture_cost,restores.back(),elapsed-waits.back()-(p.graphics_wait-profile_start.graphics_wait)/1e6-capture_cost-restores.back());
    }
    catchup_active = false;
    auto counts = render_counts();
    RenderCounts delta{counts.drawn-replay_start.drawn, counts.skipped-replay_start.skipped,
        counts.presented-replay_start.presented, counts.hidden-replay_start.hidden};
    if (render_all) final_update = {baseline_previous.drawn-baseline_start.drawn, 0, baseline_previous.presented-baseline_start.presented, 0};
    require(delta.drawn == final_update.drawn && delta.presented == final_update.presented &&
        delta.skipped == baseline_previous.drawn-baseline_start.drawn-final_update.drawn &&
        delta.hidden == baseline_previous.presented-baseline_start.presented-final_update.presented,
        "catch-up did not render only the final update");
    totals.drawn += delta.drawn; totals.skipped += delta.skipped;
    totals.presented += delta.presented; totals.hidden += delta.hidden;
    fprintf(stderr,"CATCHUP: sample=%u updates=%u end=%llu total_ms=%.3f final_tasks=%llu native_only_tasks=%llu screen_updates=%llu hidden_updates=%llu\n",
        completed+1,length,end,elapsed,delta.drawn,delta.skipped,delta.presented,delta.hidden);
    if (++completed == samples) {
        require(!std::getenv("BM64_NEGATIVE_INPUT") && !std::getenv("BM64_NEGATIVE_STACKS"), "negative control failed to diverge");
        distribution("catchup_total",times); distribution("catchup_restore",restores); distribution("catchup_present_wait",waits); distribution("catchup_work_excluding_final_wait",work_times);
        auto over = std::count_if(times.begin(),times.end(),[](double t) { return t > 1000.0/60; });
        fprintf(stderr,"CATCHUP: verification=%s budget_ms=16.667 over_budget=%zu/%u drawn=%llu native_only=%llu presented=%llu hidden=%llu\n",verify ? "full_RAM_and_registers" : "disabled",size_t(over),samples,totals.drawn,totals.skipped,totals.presented,totals.hidden);
        fprintf(stderr,"PASS: catch-up completed %u windows of %u updates with %s presentation\n",samples,length,render_all ? "all-update" : "final-update-only"); fflush(stderr); std::_Exit(0);
    }
    replay_pass = 0;
    end = 0;
    return catchup(at); // Current corrected state starts the next ordinary window.
}
