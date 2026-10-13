// SPDX-License-Identifier: GPL-2.0-or-later
#include "gpu/shim/fsr_windows_headers.h"
int main() {
    alignas(std::mutex) unsigned char storage[sizeof(std::mutex)];
    auto* mutex = new (storage) std::mutex();
    { std::lock_guard<std::mutex> guard(*mutex); }
    mutex->~mutex();
    if (log2(8.0) != 3.0) return 1;
    int values[3] = {0};
    static_assert(_countof(values) == 3);
    wchar_t output[32] = {};
    const int count = swprintf_s(output, _countof(output), L"%ls", L"check");
    return count != 5 || output[0] != L'c' || output[5] != L'\0';
}
