#pragma once
#include <windows.h>
#include <ole2.h>
#include <oleidl.h>
#include <oaidl.h>
#include <exdisp.h>
#include <exdispid.h>
#include <mshtmhst.h>

class COleSite;
class BrowserHost;

// ---------------------------------------------------------------
// COleClientSite
// ---------------------------------------------------------------
class COleClientSite : public IOleClientSite {
public:
    explicit COleClientSite(COleSite* pSite) : m_pSite(pSite) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP SaveObject() override { return E_NOTIMPL; }
    STDMETHODIMP GetMoniker(DWORD, DWORD, IMoniker**) override { return E_NOTIMPL; }
    STDMETHODIMP GetContainer(IOleContainer**) override { return E_NOINTERFACE; }
    STDMETHODIMP ShowObject() override { return S_OK; }
    STDMETHODIMP OnShowWindow(BOOL) override { return S_OK; }
    STDMETHODIMP RequestNewObjectLayout() override { return E_NOTIMPL; }
private:
    COleSite* m_pSite;
    ULONG m_ref = 0;
};

// ---------------------------------------------------------------
// COleInPlaceSite
// ---------------------------------------------------------------
class COleInPlaceSite : public IOleInPlaceSite {
public:
    explicit COleInPlaceSite(COleSite* pSite) : m_pSite(pSite) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP GetWindow(HWND* phwnd) override;
    STDMETHODIMP ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP CanInPlaceActivate() override { return S_OK; }
    STDMETHODIMP OnInPlaceActivate() override;
    STDMETHODIMP OnUIActivate() override { return S_OK; }
    STDMETHODIMP GetWindowContext(IOleInPlaceFrame**, IOleInPlaceUIWindow**,
                                  LPRECT, LPRECT, LPOLEINPLACEFRAMEINFO) override;
    STDMETHODIMP Scroll(SIZE) override { return E_NOTIMPL; }
    STDMETHODIMP OnUIDeactivate(BOOL) override { return S_OK; }
    STDMETHODIMP OnInPlaceDeactivate() override;
    STDMETHODIMP DiscardUndoState() override { return E_NOTIMPL; }
    STDMETHODIMP DeactivateAndUndo() override { return E_NOTIMPL; }
    STDMETHODIMP OnPosRectChange(LPCRECT) override { return S_OK; }
private:
    COleSite* m_pSite;
    ULONG m_ref = 0;
};

// ---------------------------------------------------------------
// COleInPlaceFrame
// ---------------------------------------------------------------
class COleInPlaceFrame : public IOleInPlaceFrame {
public:
    explicit COleInPlaceFrame(COleSite* pSite) : m_pSite(pSite) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP GetWindow(HWND* phwnd) override;
    STDMETHODIMP ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP GetBorder(LPRECT) override { return INPLACE_E_NOTOOLSPACE; }
    STDMETHODIMP RequestBorderSpace(LPCBORDERWIDTHS) override { return INPLACE_E_NOTOOLSPACE; }
    STDMETHODIMP SetBorderSpace(LPCBORDERWIDTHS) override { return S_OK; }
    STDMETHODIMP SetActiveObject(IOleInPlaceActiveObject*, LPCOLESTR) override { return S_OK; }
    STDMETHODIMP InsertMenus(HMENU, LPOLEMENUGROUPWIDTHS) override { return S_OK; }
    STDMETHODIMP SetMenu(HMENU, HOLEMENU, HWND) override { return S_OK; }
    STDMETHODIMP RemoveMenus(HMENU) override { return S_OK; }
    STDMETHODIMP SetStatusText(LPCOLESTR) override { return S_OK; }
    STDMETHODIMP EnableModeless(BOOL) override { return S_OK; }
    STDMETHODIMP TranslateAccelerator(LPMSG, WORD) override { return S_FALSE; }
private:
    COleSite* m_pSite;
    ULONG m_ref = 0;
};

// ---------------------------------------------------------------
// COleSite - main COM identity
// ---------------------------------------------------------------
class COleSite : public IDocHostUIHandler,
                 public IDispatch
{
    friend class COleClientSite;
    friend class COleInPlaceSite;
    friend class COleInPlaceFrame;
    friend class BrowserHost;

public:
    COleSite();
    ~COleSite();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IDocHostUIHandler
    STDMETHODIMP ShowContextMenu(DWORD, POINT*, IUnknown*, IDispatch*) override;
    STDMETHODIMP GetHostInfo(DOCHOSTUIINFO*) override;
    STDMETHODIMP ShowUI(DWORD, IOleInPlaceActiveObject*, IOleCommandTarget*,
                        IOleInPlaceFrame*, IOleInPlaceUIWindow*) override { return S_FALSE; }
    STDMETHODIMP HideUI() override { return S_OK; }
    STDMETHODIMP UpdateUI() override { return S_OK; }
    STDMETHODIMP EnableModeless(BOOL) override { return S_OK; }
    STDMETHODIMP OnDocWindowActivate(BOOL) override { return S_OK; }
    STDMETHODIMP OnFrameWindowActivate(BOOL) override { return S_OK; }
    STDMETHODIMP ResizeBorder(LPCRECT, IOleInPlaceUIWindow*, BOOL) override { return S_OK; }
    STDMETHODIMP TranslateAccelerator(LPMSG, const GUID*, DWORD) override { return S_FALSE; }
    STDMETHODIMP GetOptionKeyPath(BSTR*, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP GetDropTarget(IDropTarget*, IDropTarget**) override { return E_NOTIMPL; }
    STDMETHODIMP GetExternal(IDispatch**) override { return E_NOTIMPL; }
    STDMETHODIMP TranslateUrl(DWORD, OLECHAR*, OLECHAR**) override { return S_FALSE; }
    STDMETHODIMP FilterDataObject(IDataObject*, IDataObject**) override { return S_FALSE; }

    // IDispatch (DWebBrowserEvents2 sink)
    STDMETHODIMP GetTypeInfoCount(UINT*) override;
    STDMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo**) override;
    STDMETHODIMP GetIDsOfNames(REFIID, OLECHAR**, UINT, LCID, DISPID*) override;
    STDMETHODIMP Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*,
                        VARIANT*, EXCEPINFO*, UINT*) override;


private:
    ULONG              m_ref = 1;
    HWND               m_hWnd = nullptr;
    IStorage*          m_lpStorage = nullptr;
    IOleObject*        m_lpOleObject = nullptr;
    IOleInPlaceObject* m_lpInPlaceObject = nullptr;
    COleClientSite*    m_pClientSite = nullptr;
    COleInPlaceSite*   m_pInPlaceSite = nullptr;
    COleInPlaceFrame*  m_pInPlaceFrame = nullptr;
    BrowserHost*       m_pBrowserHost = nullptr;
};

// ---------------------------------------------------------------
// BrowserHost - high-level API
// ---------------------------------------------------------------
using NavigateCompleteCallback = void(*)(const wchar_t* url, void* ctx);
using TitleChangeCallback = void(*)(const wchar_t* title, void* ctx);

class BrowserHost {
    friend class COleSite;
public:
    BrowserHost() = default;
    ~BrowserHost() { Destroy(); }

    bool Initialize(HWND hwndParent, const RECT& rc);
    void Navigate(const wchar_t* url);
    void GoBack();
    void GoForward();
    void Refresh();
    void Stop();
    void Resize(const RECT& rc);
    bool TranslateAccelerator(MSG* msg);
    void Destroy();

    void SetNavigateCompleteCallback(NavigateCompleteCallback cb, void* ctx) {
        m_navCallback = cb; m_navCtx = ctx;
    }
    void SetTitleChangeCallback(TitleChangeCallback cb, void* ctx) {
        m_titleCallback = cb; m_titleCtx = ctx;
    }

    IWebBrowser2* GetWebBrowser() { return m_pWebBrowser; }

private:
    COleSite*                m_pSite = nullptr;
    IWebBrowser2*            m_pWebBrowser = nullptr;
    IOleObject*              m_pOleObject = nullptr;
    IOleInPlaceActiveObject* m_pIPActiveObj = nullptr;
    DWORD                    m_dwEventCookie = 0;

    NavigateCompleteCallback m_navCallback = nullptr;
    void*                    m_navCtx = nullptr;
    TitleChangeCallback      m_titleCallback = nullptr;
    void*                    m_titleCtx = nullptr;

    void ConnectEvents();
    void DisconnectEvents();
};
