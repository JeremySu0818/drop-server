#include <node_api.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <strings.h>
#endif

namespace {

constexpr double kMaxSafeInteger = 9007199254740991.0;
constexpr const char *kNotFoundMessage =
    "Image not found. It may have already been downloaded or expired.";
constexpr const char *kExpiredMessage = "Image has expired.";
constexpr const char *kCapacityMessage = "Encrypted storage capacity exceeded.";

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

napi_value MakeString(napi_env env, const char *value) {
  napi_value result;
  CheckNapi(env,
            napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &result));
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

class UploadStore {
public:
  UploadStore(std::int64_t ttl_ms, std::size_t capacity_bytes)
      : ttl_ms_(ttl_ms), capacity_bytes_(capacity_bytes) {}

  std::size_t PurgeExpired(std::int64_t now) {
    std::size_t purged = 0;
    for (auto iterator = uploads_.begin(); iterator != uploads_.end();) {
      if (iterator->second.expires_at <= now) {
        RemoveFromTotals(iterator->second);
        iterator = uploads_.erase(iterator);
        ++purged;
      } else {
        ++iterator;
      }
    }
    return purged;
  }

  napi_value Upsert(napi_env env, std::string key,
                    std::vector<EncryptedFile> files,
                    std::size_t incoming_bytes) {
    const std::int64_t now = NowMilliseconds();
    PurgeExpired(now);

    if (incoming_bytes > capacity_bytes_ - total_bytes_) {
      return MakeResult(env, 507, MakeErrorPayload(env, kCapacityMessage));
    }

    auto existing = uploads_.find(key);
    if (existing != uploads_.end()) {
      auto &record = existing->second;
      record.files.reserve(record.files.size() + files.size());
      for (auto &file : files) {
        record.files.push_back(std::move(file));
      }
      record.bytes += incoming_bytes;
      total_bytes_ += incoming_bytes;
      file_count_ += files.size();

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
    total_bytes_ += incoming_bytes;
    file_count_ += incoming_file_count;

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
    RemoveFromTotals(record);

    if (record.expires_at <= NowMilliseconds()) {
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
    return MakeResult(env, 200, payload);
  }

  std::size_t Clear() {
    const std::size_t cleared = uploads_.size();
    uploads_.clear();
    total_bytes_ = 0;
    file_count_ = 0;
    return cleared;
  }

  napi_value Stats(napi_env env) {
    PurgeExpired(NowMilliseconds());
    napi_value stats;
    CheckNapi(env, napi_create_object(env, &stats));
    SetProperty(env, stats, "uploadCount",
                MakeNumber(env, static_cast<double>(uploads_.size())));
    SetProperty(env, stats, "fileCount",
                MakeNumber(env, static_cast<double>(file_count_)));
    SetProperty(env, stats, "encryptedBytes",
                MakeNumber(env, static_cast<double>(total_bytes_)));
    SetProperty(env, stats, "capacityBytes",
                MakeNumber(env, static_cast<double>(capacity_bytes_)));
    return stats;
  }

private:
  void RemoveFromTotals(const UploadRecord &record) noexcept {
    total_bytes_ -= record.bytes;
    file_count_ -= record.files.size();
  }

  std::unordered_map<std::string, UploadRecord> uploads_;
  std::int64_t ttl_ms_;
  std::size_t capacity_bytes_;
  std::size_t total_bytes_ = 0;
  std::size_t file_count_ = 0;
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
    std::size_t argc = 2;
    napi_value arguments[2];
    CheckNapi(env,
              napi_get_cb_info(env, info, &argc, arguments, nullptr, nullptr));
    if (argc < 2) {
      ThrowTypeError(env,
                     "createUploadStore requires ttlMs and capacityBytes.");
    }

    const std::uint64_t ttl_ms =
        ReadPositiveInteger(env, arguments[0], "ttlMs");
    const std::uint64_t capacity_bytes =
        ReadPositiveInteger(env, arguments[1], "capacityBytes");
    if (ttl_ms > static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max()) ||
        capacity_bytes > static_cast<std::uint64_t>(
                             std::numeric_limits<std::size_t>::max())) {
      ThrowTypeError(env, "Native store configuration is too large.");
    }

    auto store =
        std::make_unique<UploadStore>(static_cast<std::int64_t>(ttl_ms),
                                      static_cast<std::size_t>(capacity_bytes));

    napi_value object;
    CheckNapi(env, napi_create_object(env, &object));
    napi_property_descriptor properties[] = {
        {"upsertUpload", nullptr, UpsertCallback, nullptr, nullptr, nullptr,
         napi_default, store.get()},
        {"takeDownload", nullptr, TakeCallback, nullptr, nullptr, nullptr,
         napi_default, store.get()},
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
