#pragma once

#include <cstdint>

#if defined(_WIN32)
#if defined(SCARLETTX_BUILDING_ACPHAL)
#define SCARLETTX_ACP_API __declspec(dllexport)
#else
#define SCARLETTX_ACP_API __declspec(dllimport)
#endif
#define SCARLETTX_ACP_CALL __stdcall
#else
#define SCARLETTX_ACP_API __attribute__((visibility("default")))
#define SCARLETTX_ACP_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct APU_HEAP_STATE {
    uint32_t bytesFree;
    uint32_t bytesAllocated;
    uint32_t bytesLost;
    uint32_t maximumBlockSizeAvailable;
    uint32_t allocationCount;
} APU_HEAP_STATE;

typedef struct ACP_SHAPE_CONTEXT_COUNTS {
    uint32_t numSrcContexts;
    uint32_t numEqCompContexts;
    uint32_t numFiltVolContexts;
    uint32_t numDmaContexts;
    uint32_t numXmaContexts;
    uint32_t numPcmContexts;
} ACP_SHAPE_CONTEXT_COUNTS;

// All 10 verified exports from EmbeddedXvd/Windows/System32/AcpHal.dll
SCARLETTX_ACP_API int32_t SCARLETTX_ACP_CALL AcpHalAllocateShapeContexts(
    const ACP_SHAPE_CONTEXT_COUNTS* numContexts);

SCARLETTX_ACP_API int32_t SCARLETTX_ACP_CALL AcpHalCreate(
    void** acpHal);

SCARLETTX_ACP_API int32_t SCARLETTX_ACP_CALL AcpHalReleaseShapeContexts(void);

SCARLETTX_ACP_API int32_t SCARLETTX_ACP_CALL ApuAlloc(
    void** virtualAddress,
    uint32_t* physicalAddress,
    uint32_t sizeInBytes,
    uint32_t alignmentInBytes,
    uint32_t flags);

SCARLETTX_ACP_API int32_t SCARLETTX_ACP_CALL ApuCreateHeap(
    uint32_t cachedSizeInBytes,
    uint32_t nonCachedSizeInBytes);

SCARLETTX_ACP_API int32_t SCARLETTX_ACP_CALL ApuFree(
    void* virtualAddress);

SCARLETTX_ACP_API int32_t SCARLETTX_ACP_CALL ApuHeapGetState(
    APU_HEAP_STATE* apuHeapState,
    uint32_t flags);

SCARLETTX_ACP_API int32_t SCARLETTX_ACP_CALL ApuIsVirtualAddressValid(
    const void* virtualAddress,
    uint32_t sizeInBytes);

SCARLETTX_ACP_API void* SCARLETTX_ACP_CALL ApuMapApuAddress(
    uint32_t apuPhysicalAddress);

SCARLETTX_ACP_API uint32_t SCARLETTX_ACP_CALL ApuMapVirtualAddress(
    const void* virtualAddress);

#ifdef __cplusplus
}
#endif
