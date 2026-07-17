#include "swf_mime_filter.h"

#include <stdlib.h>

#include "debug.h"
#include "text_encoding.h"

#include <windows.h>
#include <shlwapi.h>
#include <urlmon.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <string>

namespace {

constexpr wchar_t FLASH_MIME_TYPE[] = L"application/x-shockwave-flash";
constexpr DWORD MIME_FILTER_FLASH = 0x1;
constexpr DWORD PROTOCOL_HANDLER_FILE = 0x2;

// Process-local registration identity. It is never registered in COM or the
// system registry; URLMon receives the in-memory class factory directly.
const CLSID CLSID_FlashIeSwfMimeFilter =
    {0x9f514f32, 0x5a6c, 0x4f86, {0xa8, 0xe7, 0x70, 0x6d, 0x2b, 0xc1, 0x4c, 0x73}};

class SwfMimeProtocol;
class SwfMimeFilterFactory;

SRWLOCK g_stateLock = SRWLOCK_INIT;
IInternetSession* g_session = nullptr;
SwfMimeFilterFactory* g_factory = nullptr;
std::wstring g_pendingUrl;
ULONGLONG g_generation = 0;
bool g_armed = false;
DWORD g_registeredHandlers = 0;
SwfMimeProtocol* g_completionFilter = nullptr;
UINT_PTR g_completionTimer = 0;

enum class NavigationScheme {
    Unsupported,
    Http,
    File,
};

NavigationScheme NormalizeNavigationUrl(
    const wchar_t* url, bool requireSwfPath, std::wstring& normalized)
{
    normalized.clear();
    if (!url || !url[0])
        return NavigationScheme::Unsupported;

    IUri* uri = nullptr;
    HRESULT hr = CreateUri(url,
        Uri_CREATE_CANONICALIZE |
        Uri_CREATE_ALLOW_IMPLICIT_FILE_SCHEME,
        0, &uri);
    if (FAILED(hr) || !uri)
        return NavigationScheme::Unsupported;

    BSTR scheme = nullptr;
    BSTR path = nullptr;
    BSTR absolute = nullptr;
    NavigationScheme result = NavigationScheme::Unsupported;
    if (SUCCEEDED(uri->GetSchemeName(&scheme)) && scheme) {
        if (_wcsicmp(scheme, L"http") == 0 ||
            _wcsicmp(scheme, L"https") == 0) {
            result = NavigationScheme::Http;
        } else if (_wcsicmp(scheme, L"file") == 0) {
            result = NavigationScheme::File;
        }
    }

    bool matches = result != NavigationScheme::Unsupported &&
        SUCCEEDED(uri->GetPath(&path)) && path;

    if (matches && requireSwfPath) {
        size_t pathLength = SysStringLen(path);
        matches = pathLength >= 4 &&
            _wcsicmp(path + pathLength - 4, L".swf") == 0;
    }

    if (matches && SUCCEEDED(uri->GetAbsoluteUri(&absolute)) && absolute) {
        normalized.assign(absolute, SysStringLen(absolute));
        size_t fragment = normalized.find(L'#');
        if (fragment != std::wstring::npos)
            normalized.erase(fragment);
    } else {
        matches = false;
    }

    SysFreeString(absolute);
    SysFreeString(path);
    SysFreeString(scheme);
    uri->Release();
    return matches ? result : NavigationScheme::Unsupported;
}

bool IsExistingFileUrl(const std::wstring& url)
{
    std::wstring path(32768, L'\0');
    DWORD pathLength = static_cast<DWORD>(path.size());
    if (FAILED(PathCreateFromUrlW(
            url.c_str(), path.data(), &pathLength, 0)))
        return false;

    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring EscapeHtmlAttribute(const std::wstring& value)
{
    std::wstring escaped;
    escaped.reserve(value.size());
    for (wchar_t ch : value) {
        switch (ch) {
        case L'&':  escaped += L"&amp;";  break;
        case L'<':  escaped += L"&lt;";   break;
        case L'>':  escaped += L"&gt;";   break;
        case L'\"': escaped += L"&quot;"; break;
        case L'\'': escaped += L"&#39;";  break;
        default:    escaped += ch;        break;
        }
    }
    return escaped;
}

std::string BuildWrapperHtml(const std::wstring& movieUrl)
{
    std::wstring escaped = EscapeHtmlAttribute(movieUrl);
    std::wstring html =
        L"<!doctype html><html><head><meta charset=\"utf-8\">"
        L"<meta http-equiv=\"X-UA-Compatible\" content=\"IE=edge\">"
        L"<title>SWF Player</title><style>html,body,embed{display:block;width:100%;"
        L"height:100%;margin:0;padding:0;overflow:hidden}</style></head><body>"
        L"<embed id=\"flash-content\" src=\"" + escaped + L"\" "
        L"type=\"application/x-shockwave-flash\" width=\"100%\" "
        L"height=\"100%\" allowFullScreen=\"true\"></embed>"
        L"</body></html>";
    std::string utf8;
    if (!TextEncoding::WideToUtf8(html.c_str(), utf8))
        return {};
    return utf8;
}

void UnregisterHandlers()
{
    IInternetSession* session = nullptr;
    IClassFactory* factory = nullptr;
    DWORD registeredHandlers = 0;

    AcquireSRWLockExclusive(&g_stateLock);
    if (g_registeredHandlers && g_session && g_factory) {
        session = g_session;
        factory = reinterpret_cast<IClassFactory*>(g_factory);
        session->AddRef();
        factory->AddRef();
        registeredHandlers = g_registeredHandlers;
        g_registeredHandlers = 0;
    }
    ReleaseSRWLockExclusive(&g_stateLock);

    if (session && factory) {
        if (registeredHandlers & PROTOCOL_HANDLER_FILE)
            session->UnregisterNameSpace(factory, L"file");
        if (registeredHandlers & MIME_FILTER_FLASH)
            session->UnregisterMimeFilter(factory, FLASH_MIME_TYPE);
        factory->Release();
        session->Release();
    }
}

bool ClaimPendingNavigation(
    const wchar_t* url, IInternetBindInfo* bindInfo,
    std::wstring& movieUrl, ULONGLONG& generation)
{
    LPOLESTR bindingUrlValue = nullptr;
    LPOLESTR rootDocumentValue = nullptr;
    LPOLESTR documentUrlValue = nullptr;
    ULONG fetched = 0;
    if (bindInfo)
        bindInfo->GetBindString(
            BINDSTRING_URL, &bindingUrlValue, 1, &fetched);
    fetched = 0;
    if (bindInfo)
        bindInfo->GetBindString(
            BINDSTRING_ROOTDOC_URL, &rootDocumentValue, 1, &fetched);
    fetched = 0;
    if (bindInfo)
        bindInfo->GetBindString(
            BINDSTRING_DOC_URL, &documentUrlValue, 1, &fetched);

    const wchar_t* bindingUrl = bindingUrlValue ? bindingUrlValue : url;
    std::wstring normalized;
    NavigationScheme bindingScheme = NormalizeNavigationUrl(
        bindingUrl, false, normalized);
    bool normalizedBinding =
        bindingScheme != NavigationScheme::Unsupported;

    DWORD bindFlags = 0;
    BINDINFO info = {};
    info.cbSize = sizeof(info);
    HRESULT bindHr = bindInfo
        ? bindInfo->GetBindInfo(&bindFlags, &info)
        : E_NOINTERFACE;
    DWORD options = SUCCEEDED(bindHr) ? info.dwOptions : 0;
    if (SUCCEEDED(bindHr))
        ReleaseBindInfo(&info);

    bool claimed = false;
    AcquireSRWLockExclusive(&g_stateLock);
    bool exactUrl = normalizedBinding &&
        _wcsicmp(normalized.c_str(), g_pendingUrl.c_str()) == 0;
    bool topLevelRedirect = bindingScheme == NavigationScheme::Http &&
        (!rootDocumentValue || !rootDocumentValue[0]) &&
        (!documentUrlValue || !documentUrlValue[0]);
    if (g_armed && (exactUrl || topLevelRedirect)) {
        movieUrl = normalized;
        generation = g_generation;
        g_armed = false;
        claimed = true;
    }
    ReleaseSRWLockExclusive(&g_stateLock);

    DbgTrace(L"[FlashIE] SWF MIME Start: type=%s bindUrl=%s rootUrl=%s docUrl=%s "
             L"flags=0x%08X options=0x%08X navigate=%d claimed=%d\n",
             url ? url : L"(null)",
             bindingUrlValue ? bindingUrlValue : L"(null)",
             rootDocumentValue ? rootDocumentValue : L"(null)",
             documentUrlValue ? documentUrlValue : L"(null)",
             bindFlags, options,
             (options & BINDINFO_OPTIONS_SHDOCVW_NAVIGATE) != 0, claimed);

    CoTaskMemFree(documentUrlValue);
    CoTaskMemFree(rootDocumentValue);
    CoTaskMemFree(bindingUrlValue);

    if (claimed)
        UnregisterHandlers();
    return claimed;
}

class SwfMimeProtocol final : public IInternetProtocol,
                              public IInternetProtocolSink {
public:
    static void CancelPendingCompletion()
    {
        SwfMimeProtocol* filter = nullptr;
        UINT_PTR timer = 0;
        AcquireSRWLockExclusive(&g_stateLock);
        filter = g_completionFilter;
        timer = g_completionTimer;
        g_completionFilter = nullptr;
        g_completionTimer = 0;
        if (filter)
            filter->m_completionTimer = 0;
        ReleaseSRWLockExclusive(&g_stateLock);

        if (timer)
            KillTimer(nullptr, timer);
        if (filter)
            filter->Release();
    }

    // IUnknown through IInternetProtocol
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IInternetProtocolRoot ||
            riid == IID_IInternetProtocol) {
            *ppv = static_cast<IInternetProtocol*>(this);
        } else if (riid == IID_IInternetProtocolSink) {
            *ppv = static_cast<IInternetProtocolSink*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_ref));
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG ref = static_cast<ULONG>(InterlockedDecrement(&m_ref));
        if (!ref)
            delete this;
        return ref;
    }

    // IInternetProtocolRoot
    STDMETHODIMP Start(LPCWSTR url, IInternetProtocolSink* sink,
        IInternetBindInfo* bindInfo, DWORD grfPI, HANDLE_PTR) override
    {
        DbgTrace(L"[FlashIE] SWF MIME filter Start grfPI=0x%08X\n", grfPI);
        std::wstring movieUrl;
        ULONGLONG generation = 0;
        if (!sink || !ClaimPendingNavigation(
                url, bindInfo, movieUrl, generation))
            return INET_E_USE_DEFAULT_PROTOCOLHANDLER;

        m_html = BuildWrapperHtml(movieUrl);
        if (m_html.empty())
            return E_OUTOFMEMORY;

        m_sink = sink;
        m_sink->AddRef();
        m_generation = generation;

        HRESULT filterMimeHr = m_sink->ReportProgress(
            BINDSTATUS_FILTERREPORTMIMETYPE, L"text/html");
        DbgTrace(L"[FlashIE] SWF MIME filter final MIME -> 0x%08X\n",
                 filterMimeHr);

        HRESULT hr = m_sink->ReportProgress(
            BINDSTATUS_MIMETYPEAVAILABLE, L"text/html");
        if (FAILED(hr))
            return hr;

        hr = m_sink->ReportData(
            BSCF_FIRSTDATANOTIFICATION | BSCF_LASTDATANOTIFICATION |
            BSCF_DATAFULLYAVAILABLE,
            static_cast<ULONG>(m_html.size()),
            static_cast<ULONG>(m_html.size()));
        if (FAILED(hr))
            return hr;

        DbgTrace(L"[FlashIE] SWF MIME filter transformed generation=%llu bytes=%u\n",
                 m_generation, static_cast<unsigned>(m_html.size()));
        return S_OK;
    }

    STDMETHODIMP Continue(PROTOCOLDATA*) override { return E_NOTIMPL; }

    STDMETHODIMP Abort(HRESULT, DWORD) override
    {
        m_aborted = true;
        return S_OK;
    }

    STDMETHODIMP Terminate(DWORD) override
    {
        if (m_sink) {
            m_sink->Release();
            m_sink = nullptr;
        }
        return S_OK;
    }

    STDMETHODIMP Suspend() override { return E_NOTIMPL; }
    STDMETHODIMP Resume() override { return E_NOTIMPL; }

    // IInternetProtocol
    STDMETHODIMP Read(void* buffer, ULONG size, ULONG* read) override
    {
        if (!read)
            return E_POINTER;
        *read = 0;
        if (!buffer && size)
            return E_POINTER;
        if (m_offset >= m_html.size())
            return S_FALSE;

        size_t count = std::min<size_t>(size, m_html.size() - m_offset);
        memcpy(buffer, m_html.data() + m_offset, count);
        m_offset += count;
        *read = static_cast<ULONG>(count);

        if (m_offset == m_html.size() && !m_completionTimer) {
            AddRef();
            UINT_PTR timer = SetTimer(
                nullptr, 0, 1, &SwfMimeProtocol::CompletionTimerProc);
            if (!timer) {
                Release();
                return E_FAIL;
            }

            bool scheduled = false;
            AcquireSRWLockExclusive(&g_stateLock);
            if (!g_completionFilter) {
                g_completionFilter = this;
                g_completionTimer = timer;
                m_completionTimer = timer;
                scheduled = true;
            }
            ReleaseSRWLockExclusive(&g_stateLock);

            if (!scheduled) {
                KillTimer(nullptr, timer);
                Release();
                return E_UNEXPECTED;
            }
        }
        return S_OK;
    }

    STDMETHODIMP Seek(LARGE_INTEGER, DWORD, ULARGE_INTEGER*) override
    {
        return E_NOTIMPL;
    }

    STDMETHODIMP LockRequest(DWORD) override { return S_OK; }
    STDMETHODIMP UnlockRequest() override { return S_OK; }

    // IInternetProtocolSink: forward callbacks if URLMon composes the filter
    // into an upstream protocol chain.
    STDMETHODIMP Switch(PROTOCOLDATA* data) override
    {
        return m_sink ? m_sink->Switch(data) : E_UNEXPECTED;
    }

    STDMETHODIMP ReportProgress(ULONG code, LPCWSTR text) override
    {
        return m_sink ? m_sink->ReportProgress(code, text) : E_UNEXPECTED;
    }

    STDMETHODIMP ReportData(DWORD flags, ULONG progress, ULONG maximum) override
    {
        return m_sink
            ? m_sink->ReportData(flags, progress, maximum)
            : E_UNEXPECTED;
    }

    STDMETHODIMP ReportResult(HRESULT hr, DWORD error, LPCWSTR result) override
    {
        return m_sink
            ? m_sink->ReportResult(hr, error, result)
            : E_UNEXPECTED;
    }

private:
    static void CALLBACK CompletionTimerProc(
        HWND, UINT, UINT_PTR timer, DWORD)
    {
        SwfMimeProtocol* filter = nullptr;
        AcquireSRWLockExclusive(&g_stateLock);
        if (g_completionTimer == timer) {
            filter = g_completionFilter;
            g_completionFilter = nullptr;
            g_completionTimer = 0;
            if (filter)
                filter->m_completionTimer = 0;
        }
        ReleaseSRWLockExclusive(&g_stateLock);

        KillTimer(nullptr, timer);
        if (!filter)
            return;

        if (!filter->m_aborted && filter->m_sink) {
            IInternetProtocolSink* sink = filter->m_sink;
            sink->AddRef();
            DbgTrace(L"[FlashIE] SWF MIME filter completed generation=%llu\n",
                     filter->m_generation);
            sink->ReportResult(S_OK, 0, nullptr);
            sink->Release();
        }
        filter->Release();
    }

    ~SwfMimeProtocol()
    {
        if (m_sink)
            m_sink->Release();
    }

    LONG m_ref = 1;
    IInternetProtocolSink* m_sink = nullptr;
    std::string m_html;
    size_t m_offset = 0;
    ULONGLONG m_generation = 0;
    UINT_PTR m_completionTimer = 0;
    bool m_aborted = false;
};

class SwfMimeFilterFactory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        *ppv = nullptr;
        if (riid != IID_IUnknown && riid != IID_IClassFactory)
            return E_NOINTERFACE;
        *ppv = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_ref));
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG ref = static_cast<ULONG>(InterlockedDecrement(&m_ref));
        if (!ref)
            delete this;
        return ref;
    }

    STDMETHODIMP CreateInstance(
        IUnknown* outer, REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        *ppv = nullptr;
        if (outer)
            return CLASS_E_NOAGGREGATION;

        SwfMimeProtocol* filter = new (std::nothrow) SwfMimeProtocol();
        if (!filter)
            return E_OUTOFMEMORY;
        HRESULT hr = filter->QueryInterface(riid, ppv);
        filter->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL) override { return S_OK; }

private:
    ~SwfMimeFilterFactory() = default;
    LONG m_ref = 1;
};

} // namespace

namespace SwfMimeFilter {

bool Initialize()
{
    AcquireSRWLockExclusive(&g_stateLock);
    bool initialized = g_session && g_factory;
    ReleaseSRWLockExclusive(&g_stateLock);
    if (initialized)
        return true;

    IInternetSession* session = nullptr;
    HRESULT hr = CoInternetGetSession(0, &session, 0);
    if (FAILED(hr) || !session) {
        DbgTrace(L"[FlashIE] CoInternetGetSession failed: 0x%08X\n", hr);
        return false;
    }

    SwfMimeFilterFactory* factory =
        new (std::nothrow) SwfMimeFilterFactory();
    if (!factory) {
        session->Release();
        return false;
    }

    AcquireSRWLockExclusive(&g_stateLock);
    g_session = session;
    g_factory = factory;
    ReleaseSRWLockExclusive(&g_stateLock);
    return true;
}

void Shutdown()
{
    SwfMimeProtocol::CancelPendingCompletion();
    Cancel();

    IInternetSession* session = nullptr;
    SwfMimeFilterFactory* factory = nullptr;
    AcquireSRWLockExclusive(&g_stateLock);
    session = g_session;
    factory = g_factory;
    g_session = nullptr;
    g_factory = nullptr;
    g_pendingUrl.clear();
    g_armed = false;
    ReleaseSRWLockExclusive(&g_stateLock);

    if (factory)
        factory->Release();
    if (session)
        session->Release();
}

bool IsSupportedSwfUrl(const wchar_t* url)
{
    std::wstring normalized;
    NavigationScheme scheme = NormalizeNavigationUrl(url, true, normalized);
    return scheme != NavigationScheme::Unsupported &&
        (scheme != NavigationScheme::File || IsExistingFileUrl(normalized));
}

bool Arm(const wchar_t* url)
{
    std::wstring normalized;
    NavigationScheme scheme = NormalizeNavigationUrl(url, true, normalized);
    if (scheme == NavigationScheme::Unsupported ||
        (scheme == NavigationScheme::File && !IsExistingFileUrl(normalized)))
        return false;

    Cancel();

    IInternetSession* session = nullptr;
    IClassFactory* factory = nullptr;
    AcquireSRWLockShared(&g_stateLock);
    if (g_session && g_factory) {
        session = g_session;
        factory = reinterpret_cast<IClassFactory*>(g_factory);
        session->AddRef();
        factory->AddRef();
    }
    ReleaseSRWLockShared(&g_stateLock);
    if (!session || !factory) {
        if (factory) factory->Release();
        if (session) session->Release();
        return false;
    }

    DWORD registeredHandlers = 0;
    HRESULT hr = session->RegisterMimeFilter(
        factory, CLSID_FlashIeSwfMimeFilter, FLASH_MIME_TYPE);
    if (SUCCEEDED(hr))
        registeredHandlers |= MIME_FILTER_FLASH;
    if (SUCCEEDED(hr) && scheme == NavigationScheme::File) {
        // The built-in file: handler bypasses MIME filters and classifies SWF
        // data as application/octet-stream, so intercept this one navigation
        // at the namespace layer. ClaimPendingNavigation still requires an
        // exact URL match, and the handler is removed before embed loads it.
        hr = session->RegisterNameSpace(
            factory, CLSID_FlashIeSwfMimeFilter, L"file", 0, nullptr, 0);
        if (SUCCEEDED(hr))
            registeredHandlers |= PROTOCOL_HANDLER_FILE;
    }

    if (SUCCEEDED(hr)) {
        ULONGLONG generation = 0;
        std::wstring armedUrl;
        AcquireSRWLockExclusive(&g_stateLock);
        g_pendingUrl = std::move(normalized);
        g_generation++;
        g_armed = true;
        g_registeredHandlers = registeredHandlers;
        generation = g_generation;
        armedUrl = g_pendingUrl;
        ReleaseSRWLockExclusive(&g_stateLock);
        DbgTrace(L"[FlashIE] SWF MIME armed generation=%llu url=%s\n",
                 generation, armedUrl.c_str());
    } else {
        DbgTrace(L"[FlashIE] Register SWF URLMon handler failed: 0x%08X\n",
                 hr);
        if (registeredHandlers & PROTOCOL_HANDLER_FILE)
            session->UnregisterNameSpace(factory, L"file");
        if (registeredHandlers & MIME_FILTER_FLASH)
            session->UnregisterMimeFilter(factory, FLASH_MIME_TYPE);
    }

    factory->Release();
    session->Release();
    return SUCCEEDED(hr);
}

void Cancel()
{
    bool wasArmed = false;
    AcquireSRWLockExclusive(&g_stateLock);
    wasArmed = g_armed;
    g_armed = false;
    g_pendingUrl.clear();
    ReleaseSRWLockExclusive(&g_stateLock);
    UnregisterHandlers();
    if (wasArmed)
        DbgTrace(L"[FlashIE] SWF MIME canceled\n");
}

bool IsArmed()
{
    AcquireSRWLockShared(&g_stateLock);
    bool armed = g_armed;
    ReleaseSRWLockShared(&g_stateLock);
    return armed;
}

} // namespace SwfMimeFilter
