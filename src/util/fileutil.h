#pragma once
#include "mapledb/status.h"
#include <string>

namespace mapledb {
Status SyncDirectory(const std::string& path);
Status CreateDBDirectory(const std::string& path);
} // namespace mapledb
