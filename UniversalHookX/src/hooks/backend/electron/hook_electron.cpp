#include "../../../backend.hpp"
#include "../../../console/console.hpp"

#ifdef ENABLE_BACKEND_ELECTRON
#include <Windows.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <vector>

#include "../../../dependencies/minhook/MinHook.h"
#include "../../../utils/utils.hpp"
#include "hook_electron.hpp"

namespace Electron {

    static DWORD g_gpuSubprocessPID = 0;
    DWORD GetGPUSubprocessPID( ) { return g_gpuSubprocessPID; }

    bool IsGPUProcess( ) {
        LPWSTR cmd = GetCommandLineW( );
        return cmd && wcsstr(cmd, L"--type=gpu-process") != nullptr;
    }

    // ---- Manual DLL mapper ----
    // Used when the GPU process has MicrosoftSignedOnly policy that blocks
    // LoadLibraryA for unsigned DLLs. Since ProhibitDynamicCode is not set, we
    // can still allocate executable memory and call DllMain via shellcode.

    static bool ManualMapInject(HANDLE hProcess, const char* dllPath) {
        LOG("[UHX] Electron: Attempting manual map injection...\n");

        HANDLE hFile = CreateFileA(dllPath, GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            LOG("[UHX] Electron ManualMap: Failed to open DLL file.\n");
            return false;
        }
        DWORD fileSize = GetFileSize(hFile, nullptr);
        std::vector<BYTE> file(fileSize);
        DWORD bytesRead = 0;
        ReadFile(hFile, file.data( ), fileSize, &bytesRead, nullptr);
        CloseHandle(hFile);
        if (bytesRead != fileSize) {
            LOG("[UHX] Electron ManualMap: Incomplete file read.\n");
            return false;
        }

        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data( ));
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            LOG("[UHX] Electron ManualMap: Bad DOS signature.\n");
            return false;
        }
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(file.data( ) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
            LOG("[UHX] Electron ManualMap: Not a 64-bit PE.\n");
            return false;
        }

        SIZE_T imgSize = nt->OptionalHeader.SizeOfImage;
        std::vector<BYTE> img(imgSize, 0);
        memcpy(img.data( ), file.data( ), nt->OptionalHeader.SizeOfHeaders);
        {
            auto* sec = IMAGE_FIRST_SECTION(nt);
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
                if (sec->SizeOfRawData == 0) continue;
                DWORD copyLen = sec->SizeOfRawData < sec->Misc.VirtualSize
                              ? sec->SizeOfRawData : sec->Misc.VirtualSize;
                if (sec->VirtualAddress + copyLen > imgSize) continue;
                memcpy(img.data( ) + sec->VirtualAddress,
                       file.data( ) + sec->PointerToRawData, copyLen);
            }
        }

        LPVOID remoteBase = VirtualAllocEx(hProcess,
            reinterpret_cast<LPVOID>(nt->OptionalHeader.ImageBase),
            imgSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!remoteBase)
            remoteBase = VirtualAllocEx(hProcess, nullptr, imgSize,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!remoteBase) {
            LOG("[UHX] Electron ManualMap: VirtualAllocEx for image failed.\n");
            return false;
        }
        LOG("[UHX] Electron ManualMap: Remote image base=0x%p size=%zu\n", remoteBase, imgSize);

        // Apply base relocations in local image
        ULONG_PTR delta = reinterpret_cast<ULONG_PTR>(remoteBase) - nt->OptionalHeader.ImageBase;
        if (delta) {
            auto& rd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            if (rd.VirtualAddress && rd.Size) {
                auto* reloc = reinterpret_cast<IMAGE_BASE_RELOCATION*>(img.data( ) + rd.VirtualAddress);
                while (reloc->VirtualAddress && reloc->SizeOfBlock >= sizeof(IMAGE_BASE_RELOCATION)) {
                    DWORD count = (reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
                    auto* entries = reinterpret_cast<WORD*>(reloc + 1);
                    for (DWORD j = 0; j < count; j++) {
                        if ((entries[j] >> 12) == IMAGE_REL_BASED_DIR64) {
                            DWORD off = reloc->VirtualAddress + (entries[j] & 0xFFF);
                            if (off + sizeof(ULONG_PTR) <= imgSize)
                                *reinterpret_cast<ULONG_PTR*>(img.data( ) + off) += delta;
                        }
                    }
                    reloc = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                        reinterpret_cast<BYTE*>(reloc) + reloc->SizeOfBlock);
                }
            }
        }

        // Resolve imports — system DLLs load at the same virtual address in
        // every process within a session, so local GetProcAddress is correct.
        auto& idd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (idd.VirtualAddress && idd.Size) {
            auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(img.data( ) + idd.VirtualAddress);
            while (desc->Name) {
                const char* depName = reinterpret_cast<const char*>(img.data( ) + desc->Name);
                HMODULE hDep = GetModuleHandleA(depName);
                if (!hDep) hDep = LoadLibraryA(depName);

                if (hDep) {
                    ULONG_PTR intRVA = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
                    auto* thunkINT = reinterpret_cast<IMAGE_THUNK_DATA64*>(img.data( ) + intRVA);
                    auto* thunkIAT = reinterpret_cast<IMAGE_THUNK_DATA64*>(img.data( ) + desc->FirstThunk);
                    for (; thunkINT->u1.AddressOfData; thunkINT++, thunkIAT++) {
                        if (thunkINT->u1.Ordinal & IMAGE_ORDINAL_FLAG64) {
                            thunkIAT->u1.Function = reinterpret_cast<ULONG_PTR>(
                                GetProcAddress(hDep, MAKEINTRESOURCEA(IMAGE_ORDINAL64(thunkINT->u1.Ordinal))));
                        } else {
                            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                                img.data( ) + thunkINT->u1.AddressOfData);
                            thunkIAT->u1.Function = reinterpret_cast<ULONG_PTR>(
                                GetProcAddress(hDep, ibn->Name));
                        }
                    }
                } else {
                    LOG("[UHX] Electron ManualMap: Missing dependency: %s\n", depName);
                }
                desc++;
            }
        }

        // Look up the exception directory (.pdata) so SEH/C++ exception unwind
        // works in the manually-mapped DLL. Without registering this, the CRT
        // initializer faults on the first try/catch or stack unwind.
        ULONG_PTR funcTableAddr  = 0;
        ULONG     funcTableCount = 0;
        {
            auto& exDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
            if (exDir.VirtualAddress && exDir.Size) {
                funcTableAddr  = reinterpret_cast<ULONG_PTR>(remoteBase) + exDir.VirtualAddress;
                funcTableCount = exDir.Size / sizeof(RUNTIME_FUNCTION);
            }
        }

        // Look up TLS callbacks. The MSVC CRT auto-generates `_tls_used` with
        // a TLS callback that initializes per-thread CRT state — without it,
        // _DllMainCRTStartup faults touching uninitialized TLS during init.
        // AddressOfCallBacks is post-relocation, so we convert back to RVA to
        // read the array out of our local image.
        std::vector<ULONG_PTR> tlsCallbacks;
        {
            auto& tlsD = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
            if (tlsD.VirtualAddress && tlsD.Size) {
                auto* tlsDir = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(img.data( ) + tlsD.VirtualAddress);
                if (tlsDir->AddressOfCallBacks) {
                    ULONG_PTR cbRVA = tlsDir->AddressOfCallBacks - reinterpret_cast<ULONG_PTR>(remoteBase);
                    if (cbRVA + sizeof(ULONG_PTR) <= imgSize) {
                        auto* cbArr = reinterpret_cast<ULONG_PTR*>(img.data( ) + cbRVA);
                        while (*cbArr && tlsCallbacks.size( ) < 32) {
                            tlsCallbacks.push_back(*cbArr);
                            cbArr++;
                        }
                    }
                }
            }
        }

        if (!WriteProcessMemory(hProcess, remoteBase, img.data( ), imgSize, nullptr)) {
            LOG("[UHX] Electron ManualMap: WriteProcessMemory failed.\n");
            VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
            return false;
        }

        FARPROC pRtlAdd = GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlAddFunctionTable");
        ULONG_PTR entry = reinterpret_cast<ULONG_PTR>(remoteBase) + nt->OptionalHeader.AddressOfEntryPoint;
        const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(remoteBase);

        // Resolve the remote address of UHXOverlayInit by walking the export
        // table. We use this as a fallback init path: even if DllMain →
        // CreateThread(OnProcessAttach) silently doesn't dispatch the thread
        // in the GPU subprocess (Chromium sandbox quirk), this synchronous
        // call gets init running. The atomic guard inside OnProcessAttach
        // makes the redundant call a no-op when DllMain's path worked.
        ULONG_PTR initFnRemote = 0;
        {
            auto& expDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (expDir.VirtualAddress && expDir.Size) {
                auto* exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(img.data( ) + expDir.VirtualAddress);
                auto* names = reinterpret_cast<DWORD*>(img.data( ) + exp->AddressOfNames);
                auto* ords  = reinterpret_cast<WORD*>(img.data( ) + exp->AddressOfNameOrdinals);
                auto* funcs = reinterpret_cast<DWORD*>(img.data( ) + exp->AddressOfFunctions);
                for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
                    const char* fname = reinterpret_cast<const char*>(img.data( ) + names[i]);
                    if (strcmp(fname, "UHXOverlayInit") == 0) {
                        initFnRemote = base + funcs[ords[i]];
                        break;
                    }
                }
            }
        }
        if (!initFnRemote)
            LOG("[UHX] Electron ManualMap: UHXOverlayInit export not found — fallback init disabled.\n");

        // Build the shellcode dynamically. Each "call(arg1, arg2, arg3, fn)"
        // emits 30 bytes (mov rcx,imm64 + mov rdx,imm64 + mov r8,imm64 + mov rax,imm64 + call rax),
        // or 33 bytes if RtlAddFunctionTable (uses imm32 for count via mov edx).
        std::vector<BYTE> sc;
        auto emit  = [&](const BYTE* b, size_t n) { sc.insert(sc.end( ), b, b + n); };
        auto emitU8  = [&](BYTE v)        { sc.push_back(v); };
        auto emitU64 = [&](ULONG_PTR v)   { BYTE* p = reinterpret_cast<BYTE*>(&v); emit(p, 8); };
        auto emitU32 = [&](ULONG v)       { BYTE* p = reinterpret_cast<BYTE*>(&v); emit(p, 4); };
        auto movRcx = [&](ULONG_PTR v)    { emitU8(0x48); emitU8(0xB9); emitU64(v); };
        auto movRdx = [&](ULONG_PTR v)    { emitU8(0x48); emitU8(0xBA); emitU64(v); };
        auto movR8  = [&](ULONG_PTR v)    {
            if (v == 0) { // xor r8, r8
                emitU8(0x4D); emitU8(0x31); emitU8(0xC0);
            } else {
                emitU8(0x49); emitU8(0xB8); emitU64(v);
            }
        };
        auto movRax = [&](ULONG_PTR v)    { emitU8(0x48); emitU8(0xB8); emitU64(v); };
        auto callRax= [&]( )              { emitU8(0xFF); emitU8(0xD0); };

        // sub rsp, 28h  (shadow space + 16-byte alignment)
        emitU8(0x48); emitU8(0x83); emitU8(0xEC); emitU8(0x28);

        // TLS callbacks: callback(base, DLL_PROCESS_ATTACH=1, NULL)
        for (ULONG_PTR cb : tlsCallbacks) {
            movRcx(base);
            movRdx(1);
            movR8(0);
            movRax(cb);
            callRax( );
        }

        // RtlAddFunctionTable(funcTable, count, base) — only if we have a .pdata
        // section and the function pointer resolved. Skipping this when the
        // DLL has no exception directory avoids the NULL-arg crash.
        if (funcTableAddr && pRtlAdd) {
            movRcx(funcTableAddr);
            // mov edx, count (5-byte BA imm32, zero-extends)
            emitU8(0xBA); emitU32(funcTableCount);
            movR8(base);
            movRax(reinterpret_cast<ULONG_PTR>(pRtlAdd));
            callRax( );
        }

        // Entry point: _DllMainCRTStartup(base, DLL_PROCESS_ATTACH=1, NULL)
        // This initializes the CRT and calls user DllMain.
        movRcx(base);
        movRdx(1);
        movR8(0);
        movRax(entry);
        callRax( );

        // Fallback init: UHXOverlayInit(NULL). After CRT init has run, call
        // our exported init function explicitly — it spawns OnProcessAttach
        // on its own thread, with an atomic guard that no-ops if DllMain
        // already started it. This is the workaround for sandboxed GPU
        // subprocesses where DllMain's CreateThread silently fails to
        // dispatch the init thread.
        if (initFnRemote) {
            movRcx(0);
            movRax(initFnRemote);
            callRax( );
        }

        // add rsp, 28h ; ret
        emitU8(0x48); emitU8(0x83); emitU8(0xC4); emitU8(0x28);
        emitU8(0xC3);

        LOG("[UHX] Electron ManualMap: funcTable=0x%llX count=%lu entry=0x%llX initFn=0x%llX tlsCallbacks=%zu shellcode=%zub\n",
            (unsigned long long)funcTableAddr, funcTableCount,
            (unsigned long long)entry, (unsigned long long)initFnRemote,
            tlsCallbacks.size( ), sc.size( ));

        LPVOID remoteSC = VirtualAllocEx(hProcess, nullptr, sc.size( ),
                                         MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!remoteSC) {
            LOG("[UHX] Electron ManualMap: VirtualAllocEx for shellcode failed.\n");
            VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
            return false;
        }
        WriteProcessMemory(hProcess, remoteSC, sc.data( ), sc.size( ), nullptr);

        HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteSC), nullptr, 0, nullptr);

        bool ok = false;
        if (hThread) {
            WaitForSingleObject(hThread, 15000);
            DWORD exitCode = 0;
            GetExitCodeThread(hThread, &exitCode);
            // 0xC0000005 etc. are NTSTATUS exception codes — treat as failure.
            // DllMain returning 1 (TRUE) is the success signal.
            ok = (exitCode == 1);
            CloseHandle(hThread);
            LOG("[UHX] Electron ManualMap: DllMain/shellcode returned 0x%08lX (%s)\n",
                exitCode, ok ? "OK" : "FAIL");
        } else {
            LOG("[UHX] Electron ManualMap: CreateRemoteThread for shellcode failed.\n");
        }

        VirtualFreeEx(hProcess, remoteSC, 0, MEM_RELEASE);
        if (!ok) VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
        return ok;
    }

    // ---- GPU process injection ----

    static void InjectDLL(DWORD pid) {
        char dllPath[MAX_PATH] = { };
        if (!GetModuleFileNameA(U::GetCurrentImageBase( ), dllPath, MAX_PATH) || !dllPath[0]) {
            LOG("[UHX] Electron: Could not determine own DLL path.\n");
            return;
        }

        LOG("[UHX] Electron: InjectDLL -> PID=%lu path=%s\n", pid, dllPath);

        HANDLE hProcess = OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
            FALSE, pid);
        if (!hProcess) {
            LOG("[UHX] Electron: OpenProcess(%lu) failed: err=%lu\n", pid, GetLastError( ));
            return;
        }

        bool needsManualMap = false;
        {
            PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY sig = {};
            if (GetProcessMitigationPolicy(hProcess, ProcessSignaturePolicy, &sig, sizeof(sig))) {
                LOG("[UHX] Electron: SignaturePolicy: MicrosoftSignedOnly=%d StoreSignedOnly=%d OptIn=%d\n",
                    sig.MicrosoftSignedOnly, sig.StoreSignedOnly, sig.MitigationOptIn);
                if (sig.MicrosoftSignedOnly || sig.StoreSignedOnly) {
                    LOG("[UHX] Electron: Signature policy active — using manual map.\n");
                    needsManualMap = true;
                }
            }
            PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dyn = {};
            if (GetProcessMitigationPolicy(hProcess, ProcessDynamicCodePolicy, &dyn, sizeof(dyn))) {
                LOG("[UHX] Electron: DynamicCodePolicy: ProhibitDynamicCode=%d\n", dyn.ProhibitDynamicCode);
                if (dyn.ProhibitDynamicCode) {
                    LOG("[UHX] Electron: ProhibitDynamicCode is set — cannot allocate executable memory. Launch with --no-sandbox.\n");
                    CloseHandle(hProcess);
                    return;
                }
            }
        }

        if (needsManualMap) {
            if (!ManualMapInject(hProcess, dllPath))
                LOG("[UHX] Electron: Manual map injection failed.\n");
        } else {
            // Standard LoadLibraryA injection
            SIZE_T pathLen = strlen(dllPath) + 1;
            void* pRemote = VirtualAllocEx(hProcess, nullptr, pathLen,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (pRemote) {
                WriteProcessMemory(hProcess, pRemote, dllPath, pathLen, nullptr);
                FARPROC pLoadLibraryA = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
                HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
                    reinterpret_cast<LPTHREAD_START_ROUTINE>(pLoadLibraryA), pRemote, 0, nullptr);
                if (hThread) {
                    WaitForSingleObject(hThread, 8000);
                    DWORD exitCode = 0;
                    GetExitCodeThread(hThread, &exitCode);
                    CloseHandle(hThread);
                    if (exitCode)
                        LOG("[UHX] Electron: LoadLibraryA injection succeeded.\n");
                    else
                        LOG("[UHX] Electron: LoadLibraryA returned NULL.\n");
                }
                VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
            }
        }

        CloseHandle(hProcess);
    }

    struct InjectionParam { DWORD pid; };

    static DWORD WINAPI InjectionThread(LPVOID param) {
        InjectionParam* p = static_cast<InjectionParam*>(param);
        DWORD pid = p->pid;
        delete p;
        Sleep(800);
        InjectDLL(pid);
        return 0;
    }

    // ---- Find already-running GPU process ----
    // Uses NtQueryInformationProcess ProcessCommandLineInformation (class 60, Win 8.1+)

    typedef NTSTATUS (NTAPI* PFN_NtQIP)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

    static bool ProcessHasGPUFlag(DWORD pid) {
        static PFN_NtQIP pfn = reinterpret_cast<PFN_NtQIP>(
            GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess"));
        if (!pfn) return false;

        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProcess) return false;

        ULONG needed = 0;
        pfn(hProcess, static_cast<PROCESSINFOCLASS>(60), nullptr, 0, &needed);

        bool found = false;
        if (needed > 0) {
            std::vector<BYTE> buf(needed, 0);
            if (pfn(hProcess, static_cast<PROCESSINFOCLASS>(60), buf.data( ), needed, &needed) == 0) {
                struct UStr { USHORT Len; USHORT MaxLen; WCHAR* Buf; };
                if (buf.size( ) >= sizeof(UStr)) {
                    auto* us = reinterpret_cast<UStr*>(buf.data( ));
                    ULONG strOffset = static_cast<ULONG>(sizeof(UStr));
                    ULONG strBytes  = us->Len;
                    if (strOffset + strBytes <= static_cast<ULONG>(buf.size( )) && strBytes > 0) {
                        const WCHAR* str = reinterpret_cast<const WCHAR*>(buf.data( ) + strOffset);
                        ULONG strLen = strBytes / sizeof(WCHAR);
                        const WCHAR* needle = L"--type=gpu-process";
                        ULONG needleLen = static_cast<ULONG>(wcslen(needle));
                        for (ULONG i = 0; i + needleLen <= strLen; i++) {
                            if (wcsncmp(str + i, needle, needleLen) == 0) { found = true; break; }
                        }
                    }
                }
            }
        }

        CloseHandle(hProcess);
        return found;
    }

    static DWORD FindExistingGPUProcess( ) {
        DWORD currentPID = GetCurrentProcessId( );
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap == INVALID_HANDLE_VALUE) {
            LOG("[UHX] Electron: CreateToolhelp32Snapshot failed.\n");
            return 0;
        }

        LOG("[UHX] Electron: FindExistingGPUProcess - current PID=%lu\n", currentPID);

        PROCESSENTRY32W pe = { sizeof(pe) };
        DWORD gpuPID = 0;

        // First pass: direct children only
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (pe.th32ProcessID == currentPID) continue;
                if (pe.th32ParentProcessID != currentPID) continue;

                LOG("[UHX] Electron: Child PID=%lu exe=%ls - checking gpu flag\n",
                    pe.th32ProcessID, pe.szExeFile);

                if (ProcessHasGPUFlag(pe.th32ProcessID)) {
                    gpuPID = pe.th32ProcessID;
                    LOG("[UHX] Electron: GPU process (child): PID=%lu exe=%ls\n",
                        gpuPID, pe.szExeFile);
                    break;
                }
            } while (Process32NextW(hSnap, &pe));
        }

        // Second pass: scan all processes with same exe name (launcher scenarios)
        if (!gpuPID) {
            LOG("[UHX] Electron: No GPU child found - scanning same-exe processes (launcher scenario).\n");

            WCHAR selfExe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, selfExe, MAX_PATH);
            WCHAR* selfName = wcsrchr(selfExe, L'\\');
            if (selfName) selfName++; else selfName = selfExe;

            CloseHandle(hSnap);
            hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

            if (hSnap != INVALID_HANDLE_VALUE && Process32FirstW(hSnap, &pe)) {
                do {
                    if (pe.th32ProcessID == currentPID) continue;
                    if (_wcsicmp(pe.szExeFile, selfName) != 0) continue;

                    LOG("[UHX] Electron: Same-exe PID=%lu parent=%lu - checking gpu flag\n",
                        pe.th32ProcessID, pe.th32ParentProcessID);

                    if (ProcessHasGPUFlag(pe.th32ProcessID)) {
                        gpuPID = pe.th32ProcessID;
                        LOG("[UHX] Electron: GPU process (grandchild): PID=%lu\n", gpuPID);
                        break;
                    }
                } while (Process32NextW(hSnap, &pe));
            }
        }

        CloseHandle(hSnap);
        return gpuPID;
    }

    // ---- CreateProcessW / CreateProcessA hooks ----

    typedef BOOL(WINAPI* FnCreateProcessW)(
        LPCWSTR, LPWSTR,
        LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
        BOOL, DWORD, LPVOID, LPCWSTR,
        LPSTARTUPINFOW, LPPROCESS_INFORMATION);

    static FnCreateProcessW oCreateProcessW = nullptr;

    static BOOL WINAPI hkCreateProcessW(
        LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
        LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
        BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment,
        LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo,
        LPPROCESS_INFORMATION lpProcessInformation) {
        PROCESS_INFORMATION localPI = { };
        LPPROCESS_INFORMATION pPI = lpProcessInformation ? lpProcessInformation : &localPI;

        BOOL result = oCreateProcessW(lpApplicationName, lpCommandLine,
            lpProcessAttributes, lpThreadAttributes, bInheritHandles,
            dwCreationFlags, lpEnvironment, lpCurrentDirectory, lpStartupInfo, pPI);

        if (result && lpCommandLine && wcsstr(lpCommandLine, L"--type=gpu-process")) {
            LOG("[UHX] Electron: GPU subprocess spawned (W). Scheduling injection PID=%lu...\n", pPI->dwProcessId);
            g_gpuSubprocessPID = pPI->dwProcessId;
            InjectionParam* p = new InjectionParam{ pPI->dwProcessId };
            HANDLE hThread = CreateThread(nullptr, 0, InjectionThread, p, 0, nullptr);
            if (hThread) CloseHandle(hThread);
        }
        return result;
    }

    typedef BOOL(WINAPI* FnCreateProcessA)(
        LPCSTR, LPSTR,
        LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
        BOOL, DWORD, LPVOID, LPCSTR,
        LPSTARTUPINFOA, LPPROCESS_INFORMATION);

    static FnCreateProcessA oCreateProcessA = nullptr;

    static BOOL WINAPI hkCreateProcessA(
        LPCSTR lpApplicationName, LPSTR lpCommandLine,
        LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
        BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment,
        LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo,
        LPPROCESS_INFORMATION lpProcessInformation) {
        PROCESS_INFORMATION localPI = { };
        LPPROCESS_INFORMATION pPI = lpProcessInformation ? lpProcessInformation : &localPI;

        BOOL result = oCreateProcessA(lpApplicationName, lpCommandLine,
            lpProcessAttributes, lpThreadAttributes, bInheritHandles,
            dwCreationFlags, lpEnvironment, lpCurrentDirectory, lpStartupInfo, pPI);

        if (result && lpCommandLine && strstr(lpCommandLine, "--type=gpu-process")) {
            LOG("[UHX] Electron: GPU subprocess spawned (A). Scheduling injection PID=%lu...\n", pPI->dwProcessId);
            g_gpuSubprocessPID = pPI->dwProcessId;
            InjectionParam* p = new InjectionParam{ pPI->dwProcessId };
            HANDLE hThread = CreateThread(nullptr, 0, InjectionThread, p, 0, nullptr);
            if (hThread) CloseHandle(hThread);
        }
        return result;
    }

    void Hook( ) {
        // GPU process: nothing to do here. We're the rendering side; the
        // browser process is responsible for spawning more GPU processes
        // (none for us to monitor) and the rendering hooks (DX11/DX12) are
        // installed elsewhere in H::Init.
        if (IsGPUProcess( )) {
            LOG("[UHX] Electron: in GPU subprocess - skipping subprocess monitor.\n");
            return;
        }

        HMODULE hKernel = GetModuleHandleA("kernel32.dll");
        if (!hKernel) {
            LOG("[UHX] Electron: kernel32.dll not loaded - skipping.\n");
            return;
        }

        void* fnW = reinterpret_cast<void*>(GetProcAddress(hKernel, "CreateProcessW"));
        if (fnW) {
            static MH_STATUS sW = MH_CreateHook(fnW, reinterpret_cast<void*>(&hkCreateProcessW),
                                                reinterpret_cast<void**>(&oCreateProcessW));
            (void)sW;
            MH_EnableHook(fnW);
        }

        void* fnA = reinterpret_cast<void*>(GetProcAddress(hKernel, "CreateProcessA"));
        if (fnA) {
            static MH_STATUS sA = MH_CreateHook(fnA, reinterpret_cast<void*>(&hkCreateProcessA),
                                                reinterpret_cast<void**>(&oCreateProcessA));
            (void)sA;
            MH_EnableHook(fnA);
        }

        LOG("[UHX] Electron: CreateProcess hooks installed for GPU subprocess injection.\n");

        // GPU process is almost always already running by injection time -
        // scan for it now and inject immediately if found.
        DWORD gpuPID = FindExistingGPUProcess( );
        if (gpuPID) {
            g_gpuSubprocessPID = gpuPID;
            LOG("[UHX] Electron: Found existing GPU process. Injecting immediately...\n");
            InjectDLL(gpuPID);
        } else {
            LOG("[UHX] Electron: No existing GPU process found. Waiting via CreateProcess hook.\n");
        }
    }

    void Unhook( ) {
        HMODULE hKernel = GetModuleHandleA("kernel32.dll");
        if (!hKernel) return;
        void* fnW = reinterpret_cast<void*>(GetProcAddress(hKernel, "CreateProcessW"));
        if (fnW) MH_DisableHook(fnW);
        void* fnA = reinterpret_cast<void*>(GetProcAddress(hKernel, "CreateProcessA"));
        if (fnA) MH_DisableHook(fnA);
    }

} // namespace Electron

#else // ENABLE_BACKEND_ELECTRON

namespace Electron {
    bool IsGPUProcess( ) { return false; }
    void Hook( ) { }
    void Unhook( ) { }
    DWORD GetGPUSubprocessPID( ) { return 0; }
} // namespace Electron

#endif
