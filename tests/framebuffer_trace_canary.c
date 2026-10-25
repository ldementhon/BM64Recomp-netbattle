#include <stdint.h>
uint32_t bm64_trace_canary(void* p) { volatile uint32_t* word=p; uint32_t a=*word; *word=a; return *word; }
