# third_party/

存放 Windows 构建所需的第三方依赖（仅 Windows 使用，Linux/Termux 不受影响）。

## curl-windows/

libcurl 的 Windows 开发文件（头文件 + 静态/导入库 + DLL）。

> **如何生成：** 在 Windows 上运行项目根目录下的
> `powershell -ExecutionPolicy Bypass -File scripts\setup_curl_windows.ps1`
> 脚本会自动下载官方 curl 包并解压放置到本目录。

预期结构：

```
third_party/curl-windows/
├── include/curl/*.h          ← 头文件
├── lib/libcurl*.a            ← 静态库 / 导入库
└── bin/libcurl-*.dll         ← 运行时 DLL（动态链接时需要）
```

CMake 在 Windows 下会自动查找 `third_party/curl-windows`（见 CMakeLists.txt 中
`_curl_local` 相关逻辑），找到后无需 `-DCMAKE_PREFIX_PATH` 或 vcpkg 即可编译。

> ⚠️ 本目录内的二进制文件（.a / .dll / .exe）体积较大，建议加入 `.gitignore`，
> 不随源码仓库分发；换新机器时重新运行脚本即可。
