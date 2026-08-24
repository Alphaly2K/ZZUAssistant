# ZZUAssistant

ZZUAssistant 是一个面向郑州大学相关服务的跨平台命令行工具。

> 本项目不是郑州大学官方软件。登录、充值等接口可能随学校系统更新而变化。
> 执行真实充值前请仔细核对用户、房间、类型和金额。

## 功能

- Web SSO/CAS 登录、短信 MFA、超级 App 扫码 MFA 和独立会话管理
- 超级 App 手机短信登录及独立凭据存储
- 校园网 Portal 发现、登录和注销
- 校园卡余额查询、充值订单创建及终端支付二维码
- 照明电费和空调电费房间配置、余额查询及充值
- 教务系统课程表读取及 ICS 日历导出
- PowerShell、Bash、Zsh 和 Fish 命令补全
- 多用户隔离：每个用户名对应独立哈希会话文件

SSO、Portal 和超级 App 是三套相互独立的认证路径。校园卡、电费和课程表功能默认使用当前超级 App 用户。

## 构建

推荐使用 vcpkg 清单模式。请先准备支持 C++23 的编译器、CMake 3.25 或
更高版本、Ninja、Git、Python 3 和 vcpkg：

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "C:\vcpkg"

cmake --preset vcpkg-debug
cmake --build --preset vcpkg-debug
```

Release 构建：

```powershell
cmake --preset vcpkg-release
cmake --build --preset vcpkg-release
```

详细的跨平台和手动依赖构建方法参见 [BUILDING.md](BUILDING.md)。

## 常用命令

查看主帮助：

```text
ZZUAssistant help
```

### 认证

```text
ZZUAssistant app login <用户名>
ZZUAssistant app logout [用户名]

ZZUAssistant sso login <用户名> [--mfa phone|qr]
ZZUAssistant sso logout [用户名]

ZZUAssistant portal discover
ZZUAssistant portal login [用户名]
ZZUAssistant portal logout [用户名]
```

### 校园卡和电费

```text
ZZUAssistant ecard balance [用户名] [--porcelain]
ZZUAssistant ecard recharge <金额> [用户名] [-y|--yes]

ZZUAssistant electricity setup [用户名]
ZZUAssistant electricity show [用户名] [--porcelain]
ZZUAssistant electricity recharge <lighting|air> <金额> [用户名]
```

校园卡充值默认在终端显示支付二维码；使用 `--porcelain --yes` 时输出收银台
URL，便于程序读取。


### 课程表

```text
ZZUAssistant course [用户名]
ZZUAssistant course --semester <current|学期ID|学期代码>
ZZUAssistant course --semester 2026-2027-1 -o schedule.ics
```

课程表导出使用服务器提供的实际上课日期，因此能够保留调课、补课和周末课程。

### 命令补全

```text
ZZUAssistant _completions powershell install <配置文件路径>
ZZUAssistant _completions bash install
ZZUAssistant _completions zsh install
ZZUAssistant _completions fish install
```

## 环境变量

### ZZUAssistant 专用变量

| 变量 | 用途 |
| --- | --- |
| `ZZUASSISTANT_STATE_DIR` | 覆盖会话数据库、当前用户和电费房间配置的保存目录。 |
| `ZZUASSISTANT_APP_DEVICE_ID` | 覆盖超级 App 登录使用的 device ID；设为手机端同一值可降低手机会话被挤下线的概率。 |
| `ZZUASSISTANT_ECARD_PAYMENT_PASSWORD` | `electricity recharge --yes` 默认读取的校园卡支付密码，用于无交互电费充值。 |
| `ZZUASSISTANT_ECARD_ACCESS_TOKEN` | 手动导入 eCard access token；通常不需要设置，正常情况下由超级 App 会话自动换取。 |
| `ZZUASSISTANT_ECARD_REFRESH_TOKEN` | 与手动导入的 eCard access token 配套的 refresh token。 |
| `ZZUASSISTANT_PYTHON` | 指定课程表工具使用的 Python 3 解释器完整路径。 |
| `ZZUASSISTANT_TOOL_DIR` | 覆盖 Python 工具目录；目录中应包含 `course.py`。 |

电费静默充值的 PowerShell 示例：

```powershell
$env:ZZUASSISTANT_ECARD_PAYMENT_PASSWORD = "校园卡支付密码"
ZZUAssistant electricity recharge lighting 10 --yes
Remove-Item Env:ZZUASSISTANT_ECARD_PAYMENT_PASSWORD
```

Bash、Zsh 或其他 POSIX Shell：

```bash
ZZUASSISTANT_ECARD_PAYMENT_PASSWORD='校园卡支付密码' \
  ./ZZUAssistant electricity recharge lighting 10 --yes
```

也可以使用 `--payment-password-env NAME` 从自定义变量读取密码。密码不会写入
会话数据库，程序使用后的内存副本会被清理。不要把真实密码提交到源码、脚本或
版本控制中，也不建议长期写入 Shell 配置文件。

如需让 CLI 与 iOS 手机复用同一超级 App 设备标识，可从手机登录请求或 JWT 的
`deviceId` 字段取得值，然后在重新登录前设置：

```powershell
$env:ZZUASSISTANT_APP_DEVICE_ID = "手机端的 deviceId"
ZZUAssistant app login <用户名>
```

设置值会保存到该用户独立的超级 App 会话中。后续未设置环境变量时仍会复用已
保存的 device ID；只有首次登录且没有覆盖值时才会随机生成。

### 通用和构建变量

| 变量 | 用途 |
| --- | --- |
| `SSL_CERT_FILE` | 指定 HTTPS 使用的 CA 证书包。系统证书不可用时可设置。 |
| `NO_COLOR` | 禁用 ANSI 彩色输出。 |
| `VCPKG_ROOT` | vcpkg 安装目录，供 `CMakePresets.json` 定位工具链。 |
| `LOCALAPPDATA` | Windows 默认状态目录的系统来源，一般无需手动设置。 |
| `HOME` | macOS/Linux 状态目录及 Shell 配置目录的系统来源。 |
| `XDG_STATE_HOME` | Linux 状态目录；设置后优先于 `$HOME/.local/state`。 |
| `XDG_CONFIG_HOME` | Linux Fish 补全配置目录；设置后优先于 `$HOME/.config`。 |

## 数据和凭据

默认状态目录：

- Windows：`%LOCALAPPDATA%\ZZUAssistant`
- macOS：`~/Library/Application Support/ZZUAssistant`
- Linux：`$XDG_STATE_HOME/zzu-assistant`，未设置时使用
  `~/.local/state/zzu-assistant`

SSO Cookie、Portal 会话、超级 App JWT 和 eCard token 分开保存。用户会话文件
使用用户名的 SHA-256 哈希作为文件名，切换当前用户不会复用其他用户的缓存。

## 许可证

除明确标记的第三方组件外，本项目使用 [PolyForm Noncommercial License 1.0.0](LICENSE.md)，仅授权非商业用途。该许可证属于源码可用许可证，不是 OSI 认可的开源许可证。版权通知见 [NOTICE](NOTICE)，
第三方组件的版权和许可证见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 致谢

- [ZZU.Py](https://github.com/Illustar0/ZZU.Py)：为郑州大学相关认证、Portal、
  校园卡和教务接口的实现提供了重要参考；该项目使用 MIT 许可证。
- [Project Nayuki QR Code Generator](https://www.nayuki.io/page/qr-code-generator-library)：
  提供终端二维码生成核心，使用 MIT 许可证。
- Boost、OpenSSL、libpng 和 zlib：提供网络、密码学和图像处理基础能力。
