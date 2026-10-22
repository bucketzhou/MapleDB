#include "util/fileutil.h"
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>

namespace mapledb {
Status SyncDirectory(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return Status::IOError("cannot open directory: " + path, strerror(errno));
    Status s;
    // 文件内容同步不保证目录项存在；新建/rename 后必须再同步目录。
    if (fsync(fd) != 0)
        s = Status::IOError("cannot sync directory: " + path, strerror(errno));
    if (close(fd) != 0 && s.Ok())
        s = Status::IOError("cannot close directory: " + path, strerror(errno));
    return s;
}

Status CreateDBDirectory(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code error;
    bool created = fs::create_directories(path, error);
    if (error)
        return Status::IOError("cannot create database directory: " + path, error.message());
    if (created) {
        auto parent = fs::absolute(path, error).parent_path();
        if (error)
            return Status::IOError("cannot resolve database directory", error.message());
        // 多级目录可能同时新建，逐级同步父目录，保证整条目录路径可恢复。
        for (;;) {
            Status s = SyncDirectory(parent.string());
            if (!s.Ok())
                return s;
            if (parent == parent.root_path())
                break;
            parent = parent.parent_path();
        }
    }
    return Status::OK();
}
} // namespace mapledb
