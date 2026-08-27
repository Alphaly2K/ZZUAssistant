# ZZUAssistant Client API

本文档描述 `ZZUAssistant::Client` 静态库的完整公开 C++ API。当前项目版本为
`0.1.0`，要求 C++23。

其他语言应使用独立的 [C ABI / FFI 文档](FFI.md)。

## 1. 设计边界

Client 库只负责同步网络请求、协议处理和对象内会话状态：

- 不读取环境变量；
- 不读写凭证文件；
- 不选择 CLI 的当前用户；
- 不包含 CLI 输出、交互提示或二维码终端渲染；
- 登录成功后的凭证只保存在对应 Client 对象内存中。

调用方可用 `session()` 导出会话快照，并用特殊重载 `login(Session)` 恢复。
快照的加密、持久化、权限控制和生命周期均由调用方负责。

所有网络 API 均为同步调用。Client 可移动、不可复制；同一个实例不保证线程安全。
如需并发调用，应为每个并发流程创建独立实例，或由调用方串行化访问。

## 2. 引入与链接

统一头文件：

```cpp
#include <zzuassistant/client.h>
```

源码方式：

```cmake
set(ZZU_BUILD_CLI OFF CACHE BOOL "" FORCE)
add_subdirectory(path/to/ZZUAssistant)
target_link_libraries(your_target PRIVATE ZZUAssistant::Client)
```

安装方式：

```cmake
find_package(ZZUAssistant CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE ZZUAssistant::Client)
```

依赖由 CMake 包配置查找：Boost 1.84+、OpenSSL 3、Threads。库本身以静态库形式
构建；OpenSSL 是否动态链接由所使用的 OpenSSL 包决定。

## 3. 通用配置

命名空间：`zzu_assistant`

```cpp
struct NetworkOptions {
    std::string ca_file;
};
```

`ca_file` 是 PEM CA 证书包路径。为空时使用 OpenSSL 默认验证路径；若构建时配置了
项目的 CA fallback，也可能使用该路径。该选项不会从 `SSL_CERT_FILE` 自动读取。

## 4. Super App API

头文件：`client/app_client.h`  
命名空间：`zzu_assistant::app`

### 4.1 类型

```cpp
struct ClientOptions : NetworkOptions {
    std::string device_id;
};
```

- `device_id`：调用方管理的设备 ID。建议保持稳定，以避免产生额外设备会话。
- 为空时，首次密码登录会在对象内生成随机 UUID。
- 使用 `login(Session)` 时，构造参数中非空的 `device_id` 优先于快照中的值。

```cpp
struct LoginOptions {
    std::string_view username;
    std::string_view password;
    std::function<void(std::string_view)> notify;
    std::function<std::string(std::string_view)> prompt;
};

struct LoginResult {
    bool success{false};
    std::string message;
};

struct Session {
    std::string username;
    std::string device_id;
    std::string id_token;
    std::string refresh_token;
};
```

`notify` 接收 MFA 状态文本；`prompt` 在需要手机短信 MFA 时读取验证码。回调仅在
`login(LoginOptions)` 调用期间同步执行。

### 4.2 `AppClient`

```cpp
explicit AppClient(ClientOptions options = {});
```

创建未登录 Client。构造过程会配置 TLS，但不会发起登录。

```cpp
LoginResult login(const LoginOptions& options);
```

通过用户名、密码和手机短信 MFA 获取 Super App 凭证。认证和网络错误通常封装在
`LoginResult` 中，不要求调用方捕获异常。

```cpp
LoginResult login(const Session& session);
```

将已有凭证直接装入对象，不访问网络。用户名或 `id_token` 为空时返回失败。

```cpp
LoginResult logout();
```

清除对象内 Super App 会话。当前实现不向远端发送注销请求。

```cpp
std::string id_token() const;
```

返回当前 `idToken`。对象未登录时抛出 `std::runtime_error`。

```cpp
double card_balance() const;
```

查询当前用户校园卡余额，单位为元。需要已登录的 App 会话；网络、HTTP 或响应格式
错误抛出 `std::runtime_error`。

```cpp
Session session() const;
```

导出当前对象状态。`id_token` 和 `refresh_token` 属于敏感凭证。

### 4.3 示例

```cpp
using namespace zzu_assistant;

app::AppClient client(app::ClientOptions{
    NetworkOptions{.ca_file = "ca-bundle.pem"},
    "stable-device-id",
});

auto result = client.login(app::LoginOptions{
    .username = "202500000000",
    .password = password,
    .notify = [](std::string_view text) { /* 显示状态 */ },
    .prompt = [](std::string_view text) { return read_sms_code(text); },
});
if (!result.success) {
    throw std::runtime_error(result.message);
}

const double balance = client.card_balance();
const app::Session snapshot = client.session();
```

## 5. CAS SSO API

头文件：`client/sso_client.h`  
命名空间：`zzu_assistant::sso`

### 5.1 类型

```cpp
enum class LoginStatus {
    success,
    no_session,
    rejected,
    captcha_required,
    mfa_rejected,
    mfa_expired,
    protocol_error,
    network_error,
};
```

| 状态 | 含义 |
|---|---|
| `success` | 登录、恢复或注销成功 |
| `no_session` | 对象无凭证或服务端会话已失效 |
| `rejected` | 用户名、密码或认证请求被拒绝 |
| `captcha_required` | 服务端要求当前 API 未处理的图片验证码 |
| `mfa_rejected` | MFA 验证失败或被拒绝 |
| `mfa_expired` | 二维码/MFA 等待超时或过期 |
| `protocol_error` | 参数、回调或响应协议不符合要求 |
| `network_error` | DNS、TLS、HTTP 或其他网络错误 |

```cpp
enum class MfaMethod {
    secure_phone,
    qr_code,
};
```

```cpp
struct LoginOptions {
    std::string_view username;
    std::string_view password;
    std::string_view service_url;
    MfaMethod mfa_method{MfaMethod::secure_phone};
    std::chrono::seconds mfa_timeout{120};
    std::function<void(std::string_view)> notify;
    std::function<std::string(std::string_view)> prompt;
    std::function<void(const std::vector<unsigned char>&)> display_qr;
};
```

- `service_url` 为空时只建立 CAS 会话；非空时登录后跳转到该服务。
- 手机 MFA 需要 `prompt` 回调。
- 二维码 MFA 需要 `display_qr`；参数是完整 PNG 文件字节，不是二维码 URL。
- QR 状态每两秒轮询一次，最长等待 `mfa_timeout`。

```cpp
struct LoginResult {
    LoginStatus status{LoginStatus::protocol_error};
    std::string message;
    std::string final_url;
    bool succeeded() const noexcept;
};
```

```cpp
struct Session {
    std::string username;
    std::string final_url;
    std::int64_t updated_at{};
    std::string cookies;
    std::string cas_cookie;
};
```

`cookies` 是 Client 的序列化 Cookie Jar，格式属于库内部实现；调用方应原样保存和
恢复，不应解析或修改。`cas_cookie` 可承载独立的 CAS Cookie。两者都是敏感凭证。
`updated_at` 是 Unix 秒时间戳。

### 5.2 `SsoClient`

```cpp
explicit SsoClient(NetworkOptions options = {});
LoginResult probe();
```

`probe()` 检查 SSO TLS、登录页和 RSA 公钥是否可用，不需要凭证。

```cpp
LoginResult login(const LoginOptions& options);
```

执行完整 CAS 登录，包括密码 RSA 加密及可选手机/二维码 MFA。

```cpp
LoginResult login(const Session& session);
```

不访问网络，直接恢复 Cookie。用户名为空，或 `cookies` 与 `cas_cookie` 均为空时
返回失败。

```cpp
LoginResult resume(std::string_view service_url = {});
```

使用对象内已有 Cookie 检查服务端会话，并可获取指定服务的最终跳转 URL。必须先
成功调用某个 `login` 重载。

```cpp
LoginResult logout();
```

请求 CAS 注销端点并清除对象内 Cookie。无当前会话时返回 `no_session`。

```cpp
Session session() const;
```

导出当前 SSO 会话快照。

### 5.3 恢复与服务跳转示例

```cpp
sso::SsoClient client;
auto restored = client.login(snapshot);
if (!restored.succeeded()) {
    throw std::runtime_error(restored.message);
}

auto result = client.resume("https://service.example.edu/");
if (result.succeeded()) {
    use_final_url(result.final_url);
}
```

## 6. 校园网 Portal API

头文件：`client/portal_client.h`  
命名空间：`zzu_assistant::portal`

### 6.1 类型

```cpp
struct PortalInfo {
    std::string auth_url;
    std::string server_url;
    std::string user_ip;
};

struct AuthOptions {
    std::string_view server_url;
    std::string_view user_ip;
    std::string_view username;
    std::string_view password;
    std::string_view isp_suffix;
    bool encrypt_parameters{false};
};

struct AuthResult {
    bool success{false};
    int result{};
    std::optional<int> return_code;
    std::string message;
};

struct Session {
    std::string username;
    std::string server_url;
    std::string user_ip;
    std::string isp_suffix;
};
```

`isp_suffix` 是拼接到用户名后的运营商后缀。`user_ip` 为空时尝试自动检测本机 IPv4。
`encrypt_parameters` 启用 Portal 兼容的 XOR 参数编码；它不是安全加密，不能替代
HTTPS。

### 6.2 `PortalClient`

```cpp
explicit PortalClient(NetworkOptions options = {});
PortalInfo discover();
```

通过连通性探测识别 Portal 劫持地址、认证服务器和用户 IP。网络已认证或响应不符合
Portal 格式时抛出 `std::runtime_error`。

```cpp
AuthResult login(const AuthOptions& options);
```

执行 Portal 登录。登录成功后在对象内保存登出所需的 `Session`。

```cpp
void login(const Session& session);
```

直接恢复登出所需状态，不访问网络。用户名或服务器 URL 为空时抛出
`std::invalid_argument`。

```cpp
AuthResult logout();
```

使用对象内 `Session` 发起 Portal 注销。未登录时返回 `success == false`。

```cpp
std::optional<Session> session() const;
static std::string local_ipv4();
```

`session()` 返回当前状态；未登录或成功注销后为 `std::nullopt`。
`local_ipv4()` 返回系统为外部 IPv4 路由选择的本机地址，检测失败时返回空字符串。

## 7. eCard、电费与校园卡充值 API

头文件：`client/ecard_client.h`、`model/ecard.h`、`model/electricity.h`  
命名空间：`zzu_assistant::ecard`

### 7.1 会话与登录

```cpp
struct Session {
    std::string username;
    std::string access_token;
    std::string refresh_token;
    std::string access_token_expire;
    ElectricityProfiles profiles;
};

explicit EcardClient(NetworkOptions options = {});
void login(std::string_view username,
           std::string_view super_app_id_token);
void login(const Session& session);
Session session() const;
```

第一个 `login` 使用 Super App `idToken` 换取 eCard 凭证。若对象已经通过
`login(Session)` 恢复了 access token，则不会重复交换。特殊登录不访问网络，要求
用户名和 access token 非空。Client 会在受保护请求收到未授权响应时尝试用 refresh
token 刷新一次。

### 7.2 房间与电量模型

```cpp
struct LocationPath {
    std::string big_area;
    std::string area;
    std::string building;
    std::string unit;
    std::string level;
    std::string room;
    std::string sub_area;
};

struct LocationOption {
    std::string id;
    std::string name;
};

struct LocationPage {
    std::string location_type;
    std::string next_location_type;
    bool end{false};
    std::vector<LocationOption> options;
};

struct ElectricityReading {
    double quantity_kwh{};
    double price_yuan_per_kwh{};
};

struct RechargeResult {
    bool success{false};
    std::string message;
};

struct ElectricityProfiles {
    LocationPath lighting;
    LocationPath air_conditioning;
};
```

`LocationPath` 中各字段保存服务端 ID，而不是显示名称。照明与空调通常是两个独立的
房间路径，不应假设它们只由奇偶 ID 区分。

### 7.3 房间与电费方法

```cpp
LocationPage locations(std::string_view location_type,
                       const LocationPath& path);
```

按层级查询位置选项。`location_type` 使用服务端层级名，例如 `area`、`building`、
`unit`、`level`、`room`；`path` 携带已选择的上级 ID。下一层名称由
`next_location_type` 返回。

```cpp
ElectricityReading account(const LocationPath& path);
```

查询指定照明或空调账户的剩余电量和单价。

```cpp
RechargeResult recharge(const LocationPath& path,
                        std::string_view payment_password,
                        unsigned amount_yuan);
```

创建并执行电费充值。金额必须为 1–1000 元，支付密码不能为空，且 `area`、
`building`、`level`、`room` 必须完整。密码仅作为本次调用参数使用。

```cpp
bool load_profiles(ElectricityProfiles& profiles) const;
void save_profiles(const ElectricityProfiles& profiles) const;
```

这两个方法只读写当前 Client 对象内的 profile 状态，不访问磁盘。`load_profiles`
仅在照明和空调 profile 均包含房间 ID 时返回 `true`。

### 7.4 校园卡充值模型与方法

```cpp
struct CampusCardRechargeConfig {
    double balance_yuan{};
    std::vector<unsigned> amounts_yuan;
};

struct CampusCardRechargeOrder {
    unsigned amount_yuan{};
    std::string checkout_url;
};
```

```cpp
CampusCardRechargeConfig campus_card_recharge_config();
CampusCardRechargeOrder campus_card_recharge(unsigned amount_yuan);
```

前者查询校园卡余额及服务端建议金额；后者创建 1–1000 元的支付订单并返回结算 URL，
不会等待用户在支付页面完成付款。

## 8. SSO 用户信息 API

头文件：`client/userinfo_client.h`、`model/userinfo.h`  
命名空间：`zzu_assistant::userinfo`、`zzu_assistant::model::userinfo`

```cpp
struct UserInfo {
    std::string username;
    std::string name;
    std::string identity_type_code;
    std::string identity_type_name;
    std::string organization_code;
    std::string organization_name;
    std::string account_id;
    std::string user_id;
    std::string uid;
    std::int64_t expires_at{};
};
```

`expires_at` 是身份 JWT 的 Unix 秒过期时间。

```cpp
UserInfoClient();

model::userinfo::UserInfo parse_sso_redirect(
    std::string_view final_url,
    std::string_view expected_username) const;
```

该 Client 不发起网络请求。它解析用户信息服务的 CAS 最终跳转 URL，校验允许的主机、
JWT 结构、issuer、audience、有效期和预期用户名，然后返回身份字段。URL、ticket、
JWT 或身份值不合法时抛出 `std::runtime_error`。

典型调用：

```cpp
sso::SsoClient sso;
if (!sso.login(saved_sso_session).succeeded()) {
    throw std::runtime_error("invalid SSO session");
}
auto redirect = sso.resume(model::userinfo::SERVICE_URL);
if (!redirect.succeeded()) {
    throw std::runtime_error(redirect.message);
}

userinfo::UserInfoClient parser;
auto info = parser.parse_sso_redirect(redirect.final_url, "202500000000");
```

`model::userinfo` 还公开以下协议常量：`PORTAL_HOST`、`SSO_ISSUER`、
`IDENTITY_ISSUER` 和 `SERVICE_URL`。它们描述当前郑大用户信息服务，可能随上游协议
变化，不建议复制其字面值。

## 9. 错误处理

- `AppClient::login` 和多数 SSO 操作使用结果对象报告认证失败。
- Portal/eCard/UserInfo 的参数、网络、TLS、HTTP、JSON 和协议错误通过
  `std::invalid_argument` 或 `std::runtime_error` 抛出。
- eCard 的 `RechargeResult::success == false` 表示服务端正常响应但充值业务失败。
- 所有标记为 `[[nodiscard]]` 的结果都应被检查。

调用方不应依赖 `message` 的固定文本进行分支；优先使用布尔值、`LoginStatus` 或
`return_code`。

## 10. 生命周期与安全

- `LoginOptions`、`AuthOptions` 中的 `std::string_view` 必须在对应同步调用结束前有效。
- 回调捕获的对象必须覆盖登录调用的整个生命周期。
- `Session`、支付密码、Cookie 和 token 都应按敏感数据处理。
- Client 不会替调用方加密落盘快照，也不会清理调用方持有的字符串副本。
- 不要在日志、异常包装或调试输出中打印 token、Cookie、密码或完整充值 URL。
- 一个 Client 实例只代表一个当前用户；切换用户应创建新实例，或明确调用新的
  `login(...)` 覆盖对象状态。

## 11. 不属于公共库的功能

以下内容属于 CLI 层，不随 `ZZUAssistant::Client` 导出：

- 环境变量读取与校验；
- 多用户当前账号选择；
- `cli::SessionStore` 及凭证文件格式；
- 交互式密码/验证码读取；
- 终端颜色、日志和二维码渲染；
- 各 `Service` 命令及 Registry；
- Python 课程表工具和自动充值脚本。

因此外部项目不应包含 `service/*`、`cli/*` 或 `auth/environment.h`，也不应依赖 CLI
状态目录中的 JSON 格式。
