// =====================================================================
// Section 1: Includes & Constants
// =====================================================================

#include "flash_loader.h"
#include "debug.h"
#include "text_encoding.h"

#include <windows.h>
#include <objbase.h>

#include "flash.h"      // MIDL-generated Flash COM interface definitions

#include <detours.h>
#include <jscriptcc/CCPreprocessor.h>
#include <swc_es5.h>

#include <exception>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include <string.h>
#include <stdint.h>
#include <intrin.h>
#include <shlwapi.h>
#include <ocidl.h>      // IQuickActivate and debug-probed OLE interfaces
#include <oleidl.h>     // IOleObject, IOleInPlaceObject, etc.
#include <objsafe.h>    // IObjectSafety
#include <activscp.h>   // IActiveScriptParse (architecture-selected IID)

namespace {

// Flash CLSID string form for comparisons
constexpr wchar_t FLASH_CLSID_STR[] = L"{D27CDB6E-AE6D-11CF-96B8-444553540000}";

// In-box JScript Active Scripting engines. VBScript implements the same parse
// interface, so the class ID must be checked before installing JScriptCC.
constexpr CLSID CLSID_JScript =
    {0xF414C260, 0x6AC0, 0x11CF, {0xB6, 0xD1, 0x00, 0xAA, 0x00, 0xBB, 0xBB, 0x58}};
constexpr CLSID CLSID_JScript9 =
    {0x16D51579, 0xA30B, 0x4C8B, {0xA2, 0x76, 0x0F, 0xF4, 0xDC, 0x41, 0xE7, 0x55}};

// Bounded hook tables, activation queues, and deferred retry count.
constexpr int MAX_SCRIPT_HOOKS = 4;
constexpr int MAX_PENDING = 16;
constexpr int MAX_DEFERRED = 16;
constexpr int MAX_DEFERRED_RETRIES = 50;

// =====================================================================
// Section 2: Function Pointer Typedefs
// =====================================================================

// --- COM / OLE ---

typedef HRESULT (STDAPICALLTYPE *FN_DllGetClassObject)(
    REFCLSID rclsid, REFIID riid, LPVOID* ppv);

typedef HRESULT (STDAPICALLTYPE *FN_CoGetClassObject)(
    REFCLSID rclsid, DWORD dwClsContext, LPVOID pvReserved,
    REFIID riid, LPVOID* ppv);

typedef HRESULT (STDAPICALLTYPE *FN_CoCreateInstance)(
    REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext,
    REFIID riid, LPVOID* ppv);

typedef HRESULT (STDAPICALLTYPE *FN_CoGetClassObjectFromURL)(
    REFCLSID rclsid, LPCWSTR szCodeURL,
    DWORD dwFileVersionMS, DWORD dwFileVersionLS,
    LPCWSTR szContentType, LPBINDCTX pBindCtx,
    DWORD dwClsContext, LPVOID pvReserved,
    REFIID riid, LPVOID* ppv);

typedef HRESULT (STDAPICALLTYPE *FN_CLSIDFromProgID)(
    LPCOLESTR lpszProgID, LPCLSID lpclsid);

// --- Registry ---

typedef LSTATUS (WINAPI *FN_RegOpenKeyExW)(
    HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions,
    REGSAM samDesired, PHKEY phkResult);

typedef LSTATUS (WINAPI *FN_RegQueryValueExW)(
    HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved,
    LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);

typedef LSTATUS (WINAPI *FN_RegCloseKey)(HKEY hKey);

// --- File ---

typedef BOOL (WINAPI *FN_DeleteFileA)(LPCSTR lpFileName);
typedef BOOL (WINAPI *FN_DeleteFileW)(LPCWSTR lpFileName);

typedef DWORD (WINAPI *FN_GetModuleFileNameA)(
    HMODULE hModule, LPSTR lpFilename, DWORD nSize);
typedef DWORD (WINAPI *FN_GetModuleFileNameW)(
    HMODULE hModule, LPWSTR lpFilename, DWORD nSize);

// --- Windows Lockdown Policy (WLDP) ---

typedef HRESULT (WINAPI *FN_WldpIsClassInApprovedList)(
    const CLSID* classID, void* hostInfo, BOOL* isApproved, DWORD optionalFlags);

typedef HRESULT (WINAPI *FN_WldpQueryDynamicCodeTrust)(
    HANDLE fileHandle, void* baseImage, DWORD imageSize);

// --- TypeLib ---

typedef HRESULT (WINAPI *FN_LoadRegTypeLib)(
    REFGUID rguid, WORD wVerMajor, WORD wVerMinor,
    LCID lcid, ITypeLib** pptlib);

typedef HRESULT (WINAPI *FN_LoadTypeLibEx)(
    LPCOLESTR szFile, REGKIND regkind, ITypeLib** pptlib);

// --- Script Engine ---

// IActiveScriptParse::ParseScriptText (vtable index 5)
typedef HRESULT (STDMETHODCALLTYPE *FN_ParseScriptText)(
    void* pThis, LPCOLESTR pstrCode, LPCOLESTR pstrItemName,
    IUnknown* punkContext, LPCOLESTR pstrDelimiter,
    DWORD_PTR dwSourceContextCookie, ULONG ulStartingLineNumber,
    DWORD dwFlags, VARIANT* pvarResult, EXCEPINFO* pexcepinfo);

// --- Flash object vtable ---

typedef HRESULT (STDMETHODCALLTYPE *FN_FlashQueryInterface)(
    void* pThis, REFIID riid, void** ppv);

typedef HRESULT (STDMETHODCALLTYPE *FN_OleSetClientSite)(
    IOleObject* pThis, IOleClientSite* pClientSite);

typedef HRESULT (STDMETHODCALLTYPE *FN_QuickActivate)(
    IQuickActivate* pThis, QACONTAINER* pQAContainer, QACONTROL* pQAControl);

// =====================================================================
// Section 3: Process-Wide State
// =====================================================================

class LoggingClassFactory;

enum class LoaderPhase {
    Inactive,
    Activated,
    HooksInstalled,
    CleanupPending,
};

enum FakeKeyType {
    FK_NONE = 0,
    FK_CLSID_ROOT,       // HKCR\CLSID\{D27CDB6E-...}
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
    FK_FLASHPLAYER_VER,  // HKLM\SOFTWARE\Macromedia\FlashPlayer[ActiveX]
    FK_BROWSER_EMULATION,// Real FEATURE_BROWSER_EMULATION key with one fake value
};

struct FakeKeyEntry {
    HKEY        hKey;
    FakeKeyType type;
};

struct ScriptVtableHook {
    void**             vtable;
    FN_ParseScriptText original;
};

struct PendingActivation {
    IOleObject*     pObj;
    IOleClientSite* pSite;
};

struct ApiHookState {
    FN_CoGetClassObject          coGetClassObject = nullptr;
    FN_CoCreateInstance          coCreateInstance = nullptr;
    FN_RegOpenKeyExW             regOpenKeyExW = nullptr;
    FN_RegQueryValueExW          regQueryValueExW = nullptr;
    FN_RegCloseKey               regCloseKey = nullptr;
    FN_DeleteFileA               deleteFileA = nullptr;
    FN_DeleteFileW               deleteFileW = nullptr;
    FN_GetModuleFileNameA        getModuleFileNameA = nullptr;
    FN_GetModuleFileNameW        getModuleFileNameW = nullptr;
    FN_CoGetClassObjectFromURL   coGetClassObjectFromURL = nullptr;
    FN_WldpIsClassInApprovedList wldpIsClassInApprovedList = nullptr;
    FN_WldpQueryDynamicCodeTrust wldpQueryDynamicCodeTrust = nullptr;
    FN_LoadRegTypeLib            loadRegTypeLib = nullptr;
    FN_LoadTypeLibEx             loadTypeLibEx = nullptr;
    FN_CLSIDFromProgID           clsidFromProgID = nullptr;
    bool                         installed = false;
};

struct FactoryState {
    SRWLOCK              lock = SRWLOCK_INIT;
    IClassFactory*       real = nullptr;
    LoggingClassFactory* wrapper = nullptr;
    LoggingClassFactory* published = nullptr;
    DWORD                cookie = 0;
};

struct FakeRegistryState {
    SRWLOCK                   lock = SRWLOCK_INIT;
    std::vector<FakeKeyEntry> keys;
};

struct FlashHookState {
    SRWLOCK                lock = SRWLOCK_INIT;
    FN_FlashQueryInterface queryInterface = nullptr;
    bool                   queryInterfaceHooked = false;
    FN_OleSetClientSite    setClientSite = nullptr;
    void**                 oleVtable = nullptr;
    FN_QuickActivate       quickActivate = nullptr;
    void**                 quickVtable = nullptr;
};

struct ScriptHookState {
    SRWLOCK          lock = SRWLOCK_INIT;
    ScriptVtableHook hooks[MAX_SCRIPT_HOOKS] = {};
    int              count = 0;
};

struct ActivationState {
    PendingActivation pending[MAX_PENDING] = {};
    int               pendingCount = 0;
    UINT_PTR          activateTimer = 0;
    PendingActivation deferred[MAX_DEFERRED] = {};
    int               deferredCount = 0;
    UINT_PTR          deferredTimer = 0;
    int               deferredRetries = 0;
};

struct LoaderState {
    SRWLOCK           lifecycleLock = SRWLOCK_INIT;
    LoaderPhase       phase = LoaderPhase::Inactive;
    DWORD             ownerThreadId = 0;
    HMODULE           ocxModule = nullptr;
    wchar_t           ocxPath[MAX_PATH] = {};
    wchar_t           exeName[MAX_PATH] = {};
    ApiHookState      api;
    FactoryState      factory;
    FakeRegistryState registry;
    FlashHookState    flashHooks;
    ScriptHookState   scriptHooks;
    ActivationState   activation;
};

LoaderState g_loader;

// =====================================================================
// Section 4: API Hook Target Resolution
// =====================================================================

void* ResolveTarget(
    HMODULE hPrimary, const char* funcName, HMODULE hFallback = nullptr)
{
    void* pTarget = nullptr;

    if (hPrimary)
        pTarget = reinterpret_cast<void*>(GetProcAddress(hPrimary, funcName));

    if (!pTarget && hFallback)
        pTarget = reinterpret_cast<void*>(GetProcAddress(hFallback, funcName));

    return pTarget;
}

void ResetApiHookPointers()
{
    g_loader.api = {};
}

// =====================================================================
// Section 5: Fake Registry Key System
// =====================================================================

bool TrackKey(HKEY hKey, FakeKeyType type)
{
    bool tracked = false;
    AcquireSRWLockExclusive(&g_loader.registry.lock);
    try {
        g_loader.registry.keys.push_back({ hKey, type });
        tracked = true;
    }
    catch (...) {
        // Do not let allocation failures escape a Win32 hook boundary.
    }
    ReleaseSRWLockExclusive(&g_loader.registry.lock);

    if (!tracked)
        DbgTrace(L"[FlashIE] Failed to track registry key\n");
    return tracked;
}

HKEY AllocFakeKey(FakeKeyType type)
{
    HKEY hReal = nullptr;
    if (g_loader.api.regOpenKeyExW) {
        g_loader.api.regOpenKeyExW(
            HKEY_CURRENT_USER, L"Environment", 0, KEY_READ, &hReal);
    }
    if (!hReal) return nullptr;

    if (!TrackKey(hReal, type)) {
        if (g_loader.api.regCloseKey)
            g_loader.api.regCloseKey(hReal);
        return nullptr;
    }
    return hReal;
}

FakeKeyType GetFakeKeyType(HKEY hKey)
{
    FakeKeyType type = FK_NONE;
    AcquireSRWLockShared(&g_loader.registry.lock);
    for (const auto& entry : g_loader.registry.keys) {
        if (entry.hKey == hKey) {
            type = entry.type;
            break;
        }
    }
    ReleaseSRWLockShared(&g_loader.registry.lock);
    return type;
}

bool CloseFakeKey(HKEY hKey)
{
    bool found = false;
    AcquireSRWLockExclusive(&g_loader.registry.lock);
    for (size_t i = 0; i < g_loader.registry.keys.size(); i++) {
        if (g_loader.registry.keys[i].hKey == hKey) {
            g_loader.registry.keys.erase(g_loader.registry.keys.begin() + i);
            found = true;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_loader.registry.lock);

    if (found && g_loader.api.regCloseKey)
        g_loader.api.regCloseKey(hKey);
    return found;
}

void CloseAllTrackedKeys()
{
    std::vector<FakeKeyEntry> keys;

    AcquireSRWLockExclusive(&g_loader.registry.lock);
    keys.swap(g_loader.registry.keys);
    ReleaseSRWLockExclusive(&g_loader.registry.lock);

    if (g_loader.api.regCloseKey) {
        for (const auto& entry : keys)
            g_loader.api.regCloseKey(entry.hKey);
    }
}

// =====================================================================
// Section 6: COM Wrapper Classes
// =====================================================================

// Forward declaration: installs IObjectSafety hook on Flash's QueryInterface
void MaybeHookFlashQI(IUnknown* pObj);
// Forward declaration: installs SetClientSite hook for forced activation
void MaybeHookFlashSetClientSite(IUnknown* pObj);
// Forward declaration: installs QuickActivate hook for iframe forced activation
void MaybeHookFlashQuickActivate(IUnknown* pObj);
// Forward declaration: hooks ParseScriptText for JScriptCC and SWC processing
void MaybeHookScriptParseText(REFCLSID rclsid, IUnknown* pObj);

class LoggingClassFactory : public IClassFactory {
    IClassFactory* m_real;
    LONG m_ref;
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
    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&m_ref));
    }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG ref = static_cast<ULONG>(InterlockedDecrement(&m_ref));
        if (ref == 0) delete this;
        return ref;
    }
    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        DbgTrace(L"[FlashIE] Factory::CreateInstance riid={%08X-...} outer=%p\n",
                 riid.Data1, pUnkOuter);
        HRESULT hr = m_real->CreateInstance(pUnkOuter, riid, ppv);
        DbgTrace(L"[FlashIE] Factory::CreateInstance -> hr=0x%08X obj=%p\n",
                 hr, (ppv && SUCCEEDED(hr)) ? *ppv : nullptr);
        if (SUCCEEDED(hr) && ppv && *ppv) {
            IUnknown* pObj = static_cast<IUnknown*>(*ppv);
#ifdef _DEBUG
            // Probe key interfaces to see what MSHTML asks for
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
#endif
            // Hook Flash's QueryInterface to inject IObjectSafety support
            MaybeHookFlashQI(pObj);
            // Hook Flash's SetClientSite to force in-place activation
            MaybeHookFlashSetClientSite(pObj);
            // Hook Flash's QuickActivate for iframe activation
            MaybeHookFlashQuickActivate(pObj);
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
    LONG m_ref;
public:
    // Consumes the reference returned by QI(IID_IUnknown).
    explicit FlashSafetyTearoff(IUnknown* pFlash) : m_pFlash(pFlash), m_ref(1) {}
    ~FlashSafetyTearoff() { if (m_pFlash) m_pFlash->Release(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IObjectSafety) {
            *ppv = static_cast<IObjectSafety*>(this);
            AddRef();
            return S_OK;
        }
        return m_pFlash->QueryInterface(riid, ppv);
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&m_ref));
    }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG ref = static_cast<ULONG>(InterlockedDecrement(&m_ref));
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
// Section 7a: COM Hooks
// =====================================================================

// Return a referenced factory snapshot so shutdown cannot invalidate it while
// a process-wide COM hook is using it.
IClassFactory* AcquireFlashFactory(REFCLSID rclsid)
{
    if (!IsEqualCLSID(rclsid, CLSID_ShockwaveFlash))
        return nullptr;

    IClassFactory* factory = nullptr;
    AcquireSRWLockShared(&g_loader.factory.lock);
    if (g_loader.factory.published) {
        factory = static_cast<IClassFactory*>(g_loader.factory.published);
        factory->AddRef();
    }
    ReleaseSRWLockShared(&g_loader.factory.lock);
    return factory;
}

HRESULT STDAPICALLTYPE Hooked_CoGetClassObject(
    REFCLSID rclsid, DWORD dwClsContext, LPVOID pvReserved,
    REFIID riid, LPVOID* ppv)
{
    if (IClassFactory* factory = AcquireFlashFactory(rclsid)) {
        HRESULT hr = factory->QueryInterface(riid, ppv);
        factory->Release();
        DbgTrace(L"[FlashIE] CoGetClassObject(Flash) -> hr=0x%08X ppv=%p\n", hr, ppv ? *ppv : nullptr);
        return hr;
    }
    HRESULT hr = g_loader.api.coGetClassObject(
        rclsid, dwClsContext, pvReserved, riid, ppv);
    DbgTrace(L"[FlashIE] CoGetClassObject({%08X-...}) -> hr=0x%08X\n", rclsid.Data1, hr);
    return hr;
}

HRESULT STDAPICALLTYPE Hooked_CoCreateInstance(
    REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext,
    REFIID riid, LPVOID* ppv)
{
    if (IClassFactory* factory = AcquireFlashFactory(rclsid)) {
        HRESULT hr = factory->CreateInstance(pUnkOuter, riid, ppv);
        factory->Release();
        DbgTrace(L"[FlashIE] CoCreateInstance(Flash) -> hr=0x%08X\n", hr);
        return hr;
    }
    HRESULT hr = g_loader.api.coCreateInstance(
        rclsid, pUnkOuter, dwClsContext, riid, ppv);
    DbgTrace(L"[FlashIE] CoCreateInstance({%08X-...}) -> hr=0x%08X\n", rclsid.Data1, hr);
    // Hook script engine ParseScriptText for JScriptCC and SWC processing
    if (SUCCEEDED(hr) && ppv && *ppv)
        MaybeHookScriptParseText(rclsid, static_cast<IUnknown*>(*ppv));
    return hr;
}

// CoGetClassObjectFromURL (urlmon.dll) — the normal MSHTML code path
// for loading ActiveX controls from <object> tags.
HRESULT STDAPICALLTYPE Hooked_CoGetClassObjectFromURL(
    REFCLSID rclsid, LPCWSTR szCodeURL,
    DWORD dwFileVersionMS, DWORD dwFileVersionLS,
    LPCWSTR szContentType, LPBINDCTX pBindCtx,
    DWORD dwClsContext, LPVOID pvReserved,
    REFIID riid, LPVOID* ppv)
{
    if (IClassFactory* factory = AcquireFlashFactory(rclsid)) {
        HRESULT hr = factory->QueryInterface(riid, ppv);
        factory->Release();
        DbgTrace(L"[FlashIE] CoGetClassObjectFromURL(Flash) -> hr=0x%08X\n", hr);
        return hr;
    }
    return g_loader.api.coGetClassObjectFromURL(
        rclsid, szCodeURL, dwFileVersionMS, dwFileVersionLS,
        szContentType, pBindCtx, dwClsContext, pvReserved, riid, ppv);
}

// CLSIDFromProgID — make "ShockwaveFlash.ShockwaveFlash" ProgID resolve
// to Flash CLSID. On Win10, CLSIDFromProgID uses the COM catalog
// (cached in-process) rather than calling RegOpenKeyExW. Critical for
// JavaScript "new ActiveXObject(...)" calls.
HRESULT STDAPICALLTYPE Hooked_CLSIDFromProgID(
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
    return g_loader.api.clsidFromProgID(lpszProgID, lpclsid);
}

// =====================================================================
// Section 7b: Registry Hooks
//
// Intercepts registry access to:
// - Bypass Flash ActiveX kill bit (KB4561600)
// - Fake Flash CLSID registration (InprocServer32, TypeLib, ProgID, etc.)
// - Fake MIME type -> CLSID mapping
// - Fake FEATURE_BROWSER_EMULATION value
// =====================================================================

// Helper to check if a subkey path ends with a specific suffix (case-insensitive)
bool SubKeyEndsWith(LPCWSTR lpSubKey, LPCWSTR suffix)
{
    size_t keyLen = wcslen(lpSubKey);
    size_t sufLen = wcslen(suffix);
    if (keyLen < sufLen) return false;
    return _wcsicmp(lpSubKey + keyLen - sufLen, suffix) == 0;
}

// Helper: fill a REG_SZ value into the query result buffer
LSTATUS FakeRegSz(LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData, const wchar_t* val)
{
    if (lpData && !lpcbData)
        return ERROR_INVALID_PARAMETER;

    DWORD needed = (DWORD)((wcslen(val) + 1) * sizeof(wchar_t));
    if (lpType) *lpType = REG_SZ;
    if (lpcbData) {
        DWORD avail = *lpcbData;
        *lpcbData = needed;
        if (lpData && avail < needed)
            return ERROR_MORE_DATA;
        if (lpData)
            memcpy(lpData, val, needed);
    }
    return ERROR_SUCCESS;
}

LSTATUS FakeRegDword(LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData, DWORD val)
{
    if (lpData && !lpcbData)
        return ERROR_INVALID_PARAMETER;

    if (lpType) *lpType = REG_DWORD;
    if (lpcbData) {
        DWORD avail = *lpcbData;
        *lpcbData = sizeof(DWORD);
        if (lpData && avail < sizeof(DWORD))
            return ERROR_MORE_DATA;
        if (lpData)
            memcpy(lpData, &val, sizeof(DWORD));
    }
    return ERROR_SUCCESS;
}

// Table-driven subkey mapping for opens under fake parent keys.
// parentOnly: if not FK_NONE, only matches when parent has this type.
struct FakeSubKeyEntry {
    const wchar_t* name;
    FakeKeyType type;
    FakeKeyType parentOnly;
};

constexpr FakeSubKeyEntry FAKE_SUB_KEYS[] = {
    { L"CLSID",                    FK_PROGID_CLSID,   FK_PROGID_ROOT },
    { L"CurVer",                   FK_PROGID_CURVER,  FK_NONE },
    { L"InprocServer32",           FK_INPROC,         FK_NONE },
    { L"MiscStatus\\1",            FK_MISCSTATUS1,    FK_NONE },
    { L"1",                        FK_MISCSTATUS1,    FK_NONE },
    { L"MiscStatus",               FK_MISCSTATUS,     FK_NONE },
    { L"ProgID",                   FK_PROGID,         FK_NONE },
    { L"TypeLib",                  FK_TYPELIB,        FK_NONE },
    { L"Control",                  FK_CONTROL,        FK_NONE },
    { L"Version",                  FK_VERSION,        FK_NONE },
    { L"VersionIndependentProgID", FK_VERSION,        FK_NONE },
    { L"InstalledVersion",         FK_INSTALLED_VER,  FK_NONE },
};

// Table-driven suffix matching for CLSID absolute-path lookups.
struct ClsidSuffixEntry {
    const wchar_t* suffix;
    FakeKeyType type;
};

constexpr ClsidSuffixEntry CLSID_SUFFIXES[] = {
    { L"InprocServer32",           FK_INPROC },
    { L"MiscStatus\\1",            FK_MISCSTATUS1 },
    { L"MiscStatus",               FK_MISCSTATUS },
    { L"ProgID",                   FK_PROGID },
    { L"TypeLib",                  FK_TYPELIB },
    { L"Control",                  FK_CONTROL },
    { L"Version",                  FK_VERSION },
    { L"VersionIndependentProgID", FK_VERSION },
};

// Helper: allocate fake key, set output, log, and return ERROR_SUCCESS.
LSTATUS ReturnFakeKey(FakeKeyType type, PHKEY phkResult,
                      LPCWSTR lpSubKey, const wchar_t* desc)
{
    HKEY h = AllocFakeKey(type);
    if (h && phkResult) {
        *phkResult = h;
        DbgTrace(L"[FlashIE] RegOpenKeyExW FAKE: %s -> %s\n", lpSubKey, desc);
        return ERROR_SUCCESS;
    }
    return ERROR_FILE_NOT_FOUND;
}

bool IsPathBoundary(wchar_t ch)
{
    return ch == L'\0' || ch == L'\\';
}

const wchar_t* FindPathElementI(LPCWSTR path, LPCWSTR element)
{
    size_t elementLength = wcslen(element);
    const wchar_t* cursor = path;
    while (const wchar_t* match = StrStrIW(cursor, element)) {
        bool validStart = match == path || match[-1] == L'\\';
        if (validStart && IsPathBoundary(match[elementLength]))
            return match;
        cursor = match + 1;
    }
    return nullptr;
}

// Check if lpSubKey contains a complete ShockwaveFlash ProgID path element
// with an optional all-numeric version suffix no greater than 34.
bool IsFlashProgIDPath(LPCWSTR lpSubKey)
{
    static const wchar_t base[] = L"ShockwaveFlash.ShockwaveFlash";
    const wchar_t* cursor = lpSubKey;

    while (const wchar_t* progid = StrStrIW(cursor, base)) {
        if (progid != lpSubKey && progid[-1] != L'\\') {
            cursor = progid + 1;
            continue;
        }

        const wchar_t* suffix = progid + _countof(base) - 1;
        if (IsPathBoundary(*suffix))
            return true;
        if (*suffix != L'.') {
            cursor = progid + 1;
            continue;
        }

        const wchar_t* digit = suffix + 1;
        if (*digit < L'0' || *digit > L'9') {
            cursor = progid + 1;
            continue;
        }

        unsigned version = 0;
        while (*digit >= L'0' && *digit <= L'9') {
            version = version * 10 + static_cast<unsigned>(*digit - L'0');
            if (version > 34)
                break;
            digit++;
        }
        if (version <= 34 && IsPathBoundary(*digit))
            return true;
        cursor = progid + 1;
    }
    return false;
}

// Registry class paths use the complete braced GUID as one path element.
bool PathContainsFlashCLSID(LPCWSTR lpSubKey)
{
    return FindPathElementI(lpSubKey, FLASH_CLSID_STR) != nullptr;
}

LSTATUS WINAPI Hooked_RegOpenKeyExW(
    HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions,
    REGSAM samDesired, PHKEY phkResult)
{
    // ---- Handle subkey opens under fake Flash HKEY handles ----
    FakeKeyType parentType = GetFakeKeyType(hKey);
    if (parentType == FK_BROWSER_EMULATION) {
        return g_loader.api.regOpenKeyExW(
            hKey, lpSubKey, ulOptions, samDesired, phkResult);
    }

    if (parentType != FK_NONE && lpSubKey && phkResult) {
        FakeKeyType subType = FK_NONE;

        for (const auto& e : FAKE_SUB_KEYS) {
            if (e.parentOnly != FK_NONE && parentType != e.parentOnly)
                continue;
            if (_wcsicmp(lpSubKey, e.name) == 0) {
                subType = e.type;
                break;
            }
        }
        if (subType == FK_NONE && _wcsnicmp(lpSubKey, L"Implemented Categories\\", 23) == 0)
            subType = FK_IMPL_CATEGORY;

        if (subType != FK_NONE) {
            HKEY h = AllocFakeKey(subType);
            if (h) { *phkResult = h; return ERROR_SUCCESS; }
        }
        DbgTrace(L"[FlashIE] RegOpenKeyExW FAKE parent, unknown subkey: %s\n", lpSubKey);
        return ERROR_FILE_NOT_FOUND;
    }

    if (lpSubKey) {
        bool hasFlashClsid = PathContainsFlashCLSID(lpSubKey);

        // ---- Flash kill bit bypass ----
        if (hasFlashClsid) {
            if (FindPathElementI(lpSubKey, L"ActiveX Compatibility") ||
                FindPathElementI(lpSubKey, L"Extension Compatibility")) {
                DbgTrace(L"[FlashIE] RegOpenKeyExW BLOCKED (kill bit): %s\n", lpSubKey);
                return ERROR_FILE_NOT_FOUND;
            }
        }

        // ---- Fake Flash ProgID registration ----
        if (IsFlashProgIDPath(lpSubKey) && phkResult) {
            FakeKeyType type = SubKeyEndsWith(lpSubKey, L"\\CLSID")
                ? FK_PROGID_CLSID : FK_PROGID_ROOT;
            return ReturnFakeKey(type, phkResult, lpSubKey,
                type == FK_PROGID_CLSID ? L"ProgID\\CLSID" : L"ProgID root");
        }

        // ---- Fake Flash CLSID subkeys ----
        if (hasFlashClsid) {
            FakeKeyType fkType = FK_NONE;
            const wchar_t* fkName = nullptr;

            if (SubKeyEndsWith(lpSubKey, FLASH_CLSID_STR)) {
                fkType = FK_CLSID_ROOT; fkName = L"CLSID root";
            } else {
                for (const auto& e : CLSID_SUFFIXES) {
                    if (SubKeyEndsWith(lpSubKey, e.suffix)) {
                        fkType = e.type; fkName = e.suffix;
                        break;
                    }
                }
            }

            if (fkType != FK_NONE && phkResult)
                return ReturnFakeKey(fkType, phkResult, lpSubKey, fkName);

            DbgTrace(L"[FlashIE] RegOpenKeyExW ALLOW (Flash): %s\n", lpSubKey);
        }

        // ---- Fake MIME type -> CLSID mapping ----
        if (FindPathElementI(lpSubKey, L"application/x-shockwave-flash") &&
            FindPathElementI(lpSubKey, L"Content Type"))
            return ReturnFakeKey(FK_MIME, phkResult, lpSubKey, L"MIME mapping");

        // ---- Fake Flash Player version info ----
        // SWFObject and similar JS detection check Macromedia\FlashPlayer registry keys.
        if ((FindPathElementI(lpSubKey, L"Macromedia\\FlashPlayer") ||
             FindPathElementI(lpSubKey, L"Macromedia\\FlashPlayerActiveX")) && phkResult)
            return ReturnFakeKey(FK_FLASHPLAYER_VER, phkResult, lpSubKey, L"FlashPlayer version");

        // ---- FEATURE_BROWSER_EMULATION tracking ----
        if (FindPathElementI(lpSubKey, L"FEATURE_BROWSER_EMULATION")) {
            LSTATUS res = g_loader.api.regOpenKeyExW(
                hKey, lpSubKey, ulOptions, samDesired, phkResult);
            if (res == ERROR_SUCCESS && phkResult)
                TrackKey(*phkResult, FK_BROWSER_EMULATION);
            return res;
        }
    }
    LSTATUS res = g_loader.api.regOpenKeyExW(
        hKey, lpSubKey, ulOptions, samDesired, phkResult);
    DbgTrace(L"[FlashIE] RegOpenKeyExW: %s -> 0x%X\n", lpSubKey ? lpSubKey : L"(null)", res);
    return res;
}

LSTATUS WINAPI Hooked_RegQueryValueExW(
    HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved,
    LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    // ---- Fake Flash CLSID registration values ----
    FakeKeyType fkt = GetFakeKeyType(hKey);
    if (fkt == FK_BROWSER_EMULATION) {
        if (lpValueName && g_loader.exeName[0] &&
            _wcsicmp(lpValueName, g_loader.exeName) == 0) {
            DbgTrace(L"[FlashIE] RegQueryValueExW: FEATURE_BROWSER_EMULATION -> 11001\n");
            return FakeRegDword(lpType, lpData, lpcbData, 11001);
        }
        return g_loader.api.regQueryValueExW(
            hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
    }

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
                DbgTrace(L"[FlashIE] RegQueryValueExW FAKE: InprocServer32 -> %s\n",
                         g_loader.ocxPath);
                return FakeRegSz(lpType, lpData, lpcbData, g_loader.ocxPath);
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

        case FK_FLASHPLAYER_VER:
            if (!lpValueName || lpValueName[0] == 0)
                return FakeRegSz(lpType, lpData, lpcbData, g_loader.ocxPath);
            if (_wcsicmp(lpValueName, L"CurrentVersion") == 0)
                return FakeRegSz(lpType, lpData, lpcbData, L"34.0");
            if (_wcsicmp(lpValueName, L"Version") == 0)
                return FakeRegSz(lpType, lpData, lpcbData, L"34.0.0.330");
            if (_wcsicmp(lpValueName, L"PlayerPath") == 0)
                return FakeRegSz(lpType, lpData, lpcbData, g_loader.ocxPath);
            return ERROR_FILE_NOT_FOUND;

        default:
            break;
        }
    }

    return g_loader.api.regQueryValueExW(
        hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
}

LSTATUS WINAPI Hooked_RegCloseKey(HKEY hKey)
{
    if (CloseFakeKey(hKey)) {
        return ERROR_SUCCESS;
    }
    return g_loader.api.regCloseKey(hKey);
}

// =====================================================================
// Section 7c: Security Hooks
// =====================================================================

bool IsFlashCodeImage(HANDLE fileHandle, void* baseImage)
{
    if (baseImage && g_loader.ocxModule) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(baseImage, &mbi, sizeof(mbi)) == sizeof(mbi) &&
            mbi.AllocationBase == g_loader.ocxModule) {
            return true;
        }
    }

    if (fileHandle && fileHandle != INVALID_HANDLE_VALUE && g_loader.ocxPath[0]) {
        wchar_t path[MAX_PATH + 4] = {};
        DWORD length = GetFinalPathNameByHandleW(
            fileHandle, path, _countof(path), FILE_NAME_NORMALIZED);
        if (length > 0 && length < _countof(path)) {
            const wchar_t* normalized = path;
            if (wcsncmp(normalized, L"\\\\?\\", 4) == 0)
                normalized += 4;
            if (_wcsicmp(normalized, g_loader.ocxPath) == 0)
                return true;
        }
    }

    return false;
}

// Windows 10+ MSHTML calls wldp!WldpIsClassInApprovedList to check if
// an ActiveX CLSID is approved for instantiation. Only the local Flash class
// is exempted; every other class keeps the system policy result.
HRESULT WINAPI Hooked_WldpIsClassInApprovedList(
    const CLSID* classID, void* hostInfo, BOOL* isApproved, DWORD optionalFlags)
{
    if (classID && IsEqualCLSID(*classID, CLSID_ShockwaveFlash)) {
        if (isApproved) *isApproved = TRUE;
        DbgTrace(L"[FlashIE] WldpIsClassInApprovedList(Flash) -> approved\n");
        return S_OK;
    }

    return g_loader.api.wldpIsClassInApprovedList(
        classID, hostInfo, isApproved, optionalFlags);
}

HRESULT WINAPI Hooked_WldpQueryDynamicCodeTrust(
    HANDLE fileHandle, void* baseImage, DWORD imageSize)
{
    if (IsFlashCodeImage(fileHandle, baseImage)) {
        DbgTrace(L"[FlashIE] WldpQueryDynamicCodeTrust(Flash.ocx) -> trusted\n");
        return S_OK;
    }

    return g_loader.api.wldpQueryDynamicCodeTrust(fileHandle, baseImage, imageSize);
}

// =====================================================================
// Section 7d: Flash QI Hook
//
// Intercepts IObjectSafety queries on Flash objects. Injecting this
// interface is required by MSHTML for scripting access. Property-bag
// values, including allowScriptAccess, remain controlled by the page.
// =====================================================================

HRESULT STDMETHODCALLTYPE Hooked_FlashQI(void* pThis, REFIID riid, void** ppv)
{
    if (IsEqualIID(riid, IID_IObjectSafety)) {
        IUnknown* pFlashUnk = nullptr;
        g_loader.flashHooks.queryInterface(
            pThis, IID_IUnknown, reinterpret_cast<void**>(&pFlashUnk));
        if (pFlashUnk) {
            FlashSafetyTearoff* tearoff =
                new (std::nothrow) FlashSafetyTearoff(pFlashUnk);
            if (!tearoff) {
                pFlashUnk->Release();
                return E_OUTOFMEMORY;
            }
            *ppv = static_cast<IObjectSafety*>(tearoff);
            DbgTrace(L"[FlashIE] Flash::QI(IObjectSafety) -> HOOKED tearoff\n");
            return S_OK;
        }
    }

    return g_loader.flashHooks.queryInterface(pThis, riid, ppv);
}

void MaybeHookFlashQI(IUnknown* pObj)
{
    AcquireSRWLockExclusive(&g_loader.flashHooks.lock);
    if (g_loader.flashHooks.queryInterfaceHooked) {
        ReleaseSRWLockExclusive(&g_loader.flashHooks.lock);
        return;
    }
    void** vtable = *reinterpret_cast<void***>(pObj);
    void* pFlashQI = vtable[0];

    g_loader.flashHooks.queryInterface =
        reinterpret_cast<FN_FlashQueryInterface>(pFlashQI);
    LONG error = DetourTransactionBegin();
    const bool transactionStarted = error == NO_ERROR;
    if (error == NO_ERROR)
        error = DetourUpdateThread(GetCurrentThread());
    if (error == NO_ERROR) {
        error = DetourAttach(
            reinterpret_cast<void**>(&g_loader.flashHooks.queryInterface),
            reinterpret_cast<void*>(Hooked_FlashQI));
    }
    if (error != NO_ERROR) {
        if (transactionStarted)
            DetourTransactionAbort();
    } else {
        error = DetourTransactionCommit();
    }
    if (error == NO_ERROR) {
        g_loader.flashHooks.queryInterfaceHooked = true;
        DbgTrace(L"[FlashIE] Hook Flash::QueryInterface: OK (addr=%p)\n", pFlashQI);
    } else {
        g_loader.flashHooks.queryInterface = nullptr;
        DbgTrace(L"[FlashIE] Hook Flash::QueryInterface: FAILED (error=%d)\n", error);
    }
    ReleaseSRWLockExclusive(&g_loader.flashHooks.lock);
}

// =====================================================================
// Section 7e: Flash Forced In-Place Activation
//
// MSHTML creates Flash objects but defers DoVerb(INPLACEACTIVATE)
// until a user click (Windows 10 Flash phase-out behavior).
// We hook Flash's IOleObject::SetClientSite; once MSHTML sets the site,
// we schedule an owning-STA timer to obtain the in-place site's HWND/RECT
// and call DoVerb(OLEIVERB_INPLACEACTIVATE), forcing the control to activate
// without user interaction. SetClientSite and QuickActivate vtable slots are
// restored during deactivation.
// =====================================================================

// Saved references for deferred re-activation (handles display:none iframes).
// A 200ms repeating timer calls DoVerb until the iframe becomes visible,
// up to 50 retries (10 seconds total).
bool IsActivationThread()
{
    return g_loader.ownerThreadId != 0 &&
           g_loader.ownerThreadId == GetCurrentThreadId();
}

HRESULT ActivateInPlace(IOleObject* pObj, IOleClientSite* pSite)
{
    IOleInPlaceSite* inPlaceSite = nullptr;
    HRESULT hr = pSite->QueryInterface(
        IID_IOleInPlaceSite, reinterpret_cast<void**>(&inPlaceSite));
    if (FAILED(hr) || !inPlaceSite)
        return FAILED(hr) ? hr : E_NOINTERFACE;

    HWND parent = nullptr;
    hr = inPlaceSite->GetWindow(&parent);

    RECT posRect = {};
    if (SUCCEEDED(hr) && parent) {
        IOleInPlaceFrame* frame = nullptr;
        IOleInPlaceUIWindow* doc = nullptr;
        RECT clipRect = {};
        OLEINPLACEFRAMEINFO frameInfo = {};
        frameInfo.cb = sizeof(frameInfo);

        HRESULT contextHr = inPlaceSite->GetWindowContext(
            &frame, &doc, &posRect, &clipRect, &frameInfo);
        if (frame) frame->Release();
        if (doc) doc->Release();

        if (FAILED(contextHr) && !GetClientRect(parent, &posRect))
            hr = HRESULT_FROM_WIN32(GetLastError());
    } else if (SUCCEEDED(hr)) {
        hr = E_HANDLE;
    }

    inPlaceSite->Release();
    if (FAILED(hr))
        return hr;

    return pObj->DoVerb(
        OLEIVERB_INPLACEACTIVATE, nullptr, pSite, 0, parent, &posRect);
}

void CALLBACK DeferredActivateTimerProc(HWND, UINT, UINT_PTR idTimer, DWORD)
{
    g_loader.activation.deferredRetries++;

    for (int i = 0; i < g_loader.activation.deferredCount; i++) {
        IOleObject* pObj = g_loader.activation.deferred[i].pObj;
        IOleClientSite* pSite = g_loader.activation.deferred[i].pSite;
        if (pObj && pSite) {
            ActivateInPlace(pObj, pSite);
        }
    }

    // Stop after max retries — release references and kill timer
    if (g_loader.activation.deferredRetries >= MAX_DEFERRED_RETRIES) {
        KillTimer(nullptr, idTimer);
        g_loader.activation.deferredTimer = 0;
        for (int i = 0; i < g_loader.activation.deferredCount; i++) {
            if (g_loader.activation.deferred[i].pSite)
                g_loader.activation.deferred[i].pSite->Release();
            if (g_loader.activation.deferred[i].pObj)
                g_loader.activation.deferred[i].pObj->Release();
        }
        g_loader.activation.deferredCount = 0;
        DbgTrace(L"[FlashIE] DeferredActivate stopped after %d retries\n",
                 g_loader.activation.deferredRetries);
    }
}

void CALLBACK ForceActivateTimerProc(HWND, UINT, UINT_PTR idTimer, DWORD)
{
    KillTimer(nullptr, idTimer);
    g_loader.activation.activateTimer = 0;

    for (int i = 0; i < g_loader.activation.pendingCount; i++) {
        IOleObject* pObj = g_loader.activation.pending[i].pObj;
        IOleClientSite* pSite = g_loader.activation.pending[i].pSite;
        if (pObj && pSite) {
            HRESULT hr = ActivateInPlace(pObj, pSite);
            DbgTrace(L"[FlashIE] ForceActivate DoVerb -> hr=0x%08X\n", hr);

            // Save for deferred re-activation: Flash in display:none iframes
            // may need re-activation when the iframe becomes visible.
            if (g_loader.activation.deferredCount < MAX_DEFERRED) {
                pObj->AddRef();
                pSite->AddRef();
                int index = g_loader.activation.deferredCount++;
                g_loader.activation.deferred[index].pObj = pObj;
                g_loader.activation.deferred[index].pSite = pSite;
            }

            pSite->Release();
            pObj->Release();
        }
    }
    g_loader.activation.pendingCount = 0;

    // Schedule repeating re-activation every 200ms for display:none iframes
    if (g_loader.activation.deferredCount > 0 &&
        !g_loader.activation.deferredTimer) {
        g_loader.activation.deferredRetries = 0;
        g_loader.activation.deferredTimer =
            SetTimer(nullptr, 0, 200, DeferredActivateTimerProc);
    }
}

// Release any pending activation references (called during shutdown).
void FlushPendingActivations()
{
    if (g_loader.activation.activateTimer) {
        KillTimer(nullptr, g_loader.activation.activateTimer);
        g_loader.activation.activateTimer = 0;
    }
    if (g_loader.activation.deferredTimer) {
        KillTimer(nullptr, g_loader.activation.deferredTimer);
        g_loader.activation.deferredTimer = 0;
    }
    for (int i = 0; i < g_loader.activation.pendingCount; i++) {
        if (g_loader.activation.pending[i].pSite)
            g_loader.activation.pending[i].pSite->Release();
        if (g_loader.activation.pending[i].pObj)
            g_loader.activation.pending[i].pObj->Release();
    }
    g_loader.activation.pendingCount = 0;
    for (int i = 0; i < g_loader.activation.deferredCount; i++) {
        if (g_loader.activation.deferred[i].pSite)
            g_loader.activation.deferred[i].pSite->Release();
        if (g_loader.activation.deferred[i].pObj)
            g_loader.activation.deferred[i].pObj->Release();
    }
    g_loader.activation.deferredCount = 0;
    g_loader.activation.deferredRetries = 0;
}

HRESULT STDMETHODCALLTYPE Hooked_OleSetClientSite(
    IOleObject* pThis, IOleClientSite* pClientSite)
{
    HRESULT hr = g_loader.flashHooks.setClientSite(pThis, pClientSite);

    // When MSHTML sets a non-null client site, queue forced activation
    if (SUCCEEDED(hr) && pClientSite && IsActivationThread() &&
        g_loader.activation.pendingCount < MAX_PENDING) {
        pThis->AddRef();
        pClientSite->AddRef();
        int index = g_loader.activation.pendingCount++;
        g_loader.activation.pending[index].pObj = pThis;
        g_loader.activation.pending[index].pSite = pClientSite;
        // Coalesce: restart the timer so one callback handles all pending objects
        if (g_loader.activation.activateTimer)
            KillTimer(nullptr, g_loader.activation.activateTimer);
        g_loader.activation.activateTimer =
            SetTimer(nullptr, 0, 100, ForceActivateTimerProc);
        DbgTrace(L"[FlashIE] SetClientSite -> queued forced activation\n");
    }

    return hr;
}

void MaybeHookFlashSetClientSite(IUnknown* pObj)
{
    IOleObject* pOle = nullptr;
    pObj->QueryInterface(IID_IOleObject, reinterpret_cast<void**>(&pOle));
    if (!pOle) return;

    AcquireSRWLockExclusive(&g_loader.flashHooks.lock);
    if (g_loader.flashHooks.setClientSite) {
        ReleaseSRWLockExclusive(&g_loader.flashHooks.lock);
        pOle->Release();
        return;
    }

    void** vtable = *reinterpret_cast<void***>(pOle);
    // IOleObject::SetClientSite is vtable slot 3 (after QI, AddRef, Release)
    FN_OleSetClientSite original = reinterpret_cast<FN_OleSetClientSite>(vtable[3]);

    DWORD oldProtect;
    if (VirtualProtect(&vtable[3], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        g_loader.flashHooks.setClientSite = original;
        g_loader.flashHooks.oleVtable = vtable;
        vtable[3] = reinterpret_cast<void*>(Hooked_OleSetClientSite);
        VirtualProtect(&vtable[3], sizeof(void*), oldProtect, &oldProtect);
        DbgTrace(L"[FlashIE] Hook Flash IOleObject::SetClientSite: OK\n");
    }
    ReleaseSRWLockExclusive(&g_loader.flashHooks.lock);

    pOle->Release();
}

// Also hook IQuickActivate::QuickActivate — MSHTML in cross-domain
// iframes may use IQuickActivate instead of IOleObject::SetClientSite.
// QuickActivate sets the client site internally, bypassing our
// SetClientSite vtable hook.

HRESULT STDMETHODCALLTYPE Hooked_QuickActivate(
    IQuickActivate* pThis, QACONTAINER* pQAContainer, QACONTROL* pQAControl)
{
    HRESULT hr = g_loader.flashHooks.quickActivate(
        pThis, pQAContainer, pQAControl);

    if (SUCCEEDED(hr) && pQAContainer && pQAContainer->pClientSite &&
        IsActivationThread() &&
        g_loader.activation.pendingCount < MAX_PENDING) {
        // Get IOleObject from the Flash control to call DoVerb later
        IOleObject* pOle = nullptr;
        pThis->QueryInterface(IID_IOleObject, reinterpret_cast<void**>(&pOle));
        if (pOle) {
            IOleClientSite* pSite = pQAContainer->pClientSite;
            pOle->AddRef();
            pSite->AddRef();
            int index = g_loader.activation.pendingCount++;
            g_loader.activation.pending[index].pObj = pOle;
            g_loader.activation.pending[index].pSite = pSite;
            if (g_loader.activation.activateTimer)
                KillTimer(nullptr, g_loader.activation.activateTimer);
            g_loader.activation.activateTimer =
                SetTimer(nullptr, 0, 100, ForceActivateTimerProc);
            DbgTrace(L"[FlashIE] QuickActivate -> queued forced activation\n");
            pOle->Release(); // balance the QI AddRef (pending still holds a ref from AddRef above)
        }
    }

    return hr;
}

void MaybeHookFlashQuickActivate(IUnknown* pObj)
{
    IQuickActivate* pQA = nullptr;
    pObj->QueryInterface(IID_IQuickActivate, reinterpret_cast<void**>(&pQA));
    if (!pQA) return;

    AcquireSRWLockExclusive(&g_loader.flashHooks.lock);
    if (g_loader.flashHooks.quickActivate) {
        ReleaseSRWLockExclusive(&g_loader.flashHooks.lock);
        pQA->Release();
        return;
    }

    void** vtable = *reinterpret_cast<void***>(pQA);
    // IQuickActivate::QuickActivate is vtable slot 3 (after QI, AddRef, Release)
    FN_QuickActivate original = reinterpret_cast<FN_QuickActivate>(vtable[3]);

    DWORD oldProtect;
    if (VirtualProtect(&vtable[3], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        g_loader.flashHooks.quickActivate = original;
        g_loader.flashHooks.quickVtable = vtable;
        vtable[3] = reinterpret_cast<void*>(Hooked_QuickActivate);
        VirtualProtect(&vtable[3], sizeof(void*), oldProtect, &oldProtect);
        DbgTrace(L"[FlashIE] Hook Flash IQuickActivate::QuickActivate: OK\n");
    }
    ReleaseSRWLockExclusive(&g_loader.flashHooks.lock);

    pQA->Release();
}

bool RestoreFlashVtableSlot(
    void** vtable, void* hook, void* original)
{
    if (!vtable || !original)
        return true;

    DWORD oldProtect;
    if (!VirtualProtect(&vtable[3], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    if (vtable[3] == hook)
        vtable[3] = original;
    return VirtualProtect(
        &vtable[3], sizeof(void*), oldProtect, &oldProtect) != FALSE;
}

// =====================================================================
// Section 7f: TypeLib Hook
//
// Flash.ocx's TypeLib GUID is {D27CDB6B-AE6D-11CF-96B8-444553540000}.
// Without registry entries, OLEAUT32 can't find it and returns
// TYPE_E_LIBNOTREGISTERED. We load from the OCX file directly.
// =====================================================================

HRESULT WINAPI Hooked_LoadRegTypeLib(
    REFGUID rguid, WORD wVerMajor, WORD wVerMinor, LCID lcid, ITypeLib** pptlib)
{
    if (IsEqualGUID(rguid, LIBID_ShockwaveFlashObjects) && pptlib) {
        if (g_loader.api.loadTypeLibEx) {
            HRESULT hr = g_loader.api.loadTypeLibEx(
                g_loader.ocxPath, REGKIND_NONE, pptlib);
            DbgTrace(L"[FlashIE] LoadRegTypeLib(FlashTypeLib) -> LoadTypeLibEx(%s) hr=0x%08X\n",
                     g_loader.ocxPath, hr);
            return hr;
        }
    }
    HRESULT hr = g_loader.api.loadRegTypeLib(
        rguid, wVerMajor, wVerMinor, lcid, pptlib);
    if (FAILED(hr)) {
        DbgTrace(L"[FlashIE] LoadRegTypeLib({%08X-...}) v%u.%u -> 0x%08X\n",
                 rguid.Data1, wVerMajor, wVerMinor, hr);
    }
    return hr;
}

// =====================================================================
// Section 7g: Script Engine ParseScriptText Hook
//
// Hooks IActiveScriptParse::ParseScriptText on both JScript engines. Script
// code is converted to UTF-8 once, expanded through JScriptCC, then complete
// classic scripts are transpiled to ES5 through swc-es5-c-api. The final code
// is converted back to UTF-16 for JScript. Expression-mode calls bypass SWC
// because its ABI accepts scripts, not expressions. Each stage falls back to
// the latest valid code, and errors are reported through DbgTrace.
// =====================================================================

void TraceSwcFailure(swc_es5_status_t status,
                     const swc_es5_result_t* result)
{
    std::wstring diagnostic;
    if (result && TextEncoding::Utf8ToWide(
            swc_es5_result_error(result),
            swc_es5_result_error_length(result), diagnostic) &&
        !diagnostic.empty()) {
        DbgTrace(L"[FlashIE] SWC ES5 error (status=%u): %s\n",
                 static_cast<unsigned int>(status), diagnostic.c_str());
        return;
    }

    DbgTrace(L"[FlashIE] SWC ES5 failed with status=%u\n",
             static_cast<unsigned int>(status));
}

bool TranspileScriptToEs5(const std::string& input,
                          std::string& output)
{
    try {
        swc_es5_compiler_t* rawCompiler = nullptr;
        swc_es5_status_t status = swc_es5_compiler_create(&rawCompiler);
        if (status != SWC_ES5_STATUS_OK || !rawCompiler) {
            TraceSwcFailure(status, nullptr);
            return false;
        }

        using CompilerPtr = std::unique_ptr<
            swc_es5_compiler_t, decltype(&swc_es5_compiler_destroy)>;
        CompilerPtr compiler(rawCompiler, &swc_es5_compiler_destroy);

        swc_es5_result_t* rawResult = nullptr;
        const uint8_t* bytes = input.empty()
            ? nullptr
            : reinterpret_cast<const uint8_t*>(input.data());
        status = swc_es5_transform(
            compiler.get(), bytes, input.size(), &rawResult);

        using ResultPtr = std::unique_ptr<
            swc_es5_result_t, decltype(&swc_es5_result_destroy)>;
        ResultPtr result(rawResult, &swc_es5_result_destroy);
        if (status != SWC_ES5_STATUS_OK || !result) {
            TraceSwcFailure(status, result.get());
            return false;
        }

        const uint8_t* code = swc_es5_result_code(result.get());
        size_t codeLength = swc_es5_result_code_length(result.get());
        if (!code && codeLength != 0) {
            DbgTrace(L"[FlashIE] SWC ES5 returned an invalid code buffer\n");
            return false;
        }

        if (codeLength == 0) {
            output.clear();
        } else {
            output.assign(reinterpret_cast<const char*>(code), codeLength);
        }
        return true;
    }
    catch (const std::exception& ex) {
        DbgTrace(L"[FlashIE] SWC ES5 C++ failure: %hs\n", ex.what());
        return false;
    }
    catch (...) {
        DbgTrace(L"[FlashIE] SWC ES5 C++ failure\n");
        return false;
    }
}

bool ProcessScriptForJScript(LPCOLESTR input, DWORD flags,
                             std::wstring& output, bool& usedSwc)
{
    usedSwc = false;
    try {
        std::string utf8Source;
        if (!TextEncoding::WideToUtf8(input, utf8Source)) {
            DbgTrace(L"[FlashIE] Script input is not valid UTF-16\n");
            return false;
        }

        std::string processed;
        jscriptcc::CCErrorList errors;
        jscriptcc::CCPreprocessor preprocessor;

        const auto architecture = sizeof(void*) == 8
            ? jscriptcc::TargetArchitecture::Win64
            : jscriptcc::TargetArchitecture::Win32;

        bool ok = preprocessor.process(
            utf8Source.data(), utf8Source.size(), processed,
            jscriptcc::CCEnvironment(architecture), &errors);

        for (const auto& err : errors) {
            DbgTrace(L"[FlashIE] JScriptCC error: line %d col %d: %hs\n",
                     err.line, err.column, err.message.c_str());
        }
        if (!ok) {
            DbgTrace(L"[FlashIE] JScriptCC preprocessing failed, using original code\n");
            return false;
        }

        if (!processed.empty() && !(flags & SCRIPTTEXT_ISEXPRESSION)) {
            std::string transpiled;
            if (TranspileScriptToEs5(processed, transpiled)) {
                processed.swap(transpiled);
                usedSwc = true;
            }
        }

        if (!TextEncoding::Utf8ToWide(processed, output)) {
            DbgTrace(L"[FlashIE] Processed script is not valid UTF-8\n");
            return false;
        }
        return true;
    }
    catch (const std::exception& ex) {
        DbgTrace(L"[FlashIE] Script processing C++ failure: %hs\n", ex.what());
        return false;
    }
    catch (...) {
        DbgTrace(L"[FlashIE] Script processing C++ failure\n");
        return false;
    }
}

HRESULT STDMETHODCALLTYPE Hooked_ParseScriptText(
    void* pThis, LPCOLESTR pstrCode, LPCOLESTR pstrItemName,
    IUnknown* punkContext, LPCOLESTR pstrDelimiter,
    DWORD_PTR dwSourceContextCookie, ULONG ulStartingLineNumber,
    DWORD dwFlags, VARIANT* pvarResult, EXCEPINFO* pexcepinfo)
{
    void** vtable = *reinterpret_cast<void***>(pThis);
    FN_ParseScriptText original = nullptr;
    AcquireSRWLockShared(&g_loader.scriptHooks.lock);
    for (int i = 0; i < g_loader.scriptHooks.count; i++) {
        if (g_loader.scriptHooks.hooks[i].vtable == vtable) {
            original = g_loader.scriptHooks.hooks[i].original;
            break;
        }
    }
    ReleaseSRWLockShared(&g_loader.scriptHooks.lock);
    if (!original)
        return E_UNEXPECTED;

    std::wstring processedCode;
    bool usedSwc = false;
    LPCOLESTR code = pstrCode;
    if (pstrCode &&
        ProcessScriptForJScript(pstrCode, dwFlags, processedCode, usedSwc)) {
        code = processedCode.c_str();
        DbgTrace(L"[FlashIE] === ParseScriptText (%s) begin ===\n",
                 usedSwc ? L"SWC ES5" : L"JScriptCC");
        if (pstrItemName && pstrItemName[0])
            DbgTrace(L"[FlashIE]   item: %s\n", pstrItemName);
        DbgTrace(L"[FlashIE]   code: %s\n", code);
        DbgTrace(L"[FlashIE] === ParseScriptText end ===\n");
    }

    return original(pThis, code, pstrItemName, punkContext,
        pstrDelimiter, dwSourceContextCookie, ulStartingLineNumber,
        dwFlags, pvarResult, pexcepinfo);
}

bool IsJScriptEngine(REFCLSID rclsid)
{
    return IsEqualCLSID(rclsid, CLSID_JScript) ||
           IsEqualCLSID(rclsid, CLSID_JScript9);
}

void MaybeHookScriptParseText(REFCLSID rclsid, IUnknown* pObj)
{
    if (!IsJScriptEngine(rclsid))
        return;

    void* pParse = nullptr;
    if (FAILED(pObj->QueryInterface(IID_IActiveScriptParse, &pParse)) || !pParse)
        return;

    void** vtable = *reinterpret_cast<void***>(pParse);
    // IActiveScriptParse::ParseScriptText is vtable slot 5
    // (after QI=0, AddRef=1, Release=2, InitNew=3, AddScriptlet=4)
    void* pTarget = vtable[5];

    AcquireSRWLockExclusive(&g_loader.scriptHooks.lock);
    for (int i = 0; i < g_loader.scriptHooks.count; i++) {
        if (g_loader.scriptHooks.hooks[i].vtable == vtable) {
            ReleaseSRWLockExclusive(&g_loader.scriptHooks.lock);
            static_cast<IUnknown*>(pParse)->Release();
            return;
        }
    }
    if (g_loader.scriptHooks.count >= MAX_SCRIPT_HOOKS) {
        ReleaseSRWLockExclusive(&g_loader.scriptHooks.lock);
        DbgTrace(L"[FlashIE] Script vtable hook table is full\n");
        static_cast<IUnknown*>(pParse)->Release();
        return;
    }

    DWORD oldProtect;
    if (VirtualProtect(&vtable[5], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        vtable[5] = reinterpret_cast<void*>(Hooked_ParseScriptText);
        VirtualProtect(&vtable[5], sizeof(void*), oldProtect, &oldProtect);
        int index = g_loader.scriptHooks.count++;
        g_loader.scriptHooks.hooks[index].vtable = vtable;
        g_loader.scriptHooks.hooks[index].original =
            reinterpret_cast<FN_ParseScriptText>(pTarget);
        DbgTrace(L"[FlashIE] Hook IActiveScriptParse::ParseScriptText: OK (addr=%p)\n",
                 pTarget);
    }
    ReleaseSRWLockExclusive(&g_loader.scriptHooks.lock);

    static_cast<IUnknown*>(pParse)->Release();
}

// =====================================================================
// Section 7h: Flash Browser Host Identity
//
// Flash ActiveX checks the current process executable name before enabling
// its browser navigation path. The same unmodified SWF fails under
// FlashIE.exe but navigates normally when the host is named iexplore.exe;
// in the failing case, navigateToURL returns before calling IBindHost or
// URLMon. Report the IE host basename only for calls originating in the
// local Flash.ocx and only when it queries the current process
// (hModule == nullptr). Explicit module queries and all non-Flash callers
// continue to see the real executable path.
// =====================================================================

bool IsFlashCaller(void* returnAddress)
{
    if (!returnAddress || !g_loader.ocxModule)
        return false;

    MEMORY_BASIC_INFORMATION mbi = {};
    return VirtualQuery(returnAddress, &mbi, sizeof(mbi)) == sizeof(mbi) &&
           mbi.AllocationBase == g_loader.ocxModule;
}

DWORD WINAPI Hooked_GetModuleFileNameW(
    HMODULE hModule, LPWSTR lpFilename, DWORD nSize)
{
    // Flash enables its browser navigation path only for recognized hosts.
    bool spoofHost = hModule == nullptr && IsFlashCaller(_ReturnAddress());
    DWORD length = g_loader.api.getModuleFileNameW(
        hModule, lpFilename, nSize);
    DWORD originalError = GetLastError();
    if (!spoofHost || !lpFilename || length == 0 || length >= nSize)
        return length;

    LPWSTR fileName = PathFindFileNameW(lpFilename);
    static const wchar_t IE_HOST_NAME[] = L"iexplore.exe";
    size_t prefixLength = static_cast<size_t>(fileName - lpFilename);
    size_t hostLength = _countof(IE_HOST_NAME) - 1;
    size_t resultLength = prefixLength + hostLength;
    if (resultLength >= nSize) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return nSize;
    }

    memcpy(fileName, IE_HOST_NAME, sizeof(IE_HOST_NAME));
    SetLastError(originalError);
    return static_cast<DWORD>(resultLength);
}

DWORD WINAPI Hooked_GetModuleFileNameA(
    HMODULE hModule, LPSTR lpFilename, DWORD nSize)
{
    // Keep the real directory so any path-based lookups remain process-local.
    bool spoofHost = hModule == nullptr && IsFlashCaller(_ReturnAddress());
    DWORD length = g_loader.api.getModuleFileNameA(
        hModule, lpFilename, nSize);
    DWORD originalError = GetLastError();
    if (!spoofHost || !lpFilename || length == 0 || length >= nSize)
        return length;

    LPSTR fileName = PathFindFileNameA(lpFilename);
    static const char IE_HOST_NAME[] = "iexplore.exe";
    size_t prefixLength = static_cast<size_t>(fileName - lpFilename);
    size_t hostLength = sizeof(IE_HOST_NAME) - 1;
    size_t resultLength = prefixLength + hostLength;
    if (resultLength >= nSize) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return nSize;
    }

    memcpy(fileName, IE_HOST_NAME, sizeof(IE_HOST_NAME));
    SetLastError(originalError);
    return static_cast<DWORD>(resultLength);
}

// =====================================================================
// Section 7i: SharedObject First-Save Compatibility
//
// The bundled ActiveX player treats a missing old .sol as a failed replace
// operation. For that one narrowly scoped case, report the deletion as
// successful so Flash can continue its own .sxx -> .sol commit.
// =====================================================================

bool HasSolExtension(LPCWSTR path)
{
    if (!path)
        return false;
    size_t length = wcslen(path);
    return length >= 4 && _wcsicmp(path + length - 4, L".sol") == 0;
}

bool HasSolExtension(LPCSTR path)
{
    if (!path)
        return false;
    size_t length = strlen(path);
    return length >= 4 && _stricmp(path + length - 4, ".sol") == 0;
}

BOOL ReturnMissingSolDeleteAsSuccess(
    BOOL result, DWORD error, bool isFlashSol)
{
    if (!result && error == ERROR_FILE_NOT_FOUND && isFlashSol) {
        DbgTrace(L"[FlashIE] Treating missing first-save SOL as deleted\n");
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }

    SetLastError(error);
    return result;
}

BOOL WINAPI Hooked_DeleteFileW(LPCWSTR lpFileName)
{
    bool isFlashSol = IsFlashCaller(_ReturnAddress()) && HasSolExtension(lpFileName);
    BOOL result = g_loader.api.deleteFileW(lpFileName);
    DWORD error = GetLastError();
    return ReturnMissingSolDeleteAsSuccess(result, error, isFlashSol);
}

BOOL WINAPI Hooked_DeleteFileA(LPCSTR lpFileName)
{
    bool isFlashSol = IsFlashCaller(_ReturnAddress()) && HasSolExtension(lpFileName);
    BOOL result = g_loader.api.deleteFileA(lpFileName);
    DWORD error = GetLastError();
    return ReturnMissingSolDeleteAsSuccess(result, error, isFlashSol);
}

// =====================================================================
// Section 8: Public Namespace API & Lifecycle
// =====================================================================

class ExclusiveSrwLockGuard {
public:
    explicit ExclusiveSrwLockGuard(SRWLOCK& lock) : m_lock(&lock)
    {
        AcquireSRWLockExclusive(m_lock);
    }

    ~ExclusiveSrwLockGuard()
    {
        ReleaseSRWLockExclusive(m_lock);
    }

    ExclusiveSrwLockGuard(const ExclusiveSrwLockGuard&) = delete;
    ExclusiveSrwLockGuard& operator=(const ExclusiveSrwLockGuard&) = delete;

private:
    SRWLOCK* m_lock;
};

bool RestoreScriptVtableHooks()
{
    bool restored = true;
    AcquireSRWLockExclusive(&g_loader.scriptHooks.lock);
    for (int i = 0; i < g_loader.scriptHooks.count; i++) {
        ScriptVtableHook& hook = g_loader.scriptHooks.hooks[i];
        if (!hook.vtable || !hook.original) {
            hook = {};
            continue;
        }
        if (hook.vtable[5] != reinterpret_cast<void*>(Hooked_ParseScriptText)) {
            hook = {};
            continue;
        }

        DWORD oldProtect;
        if (!VirtualProtect(&hook.vtable[5], sizeof(void*),
                            PAGE_EXECUTE_READWRITE, &oldProtect)) {
            restored = false;
            continue;
        }
        hook.vtable[5] = reinterpret_cast<void*>(hook.original);
        if (!VirtualProtect(&hook.vtable[5], sizeof(void*), oldProtect,
                            &oldProtect)) {
            restored = false;
            continue;
        }
        hook = {};
    }
    if (restored)
        g_loader.scriptHooks.count = 0;
    ReleaseSRWLockExclusive(&g_loader.scriptHooks.lock);
    return restored;
}

} // namespace

namespace FlashLoader {

bool Activate()
{
    ExclusiveSrwLockGuard lifecycleGuard(g_loader.lifecycleLock);
    DWORD currentThreadId = GetCurrentThreadId();

    if (g_loader.phase != LoaderPhase::Inactive) {
        if ((g_loader.phase == LoaderPhase::Activated ||
             g_loader.phase == LoaderPhase::HooksInstalled) &&
            g_loader.ownerThreadId == currentThreadId) {
            return true;
        }
        DbgTrace(L"[FlashIE] Activate rejected for the current lifecycle/thread\n");
        return false;
    }

    WCHAR exePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH))
        return false;

    WCHAR exeDir[MAX_PATH] = {};
    wcscpy_s(exeDir, exePath);
    if (!PathRemoveFileSpecW(exeDir))
        return false;

    WCHAR ocxPath[MAX_PATH] = {};
    if (!PathCombineW(ocxPath, exeDir, L"Flash.ocx"))
        return false;

    // Ensure Flash.ocx's dependencies resolve from the exe directory
    SetDllDirectoryW(exeDir);

    HMODULE ocxModule = LoadLibraryW(ocxPath);

    // Pin Flash.ocx in memory — prevent COM from unloading it when all
    // Flash objects are released.  Our hooks (factory pointers, etc.)
    // reference Flash.ocx code/data for the process lifetime.
    if (ocxModule) {
        HMODULE hPinned = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_PIN,
            ocxPath, &hPinned);
    }

    if (!ocxModule) {
        DbgTrace(L"[FlashIE] LoadLibrary(%s) FAILED, err=%u\n",
                 ocxPath, GetLastError());
        return false;
    }
    DbgTrace(L"[FlashIE] Flash.ocx loaded at %p\n", ocxModule);

    auto getClassObject = reinterpret_cast<FN_DllGetClassObject>(
        GetProcAddress(ocxModule, "DllGetClassObject"));
    if (!getClassObject) {
        FreeLibrary(ocxModule);
        return false;
    }

    IClassFactory* realFactory = nullptr;
    HRESULT hr = getClassObject(
        CLSID_ShockwaveFlash, IID_IClassFactory,
        reinterpret_cast<void**>(&realFactory));
    if (FAILED(hr) || !realFactory) {
        if (realFactory)
            realFactory->Release();
        FreeLibrary(ocxModule);
        return false;
    }

    // Create logging wrapper around the real factory
    LoggingClassFactory* loggingFactory =
        new (std::nothrow) LoggingClassFactory(realFactory);
    if (!loggingFactory) {
        realFactory->Release();
        FreeLibrary(ocxModule);
        return false;
    }

    // Register the LOGGING wrapper (not raw factory) via CoRegisterClassObject.
    // This ensures that even when COM routes through the registered class object
    // (e.g., cross-domain iframes), our wrapper is used and SetClientSite/
    // QuickActivate hooks are triggered for forced activation.
    DWORD cookie = 0;
    HRESULT hrReg = CoRegisterClassObject(
        CLSID_ShockwaveFlash, loggingFactory, CLSCTX_INPROC_SERVER,
        REGCLS_MULTIPLEUSE, &cookie);
    DbgTrace(L"[FlashIE] CoRegisterClassObject -> hr=0x%08X cookie=%u\n",
             hrReg, cookie);
    if (FAILED(hrReg)) {
        loggingFactory->Release();
        realFactory->Release();
        FreeLibrary(ocxModule);
        return false;
    }

    g_loader.ocxModule = ocxModule;
    wcscpy_s(g_loader.ocxPath, ocxPath);
    wcscpy_s(g_loader.exeName, PathFindFileNameW(exePath));
    g_loader.factory.real = realFactory;
    g_loader.factory.wrapper = loggingFactory;
    g_loader.factory.cookie = cookie;
    g_loader.ownerThreadId = currentThreadId;
    g_loader.phase = LoaderPhase::Activated;

    AcquireSRWLockExclusive(&g_loader.factory.lock);
    g_loader.factory.published = loggingFactory;
    ReleaseSRWLockExclusive(&g_loader.factory.lock);

    return true;
}

bool InstallHooks()
{
    ExclusiveSrwLockGuard lifecycleGuard(g_loader.lifecycleLock);
    DWORD currentThreadId = GetCurrentThreadId();

    if (g_loader.phase == LoaderPhase::HooksInstalled &&
        g_loader.ownerThreadId == currentThreadId) {
        return true;
    }
    if (g_loader.phase != LoaderPhase::Activated ||
        g_loader.ownerThreadId != currentThreadId) {
        DbgTrace(L"[FlashIE] InstallHooks requires an active loader on its owning STA\n");
        return false;
    }

    // --- Step 1: Force-load IE DLLs so we can hook them ---
    LoadLibraryW(L"mshtml.dll");
    LoadLibraryW(L"urlmon.dll");
    LoadLibraryW(L"ieframe.dll");

    // --- Step 2: Resolve module handles ---
    HMODULE hCombase    = GetModuleHandleW(L"combase.dll");
    HMODULE hOle32      = GetModuleHandleW(L"ole32.dll");
    HMODULE hKernelBase = GetModuleHandleW(L"kernelbase.dll");
    HMODULE hKernel32   = GetModuleHandleW(L"kernel32.dll");
    HMODULE hAdvapi32   = GetModuleHandleW(L"advapi32.dll");
    HMODULE hUrlmon     = GetModuleHandleW(L"urlmon.dll");
    HMODULE hOleAut32   = GetModuleHandleW(L"oleaut32.dll");
    HMODULE hWldp       = GetModuleHandleW(L"wldp.dll");
    if (!hWldp) hWldp   = LoadLibraryW(L"wldp.dll");

    // --- Step 3: Resolve every required target before modifying code ---
    g_loader.api.regOpenKeyExW = reinterpret_cast<FN_RegOpenKeyExW>(
        ResolveTarget(hKernelBase, "RegOpenKeyExW", hAdvapi32));
    g_loader.api.regQueryValueExW = reinterpret_cast<FN_RegQueryValueExW>(
        ResolveTarget(hKernelBase, "RegQueryValueExW", hAdvapi32));
    g_loader.api.regCloseKey = reinterpret_cast<FN_RegCloseKey>(
        ResolveTarget(hKernelBase, "RegCloseKey", hAdvapi32));
    g_loader.api.deleteFileA = reinterpret_cast<FN_DeleteFileA>(
        ResolveTarget(hKernel32, "DeleteFileA", hKernelBase));
    g_loader.api.deleteFileW = reinterpret_cast<FN_DeleteFileW>(
        ResolveTarget(hKernel32, "DeleteFileW", hKernelBase));
    g_loader.api.getModuleFileNameA = reinterpret_cast<FN_GetModuleFileNameA>(
        ResolveTarget(hKernelBase, "GetModuleFileNameA", hKernel32));
    g_loader.api.getModuleFileNameW = reinterpret_cast<FN_GetModuleFileNameW>(
        ResolveTarget(hKernelBase, "GetModuleFileNameW", hKernel32));
    g_loader.api.coGetClassObject = reinterpret_cast<FN_CoGetClassObject>(
        ResolveTarget(hCombase, "CoGetClassObject", hOle32));
    g_loader.api.coCreateInstance = reinterpret_cast<FN_CoCreateInstance>(
        ResolveTarget(hCombase, "CoCreateInstance", hOle32));
    g_loader.api.clsidFromProgID = reinterpret_cast<FN_CLSIDFromProgID>(
        ResolveTarget(hCombase, "CLSIDFromProgID", hOle32));
    g_loader.api.coGetClassObjectFromURL = reinterpret_cast<FN_CoGetClassObjectFromURL>(
        ResolveTarget(hUrlmon, "CoGetClassObjectFromURL"));
    g_loader.api.loadRegTypeLib = reinterpret_cast<FN_LoadRegTypeLib>(
        ResolveTarget(hOleAut32, "LoadRegTypeLib"));
    g_loader.api.loadTypeLibEx = reinterpret_cast<FN_LoadTypeLibEx>(
        ResolveTarget(hOleAut32, "LoadTypeLibEx"));

    if (!g_loader.api.regOpenKeyExW || !g_loader.api.regQueryValueExW ||
        !g_loader.api.regCloseKey || !g_loader.api.deleteFileA ||
        !g_loader.api.deleteFileW || !g_loader.api.getModuleFileNameA ||
        !g_loader.api.getModuleFileNameW || !g_loader.api.coGetClassObject ||
        !g_loader.api.coCreateInstance || !g_loader.api.clsidFromProgID ||
        !g_loader.api.coGetClassObjectFromURL ||
        !g_loader.api.loadRegTypeLib || !g_loader.api.loadTypeLibEx) {
        ResetApiHookPointers();
        return false;
    }

    // WLDP does not exist on older Windows versions. Install both hooks only
    // when the OS exports the complete API pair.
    if (hWldp) {
        g_loader.api.wldpIsClassInApprovedList =
            reinterpret_cast<FN_WldpIsClassInApprovedList>(
                ResolveTarget(hWldp, "WldpIsClassInApprovedList"));
        g_loader.api.wldpQueryDynamicCodeTrust =
            reinterpret_cast<FN_WldpQueryDynamicCodeTrust>(
                ResolveTarget(hWldp, "WldpQueryDynamicCodeTrust"));
        if (!g_loader.api.wldpIsClassInApprovedList ||
            !g_loader.api.wldpQueryDynamicCodeTrust) {
            g_loader.api.wldpIsClassInApprovedList = nullptr;
            g_loader.api.wldpQueryDynamicCodeTrust = nullptr;
        }
    }

    // --- Step 4: Attach the complete set atomically ---
    LONG error = DetourTransactionBegin();
    const bool transactionStarted = error == NO_ERROR;
    if (error == NO_ERROR)
        error = DetourUpdateThread(GetCurrentThread());

    #define ATTACH(orig, func) \
        if (error == NO_ERROR && orig) \
            error = DetourAttach(reinterpret_cast<void**>(&orig), reinterpret_cast<void*>(&func));

    ATTACH(g_loader.api.regOpenKeyExW,             Hooked_RegOpenKeyExW);
    ATTACH(g_loader.api.regQueryValueExW,          Hooked_RegQueryValueExW);
    ATTACH(g_loader.api.regCloseKey,               Hooked_RegCloseKey);
    ATTACH(g_loader.api.deleteFileA,               Hooked_DeleteFileA);
    ATTACH(g_loader.api.deleteFileW,               Hooked_DeleteFileW);
    ATTACH(g_loader.api.getModuleFileNameA,        Hooked_GetModuleFileNameA);
    ATTACH(g_loader.api.getModuleFileNameW,        Hooked_GetModuleFileNameW);
    ATTACH(g_loader.api.coGetClassObject,          Hooked_CoGetClassObject);
    ATTACH(g_loader.api.coCreateInstance,          Hooked_CoCreateInstance);
    ATTACH(g_loader.api.clsidFromProgID,           Hooked_CLSIDFromProgID);
    ATTACH(g_loader.api.coGetClassObjectFromURL,   Hooked_CoGetClassObjectFromURL);
    ATTACH(g_loader.api.wldpIsClassInApprovedList, Hooked_WldpIsClassInApprovedList);
    ATTACH(g_loader.api.wldpQueryDynamicCodeTrust, Hooked_WldpQueryDynamicCodeTrust);
    ATTACH(g_loader.api.loadRegTypeLib,            Hooked_LoadRegTypeLib);

    #undef ATTACH

    if (error != NO_ERROR) {
        if (transactionStarted)
            DetourTransactionAbort();
        ResetApiHookPointers();
        return false;
    }

    error = DetourTransactionCommit();
    if (error != NO_ERROR) {
        ResetApiHookPointers();
        return false;
    }

    g_loader.api.installed = true;
    g_loader.phase = LoaderPhase::HooksInstalled;
    return true;
}

void Deactivate()
{
    ExclusiveSrwLockGuard lifecycleGuard(g_loader.lifecycleLock);
    if (g_loader.phase == LoaderPhase::Inactive)
        return;

    if (g_loader.ownerThreadId != GetCurrentThreadId()) {
        DbgTrace(L"[FlashIE] Deactivate must run on the Flash activation STA\n");
        return;
    }

    g_loader.phase = LoaderPhase::CleanupPending;
    FlushPendingActivations();

    AcquireSRWLockExclusive(&g_loader.flashHooks.lock);

    bool restoredOle = RestoreFlashVtableSlot(
        g_loader.flashHooks.oleVtable,
        reinterpret_cast<void*>(Hooked_OleSetClientSite),
        reinterpret_cast<void*>(g_loader.flashHooks.setClientSite));
    bool restoredQuick = RestoreFlashVtableSlot(
        g_loader.flashHooks.quickVtable,
        reinterpret_cast<void*>(Hooked_QuickActivate),
        reinterpret_cast<void*>(g_loader.flashHooks.quickActivate));

    if (restoredOle) {
        g_loader.flashHooks.oleVtable = nullptr;
        g_loader.flashHooks.setClientSite = nullptr;
    }
    if (restoredQuick) {
        g_loader.flashHooks.quickVtable = nullptr;
        g_loader.flashHooks.quickActivate = nullptr;
    }
    if (!restoredOle || !restoredQuick) {
        DbgTrace(L"[FlashIE] Failed to restore a Flash activation vtable\n");
        ReleaseSRWLockExclusive(&g_loader.flashHooks.lock);
        return;
    }

    if (!RestoreScriptVtableHooks()) {
        DbgTrace(L"[FlashIE] Failed to restore a script engine vtable\n");
        ReleaseSRWLockExclusive(&g_loader.flashHooks.lock);
        return;
    }

    bool hasDetours = g_loader.api.installed ||
        g_loader.flashHooks.queryInterfaceHooked;
    if (hasDetours) {
        LONG error = DetourTransactionBegin();
        const bool transactionStarted = error == NO_ERROR;
        if (error == NO_ERROR)
            error = DetourUpdateThread(GetCurrentThread());

        #define DETACH(orig, func) \
            if (error == NO_ERROR && orig) \
                error = DetourDetach(reinterpret_cast<void**>(&orig), reinterpret_cast<void*>(&func));

        if (g_loader.api.installed) {
            DETACH(g_loader.api.regOpenKeyExW,              Hooked_RegOpenKeyExW);
            DETACH(g_loader.api.regQueryValueExW,           Hooked_RegQueryValueExW);
            DETACH(g_loader.api.regCloseKey,                Hooked_RegCloseKey);
            DETACH(g_loader.api.deleteFileA,                Hooked_DeleteFileA);
            DETACH(g_loader.api.deleteFileW,                Hooked_DeleteFileW);
            DETACH(g_loader.api.getModuleFileNameA,         Hooked_GetModuleFileNameA);
            DETACH(g_loader.api.getModuleFileNameW,         Hooked_GetModuleFileNameW);
            DETACH(g_loader.api.coGetClassObject,           Hooked_CoGetClassObject);
            DETACH(g_loader.api.coCreateInstance,           Hooked_CoCreateInstance);
            DETACH(g_loader.api.clsidFromProgID,            Hooked_CLSIDFromProgID);
            DETACH(g_loader.api.coGetClassObjectFromURL,    Hooked_CoGetClassObjectFromURL);
            DETACH(g_loader.api.wldpIsClassInApprovedList,  Hooked_WldpIsClassInApprovedList);
            DETACH(g_loader.api.wldpQueryDynamicCodeTrust,  Hooked_WldpQueryDynamicCodeTrust);
            DETACH(g_loader.api.loadRegTypeLib,             Hooked_LoadRegTypeLib);
        }

        #undef DETACH

        // Detach Flash QI hook separately (installed via MaybeHookFlashQI)
        if (error == NO_ERROR && g_loader.flashHooks.queryInterfaceHooked &&
            g_loader.flashHooks.queryInterface) {
            error = DetourDetach(
                reinterpret_cast<void**>(&g_loader.flashHooks.queryInterface),
                reinterpret_cast<void*>(Hooked_FlashQI));
        }

        if (error != NO_ERROR) {
            if (transactionStarted)
                DetourTransactionAbort();
            DbgTrace(L"[FlashIE] Detour detach transaction failed: %d\n", error);
            ReleaseSRWLockExclusive(&g_loader.flashHooks.lock);
            return;
        }

        error = DetourTransactionCommit();
        if (error != NO_ERROR) {
            DbgTrace(L"[FlashIE] Detour detach commit failed: %d\n", error);
            ReleaseSRWLockExclusive(&g_loader.flashHooks.lock);
            return;
        }

    }

    g_loader.api.installed = false;
    g_loader.flashHooks.queryInterface = nullptr;
    g_loader.flashHooks.queryInterfaceHooked = false;

    // No new fake handles can be created after the registry hook is detached.
    // Keep the original RegCloseKey pointer until all tracked handles are closed.
    CloseAllTrackedKeys();
    ResetApiHookPointers();

    AcquireSRWLockExclusive(&g_loader.factory.lock);
    g_loader.factory.published = nullptr;
    ReleaseSRWLockExclusive(&g_loader.factory.lock);

    if (g_loader.factory.cookie) {
        HRESULT hr = CoRevokeClassObject(g_loader.factory.cookie);
        if (FAILED(hr)) {
            DbgTrace(L"[FlashIE] CoRevokeClassObject failed: 0x%08X\n", hr);
            ReleaseSRWLockExclusive(&g_loader.flashHooks.lock);
            return;
        }
        g_loader.factory.cookie = 0;
    }

    if (g_loader.factory.wrapper) {
        g_loader.factory.wrapper->Release();
        g_loader.factory.wrapper = nullptr;
    }
    if (g_loader.factory.real) {
        g_loader.factory.real->Release();
        g_loader.factory.real = nullptr;
    }
    if (g_loader.ocxModule) {
        FreeLibrary(g_loader.ocxModule);
        g_loader.ocxModule = nullptr;
    }

    g_loader.ocxPath[0] = L'\0';
    g_loader.exeName[0] = L'\0';
    g_loader.ownerThreadId = 0;
    g_loader.phase = LoaderPhase::Inactive;
    ReleaseSRWLockExclusive(&g_loader.flashHooks.lock);
}

} // namespace FlashLoader
