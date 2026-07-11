#pragma once
#include <windows.h>
#include <objbase.h>

class FlashLoader {
public:
    FlashLoader() = default;
    ~FlashLoader() { Deactivate(); }

    FlashLoader(const FlashLoader&) = delete;
    FlashLoader& operator=(const FlashLoader&) = delete;

    // Phase 1: Load Flash.ocx and obtain its class factory.
    // Must be called AFTER OleInitialize.
    bool Activate();

    // Phase 2: Install inline hooks (detours) on COM, registry,
    // security, and TypeLib APIs. Force-loads
    // mshtml.dll/urlmon.dll/ieframe.dll, then patches them.
    // Call BEFORE browser creation so hooks are in place when
    // MSHTML initializes.
    void InstallHooks();

    // Cleanup. Must be called BEFORE OleUninitialize.
    void Deactivate();

    bool IsActive() const { return m_hModule != nullptr; }

    // Exposed for static hook helpers (GetFlashFactory, IsFlashCLSID)
    static IClassFactory* s_pFlashFactory;

private:
    HMODULE        m_hModule = nullptr;
    IClassFactory* m_pFactory = nullptr;
    DWORD          m_dwCookie = 0;
    bool           m_hooked = false;

    // Hook callbacks
    static HRESULT STDAPICALLTYPE Hooked_CoGetClassObject(
        REFCLSID rclsid, DWORD dwClsContext, LPVOID pvReserved,
        REFIID riid, LPVOID* ppv);

    static HRESULT STDAPICALLTYPE Hooked_CoCreateInstance(
        REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext,
        REFIID riid, LPVOID* ppv);

    static LSTATUS WINAPI Hooked_RegOpenKeyExW(
        HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions,
        REGSAM samDesired, PHKEY phkResult);

    static LSTATUS WINAPI Hooked_RegQueryValueExW(
        HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved,
        LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);

    static LSTATUS WINAPI Hooked_RegCloseKey(HKEY hKey);

    static HRESULT STDAPICALLTYPE Hooked_CoGetClassObjectFromURL(
        REFCLSID rclsid, LPCWSTR szCodeURL,
        DWORD dwFileVersionMS, DWORD dwFileVersionLS,
        LPCWSTR szContentType, LPBINDCTX pBindCtx,
        DWORD dwClsContext, LPVOID pvReserved,
        REFIID riid, LPVOID* ppv);

    static HRESULT WINAPI Hooked_WldpIsClassInApprovedList(
        const CLSID* classID, /*PWLDP_HOST_INFORMATION*/ void* hostInfo,
        BOOL* isApproved, DWORD optionalFlags);

    static HRESULT WINAPI Hooked_WldpQueryDynamicCodeTrust(
        HANDLE fileHandle, void* baseImage, DWORD imageSize);

};
