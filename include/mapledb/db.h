/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-17 10:54:19
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-17 10:55:40
 * @Description: DB 对外接口（用户直接使用这个头文件）
 */

#pragma once

#include "mapledb/options.h"
#include "mapledb/slice.h"
#include "mapledb/status.h"

#include <string>

namespace mapledb {

class DB {
public:
    // 打开/创建一个 DB
    //   options : 配置项（block_size, write_buffer_size 等）
    //   dbname  : DB 目录路径
    //   dbptr   : 成功时输出 DB 指针（调用方负责 delete）
    static Status Open(const Options& options, const std::string& dbname, DB** dbptr);

    virtual ~DB() = default;

    // 写入一个 key-value 对
    virtual Status Put(const std::string& key, const std::string& value) = 0;

    // 删除一个 key（写入 tombstone）
    virtual Status Delete(const std::string& key) = 0;

    // 查询一个 key 对应的 value
    // 找到返回 OK，未找到返回 Status::NotFound
    virtual Status Get(const std::string& key, std::string* value) = 0;

    DB(const DB&)            = delete;
    DB& operator=(const DB&) = delete;

protected:
    DB() = default;
};

} // namespace mapledb