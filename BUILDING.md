# 构建 ZZUAssistant

推荐使用 vcpkg 的清单模式管理依赖。该方式只会安装本项目实际使用的
Boost 组件，以及 OpenSSL 和 libpng，不需要安装完整 Boost 软件包。

## 使用 vcpkg 构建

请先安装支持 C++23 的编译器、CMake 3.25 或更高版本、Ninja、Git 和
Python 3。然后在 PowerShell 中执行：

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "C:\vcpkg"
cmake --preset vcpkg-debug
cmake --build --preset vcpkg-debug
```

Linux 或 macOS 请运行 `./bootstrap-vcpkg.sh`，并将 `VCPKG_ROOT` 设置为
对应的 vcpkg 克隆目录。需要优化构建时，将预设名称换成
`vcpkg-release`。

Windows 默认 triplet 使用动态链接的 OpenSSL/libpng 和 `/MD` 运行库，
所需 DLL 由 vcpkg 部署。项目目前使用的 Boost 组件均为仅头文件组件。
如需指定其他 triplet，可在配置时传入：

```powershell
cmake --preset vcpkg-debug -DVCPKG_TARGET_TRIPLET=<triplet>
```

## 手动提供依赖

原来的手动依赖构建方式仍然可用：

```powershell
cmake -S . -B build `
  -DBOOST_ROOT=C:\path\to\boost `
  -DZZU_LOCAL_DEPENDENCY_ROOT=C:\path\to\dependency-prefix
cmake --build build
```

只有在所有手动提供的 Windows 依赖都使用匹配的静态 CRT 时，才应设置
`ZZU_USE_STATIC_MSVC_RUNTIME=ON`。运行时可通过 `SSL_CERT_FILE` 指定 CA
证书包，也可通过 `ZZUASSISTANT_PYTHON` 指定 Python 解释器。
