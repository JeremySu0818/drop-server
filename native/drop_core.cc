#include <node_api.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <strings.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#endif

namespace {

constexpr double kMaxSafeInteger = 9007199254740991.0;
constexpr std::size_t kChunkSizeBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kGcmTagBytes = 16;
constexpr const char *kNotFoundMessage =
    "Image not found. It may have already been downloaded or expired.";
constexpr const char *kExpiredMessage = "Image has expired.";

void SecureZero(void *pointer, std::size_t size) noexcept {
  if (pointer == nullptr || size == 0) {
    return;
  }

#if defined(_WIN32)
  SecureZeroMemory(pointer, size);
#elif defined(__GLIBC__) || defined(__linux__) || defined(__APPLE__) ||        \
    defined(__FreeBSD__) || defined(__OpenBSD__)
  explicit_bzero(pointer, size);
#else
  volatile auto *bytes = static_cast<volatile unsigned char *>(pointer);
  while (size-- > 0) {
    *bytes++ = 0;
  }
#endif
}

void ReleaseUnusedMemory() noexcept {
#if defined(__GLIBC__)
  malloc_trim(0);
#endif
}

class SecureBytes {
public:
  SecureBytes() noexcept = default;

  explicit SecureBytes(std::size_t size)
      : data_(size == 0 ? nullptr : std::make_unique<std::uint8_t[]>(size)),
        size_(size) {}

  ~SecureBytes() { Reset(); }

  SecureBytes(const SecureBytes &) = delete;
  SecureBytes &operator=(const SecureBytes &) = delete;

  SecureBytes(SecureBytes &&other) noexcept
      : data_(std::move(other.data_)), size_(std::exchange(other.size_, 0)) {}

  SecureBytes &operator=(SecureBytes &&other) noexcept {
    if (this != &other) {
      Reset();
      data_ = std::move(other.data_);
      size_ = std::exchange(other.size_, 0);
    }
    return *this;
  }

  std::uint8_t *data() noexcept { return data_.get(); }
  const std::uint8_t *data() const noexcept { return data_.get(); }
  std::size_t size() const noexcept { return size_; }

  void CopyAt(std::size_t offset, const std::uint8_t *source,
              std::size_t length) {
    if (offset > size_ || length > size_ - offset) {
      throw std::out_of_range("Secure byte write exceeds allocation.");
    }
    if (length > 0) {
      std::memcpy(data_.get() + offset, source, length);
    }
  }

  void Reset() noexcept {
    if (data_ != nullptr) {
      SecureZero(data_.get(), size_);
      data_.reset();
      size_ = 0;
    }
  }

private:
  std::unique_ptr<std::uint8_t[]> data_;
  std::size_t size_ = 0;
};

struct EncryptedFile {
  SecureBytes file_iv;
  SecureBytes file_ciphertext;
  SecureBytes meta_iv;
  SecureBytes meta_ciphertext;

  std::size_t ByteSize() const noexcept {
    return file_iv.size() + file_ciphertext.size() + meta_iv.size() +
           meta_ciphertext.size();
  }
};

struct UploadRecord {
  std::vector<EncryptedFile> files;
  std::int64_t created_at = 0;
  std::int64_t expires_at = 0;
  std::size_t bytes = 0;
};

struct EncryptedChunk {
  SecureBytes iv;
  SecureBytes ciphertext;
  std::size_t written = 0;
  bool receiving = false;
  bool complete = false;
};

struct ChunkedFile {
  std::string id;
  std::uint64_t size = 0;
  std::uint32_t chunk_count = 0;
  SecureBytes meta_iv;
  SecureBytes meta_ciphertext;
  std::vector<EncryptedChunk> chunks;
};

enum class ChunkedSessionState {
  Uploading,
  Ready,
  Downloading,
};

struct ChunkedSession {
  std::string lookup_key;
  std::int64_t expires_at = 0;
  ChunkedSessionState state = ChunkedSessionState::Uploading;
  std::string download_token;
  std::size_t active_leases = 0;
  bool pending_delete = false;
  std::vector<ChunkedFile> files;
};

struct PendingJavaScriptException final {};

class NapiFailure final : public std::runtime_error {
public:
  explicit NapiFailure(const std::string &message)
      : std::runtime_error(message) {}
};

void CheckNapi(napi_env env, napi_status status) {
  if (status == napi_ok) {
    return;
  }
  if (status == napi_pending_exception) {
    throw PendingJavaScriptException{};
  }

  const napi_extended_error_info *info = nullptr;
  napi_get_last_error_info(env, &info);
  const char *message = info != nullptr && info->error_message != nullptr
                            ? info->error_message
                            : "Native N-API operation failed.";
  throw NapiFailure(message);
}

[[noreturn]] void ThrowTypeError(napi_env env, const char *message) {
  napi_throw_type_error(env, nullptr, message);
  throw PendingJavaScriptException{};
}

std::int64_t NowMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string MakeSecret() {
  static constexpr char hexadecimal[] = "0123456789abcdef";
  std::random_device random;
  std::array<std::uint8_t, 32> bytes{};
  for (auto &byte : bytes) {
    byte = static_cast<std::uint8_t>(random());
  }
  std::string result;
  result.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    result.push_back(hexadecimal[(byte >> 4U) & 0x0fU]);
    result.push_back(hexadecimal[byte & 0x0fU]);
  }
  SecureZero(bytes.data(), bytes.size());
  return result;
}

bool SafeAdd(std::size_t left, std::size_t right,
             std::size_t *result) noexcept {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    return false;
  }
  *result = left + right;
  return true;
}

std::uint64_t ReadPositiveInteger(napi_env env, napi_value value,
                                  const char *name) {
  napi_valuetype type;
  CheckNapi(env, napi_typeof(env, value, &type));
  if (type != napi_number) {
    const std::string message = std::string(name) + " must be a number.";
    ThrowTypeError(env, message.c_str());
  }

  double number = 0;
  CheckNapi(env, napi_get_value_double(env, value, &number));
  if (!std::isfinite(number) || number <= 0 || std::floor(number) != number ||
      number > kMaxSafeInteger) {
    const std::string message =
        std::string(name) + " must be a positive safe integer.";
    ThrowTypeError(env, message.c_str());
  }
  return static_cast<std::uint64_t>(number);
}

std::uint64_t ReadNonNegativeInteger(napi_env env, napi_value value,
                                     const char *name) {
  napi_valuetype type;
  CheckNapi(env, napi_typeof(env, value, &type));
  if (type != napi_number) {
    const std::string message = std::string(name) + " must be a number.";
    ThrowTypeError(env, message.c_str());
  }

  double number = 0;
  CheckNapi(env, napi_get_value_double(env, value, &number));
  if (!std::isfinite(number) || number < 0 || std::floor(number) != number ||
      number > kMaxSafeInteger) {
    const std::string message =
        std::string(name) + " must be a non-negative safe integer.";
    ThrowTypeError(env, message.c_str());
  }
  return static_cast<std::uint64_t>(number);
}

std::string ReadString(napi_env env, napi_value value, const char *name) {
  napi_valuetype type;
  CheckNapi(env, napi_typeof(env, value, &type));
  if (type != napi_string) {
    const std::string message = std::string(name) + " must be a string.";
    ThrowTypeError(env, message.c_str());
  }

  std::size_t length = 0;
  CheckNapi(env, napi_get_value_string_utf8(env, value, nullptr, 0, &length));
  std::string result(length + 1, '\0');
  std::size_t written = 0;
  CheckNapi(env, napi_get_value_string_utf8(env, value, result.data(),
                                            length + 1, &written));
  result.resize(written);
  return result;
}

struct ByteView {
  const std::uint8_t *data = nullptr;
  std::size_t size = 0;
};

ByteView ReadByteView(napi_env env, napi_value value, const char *name) {
  bool is_buffer = false;
  CheckNapi(env, napi_is_buffer(env, value, &is_buffer));
  if (is_buffer) {
    void *data = nullptr;
    std::size_t size = 0;
    CheckNapi(env, napi_get_buffer_info(env, value, &data, &size));
    return {static_cast<const std::uint8_t *>(data), size};
  }

  bool is_typed_array = false;
  CheckNapi(env, napi_is_typedarray(env, value, &is_typed_array));
  if (is_typed_array) {
    napi_typedarray_type array_type;
    std::size_t length = 0;
    void *data = nullptr;
    napi_value array_buffer;
    std::size_t byte_offset = 0;
    CheckNapi(env, napi_get_typedarray_info(
                       env, value, &array_type, &length, &data, &array_buffer,
                       &byte_offset));
    if (array_type != napi_uint8_array &&
        array_type != napi_uint8_clamped_array) {
      const std::string message =
          std::string(name) + " must be an unsigned byte array.";
      ThrowTypeError(env, message.c_str());
    }
    return {static_cast<const std::uint8_t *>(data), length};
  }

  const std::string message =
      std::string(name) + " must be an unsigned byte array.";
  ThrowTypeError(env, message.c_str());
}

int Base64Value(unsigned char character) noexcept {
  if (character >= 'A' && character <= 'Z') {
    return character - 'A';
  }
  if (character >= 'a' && character <= 'z') {
    return character - 'a' + 26;
  }
  if (character >= '0' && character <= '9') {
    return character - '0' + 52;
  }
  if (character == '+') {
    return 62;
  }
  if (character == '/') {
    return 63;
  }
  return -1;
}

bool DecodeBase64(const std::uint8_t *encoded, std::size_t encoded_size,
                  SecureBytes *output) {
  if (encoded_size == 0) {
    return false;
  }

  std::size_t padding = 0;
  while (padding < encoded_size && encoded[encoded_size - 1 - padding] == '=') {
    ++padding;
  }
  if (padding > 2 || (padding > 0 && encoded_size % 4 != 0)) {
    return false;
  }

  const std::size_t content_size = encoded_size - padding;
  if (content_size % 4 == 1) {
    return false;
  }
  for (std::size_t index = 0; index < content_size; ++index) {
    if (Base64Value(encoded[index]) < 0) {
      return false;
    }
  }
  for (std::size_t index = content_size; index < encoded_size; ++index) {
    if (encoded[index] != '=') {
      return false;
    }
  }

  if (content_size > std::numeric_limits<std::size_t>::max() / 6) {
    return false;
  }
  const std::size_t decoded_size = (content_size * 6) / 8;
  SecureBytes decoded(decoded_size);
  std::uint32_t accumulator = 0;
  int bits = 0;
  std::size_t output_index = 0;

  for (std::size_t index = 0; index < content_size; ++index) {
    accumulator = (accumulator << 6) |
                  static_cast<std::uint32_t>(Base64Value(encoded[index]));
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      decoded.data()[output_index++] =
          static_cast<std::uint8_t>((accumulator >> bits) & 0xffU);
    }
  }

  if (output_index != decoded_size) {
    return false;
  }
  *output = std::move(decoded);
  return true;
}

SecureBytes ReadBase64Property(napi_env env, napi_value object,
                               const char *property_name) {
  napi_value value;
  CheckNapi(env, napi_get_named_property(env, object, property_name, &value));

  napi_valuetype type;
  CheckNapi(env, napi_typeof(env, value, &type));
  if (type != napi_string) {
    const std::string message = std::string(property_name) + " must be base64.";
    ThrowTypeError(env, message.c_str());
  }

  std::size_t encoded_length = 0;
  CheckNapi(
      env, napi_get_value_string_utf8(env, value, nullptr, 0, &encoded_length));
  SecureBytes encoded(encoded_length + 1);
  std::size_t written = 0;
  CheckNapi(env, napi_get_value_string_utf8(
                     env, value, reinterpret_cast<char *>(encoded.data()),
                     encoded.size(), &written));

  SecureBytes decoded;
  if (written != encoded_length ||
      !DecodeBase64(encoded.data(), written, &decoded)) {
    const std::string message =
        std::string(property_name) + " must be valid base64.";
    ThrowTypeError(env, message.c_str());
  }
  return decoded;
}

SecureBytes ReadBase64Value(napi_env env, napi_value value,
                            const char *name) {
  const std::string encoded = ReadString(env, value, name);
  SecureBytes decoded;
  if (!DecodeBase64(
          reinterpret_cast<const std::uint8_t *>(encoded.data()),
          encoded.size(), &decoded)) {
    const std::string message = std::string(name) + " must be valid base64.";
    ThrowTypeError(env, message.c_str());
  }
  return decoded;
}

EncryptedFile ReadEncryptedFile(napi_env env, napi_value value,
                                std::uint32_t index) {
  napi_valuetype type;
  CheckNapi(env, napi_typeof(env, value, &type));
  if (type != napi_object) {
    const std::string message =
        "files[" + std::to_string(index) + "] must be an object.";
    ThrowTypeError(env, message.c_str());
  }

  EncryptedFile file;
  file.file_iv = ReadBase64Property(env, value, "fileIv");
  file.file_ciphertext = ReadBase64Property(env, value, "fileCiphertext");
  file.meta_iv = ReadBase64Property(env, value, "metaIv");
  file.meta_ciphertext = ReadBase64Property(env, value, "metaCiphertext");
  return file;
}

std::vector<EncryptedFile> ReadFiles(napi_env env, napi_value value,
                                     std::size_t *byte_size) {
  bool is_array = false;
  CheckNapi(env, napi_is_array(env, value, &is_array));
  if (!is_array) {
    ThrowTypeError(env, "files must be an array.");
  }

  std::uint32_t length = 0;
  CheckNapi(env, napi_get_array_length(env, value, &length));
  if (length == 0) {
    ThrowTypeError(env, "files must include at least one encrypted file.");
  }

  std::vector<EncryptedFile> files;
  files.reserve(length);
  std::size_t total = 0;
  for (std::uint32_t index = 0; index < length; ++index) {
    napi_value item;
    CheckNapi(env, napi_get_element(env, value, index, &item));
    EncryptedFile file = ReadEncryptedFile(env, item, index);
    std::size_t next_total = 0;
    if (!SafeAdd(total, file.ByteSize(), &next_total)) {
      ThrowTypeError(env, "Encrypted payload is too large.");
    }
    total = next_total;
    files.push_back(std::move(file));
  }

  *byte_size = total;
  return files;
}

std::vector<ChunkedFile> ReadChunkedFiles(napi_env env, napi_value value) {
  bool is_array = false;
  CheckNapi(env, napi_is_array(env, value, &is_array));
  if (!is_array) {
    ThrowTypeError(env, "files must be an array.");
  }

  std::uint32_t length = 0;
  CheckNapi(env, napi_get_array_length(env, value, &length));
  if (length == 0 || length > 100) {
    ThrowTypeError(env, "files must contain between 1 and 100 entries.");
  }

  std::vector<ChunkedFile> files;
  files.reserve(length);
  for (std::uint32_t index = 0; index < length; ++index) {
    napi_value item;
    CheckNapi(env, napi_get_element(env, value, index, &item));

    napi_value id_value;
    napi_value size_value;
    napi_value chunk_count_value;
    CheckNapi(env, napi_get_named_property(env, item, "id", &id_value));
    CheckNapi(env, napi_get_named_property(env, item, "size", &size_value));
    CheckNapi(env,
              napi_get_named_property(env, item, "chunkCount",
                                      &chunk_count_value));

    ChunkedFile file;
    file.id = ReadString(env, id_value, "file id");
    file.size = ReadNonNegativeInteger(env, size_value, "file size");
    const auto chunk_count =
        ReadPositiveInteger(env, chunk_count_value, "chunk count");
    const std::uint64_t expected_chunks =
        file.size == 0 ? 1 : ((file.size - 1) / kChunkSizeBytes) + 1;
    if (chunk_count != expected_chunks ||
        chunk_count > std::numeric_limits<std::uint32_t>::max()) {
      ThrowTypeError(env, "chunk count does not match file size.");
    }
    if (file.id.empty() || file.id.size() > 64) {
      ThrowTypeError(env, "file id has an invalid length.");
    }
    for (const auto &existing : files) {
      if (existing.id == file.id) {
        ThrowTypeError(env, "file ids must be unique.");
      }
    }

    file.chunk_count = static_cast<std::uint32_t>(chunk_count);
    file.meta_iv = ReadBase64Property(env, item, "metaIv");
    file.meta_ciphertext =
        ReadBase64Property(env, item, "metaCiphertext");
    file.chunks.resize(file.chunk_count);
    files.push_back(std::move(file));
  }
  return files;
}

napi_value MakeNumber(napi_env env, double value) {
  napi_value result;
  CheckNapi(env, napi_create_double(env, value, &result));
  return result;
}

napi_value MakeBoolean(napi_env env, bool value) {
  napi_value result;
  CheckNapi(env, napi_get_boolean(env, value, &result));
  return result;
}

napi_value MakeUndefined(napi_env env) {
  napi_value result;
  CheckNapi(env, napi_get_undefined(env, &result));
  return result;
}

napi_value MakeString(napi_env env, const char *value) {
  napi_value result;
  CheckNapi(env,
            napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &result));
  return result;
}

napi_value MakeString(napi_env env, const std::string &value) {
  napi_value result;
  CheckNapi(env,
            napi_create_string_utf8(env, value.data(), value.size(), &result));
  return result;
}

void SetProperty(napi_env env, napi_value object, const char *name,
                 napi_value value) {
  CheckNapi(env, napi_set_named_property(env, object, name, value));
}

napi_value MakeErrorPayload(napi_env env, const char *message) {
  napi_value payload;
  CheckNapi(env, napi_create_object(env, &payload));
  SetProperty(env, payload, "error", MakeString(env, message));
  return payload;
}

napi_value MakeResult(napi_env env, int status, napi_value payload) {
  napi_value result;
  CheckNapi(env, napi_create_object(env, &result));
  SetProperty(env, result, "status", MakeNumber(env, status));
  SetProperty(env, result, "payload", payload);
  return result;
}

napi_value EncodeBase64(napi_env env, const SecureBytes &input) {
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const std::size_t encoded_size = ((input.size() + 2) / 3) * 4;
  SecureBytes encoded(encoded_size);

  std::size_t input_index = 0;
  std::size_t output_index = 0;
  while (input_index + 3 <= input.size()) {
    const std::uint32_t block =
        (static_cast<std::uint32_t>(input.data()[input_index]) << 16) |
        (static_cast<std::uint32_t>(input.data()[input_index + 1]) << 8) |
        static_cast<std::uint32_t>(input.data()[input_index + 2]);
    encoded.data()[output_index++] = alphabet[(block >> 18) & 0x3fU];
    encoded.data()[output_index++] = alphabet[(block >> 12) & 0x3fU];
    encoded.data()[output_index++] = alphabet[(block >> 6) & 0x3fU];
    encoded.data()[output_index++] = alphabet[block & 0x3fU];
    input_index += 3;
  }

  const std::size_t remaining = input.size() - input_index;
  if (remaining == 1) {
    const std::uint32_t block =
        static_cast<std::uint32_t>(input.data()[input_index]) << 16;
    encoded.data()[output_index++] = alphabet[(block >> 18) & 0x3fU];
    encoded.data()[output_index++] = alphabet[(block >> 12) & 0x3fU];
    encoded.data()[output_index++] = '=';
    encoded.data()[output_index++] = '=';
  } else if (remaining == 2) {
    const std::uint32_t block =
        (static_cast<std::uint32_t>(input.data()[input_index]) << 16) |
        (static_cast<std::uint32_t>(input.data()[input_index + 1]) << 8);
    encoded.data()[output_index++] = alphabet[(block >> 18) & 0x3fU];
    encoded.data()[output_index++] = alphabet[(block >> 12) & 0x3fU];
    encoded.data()[output_index++] = alphabet[(block >> 6) & 0x3fU];
    encoded.data()[output_index++] = '=';
  }

  napi_value result;
  CheckNapi(env, napi_create_string_utf8(
                     env, reinterpret_cast<const char *>(encoded.data()),
                     encoded_size, &result));
  return result;
}

napi_value MakeFilePayload(napi_env env, const EncryptedFile &file) {
  napi_value result;
  CheckNapi(env, napi_create_object(env, &result));
  SetProperty(env, result, "fileIv", EncodeBase64(env, file.file_iv));
  SetProperty(env, result, "fileCiphertext",
              EncodeBase64(env, file.file_ciphertext));
  SetProperty(env, result, "metaIv", EncodeBase64(env, file.meta_iv));
  SetProperty(env, result, "metaCiphertext",
              EncodeBase64(env, file.meta_ciphertext));
  return result;
}

void NoopExternalBufferFinalizer(napi_env, void *, void *) {}

napi_value MakeOkPayload(napi_env env) {
  napi_value payload;
  CheckNapi(env, napi_create_object(env, &payload));
  SetProperty(env, payload, "ok", MakeBoolean(env, true));
  return payload;
}

class UploadStore {
public:
  explicit UploadStore(std::int64_t ttl_ms) : ttl_ms_(ttl_ms) {}

  std::size_t PurgeExpired(std::int64_t now) {
    std::size_t purged = 0;
    for (auto iterator = uploads_.begin(); iterator != uploads_.end();) {
      if (iterator->second.expires_at <= now) {
        iterator = uploads_.erase(iterator);
        ++purged;
      } else {
        ++iterator;
      }
    }
    for (auto iterator = chunked_uploads_.begin();
         iterator != chunked_uploads_.end();) {
      if (iterator->second.pending_delete) {
        ++iterator;
        continue;
      }
      if (iterator->second.expires_at > now) {
        ++iterator;
        continue;
      }
      if (iterator->second.active_leases > 0) {
        ++iterator;
        continue;
      }
      chunked_lookup_.erase(iterator->second.lookup_key);
      iterator = chunked_uploads_.erase(iterator);
      ++purged;
    }
    if (purged > 0) {
      ReleaseUnusedMemory();
    }
    return purged;
  }

  napi_value Upsert(napi_env env, std::string key,
                    std::vector<EncryptedFile> files,
                    std::size_t incoming_bytes) {
    const std::int64_t now = NowMilliseconds();
    PurgeExpired(now);

    auto existing = uploads_.find(key);
    if (existing != uploads_.end()) {
      auto &record = existing->second;
      record.files.reserve(record.files.size() + files.size());
      for (auto &file : files) {
        record.files.push_back(std::move(file));
      }
      record.bytes += incoming_bytes;

      napi_value payload;
      CheckNapi(env, napi_create_object(env, &payload));
      SetProperty(env, payload, "ok", MakeBoolean(env, true));
      SetProperty(env, payload, "expiresAt",
                  MakeNumber(env, static_cast<double>(record.expires_at)));
      SetProperty(env, payload, "files",
                  MakeNumber(env, static_cast<double>(record.files.size())));
      return MakeResult(env, 200, payload);
    }

    const std::int64_t expires_at =
        now > std::numeric_limits<std::int64_t>::max() - ttl_ms_
            ? std::numeric_limits<std::int64_t>::max()
            : now + ttl_ms_;
    const std::size_t incoming_file_count = files.size();
    UploadRecord record{
        std::move(files),
        now,
        expires_at,
        incoming_bytes,
    };
    uploads_.emplace(std::move(key), std::move(record));

    napi_value payload;
    CheckNapi(env, napi_create_object(env, &payload));
    SetProperty(env, payload, "ok", MakeBoolean(env, true));
    SetProperty(env, payload, "expiresAt",
                MakeNumber(env, static_cast<double>(expires_at)));
    SetProperty(env, payload, "files",
                MakeNumber(env, static_cast<double>(incoming_file_count)));
    return MakeResult(env, 201, payload);
  }

  napi_value Take(napi_env env, const std::string &key) {
    auto iterator = uploads_.find(key);
    if (iterator == uploads_.end()) {
      return MakeResult(env, 404, MakeErrorPayload(env, kNotFoundMessage));
    }

    auto record_node = uploads_.extract(iterator);
    UploadRecord &record = record_node.mapped();

    if (record.expires_at <= NowMilliseconds()) {
      record.files.clear();
      ReleaseUnusedMemory();
      return MakeResult(env, 410, MakeErrorPayload(env, kExpiredMessage));
    }

    napi_value files;
    CheckNapi(env,
              napi_create_array_with_length(env, record.files.size(), &files));
    for (std::size_t index = 0; index < record.files.size(); ++index) {
      CheckNapi(env,
                napi_set_element(env, files, static_cast<std::uint32_t>(index),
                                 MakeFilePayload(env, record.files[index])));
    }

    napi_value payload;
    CheckNapi(env, napi_create_object(env, &payload));
    SetProperty(env, payload, "files", files);
    napi_value result = MakeResult(env, 200, payload);
    record.files.clear();
    ReleaseUnusedMemory();
    return result;
  }

  std::size_t Clear() {
    const std::size_t cleared = uploads_.size() + chunked_uploads_.size();
    uploads_.clear();
    chunked_lookup_.clear();
    for (auto iterator = chunked_uploads_.begin();
         iterator != chunked_uploads_.end();) {
      if (iterator->second.active_leases > 0) {
        iterator->second.pending_delete = true;
        ++iterator;
      } else {
        iterator = chunked_uploads_.erase(iterator);
      }
    }
    ReleaseUnusedMemory();
    return cleared;
  }

  napi_value Stats(napi_env env) {
    PurgeExpired(NowMilliseconds());
    std::size_t file_count = 0;
    std::size_t encrypted_bytes = 0;
    for (const auto &entry : uploads_) {
      file_count += entry.second.files.size();
      encrypted_bytes += entry.second.bytes;
    }
    for (const auto &entry : chunked_uploads_) {
      file_count += entry.second.files.size();
      for (const auto &file : entry.second.files) {
        encrypted_bytes += file.meta_iv.size() + file.meta_ciphertext.size();
        for (const auto &chunk : file.chunks) {
          encrypted_bytes += chunk.iv.size() + chunk.ciphertext.size();
        }
      }
    }

    napi_value stats;
    CheckNapi(env, napi_create_object(env, &stats));
    SetProperty(env, stats, "uploadCount",
                MakeNumber(env, static_cast<double>(
                                    uploads_.size() + chunked_uploads_.size())));
    SetProperty(env, stats, "fileCount",
                MakeNumber(env, static_cast<double>(file_count)));
    SetProperty(env, stats, "encryptedBytes",
                MakeNumber(env, static_cast<double>(encrypted_bytes)));
    return stats;
  }

  napi_value CreateChunkedUpload(napi_env env, std::string lookup_key,
                                 std::vector<ChunkedFile> files) {
    PurgeExpired(NowMilliseconds());
    if (uploads_.find(lookup_key) != uploads_.end() ||
        chunked_lookup_.find(lookup_key) != chunked_lookup_.end()) {
      return MakeResult(
          env, 409,
          MakeErrorPayload(env, "An upload already exists for this code."));
    }

    std::string upload_id;
    do {
      upload_id = MakeSecret();
    } while (chunked_uploads_.find(upload_id) != chunked_uploads_.end());

    ChunkedSession session;
    session.lookup_key = std::move(lookup_key);
    session.expires_at = ExpiryFromNow();
    session.files = std::move(files);
    const std::string lookup_copy = session.lookup_key;
    const std::int64_t expires_at = session.expires_at;
    chunked_uploads_.emplace(upload_id, std::move(session));
    chunked_lookup_.emplace(lookup_copy, upload_id);

    napi_value payload;
    CheckNapi(env, napi_create_object(env, &payload));
    SetProperty(env, payload, "uploadId", MakeString(env, upload_id));
    SetProperty(env, payload, "expiresAt",
                MakeNumber(env, static_cast<double>(expires_at)));
    SetProperty(env, payload, "chunkSize",
                MakeNumber(env, static_cast<double>(kChunkSizeBytes)));
    return MakeResult(env, 201, payload);
  }

  napi_value BeginChunk(napi_env env, const std::string &upload_id,
                        const std::string &file_id, std::uint32_t index,
                        SecureBytes iv, std::size_t content_length) {
    ChunkedSession *session = FindChunkedSession(upload_id);
    if (session == nullptr || session->expires_at <= NowMilliseconds()) {
      if (session != nullptr) {
        EraseChunkedUpload(upload_id);
      }
      return MakeResult(
          env, 404,
          MakeErrorPayload(env, "Upload session not found or expired."));
    }
    if (session->state != ChunkedSessionState::Uploading ||
        session->pending_delete) {
      return MakeResult(
          env, 409,
          MakeErrorPayload(env, "Upload session is no longer writable."));
    }
    ChunkedFile *file = FindChunkedFile(*session, file_id);
    if (file == nullptr || index >= file->chunk_count) {
      return MakeResult(env, 400,
                        MakeErrorPayload(env, "Invalid file chunk."));
    }
    EncryptedChunk &chunk = file->chunks[index];
    if (chunk.complete || chunk.receiving) {
      return MakeResult(
          env, 409,
          MakeErrorPayload(env, "This chunk was already uploaded."));
    }
    if (iv.size() != 12 ||
        content_length != ExpectedChunkBytes(*file, index)) {
      return MakeResult(
          env, 400,
          MakeErrorPayload(env, "Encrypted chunk size or IV is invalid."));
    }

    chunk.iv = std::move(iv);
    chunk.ciphertext = SecureBytes(content_length);
    chunk.written = 0;
    chunk.receiving = true;
    Touch(*session);
    return MakeResult(env, 200, MakeOkPayload(env));
  }

  void AppendChunkPart(const std::string &upload_id,
                       const std::string &file_id, std::uint32_t index,
                       const ByteView &bytes) {
    ChunkedSession *session = FindChunkedSession(upload_id);
    ChunkedFile *file =
        session == nullptr ? nullptr : FindChunkedFile(*session, file_id);
    if (session == nullptr || file == nullptr || index >= file->chunk_count) {
      throw std::runtime_error("Upload session or file chunk was not found.");
    }
    EncryptedChunk &chunk = file->chunks[index];
    if (!chunk.receiving || chunk.complete ||
        bytes.size > chunk.ciphertext.size() - chunk.written) {
      throw std::runtime_error("Encrypted chunk stream is invalid.");
    }
    chunk.ciphertext.CopyAt(chunk.written, bytes.data, bytes.size);
    chunk.written += bytes.size;
    Touch(*session);
  }

  napi_value FinishChunk(napi_env env, const std::string &upload_id,
                         const std::string &file_id, std::uint32_t index) {
    ChunkedSession *session = FindChunkedSession(upload_id);
    ChunkedFile *file =
        session == nullptr ? nullptr : FindChunkedFile(*session, file_id);
    if (session == nullptr || file == nullptr || index >= file->chunk_count) {
      return MakeResult(
          env, 404,
          MakeErrorPayload(env, "Upload session not found or expired."));
    }
    EncryptedChunk &chunk = file->chunks[index];
    if (!chunk.receiving || chunk.written != chunk.ciphertext.size()) {
      return MakeResult(
          env, 400,
          MakeErrorPayload(env, "Encrypted chunk ended before completion."));
    }
    chunk.receiving = false;
    chunk.complete = true;
    Touch(*session);
    return MakeResult(env, 200, MakeOkPayload(env));
  }

  void FailChunk(const std::string &upload_id, const std::string &file_id,
                 std::uint32_t index) {
    ChunkedSession *session = FindChunkedSession(upload_id);
    ChunkedFile *file =
        session == nullptr ? nullptr : FindChunkedFile(*session, file_id);
    if (session == nullptr || file == nullptr || index >= file->chunk_count) {
      return;
    }
    EncryptedChunk &chunk = file->chunks[index];
    chunk.iv.Reset();
    chunk.ciphertext.Reset();
    chunk.written = 0;
    chunk.receiving = false;
    chunk.complete = false;
    Touch(*session);
    ReleaseUnusedMemory();
  }

  napi_value CompleteChunkedUpload(napi_env env,
                                   const std::string &upload_id) {
    ChunkedSession *session = FindChunkedSession(upload_id);
    if (session == nullptr || session->expires_at <= NowMilliseconds()) {
      if (session != nullptr) {
        EraseChunkedUpload(upload_id);
      }
      return MakeResult(
          env, 404,
          MakeErrorPayload(env, "Upload session not found or expired."));
    }
    if (session->state != ChunkedSessionState::Uploading) {
      return MakeResult(env, 409,
                        MakeErrorPayload(env, "Upload was already completed."));
    }
    for (const auto &file : session->files) {
      for (const auto &chunk : file.chunks) {
        if (!chunk.complete || chunk.receiving) {
          return MakeResult(
              env, 409,
              MakeErrorPayload(env, "Not all file chunks were uploaded."));
        }
      }
    }
    session->state = ChunkedSessionState::Ready;
    Touch(*session);
    napi_value payload = MakeOkPayload(env);
    SetProperty(env, payload, "expiresAt",
                MakeNumber(env, static_cast<double>(session->expires_at)));
    return MakeResult(env, 200, payload);
  }

  napi_value BeginChunkedDownload(napi_env env,
                                  const std::string &lookup_key) {
    PurgeExpired(NowMilliseconds());
    const auto lookup = chunked_lookup_.find(lookup_key);
    if (lookup == chunked_lookup_.end()) {
      return MakeResult(
          env, 404,
          MakeErrorPayload(
              env, "Upload not found, expired, or already claimed."));
    }
    ChunkedSession *session = FindChunkedSession(lookup->second);
    if (session == nullptr || session->pending_delete ||
        (session->state != ChunkedSessionState::Ready &&
         session->state != ChunkedSessionState::Downloading)) {
      return MakeResult(
          env, 404,
          MakeErrorPayload(
              env, "Upload not found, expired, or already claimed."));
    }
    if (session->state == ChunkedSessionState::Ready) {
      session->state = ChunkedSessionState::Downloading;
      session->download_token = MakeSecret();
    }
    Touch(*session);

    napi_value files;
    CheckNapi(env,
              napi_create_array_with_length(env, session->files.size(), &files));
    for (std::size_t index = 0; index < session->files.size(); ++index) {
      const ChunkedFile &file = session->files[index];
      napi_value item;
      CheckNapi(env, napi_create_object(env, &item));
      SetProperty(env, item, "id", MakeString(env, file.id));
      SetProperty(env, item, "size",
                  MakeNumber(env, static_cast<double>(file.size)));
      SetProperty(env, item, "chunkCount",
                  MakeNumber(env, static_cast<double>(file.chunk_count)));
      SetProperty(env, item, "metaIv", EncodeBase64(env, file.meta_iv));
      SetProperty(env, item, "metaCiphertext",
                  EncodeBase64(env, file.meta_ciphertext));
      CheckNapi(env, napi_set_element(env, files,
                                      static_cast<std::uint32_t>(index), item));
    }

    napi_value payload;
    CheckNapi(env, napi_create_object(env, &payload));
    SetProperty(env, payload, "downloadId", MakeString(env, lookup->second));
    SetProperty(env, payload, "downloadToken",
                MakeString(env, session->download_token));
    SetProperty(env, payload, "files", files);
    return MakeResult(env, 200, payload);
  }

  napi_value AcquireChunkedDownloadChunk(
      napi_env env, const std::string &download_id, const std::string &token,
      const std::string &file_id, std::uint32_t index) {
    ChunkedSession *session = FindChunkedSession(download_id);
    if (session == nullptr || session->expires_at <= NowMilliseconds() ||
        session->pending_delete ||
        session->state != ChunkedSessionState::Downloading ||
        session->download_token != token) {
      return MakeResult(
          env, 404,
          MakeErrorPayload(env, "Download session not found or expired."));
    }
    ChunkedFile *file = FindChunkedFile(*session, file_id);
    if (file == nullptr || index >= file->chunk_count ||
        !file->chunks[index].complete) {
      return MakeResult(env, 404,
                        MakeErrorPayload(env, "File chunk not found."));
    }
    EncryptedChunk &chunk = file->chunks[index];
    napi_value bytes;
    CheckNapi(env,
              napi_create_external_buffer(
                  env, chunk.ciphertext.size(),
                  reinterpret_cast<char *>(chunk.ciphertext.data()),
                  NoopExternalBufferFinalizer, nullptr, &bytes));
    session->active_leases += 1;
    Touch(*session);

    napi_value payload;
    CheckNapi(env, napi_create_object(env, &payload));
    SetProperty(env, payload, "iv", EncodeBase64(env, chunk.iv));
    SetProperty(env, payload, "bytes", bytes);
    return MakeResult(env, 200, payload);
  }

  void ReleaseChunkedDownloadChunk(const std::string &download_id,
                                   const std::string &token) {
    auto iterator = chunked_uploads_.find(download_id);
    if (iterator == chunked_uploads_.end() ||
        iterator->second.download_token != token ||
        iterator->second.active_leases == 0) {
      return;
    }
    iterator->second.active_leases -= 1;
    if (iterator->second.active_leases == 0) {
      if (iterator->second.pending_delete) {
        chunked_lookup_.erase(iterator->second.lookup_key);
        chunked_uploads_.erase(iterator);
        ReleaseUnusedMemory();
      } else {
        Touch(iterator->second);
      }
    }
  }

  napi_value FinishChunkedDownload(napi_env env,
                                   const std::string &download_id,
                                   const std::string &token) {
    auto iterator = chunked_uploads_.find(download_id);
    if (iterator == chunked_uploads_.end() ||
        iterator->second.state != ChunkedSessionState::Downloading ||
        iterator->second.download_token != token) {
      return MakeResult(
          env, 404,
          MakeErrorPayload(env, "Download session not found or expired."));
    }
    chunked_lookup_.erase(iterator->second.lookup_key);
    if (iterator->second.active_leases > 0) {
      iterator->second.pending_delete = true;
    } else {
      chunked_uploads_.erase(iterator);
      ReleaseUnusedMemory();
    }
    return MakeResult(env, 200, MakeOkPayload(env));
  }

  napi_value AbortChunkedUpload(napi_env env, const std::string &upload_id) {
    auto iterator = chunked_uploads_.find(upload_id);
    if (iterator == chunked_uploads_.end() ||
        iterator->second.state != ChunkedSessionState::Uploading) {
      return MakeResult(env, 404,
                        MakeErrorPayload(env, "Upload session not found."));
    }
    chunked_lookup_.erase(iterator->second.lookup_key);
    chunked_uploads_.erase(iterator);
    ReleaseUnusedMemory();
    return MakeResult(env, 200, MakeOkPayload(env));
  }

private:
  std::int64_t ExpiryFromNow() const noexcept {
    const std::int64_t now = NowMilliseconds();
    return now > std::numeric_limits<std::int64_t>::max() - ttl_ms_
               ? std::numeric_limits<std::int64_t>::max()
               : now + ttl_ms_;
  }

  void Touch(ChunkedSession &session) const noexcept {
    session.expires_at = ExpiryFromNow();
  }

  static std::size_t ExpectedChunkBytes(const ChunkedFile &file,
                                        std::uint32_t index) {
    const std::uint64_t offset =
        static_cast<std::uint64_t>(index) * kChunkSizeBytes;
    const std::uint64_t plaintext =
        std::min<std::uint64_t>(kChunkSizeBytes, file.size - offset);
    return static_cast<std::size_t>(plaintext) + kGcmTagBytes;
  }

  ChunkedSession *FindChunkedSession(const std::string &id) {
    const auto iterator = chunked_uploads_.find(id);
    return iterator == chunked_uploads_.end() ? nullptr : &iterator->second;
  }

  static ChunkedFile *FindChunkedFile(ChunkedSession &session,
                                      const std::string &id) {
    for (auto &file : session.files) {
      if (file.id == id) {
        return &file;
      }
    }
    return nullptr;
  }

  void EraseChunkedUpload(const std::string &id) {
    const auto iterator = chunked_uploads_.find(id);
    if (iterator == chunked_uploads_.end()) {
      return;
    }
    chunked_lookup_.erase(iterator->second.lookup_key);
    if (iterator->second.active_leases > 0) {
      iterator->second.pending_delete = true;
      return;
    }
    chunked_uploads_.erase(iterator);
    ReleaseUnusedMemory();
  }

  std::unordered_map<std::string, UploadRecord> uploads_;
  std::unordered_map<std::string, ChunkedSession> chunked_uploads_;
  std::unordered_map<std::string, std::string> chunked_lookup_;
  std::int64_t ttl_ms_;
};

template <typename Callback>
napi_value RunCallback(napi_env env, Callback &&callback) {
  try {
    return callback();
  } catch (const PendingJavaScriptException &) {
    return nullptr;
  } catch (const std::bad_alloc &) {
    napi_throw_error(env, nullptr, "Native encrypted store is out of memory.");
    return nullptr;
  } catch (const std::exception &error) {
    napi_throw_error(env, nullptr, error.what());
    return nullptr;
  } catch (...) {
    napi_throw_error(env, nullptr, "Unknown native core error.");
    return nullptr;
  }
}

UploadStore *GetStore(napi_env env, napi_callback_info info, std::size_t *argc,
                      napi_value *arguments) {
  void *data = nullptr;
  CheckNapi(env, napi_get_cb_info(env, info, argc, arguments, nullptr, &data));
  if (data == nullptr) {
    throw NapiFailure("Native encrypted store is unavailable.");
  }
  return static_cast<UploadStore *>(data);
}

napi_value UpsertCallback(napi_env env, napi_callback_info info) {
  return RunCallback(env, [&]() -> napi_value {
    std::size_t argc = 2;
    napi_value arguments[2];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 2) {
      ThrowTypeError(env, "upsertUpload requires lookupKey and files.");
    }

    std::string key = ReadString(env, arguments[0], "lookupKey");
    std::size_t incoming_bytes = 0;
    std::vector<EncryptedFile> files =
        ReadFiles(env, arguments[1], &incoming_bytes);
    return store->Upsert(env, std::move(key), std::move(files), incoming_bytes);
  });
}

napi_value TakeCallback(napi_env env, napi_callback_info info) {
  return RunCallback(env, [&]() -> napi_value {
    std::size_t argc = 1;
    napi_value arguments[1];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 1) {
      ThrowTypeError(env, "takeDownload requires lookupKey.");
    }
    return store->Take(env, ReadString(env, arguments[0], "lookupKey"));
  });
}

napi_value CreateChunkedUploadCallback(napi_env env,
                                       napi_callback_info info) {
  return RunCallback(env, [&]() -> napi_value {
    std::size_t argc = 2;
    napi_value arguments[2];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 2) {
      ThrowTypeError(env,
                     "createChunkedUpload requires lookupKey and files.");
    }
    return store->CreateChunkedUpload(
        env, ReadString(env, arguments[0], "lookupKey"),
        ReadChunkedFiles(env, arguments[1]));
  });
}

napi_value BeginChunkCallback(napi_env env, napi_callback_info info) {
  return RunCallback(env, [&]() -> napi_value {
    std::size_t argc = 5;
    napi_value arguments[5];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 5) {
      ThrowTypeError(
          env,
          "beginChunk requires uploadId, fileId, index, iv and contentLength.");
    }
    const auto index = ReadNonNegativeInteger(env, arguments[2], "index");
    const auto content_length =
        ReadPositiveInteger(env, arguments[4], "contentLength");
    if (index > std::numeric_limits<std::uint32_t>::max() ||
        content_length > std::numeric_limits<std::size_t>::max()) {
      ThrowTypeError(env, "Chunk index or size is too large.");
    }
    return store->BeginChunk(
        env, ReadString(env, arguments[0], "uploadId"),
        ReadString(env, arguments[1], "fileId"),
        static_cast<std::uint32_t>(index),
        ReadBase64Value(env, arguments[3], "iv"),
        static_cast<std::size_t>(content_length));
  });
}

napi_value AppendChunkPartCallback(napi_env env, napi_callback_info info) {
  return RunCallback(env, [&]() -> napi_value {
    std::size_t argc = 4;
    napi_value arguments[4];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 4) {
      ThrowTypeError(
          env, "appendChunkPart requires uploadId, fileId, index and bytes.");
    }
    const auto index = ReadNonNegativeInteger(env, arguments[2], "index");
    if (index > std::numeric_limits<std::uint32_t>::max()) {
      ThrowTypeError(env, "Chunk index is too large.");
    }
    store->AppendChunkPart(
        ReadString(env, arguments[0], "uploadId"),
        ReadString(env, arguments[1], "fileId"),
        static_cast<std::uint32_t>(index),
        ReadByteView(env, arguments[3], "bytes"));
    return MakeUndefined(env);
  });
}

napi_value FinishChunkCallback(napi_env env, napi_callback_info info) {
  return RunCallback(env, [&]() -> napi_value {
    std::size_t argc = 3;
    napi_value arguments[3];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 3) {
      ThrowTypeError(env, "finishChunk requires uploadId, fileId and index.");
    }
    const auto index = ReadNonNegativeInteger(env, arguments[2], "index");
    if (index > std::numeric_limits<std::uint32_t>::max()) {
      ThrowTypeError(env, "Chunk index is too large.");
    }
    return store->FinishChunk(
        env, ReadString(env, arguments[0], "uploadId"),
        ReadString(env, arguments[1], "fileId"),
        static_cast<std::uint32_t>(index));
  });
}

napi_value FailChunkCallback(napi_env env, napi_callback_info info) {
  return RunCallback(env, [&]() -> napi_value {
    std::size_t argc = 3;
    napi_value arguments[3];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 3) {
      ThrowTypeError(env, "failChunk requires uploadId, fileId and index.");
    }
    const auto index = ReadNonNegativeInteger(env, arguments[2], "index");
    if (index <= std::numeric_limits<std::uint32_t>::max()) {
      store->FailChunk(
          ReadString(env, arguments[0], "uploadId"),
          ReadString(env, arguments[1], "fileId"),
          static_cast<std::uint32_t>(index));
    }
    return MakeUndefined(env);
  });
}

napi_value CompleteChunkedUploadCallback(napi_env env,
                                         napi_callback_info info) {
  return RunCallback(env, [&]() -> napi_value {
    std::size_t argc = 1;
    napi_value arguments[1];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 1) {
      ThrowTypeError(env, "completeChunkedUpload requires uploadId.");
    }
    return store->CompleteChunkedUpload(
        env, ReadString(env, arguments[0], "uploadId"));
  });
}

napi_value BeginChunkedDownloadCallback(napi_env env,
                                        napi_callback_info info) {
  return RunCallback(env, [&]() -> napi_value {
    std::size_t argc = 1;
    napi_value arguments[1];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 1) {
      ThrowTypeError(env, "beginChunkedDownload requires lookupKey.");
    }
    return store->BeginChunkedDownload(
        env, ReadString(env, arguments[0], "lookupKey"));
  });
}

napi_value AcquireChunkedDownloadChunkCallback(napi_env env,
                                               napi_callback_info info) {
  return RunCallback(env, [&]() -> napi_value {
    std::size_t argc = 4;
    napi_value arguments[4];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 4) {
      ThrowTypeError(
          env,
          "acquireChunkedDownloadChunk requires downloadId, token, fileId and index.");
    }
    const auto index = ReadNonNegativeInteger(env, arguments[3], "index");
    if (index > std::numeric_limits<std::uint32_t>::max()) {
      ThrowTypeError(env, "Chunk index is too large.");
    }
    return store->AcquireChunkedDownloadChunk(
        env, ReadString(env, arguments[0], "downloadId"),
        ReadString(env, arguments[1], "token"),
        ReadString(env, arguments[2], "fileId"),
        static_cast<std::uint32_t>(index));
  });
}

napi_value ReleaseChunkedDownloadChunkCallback(napi_env env,
                                               napi_callback_info info) {
  return RunCallback(env, [&]() -> napi_value {
    std::size_t argc = 2;
    napi_value arguments[2];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 2) {
      ThrowTypeError(env,
                     "releaseChunkedDownloadChunk requires downloadId and token.");
    }
    store->ReleaseChunkedDownloadChunk(
        ReadString(env, arguments[0], "downloadId"),
        ReadString(env, arguments[1], "token"));
    return MakeUndefined(env);
  });
}

napi_value FinishChunkedDownloadCallback(napi_env env,
                                         napi_callback_info info) {
  return RunCallback(env, [&]() -> napi_value {
    std::size_t argc = 2;
    napi_value arguments[2];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 2) {
      ThrowTypeError(env,
                     "finishChunkedDownload requires downloadId and token.");
    }
    return store->FinishChunkedDownload(
        env, ReadString(env, arguments[0], "downloadId"),
        ReadString(env, arguments[1], "token"));
  });
}

napi_value AbortChunkedUploadCallback(napi_env env,
                                      napi_callback_info info) {
  return RunCallback(env, [&]() -> napi_value {
    std::size_t argc = 1;
    napi_value arguments[1];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 1) {
      ThrowTypeError(env, "abortChunkedUpload requires uploadId.");
    }
    return store->AbortChunkedUpload(
        env, ReadString(env, arguments[0], "uploadId"));
  });
}

napi_value PurgeCallback(napi_env env, napi_callback_info info) {
  return RunCallback(env, [&]() -> napi_value {
    std::size_t argc = 0;
    UploadStore *store = GetStore(env, info, &argc, nullptr);
    return MakeNumber(
        env, static_cast<double>(store->PurgeExpired(NowMilliseconds())));
  });
}

napi_value ClearCallback(napi_env env, napi_callback_info info) {
  return RunCallback(env, [&]() -> napi_value {
    std::size_t argc = 0;
    UploadStore *store = GetStore(env, info, &argc, nullptr);
    return MakeNumber(env, static_cast<double>(store->Clear()));
  });
}

napi_value StatsCallback(napi_env env, napi_callback_info info) {
  return RunCallback(env, [&]() -> napi_value {
    std::size_t argc = 0;
    UploadStore *store = GetStore(env, info, &argc, nullptr);
    return store->Stats(env);
  });
}

void FinalizeStore(napi_env, void *data, void *) {
  delete static_cast<UploadStore *>(data);
}

napi_value CreateUploadStore(napi_env env, napi_callback_info info) {
  return RunCallback(env, [&]() -> napi_value {
    std::size_t argc = 1;
    napi_value arguments[1];
    CheckNapi(env,
              napi_get_cb_info(env, info, &argc, arguments, nullptr, nullptr));
    if (argc < 1) {
      ThrowTypeError(env, "createUploadStore requires ttlMs.");
    }

    const std::uint64_t ttl_ms =
        ReadPositiveInteger(env, arguments[0], "ttlMs");
    if (ttl_ms > static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max())) {
      ThrowTypeError(env, "Native store configuration is too large.");
    }

    auto store =
        std::make_unique<UploadStore>(static_cast<std::int64_t>(ttl_ms));

    napi_value object;
    CheckNapi(env, napi_create_object(env, &object));
    napi_property_descriptor properties[] = {
        {"upsertUpload", nullptr, UpsertCallback, nullptr, nullptr, nullptr,
         napi_default, store.get()},
        {"takeDownload", nullptr, TakeCallback, nullptr, nullptr, nullptr,
         napi_default, store.get()},
        {"createChunkedUpload", nullptr, CreateChunkedUploadCallback, nullptr,
         nullptr, nullptr, napi_default, store.get()},
        {"beginChunk", nullptr, BeginChunkCallback, nullptr, nullptr, nullptr,
         napi_default, store.get()},
        {"appendChunkPart", nullptr, AppendChunkPartCallback, nullptr, nullptr,
         nullptr, napi_default, store.get()},
        {"finishChunk", nullptr, FinishChunkCallback, nullptr, nullptr, nullptr,
         napi_default, store.get()},
        {"failChunk", nullptr, FailChunkCallback, nullptr, nullptr, nullptr,
         napi_default, store.get()},
        {"completeChunkedUpload", nullptr, CompleteChunkedUploadCallback,
         nullptr, nullptr, nullptr, napi_default, store.get()},
        {"beginChunkedDownload", nullptr, BeginChunkedDownloadCallback, nullptr,
         nullptr, nullptr, napi_default, store.get()},
        {"acquireChunkedDownloadChunk", nullptr,
         AcquireChunkedDownloadChunkCallback, nullptr, nullptr, nullptr,
         napi_default, store.get()},
        {"releaseChunkedDownloadChunk", nullptr,
         ReleaseChunkedDownloadChunkCallback, nullptr, nullptr, nullptr,
         napi_default, store.get()},
        {"finishChunkedDownload", nullptr, FinishChunkedDownloadCallback,
         nullptr, nullptr, nullptr, napi_default, store.get()},
        {"abortChunkedUpload", nullptr, AbortChunkedUploadCallback, nullptr,
         nullptr, nullptr, napi_default, store.get()},
        {"purgeExpired", nullptr, PurgeCallback, nullptr, nullptr, nullptr,
         napi_default, store.get()},
        {"clearUploads", nullptr, ClearCallback, nullptr, nullptr, nullptr,
         napi_default, store.get()},
        {"getStats", nullptr, StatsCallback, nullptr, nullptr, nullptr,
         napi_default, store.get()},
    };
    CheckNapi(env, napi_define_properties(
                       env, object, sizeof(properties) / sizeof(properties[0]),
                       properties));
    CheckNapi(env, napi_add_finalizer(env, object, store.get(), FinalizeStore,
                                      nullptr, nullptr));
    store.release();
    return object;
  });
}

napi_value Initialize(napi_env env, napi_value exports) {
  return RunCallback(env, [&]() -> napi_value {
    napi_property_descriptor descriptor = {"createUploadStore",
                                           nullptr,
                                           CreateUploadStore,
                                           nullptr,
                                           nullptr,
                                           nullptr,
                                           napi_default,
                                           nullptr};
    CheckNapi(env, napi_define_properties(env, exports, 1, &descriptor));
    return exports;
  });
}

} // namespace

NAPI_MODULE(NODE_GYP_MODULE_NAME, Initialize)
