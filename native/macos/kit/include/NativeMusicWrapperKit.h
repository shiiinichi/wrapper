#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct NMWRuntime NMWRuntime;
typedef struct NMWOperation NMWOperation;
typedef struct NMWWorker NMWWorker;
typedef struct NMWLogin NMWLogin;
typedef struct {
    uint8_t *bytes;
    size_t size;
} NMWBytes;
typedef struct {
    int32_t status;
    NMWBytes metadata;
    NMWBytes binary;
} NMWReply;
enum {
    NMW_OK = 0,
    NMW_INVALID = 1,
    NMW_UNAVAILABLE = 2,
    NMW_REJECTED = 3,
    NMW_OVERSIZED = 4,
    NMW_BUSY = 5,
    NMW_CANCELLED = 6,
    NMW_TIMEOUT = 7,
    NMW_MESSAGE = 8,
    NMW_NO_ACCOUNT = 9,
    NMW_NOT_FOUND = 10,
    NMW_HTTP_BASE = 1000
};
enum { NMW_WORKER = 1, NMW_HTTP = 2, NMW_GENERATION = 3, NMW_CLOSE_WORKER = 4 };
// Optional external-boundary hooks for deterministic integration tests.
// Returned buffers must be allocated with nmw_copy_bytes. Production uses the
// C++ private-pipe worker and HTTPS client when hooks are null.
typedef int32_t (*NMWExternal)(void *context, int32_t kind, const NMWOperation *operation, const uint8_t *input,
                               size_t input_size, NMWBytes *output);
NMWRuntime *nmw_create(const char *binary, const char *assets, const char *profile, void *context,
                       NMWExternal hook);
void nmw_destroy(NMWRuntime *runtime);
NMWOperation *nmw_operation_create(void);
void nmw_operation_cancel(NMWOperation *operation);
int nmw_operation_cancelled(const NMWOperation *operation);
void nmw_operation_destroy(NMWOperation *operation);
// /profile reads the active account's cached Apple Music display metadata;
// query {"refresh":"true"} explicitly requests a bounded SDK refresh.
NMWReply nmw_send(NMWRuntime *runtime, NMWOperation *operation, const char *route, const char *query_json,
                  const uint8_t *body, size_t body_size, const char *key_context);
// Reserve the account while a host presents sign-in, or replace it from a
// stopped profile. A released account invalidates cached tokens and keys.
int32_t nmw_begin_account(NMWRuntime *runtime, NMWOperation *operation);
void nmw_end_account(NMWRuntime *runtime);
// Wait for admitted work to drain, then close the worker before returning.
void nmw_close_worker(NMWRuntime *runtime);
size_t nmw_pending_requests(NMWRuntime *runtime);
int32_t nmw_import_profile(const char *source, const char *destination);
NMWBytes nmw_copy_bytes(const uint8_t *bytes, size_t size);
void nmw_free_bytes(NMWBytes bytes);
int32_t nmw_sample_sizes(const uint8_t *data, size_t size, NMWBytes *sizes);
int nmw_permits_redirect(const char *source, const char *destination);
int nmw_valid_challenge(const char *challenge, const char *uri, const char *drm);
NMWWorker *nmw_worker_create(const char *binary, const char *assets, const char *profile);
NMWReply nmw_worker_exchange(NMWWorker *worker, NMWOperation *operation, const uint8_t *data, size_t size);
void nmw_worker_close(NMWWorker *worker);
void nmw_worker_destroy(NMWWorker *worker);
typedef int32_t (*NMWAuthPromptCallback)(void *context, const uint8_t *prompt_json, size_t size);
NMWLogin *nmw_login_create(const char *binary, const char *assets, const char *profile, const char *operation);
int32_t nmw_login_run(NMWLogin *login, void *context, NMWAuthPromptCallback callback);
void nmw_login_submit(NMWLogin *login, const uint8_t *answer, size_t size);
void nmw_login_cancel(NMWLogin *login);
void nmw_login_destroy(NMWLogin *login);
#ifdef __cplusplus
}
#endif
