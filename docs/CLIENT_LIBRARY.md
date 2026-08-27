# Client 库

完整接口参考见 [API.md](API.md)。
其他语言调用见 [FFI.md](FFI.md)。

`ZZUAssistant::Client` 是静态 C++23 库，只包含可复用的认证和业务客户端，不包含
CLI、service 或二维码渲染。依赖 Boost headers、OpenSSL 和 Threads。

## 引入

源码集成：

```cmake
set(ZZU_BUILD_CLI OFF CACHE BOOL "" FORCE)
add_subdirectory(path/to/ZZUAssistant)
target_link_libraries(your_target PRIVATE ZZUAssistant::Client)
```

安装后集成：

```cmake
find_package(ZZUAssistant CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE ZZUAssistant::Client)
```

```cpp
#include <zzuassistant/client.h>
```

## 接口

| 类                          | 用途              | 认证        |
|----------------------------|-----------------|-----------|
| `app::AppClient`           | 超级 App 登录、校园卡余额 | App       |
| `sso::SsoClient`           | CAS 登录、恢复和退出    | SSO       |
| `portal::PortalClient`     | 校园网发现、登录和退出     | Portal    |
| `ecard::EcardClient`       | 校园卡、电费查询和充值     | App/eCard |
| `userinfo::UserInfoClient` | 解析经 CAS 验证的用户信息 | SSO       |

所有类位于 `zzu_assistant` 命名空间。Client 不读取环境变量、不读写磁盘；CA
证书路径和设备 ID 通过构造参数传入。登录凭证只保存在对象内存中。

每个认证 Client 都提供两类登录入口：正常登录用于获取新凭证，`login(Session)`
用于恢复调用方已有的凭证。调用 `session()` 可取得快照，是否持久化由调用方决定。
登录回调同步执行，网络或协议错误可能抛出 `std::exception`。

## 示例

```cpp
#include <zzuassistant/client.h>
#include <iostream>

int main() {
    using namespace zzu_assistant;

    app::AppClient app(app::ClientOptions{
        NetworkOptions{.ca_file = "ca-bundle.pem"},
        "caller-managed-device-id",
    });
    app.login(app::Session{
        .username = "202500000000",
        .device_id = "caller-managed-device-id",
        .id_token = "existing-id-token",
        .refresh_token = "existing-refresh-token",
    });

    std::cout << app.card_balance() << '\n';

    ecard::EcardClient ecard({.ca_file = "ca-bundle.pem"});
    ecard.login("202500000000", app.id_token());
    const auto config = ecard.campus_card_recharge_config();
    std::cout << config.balance_yuan << '\n';
}
```

CLI 中的环境变量解析、多用户选择和会话文件由 CLI 层的 `SessionStore`
处理，不属于 `ZZUAssistant::Client`。
