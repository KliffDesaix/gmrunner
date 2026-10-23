// Filesystem helpers that also work with libnx device paths ("sdmc:/...", "romfs:/...").
#pragma once

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gm {

// mkdir -p. std::filesystem::create_directories can trip over device prefixes like "sdmc:".
inline void makeDirs(const std::string& path) {
    std::string cur;
    size_t i = 0;
    size_t colon = path.find(":/");
    if (colon != std::string::npos && path.find('/') > colon) {  // keep "sdmc:/" as the root
        cur = path.substr(0, colon + 2);
        i = colon + 2;
    } else if (!path.empty() && path[0] == '/') {
        cur = "/";
        i = 1;
    }
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        if (j > i) {
            cur += path.substr(i, j - i);
            mkdir(cur.c_str(), 0755);
            cur += '/';
        }
        i = j + 1;
    }
}

// A whole file, or `size` bytes at `offset`, in one read (byte-wise stream iterators are many
// times slower, which matters for the large audio files games ship on the Switch's SD card).
template <typename Byte = uint8_t>
inline bool readFile(const std::string& path, std::vector<Byte>& out, uint64_t offset = 0, int64_t size = -1) {
    static_assert(sizeof(Byte) == 1, "byte vectors only");
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    if (size < 0) {
        if (fseeko(f, 0, SEEK_END) != 0) return fclose(f), false;
        int64_t end = ftello(f);
        size = end > static_cast<int64_t>(offset) ? end - static_cast<int64_t>(offset) : 0;
    }
    out.resize(static_cast<size_t>(size));
    bool ok = fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0 &&
              (size == 0 || fread(out.data(), 1, out.size(), f) == out.size());
    fclose(f);
    return ok;
}

inline bool isFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

inline bool isDir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// rm -r (directory_destroy).
inline void removeTree(const std::string& path) {
    if (DIR* d = opendir(path.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            std::string p = path + "/" + n;
            if (isDir(p)) removeTree(p);
            else unlink(p.c_str());
        }
        closedir(d);
    }
    rmdir(path.c_str());
}

inline std::string parentDir(const std::string& path) {
    size_t p = path.find_last_of('/');
    return p == std::string::npos ? std::string() : path.substr(0, p);
}

}  // namespace gm
