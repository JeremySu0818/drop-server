#ifndef DROP_SERVER_NAPI_NAPI_HELPERS_H_
#define DROP_SERVER_NAPI_NAPI_HELPERS_H_

#include "core/types.h"

#include <node_api.h>

#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace drop_server::napi {

struct PendingJavaScriptException final {};

class NapiFailure final : public std::runtime_error {
public:
  explicit NapiFailure(const std::string &message);
};

void Check(napi_env env, napi_status status);
[[noreturn]] void ThrowTypeError(napi_env env, const char *message);

std::uint64_t ReadPositiveInteger(napi_env env, napi_value value,
                                  const char *name);
std::uint64_t ReadNonNegativeInteger(napi_env env, napi_value value,
                                     const char *name);
std::string ReadString(napi_env env, napi_value value, const char *name);
ByteView ReadByteView(napi_env env, napi_value value, const char *name);
SecureBytes ReadBase64Value(napi_env env, napi_value value, const char *name);
std::vector<EncryptedFile> ReadFiles(napi_env env, napi_value value,
                                     std::size_t *byte_size);
std::vector<ChunkedFile> ReadChunkedFiles(napi_env env, napi_value value);

napi_value MakeNumber(napi_env env, double value);
napi_value MakeUndefined(napi_env env);
napi_value Render(napi_env env, const StatusResult &result);
napi_value Render(napi_env env, const UpsertResult &result);
napi_value Render(napi_env env, TakeResult result);
napi_value Render(napi_env env, const Stats &stats);
napi_value Render(napi_env env, const CreateChunkedUploadResult &result);
napi_value Render(napi_env env, const CompleteChunkedUploadResult &result);
napi_value Render(napi_env env, const BeginChunkedDownloadResult &result);
napi_value Render(napi_env env, const AcquireChunkResult &result);

template <typename Callback> napi_value Run(napi_env env, Callback &&callback) {
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

} // namespace drop_server::napi

#endif
