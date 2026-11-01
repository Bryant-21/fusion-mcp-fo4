#ifndef FUSIONMCP_PROVIDER_API_H
#define FUSIONMCP_PROVIDER_API_H

/* Project-owned GPL-3.0. C ABI v1; all text is length-delimited UTF-8.
 * Each pointer is borrowed for the duration of the call. No STL, allocator,
 * engine pointer, COM pointer, or ownership transfer crosses this interface.
 * DLL hot-unload is unsupported. See docs/PROVIDER_API.md for lifetime rules. */
/* Native providers are trusted code in this process. Capabilities and flags
 * authorize host dispatch; they do not sandbox a DLL or limit its OS access. */
#include <stdint.h>
/* Fix layout even when included inside a provider's other packed headers. */
#pragma pack(push, 8)
#ifdef __cplusplus
#define FUSIONMCP_NOEXCEPT noexcept
extern "C" {
#else
#define FUSIONMCP_NOEXCEPT
#endif
#ifdef _WIN32
#define FUSIONMCP_CALL __cdecl
#else
#define FUSIONMCP_CALL
#endif

#define FUSIONMCP_ABI_V1 1u
#define FUSIONMCP_MAX_ARGUMENT_BYTES 65536u
#define FUSIONMCP_MAX_RESULT_BYTES 262144u
#define FUSIONMCP_HOST_EXPORT "FUSIONMCP_GetHostAPI"

typedef uint64_t FUSIONMCP_ProviderHandle;
typedef uint32_t FUSIONMCP_Status;
#define FUSIONMCP_OK 0u
#define FUSIONMCP_INVALID_ARGUMENT 1u
#define FUSIONMCP_INCOMPATIBLE 2u
#define FUSIONMCP_UNAVAILABLE 3u
#define FUSIONMCP_BUSY 4u
#define FUSIONMCP_INTERNAL_FAILURE 5u
#define FUSIONMCP_WRITER_FAILED 6u
#define FUSIONMCP_PENDING 7u

typedef uint32_t FUSIONMCP_Owner;
#define FUSIONMCP_OWNER_GAME 1u
#define FUSIONMCP_OWNER_RENDER 2u
#define FUSIONMCP_OWNER_UI 3u
#define FUSIONMCP_OWNER_VM 4u

typedef uint32_t FUSIONMCP_CommandFlags;
#define FUSIONMCP_COMMAND_MUTATING 1u
#define FUSIONMCP_COMMAND_ASYNC 2u

/* Synchronous, at most once; sink copies before returning. 1 means copied. */
typedef uint32_t (FUSIONMCP_CALL *FUSIONMCP_WriteFn)(void* sink,
    const char* utf8, uint32_t bytes) FUSIONMCP_NOEXCEPT;

typedef struct FUSIONMCP_CommandV1 {
    uint32_t version;
    uint32_t size;
    const char* name;             /* local name, e.g. settings.get; no namespace */
    uint32_t name_bytes;
    const char* description;
    uint32_t description_bytes;
    const char* input_schema;     /* bounded JSON Schema object */
    uint32_t input_schema_bytes;
    FUSIONMCP_Owner owner;
    FUSIONMCP_CommandFlags flags;
    uint64_t required_capabilities;
} FUSIONMCP_CommandV1;

typedef struct FUSIONMCP_InvocationV1 {
    uint32_t version;
    uint32_t size;
    uint64_t request_id;
    uint64_t load_epoch;
    uint32_t remaining_timeout_ms;
    uint32_t reserved;            /* zero */
    const char* command;          /* local command name */
    uint32_t command_bytes;
    const char* arguments_json;
    uint32_t arguments_bytes;
} FUSIONMCP_InvocationV1;

typedef FUSIONMCP_Status (FUSIONMCP_CALL *FUSIONMCP_InvokeFn)(void* context,
    const FUSIONMCP_InvocationV1* invocation, FUSIONMCP_WriteFn writer,
    void* sink) FUSIONMCP_NOEXCEPT;

typedef struct FUSIONMCP_OperationV1 {
    uint32_t version;
    uint32_t size;
    uint64_t request_id;
    uint64_t load_epoch;          /* epoch at start, also supplied to cancel */
    uint32_t remaining_timeout_ms;
    uint32_t reserved;
    const char* operation_id;     /* copied opaque UTF-8 id, 1..128 bytes */
    uint32_t operation_id_bytes;
    uint32_t reserved2;
} FUSIONMCP_OperationV1;

typedef FUSIONMCP_Status (FUSIONMCP_CALL *FUSIONMCP_PollFn)(void* context,
    const FUSIONMCP_OperationV1*, FUSIONMCP_WriteFn writer, void* sink) FUSIONMCP_NOEXCEPT;
typedef FUSIONMCP_Status (FUSIONMCP_CALL *FUSIONMCP_CancelFn)(void* context,
    const FUSIONMCP_OperationV1*) FUSIONMCP_NOEXCEPT;

typedef struct FUSIONMCP_ProviderV1 {
    uint32_t version;
    uint32_t size;
    const char* name_space;       /* f4fs or tales; fusionmcp is reserved */
    uint32_t namespace_bytes;
    uint64_t capabilities;
    void* context;                /* opaque, used only by provider callbacks */
    const FUSIONMCP_CommandV1* commands;
    uint32_t command_count;
    uint32_t reserved;            /* zero */
    FUSIONMCP_InvokeFn invoke;
    /* Required when any command has ASYNC. Poll/cancel run on that command's
     * owner. No callback blocks awaiting game/render/UI/VM progress. */
    FUSIONMCP_PollFn poll;
    FUSIONMCP_CancelFn cancel;
} FUSIONMCP_ProviderV1;

typedef struct FUSIONMCP_HostV1 {
    uint32_t version;
    uint32_t size;
    FUSIONMCP_Status (FUSIONMCP_CALL *register_provider)(const FUSIONMCP_ProviderV1*,
        FUSIONMCP_ProviderHandle*) FUSIONMCP_NOEXCEPT;
    FUSIONMCP_Status (FUSIONMCP_CALL *set_enabled)(FUSIONMCP_ProviderHandle,
        uint32_t enabled) FUSIONMCP_NOEXCEPT;
    FUSIONMCP_Status (FUSIONMCP_CALL *unregister_provider)(FUSIONMCP_ProviderHandle) FUSIONMCP_NOEXCEPT;
} FUSIONMCP_HostV1;

typedef const FUSIONMCP_HostV1* (FUSIONMCP_CALL *FUSIONMCP_GetHostAPIFn)(
    uint32_t requested_version, uint32_t caller_size) FUSIONMCP_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#pragma pack(pop)
#endif
