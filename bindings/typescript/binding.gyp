{
  # Builds the N-API addon and static-links flowd/libflowd.a directly
  # into flowd_native.node — no shared libflowd, no FLOWD_LIB lookup.
  # libflowd.a references libcrypto (SHA-256) and libcurl (model gateway),
  # so both are linked. Paths use <(module_root_dir) (this package's root)
  # so they resolve regardless of the build output directory.
  "targets": [
    {
      "target_name": "flowd_native",
      "sources": ["src/native.c"],
      "include_dirs": ["<(module_root_dir)/../../flowd/src"],
      "libraries": [
        "<(module_root_dir)/../../flowd/libflowd.a",
        "-lcrypto",
        "-lcurl"
      ],
      "defines": ["NAPI_VERSION=8"],
      "conditions": [
        ["OS=='mac'", {
          "xcode_settings": {
            "OTHER_CFLAGS": ["-Wall", "-Wextra"],
            # libcrypto has no linkable system copy on macOS; libcurl is
            # taken from Homebrew to match the flowd build. Override the
            # search paths here for non-Homebrew layouts.
            "OTHER_LDFLAGS": [
              "-L/opt/homebrew/opt/openssl@3/lib",
              "-L/opt/homebrew/opt/curl/lib"
            ]
          }
        }],
        ["OS=='linux'", {
          "cflags": ["-Wall", "-Wextra"]
        }]
      ]
    }
  ]
}
