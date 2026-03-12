#include "flash_loader.h"
#include <shlwapi.h>
#include <shlobj.h>      // SHGetFolderPathW, CSIDL_APPDATA
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <intrin.h>
#include <ocidl.h>     // IQuickActivate, IPersistPropertyBag
#include <oleidl.h>     // IOleObject, IOleInPlaceObject, etc.

// Flash Player ActiveX CLSID: {D27CDB6E-AE6D-11CF-96B8-444553540000}
static const CLSID CLSID_ShockwaveFlash =
    {0xD27CDB6E, 0xAE6D, 0x11CF, {0x96, 0xB8, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

static wchar_t g_szOcxPath[MAX_PATH] = {};

typedef HRESULT (STDAPICALLTYPE *FN_DllGetClassObject)(REFCLSID, REFIID, LPVOID*);
typedef HRESULT (STDAPICALLTYPE *FN_CoGetClassObject)(
    REFCLSID, DWORD, LPVOID, REFIID, LPVOID*);
typedef HRESULT (STDAPICALLTYPE *FN_CoCreateInstance)(
    REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
typedef LSTATUS (WINAPI *FN_RegOpenKeyExW)(
    HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
typedef LSTATUS (WINAPI *FN_RegQueryValueExW)(
    HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef HRESULT (STDAPICALLTYPE *FN_CoGetClassObjectFromURL)(
    REFCLSID, LPCWSTR, DWORD, DWORD, LPCWSTR, LPBINDCTX,
    DWORD, LPVOID, REFIID, LPVOID*);
typedef HRESULT (STDAPICALLTYPE *FN_CoInternetIsFeatureEnabled)(
    DWORD, DWORD);
typedef void (WINAPI *FN_GetLocalTime)(LPSYSTEMTIME);
typedef void (WINAPI *FN_GetSystemTime)(LPSYSTEMTIME);
typedef void (WINAPI *FN_GetSystemTimeAsFileTime)(LPFILETIME);
typedef LSTATUS (WINAPI *FN_RegCloseKey)(HKEY);
typedef HRESULT (WINAPI *FN_WldpIsClassInApprovedList)(
    const CLSID*, void*, BOOL*, DWORD);
typedef HRESULT (WINAPI *FN_WldpQueryDynamicCodeTrust)(
    HANDLE, void*, DWORD);
typedef HRESULT (WINAPI *FN_LoadRegTypeLib)(
    REFGUID rguid, WORD wVerMajor, WORD wVerMinor, LCID lcid, ITypeLib** pptlib);
typedef HRESULT (WINAPI *FN_LoadTypeLibEx)(
    LPCOLESTR szFile, REGKIND regkind, ITypeLib** pptlib);
typedef HANDLE (WINAPI *FN_CreateFileW)(
    LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *FN_CreateDirectoryW)(LPCWSTR, LPSECURITY_ATTRIBUTES);
typedef DWORD (WINAPI *FN_GetFileAttributesW)(LPCWSTR);
typedef BOOL (WINAPI *FN_MoveFileW)(LPCWSTR, LPCWSTR);
typedef BOOL (WINAPI *FN_DeleteFileW)(LPCWSTR);
typedef HANDLE (WINAPI *FN_FindFirstFileW)(LPCWSTR, LPWIN32_FIND_DATAW);
typedef BOOL (WINAPI *FN_SetFileAttributesW)(LPCWSTR, DWORD);
typedef BOOL (WINAPI *FN_FindNextFileW)(HANDLE, LPWIN32_FIND_DATAW);
typedef BOOL (WINAPI *FN_MoveFileExW)(LPCWSTR, LPCWSTR, DWORD);
typedef HRESULT (STDAPICALLTYPE *FN_CLSIDFromProgID)(LPCOLESTR, LPCLSID);

// Debug tracing helper — output visible in Visual Studio Output or DebugView
static void DbgTrace(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringW(buf);
}

// Static member
IClassFactory* FlashLoader::s_pFlashFactory = nullptr;

// ===================================================================
// Logging IClassFactory wrapper — intercepts CreateInstance calls
// from MSHTML which calls pCF->CreateInstance() on the vtable directly
// (not through CoCreateInstance), so our COM hooks don't see it.
// ===================================================================
class LoggingClassFactory : public IClassFactory {
    IClassFactory* m_real;
    ULONG m_ref;
public:
    LoggingClassFactory(IClassFactory* real) : m_real(real), m_ref(1) { m_real->AddRef(); }
    ~LoggingClassFactory() { m_real->Release(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        HRESULT hr = m_real->QueryInterface(riid, ppv);
        DbgTrace(L"[FlashIE] Factory::QI {%08X-...} -> 0x%08X\n", riid.Data1, hr);
        return hr;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_ref; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG ref = --m_ref;
        if (ref == 0) delete this;
        return ref;
    }
    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        DbgTrace(L"[FlashIE] Factory::CreateInstance riid={%08X-...} outer=%p\n",
                 riid.Data1, pUnkOuter);
        HRESULT hr = m_real->CreateInstance(pUnkOuter, riid, ppv);
        DbgTrace(L"[FlashIE] Factory::CreateInstance -> hr=0x%08X obj=%p\n",
                 hr, (ppv && SUCCEEDED(hr)) ? *ppv : nullptr);
        // Log QI calls on the created object to see what MSHTML asks for
        if (SUCCEEDED(hr) && ppv && *ppv) {
            IUnknown* pObj = static_cast<IUnknown*>(*ppv);
            // Probe key interfaces MSHTML needs
            static const struct { IID iid; const wchar_t* name; } probes[] = {
                { IID_IOleObject, L"IOleObject" },
                { IID_IViewObject, L"IViewObject" },
                { IID_IViewObject2, L"IViewObject2" },
                { IID_IOleInPlaceObject, L"IOleInPlaceObject" },
                { IID_IOleInPlaceActiveObject, L"IOleInPlaceActiveObject" },
                { IID_IPersistStreamInit, L"IPersistStreamInit" },
                { IID_IPersistPropertyBag, L"IPersistPropertyBag" },
                { IID_IOleControl, L"IOleControl" },
                { IID_IQuickActivate, L"IQuickActivate" },
            };
            for (int i = 0; i < _countof(probes); i++) {
                void* pTest = nullptr;
                HRESULT hrQI = pObj->QueryInterface(probes[i].iid, &pTest);
                DbgTrace(L"[FlashIE] FlashObj::QI %s -> 0x%08X\n", probes[i].name, hrQI);
                if (pTest) static_cast<IUnknown*>(pTest)->Release();
            }
        }
        return hr;
    }
    STDMETHODIMP LockServer(BOOL fLock) override {
        return m_real->LockServer(fLock);
    }
};

static LoggingClassFactory* s_pLoggingFactory = nullptr;

// HKEY tracking for FEATURE_BROWSER_EMULATION (fake without registry writes)
static HKEY s_hkeyBrowserEmulation = nullptr;
static wchar_t s_szExeName[MAX_PATH] = {};

// Fake Flash CLSID registration using REAL HKEY handles.
// We can't use sentinel values because Windows internal code (rpcrt4, ole32)
// dereferences HKEY as a pointer to an internal structure, causing AV.
// Instead, we open real existing keys (read-only, no writes!) and track them.
enum FakeKeyType {
    FK_NONE = 0,
    FK_CLSID_ROOT,      // HKCR\CLSID\{D27CDB6E-...}
    FK_INPROC,           // ...\InprocServer32
    FK_MIME,             // MIME\Database\Content Type\application/x-shockwave-flash
    FK_MISCSTATUS,       // ...\MiscStatus
    FK_MISCSTATUS1,      // ...\MiscStatus\1
    FK_TYPELIB,          // ...\TypeLib
    FK_PROGID,           // ...\ProgID
    FK_CONTROL,          // ...\Control
    FK_VERSION,          // ...\Version
    FK_PROGID_ROOT,      // HKCR\ShockwaveFlash.ShockwaveFlash (ProgID root)
    FK_PROGID_CLSID,     // HKCR\ShockwaveFlash.ShockwaveFlash\CLSID
    FK_PROGID_CURVER,    // HKCR\ShockwaveFlash.ShockwaveFlash\CurVer
    FK_INSTALLED_VER,    // HKCR\CLSID\{...}\InstalledVersion
};

struct FakeKeyEntry {
    HKEY    hKey;
    FakeKeyType type;
};

static const int MAX_FAKE_KEYS = 64;
static FakeKeyEntry s_fakeKeys[MAX_FAKE_KEYS] = {};
static int s_fakeKeyCount = 0;

// Forward declarations — defined after InlineHook structs
static HKEY AllocFakeKey(FakeKeyType type);
static FakeKeyType GetFakeKeyType(HKEY hKey);
static bool IsFakeHKey(HKEY hKey);
static void CloseFakeKey(HKEY hKey);

// ===================================================================
// Minimal x86/x64 instruction length decoder for function prologues.
// Returns instruction length at 'code', or 0 if unrecognized.
// ===================================================================
static int InsnLength(const BYTE* code)
{
    const BYTE* p = code;

#ifdef _WIN64
    // REX prefix (0x40-0x4F)
    if (*p >= 0x40 && *p <= 0x4F)
        p++;
#endif

    BYTE op = *p++;

    // 1-byte: PUSH/POP reg, NOP, RET, INT3
    if ((op >= 0x50 && op <= 0x5F) || op == 0x90 || op == 0xC3 || op == 0xCC)
        return static_cast<int>(p - code);

#ifndef _WIN64
    // x86: INC/DEC reg
    if (op >= 0x40 && op <= 0x4F)
        return static_cast<int>(p - code);
#endif

    // 2-byte opcode escape (0F xx)
    bool twoByteOpcode = false;
    if (op == 0x0F) {
        BYTE op2 = *p++;
        switch (op2) {
        case 0x1F: // multi-byte NOP
        case 0xB6: case 0xB7: // MOVZX
        case 0xBE: case 0xBF: // MOVSX
            twoByteOpcode = true;
            break;
        default:
            return 0;
        }
    }

    int immSize = 0;
    bool hasModRM = twoByteOpcode;

    if (!twoByteOpcode) {
        switch (op) {
        // ALU r/m, r  and  r, r/m
        case 0x00: case 0x01: case 0x02: case 0x03:
        case 0x08: case 0x09: case 0x0A: case 0x0B:
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x38: case 0x39: case 0x3A: case 0x3B:
        case 0x84: case 0x85: // TEST
        case 0x86: case 0x87: // XCHG
        case 0x88: case 0x89: case 0x8A: case 0x8B: // MOV
        case 0x8D: // LEA
        case 0x8F: // POP r/m
        case 0xD1: case 0xD3: // SHL/SHR/etc.
        case 0xF7: // NOT/NEG/MUL/DIV
        case 0xFF: // INC/DEC/CALL/JMP r/m
            hasModRM = true;
            break;
        case 0x80: case 0x82: case 0x83: // Group1 r/m, imm8
        case 0xC0: case 0xC1: // SHL/SHR r/m, imm8
        case 0xC6: // MOV r/m8, imm8
        case 0x6B: // IMUL r, r/m, imm8
            hasModRM = true; immSize = 1; break;
        case 0x81: // Group1 r/m, imm32
        case 0x69: // IMUL r, r/m, imm32
        case 0xC7: // MOV r/m, imm32
            hasModRM = true; immSize = 4; break;

        // MOV reg, imm8
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            return static_cast<int>(p - code) + 1;
        // MOV reg, imm32/64
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
#ifdef _WIN64
            return static_cast<int>(p - code) + ((code[0] & 0x48) == 0x48 ? 8 : 4);
#else
            return static_cast<int>(p - code) + 4;
#endif
        // ALU AL/AX/EAX, imm8
        case 0x04: case 0x0C: case 0x14: case 0x1C:
        case 0x24: case 0x2C: case 0x34: case 0x3C:
            return static_cast<int>(p - code) + 1;
        // ALU AL/AX/EAX, imm32
        case 0x05: case 0x0D: case 0x15: case 0x1D:
        case 0x25: case 0x2D: case 0x35: case 0x3D:
            return static_cast<int>(p - code) + 4;

        default:
            return 0;
        }
    }

    if (hasModRM) {
        BYTE modrm = *p++;
        BYTE mod = (modrm >> 6) & 3;
        BYTE rm  = modrm & 7;

        if (mod != 3 && rm == 4) // SIB byte
            p++;

        if (mod == 0) {
            if (rm == 5) {
                p += 4; // disp32 (RIP-relative on x64)
#ifdef _WIN64
                return 0; // RIP-relative — cannot safely relocate
#endif
            }
        } else if (mod == 1) {
            p += 1; // disp8
        } else if (mod == 2) {
            p += 4; // disp32
        }

        p += immSize;
    }

    return static_cast<int>(p - code);
}

// Calculate how many bytes to copy (sum of complete instructions >= hookSize).
// Returns 0 if an instruction can't be decoded.
static int CalcPrologueSize(const BYTE* code, int hookSize)
{
    int total = 0;
    while (total < hookSize) {
        int len = InsnLength(code + total);
        if (len == 0) return 0;
        total += len;
    }
    return total;
}

// ===================================================================
// Inline hook (detour) infrastructure
// ===================================================================

#ifdef _WIN64
static constexpr int MIN_HOOK_SIZE = 14; // FF 25 00 00 00 00 + 8-byte addr
#else
static constexpr int MIN_HOOK_SIZE = 5;  // E9 + 4-byte rel32
#endif

struct InlineHook {
    void* pTarget     = nullptr; // Original function address
    void* pTrampoline = nullptr; // Executable trampoline to call original
    int   copySize    = 0;       // Bytes copied to trampoline
    bool  active      = false;
};

static InlineHook s_hookCoGetClassObject;
static InlineHook s_hookCoCreateInstance;
static InlineHook s_hookRegOpenKeyExW;
static InlineHook s_hookRegQueryValueExW;
static InlineHook s_hookRegCloseKey;
static InlineHook s_hookCoGetClassObjectFromURL;
static InlineHook s_hookCoInternetIsFeatureEnabled;
static InlineHook s_hookGetLocalTime;
static InlineHook s_hookGetSystemTime;
static InlineHook s_hookWldpIsClassInApprovedList;
static InlineHook s_hookWldpQueryDynamicCodeTrust;
static InlineHook s_hookLoadRegTypeLib;
static InlineHook s_hookGetSystemTimeAsFileTime;
static InlineHook s_hookCreateFileW;
static InlineHook s_hookCreateDirectoryW;
static InlineHook s_hookGetFileAttributesW;
static InlineHook s_hookFindFirstFileW;
static InlineHook s_hookMoveFileW;
static InlineHook s_hookMoveFileExW;
static InlineHook s_hookDeleteFileW;
static InlineHook s_hookCLSIDFromProgID;

// Path to our local mms.cfg (next to Flash.ocx)
static wchar_t g_szMmsCfgPath[MAX_PATH] = {};

// Path to local FlashData directory (next to exe) — replaces %APPDATA%\{Macromedia,Adobe}\Flash Player
static wchar_t g_szFlashDataDir[MAX_PATH] = {};
static int g_flashDataDirLen = 0; // wcslen(g_szFlashDataDir)

// The roaming path prefixes Flash uses
static wchar_t g_szRoamingFlashDir[MAX_PATH] = {};   // %APPDATA%\Macromedia\Flash Player
static int g_roamingFlashDirLen = 0;
static wchar_t g_szRoamingAdobeDir[MAX_PATH] = {};   // %APPDATA%\Adobe\Flash Player
static int g_roamingAdobeDirLen = 0;

// Forward declaration
static void NeutralizeFlashBlock();

// ===================================================================
// Fake HKEY allocation — uses real HKEY handles to avoid AV crashes.
// Must be defined after InlineHook structs so we can access trampolines.
// ===================================================================
static HKEY AllocFakeKey(FakeKeyType type)
{
    HKEY hReal = nullptr;
    auto pfn = reinterpret_cast<FN_RegOpenKeyExW>(s_hookRegOpenKeyExW.pTrampoline);
    if (pfn) {
        pfn(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ, &hReal);
    }
    if (!hReal) return nullptr;
    if (s_fakeKeyCount < MAX_FAKE_KEYS) {
        s_fakeKeys[s_fakeKeyCount].hKey = hReal;
        s_fakeKeys[s_fakeKeyCount].type = type;
        s_fakeKeyCount++;
    }
    return hReal;
}

static FakeKeyType GetFakeKeyType(HKEY hKey)
{
    for (int i = 0; i < s_fakeKeyCount; i++) {
        if (s_fakeKeys[i].hKey == hKey) return s_fakeKeys[i].type;
    }
    return FK_NONE;
}

static bool IsFakeHKey(HKEY hKey)
{
    return GetFakeKeyType(hKey) != FK_NONE;
}

static void CloseFakeKey(HKEY hKey)
{
    for (int i = 0; i < s_fakeKeyCount; i++) {
        if (s_fakeKeys[i].hKey == hKey) {
            auto pfn = reinterpret_cast<FN_RegCloseKey>(s_hookRegCloseKey.pTrampoline);
            if (pfn) pfn(hKey);
            s_fakeKeys[i] = s_fakeKeys[s_fakeKeyCount - 1];
            s_fakeKeyCount--;
            return;
        }
    }
}

static bool InstallDetour(void* targetFunc, void* hookFunc, InlineHook& hook)
{
    if (!targetFunc || !hookFunc) return false;

    // Determine how many prologue bytes to copy
    int copySize = CalcPrologueSize(static_cast<const BYTE*>(targetFunc), MIN_HOOK_SIZE);
    if (copySize == 0) return false;

    // Allocate executable trampoline: saved bytes + jump-back
    SIZE_T trampolineSize = copySize + MIN_HOOK_SIZE + 16; // extra padding
    hook.pTrampoline = VirtualAlloc(nullptr, trampolineSize,
                                    MEM_COMMIT | MEM_RESERVE,
                                    PAGE_EXECUTE_READWRITE);
    if (!hook.pTrampoline) return false;

    hook.pTarget  = targetFunc;
    hook.copySize = copySize;

    // Copy original prologue bytes to trampoline
    memcpy(hook.pTrampoline, targetFunc, copySize);

    // Append jump-back to (targetFunc + copySize)
    BYTE* jmpBack = static_cast<BYTE*>(hook.pTrampoline) + copySize;
    void* resumeAddr = static_cast<BYTE*>(targetFunc) + copySize;

#ifdef _WIN64
    // FF 25 00 00 00 00 [8-byte absolute address]
    jmpBack[0] = 0xFF;
    jmpBack[1] = 0x25;
    memset(jmpBack + 2, 0, 4);
    memcpy(jmpBack + 6, &resumeAddr, 8);
#else
    // E9 [4-byte relative offset]
    jmpBack[0] = 0xE9;
    intptr_t rel = reinterpret_cast<intptr_t>(resumeAddr)
                 - reinterpret_cast<intptr_t>(jmpBack + 5);
    int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(jmpBack + 1, &rel32, 4);
#endif

    // Patch the target function prologue
    DWORD oldProtect;
    if (!VirtualProtect(targetFunc, copySize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(hook.pTrampoline, 0, MEM_RELEASE);
        hook.pTrampoline = nullptr;
        return false;
    }

    BYTE* p = static_cast<BYTE*>(targetFunc);

#ifdef _WIN64
    // 14-byte absolute jump: FF 25 00 00 00 00 [8-byte address]
    p[0] = 0xFF;
    p[1] = 0x25;
    memset(p + 2, 0, 4);
    memcpy(p + 6, &hookFunc, 8);
    // NOP any remaining bytes so debuggers show clean disassembly
    for (int i = 14; i < copySize; i++) p[i] = 0x90;
#else
    // 5-byte relative jump: E9 [relative32]
    p[0] = 0xE9;
    intptr_t hookRel = reinterpret_cast<intptr_t>(hookFunc)
                     - reinterpret_cast<intptr_t>(p + 5);
    int32_t hookRel32 = static_cast<int32_t>(hookRel);
    memcpy(p + 1, &hookRel32, 4);
    for (int i = 5; i < copySize; i++) p[i] = 0x90;
#endif

    VirtualProtect(targetFunc, copySize, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), targetFunc, copySize);

    hook.active = true;
    return true;
}

static void RemoveDetour(InlineHook& hook)
{
    if (!hook.active) return;

    // Restore original prologue bytes
    DWORD oldProtect;
    if (VirtualProtect(hook.pTarget, hook.copySize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        memcpy(hook.pTarget, hook.pTrampoline, hook.copySize);
        VirtualProtect(hook.pTarget, hook.copySize, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), hook.pTarget, hook.copySize);
    }

    VirtualFree(hook.pTrampoline, 0, MEM_RELEASE);
    hook.pTrampoline = nullptr;
    hook.pTarget     = nullptr;
    hook.copySize    = 0;
    hook.active      = false;
}

// ===================================================================
// Hook: CoGetClassObject — intercept Flash CLSID
// ===================================================================
HRESULT STDAPICALLTYPE FlashLoader::Hooked_CoGetClassObject(
    REFCLSID rclsid, DWORD dwClsContext, LPVOID pvReserved,
    REFIID riid, LPVOID* ppv)
{
    if (s_pFlashFactory && IsEqualCLSID(rclsid, CLSID_ShockwaveFlash)) {
        // Return the logging wrapper so we can trace CreateInstance calls
        if (s_pLoggingFactory) {
            HRESULT hr = s_pLoggingFactory->QueryInterface(riid, ppv);
            DbgTrace(L"[FlashIE] CoGetClassObject(Flash) -> hr=0x%08X ppv=%p\n", hr, ppv ? *ppv : nullptr);
            return hr;
        }
        HRESULT hr = s_pFlashFactory->QueryInterface(riid, ppv);
        DbgTrace(L"[FlashIE] CoGetClassObject(Flash) -> hr=0x%08X ppv=%p\n", hr, ppv ? *ppv : nullptr);
        return hr;
    }
    HRESULT hr = reinterpret_cast<FN_CoGetClassObject>(s_hookCoGetClassObject.pTrampoline)(
        rclsid, dwClsContext, pvReserved, riid, ppv);
    DbgTrace(L"[FlashIE] CoGetClassObject({%08X-...}) -> hr=0x%08X\n", rclsid.Data1, hr);
    return hr;
}

// ===================================================================
// Hook: CoCreateInstance — intercept Flash CLSID (catches direct creation)
// ===================================================================
HRESULT STDAPICALLTYPE FlashLoader::Hooked_CoCreateInstance(
    REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext,
    REFIID riid, LPVOID* ppv)
{
    if (s_pFlashFactory && IsEqualCLSID(rclsid, CLSID_ShockwaveFlash)) {
        IClassFactory* pCF = s_pLoggingFactory ? static_cast<IClassFactory*>(s_pLoggingFactory) : s_pFlashFactory;
        HRESULT hr = pCF->CreateInstance(pUnkOuter, riid, ppv);
        DbgTrace(L"[FlashIE] CoCreateInstance(Flash) -> hr=0x%08X\n", hr);
        return hr;
    }
    HRESULT hr = reinterpret_cast<FN_CoCreateInstance>(s_hookCoCreateInstance.pTrampoline)(
        rclsid, pUnkOuter, dwClsContext, riid, ppv);
    // Log all CoCreateInstance calls for diagnostic purposes
    DbgTrace(L"[FlashIE] CoCreateInstance({%08X-...}) -> hr=0x%08X\n", rclsid.Data1, hr);
    return hr;
}

// ===================================================================
// Hook: RegOpenKeyExW — bypass Flash ActiveX kill bit
//
// Microsoft's Flash EOL update (KB4561600) sets a kill bit at:
//   HKLM\SOFTWARE\Microsoft\Internet Explorer\ActiveX Compatibility\
//     {D27CDB6E-AE6D-11CF-96B8-444553540000}
// MSHTML checks this BEFORE attempting any CoGetClassObject call.
// If set, Flash instantiation is blocked entirely. We intercept the
// registry open and pretend the key doesn't exist.
// ===================================================================
// Helper to check if a subkey path ends with a specific suffix (case-insensitive)
static bool SubKeyEndsWith(LPCWSTR lpSubKey, LPCWSTR suffix)
{
    size_t keyLen = wcslen(lpSubKey);
    size_t sufLen = wcslen(suffix);
    if (keyLen < sufLen) return false;
    return _wcsicmp(lpSubKey + keyLen - sufLen, suffix) == 0;
}

// Flash CLSID string for comparisons
static const wchar_t FLASH_CLSID_STR[] = L"{D27CDB6E-AE6D-11CF-96B8-444553540000}";
static const wchar_t FLASH_CLSID_UPPER[] = L"D27CDB6E-AE6D-11CF-96B8-444553540000";

LSTATUS WINAPI FlashLoader::Hooked_RegOpenKeyExW(
    HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions,
    REGSAM samDesired, PHKEY phkResult)
{
    // ---- Handle subkey opens under fake Flash HKEY handles ----
    if (IsFakeHKey(hKey) && lpSubKey && phkResult) {
        FakeKeyType parentType = GetFakeKeyType(hKey);
        FakeKeyType subType = FK_NONE;

        // Handle ProgID root -> CLSID / CurVer subkeys
        if (parentType == FK_PROGID_ROOT && _wcsicmp(lpSubKey, L"CLSID") == 0)
            subType = FK_PROGID_CLSID;
        else if (parentType == FK_PROGID_ROOT && _wcsicmp(lpSubKey, L"CurVer") == 0)
            subType = FK_PROGID_CURVER;
        else if (_wcsicmp(lpSubKey, L"InprocServer32") == 0 || _wcsicmp(lpSubKey, L"InProcServer32") == 0)
            subType = FK_INPROC;
        else if (_wcsicmp(lpSubKey, L"MiscStatus") == 0)
            subType = FK_MISCSTATUS;
        else if (_wcsicmp(lpSubKey, L"MiscStatus\\1") == 0 || _wcsicmp(lpSubKey, L"1") == 0)
            subType = FK_MISCSTATUS1;
        else if (_wcsicmp(lpSubKey, L"ProgID") == 0)
            subType = FK_PROGID;
        else if (_wcsicmp(lpSubKey, L"TypeLib") == 0)
            subType = FK_TYPELIB;
        else if (_wcsicmp(lpSubKey, L"Control") == 0)
            subType = FK_CONTROL;
        else if (_wcsicmp(lpSubKey, L"Version") == 0 || _wcsicmp(lpSubKey, L"VersionIndependentProgID") == 0)
            subType = FK_VERSION;
        else if (_wcsicmp(lpSubKey, L"CurVer") == 0)
            subType = FK_PROGID_CURVER;
        else if (_wcsicmp(lpSubKey, L"InstalledVersion") == 0)
            subType = FK_INSTALLED_VER;

        if (subType != FK_NONE) {
            HKEY h = AllocFakeKey(subType);
            if (h) { *phkResult = h; return ERROR_SUCCESS; }
        }
        DbgTrace(L"[FlashIE] RegOpenKeyExW FAKE parent, unknown subkey: %s\n", lpSubKey);
        return ERROR_FILE_NOT_FOUND;
    }

    if (lpSubKey) {
        // ---- Flash kill bit bypass ----
        if (wcsstr(lpSubKey, L"D27CDB6E") || wcsstr(lpSubKey, L"d27cdb6e")) {
            // Block ActiveX Compatibility and Extension Compatibility (kill bit)
            if (wcsstr(lpSubKey, L"ActiveX Compatibility") ||
                wcsstr(lpSubKey, L"Extension Compatibility")) {
                DbgTrace(L"[FlashIE] RegOpenKeyExW BLOCKED (kill bit): %s\n", lpSubKey);
                LazyPatchModules();
                return ERROR_FILE_NOT_FOUND;
            }
        }

        // ---- Fake Flash ProgID registration ----
        // JavaScript "new ActiveXObject('ShockwaveFlash.ShockwaveFlash')" triggers
        // lookup of HKCR\ShockwaveFlash.ShockwaveFlash and its \CLSID subkey.
        // We match unversioned and versioned (up to .34) ProgIDs.
        if (_wcsnicmp(lpSubKey, L"ShockwaveFlash.ShockwaveFlash", 29) == 0 ||
            (wcsstr(lpSubKey, L"ShockwaveFlash.ShockwaveFlash") != nullptr)) {
            // Check versioned ProgID: reject versions > 34
            // e.g. "ShockwaveFlash.ShockwaveFlash.40" should fail
            bool versionOk = true;
            {
                // Find the ProgID portion (might have \CLSID or \CurVer suffix)
                const wchar_t* progid = wcsstr(lpSubKey, L"ShockwaveFlash.ShockwaveFlash");
                if (progid) {
                    const wchar_t* afterBase = progid + 29; // after "ShockwaveFlash.ShockwaveFlash"
                    if (*afterBase == L'.') {
                        // Versioned: parse the number
                        int ver = _wtoi(afterBase + 1);
                        if (ver > 34) versionOk = false;
                    }
                }
            }
            if (versionOk && phkResult) {
                // Check if it's the \CLSID subkey or the root
                if (wcsstr(lpSubKey, L"\\CLSID") || SubKeyEndsWith(lpSubKey, L"CLSID")) {
                    HKEY h = AllocFakeKey(FK_PROGID_CLSID);
                    if (h) {
                        *phkResult = h;
                        DbgTrace(L"[FlashIE] RegOpenKeyExW FAKE: %s -> ProgID\\CLSID\n", lpSubKey);
                        return ERROR_SUCCESS;
                    }
                } else {
                    HKEY h = AllocFakeKey(FK_PROGID_ROOT);
                    if (h) {
                        *phkResult = h;
                        DbgTrace(L"[FlashIE] RegOpenKeyExW FAKE: %s -> ProgID root\n", lpSubKey);
                        return ERROR_SUCCESS;
                    }
                }
            }
        }

        // ---- Fake Flash CLSID registration ----
        // MSHTML looks up HKCR\CLSID\{D27CDB6E-...} to check if the control
        // is installed. We fake the entire CLSID tree so MSHTML proceeds.

        // ---- Fake Flash CLSID subkeys ----
        if (wcsstr(lpSubKey, L"D27CDB6E") || wcsstr(lpSubKey, L"d27cdb6e")) {
            FakeKeyType fkType = FK_NONE;
            const wchar_t* fkName = nullptr;

            if (SubKeyEndsWith(lpSubKey, FLASH_CLSID_STR) &&
                (wcsstr(lpSubKey, L"CLSID") || wcsstr(lpSubKey, L"clsid"))) {
                fkType = FK_CLSID_ROOT; fkName = L"CLSID root";
                LazyPatchModules();
            } else if (SubKeyEndsWith(lpSubKey, L"InprocServer32") || SubKeyEndsWith(lpSubKey, L"InProcServer32")) {
                fkType = FK_INPROC; fkName = L"InprocServer32";
            } else if (SubKeyEndsWith(lpSubKey, L"MiscStatus\\1")) {
                fkType = FK_MISCSTATUS1; fkName = L"MiscStatus\\1";
            } else if (SubKeyEndsWith(lpSubKey, L"MiscStatus")) {
                fkType = FK_MISCSTATUS; fkName = L"MiscStatus";
            } else if (SubKeyEndsWith(lpSubKey, L"ProgID")) {
                fkType = FK_PROGID; fkName = L"ProgID";
            } else if (SubKeyEndsWith(lpSubKey, L"TypeLib")) {
                fkType = FK_TYPELIB; fkName = L"TypeLib";
            } else if (SubKeyEndsWith(lpSubKey, L"Control")) {
                fkType = FK_CONTROL; fkName = L"Control";
            } else if (SubKeyEndsWith(lpSubKey, L"Version") || SubKeyEndsWith(lpSubKey, L"VersionIndependentProgID")) {
                fkType = FK_VERSION; fkName = L"Version";
            }

            if (fkType != FK_NONE && phkResult) {
                HKEY h = AllocFakeKey(fkType);
                if (h) {
                    *phkResult = h;
                    DbgTrace(L"[FlashIE] RegOpenKeyExW FAKE: %s -> %s\n", lpSubKey, fkName);
                    return ERROR_SUCCESS;
                }
            }
        }

        // Any other D27CDB6E path — log and let through (or fake)
        if (wcsstr(lpSubKey, L"D27CDB6E") || wcsstr(lpSubKey, L"d27cdb6e")) {
            DbgTrace(L"[FlashIE] RegOpenKeyExW ALLOW (Flash): %s\n", lpSubKey);
            LazyPatchModules();
        }

        // ---- Fake MIME type -> CLSID mapping ----
        // MSHTML resolves <embed type="application/x-shockwave-flash">
        // via HKCR\MIME\Database\Content Type\application/x-shockwave-flash
        // IMPORTANT: Only match actual MIME database paths, NOT protocol
        // filter paths (PROTOCOLS\Filter\...) or shell association paths.
        if ((wcsstr(lpSubKey, L"x-shockwave-flash") ||
             wcsstr(lpSubKey, L"x-Shockwave-Flash")) &&
            wcsstr(lpSubKey, L"Content Type")) {
            HKEY h = AllocFakeKey(FK_MIME);
            if (h && phkResult) *phkResult = h;
            DbgTrace(L"[FlashIE] RegOpenKeyExW FAKE: %s -> MIME mapping\n", lpSubKey);
            return ERROR_SUCCESS;
        }

        // ---- FEATURE_BROWSER_EMULATION tracking ----
        if (wcsstr(lpSubKey, L"FEATURE_BROWSER_EMULATION")) {
            LSTATUS res = reinterpret_cast<FN_RegOpenKeyExW>(
                s_hookRegOpenKeyExW.pTrampoline)(
                hKey, lpSubKey, ulOptions, samDesired, phkResult);
            if (res == ERROR_SUCCESS && phkResult)
                s_hkeyBrowserEmulation = *phkResult;
            return res;
        }
    }
    LSTATUS res = reinterpret_cast<FN_RegOpenKeyExW>(s_hookRegOpenKeyExW.pTrampoline)(
        hKey, lpSubKey, ulOptions, samDesired, phkResult);
    // Log Flash-related registry access for debugging
    if (lpSubKey && (wcsstr(lpSubKey, L"Macr") || wcsstr(lpSubKey, L"Flash") ||
                     wcsstr(lpSubKey, L"flash") || wcsstr(lpSubKey, L"ShockwaveFlash") ||
                     wcsstr(lpSubKey, L"mms.cfg"))) {
        DbgTrace(L"[FlashIE] RegOpenKeyExW FLASH-RELATED: %s -> 0x%X\n", lpSubKey, res);
    }
    return res;
}

// ===================================================================
// Hook: RegQueryValueExW — belt-and-suspenders kill bit bypass.
// If CompatFlagsFromClsid already has the key open (cached handle),
// it reads "Compatibility Flags". We intercept the value read and
// return 0 (no flags) instead of 0x400 (COMPAT_EVIL_DONT_LOAD).
// ===================================================================
// Helper: fill a REG_SZ value into the query result buffer
static LSTATUS FakeRegSz(LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData, const wchar_t* val)
{
    DWORD needed = (DWORD)((wcslen(val) + 1) * sizeof(wchar_t));
    if (lpType) *lpType = REG_SZ;
    if (lpcbData) {
        DWORD avail = *lpcbData;
        *lpcbData = needed;
        if (lpData) {
            if (avail >= needed)
                memcpy(lpData, val, needed);
            else
                return ERROR_MORE_DATA;
        }
    }
    return ERROR_SUCCESS;
}

static LSTATUS FakeRegDword(LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData, DWORD val)
{
    if (lpType) *lpType = REG_DWORD;
    if (lpcbData) {
        if (lpData && *lpcbData >= sizeof(DWORD))
            memcpy(lpData, &val, sizeof(DWORD));
        *lpcbData = sizeof(DWORD);
    }
    return ERROR_SUCCESS;
}

LSTATUS WINAPI FlashLoader::Hooked_RegQueryValueExW(
    HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved,
    LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    // ---- Fake Flash CLSID registration values ----
    FakeKeyType fkt = GetFakeKeyType(hKey);
    if (fkt != FK_NONE) {
        switch (fkt) {
        case FK_CLSID_ROOT:
            if (!lpValueName || lpValueName[0] == 0) {
                DbgTrace(L"[FlashIE] RegQueryValueExW FAKE: CLSID default\n");
                return FakeRegSz(lpType, lpData, lpcbData, L"Shockwave Flash Object");
            }
            if (_wcsicmp(lpValueName, L"InstalledVersion") == 0) {
                DbgTrace(L"[FlashIE] RegQueryValueExW FAKE: InstalledVersion -> 34,0,0,330\n");
                return FakeRegSz(lpType, lpData, lpcbData, L"34,0,0,330");
            }
            if (_wcsicmp(lpValueName, L"Version") == 0) {
                DbgTrace(L"[FlashIE] RegQueryValueExW FAKE: Version -> 34.0\n");
                return FakeRegSz(lpType, lpData, lpcbData, L"34.0");
            }
            DbgTrace(L"[FlashIE] RegQueryValueExW FAKE: CLSID\\%s\n", lpValueName);
            return ERROR_FILE_NOT_FOUND;

        case FK_INPROC:
            if (!lpValueName || lpValueName[0] == 0) {
                DbgTrace(L"[FlashIE] RegQueryValueExW FAKE: InprocServer32 -> %s\n", g_szOcxPath);
                return FakeRegSz(lpType, lpData, lpcbData, g_szOcxPath);
            }
            if (_wcsicmp(lpValueName, L"ThreadingModel") == 0) {
                DbgTrace(L"[FlashIE] RegQueryValueExW FAKE: ThreadingModel -> Apartment\n");
                return FakeRegSz(lpType, lpData, lpcbData, L"Apartment");
            }
            return ERROR_FILE_NOT_FOUND;

        case FK_MISCSTATUS:
            if (!lpValueName || lpValueName[0] == 0) {
                DbgTrace(L"[FlashIE] RegQueryValueExW FAKE: MiscStatus -> 131473\n");
                return FakeRegSz(lpType, lpData, lpcbData, L"131473");
            }
            return ERROR_FILE_NOT_FOUND;

        case FK_MISCSTATUS1:
            if (!lpValueName || lpValueName[0] == 0) {
                DbgTrace(L"[FlashIE] RegQueryValueExW FAKE: MiscStatus\\1 -> 131473\n");
                return FakeRegSz(lpType, lpData, lpcbData, L"131473");
            }
            return ERROR_FILE_NOT_FOUND;

        case FK_PROGID:
            if (!lpValueName || lpValueName[0] == 0)
                return FakeRegSz(lpType, lpData, lpcbData, L"ShockwaveFlash.ShockwaveFlash.32");
            return ERROR_FILE_NOT_FOUND;

        case FK_TYPELIB:
            if (!lpValueName || lpValueName[0] == 0)
                return FakeRegSz(lpType, lpData, lpcbData, L"{D27CDB6B-AE6D-11CF-96B8-444553540000}");
            return ERROR_FILE_NOT_FOUND;

        case FK_CONTROL:
            return ERROR_FILE_NOT_FOUND;

        case FK_VERSION:
            if (!lpValueName || lpValueName[0] == 0)
                return FakeRegSz(lpType, lpData, lpcbData, L"ShockwaveFlash.ShockwaveFlash");
            return ERROR_FILE_NOT_FOUND;

        case FK_PROGID_ROOT:
            // Default value of ShockwaveFlash.ShockwaveFlash
            if (!lpValueName || lpValueName[0] == 0)
                return FakeRegSz(lpType, lpData, lpcbData, L"Shockwave Flash");
            return ERROR_FILE_NOT_FOUND;

        case FK_PROGID_CLSID:
            // Default value returns the Flash CLSID
            if (!lpValueName || lpValueName[0] == 0) {
                DbgTrace(L"[FlashIE] RegQueryValueExW FAKE: ProgID\\CLSID -> %s\n", FLASH_CLSID_STR);
                return FakeRegSz(lpType, lpData, lpcbData, FLASH_CLSID_STR);
            }
            return ERROR_FILE_NOT_FOUND;

        case FK_PROGID_CURVER:
            // Default value returns the versioned ProgID
            if (!lpValueName || lpValueName[0] == 0) {
                DbgTrace(L"[FlashIE] RegQueryValueExW FAKE: CurVer -> ShockwaveFlash.ShockwaveFlash.34\n");
                return FakeRegSz(lpType, lpData, lpcbData, L"ShockwaveFlash.ShockwaveFlash.34");
            }
            return ERROR_FILE_NOT_FOUND;

        case FK_INSTALLED_VER:
            // Default value returns comma-separated version for codebase version check
            if (!lpValueName || lpValueName[0] == 0) {
                DbgTrace(L"[FlashIE] RegQueryValueExW FAKE: InstalledVersion -> 34,0,0,330\n");
                return FakeRegSz(lpType, lpData, lpcbData, L"34,0,0,330");
            }
            return ERROR_FILE_NOT_FOUND;

        case FK_MIME:
            if (lpValueName && _wcsicmp(lpValueName, L"CLSID") == 0) {
                DbgTrace(L"[FlashIE] RegQueryValueExW FAKE: MIME CLSID -> %s\n", FLASH_CLSID_STR);
                return FakeRegSz(lpType, lpData, lpcbData, FLASH_CLSID_STR);
            }
            if (lpValueName && _wcsicmp(lpValueName, L"Extension") == 0)
                return FakeRegSz(lpType, lpData, lpcbData, L".swf");
            return ERROR_FILE_NOT_FOUND;

        default:
            break;
        }
    }

    // ---- FEATURE_BROWSER_EMULATION ----
    if (s_hkeyBrowserEmulation && hKey == s_hkeyBrowserEmulation &&
        lpValueName && s_szExeName[0] && _wcsicmp(lpValueName, s_szExeName) == 0) {
        DbgTrace(L"[FlashIE] RegQueryValueExW: FEATURE_BROWSER_EMULATION -> 11001\n");
        return FakeRegDword(lpType, lpData, lpcbData, 11001);
    }

    // ---- Clear kill bit from Compatibility Flags ----
    if (lpValueName && _wcsicmp(lpValueName, L"Compatibility Flags") == 0) {
        LSTATUS res = reinterpret_cast<FN_RegQueryValueExW>(
            s_hookRegQueryValueExW.pTrampoline)(
            hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
        if (res == ERROR_SUCCESS && lpData && lpcbData && *lpcbData >= sizeof(DWORD)) {
            DWORD val = *reinterpret_cast<DWORD*>(lpData);
            if (val & 0x400) {
                DbgTrace(L"[FlashIE] RegQueryValueExW: cleared kill bit (was 0x%X)\n", val);
                *reinterpret_cast<DWORD*>(lpData) = val & ~0x400;
            }
        }
        return res;
    }

    return reinterpret_cast<FN_RegQueryValueExW>(s_hookRegQueryValueExW.pTrampoline)(
        hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
}

// ===================================================================
// Hook: RegCloseKey — silently handle fake HKEY values
// ===================================================================
LSTATUS WINAPI FlashLoader::Hooked_RegCloseKey(HKEY hKey)
{
    if (IsFakeHKey(hKey)) {
        CloseFakeKey(hKey);
        return ERROR_SUCCESS;
    }
    return reinterpret_cast<FN_RegCloseKey>(s_hookRegCloseKey.pTrampoline)(hKey);
}

// ===================================================================
// Hook: CoGetClassObjectFromURL — intercept Flash CLSID on the normal
// MSHTML code path (without DLCTL_NO_DLACTIVEXCTLS).
// When MSHTML loads an ActiveX control from an <object> tag, it calls
// CoGetClassObjectFromURL (urlmon.dll). We intercept it for Flash CLSID
// and return our locally-loaded factory directly.
// ===================================================================
HRESULT STDAPICALLTYPE FlashLoader::Hooked_CoGetClassObjectFromURL(
    REFCLSID rclsid, LPCWSTR szCodeURL,
    DWORD dwFileVersionMS, DWORD dwFileVersionLS,
    LPCWSTR szContentType, LPBINDCTX pBindCtx,
    DWORD dwClsContext, LPVOID pvReserved,
    REFIID riid, LPVOID* ppv)
{
    if (s_pFlashFactory && IsEqualCLSID(rclsid, CLSID_ShockwaveFlash)) {
        if (s_pLoggingFactory) {
            HRESULT hr = s_pLoggingFactory->QueryInterface(riid, ppv);
            DbgTrace(L"[FlashIE] CoGetClassObjectFromURL(Flash) -> hr=0x%08X\n", hr);
            return hr;
        }
        HRESULT hr = s_pFlashFactory->QueryInterface(riid, ppv);
        DbgTrace(L"[FlashIE] CoGetClassObjectFromURL(Flash) -> hr=0x%08X\n", hr);
        return hr;
    }
    return reinterpret_cast<FN_CoGetClassObjectFromURL>(
        s_hookCoGetClassObjectFromURL.pTrampoline)(
        rclsid, szCodeURL, dwFileVersionMS, dwFileVersionLS,
        szContentType, pBindCtx, dwClsContext, pvReserved, riid, ppv);
}

// ===================================================================
// Hook: CoInternetIsFeatureEnabled — disable all IE Feature Controls.
// MSHTML checks features like FEATURE_RESTRICT_ACTIVEXINSTALL and
// FEATURE_SAFE_BINDTOOBJECT before allowing ActiveX. Returning S_FALSE
// means "feature not enabled" which allows the action to proceed.
// ===================================================================
HRESULT STDAPICALLTYPE FlashLoader::Hooked_CoInternetIsFeatureEnabled(
    DWORD dwFeature, DWORD dwFlags)
{
    // Patch mshtml.dll on first call (it's loaded by now)
    LazyPatchModules();

    // Log ALL feature checks so we can see what MSHTML is checking
    HRESULT hrOrig = reinterpret_cast<FN_CoInternetIsFeatureEnabled>(
        s_hookCoInternetIsFeatureEnabled.pTrampoline)(dwFeature, dwFlags);

    // Disable ALL features that could block ActiveX loading.
    // S_OK = feature enabled (blocks), S_FALSE = feature not enabled (allows).
    // We return S_FALSE for all features to maximally allow ActiveX.
    // Only log non-spammy features (skip feature 0 = OBJECT_CACHING which fires thousands of times)
    static int s_featureLogCount = 0;
    if (dwFeature != 0 && s_featureLogCount < 50) {
        s_featureLogCount++;
        DbgTrace(L"[FlashIE] CoInternetIsFeatureEnabled(feature=%u, flags=0x%X) orig=0x%08X -> S_FALSE\n",
                 dwFeature, dwFlags, hrOrig);
    }
    return S_FALSE;
}

// ===================================================================
// Hook: GetLocalTime / GetSystemTime — bypass Flash.ocx EOL kill switch.
//
// Flash Player 32.0.0.465 (the final version) has a hardcoded date check:
// after January 12, 2021, Flash refuses to play ANY content and shows
// an EOL notification. We intercept GetLocalTime/GetSystemTime and
// return a pre-EOL date when called from within Flash.ocx.
// ===================================================================
// Cached Flash.ocx address range for fast caller check
static BYTE* s_flashBase = nullptr;
static DWORD s_flashSize = 0;

static void CacheFlashModuleRange(HMODULE hFlash)
{
    if (!hFlash) return;
    auto pDos = reinterpret_cast<PIMAGE_DOS_HEADER>(hFlash);
    if (pDos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto pNT = reinterpret_cast<PIMAGE_NT_HEADERS>(
        reinterpret_cast<BYTE*>(hFlash) + pDos->e_lfanew);
    if (pNT->Signature != IMAGE_NT_SIGNATURE) return;
    s_flashBase = reinterpret_cast<BYTE*>(hFlash);
    s_flashSize = pNT->OptionalHeader.SizeOfImage;
}

static __forceinline bool IsCallerInFlash(void* retAddr)
{
    if (!s_flashBase) return false;
    BYTE* addr = reinterpret_cast<BYTE*>(retAddr);
    // Use subtraction to avoid pointer overflow on 32-bit
    return (addr >= s_flashBase && static_cast<DWORD>(addr - s_flashBase) < s_flashSize);
}

void WINAPI FlashLoader::Hooked_GetLocalTime(LPSYSTEMTIME lpSystemTime)
{
    reinterpret_cast<FN_GetLocalTime>(s_hookGetLocalTime.pTrampoline)(lpSystemTime);
    if (IsCallerInFlash(_ReturnAddress())) {
        static int s_gltCount = 0;
        if (s_gltCount++ < 3)
            DbgTrace(L"[FlashIE] GetLocalTime from Flash -> spoofing to 2020-12-01 (call #%d)\n", s_gltCount);
        lpSystemTime->wYear = 2020;
        lpSystemTime->wMonth = 12;
        lpSystemTime->wDay = 1;
    }
}

void WINAPI FlashLoader::Hooked_GetSystemTime(LPSYSTEMTIME lpSystemTime)
{
    reinterpret_cast<FN_GetSystemTime>(s_hookGetSystemTime.pTrampoline)(lpSystemTime);
    if (IsCallerInFlash(_ReturnAddress())) {
        static int s_gstCount = 0;
        if (s_gstCount++ < 3)
            DbgTrace(L"[FlashIE] GetSystemTime from Flash -> spoofing to 2020-12-01 (call #%d)\n", s_gstCount);
        lpSystemTime->wYear = 2020;
        lpSystemTime->wMonth = 12;
        lpSystemTime->wDay = 1;
    }
}

// ===================================================================
// Hook: GetSystemTimeAsFileTime — Flash.ocx 32.0.0.465 may use this
// instead of GetLocalTime/GetSystemTime for its EOL date check.
// We intercept and return a pre-EOL date when called from Flash.ocx.
// ===================================================================
static void WINAPI Hooked_GetSystemTimeAsFileTime(LPFILETIME lpFileTime)
{
    reinterpret_cast<FN_GetSystemTimeAsFileTime>(
        s_hookGetSystemTimeAsFileTime.pTrampoline)(lpFileTime);
    if (IsCallerInFlash(_ReturnAddress())) {
        // Convert 2020-12-01 00:00:00 UTC to FILETIME
        SYSTEMTIME st = {};
        st.wYear = 2020; st.wMonth = 12; st.wDay = 1;
        FILETIME ft;
        SystemTimeToFileTime(&st, &ft);
        *lpFileTime = ft;
        DbgTrace(L"[FlashIE] GetSystemTimeAsFileTime from Flash -> spoofing to 2020-12-01\n");
    }
}

// ===================================================================
// Hook: WldpIsClassInApprovedList — approve Flash CLSID.
//
// Windows 10+ MSHTML calls wldp!WldpIsClassInApprovedList to check if
// an ActiveX CLSID is approved for instantiation. If the function
// returns *isApproved = FALSE, MSHTML blocks the control BEFORE
// calling CoGetClassObject. We intercept and approve everything.
// ===================================================================
HRESULT WINAPI FlashLoader::Hooked_WldpIsClassInApprovedList(
    const CLSID* classID, void* hostInfo, BOOL* isApproved, DWORD optionalFlags)
{
    if (classID)
        DbgTrace(L"[FlashIE] WldpIsClassInApprovedList({%08X-...}) -> approved\n", classID->Data1);
    else
        DbgTrace(L"[FlashIE] WldpIsClassInApprovedList(null) -> approved\n");

    if (isApproved) *isApproved = TRUE;
    return S_OK;
}

// ===================================================================
// Hook: WldpQueryDynamicCodeTrust — trust all dynamic code.
// ===================================================================
HRESULT WINAPI FlashLoader::Hooked_WldpQueryDynamicCodeTrust(
    HANDLE fileHandle, void* baseImage, DWORD imageSize)
{
    DbgTrace(L"[FlashIE] WldpQueryDynamicCodeTrust -> S_OK (trusted)\n");
    return S_OK;
}

// ===================================================================
// Hook: LoadRegTypeLib — redirect Flash TypeLib loading to the OCX file.
//
// Flash.ocx's TypeLib GUID is {D27CDB6B-AE6D-11CF-96B8-444553540000}.
// Without registry entries, OLEAUT32 can't find it and returns
// TYPE_E_LIBNOTREGISTERED (0x8002801D). We intercept and load from
// the OCX file directly using LoadTypeLibEx.
// ===================================================================
static const GUID GUID_FlashTypeLib =
    {0xD27CDB6B, 0xAE6D, 0x11CF, {0x96, 0xB8, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

static HRESULT WINAPI Hooked_LoadRegTypeLib(
    REFGUID rguid, WORD wVerMajor, WORD wVerMinor, LCID lcid, ITypeLib** pptlib)
{
    if (IsEqualGUID(rguid, GUID_FlashTypeLib) && pptlib) {
        // Load TypeLib directly from Flash.ocx
        static FN_LoadTypeLibEx s_pfnLoadTypeLibEx = nullptr;
        if (!s_pfnLoadTypeLibEx) {
            HMODULE hOleAut = GetModuleHandleW(L"oleaut32.dll");
            if (hOleAut)
                s_pfnLoadTypeLibEx = reinterpret_cast<FN_LoadTypeLibEx>(
                    GetProcAddress(hOleAut, "LoadTypeLibEx"));
        }
        if (s_pfnLoadTypeLibEx) {
            HRESULT hr = s_pfnLoadTypeLibEx(g_szOcxPath, REGKIND_NONE, pptlib);
            DbgTrace(L"[FlashIE] LoadRegTypeLib(FlashTypeLib) -> LoadTypeLibEx(%s) hr=0x%08X\n",
                     g_szOcxPath, hr);
            return hr;
        }
    }
    HRESULT hr = reinterpret_cast<FN_LoadRegTypeLib>(
        s_hookLoadRegTypeLib.pTrampoline)(rguid, wVerMajor, wVerMinor, lcid, pptlib);
    if (FAILED(hr)) {
        DbgTrace(L"[FlashIE] LoadRegTypeLib({%08X-...}) v%u.%u -> 0x%08X\n",
                 rguid.Data1, wVerMajor, wVerMinor, hr);
    }
    return hr;
}

// ===================================================================
// Flash data path redirection helper.
// Checks if a path starts with the roaming Flash Player dir and
// redirects it to our local FlashData directory.
// Returns true if redirected (and writes new path to outBuf).
// ===================================================================
static bool RedirectFlashDataPath(LPCWSTR lpFileName, wchar_t* outBuf, int outBufLen)
{
    if (!g_szFlashDataDir[0]) return false;

    // Strip \\?\ extended-length path prefix if present
    LPCWSTR path = lpFileName;
    if (wcsncmp(path, L"\\\\?\\", 4) == 0)
        path += 4;

    // Case-insensitive prefix match against "%APPDATA%\Macromedia\Flash Player"
    if (g_roamingFlashDirLen &&
        _wcsnicmp(path, g_szRoamingFlashDir, g_roamingFlashDirLen) == 0) {
        const wchar_t* suffix = path + g_roamingFlashDirLen;
        _snwprintf_s(outBuf, outBufLen, _TRUNCATE, L"%s%s", g_szFlashDataDir, suffix);
        return true;
    }

    // Case-insensitive prefix match against "%APPDATA%\Adobe\Flash Player"
    if (g_roamingAdobeDirLen &&
        _wcsnicmp(path, g_szRoamingAdobeDir, g_roamingAdobeDirLen) == 0) {
        const wchar_t* suffix = path + g_roamingAdobeDirLen;
        _snwprintf_s(outBuf, outBufLen, _TRUNCATE, L"%s%s", g_szFlashDataDir, suffix);
        return true;
    }

    // Also catch SysWOW64\Macromed\Flash paths (ss.cfg, ss.sgn, etc.)
    const wchar_t* p = wcsstr(path, L"Macromed\\Flash\\");
    if (!p) p = wcsstr(path, L"macromed\\Flash\\");
    if (p) {
        p += 15; // skip "Macromed\Flash\"
        _snwprintf_s(outBuf, outBufLen, _TRUNCATE, L"%s\\%s", g_szFlashDataDir, p);
        return true;
    }

    return false;
}

// Ensure all parent directories of a path exist (for redirected paths)
static void EnsureParentDirExists(const wchar_t* filePath)
{
    wchar_t dir[MAX_PATH];
    wcsncpy_s(dir, filePath, _TRUNCATE);
    PathRemoveFileSpecW(dir);

    // If it already exists, done
    DWORD attr = GetFileAttributesW(dir);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
        return;

    // Build from FlashData root down
    if (_wcsnicmp(dir, g_szFlashDataDir, g_flashDataDirLen) != 0)
        return;

    // Walk the suffix and create each level
    wchar_t build[MAX_PATH];
    wcscpy_s(build, g_szFlashDataDir);
    const wchar_t* rest = dir + g_flashDataDirLen;
    while (*rest == L'\\') rest++;

    wchar_t* ctx = nullptr;
    wchar_t restCopy[MAX_PATH];
    wcscpy_s(restCopy, rest);
    wchar_t* tok = wcstok_s(restCopy, L"\\", &ctx);
    while (tok) {
        wcscat_s(build, L"\\");
        wcscat_s(build, tok);
        CreateDirectoryW(build, nullptr);
        tok = wcstok_s(nullptr, L"\\", &ctx);
    }
}

// ===================================================================
// Hook: CreateFileW — redirect Flash data paths to local FlashData
// directory, redirect mms.cfg, and log Flash-related file access.
// ===================================================================
static HANDLE WINAPI Hooked_CreateFileW(
    LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    auto orig = reinterpret_cast<FN_CreateFileW>(s_hookCreateFileW.pTrampoline);

    if (lpFileName) {
        // Redirect mms.cfg reads to our local copy (catches \\?\ prefixed paths too)
        if (wcsstr(lpFileName, L"mms.cfg") || wcsstr(lpFileName, L"MMS.CFG")) {
            if (g_szMmsCfgPath[0]) {
                DbgTrace(L"[FlashIE] CreateFileW: REDIRECTING %s -> %s\n",
                          lpFileName, g_szMmsCfgPath);
                return orig(g_szMmsCfgPath, dwDesiredAccess, dwShareMode,
                           lpSecurityAttributes, dwCreationDisposition,
                           dwFlagsAndAttributes, hTemplateFile);
            }
        }

        // Redirect Flash Player data paths to local FlashData directory
        wchar_t redirected[MAX_PATH];
        if (RedirectFlashDataPath(lpFileName, redirected, MAX_PATH)) {
            // Auto-create parent directories for write operations
            if (dwCreationDisposition == CREATE_ALWAYS ||
                dwCreationDisposition == CREATE_NEW ||
                dwCreationDisposition == OPEN_ALWAYS ||
                (dwDesiredAccess & GENERIC_WRITE)) {
                EnsureParentDirExists(redirected);
            }
            HANDLE h = orig(redirected, dwDesiredAccess, dwShareMode,
                           lpSecurityAttributes, dwCreationDisposition,
                           dwFlagsAndAttributes, hTemplateFile);
            static int s_redirectLogCount = 0;
            if (s_redirectLogCount < 200) {
                s_redirectLogCount++;
                DbgTrace(L"[FlashIE] CreateFileW REDIRECT: %s -> %s (%s)\n",
                          lpFileName, redirected,
                          (h != INVALID_HANDLE_VALUE) ? L"OK" : L"FAILED");
            }
            return h;
        }

        // Log Flash-related file access (SWF, Flash, Macromed)
        if (wcsstr(lpFileName, L".swf") || wcsstr(lpFileName, L".SWF") ||
            wcsstr(lpFileName, L"Flash") || wcsstr(lpFileName, L"flash") ||
            wcsstr(lpFileName, L"Macromed") || wcsstr(lpFileName, L"macromed")) {
            HANDLE h = orig(lpFileName, dwDesiredAccess, dwShareMode,
                           lpSecurityAttributes, dwCreationDisposition,
                           dwFlagsAndAttributes, hTemplateFile);
            DbgTrace(L"[FlashIE] CreateFileW: %s -> %s\n",
                      lpFileName,
                      (h != INVALID_HANDLE_VALUE) ? L"OK" : L"FAILED");
            return h;
        }
    }

    return orig(lpFileName, dwDesiredAccess, dwShareMode,
               lpSecurityAttributes, dwCreationDisposition,
               dwFlagsAndAttributes, hTemplateFile);
}

// ===================================================================
// Hook: CreateDirectoryW — redirect Flash data directory creation
// ===================================================================
static BOOL WINAPI Hooked_CreateDirectoryW(
    LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes)
{
    auto orig = reinterpret_cast<FN_CreateDirectoryW>(s_hookCreateDirectoryW.pTrampoline);

    if (lpPathName) {
        wchar_t redirected[MAX_PATH];
        if (RedirectFlashDataPath(lpPathName, redirected, MAX_PATH)) {
            BOOL ok = orig(redirected, lpSecurityAttributes);
            static int s_mkdirLogCount = 0;
            if (s_mkdirLogCount < 50) {
                s_mkdirLogCount++;
                DbgTrace(L"[FlashIE] CreateDirectoryW REDIRECT: %s -> %s (%s)\n",
                          lpPathName, redirected,
                          ok ? L"OK" : L"FAILED/EXISTS");
            }
            return ok;
        }
    }

    return orig(lpPathName, lpSecurityAttributes);
}

// ===================================================================
// Hook: GetFileAttributesW — redirect Flash data path queries
// ===================================================================
static DWORD WINAPI Hooked_GetFileAttributesW(LPCWSTR lpFileName)
{
    auto orig = reinterpret_cast<FN_GetFileAttributesW>(s_hookGetFileAttributesW.pTrampoline);

    if (lpFileName) {
        wchar_t redirected[MAX_PATH];
        if (RedirectFlashDataPath(lpFileName, redirected, MAX_PATH)) {
            return orig(redirected);
        }
    }

    return orig(lpFileName);
}

// ===================================================================
// Hook: FindFirstFileW — redirect Flash data path enumeration
// ===================================================================
static HANDLE WINAPI Hooked_FindFirstFileW(
    LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData)
{
    auto orig = reinterpret_cast<decltype(&FindFirstFileW)>(s_hookFindFirstFileW.pTrampoline);

    if (lpFileName) {
        wchar_t redirected[MAX_PATH];
        if (RedirectFlashDataPath(lpFileName, redirected, MAX_PATH)) {
            return orig(redirected, lpFindFileData);
        }
    }

    return orig(lpFileName, lpFindFileData);
}

// ===================================================================
// Hook: MoveFileW — redirect Flash data file renames (.sxx -> .sol)
// ===================================================================
static BOOL WINAPI Hooked_MoveFileW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName)
{
    auto orig = reinterpret_cast<FN_MoveFileW>(s_hookMoveFileW.pTrampoline);

    wchar_t redSrc[MAX_PATH], redDst[MAX_PATH];
    bool rSrc = lpExistingFileName && RedirectFlashDataPath(lpExistingFileName, redSrc, MAX_PATH);
    bool rDst = lpNewFileName && RedirectFlashDataPath(lpNewFileName, redDst, MAX_PATH);

    if (rSrc || rDst) {
        if (rDst) EnsureParentDirExists(redDst);
        BOOL ok = orig(rSrc ? redSrc : lpExistingFileName,
                       rDst ? redDst : lpNewFileName);
        static int s_mvLogCount = 0;
        if (s_mvLogCount < 50) {
            s_mvLogCount++;
            DbgTrace(L"[FlashIE] MoveFileW REDIRECT: %s -> %s (%s)\n",
                      rSrc ? redSrc : lpExistingFileName,
                      rDst ? redDst : lpNewFileName,
                      ok ? L"OK" : L"FAILED");
        }
        return ok;
    }

    return orig(lpExistingFileName, lpNewFileName);
}

// ===================================================================
// Hook: MoveFileExW — redirect Flash data file renames (with flags)
// ===================================================================
static BOOL WINAPI Hooked_MoveFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, DWORD dwFlags)
{
    auto orig = reinterpret_cast<FN_MoveFileExW>(s_hookMoveFileExW.pTrampoline);

    wchar_t redSrc[MAX_PATH], redDst[MAX_PATH];
    bool rSrc = lpExistingFileName && RedirectFlashDataPath(lpExistingFileName, redSrc, MAX_PATH);
    bool rDst = lpNewFileName && RedirectFlashDataPath(lpNewFileName, redDst, MAX_PATH);

    if (rSrc || rDst) {
        if (rDst) EnsureParentDirExists(redDst);
        BOOL ok = orig(rSrc ? redSrc : lpExistingFileName,
                       rDst ? redDst : lpNewFileName, dwFlags);
        static int s_mvxLogCount = 0;
        if (s_mvxLogCount < 50) {
            s_mvxLogCount++;
            DbgTrace(L"[FlashIE] MoveFileExW REDIRECT: %s -> %s (%s)\n",
                      rSrc ? redSrc : lpExistingFileName,
                      rDst ? redDst : lpNewFileName,
                      ok ? L"OK" : L"FAILED");
        }
        return ok;
    }

    return orig(lpExistingFileName, lpNewFileName, dwFlags);
}

// ===================================================================
// Hook: DeleteFileW — redirect Flash data file deletion
// ===================================================================
static BOOL WINAPI Hooked_DeleteFileW(LPCWSTR lpFileName)
{
    auto orig = reinterpret_cast<FN_DeleteFileW>(s_hookDeleteFileW.pTrampoline);

    if (lpFileName) {
        wchar_t redirected[MAX_PATH];
        if (RedirectFlashDataPath(lpFileName, redirected, MAX_PATH)) {
            return orig(redirected);
        }
    }

    return orig(lpFileName);
}

// ===================================================================
// Hook: CLSIDFromProgID — make ShockwaveFlash ProgID resolve to Flash CLSID.
//
// On Win10, CLSIDFromProgID uses the COM catalog (cached in-process)
// rather than calling RegOpenKeyExW. Our fake registry entries never
// get seen. JavaScript "new ActiveXObject('ShockwaveFlash.ShockwaveFlash')"
// fails because CLSIDFromProgID returns REGDB_E_CLASSNOTREG.
// We intercept and return the Flash CLSID for matching ProgIDs.
// ===================================================================
static HRESULT STDAPICALLTYPE Hooked_CLSIDFromProgID(
    LPCOLESTR lpszProgID, LPCLSID lpclsid)
{
    if (lpszProgID && lpclsid) {
        // Match "ShockwaveFlash.ShockwaveFlash" with optional version suffix
        if (_wcsnicmp(lpszProgID, L"ShockwaveFlash.ShockwaveFlash", 29) == 0) {
            const wchar_t* afterBase = lpszProgID + 29;
            bool versionOk = true;
            if (*afterBase == L'.') {
                int ver = _wtoi(afterBase + 1);
                if (ver > 34) versionOk = false;
            }
            if (versionOk && (*afterBase == L'\0' || *afterBase == L'.')) {
                *lpclsid = CLSID_ShockwaveFlash;
                DbgTrace(L"[FlashIE] CLSIDFromProgID(%s) -> Flash CLSID\n", lpszProgID);
                return S_OK;
            }
        }
    }
    return reinterpret_cast<FN_CLSIDFromProgID>(
        s_hookCLSIDFromProgID.pTrampoline)(lpszProgID, lpclsid);
}

// ===================================================================
// Lazy module patching — called when we detect Flash-related activity,
// ensuring all currently-loaded modules (especially mshtml.dll which
// loads late) get their Flash CLSID block lists neutralized.
// ===================================================================
void FlashLoader::LazyPatchModules()
{
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    NeutralizeFlashBlock();
    DbgTrace(L"[FlashIE] LazyPatchModules: re-ran NeutralizeFlashBlock (mshtml.dll should be loaded now)\n");
}

// ===================================================================
// Phase 1: Load Flash.ocx and get its class factory
// ===================================================================
bool FlashLoader::Activate()
{
    WCHAR szDir[MAX_PATH];
    GetModuleFileNameW(nullptr, szDir, MAX_PATH);
    PathRemoveFileSpecW(szDir);

    PathCombineW(g_szOcxPath, szDir, L"Flash.ocx");
    PathCombineW(g_szMmsCfgPath, szDir, L"mms.cfg");

    // Set up local FlashData directory to redirect Flash Player's
    // %APPDATA%\Macromedia\Flash Player writes (LSOs, settings, etc.)
    PathCombineW(g_szFlashDataDir, szDir, L"FlashData");
    g_flashDataDirLen = (int)wcslen(g_szFlashDataDir);

    // Build the roaming path prefixes that Flash normally uses
    WCHAR szAppData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, szAppData))) {
        PathCombineW(g_szRoamingFlashDir, szAppData, L"Macromedia\\Flash Player");
        g_roamingFlashDirLen = (int)wcslen(g_szRoamingFlashDir);
        PathCombineW(g_szRoamingAdobeDir, szAppData, L"Adobe\\Flash Player");
        g_roamingAdobeDirLen = (int)wcslen(g_szRoamingAdobeDir);
    }

    // Create FlashData directory structure that Flash expects
    CreateDirectoryW(g_szFlashDataDir, nullptr);
    {
        wchar_t sub[MAX_PATH];
        PathCombineW(sub, g_szFlashDataDir, L"#SharedObjects");
        CreateDirectoryW(sub, nullptr);
        // Flash creates a random subfolder name under #SharedObjects;
        // we create a default one so the first check succeeds
        PathCombineW(sub, g_szFlashDataDir, L"#SharedObjects\\FLASHIE");
        CreateDirectoryW(sub, nullptr);
        PathCombineW(sub, g_szFlashDataDir, L"macromedia.com");
        CreateDirectoryW(sub, nullptr);
        PathCombineW(sub, g_szFlashDataDir, L"macromedia.com\\support");
        CreateDirectoryW(sub, nullptr);
        PathCombineW(sub, g_szFlashDataDir, L"macromedia.com\\support\\flashplayer");
        CreateDirectoryW(sub, nullptr);
        PathCombineW(sub, g_szFlashDataDir, L"macromedia.com\\support\\flashplayer\\sys");
        CreateDirectoryW(sub, nullptr);
    }
    DbgTrace(L"[FlashIE] FlashData dir: %s\n", g_szFlashDataDir);
    DbgTrace(L"[FlashIE] Roaming Flash dir: %s (len=%d)\n", g_szRoamingFlashDir, g_roamingFlashDirLen);

    // Ensure Flash.ocx's dependencies resolve from the exe directory
    SetDllDirectoryW(szDir);

    // Install time hooks BEFORE loading Flash.ocx so the EOL kill switch
    // in DllMain (if any) sees a pre-2021 date. We set the Flash module
    // range to cover the entire address space temporarily, then narrow it
    // after loading.
    {
        HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
        HMODULE hKB = GetModuleHandleW(L"kernelbase.dll");
        void* pGLT = hK32 ? reinterpret_cast<void*>(GetProcAddress(hK32, "GetLocalTime")) : nullptr;
        if (!pGLT && hKB) pGLT = reinterpret_cast<void*>(GetProcAddress(hKB, "GetLocalTime"));
        void* pGST = hK32 ? reinterpret_cast<void*>(GetProcAddress(hK32, "GetSystemTime")) : nullptr;
        if (!pGST && hKB) pGST = reinterpret_cast<void*>(GetProcAddress(hKB, "GetSystemTime"));

        // Temporarily make IsCallerInFlash() match ALL callers
        // (We don't know Flash.ocx's base address yet)
        // Use 0xFFFFFFFE to avoid 32-bit pointer overflow (1 + 0xFFFFFFFF wraps to 0)
        s_flashBase = reinterpret_cast<BYTE*>(static_cast<uintptr_t>(1));
        s_flashSize = 0xFFFFFFFE;

        void* pGSTAFT = hK32 ? reinterpret_cast<void*>(GetProcAddress(hK32, "GetSystemTimeAsFileTime")) : nullptr;
        if (!pGSTAFT && hKB) pGSTAFT = reinterpret_cast<void*>(GetProcAddress(hKB, "GetSystemTimeAsFileTime"));

        if (pGLT) InstallDetour(pGLT, reinterpret_cast<void*>(&Hooked_GetLocalTime), s_hookGetLocalTime);
        if (pGST) InstallDetour(pGST, reinterpret_cast<void*>(&Hooked_GetSystemTime), s_hookGetSystemTime);
        if (pGSTAFT) InstallDetour(pGSTAFT, reinterpret_cast<void*>(&Hooked_GetSystemTimeAsFileTime), s_hookGetSystemTimeAsFileTime);
    }

    m_hModule = LoadLibraryW(g_szOcxPath);

    // Pin Flash.ocx in memory — prevent COM from unloading it when all
    // Flash objects are released.  Our hooks (time spoofing, factory
    // pointers) reference Flash.ocx code/data for the process lifetime.
    if (m_hModule) {
        HMODULE hPinned = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_PIN,
            g_szOcxPath, &hPinned);
    }

    // Now narrow the time hooks to only affect Flash.ocx callers
    if (m_hModule) {
        CacheFlashModuleRange(m_hModule);
    } else {
        s_flashBase = nullptr;
        s_flashSize = 0;
    }

    if (!m_hModule) {
        DbgTrace(L"[FlashIE] LoadLibrary(%s) FAILED, err=%u\n", g_szOcxPath, GetLastError());
        return false;
    }
    DbgTrace(L"[FlashIE] Flash.ocx loaded at %p\n", m_hModule);

    auto pfnGetClassObject = reinterpret_cast<FN_DllGetClassObject>(
        GetProcAddress(m_hModule, "DllGetClassObject"));
    if (!pfnGetClassObject) {
        FreeLibrary(m_hModule);
        m_hModule = nullptr;
        return false;
    }

    HRESULT hr = pfnGetClassObject(CLSID_ShockwaveFlash, IID_IClassFactory,
                                    reinterpret_cast<void**>(&m_pFactory));
    if (FAILED(hr) || !m_pFactory) {
        FreeLibrary(m_hModule);
        m_hModule = nullptr;
        return false;
    }

    // Also register via CoRegisterClassObject (backup for non-hooked paths)
    HRESULT hrReg = CoRegisterClassObject(CLSID_ShockwaveFlash, m_pFactory,
                          CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE,
                          &m_dwCookie);
    DbgTrace(L"[FlashIE] CoRegisterClassObject -> hr=0x%08X cookie=%u\n", hrReg, m_dwCookie);

    // Publish for the hook functions
    s_pFlashFactory = m_pFactory;

    // Create logging wrapper around the real factory
    s_pLoggingFactory = new LoggingClassFactory(m_pFactory);

    // Cache exe name for FEATURE_BROWSER_EMULATION hook
    WCHAR szExe[MAX_PATH];
    GetModuleFileNameW(nullptr, szExe, MAX_PATH);
    wcscpy_s(s_szExeName, PathFindFileNameW(szExe));

    return true;
}

// ===================================================================
// Neutralize hardcoded Flash CLSID block in IE/MSHTML DLLs.
//
// After Microsoft's Flash EOL (July 2021+), mshtml.dll and related
// DLLs contain a hardcoded list of blocked CLSIDs that includes
// {D27CDB6E-AE6D-11CF-96B8-444553540000}. MSHTML checks this list
// AFTER CoGetClassObject returns the factory and discards the result
// if the CLSID is blocked. We scan these DLLs for the Flash CLSID
// byte pattern and corrupt each occurrence so the comparison never
// matches. Our own CLSID constant (in flashie.exe) is unaffected.
// ===================================================================
static void NeutralizeFlashBlock()
{
    // Flash CLSID in GUID memory layout (little-endian struct)
    static const BYTE flashGuid[16] = {
        0x6E, 0xDB, 0x7C, 0xD2,  // Data1 = 0xD27CDB6E
        0x6D, 0xAE,               // Data2 = 0xAE6D
        0xCF, 0x11,               // Data3 = 0x11CF
        0x96, 0xB8, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00  // Data4
    };

    // Full CLSID string form "{d27cdb6e-ae6d-11cf-96b8-444553540000}" (lowercase)
    static const wchar_t flashFullStr[] = L"{d27cdb6e-ae6d-11cf-96b8-444553540000}";
    static const int flashFullStrBytes = (int)(wcslen(flashFullStr) * sizeof(wchar_t));
    // Uppercase variant
    static const wchar_t flashFullStrUpper[] = L"{D27CDB6E-AE6D-11CF-96B8-444553540000}";

    // IE/MSHTML modules that may contain a Flash-blocked CLSID list.
    // We deliberately skip flashie.exe and Flash.ocx.
    static const wchar_t* moduleNames[] = {
        L"mshtml.dll",
        L"ieframe.dll",
        L"iertutil.dll",
        L"urlmon.dll",
        L"msiso.dll",
        L"edgehtml.dll",
        L"wldp.dll",
    };

    HMODULE hSelf = GetModuleHandleW(nullptr); // our exe

    for (int m = 0; m < _countof(moduleNames); m++) {
        HMODULE hMod = GetModuleHandleW(moduleNames[m]);
        if (!hMod || hMod == hSelf) continue;

        auto pDos = reinterpret_cast<PIMAGE_DOS_HEADER>(hMod);
        if (pDos->e_magic != IMAGE_DOS_SIGNATURE) continue;
        auto pNT = reinterpret_cast<PIMAGE_NT_HEADERS>(
            reinterpret_cast<BYTE*>(hMod) + pDos->e_lfanew);
        if (pNT->Signature != IMAGE_NT_SIGNATURE) continue;

        BYTE* base = reinterpret_cast<BYTE*>(hMod);
        DWORD imageSize = pNT->OptionalHeader.SizeOfImage;
        if (imageSize < 16) continue;

        int patchCount = 0;
        // Scan for binary GUID form
        for (DWORD off = 0; off <= imageSize - 16; off++) {
            if (memcmp(base + off, flashGuid, 16) == 0) {
                DbgTrace(L"[FlashIE] NeutralizeFlashBlock: GUID at offset 0x%X in %s -> patching\n",
                          off, moduleNames[m]);
                DWORD oldProt;
                if (VirtualProtect(base + off, 16, PAGE_READWRITE, &oldProt)) {
                    base[off] ^= 0x01;
                    VirtualProtect(base + off, 16, oldProt, &oldProt);
                    patchCount++;
                }
            }
        }
        // Scan for wide-string CLSID form (MSHTML may compare strings)
        // Look for "D27CDB6E" as wide chars (16 bytes: 'D' 0 '2' 0 '7' 0 ...)
        static const wchar_t flashStr[] = L"D27CDB6E";
        static const int flashStrBytes = 8 * sizeof(wchar_t); // 16 bytes
        for (DWORD off = 0; off <= imageSize - flashStrBytes; off += 2) {
            if (memcmp(base + off, flashStr, flashStrBytes) == 0) {
                DWORD oldProt;
                if (VirtualProtect(base + off, flashStrBytes, PAGE_READWRITE, &oldProt)) {
                    // Corrupt first char: 'D' -> 'X'
                    *reinterpret_cast<wchar_t*>(base + off) = L'X';
                    VirtualProtect(base + off, flashStrBytes, oldProt, &oldProt);
                    patchCount++;
                }
            }
        }
        // Also scan for lowercase variant
        static const wchar_t flashStrLower[] = L"d27cdb6e";
        for (DWORD off = 0; off <= imageSize - flashStrBytes; off += 2) {
            if (memcmp(base + off, flashStrLower, flashStrBytes) == 0) {
                DWORD oldProt;
                if (VirtualProtect(base + off, flashStrBytes, PAGE_READWRITE, &oldProt)) {
                    *reinterpret_cast<wchar_t*>(base + off) = L'x';
                    VirtualProtect(base + off, flashStrBytes, oldProt, &oldProt);
                    patchCount++;
                }
            }
        }
        // Scan for full CLSID string form (e.g. "{d27cdb6e-ae6d-11cf-96b8-444553540000}")
        if (imageSize >= (DWORD)flashFullStrBytes) {
            for (DWORD off = 0; off <= imageSize - flashFullStrBytes; off += 2) {
                if (_wcsnicmp(reinterpret_cast<wchar_t*>(base + off),
                              flashFullStr, wcslen(flashFullStr)) == 0) {
                    DWORD oldProt;
                    if (VirtualProtect(base + off, flashFullStrBytes, PAGE_READWRITE, &oldProt)) {
                        // Corrupt the 'd' or 'D' after the opening brace
                        reinterpret_cast<wchar_t*>(base + off)[1] = L'X';
                        VirtualProtect(base + off, flashFullStrBytes, oldProt, &oldProt);
                        patchCount++;
                    }
                }
            }
        }
        if (patchCount > 0)
            DbgTrace(L"[FlashIE] NeutralizeFlashBlock: patched %d instance(s) in %s\n",
                      patchCount, moduleNames[m]);
    }
}

// ===================================================================
// Phase 2: Install inline hooks (call after WebBrowser created)
// ===================================================================
void FlashLoader::InstallHooks()
{
    if (m_hooked || !m_pFactory) return;

    // --- Step 1: Neutralize any hardcoded Flash-blocked CLSID lists ---
    NeutralizeFlashBlock();

    // --- Step 2: Install inline detours ---
    // Resolve actual function body addresses.
    // On Win10, COM functions live in combase.dll, registry in kernelbase.dll.
    // GetProcAddress follows forwarders, giving us the actual function body.
    HMODULE hCombase    = GetModuleHandleW(L"combase.dll");
    HMODULE hOle32      = GetModuleHandleW(L"ole32.dll");
    HMODULE hKernelBase = GetModuleHandleW(L"kernelbase.dll");
    HMODULE hAdvapi32   = GetModuleHandleW(L"advapi32.dll");

    void* pCoGetClassObject = nullptr;
    void* pCoCreateInstance = nullptr;
    void* pRegOpenKeyExW    = nullptr;

    // COM: prefer combase (Win8+ actual implementation)
    if (hCombase) {
        pCoGetClassObject = reinterpret_cast<void*>(
            GetProcAddress(hCombase, "CoGetClassObject"));
        pCoCreateInstance = reinterpret_cast<void*>(
            GetProcAddress(hCombase, "CoCreateInstance"));
    }
    if (!pCoGetClassObject && hOle32)
        pCoGetClassObject = reinterpret_cast<void*>(
            GetProcAddress(hOle32, "CoGetClassObject"));
    if (!pCoCreateInstance && hOle32)
        pCoCreateInstance = reinterpret_cast<void*>(
            GetProcAddress(hOle32, "CoCreateInstance"));

    // Registry: prefer kernelbase (Win7+ actual implementation)
    if (hKernelBase)
        pRegOpenKeyExW = reinterpret_cast<void*>(
            GetProcAddress(hKernelBase, "RegOpenKeyExW"));
    if (!pRegOpenKeyExW && hAdvapi32)
        pRegOpenKeyExW = reinterpret_cast<void*>(
            GetProcAddress(hAdvapi32, "RegOpenKeyExW"));

    // Also hook RegQueryValueExW for cached-handle kill bit bypass
    void* pRegQueryValueExW = nullptr;
    if (hKernelBase)
        pRegQueryValueExW = reinterpret_cast<void*>(
            GetProcAddress(hKernelBase, "RegQueryValueExW"));
    if (!pRegQueryValueExW && hAdvapi32)
        pRegQueryValueExW = reinterpret_cast<void*>(
            GetProcAddress(hAdvapi32, "RegQueryValueExW"));

    // Install detours — order: registry hooks first (kill bit bypass),
    // then COM hooks (Flash CLSID interception)
    bool ok;
    ok = pRegOpenKeyExW && InstallDetour(pRegOpenKeyExW,
        reinterpret_cast<void*>(&Hooked_RegOpenKeyExW), s_hookRegOpenKeyExW);
    DbgTrace(L"[FlashIE] Hook RegOpenKeyExW: %s (addr=%p)\n", ok ? L"OK" : L"FAIL", pRegOpenKeyExW);

    ok = pRegQueryValueExW && InstallDetour(pRegQueryValueExW,
        reinterpret_cast<void*>(&Hooked_RegQueryValueExW), s_hookRegQueryValueExW);
    DbgTrace(L"[FlashIE] Hook RegQueryValueExW: %s (addr=%p)\n", ok ? L"OK" : L"FAIL", pRegQueryValueExW);

    // RegCloseKey — handle fake HKEY values
    void* pRegCloseKey = nullptr;
    if (hKernelBase)
        pRegCloseKey = reinterpret_cast<void*>(
            GetProcAddress(hKernelBase, "RegCloseKey"));
    if (!pRegCloseKey && hAdvapi32)
        pRegCloseKey = reinterpret_cast<void*>(
            GetProcAddress(hAdvapi32, "RegCloseKey"));
    ok = pRegCloseKey && InstallDetour(pRegCloseKey,
        reinterpret_cast<void*>(&Hooked_RegCloseKey), s_hookRegCloseKey);
    DbgTrace(L"[FlashIE] Hook RegCloseKey: %s (addr=%p)\n", ok ? L"OK" : L"FAIL", pRegCloseKey);

    ok = pCoGetClassObject && InstallDetour(pCoGetClassObject,
        reinterpret_cast<void*>(&Hooked_CoGetClassObject), s_hookCoGetClassObject);
    DbgTrace(L"[FlashIE] Hook CoGetClassObject: %s (addr=%p)\n", ok ? L"OK" : L"FAIL", pCoGetClassObject);

    ok = pCoCreateInstance && InstallDetour(pCoCreateInstance,
        reinterpret_cast<void*>(&Hooked_CoCreateInstance), s_hookCoCreateInstance);
    DbgTrace(L"[FlashIE] Hook CoCreateInstance: %s (addr=%p)\n", ok ? L"OK" : L"FAIL", pCoCreateInstance);

    // CLSIDFromProgID — make "ShockwaveFlash.ShockwaveFlash" ProgID resolve
    // to Flash CLSID. Critical for JavaScript "new ActiveXObject(...)" calls.
    void* pCLSIDFromProgID = nullptr;
    if (hCombase)
        pCLSIDFromProgID = reinterpret_cast<void*>(
            GetProcAddress(hCombase, "CLSIDFromProgID"));
    if (!pCLSIDFromProgID && hOle32)
        pCLSIDFromProgID = reinterpret_cast<void*>(
            GetProcAddress(hOle32, "CLSIDFromProgID"));
    ok = pCLSIDFromProgID && InstallDetour(pCLSIDFromProgID,
        reinterpret_cast<void*>(&Hooked_CLSIDFromProgID), s_hookCLSIDFromProgID);
    DbgTrace(L"[FlashIE] Hook CLSIDFromProgID: %s (addr=%p)\n", ok ? L"OK" : L"FAIL", pCLSIDFromProgID);

    // CoGetClassObjectFromURL (urlmon.dll) — the normal MSHTML ActiveX path
    HMODULE hUrlmon = GetModuleHandleW(L"urlmon.dll");
    void* pCoGetClassObjectFromURL = nullptr;
    if (hUrlmon)
        pCoGetClassObjectFromURL = reinterpret_cast<void*>(
            GetProcAddress(hUrlmon, "CoGetClassObjectFromURL"));
    ok = pCoGetClassObjectFromURL && InstallDetour(pCoGetClassObjectFromURL,
        reinterpret_cast<void*>(&Hooked_CoGetClassObjectFromURL), s_hookCoGetClassObjectFromURL);
    DbgTrace(L"[FlashIE] Hook CoGetClassObjectFromURL: %s (addr=%p)\n", ok ? L"OK" : L"FAIL", pCoGetClassObjectFromURL);

    // CoInternetIsFeatureEnabled (urlmon.dll) — disable Feature Control checks
    void* pCoInternetIsFeatureEnabled = nullptr;
    if (hUrlmon)
        pCoInternetIsFeatureEnabled = reinterpret_cast<void*>(
            GetProcAddress(hUrlmon, "CoInternetIsFeatureEnabled"));
    ok = pCoInternetIsFeatureEnabled && InstallDetour(pCoInternetIsFeatureEnabled,
        reinterpret_cast<void*>(&Hooked_CoInternetIsFeatureEnabled), s_hookCoInternetIsFeatureEnabled);
    DbgTrace(L"[FlashIE] Hook CoInternetIsFeatureEnabled: %s (addr=%p)\n", ok ? L"OK" : L"FAIL", pCoInternetIsFeatureEnabled);

    // WldpIsClassInApprovedList (wldp.dll) — approve all ActiveX CLSIDs
    HMODULE hWldp = GetModuleHandleW(L"wldp.dll");
    if (!hWldp) hWldp = LoadLibraryW(L"wldp.dll");
    void* pWldpIsClass = nullptr;
    void* pWldpQueryDynamic = nullptr;
    if (hWldp) {
        pWldpIsClass = reinterpret_cast<void*>(
            GetProcAddress(hWldp, "WldpIsClassInApprovedList"));
        pWldpQueryDynamic = reinterpret_cast<void*>(
            GetProcAddress(hWldp, "WldpQueryDynamicCodeTrust"));
    }
    ok = pWldpIsClass && InstallDetour(pWldpIsClass,
        reinterpret_cast<void*>(&Hooked_WldpIsClassInApprovedList), s_hookWldpIsClassInApprovedList);
    DbgTrace(L"[FlashIE] Hook WldpIsClassInApprovedList: %s (addr=%p)\n", ok ? L"OK" : L"FAIL", pWldpIsClass);

    ok = pWldpQueryDynamic && InstallDetour(pWldpQueryDynamic,
        reinterpret_cast<void*>(&Hooked_WldpQueryDynamicCodeTrust), s_hookWldpQueryDynamicCodeTrust);
    DbgTrace(L"[FlashIE] Hook WldpQueryDynamicCodeTrust: %s (addr=%p)\n", ok ? L"OK" : L"FAIL", pWldpQueryDynamic);

    // LoadRegTypeLib (oleaut32.dll) — redirect Flash TypeLib loading
    HMODULE hOleAut32 = GetModuleHandleW(L"oleaut32.dll");
    void* pLoadRegTypeLib = nullptr;
    if (hOleAut32)
        pLoadRegTypeLib = reinterpret_cast<void*>(
            GetProcAddress(hOleAut32, "LoadRegTypeLib"));
    ok = pLoadRegTypeLib && InstallDetour(pLoadRegTypeLib,
        reinterpret_cast<void*>(&Hooked_LoadRegTypeLib), s_hookLoadRegTypeLib);
    DbgTrace(L"[FlashIE] Hook LoadRegTypeLib: %s (addr=%p)\n", ok ? L"OK" : L"FAIL", pLoadRegTypeLib);

    // CreateFileW (kernelbase.dll / kernel32.dll) — redirect mms.cfg + log file access
    HMODULE hKernelBase2 = GetModuleHandleW(L"kernelbase.dll");
    void* pCreateFileW = nullptr;
    if (hKernelBase2)
        pCreateFileW = reinterpret_cast<void*>(
            GetProcAddress(hKernelBase2, "CreateFileW"));
    if (!pCreateFileW) {
        HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
        if (hK32)
            pCreateFileW = reinterpret_cast<void*>(
                GetProcAddress(hK32, "CreateFileW"));
    }
    ok = pCreateFileW && InstallDetour(pCreateFileW,
        reinterpret_cast<void*>(&Hooked_CreateFileW), s_hookCreateFileW);
    DbgTrace(L"[FlashIE] Hook CreateFileW: %s (addr=%p)\n", ok ? L"OK" : L"FAIL", pCreateFileW);

    // CreateDirectoryW — redirect Flash data directory creation to local FlashData
    {
        void* pCreateDirW = nullptr;
        if (hKernelBase2)
            pCreateDirW = reinterpret_cast<void*>(GetProcAddress(hKernelBase2, "CreateDirectoryW"));
        if (!pCreateDirW) {
            HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
            if (hK32) pCreateDirW = reinterpret_cast<void*>(GetProcAddress(hK32, "CreateDirectoryW"));
        }
        ok = pCreateDirW && InstallDetour(pCreateDirW,
            reinterpret_cast<void*>(&Hooked_CreateDirectoryW), s_hookCreateDirectoryW);
        DbgTrace(L"[FlashIE] Hook CreateDirectoryW: %s\n", ok ? L"OK" : L"FAIL");
    }

    // GetFileAttributesW — redirect Flash data path attribute queries
    {
        void* pGetFileAttr = nullptr;
        if (hKernelBase2)
            pGetFileAttr = reinterpret_cast<void*>(GetProcAddress(hKernelBase2, "GetFileAttributesW"));
        if (!pGetFileAttr) {
            HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
            if (hK32) pGetFileAttr = reinterpret_cast<void*>(GetProcAddress(hK32, "GetFileAttributesW"));
        }
        ok = pGetFileAttr && InstallDetour(pGetFileAttr,
            reinterpret_cast<void*>(&Hooked_GetFileAttributesW), s_hookGetFileAttributesW);
        DbgTrace(L"[FlashIE] Hook GetFileAttributesW: %s\n", ok ? L"OK" : L"FAIL");
    }

    // FindFirstFileW — redirect Flash data file enumeration
    {
        void* pFindFirst = nullptr;
        if (hKernelBase2)
            pFindFirst = reinterpret_cast<void*>(GetProcAddress(hKernelBase2, "FindFirstFileW"));
        if (!pFindFirst) {
            HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
            if (hK32) pFindFirst = reinterpret_cast<void*>(GetProcAddress(hK32, "FindFirstFileW"));
        }
        ok = pFindFirst && InstallDetour(pFindFirst,
            reinterpret_cast<void*>(&Hooked_FindFirstFileW), s_hookFindFirstFileW);
        DbgTrace(L"[FlashIE] Hook FindFirstFileW: %s\n", ok ? L"OK" : L"FAIL");
    }

    // MoveFileW — redirect Flash data file renames (.sxx -> .sol)
    {
        void* p = nullptr;
        if (hKernelBase2) p = reinterpret_cast<void*>(GetProcAddress(hKernelBase2, "MoveFileW"));
        if (!p) { HMODULE hK32 = GetModuleHandleW(L"kernel32.dll"); if (hK32) p = reinterpret_cast<void*>(GetProcAddress(hK32, "MoveFileW")); }
        ok = p && InstallDetour(p, reinterpret_cast<void*>(&Hooked_MoveFileW), s_hookMoveFileW);
        DbgTrace(L"[FlashIE] Hook MoveFileW: %s\n", ok ? L"OK" : L"FAIL");
    }

    // MoveFileExW — redirect Flash data file renames (with flags)
    {
        void* p = nullptr;
        if (hKernelBase2) p = reinterpret_cast<void*>(GetProcAddress(hKernelBase2, "MoveFileExW"));
        if (!p) { HMODULE hK32 = GetModuleHandleW(L"kernel32.dll"); if (hK32) p = reinterpret_cast<void*>(GetProcAddress(hK32, "MoveFileExW")); }
        ok = p && InstallDetour(p, reinterpret_cast<void*>(&Hooked_MoveFileExW), s_hookMoveFileExW);
        DbgTrace(L"[FlashIE] Hook MoveFileExW: %s\n", ok ? L"OK" : L"FAIL");
    }

    // DeleteFileW — redirect Flash data file deletion
    {
        void* p = nullptr;
        if (hKernelBase2) p = reinterpret_cast<void*>(GetProcAddress(hKernelBase2, "DeleteFileW"));
        if (!p) { HMODULE hK32 = GetModuleHandleW(L"kernel32.dll"); if (hK32) p = reinterpret_cast<void*>(GetProcAddress(hK32, "DeleteFileW")); }
        ok = p && InstallDetour(p, reinterpret_cast<void*>(&Hooked_DeleteFileW), s_hookDeleteFileW);
        DbgTrace(L"[FlashIE] Hook DeleteFileW: %s\n", ok ? L"OK" : L"FAIL");
    }

    // GetLocalTime / GetSystemTime — already installed in Activate() (before Flash.ocx loads)
    DbgTrace(L"[FlashIE] Hook GetLocalTime: %s\n", s_hookGetLocalTime.active ? L"OK (early)" : L"FAIL");
    DbgTrace(L"[FlashIE] Hook GetSystemTime: %s\n", s_hookGetSystemTime.active ? L"OK (early)" : L"FAIL");
    DbgTrace(L"[FlashIE] Hook GetSystemTimeAsFileTime: %s\n", s_hookGetSystemTimeAsFileTime.active ? L"OK (early)" : L"FAIL");

    m_hooked = true;
}

// ===================================================================
// Cleanup
// ===================================================================
void FlashLoader::Deactivate()
{
    if (m_hooked) {
        RemoveDetour(s_hookCoGetClassObject);
        RemoveDetour(s_hookCoCreateInstance);
        RemoveDetour(s_hookRegOpenKeyExW);
        RemoveDetour(s_hookRegQueryValueExW);
        RemoveDetour(s_hookRegCloseKey);
        RemoveDetour(s_hookCoGetClassObjectFromURL);
        RemoveDetour(s_hookCoInternetIsFeatureEnabled);
        RemoveDetour(s_hookGetLocalTime);
        RemoveDetour(s_hookGetSystemTime);
        RemoveDetour(s_hookWldpIsClassInApprovedList);
        RemoveDetour(s_hookWldpQueryDynamicCodeTrust);
        RemoveDetour(s_hookLoadRegTypeLib);
        RemoveDetour(s_hookGetSystemTimeAsFileTime);
        RemoveDetour(s_hookCreateFileW);
        RemoveDetour(s_hookCreateDirectoryW);
        RemoveDetour(s_hookGetFileAttributesW);
        RemoveDetour(s_hookFindFirstFileW);
        RemoveDetour(s_hookMoveFileW);
        RemoveDetour(s_hookMoveFileExW);
        RemoveDetour(s_hookDeleteFileW);
        m_hooked = false;
    }

    s_pFlashFactory = nullptr;
    if (s_pLoggingFactory) {
        s_pLoggingFactory->Release();
        s_pLoggingFactory = nullptr;
    }

    if (m_dwCookie) {
        CoRevokeClassObject(m_dwCookie);
        m_dwCookie = 0;
    }
    if (m_pFactory) {
        m_pFactory->Release();
        m_pFactory = nullptr;
    }
    if (m_hModule) {
        FreeLibrary(m_hModule);
        m_hModule = nullptr;
    }
}
