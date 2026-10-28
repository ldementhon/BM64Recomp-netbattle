#include "../src/online/fingerprint.hpp"
#include <cassert>
#include <iostream>
#include <random>

using namespace online::lobby;
int main(int argc, char** argv) {
    assert(argc == 1);
    namespace fs = std::filesystem;
    auto directory = fs::temp_directory_path() / ("bm64-fingerprint-" + std::to_string(std::random_device{}()));
    assert(fs::create_directory(directory));
    fs::path game = directory / "game", rom = directory / u8"rom-\u00e9";
    std::ofstream(game) << "abc";
    std::ofstream(rom) << "";
    assert(fingerprint_hex(file_sha256(game)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(fingerprint_hex(file_sha256(rom)) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    auto original = client_fingerprint(game, rom);
#if defined(_WIN32)
    assert(fingerprint_hex(original) == "7c2f3b02b10486a1f2cb7cd03ab948f4bd551f5ab3728e5b77229048dd6399d8");
#elif defined(__aarch64__)
    assert(fingerprint_hex(original) == "78545be658092eece652324c21486329e7b95874583b848398a09ad7203e2739");
#else
    assert(fingerprint_hex(original) == "5cc85e3180b7c875ab290f272b110a2ecdfeceef3b437579c33eee68a0fc98e2");
#endif
    assert(client_fingerprint(game, rom) == original);
    assert(client_fingerprint(rom, game) != original);
    std::ofstream(game) << "abd";
    assert(client_fingerprint(game, rom) != original);
    std::ofstream(game) << "abc";
    std::ofstream(rom) << "xyz";
    assert(client_fingerprint(game, rom) != original);
    std::ofstream(rom) << std::string(200000, 'a');
    assert(fingerprint_hex(file_sha256(rom)) == "2287d207f24a941ff3b56c04c8a25ad56b63e3023207b3bb5b4ac0c9869d74be");
    fs::remove(rom);
    try { client_fingerprint(game, rom); assert(false); } catch (const fs::filesystem_error&) {}
    assert(fs::is_regular_file(current_executable()));
    assert(fs::equivalent(current_executable(), argv[0]));
    fs::remove_all(directory);
    std::cout << "PASS: standard SHA-256 vectors, streaming, build/ROM changes, order, missing files and executable location\n";
}
