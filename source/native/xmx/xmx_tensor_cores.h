#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <string>
#include "xmx_context.h"
#include "xess/cuda/cuda_route.h"

namespace xmx::tensor_cores
{
using context::ExecuteParams;
using context::InitParams;

enum class Setting { Undecided, On, Off };

void Configure(bool on) noexcept;
bool Create(HMODULE fg, const Rvas& rvas, ID3D12Device* device, void*& handle, std::string& failure) noexcept;
int Initialize(HMODULE fg, const Rvas& rvas, void* handle, InitParams& init) noexcept;
int Execute(HMODULE fg, const Rvas& rvas, void* handle, ID3D12GraphicsCommandList* list,
    ExecuteParams& execute) noexcept;
std::string StatusText(const xefg_cuda::Status& status, Setting setting);
std::string SummaryText();
}
