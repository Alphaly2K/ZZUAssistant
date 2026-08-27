#ifndef ZZUASSISTANT_FFI_H
#define ZZUASSISTANT_FFI_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(ZZUASSISTANT_FFI_BUILD)
#    define ZZU_FFI_API __declspec(dllexport)
#  else
#    define ZZU_FFI_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define ZZU_FFI_API __attribute__((visibility("default")))
#else
#  define ZZU_FFI_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zzu_app_client zzu_app_client;
typedef struct zzu_sso_client zzu_sso_client;
typedef struct zzu_portal_client zzu_portal_client;
typedef struct zzu_ecard_client zzu_ecard_client;
typedef struct zzu_userinfo_client zzu_userinfo_client;

typedef void (*zzu_notify_callback)(const char *message, void *user_data);
typedef const char *(*zzu_prompt_callback)(const char *prompt, void *user_data);
typedef void (*zzu_qr_callback)(const uint8_t *png, size_t size,
                                void *user_data);

/* Returned strings are UTF-8 and must be released with zzu_string_free(). */
ZZU_FFI_API const char *zzu_ffi_version(void);
ZZU_FFI_API const char *zzu_last_error(void);
ZZU_FFI_API void zzu_string_free(char *value);

ZZU_FFI_API zzu_app_client *zzu_app_create(const char *ca_file,
                                           const char *device_id);
ZZU_FFI_API void zzu_app_destroy(zzu_app_client *client);
ZZU_FFI_API char *zzu_app_login(zzu_app_client *client,
                                const char *username, const char *password,
                                zzu_notify_callback notify,
                                zzu_prompt_callback prompt, void *user_data);
ZZU_FFI_API int zzu_app_restore(zzu_app_client *client,
                                const char *session_json);
ZZU_FFI_API char *zzu_app_logout(zzu_app_client *client);
ZZU_FFI_API char *zzu_app_id_token(zzu_app_client *client);
ZZU_FFI_API char *zzu_app_card_balance(zzu_app_client *client);
ZZU_FFI_API char *zzu_app_session(zzu_app_client *client);

ZZU_FFI_API zzu_sso_client *zzu_sso_create(const char *ca_file);
ZZU_FFI_API void zzu_sso_destroy(zzu_sso_client *client);
ZZU_FFI_API char *zzu_sso_probe(zzu_sso_client *client);
ZZU_FFI_API char *zzu_sso_login(
    zzu_sso_client *client, const char *username, const char *password,
    const char *service_url, int mfa_method, unsigned mfa_timeout_seconds,
    zzu_notify_callback notify, zzu_prompt_callback prompt,
    zzu_qr_callback display_qr, void *user_data);
ZZU_FFI_API int zzu_sso_restore(zzu_sso_client *client,
                                const char *session_json);
ZZU_FFI_API char *zzu_sso_resume(zzu_sso_client *client,
                                 const char *service_url);
ZZU_FFI_API char *zzu_sso_logout(zzu_sso_client *client);
ZZU_FFI_API char *zzu_sso_session(zzu_sso_client *client);

ZZU_FFI_API zzu_portal_client *zzu_portal_create(const char *ca_file);
ZZU_FFI_API void zzu_portal_destroy(zzu_portal_client *client);
ZZU_FFI_API char *zzu_portal_discover(zzu_portal_client *client);
ZZU_FFI_API char *zzu_portal_login(
    zzu_portal_client *client, const char *server_url, const char *user_ip,
    const char *username, const char *password, const char *isp_suffix,
    int encrypt_parameters);
ZZU_FFI_API int zzu_portal_restore(zzu_portal_client *client,
                                   const char *session_json);
ZZU_FFI_API char *zzu_portal_logout(zzu_portal_client *client);
ZZU_FFI_API char *zzu_portal_session(zzu_portal_client *client);
ZZU_FFI_API char *zzu_portal_local_ipv4(void);

ZZU_FFI_API zzu_ecard_client *zzu_ecard_create(const char *ca_file);
ZZU_FFI_API void zzu_ecard_destroy(zzu_ecard_client *client);
ZZU_FFI_API int zzu_ecard_login(zzu_ecard_client *client,
                                const char *username,
                                const char *super_app_id_token);
ZZU_FFI_API int zzu_ecard_restore(zzu_ecard_client *client,
                                  const char *session_json);
ZZU_FFI_API char *zzu_ecard_session(zzu_ecard_client *client);
ZZU_FFI_API char *zzu_ecard_locations(zzu_ecard_client *client,
                                      const char *location_type,
                                      const char *path_json);
ZZU_FFI_API char *zzu_ecard_account(zzu_ecard_client *client,
                                    const char *path_json);
ZZU_FFI_API char *zzu_ecard_recharge(zzu_ecard_client *client,
                                     const char *path_json,
                                     const char *payment_password,
                                     unsigned amount_yuan);
ZZU_FFI_API char *zzu_ecard_recharge_config(zzu_ecard_client *client);
ZZU_FFI_API char *zzu_ecard_create_recharge_order(
    zzu_ecard_client *client, unsigned amount_yuan);
ZZU_FFI_API char *zzu_ecard_profiles(zzu_ecard_client *client);
ZZU_FFI_API int zzu_ecard_set_profiles(zzu_ecard_client *client,
                                       const char *profiles_json);

ZZU_FFI_API zzu_userinfo_client *zzu_userinfo_create(void);
ZZU_FFI_API void zzu_userinfo_destroy(zzu_userinfo_client *client);
ZZU_FFI_API char *zzu_userinfo_parse(zzu_userinfo_client *client,
                                     const char *final_url,
                                     const char *expected_username);

#ifdef __cplusplus
}
#endif

#endif
