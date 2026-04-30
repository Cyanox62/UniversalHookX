#include "../../../backend.hpp"
#include "../../../console/console.hpp"

#ifdef ENABLE_BACKEND_DX8
#include <Windows.h>
#include <d3d8.h>
#include <psapi.h>
#pragma comment(lib, "d3d8.lib")

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../../../menu/menu.hpp"
#include "../../../utils/utils.hpp"
#include "../../hooks.hpp"
#include "hook_directx8.hpp"

#include "../../../dependencies/imgui/imgui_impl_dx8.h"
#include "../../../dependencies/imgui/imgui_impl_win32.h"
#include "../../../dependencies/minhook/MinHook.h"

static void Trace(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    LOG("%s", buf);
}

static IDirect3DDevice8* g_activeDevice = nullptr;
static HWND s_hwnd = nullptr;
static std::atomic<bool> g_bImGuiInited{false};
static std::atomic<bool> g_hooksReduced{false};

static const GUID kIID_IDirect3DDevice8 = {
    0x7385E5DF, 0x8FE8, 0x41D5, {0x86, 0xB6, 0xD7, 0xB4, 0x85, 0x47, 0xB6, 0xCF}};

typedef HRESULT(WINAPI* fnReset_t)(IDirect3DDevice8*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(WINAPI* fnPresent_t)(IDirect3DDevice8*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(WINAPI* fnCreateDevice_t)(IDirect3D8*, UINT, D3DDEVTYPE, HWND, DWORD,
                                          D3DPRESENT_PARAMETERS*, IDirect3DDevice8**);

struct HookedFn {
    void* fnOrig;
    void* fnTrampoline;
};

static std::vector<HookedFn> g_presentHooks;
static std::vector<HookedFn> g_resetHooks;
static fnCreateDevice_t oCreateDevice = nullptr;

static void* UploadTextureRGBA_DX8(const uint8_t* rgba, int w, int h) {
    if (!g_activeDevice)
        return nullptr;

    IDirect3DTexture8* pTex = nullptr;
    if (FAILED(g_activeDevice->CreateTexture((UINT)w, (UINT)h, 1, 0,
                                             D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &pTex)))
        return nullptr;

    D3DLOCKED_RECT lr;
    if (FAILED(pTex->LockRect(0, &lr, nullptr, 0))) {
        pTex->Release( );
        return nullptr;
    }

    for (int y = 0; y < h; ++y) {
        const uint8_t* src = rgba + (size_t)y * w * 4;
        uint8_t* dst = (uint8_t*)lr.pBits + (size_t)y * lr.Pitch;
        for (int x = 0; x < w; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }
    pTex->UnlockRect(0);
    return pTex;
}

struct DLLRange {
    uintptr_t start, end;
};

static void GetSectionsByCharacteristics(HMODULE h,
                                         DWORD requireSet, DWORD requireClear,
                                         std::vector<DLLRange>& out) {
    auto* dos = (PIMAGE_DOS_HEADER)h;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    auto* nt = (PIMAGE_NT_HEADERS)((uint8_t*)h + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;

    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if ((sec->Characteristics & requireSet) != requireSet)
            continue;
        if ((sec->Characteristics & requireClear) != 0)
            continue;
        DWORD size = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        if (size == 0)
            continue;
        DLLRange r;
        r.start = (uintptr_t)h + sec->VirtualAddress;
        r.end = r.start + size;
        out.push_back(r);
    }
}

static void DumpSections(HMODULE h) {
    auto* dos = (PIMAGE_DOS_HEADER)h;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    auto* nt = (PIMAGE_NT_HEADERS)((uint8_t*)h + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        char nameBuf[9] = { };
        memcpy(nameBuf, sec->Name, 8);
        Trace("[+] DX8: section '%s' VA=0x%p size=0x%X char=0x%08X\n",
              nameBuf,
              (void*)((uintptr_t)h + sec->VirtualAddress),
              sec->Misc.VirtualSize,
              sec->Characteristics);
    }
}

static bool InRanges(uintptr_t addr, const std::vector<DLLRange>& ranges) {
    for (auto& r : ranges)
        if (addr >= r.start && addr < r.end)
            return true;
    return false;
}

static void MemScanAll(const uint8_t* hay, size_t haylen,
                       const uint8_t* needle, size_t needlen,
                       std::vector<uintptr_t>& out) {
    if (needlen == 0 || needlen > haylen)
        return;
    for (size_t i = 0; i + needlen <= haylen; ++i) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, needlen) == 0)
            out.push_back((uintptr_t)(hay + i));
    }
}

static bool FindDevice8Vtables(HMODULE hD3D8, std::vector<uintptr_t*>& outVtables) {
    DumpSections(hD3D8);

    std::vector<DLLRange> textRanges;
    GetSectionsByCharacteristics(hD3D8,
                                 IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ,
                                 0,
                                 textRanges);
    std::vector<DLLRange> rdataRanges;
    GetSectionsByCharacteristics(hD3D8,
                                 IMAGE_SCN_MEM_READ,
                                 IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE,
                                 rdataRanges);

    if (textRanges.empty( )) {
        Trace("[!] DX8: d3d8.dll has no executable sections\n");
        return false;
    }
    if (rdataRanges.empty( )) {
        Trace("[!] DX8: d3d8.dll has no read-only data sections\n");
        return false;
    }
    for (auto& r : textRanges)
        Trace("[+] DX8: text range  0x%p-0x%p\n", (void*)r.start, (void*)r.end);
    for (auto& r : rdataRanges)
        Trace("[+] DX8: rdata range 0x%p-0x%p\n", (void*)r.start, (void*)r.end);

    MODULEINFO mi = { };
    if (!GetModuleInformation(GetCurrentProcess( ), hD3D8, &mi, sizeof(mi))) {
        Trace("[!] DX8: GetModuleInformation failed\n");
        return false;
    }

    std::vector<uintptr_t> iidLocs;
    MemScanAll((const uint8_t*)hD3D8, mi.SizeOfImage,
               (const uint8_t*)&kIID_IDirect3DDevice8, sizeof(GUID),
               iidLocs);
    if (iidLocs.empty( )) {
        Trace("[!] DX8: IID_IDirect3DDevice8 not found anywhere in d3d8.dll\n");
        return false;
    }
    Trace("[+] DX8: %u copy/copies of IID_IDirect3DDevice8\n",
          (unsigned)iidLocs.size( ));
    std::sort(iidLocs.begin( ), iidLocs.end( ));

    std::vector<DLLRange> readableRanges;
    GetSectionsByCharacteristics(hD3D8, IMAGE_SCN_MEM_READ,
                                 IMAGE_SCN_MEM_WRITE, readableRanges);

    auto isInText = [&](uintptr_t a) { return InRanges(a, textRanges); };

    auto functionUsesIID = [&](uintptr_t fn) -> bool {
        if (!isInText(fn))
            return false;

        const int kScanBytes = 0x300;
        for (int off = 0; off + (int)sizeof(uint32_t) <= kScanBytes; ++off) {
            uintptr_t at = fn + off;
            if (!isInText(at))
                break;
            uint32_t v = *(uint32_t*)at;
            if (std::binary_search(iidLocs.begin( ), iidLocs.end( ), (uintptr_t)v))
                return true;
        }
        return false;
    };

    int slot0Cache_hits = 0;
    int slot0Cache_misses = 0;
    std::vector<uintptr_t> qiVerified;
    std::vector<uintptr_t> qiRejected;

    auto isQIFunction = [&](uintptr_t fn) -> bool {
        if (std::binary_search(qiVerified.begin( ), qiVerified.end( ), fn)) {
            slot0Cache_hits++;
            return true;
        }
        if (std::binary_search(qiRejected.begin( ), qiRejected.end( ), fn)) {
            slot0Cache_hits++;
            return false;
        }
        slot0Cache_misses++;
        bool ok = functionUsesIID(fn);
        if (ok) {
            qiVerified.push_back(fn);
            std::sort(qiVerified.begin( ), qiVerified.end( ));
        } else {
            qiRejected.push_back(fn);
            std::sort(qiRejected.begin( ), qiRejected.end( ));
        }
        return ok;
    };

    const int kMinDevice8Length = 80;

    struct Candidate {
        uintptr_t addr;
        int length;
    };
    std::vector<Candidate> candidates;
    int rejectedTooShort = 0;

    for (auto& rng : readableRanges) {
        if (rng.end < rng.start + kMinDevice8Length * sizeof(uintptr_t))
            continue;
        const uintptr_t scanEnd = rng.end - kMinDevice8Length * sizeof(uintptr_t);
        for (uintptr_t p = rng.start; p < scanEnd; p += sizeof(uintptr_t)) {
            uintptr_t* arr = (uintptr_t*)p;

            if (!isInText(arr[0]))
                continue;
            if (!isInText(arr[1]))
                continue;
            if (!isInText(arr[2]))
                continue;
            if (!isInText(arr[14]))
                continue;
            if (!isInText(arr[15]))
                continue;
            if (arr[0] == arr[1] || arr[14] == arr[15])
                continue;

            if (!isQIFunction(arr[0]))
                continue;

            int len = 0;
            for (int i = 0; i < 200; ++i) {
                if ((uintptr_t)&arr[i + 1] > rng.end)
                    break;
                if (!isInText(arr[i]))
                    break;
                ++len;
            }
            if (len < kMinDevice8Length) {
                ++rejectedTooShort;
                continue;
            }

            candidates.push_back({p, len});
        }
    }

    std::sort(candidates.begin( ), candidates.end( ),
              [](const Candidate& a, const Candidate& b) { return a.addr < b.addr; });
    uintptr_t lastEnd = 0;
    for (auto& c : candidates) {
        if (c.addr < lastEnd)
            continue;
        outVtables.push_back((uintptr_t*)c.addr);
        Trace("[+] DX8: Device8 vtable at 0x%p (len=%d, QI @ 0x%p, Reset @ 0x%p, Present @ 0x%p)\n",
              (void*)c.addr, c.length,
              (void*)((uintptr_t*)c.addr)[0],
              (void*)((uintptr_t*)c.addr)[14],
              (void*)((uintptr_t*)c.addr)[15]);
        lastEnd = c.addr + (uintptr_t)c.length * sizeof(uintptr_t);
    }

    Trace("[+] DX8: scan stats — %u candidates (length>=%d), %d rejected as too short, "
          "%u QI fns verified / %u rejected\n",
          (unsigned)candidates.size( ), kMinDevice8Length, rejectedTooShort,
          (unsigned)qiVerified.size( ), (unsigned)qiRejected.size( ));

    return !outVtables.empty( );
}

static fnPresent_t FindPresentTrampoline(IDirect3DDevice8* pDevice) {
    void** pVT = *(void***)pDevice;
    void* fnP = pVT[15];
    for (auto& h : g_presentHooks)
        if (h.fnOrig == fnP)
            return (fnPresent_t)h.fnTrampoline;
    return nullptr;
}

static fnReset_t FindResetTrampoline(IDirect3DDevice8* pDevice) {
    void** pVT = *(void***)pDevice;
    void* fnR = pVT[14];
    for (auto& h : g_resetHooks)
        if (h.fnOrig == fnR)
            return (fnReset_t)h.fnTrampoline;
    return nullptr;
}

static void RenderImGui_DX8(IDirect3DDevice8* pDevice);

static fnPresent_t SafeFindPresentTrampoline(IDirect3DDevice8* pDevice) {
    __try {
        return FindPresentTrampoline(pDevice);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static fnReset_t SafeFindResetTrampoline(IDirect3DDevice8* pDevice) {
    __try {
        return FindResetTrampoline(pDevice);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static HRESULT WINAPI hkPresent(IDirect3DDevice8* pDevice,
                                const RECT* pSourceRect, const RECT* pDestRect,
                                HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) {
    fnPresent_t orig = SafeFindPresentTrampoline(pDevice);
    if (orig)
        RenderImGui_DX8(pDevice); // SEH-protected internally
    if (orig)
        return orig(pDevice, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    return D3D_OK;
}

static HRESULT WINAPI hkReset(IDirect3DDevice8* pDevice, D3DPRESENT_PARAMETERS* pPP) {
    if (g_bImGuiInited.load( ) && pDevice == g_activeDevice) {
        __try {
            ImGui_ImplDX8_InvalidateDeviceObjects( );
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            OutputDebugStringA("[UHX] DX8: invalidate-on-reset threw, ignored\n");
        }
    }
    fnReset_t orig = SafeFindResetTrampoline(pDevice);
    if (orig)
        return orig(pDevice, pPP);
    return D3D_OK;
}

static void HookDevice8Vtables(const std::vector<uintptr_t*>& vtables) {
    for (auto* vt : vtables) {
        void* fnPresent = (void*)vt[15];
        void* fnReset = (void*)vt[14];

        bool presentDup = false;
        for (auto& h : g_presentHooks)
            if (h.fnOrig == fnPresent) {
                presentDup = true;
                break;
            }
        if (!presentDup) {
            void* tramp = nullptr;
            MH_STATUS s = MH_CreateHook(fnPresent, &hkPresent, &tramp);
            if (s == MH_OK && tramp) {
                g_presentHooks.push_back({fnPresent, tramp});
                Trace("[+] DX8: created Present hook @ 0x%p (tramp 0x%p)\n", fnPresent, tramp);
            } else {
                Trace("[!] DX8: MH_CreateHook(Present @ 0x%p) failed: %d\n", fnPresent, (int)s);
            }
        }

        bool resetDup = false;
        for (auto& h : g_resetHooks)
            if (h.fnOrig == fnReset) {
                resetDup = true;
                break;
            }

        bool resetCollidesWithPresent = false;
        for (auto& h : g_presentHooks)
            if (h.fnOrig == fnReset) {
                resetCollidesWithPresent = true;
                break;
            }
        if (!resetDup && !resetCollidesWithPresent) {
            void* tramp = nullptr;
            MH_STATUS s = MH_CreateHook(fnReset, &hkReset, &tramp);
            if (s == MH_OK && tramp) {
                g_resetHooks.push_back({fnReset, tramp});
                Trace("[+] DX8: created Reset hook @ 0x%p (tramp 0x%p)\n", fnReset, tramp);
            } else {
                Trace("[!] DX8: MH_CreateHook(Reset @ 0x%p) failed: %d\n", fnReset, (int)s);
            }
        }
    }

    MH_STATUS s = MH_EnableHook(MH_ALL_HOOKS);
    if (s != MH_OK) {
        Trace("[!] DX8: MH_EnableHook(MH_ALL_HOOKS) returned %d\n", (int)s);
    }
}

static HRESULT WINAPI hkCreateDevice(IDirect3D8* pD3D, UINT Adapter, D3DDEVTYPE DeviceType,
                                     HWND hFocusWindow, DWORD BehaviorFlags,
                                     D3DPRESENT_PARAMETERS* pPP,
                                     IDirect3DDevice8** ppDevice) {
    HRESULT hr = oCreateDevice(pD3D, Adapter, DeviceType, hFocusWindow,
                               BehaviorFlags, pPP, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        void** pVT = *(void***)(*ppDevice);
        void* fnP = pVT[15];
        bool covered = false;
        for (auto& h : g_presentHooks)
            if (h.fnOrig == fnP) {
                covered = true;
                break;
            }
        Trace("[+] DX8: CreateDevice -> Present @ 0x%p (%s)\n",
              fnP, covered ? "already hooked" : "UNHOOKED — overlay will not render for this device");
    }
    return hr;
}

static bool HookCreateDeviceVtable(HMODULE hD3D8) {
    typedef IDirect3D8*(WINAPI * PFN_Create8)(UINT);
    auto pfn = (PFN_Create8)GetProcAddress(hD3D8, "Direct3DCreate8");
    if (!pfn)
        return false;

    IDirect3D8* pD3D = pfn(D3D_SDK_VERSION);
    if (!pD3D) {
        Trace("[!] DX8: Direct3DCreate8 returned NULL\n");
        return false;
    }

    void** pVT = *(void***)pD3D;
    void* fnCreate = pVT[15]; // IDirect3D8::CreateDevice slot

    MH_STATUS s = MH_CreateHook(fnCreate, &hkCreateDevice, (void**)&oCreateDevice);
    if (s == MH_OK && MH_EnableHook(fnCreate) == MH_OK)
        Trace("[+] DX8: hooked IDirect3D8::CreateDevice @ 0x%p\n", fnCreate);
    else
        Trace("[!] DX8: failed to hook IDirect3D8::CreateDevice (status %d)\n", (int)s);

    pD3D->Release( );
    return true;
}

static bool DeviceBelongsToUs(IDirect3DDevice8* pDevice);

static bool SafeReadPtr(const uintptr_t* p, uintptr_t* out) {
    __try {
        *out = *p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeDeviceBelongsToUs(IDirect3DDevice8* dev) {
    __try {
        return DeviceBelongsToUs(dev);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static IDirect3DDevice8* FindGameDevice(const std::vector<uintptr_t*>& vtables,
                                        HMODULE hD3D8) {
    Trace("[+] DX8: scanning process memory for the game's live device...\n");

    std::vector<uintptr_t> vtSet;
    vtSet.reserve(vtables.size( ));
    for (auto* v : vtables)
        vtSet.push_back((uintptr_t)v);
    std::sort(vtSet.begin( ), vtSet.end( ));

    MODULEINFO mi = { };
    GetModuleInformation(GetCurrentProcess( ), hD3D8, &mi, sizeof(mi));
    const uintptr_t d3d8Base = (uintptr_t)hD3D8;
    const uintptr_t d3d8End = d3d8Base + mi.SizeOfImage;

    MEMORY_BASIC_INFORMATION mbi = { };
    uintptr_t addr = 0;
    int hits = 0;
    int rejected = 0;

    while (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) {
        const uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        addr = regionEnd;
        if (mbi.State != MEM_COMMIT)
            continue;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
            continue;
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                               PAGE_EXECUTE_WRITECOPY;
        if (!(mbi.Protect & readable))
            continue;
        if ((uintptr_t)mbi.BaseAddress >= d3d8Base &&
            (uintptr_t)mbi.BaseAddress < d3d8End)
            continue;

        uintptr_t* p = (uintptr_t*)mbi.BaseAddress;
        uintptr_t* pEnd = (uintptr_t*)regionEnd;

        for (; p < pEnd; ++p) {
            uintptr_t vtbl = 0;
            if (!SafeReadPtr(p, &vtbl))
                break;

            if (!std::binary_search(vtSet.begin( ), vtSet.end( ), vtbl))
                continue;

            ++hits;
            IDirect3DDevice8* cand = (IDirect3DDevice8*)p;

            if (!SafeDeviceBelongsToUs(cand)) {
                ++rejected;
                continue;
            }

            Trace("[+] DX8: game device found at 0x%p (vtable 0x%p)\n",
                  cand, (void*)vtbl);
            Trace("[+] DX8: scan stats — %d Device8 instances seen, %d rejected\n",
                  hits, rejected);
            return cand;
        }
    }

    Trace("[!] DX8: game device not found — %d Device8 instances seen, %d rejected\n",
          hits, rejected);
    return nullptr;
}

static HMODULE g_hD3D8 = nullptr;
static std::vector<uintptr_t*> g_vtables;
static std::atomic<bool> g_hookInstalled{false};

static bool TryHookGameVtable(HMODULE hD3D8) {
    if (g_hookInstalled.load( ))
        return true;

    IDirect3DDevice8* gameDev = FindGameDevice(g_vtables, hD3D8);
    if (!gameDev)
        return false;

    uintptr_t* gameVT = *(uintptr_t**)gameDev;
    std::vector<uintptr_t*> single;
    single.push_back(gameVT);

    HookDevice8Vtables(single);
    if (g_presentHooks.empty( )) {
        Trace("[!] DX8: hook install for game vtable failed\n");
        return false;
    }

    g_activeDevice = gameDev;
    g_hookInstalled.store(true);
    Trace("[+] DX8: locked to game device 0x%p; hooked Present @ 0x%p / Reset @ 0x%p\n",
          gameDev, (void*)gameVT[15], (void*)gameVT[14]);
    return true;
}

static DWORD WINAPI DX8DiscoveryThread(LPVOID) {
    Trace("[+] DX8: discovery thread polling for game device...\n");

    const int kMaxAttempts = 3000;
    for (int i = 0; i < kMaxAttempts; ++i) {
        if (g_hookInstalled.load( ))
            return 0;
        if (TryHookGameVtable(g_hD3D8)) {
            Trace("[+] DX8: discovery succeeded after %d poll(s)\n", i + 1);
            return 0;
        }
        Sleep(100);
    }
    Trace("[!] DX8: discovery thread giving up after %d attempts\n", kMaxAttempts);
    return 0;
}

namespace DX8 {
    void Hook(HWND hwnd) {
        s_hwnd = hwnd;
        Trace("[+] DX8::Hook entered (hwnd=0x%p)\n", hwnd);

        HMODULE hD3D8 = nullptr;
        for (int i = 0; i < 300 && !hD3D8; ++i) {
            hD3D8 = GetModuleHandleA("d3d8.dll");
            if (!hD3D8)
                Sleep(100);
        }
        if (!hD3D8) {
            Trace("[!] DX8: d3d8.dll never loaded; bailing\n");
            return;
        }
        Trace("[+] DX8: d3d8.dll @ 0x%p\n", hD3D8);

        // Forward-compat (not relied on by the discovery path).
        HookCreateDeviceVtable(hD3D8);

        if (!FindDevice8Vtables(hD3D8, g_vtables)) {
            Trace("[!] DX8: no Device8 vtables found via IID matching; overlay will not work\n");
            return;
        }
        Trace("[+] DX8: found %u Device8 vtable(s)\n", (unsigned)g_vtables.size( ));

        Menu::InitializeContext(hwnd);
        g_hD3D8 = hD3D8;

        if (TryHookGameVtable(hD3D8))
            return;

        HANDLE hThread = CreateThread(nullptr, 0, DX8DiscoveryThread, nullptr, 0, nullptr);
        if (hThread) {
            CloseHandle(hThread);
            Trace("[+] DX8: discovery thread spawned; will hook when game device appears\n");
        } else {
            Trace("[!] DX8: CreateThread failed; overlay will not work\n");
        }
    }

    void Unhook( ) {
        if (ImGui::GetCurrentContext( )) {
            if (ImGui::GetIO( ).BackendRendererUserData)
                ImGui_ImplDX8_Shutdown( );
            if (ImGui::GetIO( ).BackendPlatformUserData)
                ImGui_ImplWin32_Shutdown( );
            ImGui::DestroyContext( );
        }
        g_bImGuiInited.store(false);
        g_activeDevice = nullptr;
    }
} // namespace DX8

static bool DeviceBelongsToUs(IDirect3DDevice8* pDevice) {
    D3DDEVICE_CREATION_PARAMETERS dcp = { };
    if (FAILED(pDevice->GetCreationParameters(&dcp)))
        return false;
    if (!dcp.hFocusWindow || !IsWindow(dcp.hFocusWindow))
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(dcp.hFocusWindow, &pid);
    return pid == GetCurrentProcessId( );
}

static void RenderImGui_DX8_Body(IDirect3DDevice8* pDevice) {
    if (U::GetRenderingBackend( ) != NONE && U::GetRenderingBackend( ) != DIRECTX8)
        return;

    if (!g_activeDevice) {
        if (!DeviceBelongsToUs(pDevice))
            return;
        g_activeDevice = pDevice;
    }
    if (pDevice != g_activeDevice)
        return;

    static std::atomic<bool> s_claimed{false};
    if (!s_claimed.exchange(true)) {
        OutputDebugStringA("[UHX] DX8: locked active device\n");
        Trace("[+] DX8 Present fired — claiming backend (device 0x%p)\n", pDevice);
        if (U::GetRenderingBackend( ) == NONE)
            U::SetRenderingBackend(DIRECTX8);
    }

    if (!g_hooksReduced.exchange(true)) {
        void** vt = *(void***)pDevice;
        void* keepP = vt[15];
        void* keepR = vt[14];
        int disP = 0;
        int disR = 0;
        for (auto& h : g_presentHooks)
            if (h.fnOrig != keepP && MH_DisableHook(h.fnOrig) == MH_OK)
                ++disP;
        for (auto& h : g_resetHooks)
            if (h.fnOrig != keepR && MH_DisableHook(h.fnOrig) == MH_OK)
                ++disR;
        Trace("[+] DX8: reduced active hook set — kept Present @ 0x%p / Reset @ 0x%p; "
              "disabled %d other Present, %d other Reset\n",
              keepP, keepR, disP, disR);
    }

    if (!g_bImGuiInited.load( )) {
        D3DDEVICE_CREATION_PARAMETERS dcp = { };
        if (SUCCEEDED(pDevice->GetCreationParameters(&dcp))) {
            if (ImGui::GetIO( ).BackendPlatformUserData)
                ImGui_ImplWin32_Shutdown( );
            ImGui_ImplWin32_Init(dcp.hFocusWindow);
        }
        if (!ImGui_ImplDX8_Init(pDevice)) {
            OutputDebugStringA("[UHX] DX8: ImGui_ImplDX8_Init returned false\n");
            return;
        }
        Menu::RegisterTextureUploader(UploadTextureRGBA_DX8);
        g_bImGuiInited.store(true);
    }

    if (H::bShuttingDown || !ImGui::GetCurrentContext( ))
        return;

    ImGui_ImplDX8_NewFrame( );
    ImGui_ImplWin32_NewFrame( );
    ImGui::NewFrame( );

    Menu::Render( );

    ImGui::EndFrame( );
    if (pDevice->BeginScene( ) == D3D_OK) {
        ImGui::Render( );
        ImGui_ImplDX8_RenderDrawData(ImGui::GetDrawData( ));
        pDevice->EndScene( );
    }
}

static int FilterRenderException(unsigned code) {
    char msg[64];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                "[UHX] DX8: render SEH exception 0x%08X — frame skipped\n", code);
    OutputDebugStringA(msg);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void RenderImGui_DX8(IDirect3DDevice8* pDevice) {
    static std::atomic<bool> s_rendering{false};
    bool expected = false;
    if (!s_rendering.compare_exchange_strong(expected, true))
        return;

    __try {
        RenderImGui_DX8_Body(pDevice);
    } __except (FilterRenderException(GetExceptionCode( ))) {
        // Frame swallowed — game continues normally.
    }

    s_rendering.store(false);
}

#else
#include <Windows.h>
namespace DX8 {
    void Hook(HWND hwnd) { LOG("[!] DirectX8 backend is not enabled!\n"); }
    void Unhook( ) { }
} // namespace DX8
#endif
