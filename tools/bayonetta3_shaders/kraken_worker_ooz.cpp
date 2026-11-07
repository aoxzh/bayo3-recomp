// Offline worker adapted from enginefuture/bayonetta3-mobile's
// tools/rom_decoder_worker.cpp at commit e5ce11694d380d70b53c4ec7c4f789b458058d3e.
// The upstream MIT notice is in LICENSE.bayonetta3-mobile in this directory.
// This adapter expects the C++ Kraken_Decompress API from powzix/ooz; the
// decoder is separate and its files and combined artifacts have their own
// license considerations. Only synthetic blocks have been tested here.
#include <cstdint>
#include <cstdio>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int Kraken_Decompress(const uint8_t *, size_t, uint8_t *, size_t);

int main() {
    // The worker protocol stores sizes and results in little-endian byte order.
    const uint16_t one = 1;
    if (*reinterpret_cast<const uint8_t *>(&one) != 1) return 9;
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 ||
        _setmode(_fileno(stdout), _O_BINARY) == -1) return 8;
#endif
    uint64_t sizes[2];
    for (;;) {
        const size_t header = fread(sizes, 1, sizeof sizes, stdin);
        if (!header) return 0;
        if (header != sizeof sizes) return 2;
        if (sizes[0] > 512 * 1024 * 1024 || sizes[1] > 512 * 1024 * 1024) return 3;
        // The decoder requires additional readable/writable slack.
        std::vector<uint8_t> input(sizes[0] + 64), output(sizes[1] + 64);
        if (fread(input.data(), 1, sizes[0], stdin) != sizes[0]) return 4;
        const int64_t count = Kraken_Decompress(input.data(), sizes[0], output.data(), sizes[1]);
        if (fwrite(&count, 1, sizeof count, stdout) != sizeof count) return 5;
        if (count == static_cast<int64_t>(sizes[1]) &&
            fwrite(output.data(), 1, static_cast<size_t>(count), stdout) != static_cast<size_t>(count)) return 6;
        if (fflush(stdout)) return 7;
    }
}
