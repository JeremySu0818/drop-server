{
  "targets": [
    {
      "target_name": "drop_core",
      "sources": [
        "native/src/entry.cc",
        "native/src/memory/secure_bytes.cc",
        "native/src/core/upload_store.cc",
        "native/src/napi/napi_helpers.cc"
      ],
      "include_dirs": [
        "native/includes"
      ],
      "defines": [
        "NAPI_VERSION=8"
      ],
      "cflags_cc": [
        "-std=c++17",
        "-fexceptions",
        "-Wall",
        "-Wextra",
        "-Wpedantic"
      ],
      "cflags_cc!": [
        "-fno-exceptions"
      ],
      "xcode_settings": {
        "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
        "GCC_ENABLE_CPP_EXCEPTIONS": "YES"
      },
      "msvs_settings": {
        "VCCLCompilerTool": {
          "ExceptionHandling": 1
        }
      }
    }
  ]
}
