#include "zzuassistant/ffi.h"

#include <cstring>

int main() {
    if (std::strcmp(zzu_ffi_version(), "0.1.0") != 0) return 1;

    zzu_app_client *client = zzu_app_create(nullptr, "ffi-smoke-device");
    if (client == nullptr) return 2;

    char *session = zzu_app_session(client);
    if (session == nullptr || std::strstr(session, "\"username\"") == nullptr) {
        zzu_string_free(session);
        zzu_app_destroy(client);
        return 3;
    }
    zzu_string_free(session);
    zzu_app_destroy(client);

    if (zzu_app_session(nullptr) != nullptr) return 4;
    if (zzu_last_error() == nullptr || *zzu_last_error() == '\0') return 5;
    return 0;
}
