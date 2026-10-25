#include "../src/game/checkpoint_memory.hpp"
#include <sys/mman.h>
#include <unistd.h>
#include <array>
#include <cstdlib>
#include <cstdio>
static void check(bool ok) { if (!ok) std::abort(); }
int main() {
    size_t page = getpagesize(), size = 512ull << 20;
    auto* allocation = static_cast<uint8_t*>(mmap(nullptr, size+2*page, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0));
    check(allocation != MAP_FAILED); auto* ram = allocation+page;
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
    munmap(allocation,size+2*page);
    puts("PASS: COW ring eviction, immutable images, repeated fixed-address restores, future sparse writes and adjacent mappings");
}
