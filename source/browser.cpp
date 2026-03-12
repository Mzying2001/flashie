#include "browser.h"
#include <stdio.h>
#include <urlmon.h>    // UrlMkSetSessionOption

// ===============================================================
// COleClientSite
// ===============================================================

STDMETHODIMP COleClientSite::QueryInterface(REFIID riid, void** ppv)
{
    return m_pSite->QueryInterface(riid, ppv);
}

STDMETHODIMP_(ULONG) COleClientSite::AddRef()  { return ++m_ref; }
STDMETHODIMP_(ULONG) COleClientSite::Release() { return --m_ref; }

// ===============================================================
// COleInPlaceSite
// ===============================================================

STDMETHODIMP COleInPlaceSite::QueryInterface(REFIID riid, void** ppv)
{
    return m_pSite->QueryInterface(riid, ppv);
}

STDMETHODIMP_(ULONG) COleInPlaceSite::AddRef()  { return ++m_ref; }
STDMETHODIMP_(ULONG) COleInPlaceSite::Release() { return --m_ref; }

STDMETHODIMP COleInPlaceSite::GetWindow(HWND* phwnd)
{
    *phwnd = m_pSite->m_hWnd;
    return S_OK;
}

STDMETHODIMP COleInPlaceSite::OnInPlaceActivate()
{
    if (m_pSite->m_lpOleObject) {
        m_pSite->m_lpOleObject->QueryInterface(IID_IOleInPlaceObject,
            reinterpret_cast<void**>(&m_pSite->m_lpInPlaceObject));
    }
    return S_OK;
}

STDMETHODIMP COleInPlaceSite::GetWindowContext(
    IOleInPlaceFrame** ppFrame,
    IOleInPlaceUIWindow** ppDoc,
    LPRECT lprcPosRect,
    LPRECT lprcClipRect,
    LPOLEINPLACEFRAMEINFO lpFrameInfo)
{
    *ppFrame = m_pSite->m_pInPlaceFrame;
    m_pSite->m_pInPlaceFrame->AddRef();
    *ppDoc = nullptr;

    GetClientRect(m_pSite->m_hWnd, lprcPosRect);
    *lprcClipRect = *lprcPosRect;

    lpFrameInfo->fMDIApp = FALSE;
    lpFrameInfo->hwndFrame = m_pSite->m_hWnd;
    lpFrameInfo->haccel = nullptr;
    lpFrameInfo->cAccelEntries = 0;

    return S_OK;
}

STDMETHODIMP COleInPlaceSite::OnInPlaceDeactivate()
{
    if (m_pSite->m_lpInPlaceObject) {
        m_pSite->m_lpInPlaceObject->Release();
        m_pSite->m_lpInPlaceObject = nullptr;
    }
    return S_OK;
}

// ===============================================================
// COleInPlaceFrame
// ===============================================================

STDMETHODIMP COleInPlaceFrame::QueryInterface(REFIID riid, void** ppv)
{
    if (riid == IID_IUnknown || riid == IID_IOleWindow || riid == IID_IOleInPlaceUIWindow || riid == IID_IOleInPlaceFrame) {
        *ppv = static_cast<IOleInPlaceFrame*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) COleInPlaceFrame::AddRef()  { return ++m_ref; }
STDMETHODIMP_(ULONG) COleInPlaceFrame::Release() { return --m_ref; }

STDMETHODIMP COleInPlaceFrame::GetWindow(HWND* phwnd)
{
    *phwnd = m_pSite->m_hWnd;
    return S_OK;
}

// ===============================================================
// COleSite
// ===============================================================

COleSite::COleSite()
{
    m_pClientSite  = new COleClientSite(this);
    m_pInPlaceSite = new COleInPlaceSite(this);
    m_pInPlaceFrame = new COleInPlaceFrame(this);
    StgCreateDocfile(nullptr,
        STGM_READWRITE | STGM_TRANSACTED | STGM_SHARE_EXCLUSIVE | STGM_DELETEONRELEASE,
        0, &m_lpStorage);
}

COleSite::~COleSite()
{
    if (m_lpInPlaceObject) { m_lpInPlaceObject->Release(); m_lpInPlaceObject = nullptr; }
    if (m_lpStorage)       { m_lpStorage->Release(); m_lpStorage = nullptr; }
    delete m_pClientSite;
    delete m_pInPlaceSite;
    delete m_pInPlaceFrame;
}

STDMETHODIMP COleSite::QueryInterface(REFIID riid, void** ppv)
{
    if (riid == IID_IUnknown) {
        *ppv = static_cast<IServiceProvider*>(this);
    } else if (riid == IID_IOleClientSite) {
        *ppv = static_cast<IOleClientSite*>(m_pClientSite);
        m_pClientSite->AddRef();
        return S_OK;
    } else if (riid == IID_IOleInPlaceSite || riid == IID_IOleWindow) {
        *ppv = static_cast<IOleInPlaceSite*>(m_pInPlaceSite);
        m_pInPlaceSite->AddRef();
        return S_OK;
    } else if (riid == IID_IDocHostUIHandler) {
        *ppv = static_cast<IDocHostUIHandler*>(this);
    } else if (riid == IID_IDispatch || riid == DIID_DWebBrowserEvents2) {
        *ppv = static_cast<IDispatch*>(this);
    } else if (riid == IID_IServiceProvider) {
        *ppv = static_cast<IServiceProvider*>(this);
    } else if (riid == IID_IInternetSecurityManager) {
        *ppv = static_cast<IInternetSecurityManager*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) COleSite::AddRef()  { return ++m_ref; }
STDMETHODIMP_(ULONG) COleSite::Release()
{
    ULONG ref = --m_ref;
    if (ref == 0) delete this;
    return ref;
}

// IServiceProvider
STDMETHODIMP COleSite::QueryService(REFGUID guidService, REFIID riid, void** ppv)
{
    *ppv = nullptr;
    if (guidService == SID_SInternetSecurityManager &&
        riid == IID_IInternetSecurityManager) {
        OutputDebugStringW(L"[FlashIE] QueryService: SID_SInternetSecurityManager -> returning our ISM\n");
        *ppv = static_cast<IInternetSecurityManager*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

// IDocHostUIHandler
STDMETHODIMP COleSite::ShowContextMenu(DWORD, POINT*, IUnknown*, IDispatch*)
{
    return E_NOTIMPL; // let the control handle it
}

STDMETHODIMP COleSite::GetHostInfo(DOCHOSTUIINFO* pInfo)
{
    OutputDebugStringW(L"[FlashIE] GetHostInfo called\n");
    pInfo->cbSize = sizeof(DOCHOSTUIINFO);
    pInfo->dwFlags = DOCHOSTUIFLAG_NO3DBORDER;
    pInfo->dwDoubleClick = DOCHOSTUIDBLCLK_DEFAULT;
    pInfo->pchHostCss = nullptr;
    pInfo->pchHostNS = nullptr;
    return S_OK;
}

// IDispatch (DWebBrowserEvents2)
STDMETHODIMP COleSite::GetTypeInfoCount(UINT* pctinfo)
{
    *pctinfo = 0;
    return S_OK;
}

STDMETHODIMP COleSite::GetTypeInfo(UINT, LCID, ITypeInfo**) { return E_NOTIMPL; }
STDMETHODIMP COleSite::GetIDsOfNames(REFIID, OLECHAR**, UINT, LCID, DISPID*)
{
    return DISP_E_UNKNOWNNAME;
}

STDMETHODIMP COleSite::Invoke(DISPID dispid, REFIID, LCID, WORD wFlags, DISPPARAMS* pDispParams,
                               VARIANT* pvarResult, EXCEPINFO*, UINT*)
{
    // Log DISPIDs to diagnose navigation bug
    {
        static int s_invokeLogCount = 0;
        if (s_invokeLogCount < 500) {
            s_invokeLogCount++;
            wchar_t buf[256];
            _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                L"[FlashIE] Invoke: dispid=%d (0x%X) wFlags=0x%X cArgs=%u pvarResult=%p\n",
                dispid, dispid, wFlags, pDispParams ? pDispParams->cArgs : 0, pvarResult);
            OutputDebugStringW(buf);
        }
    }
    switch (dispid) {

    // Ambient property: download control flags (matches OOBE pattern)
    case DISPID_AMBIENT_DLCONTROL: {
        if (pvarResult) {
            V_VT(pvarResult) = VT_I4;
            V_I4(pvarResult) = DLCTL_DLIMAGES | DLCTL_VIDEOS | DLCTL_BGSOUNDS
                             | DLCTL_SILENT;
        }
        return S_OK;
    }

    case DISPID_NAVIGATECOMPLETE2: {
        // rgvarg[1] = pDisp (IDispatch of the frame), rgvarg[0] = URL
        if (pDispParams->cArgs >= 2 && m_pBrowserHost) {
            // Only process top-level frame navigations
            IDispatch* pEventDisp = pDispParams->rgvarg[1].pdispVal;
            IUnknown* pEventUnk = nullptr;
            IUnknown* pBrowserUnk = nullptr;
            bool isTopLevel = false;
            if (pEventDisp)
                pEventDisp->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&pEventUnk));
            if (m_pBrowserHost->m_pWebBrowser)
                m_pBrowserHost->m_pWebBrowser->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&pBrowserUnk));
            isTopLevel = (pEventUnk && pEventUnk == pBrowserUnk);
            if (pEventUnk) pEventUnk->Release();
            if (pBrowserUnk) pBrowserUnk->Release();

            if (isTopLevel && m_pBrowserHost->m_navCallback) {
                VARIANT* pURL = &pDispParams->rgvarg[0];
                if (pURL->vt == (VT_VARIANT | VT_BYREF))
                    pURL = pURL->pvarVal;
                if (pURL->vt == VT_BSTR)
                    m_pBrowserHost->m_navCallback(pURL->bstrVal, m_pBrowserHost->m_navCtx);
            }
        }
        return S_OK;
    }

    case DISPID_TITLECHANGE: {
        if (pDispParams->cArgs >= 1 && m_pBrowserHost && m_pBrowserHost->m_titleCallback) {
            if (pDispParams->rgvarg[0].vt == VT_BSTR)
                m_pBrowserHost->m_titleCallback(pDispParams->rgvarg[0].bstrVal,
                                                 m_pBrowserHost->m_titleCtx);
        }
        return S_OK;
    }

    case DISPID_NEWWINDOW2: {
        // Params (reverse order): rgvarg[0]=Cancel, rgvarg[1]=ppDisp
        // Cancel the new window — NewWindow3 (which has the URL) handles navigation on IE8+.
        if (pDispParams->cArgs >= 2) {
            if (pDispParams->rgvarg[0].vt == (VT_BOOL | VT_BYREF))
                *pDispParams->rgvarg[0].pboolVal = VARIANT_TRUE;
        }
        return S_OK;
    }

    case DISPID_NEWWINDOW3: {
        // Params (reverse): rgvarg[0]=bstrUrl, [1]=bstrUrlContext, [2]=dwFlags, [3]=Cancel, [4]=ppDisp
        if (pDispParams->cArgs >= 5 && m_pBrowserHost) {
            // Cancel the new window
            if (pDispParams->rgvarg[3].vt == (VT_BOOL | VT_BYREF))
                *pDispParams->rgvarg[3].pboolVal = VARIANT_TRUE;
            // Navigate to the URL in the current window
            if (pDispParams->rgvarg[0].vt == VT_BSTR && pDispParams->rgvarg[0].bstrVal)
                m_pBrowserHost->Navigate(pDispParams->rgvarg[0].bstrVal);
        }
        return S_OK;
    }

    // Navigation lifecycle events — log for diagnostics
    case 250: // DISPID_BEFORENAVIGATE2
    {
        // rgvarg: [6]=pDisp, [5]=URL, [4]=Flags, [3]=TargetFrameName, [2]=PostData, [1]=Headers, [0]=Cancel
        if (pDispParams->cArgs >= 7) {
            VARIANT* pURL = &pDispParams->rgvarg[5];
            if (pURL->vt == (VT_VARIANT | VT_BYREF)) pURL = pURL->pvarVal;
            if (pURL->vt == VT_BSTR) {
                wchar_t buf[512];
                _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                    L"[FlashIE] BEFORENAVIGATE2: %s\n", pURL->bstrVal);
                OutputDebugStringW(buf);
            }
        }
        return S_OK;
    }

    case 259: // DISPID_DOCUMENTCOMPLETE
    {
        if (pDispParams->cArgs >= 2) {
            VARIANT* pURL = &pDispParams->rgvarg[0];
            if (pURL->vt == (VT_VARIANT | VT_BYREF)) pURL = pURL->pvarVal;
            if (pURL->vt == VT_BSTR) {
                wchar_t buf[512];
                _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                    L"[FlashIE] DOCUMENTCOMPLETE: %s\n", pURL->bstrVal);
                OutputDebugStringW(buf);
            }

            // If about:blank completed as part of a reset, now navigate to the real URL.
            if (m_pBrowserHost && m_pBrowserHost->m_resetting && m_pBrowserHost->m_pendingUrl[0]) {
                wchar_t target[2048];
                wcsncpy_s(target, _countof(target), m_pBrowserHost->m_pendingUrl, _TRUNCATE);
                m_pBrowserHost->m_pendingUrl[0] = L'\0';
                m_pBrowserHost->m_resetting = false;

                wchar_t buf3[512];
                _snwprintf_s(buf3, _countof(buf3), _TRUNCATE,
                    L"[FlashIE] Reset complete, now navigating to: %s\n", target);
                OutputDebugStringW(buf3);

                VARIANT vURL;
                VariantInit(&vURL);
                vURL.vt = VT_BSTR;
                vURL.bstrVal = SysAllocString(target);
                VARIANT vEmpty;
                VariantInit(&vEmpty);
                m_pBrowserHost->m_pWebBrowser->Navigate2(&vURL, &vEmpty, &vEmpty, &vEmpty, &vEmpty);
                VariantClear(&vURL);
                return S_OK;
            }

            // Probe: can we execute scripts via execScript on this document?
            if (m_pBrowserHost && m_pBrowserHost->m_pWebBrowser) {
                IDispatch* pDoc = nullptr;
                if (SUCCEEDED(m_pBrowserHost->m_pWebBrowser->get_Document(&pDoc)) && pDoc) {
                    IHTMLDocument2* pHtml = nullptr;
                    if (SUCCEEDED(pDoc->QueryInterface(IID_IHTMLDocument2, reinterpret_cast<void**>(&pHtml))) && pHtml) {
                        // Check readyState
                        BSTR bstrState = nullptr;
                        pHtml->get_readyState(&bstrState);
                        if (bstrState) {
                            wchar_t buf2[256];
                            _snwprintf_s(buf2, _countof(buf2), _TRUNCATE,
                                L"[FlashIE] Document readyState: %s\n", bstrState);
                            OutputDebugStringW(buf2);
                            SysFreeString(bstrState);
                        }

                        // Try execScript via IHTMLWindow2 to test script engine
                        IHTMLWindow2* pWin = nullptr;
                        if (SUCCEEDED(pHtml->get_parentWindow(&pWin)) && pWin) {
                            BSTR bstrCode = SysAllocString(L"document.title = 'EXECSCRIPT_OK_' + new Date().getTime()");
                            BSTR bstrLang = SysAllocString(L"javascript");
                            VARIANT vResult;
                            VariantInit(&vResult);
                            HRESULT hrExec = pWin->execScript(bstrCode, bstrLang, &vResult);
                            {
                                wchar_t buf2[256];
                                _snwprintf_s(buf2, _countof(buf2), _TRUNCATE,
                                    L"[FlashIE] execScript hr=0x%08X\n", hrExec);
                                OutputDebugStringW(buf2);
                            }
                            VariantClear(&vResult);
                            SysFreeString(bstrCode);
                            SysFreeString(bstrLang);
                            pWin->Release();
                        }
                        pHtml->Release();
                    }
                    pDoc->Release();
                }
            }
        }
        return S_OK;
    }

    case 253: // DISPID_DOWNLOADBEGIN
        OutputDebugStringW(L"[FlashIE] DOWNLOADBEGIN\n");
        return S_OK;
    case 254: // DISPID_DOWNLOADCOMPLETE
        OutputDebugStringW(L"[FlashIE] DOWNLOADCOMPLETE\n");
        return S_OK;

    default:
        break;
    }
    return DISP_E_MEMBERNOTFOUND;
}

// IInternetSecurityManager — allow everything.
// Map ALL URLs to the Internet zone (zone 3).  This prevents zone
// elevation blocks when navigating from http:// to file:// URLs.
// If we defer to the default handler, file:// URLs get Local Machine
// zone (0), and IE blocks scripts when navigating from Internet (3)
// to Local Machine (0) — a privilege elevation.
STDMETHODIMP COleSite::MapUrlToZone(LPCWSTR pwszUrl, DWORD* pdwZone, DWORD)
{
    // Always log non-baidu URLs; log baidu URLs only first 30 times
    if (pwszUrl) {
        bool isBaidu = (wcsstr(pwszUrl, L"baidu") != nullptr);
        static int s_baiduCount = 0;
        if (!isBaidu || s_baiduCount++ < 10) {
            wchar_t buf[512];
            _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                L"[FlashIE] MapUrlToZone: url=%s -> zone=3\n",
                pwszUrl);
            OutputDebugStringW(buf);
        }
    }
    if (pdwZone)
        *pdwZone = URLZONE_INTERNET;
    return S_OK;
}

STDMETHODIMP COleSite::GetSecurityId(LPCWSTR pwszUrl, BYTE* pbSecurityId, DWORD* pcbSecurityId, DWORD_PTR dwReserved)
{
    // Return a FIXED security ID for ALL URLs.
    // Format: zone (4 bytes LE) + domain string (null-terminated).
    // By using the same ID for every URL, MSHTML treats all pages as
    // same-origin, preventing cross-domain script blocking when
    // navigating between different protocols/domains (e.g. https → file).
    // Returning INET_E_DEFAULT_ACTION here caused MSHTML to generate
    // different IDs per URL, which blocked scripts after cross-origin nav.
    static const BYTE s_secId[] = {
        0x03, 0x00, 0x00, 0x00,                     // URLZONE_INTERNET (3)
        'f', 'l', 'a', 's', 'h', 'i', 'e', 0x00    // domain "flashie"
    };

    if (!pbSecurityId || !pcbSecurityId)
        return E_INVALIDARG;

    if (*pcbSecurityId < sizeof(s_secId)) {
        *pcbSecurityId = sizeof(s_secId);
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    memcpy(pbSecurityId, s_secId, sizeof(s_secId));
    *pcbSecurityId = sizeof(s_secId);
    return S_OK;
}

STDMETHODIMP COleSite::ProcessUrlAction(LPCWSTR pwszUrl, DWORD dwAction, BYTE* pPolicy,
                                          DWORD cbPolicy, BYTE* pContext, DWORD cbContext,
                                          DWORD dwFlags, DWORD dwReserved)
{
    // Always log non-baidu URLs; log baidu URLs only first 20 times
    if (pwszUrl) {
        bool isBaidu = (wcsstr(pwszUrl, L"baidu") != nullptr);
        static int s_baiduCount = 0;
        if (!isBaidu || s_baiduCount++ < 10) {
            wchar_t buf[512];
            _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                L"[FlashIE] ProcessUrlAction: action=0x%08X url=%s\n",
                dwAction, pwszUrl);
            OutputDebugStringW(buf);
        }
    }
    if (pPolicy && cbPolicy >= sizeof(DWORD))
        *reinterpret_cast<DWORD*>(pPolicy) = URLPOLICY_ALLOW;
    return S_OK;
}

// ===============================================================
// BrowserHost
// ===============================================================

bool BrowserHost::Initialize(HWND hwndParent, const RECT& rc)
{
    // Set User-Agent to include "MSIE 11.0" so sites serve IE-compatible
    // content.  IE11 Edge mode's default UA omits "MSIE", causing many
    // Chinese Flash game sites (4399, 17roco) to serve non-IE fallback
    // pages that may show "no Flash installed" overlays.
    {
        const char ua[] = "Mozilla/5.0 (compatible; MSIE 11.0; Windows NT 10.0; WOW64; Trident/7.0)";
        UrlMkSetSessionOption(URLMON_OPTION_USERAGENT, (void*)ua, (DWORD)strlen(ua), 0);
    }

    m_pSite = new COleSite();
    m_pSite->m_hWnd = hwndParent;
    m_pSite->m_pBrowserHost = this;

    FORMATETC fe = {};
    fe.dwAspect = DVASPECT_CONTENT;
    fe.lindex = -1;
    fe.tymed = TYMED_NULL;

    HRESULT hr = OleCreate(CLSID_WebBrowser, IID_IWebBrowser2,
                           OLERENDER_DRAW, &fe,
                           m_pSite->m_pClientSite,
                           m_pSite->m_lpStorage,
                           reinterpret_cast<void**>(&m_pWebBrowser));
    if (FAILED(hr) || !m_pWebBrowser)
        return false;

    m_pWebBrowser->QueryInterface(IID_IOleObject, reinterpret_cast<void**>(&m_pOleObject));
    m_pSite->m_lpOleObject = m_pOleObject;
    OleSetContainedObject(m_pOleObject, TRUE);
    m_pOleObject->SetClientSite(m_pSite->m_pClientSite);

    m_pOleObject->DoVerb(OLEIVERB_INPLACEACTIVATE, nullptr,
                         m_pSite->m_pClientSite, -1,
                         hwndParent, &rc);

    m_pWebBrowser->QueryInterface(IID_IOleInPlaceActiveObject,
                                  reinterpret_cast<void**>(&m_pIPActiveObj));

    ConnectEvents();

    m_pWebBrowser->put_Left(rc.left);
    m_pWebBrowser->put_Top(rc.top);
    m_pWebBrowser->put_Width(rc.right - rc.left);
    m_pWebBrowser->put_Height(rc.bottom - rc.top);

    return true;
}

void BrowserHost::Navigate(const wchar_t* url)
{
    if (!m_pWebBrowser) return;

    // If this is already about:blank (initial load or reset), navigate directly.
    if (_wcsicmp(url, L"about:blank") == 0) {
        m_pendingUrl[0] = L'\0';
        m_resetting = false;
        VARIANT vURL;
        VariantInit(&vURL);
        vURL.vt = VT_BSTR;
        vURL.bstrVal = SysAllocString(url);
        VARIANT vEmpty;
        VariantInit(&vEmpty);
        m_pWebBrowser->Navigate2(&vURL, &vEmpty, &vEmpty, &vEmpty, &vEmpty);
        VariantClear(&vURL);
        return;
    }

    // Store the target URL and navigate to about:blank first to reset security context.
    wcsncpy_s(m_pendingUrl, _countof(m_pendingUrl), url, _TRUNCATE);
    m_resetting = true;

    OutputDebugStringW(L"[FlashIE] Navigate: resetting via about:blank first\n");

    VARIANT vURL;
    VariantInit(&vURL);
    vURL.vt = VT_BSTR;
    vURL.bstrVal = SysAllocString(L"about:blank");
    VARIANT vEmpty;
    VariantInit(&vEmpty);
    m_pWebBrowser->Navigate2(&vURL, &vEmpty, &vEmpty, &vEmpty, &vEmpty);
    VariantClear(&vURL);
}

void BrowserHost::GoBack()    { if (m_pWebBrowser) m_pWebBrowser->GoBack(); }
void BrowserHost::GoForward() { if (m_pWebBrowser) m_pWebBrowser->GoForward(); }
void BrowserHost::Refresh()   { if (m_pWebBrowser) m_pWebBrowser->Refresh(); }
void BrowserHost::Stop()      { if (m_pWebBrowser) m_pWebBrowser->Stop(); }

void BrowserHost::Resize(const RECT& rc)
{
    if (m_pWebBrowser) {
        m_pWebBrowser->put_Left(rc.left);
        m_pWebBrowser->put_Top(rc.top);
        m_pWebBrowser->put_Width(rc.right - rc.left);
        m_pWebBrowser->put_Height(rc.bottom - rc.top);
    }
    if (m_pSite && m_pSite->m_lpInPlaceObject)
        m_pSite->m_lpInPlaceObject->SetObjectRects(&rc, &rc);
}

bool BrowserHost::TranslateAccelerator(MSG* msg)
{
    if (m_pIPActiveObj)
        return m_pIPActiveObj->TranslateAccelerator(msg) == S_OK;
    return false;
}

void BrowserHost::ConnectEvents()
{
    if (!m_pWebBrowser) return;
    IConnectionPointContainer* pCPC = nullptr;
    if (SUCCEEDED(m_pWebBrowser->QueryInterface(IID_IConnectionPointContainer,
                                                 reinterpret_cast<void**>(&pCPC)))) {
        IConnectionPoint* pCP = nullptr;
        if (SUCCEEDED(pCPC->FindConnectionPoint(DIID_DWebBrowserEvents2, &pCP))) {
            pCP->Advise(static_cast<IDispatch*>(m_pSite), &m_dwEventCookie);
            pCP->Release();
        }
        pCPC->Release();
    }
}

void BrowserHost::DisconnectEvents()
{
    if (!m_pWebBrowser || !m_dwEventCookie) return;
    IConnectionPointContainer* pCPC = nullptr;
    if (SUCCEEDED(m_pWebBrowser->QueryInterface(IID_IConnectionPointContainer,
                                                 reinterpret_cast<void**>(&pCPC)))) {
        IConnectionPoint* pCP = nullptr;
        if (SUCCEEDED(pCPC->FindConnectionPoint(DIID_DWebBrowserEvents2, &pCP))) {
            pCP->Unadvise(m_dwEventCookie);
            m_dwEventCookie = 0;
            pCP->Release();
        }
        pCPC->Release();
    }
}

void BrowserHost::Destroy()
{
    DisconnectEvents();

    if (m_pIPActiveObj) {
        m_pIPActiveObj->Release();
        m_pIPActiveObj = nullptr;
    }

    if (m_pOleObject) {
        if (m_pSite && m_pSite->m_lpInPlaceObject) {
            m_pSite->m_lpInPlaceObject->UIDeactivate();
            m_pSite->m_lpInPlaceObject->InPlaceDeactivate();
        }
        m_pOleObject->Close(OLECLOSE_NOSAVE);
        m_pOleObject->Release();
        m_pOleObject = nullptr;
    }

    if (m_pWebBrowser) {
        m_pWebBrowser->Release();
        m_pWebBrowser = nullptr;
    }

    if (m_pSite) {
        m_pSite->m_lpOleObject = nullptr;
        m_pSite->m_pBrowserHost = nullptr;
        m_pSite->Release();
        m_pSite = nullptr;
    }
}
