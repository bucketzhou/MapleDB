#pragma once

#include "mapledb/status.h"
#include <fcntl.h>
#include <unistd.h>

namespace mapledb {
// macOS 的 fsync 不保证设备写缓存落盘；与 LevelDB POSIX Env 使用相同的回退策略。
inline int SyncFileDescriptor(int fd) {
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    if (fcntl(fd, F_FULLFSYNC) == 0) return 0;
#endif
    return fsync(fd);
}

inline Status SyncDirectory(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return Status::IOError("cannot open directory: " + path);
    const int result = SyncFileDescriptor(fd);
    close(fd);
    return result == 0 ? Status::OK() : Status::IOError("cannot sync directory: " + path);
}
} // namespace mapledb
