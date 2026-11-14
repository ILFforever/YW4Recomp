"""YW4 Recomp's patch to FidelityFX SDK v1.1.4's ffx-api (the five-function API, decision 0073): builds on Linux,
with only the FSR 3 upscaler's provider (FFX_UPSCALE_ONLY), and its headers usable from C (runtime/src/gpu_fsr.c).
Edits <ffx-api dir> in place; tools/yw4/fsr_linux.py runs it on a fresh copy. Each edit must match exactly once, so
a different SDK stops here instead of building wrongly.

    python3 yw4_ffx_api_linux.py <ffx-api dir>
"""
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])


def edit(rel, pairs):
    p = root / rel
    s = p.read_text()
    for a, b in pairs:
        if s.count(a) != 1:
            raise SystemExit(f"{rel}: {s.count(a)} matches for {a[:60]!r}")
        s = s.replace(a, b)
    p.write_text(s)


edit("include/ffx_api/ffx_api.h", [(
    "#define FFX_API_ENTRY __declspec(dllexport)",
    "#if defined(_WIN32)\n#define FFX_API_ENTRY __declspec(dllexport)\n#else\n"
    "#define FFX_API_ENTRY __attribute__((visibility(\"default\")))\n#endif",
)])

edit("src/ffx_provider.cpp", [
    ("#include <d3d12.h>\n", "#ifdef _WIN32\n#include <d3d12.h>\n#endif\n"),
    ("""    &ffxProvider_FSR3Upscale::Instance,
    &ffxProvider_FSR2::Instance,
    &ffxProvider_FrameGeneration::Instance,
""", """    &ffxProvider_FSR3Upscale::Instance,
#ifndef FFX_UPSCALE_ONLY
    &ffxProvider_FSR2::Instance,
    &ffxProvider_FrameGeneration::Instance,
#endif
"""),
    ("""#ifdef FFX_BACKEND_VK
    &ffxProvider_FrameGenerationSwapChain_VK::Instance,
#endif // FFX_BACKEND_VK
};""", """#if defined(FFX_BACKEND_VK) && !defined(FFX_UPSCALE_ONLY)
    &ffxProvider_FrameGenerationSwapChain_VK::Instance,
#endif // FFX_BACKEND_VK
};"""),
    ("static constexpr size_t providerCount = _countof(providers);",
     "static constexpr size_t providerCount = sizeof(providers) / sizeof(providers[0]);"),
    ("MIDL_INTERFACE(\"b58d6601-7401-4234-8180-6febfc0e484c\")", "#ifdef _WIN32\nMIDL_INTERFACE(\"b58d6601-7401-4234-8180-6febfc0e484c\")"),
    ("""const ffxProvider* GetffxProvider(ffxStructType_t descType, uint64_t overrideId, void* device)
{
    // check driver-side providers
    GetExternalProviders(reinterpret_cast<ID3D12Device*>(device), descType);""",
     """#else
// Linux: no driver-side providers (that hook is AMD's Windows D3D12 driver extension)
static void GetExternalProviders(void*, uint64_t) {}
#define ID3D12Device void
#endif

const ffxProvider* GetffxProvider(ffxStructType_t descType, uint64_t overrideId, void* device)
{
    // check driver-side providers
    GetExternalProviders(reinterpret_cast<ID3D12Device*>(device), descType);"""),
])

# the includes of the providers left out
edit("src/ffx_provider.cpp", [
    ('#include "ffx_provider_fsr2.h"\n#include "ffx_provider_fsr3upscale.h"\n#include "ffx_provider_framegeneration.h"\n',
     '#include "ffx_provider_fsr3upscale.h"\n#ifndef FFX_UPSCALE_ONLY\n#include "ffx_provider_fsr2.h"\n'
     '#include "ffx_provider_framegeneration.h"\n#endif\n'),
    ('#ifdef FFX_BACKEND_VK\n#include "vk/ffx_provider_framegenerationswapchain_vk.h"',
     '#if defined(FFX_BACKEND_VK) && !defined(FFX_UPSCALE_ONLY)\n#include "vk/ffx_provider_framegenerationswapchain_vk.h"'),
])
# the Vulkan helpers at the end of ffx_api_vk.h are C++ (FfxApiResource without "struct", bool): SDK 2.x puts them
# behind __cplusplus, so a C caller (runtime/src/gpu_fsr.c) can include the header
p = root / "include/ffx_api/vk/ffx_api_vk.h"
s = p.read_text()
# ... and a C caller needs the typedef C++ gives for free: VkQueueInfoFFXAPI is used without "struct"
s = s.replace("#define FFX_API_EFFECT_ID_FGSC_VK", "typedef struct VkQueueInfoFFXAPI VkQueueInfoFFXAPI;\n\n#define FFX_API_EFFECT_ID_FGSC_VK", 1)
i = s.index("static inline uint32_t ffxApiGetSurfaceFormatVK")
p.write_text(s[:i] + "#if defined(__cplusplus)\n" + s[i:].rstrip() + "\n#endif // defined(__cplusplus)\n")

# Linux file names are case-sensitive: AMD's includes say FidelityFx/ for the FidelityFX/ folder
for p in sorted((root / "src").rglob("*")):
    if p.suffix in (".cpp", ".h"):
        s = p.read_text()
        # ... and MSVC's ui64 literal suffix is standard C++'s ull
        t = re.sub(r"([0-9A-Fa-f])ui64\b", r"\1ull", s.replace("FidelityFx/", "FidelityFX/"))
        if t != s:
            p.write_text(t)
print("patched", root)
