"""YW4 Recomp's patch to FidelityFX SDK v1.1.4's ffx-api (the five-function API, decision 0073): builds on Linux,
with the providers of the FSR 3 upscaler and of frame generation without AMD's swapchain (FFX_YW4_LINUX: the game
presents and paces itself), and its headers usable from C (runtime/src/gpu_fsr.c, gpu_fg.c).
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
#ifndef FFX_YW4_LINUX
    &ffxProvider_FSR2::Instance,
#endif
    &ffxProvider_FrameGeneration::Instance,
"""),
    ("""#ifdef FFX_BACKEND_VK
    &ffxProvider_FrameGenerationSwapChain_VK::Instance,
#endif // FFX_BACKEND_VK
};""", """#if defined(FFX_BACKEND_VK) && !defined(FFX_YW4_LINUX)
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
     '#include "ffx_provider_fsr3upscale.h"\n#include "ffx_provider_framegeneration.h"\n#ifndef FFX_YW4_LINUX\n'
     '#include "ffx_provider_fsr2.h"\n#endif\n'),
    ('#ifdef FFX_BACKEND_VK\n#include "vk/ffx_provider_framegenerationswapchain_vk.h"',
     '#if defined(FFX_BACKEND_VK) && !defined(FFX_YW4_LINUX)\n#include "vk/ffx_provider_framegenerationswapchain_vk.h"'),
])

# frame generation's resource names: on Windows %s in a wide format takes a wide string, on Linux %ls does (the fix
# NX2navi's PR #455 makes, https://github.com/tangerineturtle1/NX2navi/pull/455, MIT)
edit("src/ffx_provider_framegeneration.cpp", [
    ('swprintf(Name, 255, L"%s%d", fiResourceDescs.dilatedDepth.name, i);',
     'swprintf(Name, 255, L"%ls%d", fiResourceDescs.dilatedDepth.name, i);'),
    ('swprintf(Name, 255, L"%s%d", fiResourceDescs.dilatedMotionVectors.name, i);',
     'swprintf(Name, 255, L"%ls%d", fiResourceDescs.dilatedMotionVectors.name, i);'),
    ('swprintf(Name, 255, L"%s%d", fiResourceDescs.reconstructedPrevNearestDepth.name, i);',
     'swprintf(Name, 255, L"%ls%d", fiResourceDescs.reconstructedPrevNearestDepth.name, i);'),
])
# the Vulkan helpers at the end of ffx_api_vk.h are C++ (FfxApiResource without "struct", bool): SDK 2.x puts them
# behind __cplusplus, so a C caller (runtime/src/gpu_fsr.c) can include the header
p = root / "include/ffx_api/vk/ffx_api_vk.h"
s = p.read_text()
# ... and a C caller needs the typedef C++ gives for free: VkQueueInfoFFXAPI is used without "struct"
s = s.replace("#define FFX_API_EFFECT_ID_FGSC_VK", "typedef struct VkQueueInfoFFXAPI VkQueueInfoFFXAPI;\n\n#define FFX_API_EFFECT_ID_FGSC_VK", 1)
i = s.index("static inline uint32_t ffxApiGetSurfaceFormatVK")
p.write_text(s[:i] + "#if defined(__cplusplus)\n" + s[i:].rstrip() + "\n#endif // defined(__cplusplus)\n")

# frame generation's header declares an enum with no values: C++ allows it, C does not (gpu_fg.c is C)
edit("include/ffx_api/ffx_framegeneration.h", [(
    "enum FfxApiConfigureFrameGenerationKey\n{\n    // No values.\n};",
    "#if defined(__cplusplus)\nenum FfxApiConfigureFrameGenerationKey\n{\n    // No values.\n};\n#endif",
)])

# Linux file names are case-sensitive: AMD's includes say FidelityFx/ for the FidelityFX/ folder
for p in sorted((root / "src").rglob("*")):
    if p.suffix in (".cpp", ".h"):
        s = p.read_text()
        # ... and MSVC's ui64 literal suffix is standard C++'s ull
        t = re.sub(r"([0-9A-Fa-f])ui64\b", r"\1ull", s.replace("FidelityFx/", "FidelityFX/"))
        if t != s:
            p.write_text(t)
print("patched", root)
