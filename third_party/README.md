# third_party/

存放 Windows 构建所需的第三方依赖（仅 Windows 使用，Linux/Termux 不受影响）。

## curl-windows/

libcurl 的 Windows 开发文件，来自 curl 官方 Windows 包（curl-for-win）：

```
third_party/curl-windows/
├── include/curl/*.h          ← 头文件
├── lib/libcurl.dll.a         ← 动态导入库（CMake 链接用）
├── lib/libcurl.a             ← 静态库（备选）
├── bin/libcurl-x64.dll       ← 运行时 DLL（构建时自动复制到 exe 旁）
└── bin/curl.exe              ← curl 命令行工具（顺带）
```

CMake 在 Windows 下会自动查找 `third_party/curl-windows`（见 CMakeLists.txt 中
`_curl_local` 相关逻辑），找到后：
1. 链接 `libcurl.dll.a`（动态导入库，避免静态依赖链）；
2. 构建后自动把 `bin/libcurl-x64.dll` 复制到 `coding-agent.exe` 同目录。

> **若本目录丢失**（例如从 git 仓库重新 clone，二进制文件不随 git 分发），
> 在 Windows 上运行项目根目录的
> `powershell -ExecutionPolicy Bypass -File scripts\setup_curl_windows.ps1`
> 即可重新下载官方 curl 包并放置到本目录。
