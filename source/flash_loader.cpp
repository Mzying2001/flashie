// =====================================================================
// Section 1: Includes & Constants
// =====================================================================

#include "flash_loader.h"
#include "debug.h"
#include <shlwapi.h>
#include <shlobj.h>      // SHGetFolderPathW, CSIDL_APPDATA
#include <stdint.h>
#include <string.h>
#include <intrin.h>
#include <ocidl.h>     // IQuickActivate, IPersistPropertyBag
#include <oleidl.h>     // IOleObject, IOleInPlaceObject, etc.
#include <objsafe.h>    // IObjectSafety

// Flash Player ActiveX CLSID: {D27CDB6E-AE6D-11CF-96B8-444553540000}
static const CLSID CLSID_ShockwaveFlash =
    {0xD27CDB6E, 0xAE6D, 0x11CF, {0x96, 0xB8, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

// Flash TypeLib GUID: {D27CDB6B-AE6D-11CF-96B8-444553540000}
static const GUID GUID_FlashTypeLib =
    {0xD27CDB6B, 0xAE6D, 0x11CF, {0x96, 0xB8, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

// Flash CLSID string forms for comparisons
static const wchar_t FLASH_CLSID_STR[] = L"{D27CDB6E-AE6D-11CF-96B8-444553540000}";
static const wchar_t FLASH_CLSID_UPPER[] = L"D27CDB6E-AE6D-11CF-96B8-444553540000";

// Static member
IClassFactory* FlashLoader::s_pFlashFactory = nullptr;

// =====================================================================
// Section 2: Function Pointer Typedefs
// =====================================================================

typedef HRESULT (STDAPICALLTYPE *FN_DllGetClassObject)(REFCLSID, REFIID, LPVOID*);
typedef HRESULT (STDAPICALLTYPE *FN_CoGetClassObject)(
    REFCLSID, DWORD, LPVOID, REFIID, LPVOID*);
typedef HRESULT (STDAPICALLTYPE *FN_CoCreateInstance)(
    REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
typedef LSTATUS (WINAPI *FN_RegOpenKeyExW)(
    HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
typedef LSTATUS (WINAPI *FN_RegQueryValueExW)(
    HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LSTATUS (WINAPI *FN_RegCloseKey)(HKEY);
typedef HRESULT (STDAPICALLTYPE *FN_CoGetClassObjectFromURL)(
    REFCLSID, LPCWSTR, DWORD, DWORD, LPCWSTR, LPBINDCTX,
    DWORD, LPVOID, REFIID, LPVOID*);
typedef HRESULT (STDAPICALLTYPE *FN_CoInternetIsFeatureEnabled)(
    DWORD, DWORD);
typedef void (WINAPI *FN_GetLocalTime)(LPSYSTEMTIME);
typedef void (WINAPI *FN_GetSystemTime)(LPSYSTEMTIME);
typedef void (WINAPI *FN_GetSystemTimeAsFileTime)(LPFILETIME);
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
typedef BOOL (WINAPI *FN_MoveFileExW)(LPCWSTR, LPCWSTR, DWORD);
typedef BOOL (WINAPI *FN_DeleteFileW)(LPCWSTR);
typedef HANDLE (WINAPI *FN_FindFirstFileW)(LPCWSTR, LPWIN32_FIND_DATAW);
typedef BOOL (WINAPI *FN_SetFileAttributesW)(LPCWSTR, DWORD);
typedef BOOL (WINAPI *FN_FindNextFileW)(HANDLE, LPWIN32_FIND_DATAW);
typedef HRESULT (STDAPICALLTYPE *FN_CLSIDFromProgID)(LPCOLESTR, LPCLSID);
typedef HRESULT (STDMETHODCALLTYPE *FN_FlashQueryInterface)(void*, REFIID, void**);

// =====================================================================
// Section 3: Instruction Length Decoder
//
// Minimal x86/x64 instruction length decoder for function prologues.
// Used by the inline hook infrastructure to compute trampoline sizes.
// =====================================================================

// Returns instruction length at 'code', or 0 if unrecognized.
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

        // PUSH imm8 / PUSH imm32
        case 0x6A:
            return static_cast<int>(p - code) + 1;
        case 0x68:
            return static_cast<int>(p - code) + 4;

        // CALL rel32 / JMP rel32
        case 0xE8: case 0xE9:
            return static_cast<int>(p - code) + 4;

        // JMP rel8
        case 0xEB:
            return static_cast<int>(p - code) + 1;

        // MOV eax,moffs32 / MOV moffs32,eax
        case 0xA1: case 0xA3:
#ifdef _WIN64
            return static_cast<int>(p - code) + 8;
#else
            return static_cast<int>(p - code) + 4;
#endif

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

// =====================================================================
// Section 4: Inline Hook (Detour) Infrastructure
// =====================================================================

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

// Centralized hook storage — indexed by HookId
enum HookId {
    HK_CoGetClassObject,
    HK_CoCreateInstance,
    HK_RegOpenKeyExW,
    HK_RegQueryValueExW,
    HK_RegCloseKey,
    HK_CoGetClassObjectFromURL,
    HK_CoInternetIsFeatureEnabled,
    HK_GetLocalTime,
    HK_GetSystemTime,
    HK_GetSystemTimeAsFileTime,
    HK_WldpIsClassInApprovedList,
    HK_WldpQueryDynamicCodeTrust,
    HK_LoadRegTypeLib,
    HK_CreateFileW,
    HK_CreateDirectoryW,
    HK_GetFileAttributesW,
    HK_FindFirstFileW,
    HK_MoveFileW,
    HK_MoveFileExW,
    HK_DeleteFileW,
    HK_CLSIDFromProgID,
    HK_FlashQI,
    HK_COUNT
};

static InlineHook s_hooks[HK_COUNT] = {};

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

// Resolve a function from a prioritized list of modules and install a detour.
static bool ResolveAndHook(const char* funcName, const wchar_t* logName,
    void* hookFunc, InlineHook& hook,
    HMODULE hPrimary, HMODULE hFallback = nullptr)
{
    void* pTarget = nullptr;
    if (hPrimary)
        pTarget = reinterpret_cast<void*>(GetProcAddress(hPrimary, funcName));
    if (!pTarget && hFallback)
        pTarget = reinterpret_cast<void*>(GetProcAddress(hFallback, funcName));
    bool ok = pTarget && InstallDetour(pTarget, hookFunc, hook);
    DbgTrace(L"[FlashIE] Hook %s: %s (addr=%p)\n", logName, ok ? L"OK" : L"FAIL", pTarget);
    return ok;
}

// =====================================================================
// Section 5: Fake Registry Key System
//
// Fake Flash CLSID registration using REAL HKEY handles.
// We can't use sentinel values because Windows internal code (rpcrt4,
// ole32) dereferences HKEY as a pointer to an internal structure,
// causing AV. Instead, we open real existing keys (read-only, no
// writes!) and track them in a table.
// =====================================================================

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
    FK_IMPL_CATEGORY,    // ...\Implemented Categories\{CATID_SafeFor*}
};

struct FakeKeyEntry {
    HKEY    hKey;
    FakeKeyType type;
};

static const int MAX_FAKE_KEYS = 64;
static FakeKeyEntry s_fakeKeys[MAX_FAKE_KEYS] = {};
static int s_fakeKeyCount = 0;

static HKEY AllocFakeKey(FakeKeyType type)
{
    HKEY hReal = nullptr;
    auto pfn = reinterpret_cast<FN_RegOpenKeyExW>(s_hooks[HK_RegOpenKeyExW].pTrampoline);
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
            auto pfn = reinterpret_cast<FN_RegCloseKey>(s_hooks[HK_RegCloseKey].pTrampoline);
            if (pfn) pfn(hKey);
            s_fakeKeys[i] = s_fakeKeys[s_fakeKeyCount - 1];
            s_fakeKeyCount--;
            return;
        }
    }
}

// =====================================================================
// Section 6: COM Wrapper Classes
//
// LoggingClassFactory: Wraps the real Flash class factory to log
//   CreateInstance calls from MSHTML which calls pCF->CreateInstance()
//   on the vtable directly (not through CoCreateInstance).
//
// FlashSafetyTearoff: IObjectSafety tearoff for Flash objects.
//   MSHTML checks IObjectSafety on ActiveX controls before allowing
//   JavaScript to access their IDispatch. Flash's EOL build removed
//   IObjectSafety entirely, so MSHTML blocks IDispatch delegation.
// =====================================================================

// Forward declaration: installs IObjectSafety hook on Flash's QueryInterface
static void MaybeHookFlashQI(IUnknown* pObj);

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
                { IID_IObjectSafety, L"IObjectSafety" },
            };
            for (int i = 0; i < _countof(probes); i++) {
                void* pTest = nullptr;
                HRESULT hrQI = pObj->QueryInterface(probes[i].iid, &pTest);
                DbgTrace(L"[FlashIE] FlashObj::QI %s -> 0x%08X\n", probes[i].name, hrQI);
                if (pTest) static_cast<IUnknown*>(pTest)->Release();
            }
            // Hook Flash's QueryInterface to inject IObjectSafety support
            MaybeHookFlashQI(pObj);
        }
        return hr;
    }
    STDMETHODIMP LockServer(BOOL fLock) override {
        return m_real->LockServer(fLock);
    }
};

// CRITICAL: This must be a COM tearoff (delegating IUnknown to the
// Flash object), NOT a standalone singleton. MSHTML verifies COM
// identity: pSafety->QI(IID_IUnknown) must return the same pointer
// as pFlash->QI(IID_IUnknown). A singleton with its own identity
// causes MSHTML to reject the safety assertion and block scripting.
class FlashSafetyTearoff : public IObjectSafety {
    IUnknown* m_pFlash;
public:
    explicit FlashSafetyTearoff(IUnknown* pFlash) : m_pFlash(pFlash) {}
    ~FlashSafetyTearoff() { if (m_pFlash) m_pFlash->Release(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IObjectSafety) {
            *ppv = static_cast<IObjectSafety*>(this);
            m_pFlash->AddRef();
            return S_OK;
        }
        return m_pFlash->QueryInterface(riid, ppv);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return m_pFlash->AddRef(); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG ref = m_pFlash->Release();
        if (ref == 0) delete this;
        return ref;
    }

    STDMETHODIMP GetInterfaceSafetyOptions(REFIID, DWORD* pdwSupportedOptions,
                                            DWORD* pdwEnabledOptions) override {
        if (pdwSupportedOptions)
            *pdwSupportedOptions = INTERFACESAFE_FOR_UNTRUSTED_CALLER | INTERFACESAFE_FOR_UNTRUSTED_DATA;
        if (pdwEnabledOptions)
            *pdwEnabledOptions = INTERFACESAFE_FOR_UNTRUSTED_CALLER | INTERFACESAFE_FOR_UNTRUSTED_DATA;
        return S_OK;
    }
    STDMETHODIMP SetInterfaceSafetyOptions(REFIID, DWORD, DWORD) override {
        return S_OK;
    }
};

// =====================================================================
// Section 7: Shared Static State
// =====================================================================

// Logging wrapper around the real Flash class factory
static LoggingClassFactory* s_pLoggingFactory = nullptr;

// HKEY tracking for FEATURE_BROWSER_EMULATION (fake without registry writes)
static HKEY s_hkeyBrowserEmulation = nullptr;
static wchar_t s_szExeName[MAX_PATH] = {};

// Paths
static wchar_t g_szOcxPath[MAX_PATH] = {};
static wchar_t g_szMmsCfgPath[MAX_PATH] = {};

// Path to local FlashData directory (next to exe) — replaces %APPDATA%\{Macromedia,Adobe}\Flash Player
static wchar_t g_szFlashDataDir[MAX_PATH] = {};
static int g_flashDataDirLen = 0;

// The roaming path prefixes Flash uses
static wchar_t g_szRoamingFlashDir[MAX_PATH] = {};   // %APPDATA%\Macromedia\Flash Player
static int g_roamingFlashDirLen = 0;
static wchar_t g_szRoamingAdobeDir[MAX_PATH] = {};   // %APPDATA%\Adobe\Flash Player
static int g_roamingAdobeDirLen = 0;

// Cached Flash.ocx address range for fast caller check
static BYTE* s_flashBase = nullptr;
static DWORD s_flashSize = 0;

// Forward declaration
static void NeutralizeFlashBlock();

// =====================================================================
// Section 8a: COM Hooks
// =====================================================================

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
    HRESULT hr = reinterpret_cast<FN_CoGetClassObject>(s_hooks[HK_CoGetClassObject].pTrampoline)(
        rclsid, dwClsContext, pvReserved, riid, ppv);
    DbgTrace(L"[FlashIE] CoGetClassObject({%08X-...}) -> hr=0x%08X\n", rclsid.Data1, hr);
    return hr;
}

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
    HRESULT hr = reinterpret_cast<FN_CoCreateInstance>(s_hooks[HK_CoCreateInstance].pTrampoline)(
        rclsid, pUnkOuter, dwClsContext, riid, ppv);
    DbgTrace(L"[FlashIE] CoCreateInstance({%08X-...}) -> hr=0x%08X\n", rclsid.Data1, hr);
    return hr;
}

// CoGetClassObjectFromURL (urlmon.dll) — the normal MSHTML code path
// for loading ActiveX controls from <object> tags.
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
        s_hooks[HK_CoGetClassObjectFromURL].pTrampoline)(
        rclsid, szCodeURL, dwFileVersionMS, dwFileVersionLS,
        szContentType, pBindCtx, dwClsContext, pvReserved, riid, ppv);
}

// CLSIDFromProgID — make "ShockwaveFlash.ShockwaveFlash" ProgID resolve
// to Flash CLSID. On Win10, CLSIDFromProgID uses the COM catalog
// (cached in-process) rather than calling RegOpenKeyExW. Critical for
// JavaScript "new ActiveXObject(...)" calls.
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
        s_hooks[HK_CLSIDFromProgID].pTrampoline)(lpszProgID, lpclsid);
}

// =====================================================================
// Section 8b: Registry Hooks
//
// Intercepts registry access to:
// - Bypass Flash ActiveX kill bit (KB4561600)
// - Fake Flash CLSID registration (InprocServer32, TypeLib, ProgID, etc.)
// - Fake MIME type -> CLSID mapping
// - Fake FEATURE_BROWSER_EMULATION value
// - Clear Compatibility Flags kill bit
// =====================================================================

// Helper to check if a subkey path ends with a specific suffix (case-insensitive)
static bool SubKeyEndsWith(LPCWSTR lpSubKey, LPCWSTR suffix)
{
    size_t keyLen = wcslen(lpSubKey);
    size_t sufLen = wcslen(suffix);
    if (keyLen < sufLen) return false;
    return _wcsicmp(lpSubKey + keyLen - sufLen, suffix) == 0;
}

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
        else if (_wcsnicmp(lpSubKey, L"Implemented Categories\\", 23) == 0)
            subType = FK_IMPL_CATEGORY;

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
            bool versionOk = true;
            {
                const wchar_t* progid = wcsstr(lpSubKey, L"ShockwaveFlash.ShockwaveFlash");
                if (progid) {
                    const wchar_t* afterBase = progid + 29;
                    if (*afterBase == L'.') {
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

        // ---- Fake Flash CLSID subkeys ----
        // MSHTML looks up HKCR\CLSID\{D27CDB6E-...} to check if the control
        // is installed. We fake the entire CLSID tree so MSHTML proceeds.
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
                s_hooks[HK_RegOpenKeyExW].pTrampoline)(
                hKey, lpSubKey, ulOptions, samDesired, phkResult);
            if (res == ERROR_SUCCESS && phkResult)
                s_hkeyBrowserEmulation = *phkResult;
            return res;
        }
    }
    LSTATUS res = reinterpret_cast<FN_RegOpenKeyExW>(s_hooks[HK_RegOpenKeyExW].pTrampoline)(
        hKey, lpSubKey, ulOptions, samDesired, phkResult);
    // Log Flash-related registry access for debugging
    if (lpSubKey && (wcsstr(lpSubKey, L"Macr") || wcsstr(lpSubKey, L"Flash") ||
                     wcsstr(lpSubKey, L"flash") || wcsstr(lpSubKey, L"ShockwaveFlash") ||
                     wcsstr(lpSubKey, L"mms.cfg"))) {
        DbgTrace(L"[FlashIE] RegOpenKeyExW FLASH-RELATED: %s -> 0x%X\n", lpSubKey, res);
    }
    return res;
}

// Belt-and-suspenders kill bit bypass.
// If CompatFlagsFromClsid already has the key open (cached handle),
// it reads "Compatibility Flags". We intercept the value read and
// return 0 (no flags) instead of 0x400 (COMPAT_EVIL_DONT_LOAD).
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
        case FK_IMPL_CATEGORY:
            return ERROR_FILE_NOT_FOUND;

        case FK_VERSION:
            if (!lpValueName || lpValueName[0] == 0)
                return FakeRegSz(lpType, lpData, lpcbData, L"ShockwaveFlash.ShockwaveFlash");
            return ERROR_FILE_NOT_FOUND;

        case FK_PROGID_ROOT:
            if (!lpValueName || lpValueName[0] == 0)
                return FakeRegSz(lpType, lpData, lpcbData, L"Shockwave Flash");
            return ERROR_FILE_NOT_FOUND;

        case FK_PROGID_CLSID:
            if (!lpValueName || lpValueName[0] == 0) {
                DbgTrace(L"[FlashIE] RegQueryValueExW FAKE: ProgID\\CLSID -> %s\n", FLASH_CLSID_STR);
                return FakeRegSz(lpType, lpData, lpcbData, FLASH_CLSID_STR);
            }
            return ERROR_FILE_NOT_FOUND;

        case FK_PROGID_CURVER:
            if (!lpValueName || lpValueName[0] == 0) {
                DbgTrace(L"[FlashIE] RegQueryValueExW FAKE: CurVer -> ShockwaveFlash.ShockwaveFlash.34\n");
                return FakeRegSz(lpType, lpData, lpcbData, L"ShockwaveFlash.ShockwaveFlash.34");
            }
            return ERROR_FILE_NOT_FOUND;

        case FK_INSTALLED_VER:
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
            s_hooks[HK_RegQueryValueExW].pTrampoline)(
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

    return reinterpret_cast<FN_RegQueryValueExW>(s_hooks[HK_RegQueryValueExW].pTrampoline)(
        hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
}

LSTATUS WINAPI FlashLoader::Hooked_RegCloseKey(HKEY hKey)
{
    if (IsFakeHKey(hKey)) {
        CloseFakeKey(hKey);
        return ERROR_SUCCESS;
    }
    return reinterpret_cast<FN_RegCloseKey>(s_hooks[HK_RegCloseKey].pTrampoline)(hKey);
}

// =====================================================================
// Section 8c: Security & Feature Hooks
// =====================================================================

// Disable all IE Feature Controls. MSHTML checks features like
// FEATURE_RESTRICT_ACTIVEXINSTALL and FEATURE_SAFE_BINDTOOBJECT before
// allowing ActiveX. Returning S_FALSE means "feature not enabled".
HRESULT STDAPICALLTYPE FlashLoader::Hooked_CoInternetIsFeatureEnabled(
    DWORD dwFeature, DWORD dwFlags)
{
    LazyPatchModules();

    HRESULT hrOrig = reinterpret_cast<FN_CoInternetIsFeatureEnabled>(
        s_hooks[HK_CoInternetIsFeatureEnabled].pTrampoline)(dwFeature, dwFlags);

    // S_OK = feature enabled (blocks), S_FALSE = feature not enabled (allows).
    // Only log non-spammy features (skip feature 0 = OBJECT_CACHING)
    static int s_featureLogCount = 0;
    if (dwFeature != 0 && s_featureLogCount < 50) {
        s_featureLogCount++;
        DbgTrace(L"[FlashIE] CoInternetIsFeatureEnabled(feature=%u, flags=0x%X) orig=0x%08X -> S_FALSE\n",
                 dwFeature, dwFlags, hrOrig);
    }
    return S_FALSE;
}

// Windows 10+ MSHTML calls wldp!WldpIsClassInApprovedList to check if
// an ActiveX CLSID is approved for instantiation. We approve everything.
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

HRESULT WINAPI FlashLoader::Hooked_WldpQueryDynamicCodeTrust(
    HANDLE fileHandle, void* baseImage, DWORD imageSize)
{
    DbgTrace(L"[FlashIE] WldpQueryDynamicCodeTrust -> S_OK (trusted)\n");
    return S_OK;
}

// =====================================================================
// Section 8d: Time Hooks
//
// Bypass Flash.ocx EOL kill switch. Flash Player 32.0.0.465 has a
// hardcoded date check: after January 12, 2021, Flash refuses to play
// ANY content. We spoof a pre-EOL date when called from Flash.ocx.
// =====================================================================

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
    return (addr >= s_flashBase && static_cast<DWORD>(addr - s_flashBase) < s_flashSize);
}

void WINAPI FlashLoader::Hooked_GetLocalTime(LPSYSTEMTIME lpSystemTime)
{
    reinterpret_cast<FN_GetLocalTime>(s_hooks[HK_GetLocalTime].pTrampoline)(lpSystemTime);
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
    reinterpret_cast<FN_GetSystemTime>(s_hooks[HK_GetSystemTime].pTrampoline)(lpSystemTime);
    if (IsCallerInFlash(_ReturnAddress())) {
        static int s_gstCount = 0;
        if (s_gstCount++ < 3)
            DbgTrace(L"[FlashIE] GetSystemTime from Flash -> spoofing to 2020-12-01 (call #%d)\n", s_gstCount);
        lpSystemTime->wYear = 2020;
        lpSystemTime->wMonth = 12;
        lpSystemTime->wDay = 1;
    }
}

static void WINAPI Hooked_GetSystemTimeAsFileTime(LPFILETIME lpFileTime)
{
    reinterpret_cast<FN_GetSystemTimeAsFileTime>(
        s_hooks[HK_GetSystemTimeAsFileTime].pTrampoline)(lpFileTime);
    if (IsCallerInFlash(_ReturnAddress())) {
        SYSTEMTIME st = {};
        st.wYear = 2020; st.wMonth = 12; st.wDay = 1;
        FILETIME ft;
        SystemTimeToFileTime(&st, &ft);
        *lpFileTime = ft;
        DbgTrace(L"[FlashIE] GetSystemTimeAsFileTime from Flash -> spoofing to 2020-12-01\n");
    }
}

// =====================================================================
// Section 8e: Flash QI Hook
//
// Intercepts IObjectSafety queries on Flash objects. Flash's EOL build
// removed IObjectSafety, so MSHTML blocks IDispatch delegation.
// =====================================================================

static HRESULT STDMETHODCALLTYPE Hooked_FlashQI(void* pThis, REFIID riid, void** ppv)
{
    if (IsEqualIID(riid, IID_IObjectSafety)) {
        auto origQI = reinterpret_cast<FN_FlashQueryInterface>(s_hooks[HK_FlashQI].pTrampoline);
        IUnknown* pFlashUnk = nullptr;
        origQI(pThis, IID_IUnknown, reinterpret_cast<void**>(&pFlashUnk));
        if (pFlashUnk) {
            *ppv = static_cast<IObjectSafety*>(new FlashSafetyTearoff(pFlashUnk));
            DbgTrace(L"[FlashIE] Flash::QI(IObjectSafety) -> HOOKED tearoff\n");
            return S_OK;
        }
    }
    auto origQI = reinterpret_cast<FN_FlashQueryInterface>(s_hooks[HK_FlashQI].pTrampoline);
    return origQI(pThis, riid, ppv);
}

static void MaybeHookFlashQI(IUnknown* pObj)
{
    if (s_hooks[HK_FlashQI].active) return;
    void** vtable = *reinterpret_cast<void***>(pObj);
    void* pFlashQI = vtable[0];
    if (InstallDetour(pFlashQI, reinterpret_cast<void*>(Hooked_FlashQI), s_hooks[HK_FlashQI])) {
        DbgTrace(L"[FlashIE] Hook Flash::QueryInterface: OK (addr=%p)\n", pFlashQI);
    } else {
        DbgTrace(L"[FlashIE] Hook Flash::QueryInterface: FAILED\n");
    }
}

// =====================================================================
// Section 8f: TypeLib Hook
//
// Flash.ocx's TypeLib GUID is {D27CDB6B-AE6D-11CF-96B8-444553540000}.
// Without registry entries, OLEAUT32 can't find it and returns
// TYPE_E_LIBNOTREGISTERED. We load from the OCX file directly.
// =====================================================================

static HRESULT WINAPI Hooked_LoadRegTypeLib(
    REFGUID rguid, WORD wVerMajor, WORD wVerMinor, LCID lcid, ITypeLib** pptlib)
{
    if (IsEqualGUID(rguid, GUID_FlashTypeLib) && pptlib) {
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
        s_hooks[HK_LoadRegTypeLib].pTrampoline)(rguid, wVerMajor, wVerMinor, lcid, pptlib);
    if (FAILED(hr)) {
        DbgTrace(L"[FlashIE] LoadRegTypeLib({%08X-...}) v%u.%u -> 0x%08X\n",
                 rguid.Data1, wVerMajor, wVerMinor, hr);
    }
    return hr;
}

// =====================================================================
// Section 8g: File System Redirect Hooks
//
// Redirects Flash data paths from %APPDATA%\{Macromedia,Adobe}\Flash Player
// to a local FlashData directory next to the exe. This preserves the
// "zero system pollution" invariant — no writes to %APPDATA%.
// =====================================================================

// Check if a path starts with a known Flash roaming dir and redirect
// it to our local FlashData directory.
static bool RedirectFlashDataPath(LPCWSTR lpFileName, wchar_t* outBuf, int outBufLen)
{
    if (!g_szFlashDataDir[0]) return false;

    // Strip \\?\ extended-length path prefix if present
    LPCWSTR path = lpFileName;
    if (wcsncmp(path, L"\\\\?\\", 4) == 0)
        path += 4;

    if (g_roamingFlashDirLen &&
        _wcsnicmp(path, g_szRoamingFlashDir, g_roamingFlashDirLen) == 0) {
        const wchar_t* suffix = path + g_roamingFlashDirLen;
        _snwprintf_s(outBuf, outBufLen, _TRUNCATE, L"%s%s", g_szFlashDataDir, suffix);
        return true;
    }

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

    DWORD attr = GetFileAttributesW(dir);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
        return;

    // Build from FlashData root down
    if (_wcsnicmp(dir, g_szFlashDataDir, g_flashDataDirLen) != 0)
        return;

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

static HANDLE WINAPI Hooked_CreateFileW(
    LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    auto orig = reinterpret_cast<FN_CreateFileW>(s_hooks[HK_CreateFileW].pTrampoline);

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

static BOOL WINAPI Hooked_CreateDirectoryW(
    LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes)
{
    auto orig = reinterpret_cast<FN_CreateDirectoryW>(s_hooks[HK_CreateDirectoryW].pTrampoline);

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

static DWORD WINAPI Hooked_GetFileAttributesW(LPCWSTR lpFileName)
{
    auto orig = reinterpret_cast<FN_GetFileAttributesW>(s_hooks[HK_GetFileAttributesW].pTrampoline);

    if (lpFileName) {
        wchar_t redirected[MAX_PATH];
        if (RedirectFlashDataPath(lpFileName, redirected, MAX_PATH)) {
            return orig(redirected);
        }
    }

    return orig(lpFileName);
}

static HANDLE WINAPI Hooked_FindFirstFileW(
    LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData)
{
    auto orig = reinterpret_cast<decltype(&FindFirstFileW)>(s_hooks[HK_FindFirstFileW].pTrampoline);

    if (lpFileName) {
        wchar_t redirected[MAX_PATH];
        if (RedirectFlashDataPath(lpFileName, redirected, MAX_PATH)) {
            return orig(redirected, lpFindFileData);
        }
    }

    return orig(lpFileName, lpFindFileData);
}

static BOOL WINAPI Hooked_MoveFileW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName)
{
    auto orig = reinterpret_cast<FN_MoveFileW>(s_hooks[HK_MoveFileW].pTrampoline);

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

static BOOL WINAPI Hooked_MoveFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, DWORD dwFlags)
{
    auto orig = reinterpret_cast<FN_MoveFileExW>(s_hooks[HK_MoveFileExW].pTrampoline);

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

static BOOL WINAPI Hooked_DeleteFileW(LPCWSTR lpFileName)
{
    auto orig = reinterpret_cast<FN_DeleteFileW>(s_hooks[HK_DeleteFileW].pTrampoline);

    if (lpFileName) {
        wchar_t redirected[MAX_PATH];
        if (RedirectFlashDataPath(lpFileName, redirected, MAX_PATH)) {
            return orig(redirected);
        }
    }

    return orig(lpFileName);
}

// =====================================================================
// Section 9: Memory Patching
//
// Neutralize hardcoded Flash CLSID block in IE/MSHTML DLLs.
// After Microsoft's Flash EOL (July 2021+), mshtml.dll and related
// DLLs contain a hardcoded list of blocked CLSIDs that includes
// {D27CDB6E-AE6D-11CF-96B8-444553540000}. MSHTML checks this list
// AFTER CoGetClassObject returns the factory and discards the result
// if the CLSID is blocked. We scan these DLLs for the Flash CLSID
// byte pattern and corrupt each occurrence so the comparison never
// matches. Our own CLSID constant (in flashie.exe) is unaffected.
// =====================================================================

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
        static const wchar_t flashStr[] = L"D27CDB6E";
        static const int flashStrBytes = 8 * sizeof(wchar_t); // 16 bytes
        for (DWORD off = 0; off <= imageSize - flashStrBytes; off += 2) {
            if (memcmp(base + off, flashStr, flashStrBytes) == 0) {
                DWORD oldProt;
                if (VirtualProtect(base + off, flashStrBytes, PAGE_READWRITE, &oldProt)) {
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

void FlashLoader::LazyPatchModules()
{
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    NeutralizeFlashBlock();
    DbgTrace(L"[FlashIE] LazyPatchModules: re-ran NeutralizeFlashBlock (mshtml.dll should be loaded now)\n");
}

// =====================================================================
// Section 10: Public API (Activate, InstallHooks, Deactivate)
// =====================================================================

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

        if (pGLT) InstallDetour(pGLT, reinterpret_cast<void*>(&Hooked_GetLocalTime), s_hooks[HK_GetLocalTime]);
        if (pGST) InstallDetour(pGST, reinterpret_cast<void*>(&Hooked_GetSystemTime), s_hooks[HK_GetSystemTime]);
        if (pGSTAFT) InstallDetour(pGSTAFT, reinterpret_cast<void*>(&Hooked_GetSystemTimeAsFileTime), s_hooks[HK_GetSystemTimeAsFileTime]);
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

void FlashLoader::InstallHooks()
{
    if (m_hooked || !m_pFactory) return;

    // --- Step 0: Force-load IE DLLs so we can patch and hook them ---
    LoadLibraryW(L"mshtml.dll");
    LoadLibraryW(L"urlmon.dll");
    LoadLibraryW(L"ieframe.dll");

    // --- Step 1: Neutralize any hardcoded Flash-blocked CLSID lists ---
    NeutralizeFlashBlock();

    // --- Step 2: Resolve module handles ---
    HMODULE hCombase    = GetModuleHandleW(L"combase.dll");
    HMODULE hOle32      = GetModuleHandleW(L"ole32.dll");
    HMODULE hKernelBase = GetModuleHandleW(L"kernelbase.dll");
    HMODULE hAdvapi32   = GetModuleHandleW(L"advapi32.dll");
    HMODULE hUrlmon     = GetModuleHandleW(L"urlmon.dll");
    HMODULE hK32        = GetModuleHandleW(L"kernel32.dll");
    HMODULE hOleAut32   = GetModuleHandleW(L"oleaut32.dll");
    HMODULE hWldp       = GetModuleHandleW(L"wldp.dll");
    if (!hWldp) hWldp   = LoadLibraryW(L"wldp.dll");

    // --- Step 3: Install inline detours ---
    // Order: registry hooks first (kill bit bypass), then COM hooks

    // Registry hooks
    ResolveAndHook("RegOpenKeyExW", L"RegOpenKeyExW",
        reinterpret_cast<void*>(&Hooked_RegOpenKeyExW), s_hooks[HK_RegOpenKeyExW],
        hKernelBase, hAdvapi32);

    ResolveAndHook("RegQueryValueExW", L"RegQueryValueExW",
        reinterpret_cast<void*>(&Hooked_RegQueryValueExW), s_hooks[HK_RegQueryValueExW],
        hKernelBase, hAdvapi32);

    ResolveAndHook("RegCloseKey", L"RegCloseKey",
        reinterpret_cast<void*>(&Hooked_RegCloseKey), s_hooks[HK_RegCloseKey],
        hKernelBase, hAdvapi32);

    // COM hooks
    ResolveAndHook("CoGetClassObject", L"CoGetClassObject",
        reinterpret_cast<void*>(&Hooked_CoGetClassObject), s_hooks[HK_CoGetClassObject],
        hCombase, hOle32);

    ResolveAndHook("CoCreateInstance", L"CoCreateInstance",
        reinterpret_cast<void*>(&Hooked_CoCreateInstance), s_hooks[HK_CoCreateInstance],
        hCombase, hOle32);

    ResolveAndHook("CLSIDFromProgID", L"CLSIDFromProgID",
        reinterpret_cast<void*>(&Hooked_CLSIDFromProgID), s_hooks[HK_CLSIDFromProgID],
        hCombase, hOle32);

    // URL moniker hooks
    ResolveAndHook("CoGetClassObjectFromURL", L"CoGetClassObjectFromURL",
        reinterpret_cast<void*>(&Hooked_CoGetClassObjectFromURL), s_hooks[HK_CoGetClassObjectFromURL],
        hUrlmon);

    ResolveAndHook("CoInternetIsFeatureEnabled", L"CoInternetIsFeatureEnabled",
        reinterpret_cast<void*>(&Hooked_CoInternetIsFeatureEnabled), s_hooks[HK_CoInternetIsFeatureEnabled],
        hUrlmon);

    // WLDP hooks (Windows 10+ ActiveX approval)
    ResolveAndHook("WldpIsClassInApprovedList", L"WldpIsClassInApprovedList",
        reinterpret_cast<void*>(&Hooked_WldpIsClassInApprovedList), s_hooks[HK_WldpIsClassInApprovedList],
        hWldp);

    ResolveAndHook("WldpQueryDynamicCodeTrust", L"WldpQueryDynamicCodeTrust",
        reinterpret_cast<void*>(&Hooked_WldpQueryDynamicCodeTrust), s_hooks[HK_WldpQueryDynamicCodeTrust],
        hWldp);

    // TypeLib hook
    ResolveAndHook("LoadRegTypeLib", L"LoadRegTypeLib",
        reinterpret_cast<void*>(&Hooked_LoadRegTypeLib), s_hooks[HK_LoadRegTypeLib],
        hOleAut32);

    // File system hooks (redirect Flash data paths to local FlashData dir)
    ResolveAndHook("CreateFileW", L"CreateFileW",
        reinterpret_cast<void*>(&Hooked_CreateFileW), s_hooks[HK_CreateFileW],
        hKernelBase, hK32);

    ResolveAndHook("CreateDirectoryW", L"CreateDirectoryW",
        reinterpret_cast<void*>(&Hooked_CreateDirectoryW), s_hooks[HK_CreateDirectoryW],
        hKernelBase, hK32);

    ResolveAndHook("GetFileAttributesW", L"GetFileAttributesW",
        reinterpret_cast<void*>(&Hooked_GetFileAttributesW), s_hooks[HK_GetFileAttributesW],
        hKernelBase, hK32);

    ResolveAndHook("FindFirstFileW", L"FindFirstFileW",
        reinterpret_cast<void*>(&Hooked_FindFirstFileW), s_hooks[HK_FindFirstFileW],
        hKernelBase, hK32);

    ResolveAndHook("MoveFileW", L"MoveFileW",
        reinterpret_cast<void*>(&Hooked_MoveFileW), s_hooks[HK_MoveFileW],
        hKernelBase, hK32);

    ResolveAndHook("MoveFileExW", L"MoveFileExW",
        reinterpret_cast<void*>(&Hooked_MoveFileExW), s_hooks[HK_MoveFileExW],
        hKernelBase, hK32);

    ResolveAndHook("DeleteFileW", L"DeleteFileW",
        reinterpret_cast<void*>(&Hooked_DeleteFileW), s_hooks[HK_DeleteFileW],
        hKernelBase, hK32);

    // Time hooks — already installed in Activate() (before Flash.ocx loads)
    DbgTrace(L"[FlashIE] Hook GetLocalTime: %s\n", s_hooks[HK_GetLocalTime].active ? L"OK (early)" : L"FAIL");
    DbgTrace(L"[FlashIE] Hook GetSystemTime: %s\n", s_hooks[HK_GetSystemTime].active ? L"OK (early)" : L"FAIL");
    DbgTrace(L"[FlashIE] Hook GetSystemTimeAsFileTime: %s\n", s_hooks[HK_GetSystemTimeAsFileTime].active ? L"OK (early)" : L"FAIL");

    m_hooked = true;
}

void FlashLoader::Deactivate()
{
    if (m_hooked) {
        for (int i = 0; i < HK_COUNT; i++)
            RemoveDetour(s_hooks[i]);
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
