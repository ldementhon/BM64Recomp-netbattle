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
    assert(fingerprint_hex(original) == "0d614776ff7f13ca6213e038c7aa0aa7995ebb1f4cd4810659225499e7a3928a");
#elif defined(__aarch64__)
    assert(fingerprint_hex(original) == "47a0e4e80addf506bdebd3755260adffb9ddeb1735b55c93862427850614d041");
#else
    assert(fingerprint_hex(original) == "3cce2fc53b49135f11879b9714284ea37da1aa31ed8c9bb54d66a871ad315612");
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
