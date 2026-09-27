#pragma once
#include "common/FileType.h"

#include <objidl.h>
#include <cstdint>
#include <functional>
#include <string>

namespace ct {

// Explorer asks for different sizes; we render in three buckets and scale down.
int SizeBucket(unsigned cx);

// Cache key = hash(size, mtime, content samples, type, bucket, render settings).
// Reader reads `len` bytes at `offset`, returns bytes read.
using ReadAtFn = std::function<size_t(uint64_t offset, void* buf, size_t len)>;
std::string ComputeCacheKey(const ReadAtFn& read, uint64_t size, uint64_t mtime, FileType type, int bucket,
                            uint64_t renderSignature);
bool CacheKeyFromStream(IStream* stream, uint64_t size, uint64_t mtime, FileType type, int bucket,
                        uint64_t renderSignature, std::string& key);
bool CacheKeyFromFile(const std::wstring& path, FileType type, int bucket, uint64_t renderSignature,
                      std::string& key, uint64_t* sizeOut = nullptr);

enum class CacheState { Missing, Ready, Failed };

std::wstring CachePngPath(const std::string& key);
std::wstring CacheFailPath(const std::string& key);
CacheState QueryCache(const std::string& key, int failRetryHours);
bool CommitSuccess(const std::string& key, const std::wstring& renderedPng);
void CommitFailure(const std::string& key, const std::wstring& reason);
void RemoveCacheEntry(const std::string& key);

struct CacheStats {
    uint64_t files = 0, bytes = 0, failures = 0;
};
CacheStats GetCacheStats();
void PruneCache(int maxMB, int failRetryHours);
void ClearCache();
void CleanupTempDir(int olderThanHours);

} // namespace ct
