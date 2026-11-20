// TgDesk payload packer (build tool): packs a folder into one LZMS-compressed archive and appends it to the setup exe.
//   pack.exe <stage folder> <setup stub.exe> <output setup.exe>
// Archive:  "TGDPACK1" u32 count u64 totalOriginalBytes, then per file: u16 nameLen, wchar name[nameLen] (relative path),
//           u64 originalSize, u64 compressedSize, bytes.
// Trailer of the output exe: [stub][archive][u64 archiveSize]["TGDPAYLD"].
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <compressapi.h>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

struct Entry { std::wstring rel, full; };

static void walk(const std::wstring& root, const std::wstring& rel, std::vector<Entry>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\" + rel + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) walk(root, rel + n + L"\\", out);
        else out.push_back({rel + n, root + L"\\" + rel + n});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static bool readAll(const std::wstring& p, std::vector<unsigned char>& b) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
    b.resize((size_t)sz.QuadPart);
    size_t done = 0;
    while (done < b.size()) {
        DWORD n = 0, want = (DWORD)std::min<size_t>(b.size() - done, 1 << 24);
        if (!ReadFile(f, b.data() + done, want, &n, nullptr) || n == 0) { CloseHandle(f); return false; }
        done += n;
    }
    CloseHandle(f);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) { fwprintf(stderr, L"usage: pack <stage dir> <stub.exe> <out.exe>\n"); return 2; }
    std::vector<Entry> files;
    walk(argv[1], L"", files);
    if (files.empty()) { fwprintf(stderr, L"no files in %ls\n", argv[1]); return 1; }

    COMPRESSOR_HANDLE c;
    if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &c)) { fwprintf(stderr, L"CreateCompressor failed\n"); return 1; }

    std::vector<unsigned char> arc;
    auto put = [&](const void* p, size_t n) { arc.insert(arc.end(), (const unsigned char*)p, (const unsigned char*)p + n); };
    put("TGDPACK1", 8);
    uint32_t count = (uint32_t)files.size();
    put(&count, 4);
    size_t totalPos = arc.size();
    uint64_t total = 0;
    put(&total, 8);

    for (auto& e : files) {
        std::vector<unsigned char> data;
        if (!readAll(e.full, data)) { fwprintf(stderr, L"cannot read %ls\n", e.full.c_str()); return 1; }
        std::vector<unsigned char> comp;
        SIZE_T csz = 0;
        if (!data.empty()) {
            Compress(c, data.data(), data.size(), nullptr, 0, &csz);
            comp.resize(csz);
            if (!Compress(c, data.data(), data.size(), comp.data(), comp.size(), &csz)) { fwprintf(stderr, L"compress failed: %ls\n", e.rel.c_str()); return 1; }
            comp.resize(csz);
        }
        uint16_t nl = (uint16_t)e.rel.size();
        put(&nl, 2);
        put(e.rel.data(), nl * sizeof(wchar_t));
        uint64_t o = data.size(), k = comp.size();
        put(&o, 8);
        put(&k, 8);
        if (!comp.empty()) put(comp.data(), comp.size());
        total += o;
        wprintf(L"  %-48ls %10llu -> %10llu\n", e.rel.c_str(), (unsigned long long)o, (unsigned long long)k);
    }
    CloseCompressor(c);
    memcpy(arc.data() + totalPos, &total, 8);

    std::vector<unsigned char> stub;
    if (!readAll(argv[2], stub)) { fwprintf(stderr, L"cannot read stub\n"); return 1; }
    HANDLE f = CreateFileW(argv[3], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { fwprintf(stderr, L"cannot write output\n"); return 1; }
    DWORD w;
    uint64_t asz = arc.size();
    bool ok = WriteFile(f, stub.data(), (DWORD)stub.size(), &w, nullptr);
    for (size_t off = 0; ok && off < arc.size(); off += 1 << 24)
        ok = WriteFile(f, arc.data() + off, (DWORD)std::min<size_t>(arc.size() - off, 1 << 24), &w, nullptr);
    ok = ok && WriteFile(f, &asz, 8, &w, nullptr) && WriteFile(f, "TGDPAYLD", 8, &w, nullptr);
    CloseHandle(f);
    if (!ok) { fwprintf(stderr, L"write failed\n"); return 1; }
    wprintf(L"packed %u files, %llu bytes -> %llu bytes\n", count, (unsigned long long)total, (unsigned long long)asz);
    return 0;
}
