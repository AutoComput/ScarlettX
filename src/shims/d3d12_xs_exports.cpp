#include <cstdint>

#include "scarlettx/gpu/d3d12_xs_abi.h"

extern "C" {

SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12CreateDevice(
    void* pAdapter,
    uint32_t minimumFeatureLevel,
    const void* riid,
    void** ppDevice) {
    return D3D12XsCreateDevice(pAdapter, minimumFeatureLevel, riid, ppDevice);
}

SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12GetDebugInterface(
    const void* riid,
    void** ppvDebug) {
    return D3D12XsGetDebugInterface(riid, ppvDebug);
}

SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12SerializeRootSignature(
    const void* pRootSignature,
    uint32_t version,
    void** ppBlob,
    void** ppErrorBlob) {
    return D3D12XsSerializeRootSignature(pRootSignature, version, ppBlob, ppErrorBlob);
}

SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12SerializeVersionedRootSignature(
    const void* pRootSignature,
    void** ppBlob,
    void** ppErrorBlob) {
    return D3D12XsSerializeVersionedRootSignature(pRootSignature, ppBlob, ppErrorBlob);
}

SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12CreateRootSignatureDeserializer(
    const void* pSrcData,
    size_t srcDataSizeInBytes,
    const void* pRootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer) {
    return D3D12XsCreateRootSignatureDeserializer(
        pSrcData, srcDataSizeInBytes, pRootSignatureDeserializerInterface,
        ppRootSignatureDeserializer);
}

}  // extern "C"
