#include "browser.h"
#include "swf_mime_filter.h"
#include <mshtml.h>
#include <mshtmdid.h>
#include <cwchar>
#include <new>

enum class FocusTargetKind {
    None = -1,
    Background = 0,
    Text = 1,
    Interactive = 2,
};

class FlashFocusState {
public:
    ULONG AddRef()
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_ref));
    }

    ULONG Release()
    {
        ULONG ref = static_cast<ULONG>(InterlockedDecrement(&m_ref));
        if (!ref)
            delete this;
        return ref;
    }

    void RecordPointerDown(FocusTargetKind kind)
    {
        m_targetKind = kind;
        m_messageTime = static_cast<DWORD>(GetMessageTime());
    }

    void ClearPointer()
    {
        m_targetKind = FocusTargetKind::None;
        m_messageTime = 0;
    }

    bool ShouldCancelBackgroundDeactivate() const
    {
        return m_targetKind == FocusTargetKind::Background &&
               m_messageTime == static_cast<DWORD>(GetMessageTime()) &&
               (GetKeyState(VK_LBUTTON) & 0x8000) != 0;
    }

private:
    LONG m_ref = 1;
    FocusTargetKind m_targetKind = FocusTargetKind::None;
    DWORD m_messageTime = 0;
};

static IUnknown* GetVariantObject(const VARIANT& value)
{
    if (value.vt == VT_DISPATCH)
        return value.pdispVal;
    if (value.vt == VT_UNKNOWN)
        return value.punkVal;
    if (value.vt == (VT_DISPATCH | VT_BYREF) && value.ppdispVal)
        return *value.ppdispVal;
    if (value.vt == (VT_UNKNOWN | VT_BYREF) && value.ppunkVal)
        return *value.ppunkVal;
    return nullptr;
}

static IHTMLEventObj* GetHtmlEvent(DISPPARAMS* params)
{
    if (!params || params->cArgs < 1 || !params->rgvarg)
        return nullptr;

    IUnknown* object = GetVariantObject(params->rgvarg[0]);
    IHTMLEventObj* event = nullptr;
    if (object) {
        object->QueryInterface(
            IID_IHTMLEventObj, reinterpret_cast<void**>(&event));
    }
    return event;
}

static bool GetSpecifiedAttribute(
    IHTMLElement* element, const wchar_t* name, VARIANT* value)
{
    if (!element || !name || !value)
        return false;
    VariantInit(value);

    IHTMLDOMNode* node = nullptr;
    IDispatch* attributesDispatch = nullptr;
    IHTMLAttributeCollection* attributes = nullptr;
    IDispatch* attributeDispatch = nullptr;
    IHTMLDOMAttribute* attribute = nullptr;
    bool found = false;

    if (SUCCEEDED(element->QueryInterface(
            IID_IHTMLDOMNode, reinterpret_cast<void**>(&node))) && node &&
        SUCCEEDED(node->get_attributes(&attributesDispatch)) &&
        attributesDispatch &&
        SUCCEEDED(attributesDispatch->QueryInterface(
            IID_IHTMLAttributeCollection,
            reinterpret_cast<void**>(&attributes))) && attributes) {
        VARIANT attributeName;
        VariantInit(&attributeName);
        attributeName.vt = VT_BSTR;
        attributeName.bstrVal = SysAllocString(name);
        if (attributeName.bstrVal &&
            SUCCEEDED(attributes->item(
                &attributeName, &attributeDispatch)) &&
            attributeDispatch &&
            SUCCEEDED(attributeDispatch->QueryInterface(
                IID_IHTMLDOMAttribute,
                reinterpret_cast<void**>(&attribute))) && attribute) {
            VARIANT_BOOL specified = VARIANT_FALSE;
            if (SUCCEEDED(attribute->get_specified(&specified)) &&
                specified == VARIANT_TRUE &&
                SUCCEEDED(attribute->get_nodeValue(value))) {
                found = true;
            }
        }
        VariantClear(&attributeName);
    }

    if (attribute) attribute->Release();
    if (attributeDispatch) attributeDispatch->Release();
    if (attributes) attributes->Release();
    if (attributesDispatch) attributesDispatch->Release();
    if (node) node->Release();
    if (!found) {
        VariantClear(value);
        VariantInit(value);
    }
    return found;
}

static bool VariantToLong(const VARIANT& value, long* number)
{
    if (!number)
        return false;

    VARIANT converted;
    VariantInit(&converted);
    HRESULT hr = VariantChangeType(
        &converted, const_cast<VARIANT*>(&value), 0, VT_I4);
    if (SUCCEEDED(hr))
        *number = converted.lVal;
    VariantClear(&converted);
    return SUCCEEDED(hr);
}

static bool ContainsInsensitive(BSTR value, const wchar_t* needle)
{
    if (!value || !needle)
        return false;

    const size_t valueLength = SysStringLen(value);
    const size_t needleLength = wcslen(needle);
    if (!needleLength || needleLength > valueLength)
        return false;

    for (size_t i = 0; i + needleLength <= valueLength; ++i) {
        if (_wcsnicmp(value + i, needle, needleLength) == 0)
            return true;
    }
    return false;
}

static bool AttributeContains(
    IHTMLElement* element, const wchar_t* name, const wchar_t* needle)
{
    VARIANT value;
    if (!GetSpecifiedAttribute(element, name, &value))
        return false;

    VARIANT text;
    VariantInit(&text);
    HRESULT hr = VariantChangeType(&text, &value, 0, VT_BSTR);
    bool matches = SUCCEEDED(hr) && ContainsInsensitive(text.bstrVal, needle);
    VariantClear(&text);
    VariantClear(&value);
    return matches;
}

static bool IsFlashElement(IHTMLElement* element)
{
    if (!element)
        return false;

    BSTR tag = nullptr;
    element->get_tagName(&tag);
    bool isObject = tag && _wcsicmp(tag, L"OBJECT") == 0;
    bool isEmbed = tag && _wcsicmp(tag, L"EMBED") == 0;
    SysFreeString(tag);
    if (!isObject && !isEmbed)
        return false;

    static const wchar_t flashClsid[] =
        L"D27CDB6E-AE6D-11CF-96B8-444553540000";
    static const wchar_t flashMime[] =
        L"application/x-shockwave-flash";

    if (AttributeContains(element, L"classid", flashClsid) ||
        AttributeContains(element, L"type", flashMime)) {
        return true;
    }

    if (isObject) {
        IHTMLObjectElement* object = nullptr;
        if (SUCCEEDED(element->QueryInterface(
                IID_IHTMLObjectElement,
                reinterpret_cast<void**>(&object))) && object) {
            BSTR classid = nullptr;
            BSTR type = nullptr;
            object->get_classid(&classid);
            object->get_type(&type);
            bool isFlash = ContainsInsensitive(classid, flashClsid) ||
                           ContainsInsensitive(type, flashMime);
            SysFreeString(type);
            SysFreeString(classid);
            object->Release();
            return isFlash;
        }
    }
    return false;
}

static bool IsInteractiveTag(BSTR tag)
{
    if (!tag)
        return false;

    static const wchar_t* const tags[] = {
        L"A", L"AREA", L"INPUT", L"TEXTAREA", L"SELECT", L"BUTTON",
        L"OPTION", L"LABEL", L"IFRAME", L"FRAME", L"OBJECT", L"EMBED"
    };
    for (const wchar_t* candidate : tags) {
        if (_wcsicmp(tag, candidate) == 0)
            return true;
    }
    return false;
}

static bool IsContentEditable(IHTMLElement* element)
{
    IHTMLElement3* element3 = nullptr;
    if (FAILED(element->QueryInterface(
            IID_IHTMLElement3, reinterpret_cast<void**>(&element3))) ||
        !element3) {
        return false;
    }

    VARIANT_BOOL editable = VARIANT_FALSE;
    element3->get_isContentEditable(&editable);
    element3->Release();
    return editable == VARIANT_TRUE;
}

static bool HasExplicitTabIndex(IHTMLElement* element)
{
    VARIANT value;
    if (!GetSpecifiedAttribute(element, L"tabIndex", &value))
        return false;

    long tabIndex = -1;
    bool focusable = VariantToLong(value, &tabIndex) && tabIndex >= 0;
    VariantClear(&value);
    return focusable;
}

static bool HasInteractiveAncestor(IHTMLElement* element)
{
    IHTMLElement* current = element;
    current->AddRef();

    while (current) {
        BSTR tag = nullptr;
        current->get_tagName(&tag);
        bool interactive = IsInteractiveTag(tag) ||
                           IsContentEditable(current) ||
                           HasExplicitTabIndex(current);
        SysFreeString(tag);

        IHTMLElement* parent = nullptr;
        if (!interactive)
            current->get_parentElement(&parent);
        current->Release();
        if (interactive) {
            if (parent) parent->Release();
            return true;
        }
        current = parent;
    }
    return false;
}

static bool PointIntersectsElementText(
    IHTMLElement* element, long clientX, long clientY)
{
    IDispatch* documentDispatch = nullptr;
    IHTMLDocument2* document = nullptr;
    IHTMLElement* body = nullptr;
    IHTMLBodyElement* bodyElement = nullptr;
    IHTMLTxtRange* range = nullptr;
    IHTMLTextRangeMetrics2* metrics = nullptr;
    IHTMLRectCollection* rects = nullptr;
    bool hit = false;

    if (SUCCEEDED(element->get_document(&documentDispatch)) &&
        documentDispatch &&
        SUCCEEDED(documentDispatch->QueryInterface(
            IID_IHTMLDocument2, reinterpret_cast<void**>(&document))) &&
        document && SUCCEEDED(document->get_body(&body)) && body &&
        SUCCEEDED(body->QueryInterface(
            IID_IHTMLBodyElement,
            reinterpret_cast<void**>(&bodyElement))) && bodyElement &&
        SUCCEEDED(bodyElement->createTextRange(&range)) && range &&
        SUCCEEDED(range->moveToElementText(element)) &&
        SUCCEEDED(range->QueryInterface(
            IID_IHTMLTextRangeMetrics2,
            reinterpret_cast<void**>(&metrics))) && metrics &&
        SUCCEEDED(metrics->getClientRects(&rects)) && rects) {
        long count = 0;
        if (SUCCEEDED(rects->get_length(&count))) {
            for (long i = 0; i < count && !hit; ++i) {
                VARIANT index;
                VARIANT value;
                VariantInit(&index);
                VariantInit(&value);
                index.vt = VT_I4;
                index.lVal = i;
                if (SUCCEEDED(rects->item(&index, &value))) {
                    IUnknown* object = GetVariantObject(value);
                    IHTMLRect* rect = nullptr;
                    if (object && SUCCEEDED(object->QueryInterface(
                            IID_IHTMLRect,
                            reinterpret_cast<void**>(&rect))) && rect) {
                        long left = 0;
                        long top = 0;
                        long right = 0;
                        long bottom = 0;
                        if (SUCCEEDED(rect->get_left(&left)) &&
                            SUCCEEDED(rect->get_top(&top)) &&
                            SUCCEEDED(rect->get_right(&right)) &&
                            SUCCEEDED(rect->get_bottom(&bottom))) {
                            hit = clientX >= left && clientX <= right &&
                                  clientY >= top && clientY <= bottom;
                        }
                        rect->Release();
                    }
                }
                VariantClear(&value);
            }
        }
    }

    if (rects) rects->Release();
    if (metrics) metrics->Release();
    if (range) range->Release();
    if (bodyElement) bodyElement->Release();
    if (body) body->Release();
    if (document) document->Release();
    if (documentDispatch) documentDispatch->Release();
    return hit;
}

static FocusTargetKind ClassifyPointerTarget(IHTMLEventObj* event)
{
    IHTMLElement* source = nullptr;
    if (!event || FAILED(event->get_srcElement(&source)) || !source)
        return FocusTargetKind::None;

    if (HasInteractiveAncestor(source)) {
        source->Release();
        return FocusTargetKind::Interactive;
    }

    long clientX = 0;
    long clientY = 0;
    bool hasPoint = SUCCEEDED(event->get_clientX(&clientX)) &&
                    SUCCEEDED(event->get_clientY(&clientY));
    bool hitsText = hasPoint &&
                    PointIntersectsElementText(source, clientX, clientY);
    source->Release();
    return hitsText ? FocusTargetKind::Text : FocusTargetKind::Background;
}

class HtmlDocumentEventSink final : public IDispatch {
public:
    explicit HtmlDocumentEventSink(FlashFocusState* state) : m_state(state)
    {
        m_state->AddRef();
    }

    ~HtmlDocumentEventSink()
    {
        m_state->Release();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDispatch ||
            riid == DIID_HTMLDocumentEvents2) {
            *ppv = static_cast<IDispatch*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
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

    STDMETHODIMP GetTypeInfoCount(UINT* count) override
    {
        if (!count)
            return E_POINTER;
        *count = 0;
        return S_OK;
    }

    STDMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo**) override
    {
        return E_NOTIMPL;
    }

    STDMETHODIMP GetIDsOfNames(
        REFIID, OLECHAR**, UINT, LCID, DISPID*) override
    {
        return DISP_E_UNKNOWNNAME;
    }

    STDMETHODIMP Invoke(
        DISPID dispid, REFIID, LCID, WORD, DISPPARAMS* params,
        VARIANT* result, EXCEPINFO*, UINT*) override
    {
        if (dispid == DISPID_HTMLDOCUMENTEVENTS2_ONMOUSEUP) {
            m_state->ClearPointer();
            return S_OK;
        }

        IHTMLEventObj* event = GetHtmlEvent(params);
        if (dispid == DISPID_HTMLDOCUMENTEVENTS2_ONMOUSEDOWN) {
            long button = 0;
            if (event && SUCCEEDED(event->get_button(&button)) && button == 1)
                m_state->RecordPointerDown(ClassifyPointerTarget(event));
            else
                m_state->ClearPointer();
            if (event) event->Release();
            return S_OK;
        }

        if (dispid == DISPID_HTMLDOCUMENTEVENTS2_ONBEFOREDEACTIVATE) {
            bool cancel = false;
            IHTMLElement* source = nullptr;
            if (event && SUCCEEDED(event->get_srcElement(&source)) && source) {
                cancel = IsFlashElement(source) &&
                         m_state->ShouldCancelBackgroundDeactivate();
                source->Release();
            }

            if (result) {
                VariantInit(result);
                result->vt = VT_BOOL;
                result->boolVal = cancel ? VARIANT_FALSE : VARIANT_TRUE;
            }
            if (cancel && event) {
                VARIANT returnValue;
                VariantInit(&returnValue);
                returnValue.vt = VT_BOOL;
                returnValue.boolVal = VARIANT_FALSE;
                event->put_returnValue(returnValue);
            }
            if (event) event->Release();
            return S_OK;
        }

        if (event) event->Release();
        return S_OK;
    }

private:
    LONG m_ref = 1;
    FlashFocusState* m_state;
};

static void ConnectDocumentFocusEvents(
    IDispatch* browserDispatch, FlashFocusState* state)
{
    if (!browserDispatch || !state)
        return;

    IWebBrowser2* frameBrowser = nullptr;
    IDispatch* documentDispatch = nullptr;
    IHTMLDocument2* document = nullptr;
    IConnectionPointContainer* container = nullptr;
    IConnectionPoint* connection = nullptr;

    if (SUCCEEDED(browserDispatch->QueryInterface(
            IID_IWebBrowser2, reinterpret_cast<void**>(&frameBrowser))) &&
        frameBrowser &&
        SUCCEEDED(frameBrowser->get_Document(&documentDispatch)) &&
        documentDispatch &&
        SUCCEEDED(documentDispatch->QueryInterface(
            IID_IHTMLDocument2, reinterpret_cast<void**>(&document))) &&
        document &&
        SUCCEEDED(document->QueryInterface(
            IID_IConnectionPointContainer,
            reinterpret_cast<void**>(&container))) && container &&
        SUCCEEDED(container->FindConnectionPoint(
            DIID_HTMLDocumentEvents2, &connection)) && connection) {
        HtmlDocumentEventSink* sink =
            new (std::nothrow) HtmlDocumentEventSink(state);
        if (sink) {
            DWORD cookie = 0;
            connection->Advise(static_cast<IDispatch*>(sink), &cookie);
            // The document owns the advised sink. The sink intentionally does
            // not retain the document or connection point, avoiding a cycle.
            sink->Release();
        }
    }

    if (connection) connection->Release();
    if (container) container->Release();
    if (document) document->Release();
    if (documentDispatch) documentDispatch->Release();
    if (frameBrowser) frameBrowser->Release();
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
    if (m_pSite->m_lpInPlaceObject) {
        m_pSite->m_lpInPlaceObject->Release();
        m_pSite->m_lpInPlaceObject = nullptr;
    }
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

COleInPlaceFrame::~COleInPlaceFrame()
{
    if (m_pActiveObject) {
        m_pActiveObject->Release();
        m_pActiveObject = nullptr;
    }
}

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

STDMETHODIMP COleInPlaceFrame::SetActiveObject(
    IOleInPlaceActiveObject* activeObject, LPCOLESTR)
{
    if (activeObject)
        activeObject->AddRef();
    if (m_pActiveObject)
        m_pActiveObject->Release();
    m_pActiveObject = activeObject;
    return S_OK;
}

IOleInPlaceActiveObject* COleInPlaceFrame::AcquireActiveObject()
{
    if (m_pActiveObject)
        m_pActiveObject->AddRef();
    return m_pActiveObject;
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
        // Subscribe natively in every completed frame. A shared input state is
        // required because Flash commonly lives in a cross-origin iframe while
        // the background click arrives through its parent document.
        if (pDispParams->cArgs >= 2 && m_pBrowserHost &&
            m_pBrowserHost->m_pFlashFocusState) {
            IUnknown* frame = GetVariantObject(pDispParams->rgvarg[1]);
            IDispatch* frameDispatch = nullptr;
            if (frame && SUCCEEDED(frame->QueryInterface(
                    IID_IDispatch,
                    reinterpret_cast<void**>(&frameDispatch))) &&
                frameDispatch) {
                ConnectDocumentFocusEvents(
                    frameDispatch, m_pBrowserHost->m_pFlashFocusState);
                frameDispatch->Release();
            }
        }
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
    if (!m_pFlashFocusState)
        m_pFlashFocusState = new (std::nothrow) FlashFocusState();

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

    RefreshBrowserWindow();

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
    if (!msg || msg->message < WM_KEYFIRST || msg->message > WM_KEYLAST)
        return false;

    HWND inputWindow = msg->hwnd ? msg->hwnd : GetFocus();
    if (!IsBrowserInputWindow(inputWindow))
        return false;

    IOleInPlaceActiveObject* activeObject = AcquireActiveObject();
    if (!activeObject)
        return false;

    HRESULT hr = activeObject->TranslateAccelerator(msg);
    activeObject->Release();
    return hr == S_OK;
}

void BrowserHost::OnFrameWindowActivate(bool active)
{
    IOleInPlaceActiveObject* activeObject = AcquireActiveObject();
    if (!activeObject)
        return;

    activeObject->OnFrameWindowActivate(active ? TRUE : FALSE);
    activeObject->Release();
}

IOleInPlaceActiveObject* BrowserHost::AcquireActiveObject()
{
    IOleInPlaceActiveObject* activeObject = nullptr;
    if (m_pSite && m_pSite->m_pInPlaceFrame)
        activeObject = m_pSite->m_pInPlaceFrame->AcquireActiveObject();
    if (!activeObject && m_pIPActiveObj) {
        m_pIPActiveObj->AddRef();
        activeObject = m_pIPActiveObj;
    }
    return activeObject;
}

void BrowserHost::RefreshBrowserWindow()
{
    m_hwndBrowser = nullptr;

    HWND activeWindow = nullptr;
    if (!m_pIPActiveObj ||
        FAILED(m_pIPActiveObj->GetWindow(&activeWindow)) || !activeWindow) {
        return;
    }

    HWND hostWindow = m_pSite ? m_pSite->m_hWnd : nullptr;
    while (activeWindow && activeWindow != hostWindow) {
        HWND parentWindow = GetParent(activeWindow);
        if (parentWindow == hostWindow) {
            m_hwndBrowser = activeWindow;
            return;
        }
        activeWindow = parentWindow;
    }
}

bool BrowserHost::IsBrowserInputWindow(HWND hwnd)
{
    if (!hwnd || !m_pSite)
        return false;

    // Windowless controls can leave keyboard focus on the container HWND.
    if (hwnd == m_pSite->m_hWnd)
        return true;

    if (!m_hwndBrowser || !IsWindow(m_hwndBrowser))
        RefreshBrowserWindow();

    if (m_hwndBrowser &&
        (hwnd == m_hwndBrowser || IsChild(m_hwndBrowser, hwnd))) {
        return true;
    }

    // The WebBrowser can replace its direct child window while navigating.
    RefreshBrowserWindow();
    return m_hwndBrowser &&
           (hwnd == m_hwndBrowser || IsChild(m_hwndBrowser, hwnd));
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

    if (m_pFlashFocusState) {
        m_pFlashFocusState->Release();
        m_pFlashFocusState = nullptr;
    }
}
