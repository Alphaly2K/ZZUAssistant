# ZZUAssistant

郑州大学相关服务的跨平台命令行工具。

> 非官方项目。充值前请核对用户、房间、类型和金额。

## 功能

- Web SSO、超级 App 和校园网 Portal 登录
- 用户信息、校园卡余额与课程表查询
- 校园卡、电费充值及终端二维码
- 照明/空调电费房间配置与自动充值脚本
- 多用户独立会话和 Shell 命令补全

SSO、超级 App 和 Portal 使用独立凭据。校园卡、电费和课程表使用超级 App
身份。

## 构建

需要 C++23 编译器、CMake 3.25+、Ninja、Python 3 和 vcpkg。

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "C:\vcpkg"

cmake --preset vcpkg-debug
cmake --build --preset vcpkg-debug
```

Release 使用 `vcpkg-release` preset。其他构建方式见 [BUILDING.md](BUILDING.md)。

## 使用

```text
ZZUAssistant help

ZZUAssistant sso login [用户名] [--mfa phone|qr]
ZZUAssistant sso logout [用户名]
ZZUAssistant userinfo [用户名] [--porcelain]

ZZUAssistant app login [用户名]
ZZUAssistant app logout [用户名]

ZZUAssistant portal discover [--porcelain]
ZZUAssistant portal login [用户名]
ZZUAssistant portal logout [用户名]

ZZUAssistant ecard balance [用户名] [--porcelain]
ZZUAssistant ecard recharge <金额> [用户名] [-y|--yes]

ZZUAssistant electricity setup [用户名]
ZZUAssistant electricity show [用户名] [--porcelain]
ZZUAssistant electricity recharge <lighting|air> <金额> [用户名]

ZZUAssistant course [用户名] [--semester <current|ID|代码>] [-o 文件]
```

用户名按“命令行参数 → `ZZUASSISTANT_USER` → 对应认证保存的当前用户”选择。
`--porcelain` 输出供程序读取。校园卡充值默认显示二维码。

命令补全：

```text
ZZUAssistant _completions <powershell|bash|zsh|fish> install [配置路径]
```

## 环境变量

| 变量 | 用途 |
| --- | --- |
| `ZZUASSISTANT_USER` | 当前操作用户 |
| `ZZUASSISTANT_STATE_DIR` | 状态和会话目录 |
| `ZZUASSISTANT_SSO_TOKEN` | CAS TGC 或 Cookie |
| `ZZUASSISTANT_APP_TOKEN` | 超级 App `idToken` |
| `ZZUASSISTANT_APP_DEVICE_ID` | 超级 App 设备 ID |
| `ZZUASSISTANT_ECARD_TOKEN` | eCard access token |
| `ZZUASSISTANT_ECARD_REFRESH_TOKEN` | eCard refresh token |
| `ZZUASSISTANT_ECARD_PAYMENT_PASSWORD` | 静默电费充值密码 |
| `ZZUASSISTANT_PYTHON` | Python 3 路径 |
| `ZZUASSISTANT_TOOL_DIR` | Python 工具目录 |
| `SSL_CERT_FILE` | CA 证书包 |
| `NO_COLOR` | 禁用彩色输出 |
| `VCPKG_ROOT` | vcpkg 目录 |

环境变量 token 只用于当前进程，不覆盖已保存的会话。不要将 token 或支付密码
提交到版本控制。

## 数据

状态默认保存在：

- Windows：`%LOCALAPPDATA%\ZZUAssistant`
- macOS：`~/Library/Application Support/ZZUAssistant`
- Linux：`$XDG_STATE_HOME/zzu-assistant` 或 `~/.local/state/zzu-assistant`

各认证和用户的凭据分开保存，用户会话文件名使用用户名的 SHA-256 哈希。

## 许可证

除第三方组件外，本项目使用
[PolyForm Noncommercial License 1.0.0](LICENSE.md)，仅授权非商业用途。
第三方许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 致谢

- [ZZU.Py](https://github.com/Illustar0/ZZU.Py)
- [Project Nayuki QR Code Generator](https://www.nayuki.io/page/qr-code-generator-library)
- Boost、OpenSSL、libpng 和 zlib
