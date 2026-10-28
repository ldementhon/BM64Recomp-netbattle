// Differential test against RT64's pinned original hash algorithm.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include "xxHash/xxh3.h"
#include "shared/rt64_f3d_defines.h"
#include "common/rt64_load_types.h"
#include "trial/src/common/rt64_tmem_hasher.h"
#define TMEMHasher OriginalTMEMHasher
#define bitScanForward64 originalBitScanForward64
#include "original_tmem_hasher.h"
#undef bitScanForward64
#undef TMEMHasher
int main() {
    std::mt19937 rng(0x64);
    std::array<uint8_t,4096> tmem;
    unsigned checks=0;
    for(unsigned trial=0; trial<20000; ++trial) {
        // Uniform, sparse, dense, and contiguous palette membership; also random data.
        unsigned span=1+(trial%256);
        for(auto& v:tmem) v=(trial%4==0) ? uint8_t(trial) : (trial%4==1) ? uint8_t(rng()%span) : uint8_t(rng());
        RT64::LoadTile tile{};
        tile.siz=trial%4; tile.fmt=(trial%3==0) ? G_IM_FMT_RGBA : G_IM_FMT_CI;
        tile.line=rng()%64; tile.tmem=rng()%512; tile.palette=rng()%16;
        uint16_t width=1+rng()%128, height=1+rng()%64;
        if(RT64::TMEMHasher::requiresRawTMEM(tile,width,height)) continue;
        for(unsigned version=0;version<=5;++version) for(unsigned tlut:{0U,2U,3U}) {
            auto expected=RT64::OriginalTMEMHasher::hash(tmem.data(),tile,width,height,tlut,version);
            for(bool coalesce:{false,true}) {
                auto actual=coalesce ? RT64::TMEMHasher::hash<true>(tmem.data(),tile,width,height,tlut,version) : RT64::TMEMHasher::hash<false>(tmem.data(),tile,width,height,tlut,version);
                if(actual!=expected) { fprintf(stderr,"FAIL: trial=%u version=%u tlut=%u optimized=%d\n",trial,version,tlut,coalesce); return 1; }
                ++checks;
            }
        }
    }
    printf("PASS: %u whole-texture hashes match pinned original, including palette, odd-row and TMEM wrapping cases\n",checks);
}
