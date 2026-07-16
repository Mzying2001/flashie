#include "browser.h"
#include "swf_mime_filter.h"
#include <mshtml.h>

static void InstallFlashFocusGuard(IDispatch* browserDispatch)
{
    IWebBrowser2* frameBrowser = nullptr;
    if (!browserDispatch || FAILED(browserDispatch->QueryInterface(
            IID_IWebBrowser2, reinterpret_cast<void**>(&frameBrowser))))
        return;

    IDispatch* documentDispatch = nullptr;
    frameBrowser->get_Document(&documentDispatch);
    frameBrowser->Release();
    if (!documentDispatch)
        return;

    IHTMLDocument2* document = nullptr;
    documentDispatch->QueryInterface(
        IID_IHTMLDocument2, reinterpret_cast<void**>(&document));
    documentDispatch->Release();
    if (!document)
        return;

    IHTMLWindow2* window = nullptr;
    document->get_parentWindow(&window);
    document->Release();
    if (!window)
        return;

    // Flash dispatches Stage deactivation before MSHTML reaches the OLE
    // UIDeactivate callback, so preserve focus at the cancelable DOM boundary.
    BSTR code = SysAllocString(
        L"(function(){"
        L"if(window.__flashieFocusGuard)return;"
        L"window.__flashieFocusGuard=true;"
        L"document.attachEvent('onbeforedeactivate',function(){"
        L"var e=window.event,s=e&&e.srcElement;"
        L"if(!s||!s.tagName)return;"
        L"var t=String(s.tagName).toUpperCase();"
        L"if(t!=='OBJECT'&&t!=='EMBED')return;"
        L"var c=String(s.classid||s.getAttribute('classid')||'').toUpperCase();"
        L"var m=String(s.type||s.getAttribute('type')||'').toLowerCase();"
        L"if(c.indexOf('D27CDB6E-AE6D-11CF-96B8-444553540000')<0&&"
        L"m!=='application/x-shockwave-flash')return;"
        L"for(var n=e.toElement;n&&n.tagName;n=n.parentNode){"
        L"var q=String(n.tagName).toUpperCase();"
        L"if(q==='INPUT'||q==='TEXTAREA'||q==='SELECT'||q==='BUTTON'||"
        L"n.isContentEditable)return;"
        L"}"
        L"e.returnValue=false;"
        L"});"
        L"})();");
    BSTR language = SysAllocString(L"javascript");
    if (!code || !language) {
        SysFreeString(language);
        SysFreeString(code);
        window->Release();
        return;
    }
    VARIANT result;
    VariantInit(&result);
    window->execScript(code, language, &result);
    VariantClear(&result);
    SysFreeString(language);
    SysFreeString(code);
    window->Release();
}

static bool IsTopLevelBrowserEvent(
    IDispatch* eventDispatch, IWebBrowser2* browser)
{
    IUnknown* eventIdentity = nullptr;
    IUnknown* browserIdentity = nullptr;
    if (eventDispatch)
        eventDispatch->QueryInterface(
            IID_IUnknown, reinterpret_cast<void**>(&eventIdentity));
    if (browser)
        browser->QueryInterface(
            IID_IUnknown, reinterpret_cast<void**>(&browserIdentity));

    bool isTopLevel = eventIdentity && eventIdentity == browserIdentity;
    if (eventIdentity) eventIdentity->Release();
    if (browserIdentity) browserIdentity->Release();
    return isTopLevel;
}

static const wchar_t* GetVariantString(VARIANT* value)
{
    if (!value)
        return nullptr;
    if (value->vt == (VT_VARIANT | VT_BYREF))
        value = value->pvarVal;
    if (!value)
        return nullptr;
    if (value->vt == VT_BSTR)
        return value->bstrVal;
    if (value->vt == (VT_BSTR | VT_BYREF) && value->pbstrVal)
        return *value->pbstrVal;
    return nullptr;
}

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

    *lprcPosRect = m_pSite->m_rcPos;
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
    } else if (riid == IID_IOleCommandTarget) {
        *ppv = static_cast<IOleCommandTarget*>(this);
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

// IOleCommandTarget — suppress script error dialogs
static const GUID CGID_DocHostCommandHandler =
    {0xf38bc242, 0xb950, 0x11d1, {0x89, 0x18, 0x00, 0xc0, 0x4f, 0xc2, 0xc8, 0x36}};

STDMETHODIMP COleSite::QueryStatus(const GUID*, ULONG, OLECMD[], OLECMDTEXT*)
{
    return E_NOTIMPL;
}

STDMETHODIMP COleSite::Exec(const GUID* pguidCmdGroup, DWORD nCmdID, DWORD,
                             VARIANT*, VARIANT* pvaOut)
{
    // OLECMDID_SHOWSCRIPTERROR = 40
    if (pguidCmdGroup && IsEqualGUID(*pguidCmdGroup, CGID_DocHostCommandHandler) && nCmdID == 40) {
        // Set pvaOut to VARIANT_TRUE to continue running scripts (suppress dialog)
        if (pvaOut) {
            pvaOut->vt = VT_BOOL;
            pvaOut->boolVal = VARIANT_TRUE;
        }
        return S_OK;
    }
    return OLECMDERR_E_NOTSUPPORTED;
}

// IServiceProvider / IInternetSecurityManager
STDMETHODIMP COleSite::QueryService(
    REFGUID guidService, REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;

    if (guidService == SID_SInternetSecurityManager &&
        riid == IID_IInternetSecurityManager) {
        return QueryInterface(riid, ppv);
    }
    return E_NOINTERFACE;
}

STDMETHODIMP COleSite::SetSecuritySite(IInternetSecurityMgrSite*)
{
    return INET_E_DEFAULT_ACTION;
}

STDMETHODIMP COleSite::GetSecuritySite(IInternetSecurityMgrSite**)
{
    return INET_E_DEFAULT_ACTION;
}

STDMETHODIMP COleSite::MapUrlToZone(LPCWSTR, DWORD*, DWORD)
{
    return INET_E_DEFAULT_ACTION;
}

STDMETHODIMP COleSite::GetSecurityId(LPCWSTR, BYTE*, DWORD*, DWORD_PTR)
{
    return INET_E_DEFAULT_ACTION;
}

STDMETHODIMP COleSite::ProcessUrlAction(
    LPCWSTR, DWORD action, BYTE* policy, DWORD policySize,
    BYTE*, DWORD, DWORD, DWORD)
{
    if (action != URLACTION_HTML_MIXED_CONTENT)
        return INET_E_DEFAULT_ACTION;
    if (!policy)
        return E_POINTER;
    if (policySize < sizeof(DWORD))
        return E_INVALIDARG;

    // Display insecure subresources without IE's mixed-content prompt.
    const DWORD allow = URLPOLICY_ALLOW;
    CopyMemory(policy, &allow, sizeof(allow));
    return S_OK;
}

STDMETHODIMP COleSite::QueryCustomPolicy(
    LPCWSTR, REFGUID, BYTE**, DWORD*, BYTE*, DWORD, DWORD)
{
    return INET_E_DEFAULT_ACTION;
}

STDMETHODIMP COleSite::SetZoneMapping(DWORD, LPCWSTR, DWORD)
{
    return INET_E_DEFAULT_ACTION;
}

STDMETHODIMP COleSite::GetZoneMappings(DWORD, IEnumString**, DWORD)
{
    return INET_E_DEFAULT_ACTION;
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

    case DISPID_DOWNLOADBEGIN:
        if (m_pBrowserHost && m_pBrowserHost->m_loadingStateCallback) {
            m_pBrowserHost->m_loadingStateCallback(
                true, m_pBrowserHost->m_loadingStateCtx);
        }
        return S_OK;

    case DISPID_DOWNLOADCOMPLETE:
        if (m_pBrowserHost && m_pBrowserHost->m_loadingStateCallback) {
            m_pBrowserHost->m_loadingStateCallback(
                false, m_pBrowserHost->m_loadingStateCtx);
        }
        return S_OK;

    case DISPID_BEFORENAVIGATE2: {
        // Params (reverse): URL=[5], pDisp=[6]. Arm the process-local MIME
        // filter for a top-level SWF but let the original navigation proceed.
        if (pDispParams->cArgs >= 7 && m_pBrowserHost) {
            IDispatch* eventDispatch = pDispParams->rgvarg[6].pdispVal;
            if (IsTopLevelBrowserEvent(
                    eventDispatch, m_pBrowserHost->m_pWebBrowser)) {
                const wchar_t* url = GetVariantString(
                    &pDispParams->rgvarg[5]);
                if (SwfMimeFilter::IsSupportedSwfUrl(url))
                    SwfMimeFilter::Arm(url);
                else
                    SwfMimeFilter::Cancel();
            }
        }
        return S_OK;
    }

    case DISPID_NAVIGATECOMPLETE2: {
        // rgvarg[1] = pDisp (IDispatch of the frame), rgvarg[0] = URL
        if (pDispParams->cArgs >= 2 && m_pBrowserHost) {
            // Only process top-level frame navigations
            IDispatch* pEventDisp = pDispParams->rgvarg[1].pdispVal;
            bool isTopLevel = IsTopLevelBrowserEvent(
                pEventDisp, m_pBrowserHost->m_pWebBrowser);

            if (isTopLevel && SwfMimeFilter::IsArmed())
                SwfMimeFilter::Cancel();

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

    case DISPID_DOCUMENTCOMPLETE:
        if (pDispParams->cArgs >= 2)
            InstallFlashFocusGuard(pDispParams->rgvarg[1].pdispVal);
        return S_OK;

    case DISPID_NAVIGATEERROR: {
        // Params (reverse): Cancel=[0], StatusCode=[1], Frame=[2],
        // URL=[3], pDisp=[4]. A failed top-level navigation must not leave
        // the one-shot MIME filter armed for an unrelated future request.
        if (pDispParams->cArgs >= 5 && m_pBrowserHost &&
            IsTopLevelBrowserEvent(
                pDispParams->rgvarg[4].pdispVal,
                m_pBrowserHost->m_pWebBrowser)) {
            SwfMimeFilter::Cancel();
        }
        return S_OK;
    }

    case DISPID_FILEDOWNLOAD:
        // If URLMon elected to download, it did not consume our handler.
        // Keep the native dialog as a fallback, but clear pending state.
        if (SwfMimeFilter::IsArmed())
            SwfMimeFilter::Cancel();
        return S_OK;

    case DISPID_TITLECHANGE: {
        if (pDispParams->cArgs >= 1 && m_pBrowserHost && m_pBrowserHost->m_titleCallback) {
            if (pDispParams->rgvarg[0].vt == VT_BSTR)
                m_pBrowserHost->m_titleCallback(pDispParams->rgvarg[0].bstrVal,
                                                 m_pBrowserHost->m_titleCtx);
        }
        return S_OK;
    }

    case DISPID_STATUSTEXTCHANGE: {
        if (pDispParams->cArgs >= 1 && m_pBrowserHost &&
            m_pBrowserHost->m_statusTextCallback) {
            VARIANT* pText = &pDispParams->rgvarg[0];
            if (pText->vt == (VT_VARIANT | VT_BYREF))
                pText = pText->pvarVal;

            if (pText->vt == VT_BSTR) {
                m_pBrowserHost->m_statusTextCallback(
                    pText->bstrVal ? pText->bstrVal : L"",
                    m_pBrowserHost->m_statusTextCtx);
            } else if (pText->vt == (VT_BSTR | VT_BYREF) && pText->pbstrVal) {
                m_pBrowserHost->m_statusTextCallback(
                    *pText->pbstrVal ? *pText->pbstrVal : L"",
                    m_pBrowserHost->m_statusTextCtx);
            }
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

    default:
        break;
    }

    // Handle ambient properties that MSHTML queries on the host.
    // DISPID_AMBIENT_DLCONTROL (-5512): download-control flags.
    // Returning DLCTL_DLIMAGES|VIDEOS|BGSOUNDS and NOT setting
    // DLCTL_NO_RUNACTIVEXCTLS allows ActiveX controls to auto-run.
    if (dispid == -5512 && pvarResult) { // DISPID_AMBIENT_DLCONTROL
        pvarResult->vt = VT_I4;
        // DLCTL_DLIMAGES|DLCTL_VIDEOS|DLCTL_BGSOUNDS | DLCTL_NO_DLACTIVEXCTLS
        // Allow content but block ActiveX CAB downloads (we have Flash locally).
        pvarResult->lVal = 0x00000470;
        return S_OK;
    }

    return DISP_E_MEMBERNOTFOUND;
}

// ===============================================================
// BrowserHost
// ===============================================================

bool BrowserHost::Initialize(HWND hwndParent, const RECT& rc)
{
    m_swfMimeFilterInitialized = SwfMimeFilter::Initialize();

    m_pSite = new COleSite();
    m_pSite->m_hWnd = hwndParent;
    m_pSite->m_rcPos = rc;
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

    HWND hwndActive = nullptr;
    if (m_pIPActiveObj && SUCCEEDED(m_pIPActiveObj->GetWindow(&hwndActive))) {
        while (hwndActive) {
            HWND hwndParentWindow = GetParent(hwndActive);
            if (hwndParentWindow == hwndParent) {
                m_hwndBrowser = hwndActive;
                break;
            }
            hwndActive = hwndParentWindow;
        }
    }

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

    SwfMimeFilter::Cancel();

    VARIANT vURL;
    VariantInit(&vURL);
    vURL.vt = VT_BSTR;
    vURL.bstrVal = SysAllocString(url);
    VARIANT vEmpty;
    VariantInit(&vEmpty);
    m_pWebBrowser->Navigate2(&vURL, &vEmpty, &vEmpty, &vEmpty, &vEmpty);
    VariantClear(&vURL);
}

void BrowserHost::GoBack()
{
    SwfMimeFilter::Cancel();
    if (m_pWebBrowser) m_pWebBrowser->GoBack();
}

void BrowserHost::GoForward()
{
    SwfMimeFilter::Cancel();
    if (m_pWebBrowser) m_pWebBrowser->GoForward();
}

void BrowserHost::Refresh()
{
    if (!m_pWebBrowser)
        return;

    BSTR location = nullptr;
    if (SUCCEEDED(m_pWebBrowser->get_LocationURL(&location)) && location &&
        SwfMimeFilter::IsSupportedSwfUrl(location)) {
        SwfMimeFilter::Arm(location);
    } else {
        SwfMimeFilter::Cancel();
    }
    SysFreeString(location);
    m_pWebBrowser->Refresh();
}

void BrowserHost::Stop()
{
    SwfMimeFilter::Cancel();
    if (m_pWebBrowser) m_pWebBrowser->Stop();
}

void BrowserHost::Resize(const RECT& rc)
{
    if (m_pSite)
        m_pSite->m_rcPos = rc;

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
    SwfMimeFilter::Cancel();
    m_hwndBrowser = nullptr;
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

    if (m_swfMimeFilterInitialized) {
        SwfMimeFilter::Shutdown();
        m_swfMimeFilterInitialized = false;
    }
}
