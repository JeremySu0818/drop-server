#ifndef DROP_SERVER_CORE_UPLOAD_STORE_H_
#define DROP_SERVER_CORE_UPLOAD_STORE_H_

#include "core/types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace drop_server {

std::int64_t NowMilliseconds() noexcept;

class UploadStore {
public:
  explicit UploadStore(std::int64_t ttl_ms);

  std::size_t PurgeExpired(std::int64_t now);
  UpsertResult Upsert(std::string key, std::vector<EncryptedFile> files,
                      std::size_t incoming_bytes);
  TakeResult Take(const std::string &key);
  std::size_t Clear();
  Stats GetStats();

  CreateChunkedUploadResult CreateChunkedUpload(std::string lookup_key,
                                                std::vector<ChunkedFile> files);
  StatusResult BeginChunk(const std::string &upload_id,
                          const std::string &file_id, std::uint32_t index,
                          SecureBytes iv, std::size_t content_length);
  void AppendChunkPart(const std::string &upload_id, const std::string &file_id,
                       std::uint32_t index, const ByteView &bytes);
  StatusResult FinishChunk(const std::string &upload_id,
                           const std::string &file_id, std::uint32_t index);
  void FailChunk(const std::string &upload_id, const std::string &file_id,
                 std::uint32_t index);
  CompleteChunkedUploadResult
  CompleteChunkedUpload(const std::string &upload_id);
  DownloadStatusResult GetDownloadStatus(const std::string &lookup_key);
  BeginChunkedDownloadResult
  BeginChunkedDownload(const std::string &lookup_key);
  AcquireChunkResult AcquireChunkedDownloadChunk(const std::string &download_id,
                                                 const std::string &token,
                                                 const std::string &file_id,
                                                 std::uint32_t index);
  void ReleaseChunkedDownloadChunk(const std::string &download_id,
                                   const std::string &token);
  StatusResult FinishChunkedDownload(const std::string &download_id,
                                     const std::string &token);
  StatusResult AbortChunkedUpload(const std::string &upload_id);

private:
  std::int64_t ExpiryFromNow() const noexcept;
  void Touch(ChunkedSession &session) const noexcept;
  static std::size_t ExpectedChunkBytes(const ChunkedFile &file,
                                        std::uint32_t index);
  ChunkedSession *FindChunkedSession(const std::string &id);
  static ChunkedFile *FindChunkedFile(ChunkedSession &session,
                                      const std::string &id);
  void EraseChunkedUpload(const std::string &id);

  std::unordered_map<std::string, UploadRecord> uploads_;
  std::unordered_map<std::string, ChunkedSession> chunked_uploads_;
  std::unordered_map<std::string, std::string> chunked_lookup_;
  std::int64_t ttl_ms_;
};

} // namespace drop_server

#endif
