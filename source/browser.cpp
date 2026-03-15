#include "browser.h"
#include "debug.h"

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
        *ppv = static_cast<IDocHostUIHandler*>(this);
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
    } else if (riid == IID_IInternetHostSecurityManager) {
        *ppv = static_cast<IInternetHostSecurityManager*>(this);
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

// IDocHostUIHandler
STDMETHODIMP COleSite::ShowContextMenu(DWORD, POINT*, IUnknown*, IDispatch*)
{
    return E_NOTIMPL; // let the control handle it
}

STDMETHODIMP COleSite::GetHostInfo(DOCHOSTUIINFO* pInfo)
{
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
    switch (dispid) {

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
        // Cancel the new window — NewWindow3 handles navigation on IE8+.
        if (pDispParams->cArgs >= 2) {
            if (pDispParams->rgvarg[0].vt == (VT_BOOL | VT_BYREF))
                *pDispParams->rgvarg[0].pboolVal = VARIANT_TRUE;
        }
        return S_OK;
    }

    case DISPID_NEWWINDOW3: {
        // Params (reverse): rgvarg[0]=bstrUrl, [1]=bstrUrlContext, [2]=dwFlags, [3]=Cancel, [4]=ppDisp
        if (pDispParams->cArgs >= 5 && m_pBrowserHost) {
            if (pDispParams->rgvarg[3].vt == (VT_BOOL | VT_BYREF))
                *pDispParams->rgvarg[3].pboolVal = VARIANT_TRUE;
            if (pDispParams->rgvarg[0].vt == VT_BSTR && pDispParams->rgvarg[0].bstrVal)
                m_pBrowserHost->Navigate(pDispParams->rgvarg[0].bstrVal);
        }
        return S_OK;
    }

    case DISPID_BEFORENAVIGATE2: {
        // Block Flash-detection redirects that replace the game iframe with
        // "install Flash" pages. Win10 MSHTML blocks JS->Flash IDispatch,
        // causing detection scripts (flashopen_cpp.js) to think Flash is missing.
        // The game is actually loaded and running — just block the redirect.
        if (pDispParams->cArgs >= 7) {
            VARIANT* pURL = &pDispParams->rgvarg[5];
            if (pURL->vt == (VT_VARIANT | VT_BYREF))
                pURL = pURL->pvarVal;
            if (pURL && pURL->vt == VT_BSTR && pURL->bstrVal) {
                if (wcsstr(pURL->bstrVal, L"noInstallFlash") ||
                    wcsstr(pURL->bstrVal, L"blockflashtip")) {
                    VARIANT* pCancel = &pDispParams->rgvarg[0];
                    if (pCancel->vt == (VT_BOOL | VT_BYREF))
                        *pCancel->pboolVal = VARIANT_TRUE;
                    DbgTrace(L"[FlashIE] BLOCKED Flash-block redirect: %s\n",
                             pURL->bstrVal);
                }
            }
        }
        return S_OK;
    }

    default:
        break;
    }
    return DISP_E_MEMBERNOTFOUND;
}

// IServiceProvider
STDMETHODIMP COleSite::QueryService(REFGUID guidService, REFIID riid, void** ppv)
{
    DbgTrace(L"[FlashIE] COleSite::QueryService srv={%08X-...} riid={%08X-...}\n",
             guidService.Data1, riid.Data1);
    if (guidService == IID_IInternetHostSecurityManager)
        return QueryInterface(riid, ppv);
    *ppv = nullptr;
    return E_NOINTERFACE;
}

// IInternetHostSecurityManager
STDMETHODIMP COleSite::GetSecurityId(BYTE*, DWORD* pcbSecurityId, DWORD_PTR)
{
    if (pcbSecurityId) *pcbSecurityId = 0;
    return S_OK;
}

STDMETHODIMP COleSite::ProcessUrlAction(DWORD dwAction, BYTE* pPolicy, DWORD cbPolicy,
                                         BYTE*, DWORD, DWORD, DWORD)
{
    DbgTrace(L"[FlashIE] COleSite::ProcessUrlAction action=0x%X\n", dwAction);
    if (pPolicy && cbPolicy >= sizeof(DWORD))
        *(DWORD*)pPolicy = URLPOLICY_ALLOW;
    return S_OK;
}

STDMETHODIMP COleSite::QueryCustomPolicy(REFGUID, BYTE**, DWORD*, BYTE*, DWORD, DWORD)
{
    return INET_E_DEFAULT_ACTION;
}

// ===============================================================
// BrowserHost
// ===============================================================

bool BrowserHost::Initialize(HWND hwndParent, const RECT& rc)
{
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

    m_pWebBrowser->put_Silent(VARIANT_TRUE);

    m_pWebBrowser->put_Left(rc.left);
    m_pWebBrowser->put_Top(rc.top);
    m_pWebBrowser->put_Width(rc.right - rc.left);
    m_pWebBrowser->put_Height(rc.bottom - rc.top);

    return true;
}

void BrowserHost::Navigate(const wchar_t* url)
{
    if (!m_pWebBrowser) return;

    VARIANT vURL;
    VariantInit(&vURL);
    vURL.vt = VT_BSTR;
    vURL.bstrVal = SysAllocString(url);
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
