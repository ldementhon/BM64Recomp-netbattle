// Included inside checkpoint's namespace; input reads never advance either clock.
// The scripted source is immutable: repeated reads and replay use the same frame key.
static unsigned loop_length, loop_buffer;
static uint64_t loop_at, loop_end;
static bool loop_oracle;
static constexpr unsigned loop_warmup=120, loop_lead=17;
static unsigned loop_spacing=60;
bool boot_input(int controller, uint16_t* buttons, float* x, float* y) {
    *buttons = 0; *x = *y = 0;
    if (controller < 0 || controller >= 4) return false;
    if (controller != 0) return enabled;
    auto f = boot_frame.load();
    if (f == 500 || f == 800 || f == 1000) *buttons = 0x1000;
    if (f >= 1102 && f < 1110) *y = -0.7f;
    if (f == 1120 || f == 1320 || f == 1500) *buttons = 0x8000;
    return true;
}
static uint64_t pending_begin(uint64_t f) {
    uint64_t first=loop_at+loop_warmup+loop_lead;
    if (f<first || !loop_length) return 0;
    uint64_t start=first+(f-first)/loop_spacing*loop_spacing;
    // Leave a normal update after correction so the final state is verified too.
    if (start+loop_length>=loop_end) return 0;
    return f<start+loop_length ? start : 0;
}
static uint64_t input_frame(int controller) {
    if (!loop_at || frame<loop_at) return frame;
    auto sample=frame-loop_buffer;
    // This fixture has one local player and three independently valued remote pads.
    if (controller>0 && !loop_oracle && !replay_pass) {
        auto begin=pending_begin(frame);
        if (begin) sample=begin-loop_buffer-1; // Hold the last received remote input.
    }
    return sample;
}
struct Input { unsigned begin, end; uint16_t buttons; float x,y; int controller = 0; };
bool input(int controller, uint16_t* buttons, float* x, float* y) {
    if (fast_boot) return boot_input(controller, buttons, x, y);
    if (controller < 0 || controller >= 4) return false;
    static std::vector<Input> inputs = [] {
        std::vector<Input> result;
        if (const char* path = std::getenv("BM64_INPUT_SCRIPT")) {
            std::ifstream in(path); require(in.good(), "input script missing");
            std::string line;
            while (std::getline(in,line)) {
                if (line.empty() || line[0]=='#') continue;
                Input i{}; unsigned b;
                std::istringstream s(line); require(bool(s >> i.begin >> i.end >> std::hex >> b >> std::dec >> i.x >> i.y), "bad input script");
                s >> std::ws;
                if (!s.eof()) require(bool(s >> i.controller), "invalid controller field");
                s >> std::ws; require(s.eof(), "extra input fields");
                require(i.controller >= 0 && i.controller < 4 && b <= 0xffff && i.begin < i.end && i.x >= -1 && i.x <= 1 && i.y >= -1 && i.y <= 1, "input outside valid range");
                i.buttons = b; result.push_back(i);
            }
        }
        return result;
    }();
    *buttons=0; *x=*y=0;
    auto sampled_frame=input_frame(controller);
    for (auto i:inputs) if (controller == i.controller && sampled_frame>=i.begin && sampled_frame<i.end) { *buttons=i.buttons; *x=i.x; *y=i.y; }
    if (controller == 0 && replay_pass && std::getenv("BM64_NEGATIVE_INPUT")) { *buttons ^= 0x8000; *x = std::clamp(-*x + 0.1f, -1.0f, 1.0f); }
    if (online::match_settings_locked) online::filter_match_input(controller, overlay, *buttons, *x, *y);
    return true;
}
