// Included inside checkpoint. This observer is not part of snapshot state.
#ifdef BM64_TRACE_MEMORY
extern "C" uint32_t bm64_trace_canary(void*);
static uint8_t* traced_ram;
static uint64_t trace_report_at;
static std::map<std::tuple<uint32_t,size_t,int,unsigned>,uint64_t> texture_reads;
static std::vector<uint8_t> pixel_owner;
struct TraceRead { uint64_t count=0; uint32_t address=0; uint64_t first_frame=0; };
static std::map<std::pair<uintptr_t,int>,TraceRead> trace_reads;
static std::map<std::tuple<uint32_t,size_t,bool>,uint64_t> trace_writes;
void framebuffer_written(void* pointer, size_t count, bool depth) {
    if (!traced_ram) return;
    size_t offset = static_cast<uint8_t*>(pointer)-traced_ram;
    require(offset+count <= pixel_owner.size(), "framebuffer trace outside RAM");
    std::fill_n(pixel_owner.begin()+offset,count,depth ? 2 : 1);
    if(frame>=trace_report_at) ++trace_writes[{uint32_t(offset),count,depth}];
}
void framebuffer_texture_read(const void* pointer, size_t count, unsigned kind) {
    uintptr_t address=reinterpret_cast<uintptr_t>(pointer),base=reinterpret_cast<uintptr_t>(traced_ram);
    if(!base || frame<trace_report_at || address<base || address-base+count>pixel_owner.size()) return;
    int owners=0; for(size_t i=0;i<count;++i) owners |= pixel_owner[address-base+i];
    if(owners) ++texture_reads[{uint32_t(address-base),count,owners,kind}];
}
static void trace_access(void* pointer, size_t count, bool store, void* pc) {
    uintptr_t address = reinterpret_cast<uintptr_t>(pointer), base = reinterpret_cast<uintptr_t>(traced_ram);
    if (!base || address < base || address-base+count > pixel_owner.size()) return;
    size_t offset=address-base;
    int owners=0;
    for (size_t i=0;i<count;++i) { owners |= pixel_owner[offset+i]; if (store) pixel_owner[offset+i]=0; }
    if (!store && owners && frame>=trace_report_at) {
        auto& hit=trace_reads[{reinterpret_cast<uintptr_t>(pc),owners}];
        if (!hit.count) { hit.address=offset; hit.first_frame=frame; }
        ++hit.count;
    }
}
#endif
static bool trace_frame(unsigned at) {
    unsigned count=setting("BM64_TRACE_FRAMES",0);
    if (!count) return false;
#ifdef BM64_TRACE_MEMORY
    if (frame == 1) {
        pixel_owner.resize(recomp::mem_size); traced_ram=ram;
        std::fill_n(pixel_owner.end()-4096,4,1);
        framebuffer_texture_read(ram+recomp::mem_size-4096,4,99);
        require(texture_reads.size()==1,"texture observer canary failed");
        bm64_trace_canary(ram+recomp::mem_size-4096);
        require(trace_reads.size()==1 && trace_reads.begin()->second.count==1,"instrumented load/store canary failed");
        framebuffer_texture_read(ram+recomp::mem_size-4096,4,99);
        require(texture_reads.begin()->second==1,"CPU overwrite did not clear texture ownership");
        trace_reads.clear(); texture_reads.clear(); trace_report_at=at;
        fprintf(stderr,"PASS: instrumented read observed; CPU overwrite cleared framebuffer ownership\n");
    }
    if (frame == at+count) {
        traced_ram=nullptr;
        for (const auto& [key,n]:trace_writes) fprintf(stderr,"FBWRITE: address=%08x bytes=%zu depth=%d count=%llu\n",std::get<0>(key),std::get<1>(key),std::get<2>(key),n);
        for(const auto& [key,n]:texture_reads) fprintf(stderr,"FBTEXTURE: address=%08x bytes=%zu owner=%d command=%u count=%llu\n",std::get<0>(key),std::get<1>(key),std::get<2>(key),std::get<3>(key),n);
        for (const auto& [key,hit]:trace_reads) {
            Dl_info info{}; dladdr(reinterpret_cast<void*>(key.first),&info);
            fprintf(stderr,"FBREAD: owner=%d first_address=%08x first_frame=%llu reads=%llu function=%s offset=%zu\n",key.second,hit.address,hit.first_frame,hit.count,info.dli_sname ? info.dli_sname : "unknown",key.first-reinterpret_cast<uintptr_t>(info.dli_saddr));
        }
        fprintf(stderr,"PASS: framebuffer read trace completed %u updates (%zu read sites, %zu write regions)\n",count,trace_reads.size(),trace_writes.size()); fflush(stderr); std::_Exit(0);
    }
    return true;
#else
    require(false,"framebuffer read tracing requires BM64_TRACE_MEMORY build"); return true;
#endif
}
