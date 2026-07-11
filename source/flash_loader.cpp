// =====================================================================
// Section 1: Includes & Constants
// =====================================================================

#include "flash_loader.h"
#include "debug.h"
#include "flash.h"      // MIDL-generated Flash COM interface definitions

#include <detours.h>
#include <jscriptcc/CCPreprocessor.h>

#include <string>
#include <string.h>
#include <stdint.h>
#include <shlwapi.h>
#include <ocidl.h>      // IQuickActivate, IPersistPropertyBag
#include <oleidl.h>     // IOleObject, IOleInPlaceObject, etc.
#include <objsafe.h>    // IObjectSafety

// Flash CLSID string form for comparisons
static const wchar_t FLASH_CLSID_STR[] = L"{D27CDB6E-AE6D-11CF-96B8-444553540000}";

// Static member
IClassFactory* FlashLoader::s_pFlashFactory = nullptr;

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

// IActiveScriptParse32 {BB1A2AE2-A4F9-11CF-8F20-00805F2CD064}
// NOTE: NOT BB1A2AE1 which is IID_IActiveScript (different interface!)
static const IID IID_IActiveScriptParse_ =
    {0xBB1A2AE2, 0xA4F9, 0x11CF, {0x8F, 0x20, 0x00, 0x80, 0x5F, 0x2C, 0xD0, 0x64}};

// IActiveScriptParse::ParseScriptText (vtable index 5)
typedef HRESULT (STDMETHODCALLTYPE *FN_ParseScriptText)(
    void* pThis, LPCOLESTR pstrCode, LPCOLESTR pstrItemName,
    IUnknown* punkContext, LPCOLESTR pstrDelimiter,
    DWORD_PTR dwSourceContextCookie, ULONG ulStartingLineNumber,
    DWORD dwFlags, VARIANT* pvarResult, EXCEPINFO* pexcepinfo);

// --- Flash object vtable ---

typedef HRESULT (STDMETHODCALLTYPE *FN_FlashQueryInterface)(
    void* pThis, REFIID riid, void** ppv);

// =====================================================================
// Section 3: Detours Hook Infrastructure
// =====================================================================

// Original function pointers for hooked APIs (set by DetourAttach)
static FN_CoGetClassObject            s_origCoGetClassObject = nullptr;
static FN_CoCreateInstance            s_origCoCreateInstance = nullptr;
static FN_RegOpenKeyExW               s_origRegOpenKeyExW = nullptr;
static FN_RegQueryValueExW            s_origRegQueryValueExW = nullptr;
static FN_RegCloseKey                 s_origRegCloseKey = nullptr;
static FN_CoGetClassObjectFromURL     s_origCoGetClassObjectFromURL = nullptr;
static FN_WldpIsClassInApprovedList   s_origWldpIsClassInApprovedList = nullptr;
static FN_WldpQueryDynamicCodeTrust   s_origWldpQueryDynamicCodeTrust = nullptr;
static FN_LoadRegTypeLib              s_origLoadRegTypeLib = nullptr;
static FN_CLSIDFromProgID             s_origCLSIDFromProgID = nullptr;
static FN_FlashQueryInterface         s_origFlashQI = nullptr;
static bool                           s_flashQIHooked = false;

// Attach a Detours hook: resolve the target function, begin a transaction,
// attach the hook, and commit. Returns true on success.
static bool DetoursAttach(void** ppOriginal, void* pDetour,
    HMODULE hPrimary, const char* funcName, HMODULE hFallback = nullptr)
{
    void* pTarget = nullptr;

    if (hPrimary)
        pTarget = reinterpret_cast<void*>(GetProcAddress(hPrimary, funcName));

    if (!pTarget && hFallback)
        pTarget = reinterpret_cast<void*>(GetProcAddress(hFallback, funcName));

    if (!pTarget) return false;

    *ppOriginal = pTarget;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(ppOriginal, pDetour);
    LONG error = DetourTransactionCommit();
    return error == NO_ERROR;
}

// =====================================================================
// Section 4: Fake Registry Key System
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
    FK_FLASHPLAYER_VER,  // HKLM\SOFTWARE\Macromedia\FlashPlayer[ActiveX]
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
    if (s_origRegOpenKeyExW) {
        s_origRegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ, &hReal);
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
            if (s_origRegCloseKey) s_origRegCloseKey(hKey);
            s_fakeKeys[i] = s_fakeKeys[s_fakeKeyCount - 1];
            s_fakeKeyCount--;
            return;
        }
    }
}

// =====================================================================
// Section 5: COM Wrapper Classes
// =====================================================================

// Forward declaration: installs IObjectSafety hook on Flash's QueryInterface
static void MaybeHookFlashQI(IUnknown* pObj);
// Forward declaration: installs SetClientSite hook for forced activation
static void MaybeHookFlashSetClientSite(IUnknown* pObj);
// Forward declaration: installs QuickActivate hook for iframe forced activation
static void MaybeHookFlashQuickActivate(IUnknown* pObj);
// Forward declaration: hooks IActiveScriptParse::ParseScriptText for JScriptCC preprocessing
static void MaybeHookScriptParseText(IUnknown* pObj);

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
// Section 6: Shared Static State
// =====================================================================

// Logging wrapper around the real Flash class factory
static LoggingClassFactory* s_pLoggingFactory = nullptr;

// HKEY tracking for FEATURE_BROWSER_EMULATION (fake without registry writes)
static HKEY s_hkeyBrowserEmulation = nullptr;
static wchar_t s_szExeName[MAX_PATH] = {};

// Path to Flash.ocx (next to exe)
static wchar_t g_szOcxPath[MAX_PATH] = {};
static HMODULE g_hOcxModule = nullptr;

// Script engine ParseScriptText hook state
static FN_ParseScriptText s_origParseScriptText = nullptr;
static void** s_hookedScriptVtable = nullptr;

// =====================================================================
// Section 7a: COM Hooks
// =====================================================================

// Return the active Flash class factory (logging wrapper if available).
static IClassFactory* GetFlashFactory()
{
    return s_pLoggingFactory ? static_cast<IClassFactory*>(s_pLoggingFactory)
                             : FlashLoader::s_pFlashFactory;
}

static bool IsFlashCLSID(REFCLSID rclsid)
{
    return FlashLoader::s_pFlashFactory && IsEqualCLSID(rclsid, CLSID_ShockwaveFlash);
}

HRESULT STDAPICALLTYPE FlashLoader::Hooked_CoGetClassObject(
    REFCLSID rclsid, DWORD dwClsContext, LPVOID pvReserved,
    REFIID riid, LPVOID* ppv)
{
    if (IsFlashCLSID(rclsid)) {
        HRESULT hr = GetFlashFactory()->QueryInterface(riid, ppv);
        DbgTrace(L"[FlashIE] CoGetClassObject(Flash) -> hr=0x%08X ppv=%p\n", hr, ppv ? *ppv : nullptr);
        return hr;
    }
    HRESULT hr = s_origCoGetClassObject(rclsid, dwClsContext, pvReserved, riid, ppv);
    DbgTrace(L"[FlashIE] CoGetClassObject({%08X-...}) -> hr=0x%08X\n", rclsid.Data1, hr);
    return hr;
}

HRESULT STDAPICALLTYPE FlashLoader::Hooked_CoCreateInstance(
    REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext,
    REFIID riid, LPVOID* ppv)
{
    if (IsFlashCLSID(rclsid)) {
        HRESULT hr = GetFlashFactory()->CreateInstance(pUnkOuter, riid, ppv);
        DbgTrace(L"[FlashIE] CoCreateInstance(Flash) -> hr=0x%08X\n", hr);
        return hr;
    }
    HRESULT hr = s_origCoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
    DbgTrace(L"[FlashIE] CoCreateInstance({%08X-...}) -> hr=0x%08X\n", rclsid.Data1, hr);
    // Hook script engine ParseScriptText to preprocess via JScriptCC
    if (SUCCEEDED(hr) && ppv && *ppv)
        MaybeHookScriptParseText(static_cast<IUnknown*>(*ppv));
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
    if (IsFlashCLSID(rclsid)) {
        HRESULT hr = GetFlashFactory()->QueryInterface(riid, ppv);
        DbgTrace(L"[FlashIE] CoGetClassObjectFromURL(Flash) -> hr=0x%08X\n", hr);
        return hr;
    }
    return s_origCoGetClassObjectFromURL(
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
    return s_origCLSIDFromProgID(lpszProgID, lpclsid);
}

// =====================================================================
// Section 7b: Registry Hooks
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

// Table-driven subkey mapping for opens under fake parent keys.
// parentOnly: if not FK_NONE, only matches when parent has this type.
struct FakeSubKeyEntry {
    const wchar_t* name;
    FakeKeyType type;
    FakeKeyType parentOnly;
};

static const FakeSubKeyEntry s_fakeSubKeys[] = {
    { L"CLSID",                    FK_PROGID_CLSID,   FK_PROGID_ROOT },
    { L"CurVer",                   FK_PROGID_CURVER,  FK_NONE },
    { L"InprocServer32",           FK_INPROC,         FK_NONE },
    { L"InProcServer32",           FK_INPROC,         FK_NONE },
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

static const ClsidSuffixEntry s_clsidSuffixes[] = {
    { L"InprocServer32",           FK_INPROC },
    { L"InProcServer32",           FK_INPROC },
    { L"MiscStatus\\1",            FK_MISCSTATUS1 },
    { L"MiscStatus",               FK_MISCSTATUS },
    { L"ProgID",                   FK_PROGID },
    { L"TypeLib",                  FK_TYPELIB },
    { L"Control",                  FK_CONTROL },
    { L"Version",                  FK_VERSION },
    { L"VersionIndependentProgID", FK_VERSION },
};

// Helper: allocate fake key, set output, log, and return ERROR_SUCCESS.
static LSTATUS ReturnFakeKey(FakeKeyType type, PHKEY phkResult,
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

// Check if lpSubKey contains a ShockwaveFlash ProgID with valid version (<=34).
static bool IsFlashProgIDPath(LPCWSTR lpSubKey)
{
    const wchar_t* progid = wcsstr(lpSubKey, L"ShockwaveFlash.ShockwaveFlash");
    if (!progid) return false;
    const wchar_t* afterBase = progid + 29;
    if (*afterBase == L'.') {
        int ver = _wtoi(afterBase + 1);
        if (ver > 34) return false;
    }
    return (*afterBase == L'\0' || *afterBase == L'.' || *afterBase == L'\\');
}

// Case-insensitive check for Flash CLSID substring in a path.
static bool PathContainsFlashCLSID(LPCWSTR lpSubKey)
{
    return wcsstr(lpSubKey, L"D27CDB6E") || wcsstr(lpSubKey, L"d27cdb6e");
}

LSTATUS WINAPI FlashLoader::Hooked_RegOpenKeyExW(
    HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions,
    REGSAM samDesired, PHKEY phkResult)
{
    // ---- Handle subkey opens under fake Flash HKEY handles ----
    if (IsFakeHKey(hKey) && lpSubKey && phkResult) {
        FakeKeyType parentType = GetFakeKeyType(hKey);
        FakeKeyType subType = FK_NONE;

        for (const auto& e : s_fakeSubKeys) {
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
            if (wcsstr(lpSubKey, L"ActiveX Compatibility") ||
                wcsstr(lpSubKey, L"Extension Compatibility")) {
                DbgTrace(L"[FlashIE] RegOpenKeyExW BLOCKED (kill bit): %s\n", lpSubKey);
                return ERROR_FILE_NOT_FOUND;
            }
        }

        // ---- Fake Flash ProgID registration ----
        if (IsFlashProgIDPath(lpSubKey) && phkResult) {
            FakeKeyType type = (wcsstr(lpSubKey, L"\\CLSID") || SubKeyEndsWith(lpSubKey, L"CLSID"))
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
                for (const auto& e : s_clsidSuffixes) {
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
        if ((wcsstr(lpSubKey, L"x-shockwave-flash") ||
             wcsstr(lpSubKey, L"x-Shockwave-Flash")) &&
            wcsstr(lpSubKey, L"Content Type"))
            return ReturnFakeKey(FK_MIME, phkResult, lpSubKey, L"MIME mapping");

        // ---- Fake Flash Player version info ----
        // SWFObject and similar JS detection check Macromedia\FlashPlayer registry keys.
        if (wcsstr(lpSubKey, L"Macromedia\\FlashPlayer") && phkResult)
            return ReturnFakeKey(FK_FLASHPLAYER_VER, phkResult, lpSubKey, L"FlashPlayer version");

        // ---- FEATURE_BROWSER_EMULATION tracking ----
        if (wcsstr(lpSubKey, L"FEATURE_BROWSER_EMULATION")) {
            LSTATUS res = s_origRegOpenKeyExW(
                hKey, lpSubKey, ulOptions, samDesired, phkResult);
            if (res == ERROR_SUCCESS && phkResult)
                s_hkeyBrowserEmulation = *phkResult;
            return res;
        }
    }
    LSTATUS res = s_origRegOpenKeyExW(
        hKey, lpSubKey, ulOptions, samDesired, phkResult);
    DbgTrace(L"[FlashIE] RegOpenKeyExW: %s -> 0x%X\n", lpSubKey ? lpSubKey : L"(null)", res);
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

        case FK_FLASHPLAYER_VER:
            if (!lpValueName || lpValueName[0] == 0)
                return FakeRegSz(lpType, lpData, lpcbData, g_szOcxPath);
            if (_wcsicmp(lpValueName, L"CurrentVersion") == 0)
                return FakeRegSz(lpType, lpData, lpcbData, L"34.0");
            if (_wcsicmp(lpValueName, L"Version") == 0)
                return FakeRegSz(lpType, lpData, lpcbData, L"34.0.0.330");
            if (_wcsicmp(lpValueName, L"PlayerPath") == 0)
                return FakeRegSz(lpType, lpData, lpcbData, g_szOcxPath);
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

    return s_origRegQueryValueExW(
        hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
}

LSTATUS WINAPI FlashLoader::Hooked_RegCloseKey(HKEY hKey)
{
    if (IsFakeHKey(hKey)) {
        CloseFakeKey(hKey);
        return ERROR_SUCCESS;
    }
    return s_origRegCloseKey(hKey);
}

// =====================================================================
// Section 7c: Security & Feature Hooks
// =====================================================================

static bool IsFlashCodeImage(HANDLE fileHandle, void* baseImage)
{
    if (baseImage && g_hOcxModule) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(baseImage, &mbi, sizeof(mbi)) == sizeof(mbi) &&
            mbi.AllocationBase == g_hOcxModule) {
            return true;
        }
    }

    if (fileHandle && fileHandle != INVALID_HANDLE_VALUE && g_szOcxPath[0]) {
        wchar_t path[MAX_PATH + 4] = {};
        DWORD length = GetFinalPathNameByHandleW(
            fileHandle, path, _countof(path), FILE_NAME_NORMALIZED);
        if (length > 0 && length < _countof(path)) {
            const wchar_t* normalized = path;
            if (wcsncmp(normalized, L"\\\\?\\", 4) == 0)
                normalized += 4;
            if (_wcsicmp(normalized, g_szOcxPath) == 0)
                return true;
        }
    }

    return false;
}

// Windows 10+ MSHTML calls wldp!WldpIsClassInApprovedList to check if
// an ActiveX CLSID is approved for instantiation. Only the local Flash class
// is exempted; every other class keeps the system policy result.
HRESULT WINAPI FlashLoader::Hooked_WldpIsClassInApprovedList(
    const CLSID* classID, void* hostInfo, BOOL* isApproved, DWORD optionalFlags)
{
    if (classID && IsEqualCLSID(*classID, CLSID_ShockwaveFlash)) {
        if (isApproved) *isApproved = TRUE;
        DbgTrace(L"[FlashIE] WldpIsClassInApprovedList(Flash) -> approved\n");
        return S_OK;
    }

    return s_origWldpIsClassInApprovedList(
        classID, hostInfo, isApproved, optionalFlags);
}

HRESULT WINAPI FlashLoader::Hooked_WldpQueryDynamicCodeTrust(
    HANDLE fileHandle, void* baseImage, DWORD imageSize)
{
    if (IsFlashCodeImage(fileHandle, baseImage)) {
        DbgTrace(L"[FlashIE] WldpQueryDynamicCodeTrust(Flash.ocx) -> trusted\n");
        return S_OK;
    }

    return s_origWldpQueryDynamicCodeTrust(fileHandle, baseImage, imageSize);
}

// =====================================================================
// Section 7d: Flash QI Hook
//
// Intercepts IObjectSafety queries on Flash objects. Injecting this
// interface is required by MSHTML for scripting access. Property-bag
// values, including allowScriptAccess, remain controlled by the page.
// =====================================================================

static HRESULT STDMETHODCALLTYPE Hooked_FlashQI(void* pThis, REFIID riid, void** ppv)
{
    if (IsEqualIID(riid, IID_IObjectSafety)) {
        IUnknown* pFlashUnk = nullptr;
        s_origFlashQI(pThis, IID_IUnknown, reinterpret_cast<void**>(&pFlashUnk));
        if (pFlashUnk) {
            *ppv = static_cast<IObjectSafety*>(new FlashSafetyTearoff(pFlashUnk));
            DbgTrace(L"[FlashIE] Flash::QI(IObjectSafety) -> HOOKED tearoff\n");
            return S_OK;
        }
    }

    return s_origFlashQI(pThis, riid, ppv);
}

static void MaybeHookFlashQI(IUnknown* pObj)
{
    if (s_flashQIHooked) return;
    void** vtable = *reinterpret_cast<void***>(pObj);
    void* pFlashQI = vtable[0];

    s_origFlashQI = reinterpret_cast<FN_FlashQueryInterface>(pFlashQI);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<void**>(&s_origFlashQI),
                 reinterpret_cast<void*>(Hooked_FlashQI));
    LONG error = DetourTransactionCommit();
    if (error == NO_ERROR) {
        s_flashQIHooked = true;
        DbgTrace(L"[FlashIE] Hook Flash::QueryInterface: OK (addr=%p)\n", pFlashQI);
    } else {
        DbgTrace(L"[FlashIE] Hook Flash::QueryInterface: FAILED (error=%d)\n", error);
    }
}

// =====================================================================
// Section 7e: Flash Forced In-Place Activation
//
// MSHTML creates Flash objects but defers DoVerb(INPLACEACTIVATE)
// until a user click (Windows 10 Flash phase-out behavior).
// We hook Flash's IOleObject::SetClientSite; once MSHTML sets the site,
// we schedule a timer to call DoVerb(OLEIVERB_INPLACEACTIVATE) forcing
// the control to activate without user interaction.
// =====================================================================

typedef HRESULT (STDMETHODCALLTYPE *FN_OleSetClientSite)(
    IOleObject* pThis, IOleClientSite* pClientSite);
static FN_OleSetClientSite s_origSetClientSite = nullptr;

struct PendingActivation {
    IOleObject* pObj;
    IOleClientSite* pSite;
};

static const int MAX_PENDING = 16;
static PendingActivation s_pending[MAX_PENDING] = {};
static int s_pendingCount = 0;
static UINT_PTR s_activateTimer = 0;

// Saved references for deferred re-activation (handles display:none iframes).
// A 200ms repeating timer calls DoVerb until the iframe becomes visible,
// up to 50 retries (10 seconds total).
static const int MAX_DEFERRED = 16;
static PendingActivation s_deferred[MAX_DEFERRED] = {};
static int s_deferredCount = 0;
static UINT_PTR s_deferredTimer = 0;
static int s_deferredRetries = 0;
static const int MAX_DEFERRED_RETRIES = 50; // 50 × 200ms = 10s

static void CALLBACK DeferredActivateTimerProc(HWND, UINT, UINT_PTR idTimer, DWORD)
{
    s_deferredRetries++;

    for (int i = 0; i < s_deferredCount; i++) {
        IOleObject* pObj = s_deferred[i].pObj;
        IOleClientSite* pSite = s_deferred[i].pSite;
        if (pObj && pSite) {
            pObj->DoVerb(OLEIVERB_INPLACEACTIVATE, nullptr, pSite, 0, nullptr, nullptr);
        }
    }

    // Stop after max retries — release references and kill timer
    if (s_deferredRetries >= MAX_DEFERRED_RETRIES) {
        KillTimer(nullptr, idTimer);
        s_deferredTimer = 0;
        for (int i = 0; i < s_deferredCount; i++) {
            if (s_deferred[i].pSite) s_deferred[i].pSite->Release();
            if (s_deferred[i].pObj)  s_deferred[i].pObj->Release();
        }
        s_deferredCount = 0;
        DbgTrace(L"[FlashIE] DeferredActivate stopped after %d retries\n", s_deferredRetries);
    }
}

static void CALLBACK ForceActivateTimerProc(HWND, UINT, UINT_PTR idTimer, DWORD)
{
    KillTimer(nullptr, idTimer);
    s_activateTimer = 0;

    for (int i = 0; i < s_pendingCount; i++) {
        IOleObject* pObj = s_pending[i].pObj;
        IOleClientSite* pSite = s_pending[i].pSite;
        if (pObj && pSite) {
            HRESULT hr = pObj->DoVerb(OLEIVERB_INPLACEACTIVATE, nullptr, pSite, 0, nullptr, nullptr);
            DbgTrace(L"[FlashIE] ForceActivate DoVerb -> hr=0x%08X\n", hr);

            // Save for deferred re-activation: Flash in display:none iframes
            // may need re-activation when the iframe becomes visible.
            if (s_deferredCount < MAX_DEFERRED) {
                pObj->AddRef();
                pSite->AddRef();
                s_deferred[s_deferredCount].pObj = pObj;
                s_deferred[s_deferredCount].pSite = pSite;
                s_deferredCount++;
            }

            pSite->Release();
            pObj->Release();
        }
    }
    s_pendingCount = 0;

    // Schedule repeating re-activation every 200ms for display:none iframes
    if (s_deferredCount > 0 && !s_deferredTimer) {
        s_deferredRetries = 0;
        s_deferredTimer = SetTimer(nullptr, 0, 200, DeferredActivateTimerProc);
    }
}

// Release any pending activation references (called during shutdown).
static void FlushPendingActivations()
{
    if (s_activateTimer) {
        KillTimer(nullptr, s_activateTimer);
        s_activateTimer = 0;
    }
    if (s_deferredTimer) {
        KillTimer(nullptr, s_deferredTimer);
        s_deferredTimer = 0;
    }
    for (int i = 0; i < s_pendingCount; i++) {
        if (s_pending[i].pSite) s_pending[i].pSite->Release();
        if (s_pending[i].pObj)  s_pending[i].pObj->Release();
    }
    s_pendingCount = 0;
    for (int i = 0; i < s_deferredCount; i++) {
        if (s_deferred[i].pSite) s_deferred[i].pSite->Release();
        if (s_deferred[i].pObj)  s_deferred[i].pObj->Release();
    }
    s_deferredCount = 0;
}

static HRESULT STDMETHODCALLTYPE Hooked_OleSetClientSite(
    IOleObject* pThis, IOleClientSite* pClientSite)
{
    HRESULT hr = s_origSetClientSite(pThis, pClientSite);

    // When MSHTML sets a non-null client site, queue forced activation
    if (SUCCEEDED(hr) && pClientSite && s_pendingCount < MAX_PENDING) {
        pThis->AddRef();
        pClientSite->AddRef();
        s_pending[s_pendingCount].pObj = pThis;
        s_pending[s_pendingCount].pSite = pClientSite;
        s_pendingCount++;
        // Coalesce: restart the timer so one callback handles all pending objects
        if (s_activateTimer) KillTimer(nullptr, s_activateTimer);
        s_activateTimer = SetTimer(nullptr, 0, 100, ForceActivateTimerProc);
        DbgTrace(L"[FlashIE] SetClientSite -> queued forced activation\n");
    }

    return hr;
}

static void MaybeHookFlashSetClientSite(IUnknown* pObj)
{
    if (s_origSetClientSite) return; // already hooked

    IOleObject* pOle = nullptr;
    pObj->QueryInterface(IID_IOleObject, reinterpret_cast<void**>(&pOle));
    if (!pOle) return;

    void** vtable = *reinterpret_cast<void***>(pOle);
    // IOleObject::SetClientSite is vtable slot 3 (after QI, AddRef, Release)
    s_origSetClientSite = reinterpret_cast<FN_OleSetClientSite>(vtable[3]);

    DWORD oldProtect;
    if (VirtualProtect(&vtable[3], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        vtable[3] = reinterpret_cast<void*>(Hooked_OleSetClientSite);
        VirtualProtect(&vtable[3], sizeof(void*), oldProtect, &oldProtect);
        DbgTrace(L"[FlashIE] Hook Flash IOleObject::SetClientSite: OK\n");
    }

    pOle->Release();
}

// Also hook IQuickActivate::QuickActivate — MSHTML in cross-domain
// iframes may use IQuickActivate instead of IOleObject::SetClientSite.
// QuickActivate sets the client site internally, bypassing our
// SetClientSite vtable hook.

typedef HRESULT (STDMETHODCALLTYPE *FN_QuickActivate)(
    IQuickActivate* pThis, QACONTAINER* pQAContainer, QACONTROL* pQAControl);
static FN_QuickActivate s_origQuickActivate = nullptr;

static HRESULT STDMETHODCALLTYPE Hooked_QuickActivate(
    IQuickActivate* pThis, QACONTAINER* pQAContainer, QACONTROL* pQAControl)
{
    HRESULT hr = s_origQuickActivate(pThis, pQAContainer, pQAControl);

    if (SUCCEEDED(hr) && pQAContainer && pQAContainer->pClientSite &&
        s_pendingCount < MAX_PENDING) {
        // Get IOleObject from the Flash control to call DoVerb later
        IOleObject* pOle = nullptr;
        pThis->QueryInterface(IID_IOleObject, reinterpret_cast<void**>(&pOle));
        if (pOle) {
            IOleClientSite* pSite = pQAContainer->pClientSite;
            pOle->AddRef();
            pSite->AddRef();
            s_pending[s_pendingCount].pObj = pOle;
            s_pending[s_pendingCount].pSite = pSite;
            s_pendingCount++;
            if (s_activateTimer) KillTimer(nullptr, s_activateTimer);
            s_activateTimer = SetTimer(nullptr, 0, 100, ForceActivateTimerProc);
            DbgTrace(L"[FlashIE] QuickActivate -> queued forced activation\n");
            pOle->Release(); // balance the QI AddRef (pending still holds a ref from AddRef above)
        }
    }

    return hr;
}

static void MaybeHookFlashQuickActivate(IUnknown* pObj)
{
    if (s_origQuickActivate) return; // already hooked

    IQuickActivate* pQA = nullptr;
    pObj->QueryInterface(IID_IQuickActivate, reinterpret_cast<void**>(&pQA));
    if (!pQA) return;

    void** vtable = *reinterpret_cast<void***>(pQA);
    // IQuickActivate::QuickActivate is vtable slot 3 (after QI, AddRef, Release)
    s_origQuickActivate = reinterpret_cast<FN_QuickActivate>(vtable[3]);

    DWORD oldProtect;
    if (VirtualProtect(&vtable[3], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        vtable[3] = reinterpret_cast<void*>(Hooked_QuickActivate);
        VirtualProtect(&vtable[3], sizeof(void*), oldProtect, &oldProtect);
        DbgTrace(L"[FlashIE] Hook Flash IQuickActivate::QuickActivate: OK\n");
    }

    pQA->Release();
}

// =====================================================================
// Section 7f: TypeLib Hook
//
// Flash.ocx's TypeLib GUID is {D27CDB6B-AE6D-11CF-96B8-444553540000}.
// Without registry entries, OLEAUT32 can't find it and returns
// TYPE_E_LIBNOTREGISTERED. We load from the OCX file directly.
// =====================================================================

static HRESULT WINAPI Hooked_LoadRegTypeLib(
    REFGUID rguid, WORD wVerMajor, WORD wVerMinor, LCID lcid, ITypeLib** pptlib)
{
    if (IsEqualGUID(rguid, LIBID_ShockwaveFlashObjects) && pptlib) {
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
    HRESULT hr = s_origLoadRegTypeLib(rguid, wVerMajor, wVerMinor, lcid, pptlib);
    if (FAILED(hr)) {
        DbgTrace(L"[FlashIE] LoadRegTypeLib({%08X-...}) v%u.%u -> 0x%08X\n",
                 rguid.Data1, wVerMajor, wVerMinor, hr);
    }
    return hr;
}

// =====================================================================
// Section 7g: Script Engine ParseScriptText Hook
//
// Hooks IActiveScriptParse::ParseScriptText on JScript/VBScript engines.
// Script code is preprocessed through JScriptCC's Conditional Compilation
// engine (@cc_on / @if / @set / @end) before execution. If preprocessing
// succeeds, the expanded code is passed to the original engine; otherwise
// the original code is executed unmodified. All errors are reported via
// DbgTrace (OutputDebugStringW).
// =====================================================================

static HRESULT STDMETHODCALLTYPE Hooked_ParseScriptText(
    void* pThis, LPCOLESTR pstrCode, LPCOLESTR pstrItemName,
    IUnknown* punkContext, LPCOLESTR pstrDelimiter,
    DWORD_PTR dwSourceContextCookie, ULONG ulStartingLineNumber,
    DWORD dwFlags, VARIANT* pvarResult, EXCEPINFO* pexcepinfo)
{
    if (pstrCode) {
        // Convert UTF-16 source to UTF-8 for JScriptCC
        int codeLen = static_cast<int>(wcslen(pstrCode));
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, pstrCode, codeLen, nullptr, 0, nullptr, nullptr);
        if (utf8Len > 0) {
            std::string utf8Source(utf8Len, '\0');
            WideCharToMultiByte(CP_UTF8, 0, pstrCode, codeLen, &utf8Source[0], utf8Len, nullptr, nullptr);

            // Run JScriptCC conditional compilation preprocessor
            std::string preprocessed;
            jscriptcc::CCErrorList errors;
            jscriptcc::CCPreprocessor preprocessor;

            const auto architecture = sizeof(void*) == 8
                ? jscriptcc::TargetArchitecture::Win64
                : jscriptcc::TargetArchitecture::Win32;

            bool ok = preprocessor.process(
                utf8Source, preprocessed, jscriptcc::CCEnvironment(architecture), &errors);

            // Report any preprocessing errors
            for (const auto& err : errors) {
                DbgTrace(L"[FlashIE] JScriptCC error: line %d col %d: %hs\n",
                         err.line, err.column, err.message.c_str());
            }

            if (ok && !preprocessed.empty()) {
                // Convert preprocessed UTF-8 back to UTF-16
                int wideLen = MultiByteToWideChar(CP_UTF8, 0, preprocessed.c_str(), static_cast<int>(preprocessed.size()), nullptr, 0);
                if (wideLen > 0) {
                    std::wstring preprocessedWide(wideLen, L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, preprocessed.c_str(), static_cast<int>(preprocessed.size()), &preprocessedWide[0], wideLen);

                    DbgTrace(L"[FlashIE] === ParseScriptText (preprocessed) begin ===\n");
                    if (pstrItemName && pstrItemName[0])
                        DbgTrace(L"[FlashIE]   item: %s\n", pstrItemName);
                    DbgTrace(L"[FlashIE]   code: %s\n", preprocessedWide.c_str());
                    DbgTrace(L"[FlashIE] === ParseScriptText (preprocessed) end ===\n");

                    return s_origParseScriptText(pThis, preprocessedWide.c_str(), pstrItemName,
                        punkContext, pstrDelimiter, dwSourceContextCookie, ulStartingLineNumber,
                        dwFlags, pvarResult, pexcepinfo);
                }
            }

            // Preprocessing failed or produced empty output — fall through to original code
            if (!ok) {
                DbgTrace(L"[FlashIE] JScriptCC preprocessing failed, using original code\n");
            }
        }
    }

    return s_origParseScriptText(pThis, pstrCode, pstrItemName, punkContext,
        pstrDelimiter, dwSourceContextCookie, ulStartingLineNumber,
        dwFlags, pvarResult, pexcepinfo);
}

static void MaybeHookScriptParseText(IUnknown* pObj)
{
    if (s_origParseScriptText) return; // already hooked

    void* pParse = nullptr;
    if (FAILED(pObj->QueryInterface(IID_IActiveScriptParse_, &pParse)) || !pParse)
        return;

    void** vtable = *reinterpret_cast<void***>(pParse);
    // IActiveScriptParse::ParseScriptText is vtable slot 5
    // (after QI=0, AddRef=1, Release=2, InitNew=3, AddScriptlet=4)
    void* pTarget = vtable[5];

    DWORD oldProtect;
    if (VirtualProtect(&vtable[5], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        s_origParseScriptText = reinterpret_cast<FN_ParseScriptText>(pTarget);
        vtable[5] = reinterpret_cast<void*>(Hooked_ParseScriptText);
        VirtualProtect(&vtable[5], sizeof(void*), oldProtect, &oldProtect);
        s_hookedScriptVtable = vtable;
        DbgTrace(L"[FlashIE] Hook IActiveScriptParse::ParseScriptText: OK (addr=%p)\n",
                 pTarget);
    }

    static_cast<IUnknown*>(pParse)->Release();
}

// =====================================================================
// Section 8: Public API (Activate, InstallHooks, Deactivate)
// =====================================================================

bool FlashLoader::Activate()
{
    WCHAR szDir[MAX_PATH];
    GetModuleFileNameW(nullptr, szDir, MAX_PATH);
    PathRemoveFileSpecW(szDir);

    PathCombineW(g_szOcxPath, szDir, L"Flash.ocx");

    // Ensure Flash.ocx's dependencies resolve from the exe directory
    SetDllDirectoryW(szDir);

    m_hModule = LoadLibraryW(g_szOcxPath);
    g_hOcxModule = m_hModule;

    // Pin Flash.ocx in memory — prevent COM from unloading it when all
    // Flash objects are released.  Our hooks (factory pointers, etc.)
    // reference Flash.ocx code/data for the process lifetime.
    if (m_hModule) {
        HMODULE hPinned = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_PIN,
            g_szOcxPath, &hPinned);
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
        g_hOcxModule = nullptr;
        return false;
    }

    HRESULT hr = pfnGetClassObject(CLSID_ShockwaveFlash, IID_IClassFactory,
                                    reinterpret_cast<void**>(&m_pFactory));
    if (FAILED(hr) || !m_pFactory) {
        FreeLibrary(m_hModule);
        m_hModule = nullptr;
        g_hOcxModule = nullptr;
        return false;
    }

    // Publish for the hook functions
    s_pFlashFactory = m_pFactory;

    // Create logging wrapper around the real factory
    s_pLoggingFactory = new LoggingClassFactory(m_pFactory);

    // Register the LOGGING wrapper (not raw factory) via CoRegisterClassObject.
    // This ensures that even when COM routes through the registered class object
    // (e.g., cross-domain iframes), our wrapper is used and SetClientSite/
    // QuickActivate hooks are triggered for forced activation.
    HRESULT hrReg = CoRegisterClassObject(CLSID_ShockwaveFlash, s_pLoggingFactory,
                          CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE,
                          &m_dwCookie);
    DbgTrace(L"[FlashIE] CoRegisterClassObject -> hr=0x%08X cookie=%u\n", hrReg, m_dwCookie);

    // Cache exe name for FEATURE_BROWSER_EMULATION hook
    WCHAR szExe[MAX_PATH];
    GetModuleFileNameW(nullptr, szExe, MAX_PATH);
    wcscpy_s(s_szExeName, PathFindFileNameW(szExe));

    return true;
}

void FlashLoader::InstallHooks()
{
    if (m_hooked || !m_pFactory) return;

    // --- Step 1: Force-load IE DLLs so we can hook them ---
    LoadLibraryW(L"mshtml.dll");
    LoadLibraryW(L"urlmon.dll");
    LoadLibraryW(L"ieframe.dll");

    // --- Step 2: Resolve module handles ---
    HMODULE hCombase    = GetModuleHandleW(L"combase.dll");
    HMODULE hOle32      = GetModuleHandleW(L"ole32.dll");
    HMODULE hKernelBase = GetModuleHandleW(L"kernelbase.dll");
    HMODULE hAdvapi32   = GetModuleHandleW(L"advapi32.dll");
    HMODULE hUrlmon     = GetModuleHandleW(L"urlmon.dll");
    HMODULE hOleAut32   = GetModuleHandleW(L"oleaut32.dll");
    HMODULE hWldp       = GetModuleHandleW(L"wldp.dll");
    if (!hWldp) hWldp   = LoadLibraryW(L"wldp.dll");

    // --- Step 3: Install hooks via Detours ---
    // Order: registry hooks first (kill bit bypass), then COM hooks

    // Registry hooks
    DetoursAttach(reinterpret_cast<void**>(&s_origRegOpenKeyExW),
        reinterpret_cast<void*>(&Hooked_RegOpenKeyExW), hKernelBase, "RegOpenKeyExW", hAdvapi32);
    DetoursAttach(reinterpret_cast<void**>(&s_origRegQueryValueExW),
        reinterpret_cast<void*>(&Hooked_RegQueryValueExW), hKernelBase, "RegQueryValueExW", hAdvapi32);
    DetoursAttach(reinterpret_cast<void**>(&s_origRegCloseKey),
        reinterpret_cast<void*>(&Hooked_RegCloseKey), hKernelBase, "RegCloseKey", hAdvapi32);

    // COM hooks
    DetoursAttach(reinterpret_cast<void**>(&s_origCoGetClassObject),
        reinterpret_cast<void*>(&Hooked_CoGetClassObject), hCombase, "CoGetClassObject", hOle32);
    DetoursAttach(reinterpret_cast<void**>(&s_origCoCreateInstance),
        reinterpret_cast<void*>(&Hooked_CoCreateInstance), hCombase, "CoCreateInstance", hOle32);
    DetoursAttach(reinterpret_cast<void**>(&s_origCLSIDFromProgID),
        reinterpret_cast<void*>(&Hooked_CLSIDFromProgID), hCombase, "CLSIDFromProgID", hOle32);

    // URL moniker hooks
    DetoursAttach(reinterpret_cast<void**>(&s_origCoGetClassObjectFromURL),
        reinterpret_cast<void*>(&Hooked_CoGetClassObjectFromURL), hUrlmon, "CoGetClassObjectFromURL");
    // WLDP hooks (Windows 10+ ActiveX approval)
    DetoursAttach(reinterpret_cast<void**>(&s_origWldpIsClassInApprovedList),
        reinterpret_cast<void*>(&Hooked_WldpIsClassInApprovedList), hWldp, "WldpIsClassInApprovedList");
    DetoursAttach(reinterpret_cast<void**>(&s_origWldpQueryDynamicCodeTrust),
        reinterpret_cast<void*>(&Hooked_WldpQueryDynamicCodeTrust), hWldp, "WldpQueryDynamicCodeTrust");

    // TypeLib hook
    DetoursAttach(reinterpret_cast<void**>(&s_origLoadRegTypeLib),
        reinterpret_cast<void*>(&Hooked_LoadRegTypeLib), hOleAut32, "LoadRegTypeLib");

    m_hooked = true;
}

void FlashLoader::Deactivate()
{
    FlushPendingActivations();

    if (m_hooked) {
        // Detach all API-level Detours hooks in a single transaction
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        #define DETACH(orig, func) \
            if (orig) { DetourDetach(reinterpret_cast<void**>(&orig), reinterpret_cast<void*>(&func)); orig = nullptr; }

        DETACH(s_origRegOpenKeyExW,              Hooked_RegOpenKeyExW);
        DETACH(s_origRegQueryValueExW,           Hooked_RegQueryValueExW);
        DETACH(s_origRegCloseKey,                Hooked_RegCloseKey);
        DETACH(s_origCoGetClassObject,           Hooked_CoGetClassObject);
        DETACH(s_origCoCreateInstance,           Hooked_CoCreateInstance);
        DETACH(s_origCLSIDFromProgID,            Hooked_CLSIDFromProgID);
        DETACH(s_origCoGetClassObjectFromURL,    Hooked_CoGetClassObjectFromURL);
        DETACH(s_origWldpIsClassInApprovedList,  Hooked_WldpIsClassInApprovedList);
        DETACH(s_origWldpQueryDynamicCodeTrust,  Hooked_WldpQueryDynamicCodeTrust);
        DETACH(s_origLoadRegTypeLib,             Hooked_LoadRegTypeLib);

        #undef DETACH

        // Detach Flash QI hook separately (installed via MaybeHookFlashQI)
        if (s_flashQIHooked && s_origFlashQI) {
            DetourDetach(reinterpret_cast<void**>(&s_origFlashQI),
                         reinterpret_cast<void*>(Hooked_FlashQI));
            s_origFlashQI = nullptr;
            s_flashQIHooked = false;
        }

        DetourTransactionCommit();
        m_hooked = false;
    }

    // Restore script engine vtable hook
    if (s_hookedScriptVtable && s_origParseScriptText) {
        DWORD oldProtect;
        if (VirtualProtect(&s_hookedScriptVtable[5], sizeof(void*),
                           PAGE_EXECUTE_READWRITE, &oldProtect)) {
            s_hookedScriptVtable[5] = reinterpret_cast<void*>(s_origParseScriptText);
            VirtualProtect(&s_hookedScriptVtable[5], sizeof(void*), oldProtect, &oldProtect);
        }
        s_hookedScriptVtable = nullptr;
        s_origParseScriptText = nullptr;
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
        g_hOcxModule = nullptr;
        FreeLibrary(m_hModule);
        m_hModule = nullptr;
    }
}
