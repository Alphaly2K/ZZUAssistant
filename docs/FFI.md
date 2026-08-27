# ZZUAssistant FFI

`ZZUAssistantFFI` 是面向其他语言的稳定 C ABI 胶水层。公开头文件为
`<zzuassistant/ffi.h>`，Windows 产物为 `ZZUAssistantFFI.dll`，Linux/macOS 分别为
`libZZUAssistantFFI.so` 和 `libZZUAssistantFFI.dylib`。

底层 C++ Client 的完整语义见 [API.md](API.md)。

## 构建和链接

FFI 默认启用：

```bash
cmake -S . -B build -DZZU_BUILD_FFI=ON
cmake --build build
```

在 CMake 项目中可链接：

```cmake
target_link_libraries(your_native_binding PRIVATE ZZUAssistant::FFI)
```

只需要 C++ 静态库时，可使用 `-DZZU_BUILD_FFI=OFF`。

## ABI 约定

- 所有输入和返回文本均为 UTF-8。
- Client 使用不透明指针，例如 `zzu_app_client*`。
- `create` 返回的句柄必须交给对应的 `destroy`，`destroy(NULL)` 安全。
- 返回类型为 `char*` 的函数返回 JSON。失败时返回 `NULL`。
- 非空返回字符串必须且只能用 `zzu_string_free()` 释放。
- 返回 `int` 的函数以 `0` 表示成功、`-1` 表示失败。
- `zzu_last_error()` 返回当前线程最近一次 FFI 错误；指针由库持有，不得释放。
- 一次成功的 FFI 调用会清空当前线程的错误文本。
- 认证被服务端正常拒绝时通常仍返回非空 JSON，通过其中的 `success` 或 `status`
  判断；只有异常和 ABI 参数错误才返回 `NULL`/`-1`。
- 同一 Client 句柄不保证线程安全。

可用 `zzu_ffi_version()` 获取 ABI 实现版本，当前返回 `0.1.0`。

## 回调

```c
typedef void (*zzu_notify_callback)(const char *message, void *user_data);
typedef const char *(*zzu_prompt_callback)(const char *prompt, void *user_data);
typedef void (*zzu_qr_callback)(const uint8_t *png, size_t size,
                                void *user_data);
```

- 回调均在发起登录的线程中同步调用。
- `message`、`prompt` 和 PNG 字节只在回调执行期间有效。
- `prompt` 返回的 UTF-8 字符串会在回调返回后立即复制；绑定层至少要保证它在
  回调返回瞬间仍有效。
- `zzu_qr_callback` 收到的是完整 PNG 文件，可保存、显示或交给二维码组件解码。
- 托管语言必须在整个登录调用期间固定回调对象，防止垃圾回收。
- 不需要的回调传 `NULL`。手机 MFA 缺少 `prompt`、QR MFA 缺少 `display_qr` 时，
  登录结果会报告协议错误。

SSO 的 `mfa_method`：`0` 为手机短信，`1` 为二维码。`mfa_timeout_seconds == 0`
使用默认 120 秒。

## JSON 结构

### 通用认证结果

App：

```json
{"success":true,"message":"..."}
```

SSO：

```json
{
  "status":"success",
  "success":true,
  "message":"...",
  "final_url":"https://..."
}
```

`status` 可能为 `success`、`no_session`、`rejected`、`captcha_required`、
`mfa_rejected`、`mfa_expired`、`protocol_error`、`network_error`。

Portal：

```json
{"success":true,"result":1,"return_code":null,"message":"..."}
```

### 会话

App：

```json
{
  "username":"202500000000",
  "device_id":"...",
  "id_token":"...",
  "refresh_token":"..."
}
```

SSO：

```json
{
  "username":"202500000000",
  "final_url":"https://...",
  "updated_at":1780000000,
  "cookies":"...",
  "cas_cookie":"..."
}
```

Portal：

```json
{
  "username":"202500000000",
  "server_url":"http://172.16.4.14:801",
  "user_ip":"10.0.0.2",
  "isp_suffix":""
}
```

未登录的 Portal 会话为 JSON `null`。

eCard：

```json
{
  "username":"202500000000",
  "access_token":"...",
  "refresh_token":"...",
  "access_token_expire":"...",
  "profiles":{
    "lighting":{"big_area":"","area":"99","building":"97","unit":"","level":"190","room":"...","sub_area":""},
    "air_conditioning":{"big_area":"","area":"99","building":"97","unit":"","level":"191","room":"...","sub_area":""}
  }
}
```

会话 JSON 含敏感凭证。FFI 不会读写磁盘，也不会替调用方加密。

### `LocationPath`

`zzu_ecard_locations`、`zzu_ecard_account` 和 `zzu_ecard_recharge` 的
`path_json` 参数使用：

```json
{
  "big_area":"",
  "area":"99",
  "building":"97",
  "unit":"",
  "level":"190",
  "room":"123",
  "sub_area":""
}
```

缺少的字段按空字符串处理。

## 函数分组

### App

```c
zzu_app_client *zzu_app_create(const char *ca_file, const char *device_id);
void zzu_app_destroy(zzu_app_client *client);
char *zzu_app_login(...);
int zzu_app_restore(zzu_app_client *client, const char *session_json);
char *zzu_app_logout(zzu_app_client *client);
char *zzu_app_id_token(zzu_app_client *client);
char *zzu_app_card_balance(zzu_app_client *client);
char *zzu_app_session(zzu_app_client *client);
```

`zzu_app_id_token` 返回 JSON 字符串，例如 `"eyJ..."`；不是裸文本。
`zzu_app_card_balance` 返回 `{"balance_yuan":12.34}`。

### SSO

```c
zzu_sso_client *zzu_sso_create(const char *ca_file);
void zzu_sso_destroy(zzu_sso_client *client);
char *zzu_sso_probe(zzu_sso_client *client);
char *zzu_sso_login(...);
int zzu_sso_restore(zzu_sso_client *client, const char *session_json);
char *zzu_sso_resume(zzu_sso_client *client, const char *service_url);
char *zzu_sso_logout(zzu_sso_client *client);
char *zzu_sso_session(zzu_sso_client *client);
```

### Portal

```c
zzu_portal_client *zzu_portal_create(const char *ca_file);
void zzu_portal_destroy(zzu_portal_client *client);
char *zzu_portal_discover(zzu_portal_client *client);
char *zzu_portal_login(...);
int zzu_portal_restore(zzu_portal_client *client, const char *session_json);
char *zzu_portal_logout(zzu_portal_client *client);
char *zzu_portal_session(zzu_portal_client *client);
char *zzu_portal_local_ipv4(void);
```

`discover` 返回 `auth_url`、`server_url`、`user_ip`。`local_ipv4` 返回 JSON
字符串。

### eCard

```c
zzu_ecard_client *zzu_ecard_create(const char *ca_file);
void zzu_ecard_destroy(zzu_ecard_client *client);
int zzu_ecard_login(...);
int zzu_ecard_restore(...);
char *zzu_ecard_session(...);
char *zzu_ecard_locations(...);
char *zzu_ecard_account(...);
char *zzu_ecard_recharge(...);
char *zzu_ecard_recharge_config(...);
char *zzu_ecard_create_recharge_order(...);
char *zzu_ecard_profiles(...);
int zzu_ecard_set_profiles(...);
```

- `locations` 返回当前/下一层名称、`end` 和 `{id,name}` 数组。
- `account` 返回 `quantity_kwh`、`price_yuan_per_kwh`。
- `recharge_config` 返回 `balance_yuan`、`amounts_yuan`。
- `create_recharge_order` 返回 `amount_yuan`、`checkout_url`。
- `profiles` 在照明/空调路径不完整时返回 JSON `null`。

### UserInfo

```c
zzu_userinfo_client *zzu_userinfo_create(void);
void zzu_userinfo_destroy(zzu_userinfo_client *client);
char *zzu_userinfo_parse(zzu_userinfo_client *client,
                         const char *final_url,
                         const char *expected_username);
```

返回字段：`username`、`name`、`identity_type_code`、`identity_type_name`、
`organization_code`、`organization_name`、`account_id`、`user_id`、`uid`、
`expires_at`。

## Python `ctypes` 示例

```python
import ctypes
import json

lib = ctypes.CDLL("ZZUAssistantFFI.dll")

lib.zzu_app_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
lib.zzu_app_create.restype = ctypes.c_void_p
lib.zzu_app_session.argtypes = [ctypes.c_void_p]
lib.zzu_app_session.restype = ctypes.c_void_p
lib.zzu_app_destroy.argtypes = [ctypes.c_void_p]
lib.zzu_string_free.argtypes = [ctypes.c_void_p]
lib.zzu_last_error.restype = ctypes.c_char_p

client = lib.zzu_app_create(None, b"stable-device-id")
if not client:
    raise RuntimeError(lib.zzu_last_error().decode("utf-8"))

try:
    pointer = lib.zzu_app_session(client)
    if not pointer:
        raise RuntimeError(lib.zzu_last_error().decode("utf-8"))
    try:
        session = json.loads(ctypes.string_at(pointer).decode("utf-8"))
    finally:
        lib.zzu_string_free(pointer)
finally:
    lib.zzu_app_destroy(client)
```

完整函数原型应始终以 [ffi.h](../include/zzuassistant/ffi.h) 为准。
