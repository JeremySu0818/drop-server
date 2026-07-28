#include "core/upload_store.h"
#include "napi/napi_helpers.h"

#include <node_api.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

namespace drop_server {
namespace {

UploadStore *GetStore(napi_env env, napi_callback_info info, std::size_t *argc,
                      napi_value *arguments) {
  void *data = nullptr;
  napi::Check(env,
              napi_get_cb_info(env, info, argc, arguments, nullptr, &data));
  if (data == nullptr) {
    throw napi::NapiFailure("Native encrypted store is unavailable.");
  }
  return static_cast<UploadStore *>(data);
}

napi_value UpsertCallback(napi_env env, napi_callback_info info) {
  return napi::Run(env, [&]() -> napi_value {
    std::size_t argc = 2;
    napi_value arguments[2];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 2) {
      napi::ThrowTypeError(env, "upsertUpload requires lookupKey and files.");
    }

    std::string key = napi::ReadString(env, arguments[0], "lookupKey");
    std::size_t incoming_bytes = 0;
    std::vector<EncryptedFile> files =
        napi::ReadFiles(env, arguments[1], &incoming_bytes);
    return napi::Render(
        env, store->Upsert(std::move(key), std::move(files), incoming_bytes));
  });
}

napi_value TakeCallback(napi_env env, napi_callback_info info) {
  return napi::Run(env, [&]() -> napi_value {
    std::size_t argc = 1;
    napi_value arguments[1];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 1) {
      napi::ThrowTypeError(env, "takeDownload requires lookupKey.");
    }
    return napi::Render(
        env, store->Take(napi::ReadString(env, arguments[0], "lookupKey")));
  });
}

napi_value CreateChunkedUploadCallback(napi_env env, napi_callback_info info) {
  return napi::Run(env, [&]() -> napi_value {
    std::size_t argc = 2;
    napi_value arguments[2];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 2) {
      napi::ThrowTypeError(env,
                           "createChunkedUpload requires lookupKey and files.");
    }
    return napi::Render(env,
                        store->CreateChunkedUpload(
                            napi::ReadString(env, arguments[0], "lookupKey"),
                            napi::ReadChunkedFiles(env, arguments[1])));
  });
}

napi_value BeginChunkCallback(napi_env env, napi_callback_info info) {
  return napi::Run(env, [&]() -> napi_value {
    std::size_t argc = 5;
    napi_value arguments[5];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 5) {
      napi::ThrowTypeError(
          env,
          "beginChunk requires uploadId, fileId, index, iv and contentLength.");
    }
    const auto index = napi::ReadNonNegativeInteger(env, arguments[2], "index");
    const auto content_length =
        napi::ReadPositiveInteger(env, arguments[4], "contentLength");
    if (index > std::numeric_limits<std::uint32_t>::max() ||
        content_length > std::numeric_limits<std::size_t>::max()) {
      napi::ThrowTypeError(env, "Chunk index or size is too large.");
    }
    return napi::Render(
        env, store->BeginChunk(napi::ReadString(env, arguments[0], "uploadId"),
                               napi::ReadString(env, arguments[1], "fileId"),
                               static_cast<std::uint32_t>(index),
                               napi::ReadBase64Value(env, arguments[3], "iv"),
                               static_cast<std::size_t>(content_length)));
  });
}

napi_value AppendChunkPartCallback(napi_env env, napi_callback_info info) {
  return napi::Run(env, [&]() -> napi_value {
    std::size_t argc = 4;
    napi_value arguments[4];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 4) {
      napi::ThrowTypeError(
          env, "appendChunkPart requires uploadId, fileId, index and bytes.");
    }
    const auto index = napi::ReadNonNegativeInteger(env, arguments[2], "index");
    if (index > std::numeric_limits<std::uint32_t>::max()) {
      napi::ThrowTypeError(env, "Chunk index is too large.");
    }
    store->AppendChunkPart(napi::ReadString(env, arguments[0], "uploadId"),
                           napi::ReadString(env, arguments[1], "fileId"),
                           static_cast<std::uint32_t>(index),
                           napi::ReadByteView(env, arguments[3], "bytes"));
    return napi::MakeUndefined(env);
  });
}

napi_value FinishChunkCallback(napi_env env, napi_callback_info info) {
  return napi::Run(env, [&]() -> napi_value {
    std::size_t argc = 3;
    napi_value arguments[3];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 3) {
      napi::ThrowTypeError(env,
                           "finishChunk requires uploadId, fileId and index.");
    }
    const auto index = napi::ReadNonNegativeInteger(env, arguments[2], "index");
    if (index > std::numeric_limits<std::uint32_t>::max()) {
      napi::ThrowTypeError(env, "Chunk index is too large.");
    }
    return napi::Render(
        env, store->FinishChunk(napi::ReadString(env, arguments[0], "uploadId"),
                                napi::ReadString(env, arguments[1], "fileId"),
                                static_cast<std::uint32_t>(index)));
  });
}

napi_value FailChunkCallback(napi_env env, napi_callback_info info) {
  return napi::Run(env, [&]() -> napi_value {
    std::size_t argc = 3;
    napi_value arguments[3];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 3) {
      napi::ThrowTypeError(env,
                           "failChunk requires uploadId, fileId and index.");
    }
    const auto index = napi::ReadNonNegativeInteger(env, arguments[2], "index");
    if (index <= std::numeric_limits<std::uint32_t>::max()) {
      store->FailChunk(napi::ReadString(env, arguments[0], "uploadId"),
                       napi::ReadString(env, arguments[1], "fileId"),
                       static_cast<std::uint32_t>(index));
    }
    return napi::MakeUndefined(env);
  });
}

napi_value CompleteChunkedUploadCallback(napi_env env,
                                         napi_callback_info info) {
  return napi::Run(env, [&]() -> napi_value {
    std::size_t argc = 1;
    napi_value arguments[1];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 1) {
      napi::ThrowTypeError(env, "completeChunkedUpload requires uploadId.");
    }
    return napi::Render(env, store->CompleteChunkedUpload(napi::ReadString(
                                 env, arguments[0], "uploadId")));
  });
}

napi_value BeginChunkedDownloadCallback(napi_env env, napi_callback_info info) {
  return napi::Run(env, [&]() -> napi_value {
    std::size_t argc = 1;
    napi_value arguments[1];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 1) {
      napi::ThrowTypeError(env, "beginChunkedDownload requires lookupKey.");
    }
    return napi::Render(env, store->BeginChunkedDownload(napi::ReadString(
                                 env, arguments[0], "lookupKey")));
  });
}

napi_value AcquireChunkedDownloadChunkCallback(napi_env env,
                                               napi_callback_info info) {
  return napi::Run(env, [&]() -> napi_value {
    std::size_t argc = 4;
    napi_value arguments[4];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 4) {
      napi::ThrowTypeError(env,
                           "acquireChunkedDownloadChunk requires downloadId, "
                           "token, fileId and index.");
    }
    const auto index = napi::ReadNonNegativeInteger(env, arguments[3], "index");
    if (index > std::numeric_limits<std::uint32_t>::max()) {
      napi::ThrowTypeError(env, "Chunk index is too large.");
    }
    return napi::Render(env,
                        store->AcquireChunkedDownloadChunk(
                            napi::ReadString(env, arguments[0], "downloadId"),
                            napi::ReadString(env, arguments[1], "token"),
                            napi::ReadString(env, arguments[2], "fileId"),
                            static_cast<std::uint32_t>(index)));
  });
}

napi_value ReleaseChunkedDownloadChunkCallback(napi_env env,
                                               napi_callback_info info) {
  return napi::Run(env, [&]() -> napi_value {
    std::size_t argc = 2;
    napi_value arguments[2];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 2) {
      napi::ThrowTypeError(
          env, "releaseChunkedDownloadChunk requires downloadId and token.");
    }
    store->ReleaseChunkedDownloadChunk(
        napi::ReadString(env, arguments[0], "downloadId"),
        napi::ReadString(env, arguments[1], "token"));
    return napi::MakeUndefined(env);
  });
}

napi_value FinishChunkedDownloadCallback(napi_env env,
                                         napi_callback_info info) {
  return napi::Run(env, [&]() -> napi_value {
    std::size_t argc = 2;
    napi_value arguments[2];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 2) {
      napi::ThrowTypeError(
          env, "finishChunkedDownload requires downloadId and token.");
    }
    return napi::Render(env,
                        store->FinishChunkedDownload(
                            napi::ReadString(env, arguments[0], "downloadId"),
                            napi::ReadString(env, arguments[1], "token")));
  });
}

napi_value AbortChunkedUploadCallback(napi_env env, napi_callback_info info) {
  return napi::Run(env, [&]() -> napi_value {
    std::size_t argc = 1;
    napi_value arguments[1];
    UploadStore *store = GetStore(env, info, &argc, arguments);
    if (argc < 1) {
      napi::ThrowTypeError(env, "abortChunkedUpload requires uploadId.");
    }
    return napi::Render(env, store->AbortChunkedUpload(napi::ReadString(
                                 env, arguments[0], "uploadId")));
  });
}

napi_value PurgeCallback(napi_env env, napi_callback_info info) {
  return napi::Run(env, [&]() -> napi_value {
    std::size_t argc = 0;
    UploadStore *store = GetStore(env, info, &argc, nullptr);
    return napi::MakeNumber(
        env, static_cast<double>(store->PurgeExpired(NowMilliseconds())));
  });
}

napi_value ClearCallback(napi_env env, napi_callback_info info) {
  return napi::Run(env, [&]() -> napi_value {
    std::size_t argc = 0;
    UploadStore *store = GetStore(env, info, &argc, nullptr);
    return napi::MakeNumber(env, static_cast<double>(store->Clear()));
  });
}

napi_value StatsCallback(napi_env env, napi_callback_info info) {
  return napi::Run(env, [&]() -> napi_value {
    std::size_t argc = 0;
    UploadStore *store = GetStore(env, info, &argc, nullptr);
    return napi::Render(env, store->GetStats());
  });
}

void FinalizeStore(napi_env, void *data, void *) {
  delete static_cast<UploadStore *>(data);
}

napi_value CreateUploadStore(napi_env env, napi_callback_info info) {
  return napi::Run(env, [&]() -> napi_value {
    std::size_t argc = 1;
    napi_value arguments[1];
    napi::Check(
        env, napi_get_cb_info(env, info, &argc, arguments, nullptr, nullptr));
    if (argc < 1) {
      napi::ThrowTypeError(env, "createUploadStore requires ttlMs.");
    }

    const std::uint64_t ttl_ms =
        napi::ReadPositiveInteger(env, arguments[0], "ttlMs");
    if (ttl_ms >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      napi::ThrowTypeError(env, "Native store configuration is too large.");
    }

    auto store =
        std::make_unique<UploadStore>(static_cast<std::int64_t>(ttl_ms));

    napi_value object;
    napi::Check(env, napi_create_object(env, &object));
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
    napi::Check(
        env, napi_define_properties(env, object,
                                    sizeof(properties) / sizeof(properties[0]),
                                    properties));
    napi::Check(env, napi_add_finalizer(env, object, store.get(), FinalizeStore,
                                        nullptr, nullptr));
    store.release();
    return object;
  });
}

napi_value Initialize(napi_env env, napi_value exports) {
  return napi::Run(env, [&]() -> napi_value {
    napi_property_descriptor descriptor = {"createUploadStore",
                                           nullptr,
                                           CreateUploadStore,
                                           nullptr,
                                           nullptr,
                                           nullptr,
                                           napi_default,
                                           nullptr};
    napi::Check(env, napi_define_properties(env, exports, 1, &descriptor));
    return exports;
  });
}

} // namespace
} // namespace drop_server

NAPI_MODULE(NODE_GYP_MODULE_NAME, drop_server::Initialize)
