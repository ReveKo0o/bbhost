// A big file saved in the background (the shader caches), written so that it
// does not hold up everything else that reads or writes files meanwhile.
#pragma once

#include <cstddef>
#include <cstdio>
#include <string>

// Written to `tmp` a chunk at a time, each chunk on the device (fdatasync,
// _commit) before the next is written, then renamed over `path`. Written at
// once, a cache of a few hundred MB was seconds of write-back that the other
// threads' file operations queued behind on a slow or shared disk: over NFS a
// read of a game file waited 0.6 s and a directory listing 1.5 s. A chunk at a
// time, they wait for a chunk at most.
class HostPacedFile {
public:
    HostPacedFile(std::string path, std::string tmp, std::size_t chunk = 8u << 20);
    ~HostPacedFile();  // not committed: the temporary is removed
    HostPacedFile(const HostPacedFile&) = delete;
    HostPacedFile& operator=(const HostPacedFile&) = delete;
    bool write(const void* data, std::size_t size);
    // What is left on the device, the file closed and renamed over the path.
    // False when any of it failed: the temporary removed, the old file kept.
    bool commit();

private:
    bool sync();
    std::string path_, tmp_;
    std::FILE* f_ = nullptr;
    std::size_t chunk_ = 0, unsynced_ = 0;
    bool ok_ = false, committed_ = false;
};
