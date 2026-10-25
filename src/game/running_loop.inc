// A deterministic delayed-input fixture, paced at 60 Hz without per-frame GPU drains.
static uint64_t loop_frontier;
static bool loop_verifying;
struct LoopCycle { unsigned index; uint64_t release,wake,execute,finish; double capture,restore; bool rollback, hidden; };
static bool running_loop(unsigned at) {
    const unsigned count=setting("BM64_LOOP_FRAMES",0);
    if (!count) return false;
    require(count>loop_warmup && at>16,"running loop needs warmup and a checkpoint frame");
    static uint64_t period=1000000000ull/60;
    static uint64_t epoch, cycle_start, cycle_release, execution_start;
    static double checkpoint_ms, restore_ms;
    static uint64_t gfx_before;
    static unsigned rollbacks, prediction_divergences, source_retired;
    static bool normal_hidden=false;
    static Profile before, cycle_profile;
    static std::vector<double> all, rollback_times, normal_times, checkpoint_times, simulation_times;
    static std::vector<LoopCycle> cycle_log;
    static std::vector<double> cpu_lateness;
    static std::map<uint64_t,Digest> oracle;
    static std::ofstream oracle_output;
    static bool started=false;
    if (!loop_at) {
        if(std::getenv("BM64_PRESENT_TRACE")) Probe::present_trace=present_trace;
        loop_at=at;
        loop_end=loop_at+count;
        loop_buffer=setting("BM64_INPUT_BUFFER",2);
        loop_length=setting("BM64_ROLLBACK_LENGTH",4);
        loop_spacing=setting("BM64_ROLLBACK_SPACING",60);
        require(loop_spacing>=loop_length+4,"rollback spacing too small");
#ifdef __APPLE__
        if(Probe::aligned_pacing) pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
#endif
        loop_oracle=std::getenv("BM64_LOOP_ORACLE")!=nullptr;
        loop_verifying=std::getenv("BM64_LOOP_VERIFY")!=nullptr;
        require(loop_buffer<=4 && (loop_length==0 || loop_length==4 || loop_length==8 || loop_length==12),"invalid rollback/input buffer length");
        require(!std::getenv("BM64_CATCHUP_FRAMES") && !std::getenv("BM64_SUPPRESS_REPLAY") && !std::getenv("BM64_BENCHMARK_FRAMES"),"conflicting loop modes");
        if (loop_verifying) loop_oracle=true; // Raw contexts contain native f_odd pointers; compare within this process.
        if (std::getenv("BM64_LOOP_ORACLE")) {
            const char* path=std::getenv("BM64_ORACLE_PATH"); require(path,"oracle path missing");
            oracle_output.open(path); require(oracle_output.good(),"oracle output");
        }
    }
    if (frame<at) return true;
    if (!started) {
        if(loop_oracle && loop_verifying) save(0,true);
        // Navigation ran unpaced. Clear that setup backlog once, before warmup.
        finish_presentation();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        epoch=Probe::now();
        if(!loop_oracle && !loop_verifying) loop_wait_until(epoch);
        cycle_release=epoch; cycle_start=execution_start=Probe::now(); loop_frontier=frame; before=profile();
        fprintf(stderr,"LOOP_CLOCK: epoch_ns=%llu period_ns=%llu aligned=%d bounded=%d spacing=%u\n",epoch,period,Probe::aligned_pacing,Probe::bounded_present,loop_spacing);
        checkpoint_ms=save(1+frame%16,true); restore_ms=0; gfx_before=graphics_wait_ns; cycle_profile=profile();
        presentation_frame=loop_oracle ? 0 : 1; presentation_deadline=epoch+period; started=true;
        return true;
    }
    checkpoint_ms+=save(1+frame%16,true);
    if (loop_oracle || loop_verifying) {
        auto actual=digest();
        if (loop_oracle) {
            oracle[frame]=actual;
            if(oracle_output.is_open()) oracle_output<<std::dec<<frame<<' '<<std::hex<<actual.ram<<' '<<actual.registers<<'\n';
        }
        else if (!replay_pass && pending_begin(frame-1)) {
            if(!pending_begin(frame)) {
                require(oracle.count(frame),"missing prediction-end oracle");
                auto expected=oracle.at(frame);
                prediction_divergences += actual.ram!=expected.ram || actual.registers!=expected.registers;
            }
        }
        else {
            require(oracle.count(frame),"missing oracle frame");
            auto expected=oracle.at(frame);
            if (actual.ram!=expected.ram || actual.registers!=expected.registers) {
                fprintf(stderr,"CHECKPOINT FAIL: loop frame=%llu replay=%u RAM=%016llx/%016llx registers=%016llx/%016llx\n",frame,replay_pass,expected.ram,actual.ram,expected.registers,actual.registers);
                if (std::getenv("BM64_NEGATIVE_INPUT")) { fprintf(stderr,"PASS: negative control detected divergence\n"); fflush(stderr); std::_Exit(0); }
                std::_Exit(2);
            }
        }
    }
    if (replay_pass) {
        if (frame<loop_frontier) return true;
        replay_pass=0;
        normal_hidden=Probe::bounded_present && Probe::now()>presentation_deadline;
        suppress_render=normal_hidden;
        // The new normal update is the sole presented update in this display cycle.
        return true;
    }
    uint64_t finish=Probe::now();
    unsigned index=frame-at-1;
    cycle_log.push_back({index,cycle_release,cycle_start,execution_start,finish,checkpoint_ms,restore_ms,bool(catchup_active),normal_hidden});
    source_retired+=normal_hidden;
    if (index>=loop_warmup) {
        double total=(finish-cycle_start)/1e6;
        all.push_back(total); checkpoint_times.push_back(checkpoint_ms);
        simulation_times.push_back(total-(graphics_wait_ns.load()-gfx_before)/1e6-checkpoint_ms-restore_ms);
        bool corrected=catchup_active;
        if(corrected && profiling) {
            fprintf(stderr,"LOOP_ROLLBACK: index=%u updates=%u total_ms=%.3f capture_ms=%.3f restore_ms=%.3f\n",index,loop_length+1,total,checkpoint_ms,restore_ms);
            profile_report(cycle_profile,profile(),loop_length+1);
        }
        (corrected ? rollback_times : normal_times).push_back(total);
        double late=std::max(0.0,(double(finish)-double(epoch+(index+1)*period))/1e6);
        cpu_lateness.push_back(late);
    }
    catchup_active=false;
    // Calibrate once from paced warmup displays, then freeze phase and period.
    if(index+1==loop_warmup/2 && Probe::aligned_pacing && !loop_oracle && !loop_verifying) {
        epoch=display_aligned_epoch(period)-(index+1)*period;
        Probe::refresh_period_ns=period;
        fprintf(stderr,"LOOP_CLOCK: epoch_ns=%llu period_ns=%llu aligned=1 bounded=%d spacing=%u\n",epoch,period,Probe::bounded_present,loop_spacing);
    }
    if(index+1==loop_warmup) before=profile();
    if (frame==at+count) {
        presentation_frame=0;
        finish_presentation(); // Cleanup only; outside every timed display cycle.
        if (loop_oracle) {
            if(oracle_output.is_open()) oracle_output.close();
            if(loop_verifying) {
                restore(0,true); loop_oracle=false; started=false;
                all.clear(); rollback_times.clear(); normal_times.clear(); checkpoint_times.clear(); simulation_times.clear(); cycle_log.clear(); cpu_lateness.clear();
                fprintf(stderr,"LOOP: reference recorded; starting delayed-input correction verification\n");
                return running_loop(at);
            }
            fprintf(stderr,"PASS: running-loop oracle recorded %u full-state hashes\n",count); fflush(stderr); std::_Exit(0);
        }
        require(!std::getenv("BM64_NEGATIVE_INPUT"),"negative control failed to diverge");
        if(loop_verifying && loop_length) require(prediction_divergences>0,"test did not correct any wrong predictions");
        if(loop_verifying) {
            fprintf(stderr,"PASS: running-loop full RAM/register verification; updates=%u rollbacks=%u wrong_predictions_corrected=%u source_retired=%u queue_retired=%llu\n",count,rollbacks,prediction_divergences,source_retired,Probe::retired_presents.load()); fflush(stderr); std::_Exit(0);
        }
        // Metal's displayed callback can arrive after present() returns.
        for(unsigned wait=0;wait<100;++wait) {
            bool seen=false;
            { std::lock_guard lock(presentation_mutex); for(auto p:presentations) if(p.frame==count) seen=true; }
            if(seen) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        { std::lock_guard lock(presentation_mutex);
          for(auto p:presentations) if(p.frame) fprintf(stderr,"DISPLAY: frame=%llu time_ns=%llu deadline_ns=%llu callback_ns=%llu\n",p.frame,p.time,p.deadline,p.callback);
          for(auto p:present_traces) if(p.frame) fprintf(stderr,"PRESENT_TRACE: frame=%llu queue_ns=%llu workload_ready_ns=%llu slot_ready_ns=%llu acquired_ns=%llu draw_submitted_ns=%llu draw_done_ns=%llu present_called_ns=%llu\n",p.frame,p.queue,p.workloadReady,p.slotReady,p.acquired,p.drawSubmitted,p.drawDone,p.presentCalled);
        }
        distribution("loop_deadline_lateness",cpu_lateness);
        distribution("loop_cycle",all); distribution("loop_normal",normal_times);
        if(!rollback_times.empty()) distribution("loop_rollback_plus_normal",rollback_times);
        distribution("loop_checkpoints",checkpoint_times); distribution("loop_simulation_runtime_residual",simulation_times);
        profile_report(before,profile(),count-loop_warmup+rollbacks*loop_length);
        std::map<uint64_t,uint64_t> first;
        { std::lock_guard lock(presentation_mutex); for(auto p:presentations) if(p.time && (!first.count(p.frame) || p.time<first[p.frame])) first[p.frame]=p.time; }
        std::vector<double> warm_latency, latency, gaps;
        for(auto [id,time]:first) {
            if(!id) continue;
            require(time<Probe::now()+period,"display clock epoch mismatch");
            double lag=(time-(epoch+(id-1)*period))/1e6;
            if(id>loop_warmup/2 && id<=loop_warmup) warm_latency.push_back(lag);
        }
        require(warm_latency.size()>=30,"insufficient Metal displayed callbacks");
        std::sort(warm_latency.begin(),warm_latency.end()); double pipeline_lag=warm_latency[warm_latency.size()/2];
        unsigned missing=0, late=0, strict_late=0, cpu_late=0, cpu_deadline_misses=0, spikes=0;
        uint64_t previous=0;
        for(unsigned i=loop_warmup;i<count;++i) {
            auto it=first.find(i+1);
            if(it==first.end()) { ++missing; continue; }
            double lag=(int64_t(it->second)-int64_t(epoch+i*period))/1e6;
            latency.push_back(lag); late+=lag>pipeline_lag+1.0; strict_late+=lag>1000.0/60;
            if(previous) { double gap=(int64_t(it->second)-int64_t(previous))/1e6; gaps.push_back(gap); spikes+=gap>25.0; }
            previous=it->second;
        }
        for(auto c:cycle_log) {
            double ms=(c.finish-c.wake)/1e6, late=std::max(0.0,(double(c.finish)-double(c.release+period))/1e6);
            if(c.index>=loop_warmup) { cpu_late+=ms>period/1e6; cpu_deadline_misses+=late>0; }
            fprintf(stderr,"LOOP_FRAME: index=%u release_ns=%llu wake_ns=%llu execute_ns=%llu finish_ns=%llu work_ms=%.3f execution_ms=%.3f wake_late_ms=%.3f deadline_lateness_ms=%.3f displayed_ns=%llu deadline_ns=%llu capture_ms=%.3f restore_ms=%.3f rollback=%d source_retired=%d\n",c.index,c.release,c.wake,c.execute,c.finish,ms,(c.finish-c.execute)/1e6,(int64_t(c.wake)-int64_t(c.release))/1e6,late,first.count(c.index+1)?first[c.index+1]:0,c.release+period,c.capture,c.restore,c.rollback,c.hidden);
        }
        fprintf(stderr,"PRESENT_BOUND: maximum_in_flight=%llu retired=%llu slot_wait_ms=%.3f\n",Probe::maximum_in_flight.load(),Probe::retired_presents.load(),Probe::slot_wait_ns.load()/1e6);
        fprintf(stderr,"NATIVE_UPLOADS: synchronous_calls=%llu\n",Probe::inline_upload_calls.load());
        distribution("display_latency",latency); distribution("display_gap",gaps);
        fprintf(stderr,"LOOP: frames=%u measured=%zu rollback_length=%u rollbacks=%u input_buffer=%u wrong_predictions_corrected=%u CPU_over_budget=%u CPU_deadline_misses=%u displayed_missing=%u strict_display_deadline_misses=%u calibrated_deadline_misses=%u warmup_pipeline_lag_ms=%.3f tolerance_ms=1 display_gaps_over_25ms=%u verification=%s\n",count,all.size(),loop_length,rollbacks,loop_buffer,prediction_divergences,cpu_late,cpu_deadline_misses,missing,strict_late+missing,late+missing,pipeline_lag,spikes,loop_verifying?"full_RAM_and_registers":"disabled");
        fprintf(stderr,"PASS: running-loop test completed\n"); fflush(stderr); std::_Exit(0);
    }
    loop_frontier=frame;
    uint64_t release=epoch+(frame-at)*period;
    if(!loop_oracle && !loop_verifying) {
        loop_wait_until(release);
    }
    cycle_release=release; cycle_start=Probe::now();
    unsigned next_index=frame-at;
    unsigned hitch_ms=setting("BM64_HITCH_MS",0), hitch_kind=setting("BM64_HITCH_KIND",1);
    if(!loop_oracle && hitch_ms && next_index>=241 && (next_index-241)%300==0) {
        if(hitch_kind==1) {
            auto start=Probe::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(hitch_ms));
            fprintf(stderr,"CPU_HITCH: frame=%u start_ns=%llu end_ns=%llu\n",next_index+1,start,Probe::now());
        } else {
            require(hitch_kind==2,"unknown hitch kind");
            Probe::present_hitch_ms=hitch_ms; Probe::present_hitch_frame=next_index+1;
        }
    }
    execution_start=Probe::now(); gfx_before=graphics_wait_ns; cycle_profile=profile(); checkpoint_ms=0; restore_ms=0;
    presentation_frame=loop_oracle ? 0 : frame-at+1; presentation_deadline=(loop_verifying ? execution_start : release)+period;
    normal_hidden=!loop_oracle && Probe::bounded_present && execution_start>presentation_deadline;
    suppress_render=normal_hidden;
    uint64_t first_late=at+loop_warmup+loop_lead+loop_length;
    if(!loop_oracle && loop_length && frame>=first_late && (frame-first_late)%loop_spacing==0) {
        uint64_t wanted=frame-loop_length; auto slot=1+wanted%16;
        require(snapshots[slot].saved_frame==wanted,"rollback history expired");
        catchup_active=true; suppress_render=true; replay_pass=1; ++rollbacks;
        restore_ms=restore(slot,true);
    }
    return true;
}
