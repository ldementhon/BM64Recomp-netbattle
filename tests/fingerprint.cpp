#include "../src/online/fingerprint.hpp"
#include <cassert>
#include <iostream>
#include <unistd.h>

using namespace online::lobby;
int main() {
    namespace fs = std::filesystem;
    auto pattern = (fs::temp_directory_path() / "bm64-fingerprint-XXXXXX").string();
    assert(mkdtemp(pattern.data()));
    fs::path directory(pattern), game = directory / "game", rom = directory / "rom";
    std::ofstream(game) << "abc";
    std::ofstream(rom) << "";
    assert(fingerprint_hex(file_sha256(game)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(fingerprint_hex(file_sha256(rom)) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    auto original = client_fingerprint(game, rom);
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
    fs::remove_all(directory);
    std::cout << "PASS: standard SHA-256 vectors, streaming, build/ROM changes, order, missing files and executable location\n";
}
