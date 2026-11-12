.data
PUBLIC GateFixtureCalls
GateFixtureCalls dq 0, 0, 0, 0, 0

.code

GATE_TARGET MACRO name, slot, value
    ALIGN 16
    db 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh
name PROC
    mov qword ptr [rsp+8], rbx
    lock inc qword ptr [GateFixtureCalls + slot * 8]
    mov eax, value
    mov rbx, qword ptr [rsp+8]
    ret
name ENDP
    db 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh
ENDM

GATE_TARGET NVSDK_NGX_D3D12_CreateFeature, 0, 4660
GATE_TARGET NVSDK_NGX_D3D12_EvaluateFeature, 1, 4661
GATE_TARGET NVSDK_NGX_D3D12_ReleaseFeature, 2, 4662
GATE_TARGET NVSDK_NGX_D3D12_GetFeatureRequirements, 3, 0
GATE_TARGET NVSDK_NGX_D3D12_GetCapabilityParameters, 4, 0

END
