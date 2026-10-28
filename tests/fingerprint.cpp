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
    assert(fingerprint_hex(original) == "31dc111ab45fe125df5e6e37f9d1dab21c62d80fa9543a4024165eb6341454bb");
#elif defined(__aarch64__)
    assert(fingerprint_hex(original) == "880909dd52bd5e8902f79b0bf847a6b1cd708079c3dec8451d9dbdf917a0080d");
#else
    assert(fingerprint_hex(original) == "56fb8605b36200b4c2a5e2383886efc80c54bc642ffb0a9ec1d33448880a8fad");
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
