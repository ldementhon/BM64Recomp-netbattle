#pragma once
#include <cstdint>
#include <string>

// One number for each set of Mac and Windows downloads sent out together.
// Bump it before building new downloads; server/make_release.py reads it from here.
#define BM64_ONLINE_RELEASE 1
#define BM64_ONLINE_STRING2(x) #x
#define BM64_ONLINE_STRING(x) BM64_ONLINE_STRING2(x)

namespace online {
constexpr uint16_t release = BM64_ONLINE_RELEASE;
// Shown in the launcher; make_release.py also finds it in each executable it packages.
constexpr char release_name[] = "Online v" BM64_ONLINE_STRING(BM64_ONLINE_RELEASE);
inline std::string release_label(uint16_t number) { return "v" + std::to_string(number); }
}
