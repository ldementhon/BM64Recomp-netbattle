#include "../server/release_file.hpp"
#include <cassert>
#include <iostream>

using namespace online::lobby;
static const std::string mac(64, 'a'), windows = "0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef";

static ReleaseFile parse(const std::string& text) {
    std::istringstream input(text);
    return parse_release_file(input);
}
static void invalid(const std::string& text) {
    try { parse(text); assert(false); } catch (const std::invalid_argument&) {}
}

int main() {
    auto file = parse("# Written by make_release.py\r\nrelease 3\r\n\r\nmacOS-arm64 " + mac + "\r\nWindows-x86_64 " + windows + "\r\n");
    assert(file.release == 3 && file.builds.size() == 2);
    Fingerprint aa; aa.fill(0xaa);
    assert(file.builds[0].first == "macOS-arm64" && file.builds[0].second == aa);
    assert(file.builds[1].first == "Windows-x86_64" && file.builds[1].second[8] == 0x01 && file.builds[1].second[15] == 0xef);

    invalid("");                                            // no release or builds
    invalid("release 3\n");                                 // no builds
    invalid("macOS-arm64 " + mac + "\nrelease 3\n");        // build before release
    invalid("release 3\nrelease 4\nmacOS-arm64 " + mac);    // release twice
    for (auto release : {"0", "65536", "3a", "-1"}) invalid(std::string("release ") + release + "\nmacOS-arm64 " + mac);
    invalid("release 3\nmacOS-arm64 " + mac.substr(1));     // short fingerprint
    invalid("release 3\nmacOS-arm64 " + mac.substr(1) + "g");
    invalid("release 3\nmacOS-arm64 " + mac + " extra");
    invalid("release 3\nmacOS-arm64\n");
    invalid("release 3\nmacOS-arm64 " + mac + "\nmacOS-arm64 " + windows);  // duplicate label
    invalid("release 3\nmacOS-arm64 " + mac + "\nWindows-x86_64 " + mac);   // duplicate build
    std::string five = "release 3\n";
    for (char c : std::string("abcde")) five += std::string(1, c) + " " + std::string(63, '0') + c + "\n";
    invalid(five);
    std::cout << "PASS: release files name one release and up to four builds; malformed files are refused\n";
}
