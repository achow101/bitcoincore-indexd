// Copyright (c) 2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <db_leveldb.h>

#include <utils.h>

#include <memory>

#include <leveldb/cache.h>
#include <leveldb/env.h>
#include <leveldb/filter_policy.h>
#include <memenv.h>
#include <stdint.h>
#include <algorithm>

class CBitcoinLevelDBLogger : public leveldb::Logger {
public:
    // This code is adapted from posix_logger.h, which is why it is using vsprintf.
    // Please do not do this in normal code
    void Logv(const char * format, va_list ap) override {

    }
};

static void SetMaxOpenFiles(leveldb::Options *options) {
    // On most platforms the default setting of max_open_files (which is 1000)
    // is optimal. On Windows using a large file count is OK because the handles
    // do not interfere with select() loops. On 64-bit Unix hosts this value is
    // also OK, because up to that amount LevelDB will use an mmap
    // implementation that does not use extra file descriptors (the fds are
    // closed after being mmaped).
    //
    // Increasing the value beyond the default is dangerous because LevelDB will
    // fall back to a non-mmap implementation when the file count is too large.
    // On 32-bit Unix host we should decrease the value because the handles use
    // up real fds, and we want to avoid fd exhaustion issues.
    //
    // See PR #12495 for further discussion.

    int default_open_files = options->max_open_files;
#ifndef WIN32
    if (sizeof(void*) < 8) {
        options->max_open_files = 64;
    }
#endif
    LogPrintf("LevelDB using max_open_files=%d (default=%d)\n",
             options->max_open_files, default_open_files);
}

static leveldb::Options GetOptions(size_t nCacheSize)
{
    leveldb::Options options;
    options.block_cache = leveldb::NewLRUCache(nCacheSize / 2);
    options.write_buffer_size = nCacheSize / 4; // up to two write buffers may be held in memory simultaneously
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    options.compression = leveldb::kNoCompression;
    options.info_log = new CBitcoinLevelDBLogger();
    if (leveldb::kMajorVersion > 1 || (leveldb::kMajorVersion == 1 && leveldb::kMinorVersion >= 16)) {
        // LevelDB versions before 1.16 consider short writes to be corruption. Only trigger error
        // on corruption in later versions.
        options.paranoid_checks = true;
    }
    SetMaxOpenFiles(&options);
    return options;
}

CDBWrapper::CDBWrapper(const std::string& path, size_t nCacheSize, bool fMemory, bool fWipe, bool obfuscate)
    : m_name(path)
{
    penv = nullptr;
    readoptions.verify_checksums = true;
    iteroptions.verify_checksums = true;
    iteroptions.fill_cache = false;
    syncoptions.sync = true;
    options = GetOptions(nCacheSize);
    options.create_if_missing = true;
    if (fMemory) {
        penv = leveldb::NewMemEnv(leveldb::Env::Default());
        options.env = penv;
    } else {
        if (fWipe) {
            LogPrintf("Wiping LevelDB in %s\n", path.c_str());
            leveldb::Status result = leveldb::DestroyDB(path, options);
            dbwrapper_private::HandleError(result);
        }
        //TryCreateDirectories(path);
        LogPrintf("Opening LevelDB in %s\n", path.c_str());
    }
    leveldb::Status status = leveldb::DB::Open(options, path, &pdb);
    dbwrapper_private::HandleError(status);
    LogPrintf("Opened LevelDB successfully\n");

    /*if (gArgs.GetBoolArg("-forcecompactdb", false)) {
        LogPrintf("Starting database compaction of %s\n", path.c_str();
        pdb->CompactRange(nullptr, nullptr);
        LogPrintf("Finished database compaction of %s\n", path.c_str()));
    }*/
}

CDBWrapper::~CDBWrapper()
{
    delete pdb;
    pdb = nullptr;
    delete options.filter_policy;
    options.filter_policy = nullptr;
    delete options.info_log;
    options.info_log = nullptr;
    delete options.block_cache;
    options.block_cache = nullptr;
    delete penv;
    options.env = nullptr;
}

bool CDBWrapper::WriteBatch(CDBBatch& batch, bool fSync)
{
    const bool log_memory = true;
    double mem_before = 0;
    if (log_memory) {
        mem_before = DynamicMemoryUsage() / 1024.0 / 1024;
    }
    leveldb::Status status = pdb->Write(fSync ? syncoptions : writeoptions, &batch.batch);
    dbwrapper_private::HandleError(status);
    if (log_memory) {
        double mem_after = DynamicMemoryUsage() / 1024.0 / 1024;
        LogPrintf("WriteBatch memory usage: db=%s, before=%.1fMiB, after=%.1fMiB\n",
                 m_name.c_str(), mem_before, mem_after);
    }
    return true;
}

size_t CDBWrapper::DynamicMemoryUsage() const {
    std::string memory;
    if (!pdb->GetProperty("leveldb.approximate-memory-usage", &memory)) {
        LogPrintf("Failed to get approximate-memory-usage property\n");
        return 0;
    }
    return stoul(memory);
}

bool CDBWrapper::IsEmpty()
{
    std::unique_ptr<CDBIterator> it(NewIterator());
    it->SeekToFirst();
    return !(it->Valid());
}

CDBIterator::~CDBIterator() { delete piter; }
bool CDBIterator::Valid() const { return piter->Valid(); }
void CDBIterator::SeekToFirst() { piter->SeekToFirst(); }
void CDBIterator::Next() { piter->Next(); }

namespace dbwrapper_private {

void HandleError(const leveldb::Status& status)
{
    if (status.ok())
        return;
    const std::string errmsg = "Fatal LevelDB error: " + status.ToString();
    LogPrintf("%s\n", errmsg.c_str());
    throw dbwrapper_error(errmsg);
}

} // namespace dbwrapper_private

DatabaseLEVELDB::DatabaseLEVELDB(const std::string& path) : db(path, 300*1024*1024, false, false, true) {

}

bool DatabaseLEVELDB::put_txindex(const uint8_t* key, unsigned int key_len, const uint8_t* value, unsigned int value_len) {
    std::vector<uint8_t> v_key(key, key+key_len);
    v_key.insert(v_key.begin(), 'T');
    cache[v_key] = std::vector<uint8_t>(value, value + value_len);
    if (cache.size() == 100000) {
        CDBBatch batch(db);
        for (auto const& it : cache) {
            batch.Write(it.first, it.second);
        }
        db.WriteBatch(batch);
        batch.Clear();
        cache.clear();
    }
}

bool DatabaseLEVELDB::put_header(const uint8_t* key, unsigned int key_len, const uint8_t* value, unsigned int value_len) {
    std::vector<uint8_t> v_key(key, key+key_len);
    v_key.insert(v_key.begin(), 'H');
    cache[v_key] = std::vector<uint8_t>(value, value + value_len);
}

bool DatabaseLEVELDB::close() {
    return true;
}
