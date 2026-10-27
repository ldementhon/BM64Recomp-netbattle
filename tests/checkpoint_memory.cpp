#include "../src/game/checkpoint_memory.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <array>
#include <cstdlib>
#include <cstdio>
static void check(bool ok) { if (!ok) std::abort(); }
int main() {
    size_t size = 512ull << 20;
#ifdef _WIN32
    SYSTEM_INFO info; GetSystemInfo(&info);
    size_t page = info.dwPageSize;
    auto* allocation = static_cast<uint8_t*>(VirtualAlloc(nullptr, size+2*page, MEM_RESERVE|MEM_COMMIT|MEM_WRITE_WATCH, PAGE_READWRITE));
    check(allocation != nullptr);
#else
    size_t page = getpagesize();
    auto* allocation = static_cast<uint8_t*>(mmap(nullptr, size+2*page, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0));
    check(allocation != MAP_FAILED);
#endif
    auto* ram = allocation+page;
    allocation[0]=91; ram[size]=92;
    std::array<CheckpointMemory,16> ring;
    for (unsigned i=0;i<48;++i) {
        ram[0]=i; ram[size-1]=i+1; ring[i%16].capture(ram,size);
        ram[size/2+i*page]=i+1;
    }
    for (unsigned i=32;i<48;++i) {
        ring[i%16].restore(ram);
        check(ram[0]==i && ram[size-1]==i+1);
        for (unsigned j=0;j<48;++j) check(ram[size/2+j*page] == (j<i ? j+1 : 0));
        ram[0]=255; ram[size-1]=255;
        check(allocation[0]==91 && ram[size]==92);
    }
#ifdef _WIN32
    // The ring holds about 80 distinct pages; unshared images would hold over 600.
    auto stored = [&] { size_t bytes = 0; for (auto& image : ring) bytes += image.copied_bytes(); return bytes; };
    check(stored() < 200*page);
    // Pages written back to zero store nothing.
    for (unsigned j=0;j<100;++j) { ram[size/4+j*page]=1; ram[size/4+j*page]=0; }
    size_t before = stored(); ring[7].capture(ram,size);
    check(stored() < before + 10*page);
#endif
    // Rollback reuses a slot right after restoring it: restore, replay, capture, restore.
    ring[5].restore(ram); check(ram[0]==37);
    ram[0]=200; ram[size/2+60*page]=7; ring[5].capture(ram,size);
    ram[0]=201; ram[size/2+60*page]=8; ram[size/2+61*page]=9;
    ring[6].restore(ram); check(ram[0]==38 && ram[size/2+60*page]==0 && ram[size/2+61*page]==0);
    ring[5].restore(ram); check(ram[0]==200 && ram[size/2+60*page]==7 && ram[size/2+61*page]==0);
#ifdef _WIN32
    // Write-watched images hold only pages the source has written.
    check(ring[5].copied_bytes() < size/64);
#endif
    ram[0]=255; ram[size-1]=255;
    CheckpointMemory reference; reference.capture(ram,size,true); ram[0]=3; reference.restore(ram);check(ram[0]==255);
    // Replacing an existing snapshot must not mutate snapshots that share its
    // old pages. Exercise both storage-mode transitions and a size change.
    reference.capture(ram,size); ram[0]=17;
    reference.restore(ram); check(ram[0]==255);
    reference.capture(ram,size,true); ram[0]=18;
    reference.restore(ram); check(ram[0]==255);
    reference.capture(ram,size/2); ram[0]=19;
    reference.restore(ram); check(ram[0]==255);
    ring[0].restore(ram); check(ram[0]==32 && ram[size-1]==33);
    check(allocation[0]==91 && ram[size]==92);
#ifdef _WIN32
    check(VirtualFree(allocation, 0, MEM_RELEASE));
#else
    munmap(allocation,size+2*page);
#endif
    puts("PASS: snapshot ring eviction, immutable images, repeated fixed-address restores, future sparse writes and adjacent mappings");
}
