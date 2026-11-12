# Third-party notices

## Intel XeSS Frame Generation program data

MFGUnlocker.dll embeds compute shader programs (DXIL) that run the multi-frame model of Intel XeSS Frame Generation on
non-Intel GPUs. 46 of them are derived from programs in Intel's XeSS SDK (`libxess_fg.dll`) and were modified by this
project; the others are this project's own.

For XeMFG on Vulkan it also embeds SPIR-V translations of the programs XeSS Frame Generation creates there: 83
programs, 57 of them translated from programs in Intel's XeSS SDK, the others from this project's own programs.

The Intel-derived program data is Intel's work. It is not covered by this project's MIT License and remains subject to
Intel's licence terms for the XeSS SDK. MFG Unlocker does not include Intel's XeSS SDK binaries, model weights or tensors;
it uses the XeSS Frame Generation copy that comes with the game or OptiScaler.

Intel and XeSS are trademarks of Intel Corporation or its subsidiaries.

## MFG Ada Unlock, XMX generator

The XMX frame generation backend (`source/native/xmx/`) is derived from the XMX generator of MFGAdaUnlock-RenoDx 1.9.0
by mavismmg (https://github.com/mavismmg/MFGAdaUnlock-RenoDx). That code is used under the MIT License:

```
MIT License

Copyright (c) 2026 ImDreamt

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Other components

MFGUnlocker.dll also carries the licences of Dear ImGui, MinHook and Ultimate ASI Loader, which retain their own terms.
