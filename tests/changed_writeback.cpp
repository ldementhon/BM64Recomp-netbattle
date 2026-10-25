#include "../lib/rt64/src/common/rt64_changed_writeback.h"
#include <vector>
#include <cstdlib>
#include <cstdio>
static void check(bool ok) { if(!ok) std::abort(); }
int main() {
    uint32_t random=12345;
    for(size_t size:{4,8,4092,4096,4100,153600}) {
        std::vector<uint8_t> raw(size), staging(size), destination(size+64,0x91), expected(size+64,0x91);
        for(unsigned pass=0;pass<100;++pass) {
            for(unsigned change=0;change<13;++change) {
                random^=random<<13; random^=random>>17; random^=random<<5;
                raw[random%size]=uint8_t(random>>8);
            }
            for(size_t i=0;i<size;++i) expected[32+(i^3)]=raw[i];
            staging=raw;
            check(RT64::swapAndCopyChanged(destination.data()+32,staging.data(),size)<=size);
            check(destination==expected);
            staging=raw;
            check(RT64::swapAndCopyChanged(destination.data()+32,staging.data(),size)==0);
            check(destination==expected);
        }
    }
    puts("PASS: changed-block writeback equals full byte-swapped output, preserves guards, and skips identical blocks");
}
