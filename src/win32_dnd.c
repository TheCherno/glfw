//========================================================================
// GLFW 3.x Win32 - native drag and drop
//------------------------------------------------------------------------
// Implements glfwStartDragDrop / glfwSetDragDropIcon /
// glfwSetDragDropPayload on Windows using OLE (DoDragDrop + IDropSource +
// IDataObject, and an IDropTarget on every window). Mirrors the Cocoa and
// Wayland backends: by default no payload is transferred, the session
// exists only so the OS routes hover/drop events
// (GLFW_DRAGDROP_ENTER/MOTION/LEAVE/DROP) to whichever of the app's own
// windows the cursor is over, plus a single GLFW_DRAGDROP end. A session
// given formats by glfwSetDragDropPayload is a real inter-application drag
// on top of that, and only that kind lets a foreign window take the drop.
//
// Unlike those backends, DoDragDrop runs a blocking modal loop, so the
// session is armed by startDragDrop and only entered from the window
// procedure on the next mouse message -- between frames -- so it never
// re-enters the caller's in-progress render. The drag image is a layered
// window we move ourselves (instead of IDragSourceHelper) so it can be set
// or changed at any point in the session.
//========================================================================

#include "internal.h"

#if defined(_GLFW_WIN32)

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <shellapi.h>   // HDROP and DROPFILES, for file drags in either direction

// OLE drag-and-drop (Ole32), the interface IIDs (Uuid), and DragQueryFile (Shell32).
#if defined(_MSC_VER)
 #pragma comment(lib, "ole32.lib")
 #pragma comment(lib, "uuid.lib")
 #pragma comment(lib, "shell32.lib")
#endif

#define GLFW_DRAG_ICON_CLASS L"GLFW_DragIcon"

// Private clipboard format that marks one of our drag sessions, so a drop
// target can tell it apart from an external file drag. Registered at init.
static UINT   g_dragFormat;
static ATOM   g_iconClass;
static HWND   g_iconWindow;   // layered drag image; NULL until setDragDropIcon
static int    g_iconHotX, g_iconHotY;
static DWORD  g_pendingStartTick;   // when startDragDrop armed, to bound the icon wait

// How long to let the drag image (rendered from a normal frame's offscreen capture) get staged
// before entering DoDragDrop anyway. Entering earlier would start the blocking modal loop before
// the capture completes, leaving the preview to the timer pump, which is starved while the mouse
// moves -- so the preview often never appears.
//
// The wait ends the moment the icon is staged, so this cap only bites when it never arrives; the
// usual cost is the two frames the caller needs, not this number. Budget for two slow ones: the
// capture can't be read back until the frame after the one that enqueues its render, and those
// first frames are the expensive ones (allocating the texture, then a GPU-to-host copy). At 80ms
// a 60fps pair only just fit, so any hitch lost the preview for the whole gesture.
#define GLFW_DRAGDROP_ICON_WAIT_MS 250

//========================================================================
// IEnumFORMATETC over the formats a session offers
//========================================================================

typedef struct
{
    IEnumFORMATETC  iface;
    LONG            refs;
    UINT*           formats;   // owned snapshot: payload formats, then our session marker
    ULONG           count;
    ULONG           index;     // how many have been read
} FormatEnum;

static FormatEnum* createFormatEnum(const UINT* formats, ULONG count, ULONG index);

static HRESULT STDMETHODCALLTYPE FormatEnum_QueryInterface(IEnumFORMATETC* self, REFIID riid, void** ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IEnumFORMATETC))
    {
        *ppv = self;
        self->lpVtbl->AddRef(self);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE FormatEnum_AddRef(IEnumFORMATETC* self)
{
    FormatEnum* e = (FormatEnum*) self;
    return InterlockedIncrement(&e->refs);
}

static ULONG STDMETHODCALLTYPE FormatEnum_Release(IEnumFORMATETC* self)
{
    FormatEnum* e = (FormatEnum*) self;
    const LONG refs = InterlockedDecrement(&e->refs);
    if (refs == 0)
    {
        _glfw_free(e->formats);
        _glfw_free(e);
    }
    return refs;
}

static HRESULT STDMETHODCALLTYPE FormatEnum_Next(IEnumFORMATETC* self, ULONG celt,
                                                 FORMATETC* rgelt, ULONG* pceltFetched)
{
    FormatEnum* e = (FormatEnum*) self;
    ULONG fetched = 0;
    while (fetched < celt && e->index < e->count)
    {
        FORMATETC fmt;
        fmt.cfFormat = (CLIPFORMAT) e->formats[e->index];
        fmt.ptd      = NULL;
        fmt.dwAspect = DVASPECT_CONTENT;
        fmt.lindex   = -1;
        fmt.tymed    = TYMED_HGLOBAL;
        rgelt[fetched] = fmt;
        fetched++;
        e->index++;
    }
    if (pceltFetched)
        *pceltFetched = fetched;
    return (fetched == celt) ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE FormatEnum_Skip(IEnumFORMATETC* self, ULONG celt)
{
    FormatEnum* e = (FormatEnum*) self;
    if (e->index + celt > e->count)
    {
        e->index = e->count;
        return S_FALSE;
    }
    e->index += celt;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FormatEnum_Reset(IEnumFORMATETC* self)
{
    ((FormatEnum*) self)->index = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FormatEnum_Clone(IEnumFORMATETC* self, IEnumFORMATETC** ppenum)
{
    FormatEnum* e = (FormatEnum*) self;
    FormatEnum* clone = createFormatEnum(e->formats, e->count, e->index);
    if (!clone)
        return E_OUTOFMEMORY;
    *ppenum = &clone->iface;
    return S_OK;
}

static IEnumFORMATETCVtbl g_formatEnumVtbl =
{
    FormatEnum_QueryInterface,
    FormatEnum_AddRef,
    FormatEnum_Release,
    FormatEnum_Next,
    FormatEnum_Skip,
    FormatEnum_Reset,
    FormatEnum_Clone
};

static FormatEnum* createFormatEnum(const UINT* formats, ULONG count, ULONG index)
{
    FormatEnum* e = _glfw_calloc(1, sizeof(FormatEnum));
    if (!e)
        return NULL;
    e->formats = _glfw_calloc(count ? count : 1, sizeof(UINT));
    if (!e->formats)
    {
        _glfw_free(e);
        return NULL;
    }
    memcpy(e->formats, formats, count * sizeof(UINT));
    e->iface.lpVtbl = &g_formatEnumVtbl;
    e->refs = 1;
    e->count = count;
    e->index = index;
    return e;
}

//========================================================================
// IDataObject (the session marker, plus whatever payload formats it offers)
//========================================================================

typedef struct
{
    IDataObject iface;
    LONG        refs;
    // Copied, not borrowed from the session: a target may hold this object past DoDragDrop.
    _GLFWdragpayloadWin32* payloads;
    int         payloadCount;
} DataObject;

static const _GLFWdragpayloadWin32* findPayload(DataObject* o, CLIPFORMAT format)
{
    for (int i = 0;  i < o->payloadCount;  i++)
    {
        if (o->payloads[i].format == (UINT) format)
            return &o->payloads[i];
    }
    return NULL;
}

static HRESULT STDMETHODCALLTYPE DataObject_QueryInterface(IDataObject* self, REFIID riid, void** ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDataObject))
    {
        *ppv = self;
        self->lpVtbl->AddRef(self);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE DataObject_AddRef(IDataObject* self)
{
    return InterlockedIncrement(&((DataObject*) self)->refs);
}

static ULONG STDMETHODCALLTYPE DataObject_Release(IDataObject* self)
{
    DataObject* o = (DataObject*) self;
    const LONG refs = InterlockedDecrement(&o->refs);
    if (refs == 0)
    {
        for (int i = 0;  i < o->payloadCount;  i++)
            _glfw_free(o->payloads[i].bytes);
        _glfw_free(o->payloads);
        _glfw_free(o);
    }
    return refs;
}

static HRESULT STDMETHODCALLTYPE DataObject_GetData(IDataObject* self, FORMATETC* fmt, STGMEDIUM* med)
{
    DataObject* o = (DataObject*) self;
    if (!fmt || !med)
        return E_INVALIDARG;

    // The session marker itself carries no data; our own windows read the drag from memory.
    const _GLFWdragpayloadWin32* payload = findPayload(o, fmt->cfFormat);
    if (!payload || !(fmt->tymed & TYMED_HGLOBAL))
        return DV_E_FORMATETC;

    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, payload->size);
    if (!handle)
        return E_OUTOFMEMORY;
    void* bytes = GlobalLock(handle);
    if (!bytes)
    {
        GlobalFree(handle);
        return E_OUTOFMEMORY;
    }
    memcpy(bytes, payload->bytes, payload->size);
    GlobalUnlock(handle);

    ZeroMemory(med, sizeof(*med));
    med->tymed = TYMED_HGLOBAL;
    med->hGlobal = handle;
    med->pUnkForRelease = NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE DataObject_GetDataHere(IDataObject* self, FORMATETC* fmt, STGMEDIUM* med)
{
    return DV_E_FORMATETC;
}

static HRESULT STDMETHODCALLTYPE DataObject_QueryGetData(IDataObject* self, FORMATETC* fmt)
{
    DataObject* o = (DataObject*) self;
    if (!fmt || !(fmt->tymed & TYMED_HGLOBAL))
        return DV_E_FORMATETC;
    if (fmt->cfFormat == (CLIPFORMAT) g_dragFormat || findPayload(o, fmt->cfFormat))
        return S_OK;
    return DV_E_FORMATETC;
}

static HRESULT STDMETHODCALLTYPE DataObject_GetCanonicalFormatEtc(IDataObject* self,
                                                                  FORMATETC* in, FORMATETC* out)
{
    if (out)
        out->ptd = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE DataObject_SetData(IDataObject* self, FORMATETC* fmt,
                                                    STGMEDIUM* med, BOOL release)
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE DataObject_EnumFormatEtc(IDataObject* self, DWORD dir,
                                                          IEnumFORMATETC** ppenum)
{
    DataObject* o = (DataObject*) self;
    if (dir != DATADIR_GET)
        return E_NOTIMPL;
    if (!ppenum)
        return E_INVALIDARG;

    // Payloads first: a target taking the first format it recognizes should land on real data
    // rather than on the marker only this application understands.
    const ULONG count = (ULONG) o->payloadCount + 1;
    UINT* formats = _glfw_calloc(count, sizeof(UINT));
    if (!formats)
        return E_OUTOFMEMORY;
    for (int i = 0;  i < o->payloadCount;  i++)
        formats[i] = o->payloads[i].format;
    formats[count - 1] = g_dragFormat;

    FormatEnum* e = createFormatEnum(formats, count, 0);
    _glfw_free(formats);
    if (!e)
        return E_OUTOFMEMORY;
    *ppenum = &e->iface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE DataObject_DAdvise(IDataObject* self, FORMATETC* fmt, DWORD advf,
                                                    IAdviseSink* sink, DWORD* conn)
{
    return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE DataObject_DUnadvise(IDataObject* self, DWORD conn)
{
    return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE DataObject_EnumDAdvise(IDataObject* self, IEnumSTATDATA** ppenum)
{
    return OLE_E_ADVISENOTSUPPORTED;
}

static IDataObjectVtbl g_dataObjectVtbl =
{
    DataObject_QueryInterface,
    DataObject_AddRef,
    DataObject_Release,
    DataObject_GetData,
    DataObject_GetDataHere,
    DataObject_QueryGetData,
    DataObject_GetCanonicalFormatEtc,
    DataObject_SetData,
    DataObject_EnumFormatEtc,
    DataObject_DAdvise,
    DataObject_DUnadvise,
    DataObject_EnumDAdvise
};

static IDataObject* createDataObject(void)
{
    DataObject* o = _glfw_calloc(1, sizeof(DataObject));
    if (!o)
        return NULL;
    o->iface.lpVtbl = &g_dataObjectVtbl;
    o->refs = 1;

    const int count = _glfw.win32.dragDropSession.payloadCount;
    if (count > 0)
    {
        o->payloads = _glfw_calloc((size_t) count, sizeof(_GLFWdragpayloadWin32));
        if (!o->payloads)
        {
            _glfw_free(o);
            return NULL;
        }
        for (int i = 0;  i < count;  i++)
        {
            const _GLFWdragpayloadWin32* src = &_glfw.win32.dragDropSession.payloads[i];
            void* bytes = _glfw_calloc(src->size ? src->size : 1, 1);
            if (!bytes)
                break;   // offer what did fit; the drag still routes between our own windows
            memcpy(bytes, src->bytes, src->size);
            o->payloads[o->payloadCount].format = src->format;
            o->payloads[o->payloadCount].bytes = bytes;
            o->payloads[o->payloadCount].size = src->size;
            o->payloadCount++;
        }
    }
    return &o->iface;
}

//========================================================================
// IDropSource
//========================================================================

typedef struct
{
    IDropSource iface;
    LONG        refs;
} DropSource;

static HRESULT STDMETHODCALLTYPE DropSource_QueryInterface(IDropSource* self, REFIID riid, void** ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropSource))
    {
        *ppv = self;
        self->lpVtbl->AddRef(self);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE DropSource_AddRef(IDropSource* self)
{
    return InterlockedIncrement(&((DropSource*) self)->refs);
}

static ULONG STDMETHODCALLTYPE DropSource_Release(IDropSource* self)
{
    DropSource* s = (DropSource*) self;
    const LONG refs = InterlockedDecrement(&s->refs);
    if (refs == 0)
        _glfw_free(s);
    return refs;
}

// True when this session offers something a foreign application could actually take.
static GLFWbool sessionIsExternal(void)
{
    return _glfw.win32.dragDropSession.payloadCount > 0 ? GLFW_TRUE : GLFW_FALSE;
}

// What a drop on one of our own windows reports; an effect the source never allowed isn't a
// valid answer, and an external session allows COPY only (see the DoDragDrop call).
static DWORD ownDropEffect(void)
{
    return sessionIsExternal() ? DROPEFFECT_COPY : DROPEFFECT_MOVE;
}

// True when the window under the cursor belongs to this process. The drag image is
// WS_EX_TRANSPARENT, so the hit test looks straight through it.
static GLFWbool cursorOverOwnWindow(void)
{
    POINT cursor;
    if (!GetCursorPos(&cursor))
        return GLFW_FALSE;

    const HWND hovered = WindowFromPoint(cursor);
    if (!hovered)
        return GLFW_FALSE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hovered, &pid);
    return pid == GetCurrentProcessId() ? GLFW_TRUE : GLFW_FALSE;
}

static HRESULT STDMETHODCALLTYPE DropSource_QueryContinueDrag(IDropSource* self, BOOL escape, DWORD keys)
{
    if (escape)
        return DRAGDROP_S_CANCEL;
    // The drag was started with the left button held; releasing it drops.
    if (!(keys & MK_LBUTTON))
    {
        // Dropping on a foreign window raises it, so only do that if we offer it something.
        return (cursorOverOwnWindow() || sessionIsExternal())
            ? DRAGDROP_S_DROP : DRAGDROP_S_CANCEL;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE DropSource_GiveFeedback(IDropSource* self, DWORD effect)
{
    // Keep the layered drag image glued to the cursor, even over the desktop
    // where no drop target fires DragOver. DoDragDrop calls this on every move.
    if (g_iconWindow)
    {
        POINT cursor;
        GetCursorPos(&cursor);
        SetWindowPos(g_iconWindow, HWND_TOPMOST,
                     cursor.x - g_iconHotX, cursor.y - g_iconHotY, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
    }
    // We draw our own image; let OLE supply the standard drag cursors underneath.
    return DRAGDROP_S_USEDEFAULTCURSORS;
}

static IDropSourceVtbl g_dropSourceVtbl =
{
    DropSource_QueryInterface,
    DropSource_AddRef,
    DropSource_Release,
    DropSource_QueryContinueDrag,
    DropSource_GiveFeedback
};

static IDropSource* createDropSource(void)
{
    DropSource* s = _glfw_calloc(1, sizeof(DropSource));
    if (!s)
        return NULL;
    s->iface.lpVtbl = &g_dropSourceVtbl;
    s->refs = 1;
    return &s->iface;
}

//========================================================================
// IDropTarget (one per window)
//========================================================================

typedef struct
{
    IDropTarget  iface;
    LONG         refs;
    _GLFWwindow* window;
} DropTarget;

// True while the current OLE drag is one of our own sessions.
static GLFWbool dataObjectIsOurs(IDataObject* data)
{
    FORMATETC fmt;
    fmt.cfFormat = (CLIPFORMAT) g_dragFormat;
    fmt.ptd      = NULL;
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex   = -1;
    fmt.tymed    = TYMED_HGLOBAL;
    return (data && data->lpVtbl->QueryGetData(data, &fmt) == S_OK) ? GLFW_TRUE : GLFW_FALSE;
}

// True if the drag carries dropped files (external Explorer drag).
static GLFWbool dataObjectHasFiles(IDataObject* data)
{
    FORMATETC fmt;
    fmt.cfFormat = CF_HDROP;
    fmt.ptd      = NULL;
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex   = -1;
    fmt.tymed    = TYMED_HGLOBAL;
    return (data && data->lpVtbl->QueryGetData(data, &fmt) == S_OK) ? GLFW_TRUE : GLFW_FALSE;
}

// Screen POINTL -> this window's client coordinates (GLFW cursor convention).
static void toClient(_GLFWwindow* window, POINTL pt, double* x, double* y)
{
    POINT p = { pt.x, pt.y };
    ScreenToClient(window->win32.handle, &p);
    *x = (double) p.x;
    *y = (double) p.y;
}

static HRESULT STDMETHODCALLTYPE DropTarget_QueryInterface(IDropTarget* self, REFIID riid, void** ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropTarget))
    {
        *ppv = self;
        self->lpVtbl->AddRef(self);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE DropTarget_AddRef(IDropTarget* self)
{
    return InterlockedIncrement(&((DropTarget*) self)->refs);
}

static ULONG STDMETHODCALLTYPE DropTarget_Release(IDropTarget* self)
{
    DropTarget* t = (DropTarget*) self;
    const LONG refs = InterlockedDecrement(&t->refs);
    if (refs == 0)
        _glfw_free(t);
    return refs;
}

static HRESULT STDMETHODCALLTYPE DropTarget_DragEnter(IDropTarget* self, IDataObject* data,
                                                      DWORD keys, POINTL pt, DWORD* effect)
{
    DropTarget* t = (DropTarget*) self;
    if (dataObjectIsOurs(data))
    {
        double x, y;
        toClient(t->window, pt, &x, &y);
        _glfw.win32.dragDropSession.hoverWindow = t->window;
        _glfwInputDragDrop(t->window, GLFW_DRAGDROP_ENTER, x, y,
                           _glfw.win32.dragDropSession.type);
        *effect = ownDropEffect();
    }
    else if (dataObjectHasFiles(data))
        *effect = DROPEFFECT_COPY;
    else
        *effect = DROPEFFECT_NONE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE DropTarget_DragOver(IDropTarget* self, DWORD keys, POINTL pt, DWORD* effect)
{
    DropTarget* t = (DropTarget*) self;
    if (_glfw.win32.dragDropSession.active &&
        _glfw.win32.dragDropSession.hoverWindow == t->window)
    {
        double x, y;
        toClient(t->window, pt, &x, &y);
        _glfwInputDragDrop(t->window, GLFW_DRAGDROP_MOTION, x, y,
                           _glfw.win32.dragDropSession.type);
        *effect = ownDropEffect();
    }
    else if (_glfw.win32.dragDropSession.active)
        *effect = ownDropEffect();
    else
        *effect = DROPEFFECT_COPY;   // external file drag
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE DropTarget_DragLeave(IDropTarget* self)
{
    DropTarget* t = (DropTarget*) self;
    if (_glfw.win32.dragDropSession.active &&
        _glfw.win32.dragDropSession.hoverWindow == t->window)
    {
        _glfwInputDragDrop(t->window, GLFW_DRAGDROP_LEAVE, 0.0, 0.0,
                           _glfw.win32.dragDropSession.type);
        _glfw.win32.dragDropSession.hoverWindow = NULL;
    }
    return S_OK;
}

static void dropExternalFiles(_GLFWwindow* window, IDataObject* data);

static HRESULT STDMETHODCALLTYPE DropTarget_Drop(IDropTarget* self, IDataObject* data,
                                                 DWORD keys, POINTL pt, DWORD* effect)
{
    DropTarget* t = (DropTarget*) self;
    if (_glfw.win32.dragDropSession.active && dataObjectIsOurs(data))
    {
        double x, y;
        toClient(t->window, pt, &x, &y);
        _glfw.win32.dragDropSession.dropReceived = GLFW_TRUE;
        _glfwInputDragDrop(t->window, GLFW_DRAGDROP_DROP, x, y,
                           _glfw.win32.dragDropSession.type);
        _glfw.win32.dragDropSession.hoverWindow = NULL;
        *effect = ownDropEffect();
    }
    else if (dataObjectHasFiles(data))
    {
        dropExternalFiles(t->window, data);
        *effect = DROPEFFECT_COPY;
    }
    else
        *effect = DROPEFFECT_NONE;
    return S_OK;
}

static IDropTargetVtbl g_dropTargetVtbl =
{
    DropTarget_QueryInterface,
    DropTarget_AddRef,
    DropTarget_Release,
    DropTarget_DragEnter,
    DropTarget_DragOver,
    DropTarget_DragLeave,
    DropTarget_Drop
};

// Delivers an external Explorer file drop through the legacy path so drops keep
// working now that OLE (RegisterDragDrop) supersedes WM_DROPFILES on our windows.
static void dropExternalFiles(_GLFWwindow* window, IDataObject* data)
{
    FORMATETC fmt = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM med;
    if (data->lpVtbl->GetData(data, &fmt, &med) != S_OK)
        return;

    HDROP drop = (HDROP) GlobalLock(med.hGlobal);
    if (drop)
    {
        const UINT count = DragQueryFileW(drop, 0xffffffff, NULL, 0);
        char** paths = _glfw_calloc(count, sizeof(char*));
        for (UINT i = 0;  i < count;  i++)
        {
            const UINT length = DragQueryFileW(drop, i, NULL, 0);
            WCHAR* buffer = _glfw_calloc((size_t) length + 1, sizeof(WCHAR));
            DragQueryFileW(drop, i, buffer, length + 1);
            paths[i] = _glfwCreateUTF8FromWideStringWin32(buffer);
            _glfw_free(buffer);
        }
        _glfwInputDrop(window, (int) count, (const char**) paths);
        for (UINT i = 0;  i < count;  i++)
            _glfw_free(paths[i]);
        _glfw_free(paths);
        GlobalUnlock(med.hGlobal);
    }
    ReleaseStgMedium(&med);
}

//========================================================================
// Drag image (layered window)
//========================================================================

static LRESULT CALLBACK iconWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

static void destroyIconWindow(void)
{
    if (g_iconWindow)
    {
        DestroyWindow(g_iconWindow);
        g_iconWindow = NULL;
    }
}

//========================================================================
// External payloads (glfwSetDragDropPayload)
//========================================================================

static void freeSessionPayloads(void)
{
    for (int i = 0;  i < _glfw.win32.dragDropSession.payloadCount;  i++)
        _glfw_free(_glfw.win32.dragDropSession.payloads[i].bytes);
    _glfw_free(_glfw.win32.dragDropSession.payloads);
    _glfw.win32.dragDropSession.payloads = NULL;
    _glfw.win32.dragDropSession.payloadCount = 0;
}

// Percent-decodes length bytes of URI text into out, which must hold at least that many.
static size_t decodeURI(const char* uri, size_t length, char* out)
{
    size_t written = 0;
    for (size_t i = 0;  i < length;  i++)
    {
        if (uri[i] == '%' && i + 2 < length &&
            isxdigit((unsigned char) uri[i + 1]) && isxdigit((unsigned char) uri[i + 2]))
        {
            const char hex[3] = { uri[i + 1], uri[i + 2], '\0' };
            out[written++] = (char) strtol(hex, NULL, 16);
            i += 2;
        }
        else
            out[written++] = uri[i];
    }
    return written;
}

// One URI-list line to a native path, or NULL for a line that names no local file. A line
// carrying a bare path instead of a URI is taken as-is, which costs nothing and spares callers
// on this platform from percent-encoding a path they already hold.
static char* uriLineToPath(const char* line, size_t length)
{
    char* decoded = _glfw_calloc(length + 1, 1);
    if (!decoded)
        return NULL;
    decoded[decodeURI(line, length, decoded)] = '\0';

    char* path = decoded;
    if (strncmp(path, "file:", 5) == 0)
    {
        path += 5;
        if (strncmp(path, "//", 2) == 0)
        {
            path += 2;
            if (*path == '/')
                path++;              // empty authority, the usual file:///C:/x
            else if (*path)
            {
                // A real host makes this a UNC share, and "file://" left the two characters
                // its own backslashes need.
                path -= 2;
                path[0] = '\\';
                path[1] = '\\';
            }
        }
    }
    else if (strstr(path, "://"))
    {
        _glfw_free(decoded);         // some other scheme; nothing to hand a file drop target
        return NULL;
    }

    for (char* c = path;  *c;  c++)
    {
        if (*c == '/')
            *c = '\\';
    }

    if (!*path)
    {
        _glfw_free(decoded);
        return NULL;
    }

    // path can point into the middle of the allocation; hand back a string the caller can free.
    memmove(decoded, path, strlen(path) + 1);
    return decoded;
}

// RFC 2483 URI list to the CF_HDROP byte image: a DROPFILES header, then a double-null-
// terminated list of wide paths. NULL if the list named no local file.
static void* renderFileList(const char* text, size_t size, size_t* outSize)
{
    WCHAR* paths = NULL;
    size_t used = 0, capacity = 0, start = 0;

    for (size_t i = 0;  i <= size;  i++)
    {
        if (i != size && text[i] != '\r' && text[i] != '\n')
            continue;

        const char* line = text + start;
        size_t length = i - start;
        start = i + 1;

        while (length && (*line == ' ' || *line == '\t'))
        {
            line++;
            length--;
        }
        while (length && (line[length - 1] == ' ' || line[length - 1] == '\t'))
            length--;
        if (!length || *line == '#')
            continue;

        char* path = uriLineToPath(line, length);
        if (!path)
            continue;
        WCHAR* wide = _glfwCreateWideStringFromUTF8Win32(path);
        _glfw_free(path);
        if (!wide)
            continue;

        const size_t count = wcslen(wide) + 1;
        if (used + count + 1 > capacity)
        {
            const size_t grown = (capacity ? capacity * 2 : 256) + count + 1;
            WCHAR* bigger = _glfw_realloc(paths, grown * sizeof(WCHAR));
            if (!bigger)
            {
                _glfw_free(wide);
                _glfw_free(paths);
                return NULL;
            }
            paths = bigger;
            capacity = grown;
        }
        memcpy(paths + used, wide, count * sizeof(WCHAR));
        used += count;
        _glfw_free(wide);
    }

    if (!used)
    {
        _glfw_free(paths);
        return NULL;
    }
    paths[used++] = L'\0';   // the list's own terminator, after the last path's

    const size_t bytes = sizeof(DROPFILES) + used * sizeof(WCHAR);
    char* blob = _glfw_calloc(bytes, 1);
    if (blob)
    {
        DROPFILES* header = (DROPFILES*) blob;
        header->pFiles = sizeof(DROPFILES);
        header->fWide = TRUE;
        memcpy(blob + sizeof(DROPFILES), paths, used * sizeof(WCHAR));
        *outSize = bytes;
    }
    _glfw_free(paths);
    return blob;
}

// UTF-8 to the NUL-terminated UTF-16 CF_UNICODETEXT expects. The payload itself need not be
// terminated, hence the explicit length.
static void* renderText(const char* text, size_t size, size_t* outSize)
{
    int length = 0;
    if (size)
    {
        length = MultiByteToWideChar(CP_UTF8, 0, text, (int) size, NULL, 0);
        if (length <= 0)
            return NULL;
    }

    WCHAR* wide = _glfw_calloc((size_t) length + 1, sizeof(WCHAR));
    if (!wide)
        return NULL;
    if (size)
        MultiByteToWideChar(CP_UTF8, 0, text, (int) size, wide, length);
    wide[length] = L'\0';
    *outSize = ((size_t) length + 1) * sizeof(WCHAR);
    return wide;
}

// Renders a MIME payload into the byte image its clipboard format transfers, returning that
// format, or 0 if the content is malformed for its type.
static UINT renderPayload(const char* mime, const void* data, size_t size,
                          void** bytes, size_t* bytesSize)
{
    if (strcmp(mime, "text/uri-list") == 0)
    {
        *bytes = renderFileList((const char*) data, size, bytesSize);
        return *bytes ? CF_HDROP : 0;
    }
    if (strncmp(mime, "text/plain", 10) == 0)
    {
        *bytes = renderText((const char*) data, size, bytesSize);
        return *bytes ? CF_UNICODETEXT : 0;
    }

    // Anything else travels verbatim under its own name, for an application-specific format a
    // cooperating application already knows how to read.
    WCHAR* wideMime = _glfwCreateWideStringFromUTF8Win32(mime);
    const UINT format = wideMime ? RegisterClipboardFormatW(wideMime) : 0;
    _glfw_free(wideMime);
    if (!format)
        return 0;

    void* copy = _glfw_calloc(size ? size : 1, 1);
    if (!copy)
        return 0;
    if (size)
        memcpy(copy, data, size);
    *bytes = copy;
    *bytesSize = size;
    return format;
}

//========================================================================
// Public entry points
//========================================================================

GLFWbool _glfwInitDragDropWin32(void)
{
    if (FAILED(OleInitialize(NULL)))
        return GLFW_FALSE;

    g_dragFormat = RegisterClipboardFormatW(L"application/x-glfw-dragdrop");

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = iconWindowProc;
    wc.hInstance     = _glfw.win32.instance;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = GLFW_DRAG_ICON_CLASS;
    g_iconClass = RegisterClassExW(&wc);
    return GLFW_TRUE;
}

void _glfwTerminateDragDropWin32(void)
{
    destroyIconWindow();
    if (g_iconClass)
    {
        UnregisterClassW(GLFW_DRAG_ICON_CLASS, _glfw.win32.instance);
        g_iconClass = 0;
    }
    OleUninitialize();
}

void _glfwRegisterDropTargetWin32(_GLFWwindow* window)
{
    if (!g_dragFormat || window->win32.dropTarget)
        return;

    DropTarget* target = _glfw_calloc(1, sizeof(DropTarget));
    if (!target)
        return;
    target->iface.lpVtbl = &g_dropTargetVtbl;
    target->refs = 1;
    target->window = window;

    if (SUCCEEDED(RegisterDragDrop(window->win32.handle, &target->iface)))
        window->win32.dropTarget = &target->iface;
    else
        target->iface.lpVtbl->Release(&target->iface);
}

void _glfwRevokeDropTargetWin32(_GLFWwindow* window)
{
    if (window->win32.dropTarget)
    {
        RevokeDragDrop(window->win32.handle);
        window->win32.dropTarget->lpVtbl->Release(window->win32.dropTarget);
        window->win32.dropTarget = NULL;
    }
}

GLFWbool _glfwStartDragDropWin32(_GLFWwindow* window, const char* type)
{
    if (!g_dragFormat)
        return GLFW_FALSE;
    // One session at a time.
    if (_glfw.win32.dragDropSession.pending || _glfw.win32.dragDropSession.active)
        return GLFW_FALSE;
    // A held button is what DoDragDrop tracks; without one there's nothing to drag.
    if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0)
        return GLFW_FALSE;

    _glfw.win32.dragDropSession.window = window;
    _glfw.win32.dragDropSession.type = _glfw_strdup(type);
    _glfw.win32.dragDropSession.pending = GLFW_TRUE;
    _glfw.win32.dragDropSession.active = GLFW_FALSE;
    _glfw.win32.dragDropSession.dropReceived = GLFW_FALSE;
    _glfw.win32.dragDropSession.hoverWindow = NULL;
    _glfw.win32.dragDropSession.payloads = NULL;
    _glfw.win32.dragDropSession.payloadCount = 0;
    g_pendingStartTick = GetTickCount();
    return GLFW_TRUE;
}

void _glfwEnterPendingDragDropWin32(_GLFWwindow* window)
{
    if (!_glfw.win32.dragDropSession.pending)
        return;
    if (_glfw.win32.dragDropSession.window != window)
        return;

    // Give the drag image a chance to be staged from a normal (non-pump) frame's capture before
    // the blocking modal loop starts. Enter immediately once it's ready, or after the timeout so a
    // drag with no preview isn't held up.
    if (!g_iconWindow && (GetTickCount() - g_pendingStartTick) < GLFW_DRAGDROP_ICON_WAIT_MS)
        return;

    IDataObject* data = createDataObject();
    IDropSource* source = createDropSource();
    if (!data || !source)
    {
        if (data)   data->lpVtbl->Release(data);
        if (source) source->lpVtbl->Release(source);
        _glfw_free(_glfw.win32.dragDropSession.type);
        _glfw.win32.dragDropSession.type = NULL;
        _glfw.win32.dragDropSession.pending = GLFW_FALSE;
        freeSessionPayloads();
        return;
    }

    _glfw.win32.dragDropSession.pending = GLFW_FALSE;
    _glfw.win32.dragDropSession.active = GLFW_TRUE;

    // An external session allows COPY only: MOVE invites a file manager to physically relocate
    // whatever the payload names, out of the directory it belongs to.
    const DWORD allowed = sessionIsExternal() ? DROPEFFECT_COPY : DROPEFFECT_MOVE;

    // Blocks in its own modal loop until the drag ends. The engine keeps
    // rendering meanwhile via the live-resize pump it armed in StartDragDrop.
    DWORD effect = 0;
    const HRESULT ended = DoDragDrop(data, source, allowed, &effect);

    // A foreign application accepting the payload is neither of the other two outcomes, and a
    // caller reading "not consumed" as "landed nowhere" would tear out a window over it.
    int result = GLFW_DRAGDROP_CANCELLED;
    if (_glfw.win32.dragDropSession.dropReceived)
        result = GLFW_DRAGDROP_CONSUMED;
    else if (ended == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE)
        result = GLFW_DRAGDROP_CONSUMED_EXTERNALLY;

    data->lpVtbl->Release(data);
    source->lpVtbl->Release(source);
    destroyIconWindow();

    _GLFWwindow* originator = _glfw.win32.dragDropSession.window;
    _glfw_free(_glfw.win32.dragDropSession.type);
    freeSessionPayloads();
    memset(&_glfw.win32.dragDropSession, 0, sizeof(_glfw.win32.dragDropSession));

    // Match Cocoa/Wayland: report the session end, then synthesize the button
    // release DoDragDrop's modal loop swallowed so callers still see it.
    _glfwInputDragEnd(originator, result);
    _glfwInputMouseClick(originator, GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
}

void _glfwCancelPendingDragDropWin32(_GLFWwindow* window)
{
    if (!_glfw.win32.dragDropSession.pending || _glfw.win32.dragDropSession.active)
        return;
    if (_glfw.win32.dragDropSession.window != window)
        return;

    destroyIconWindow();
    _glfw_free(_glfw.win32.dragDropSession.type);
    freeSessionPayloads();
    memset(&_glfw.win32.dragDropSession, 0, sizeof(_glfw.win32.dragDropSession));

    // Report the aborted session so the engine's platform-drag bookkeeping resets (nothing
    // was consumed). No synthesized release: the real button-up is being delivered normally.
    _glfwInputDragEnd(window, GLFW_DRAGDROP_CANCELLED);
}

GLFWbool _glfwSetDragDropIconWin32(_GLFWwindow* window, const GLFWimage* image, int xhot, int yhot)
{
    if (!_glfw.win32.dragDropSession.pending && !_glfw.win32.dragDropSession.active)
        return GLFW_FALSE;
    if (!image || image->width <= 0 || image->height <= 0)
        return GLFW_FALSE;

    // Premultiplied BGRA top-down DIB for UpdateLayeredWindow's per-pixel alpha.
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = image->width;
    bi.bmiHeader.biHeight      = -image->height;   // negative: top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = NULL;
    HDC screenDC = GetDC(NULL);
    HBITMAP bitmap = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bitmap)
    {
        ReleaseDC(NULL, screenDC);
        return GLFW_FALSE;
    }

    const unsigned char* src = (const unsigned char*) image->pixels;
    unsigned char* dst = (unsigned char*) bits;
    for (int i = 0;  i < image->width * image->height;  i++)
    {
        const unsigned char r = src[i * 4 + 0];
        const unsigned char g = src[i * 4 + 1];
        const unsigned char b = src[i * 4 + 2];
        const unsigned char a = src[i * 4 + 3];
        dst[i * 4 + 0] = (unsigned char) ((b * a) / 255);
        dst[i * 4 + 1] = (unsigned char) ((g * a) / 255);
        dst[i * 4 + 2] = (unsigned char) ((r * a) / 255);
        dst[i * 4 + 3] = a;
    }

    if (!g_iconWindow)
    {
        g_iconWindow = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            GLFW_DRAG_ICON_CLASS, L"", WS_POPUP,
            0, 0, image->width, image->height,
            NULL, NULL, _glfw.win32.instance, NULL);
    }

    if (g_iconWindow)
    {
        HDC memDC = CreateCompatibleDC(screenDC);
        HGDIOBJ old = SelectObject(memDC, bitmap);

        POINT cursor;
        GetCursorPos(&cursor);
        POINT dstPt = { cursor.x - xhot, cursor.y - yhot };
        POINT srcPt = { 0, 0 };
        SIZE size = { image->width, image->height };
        BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

        UpdateLayeredWindow(g_iconWindow, screenDC, &dstPt, &size,
                            memDC, &srcPt, 0, &blend, ULW_ALPHA);
        ShowWindow(g_iconWindow, SW_SHOWNOACTIVATE);

        g_iconHotX = xhot;
        g_iconHotY = yhot;

        SelectObject(memDC, old);
        DeleteDC(memDC);
    }

    DeleteObject(bitmap);
    ReleaseDC(NULL, screenDC);
    return g_iconWindow ? GLFW_TRUE : GLFW_FALSE;
}

GLFWbool _glfwSetDragDropPayloadWin32(_GLFWwindow* window, const char* mime,
                                      const void* data, size_t size)
{
    // Armed only: the data object is built once, when the session enters DoDragDrop, so a
    // payload arriving after that would never be offered to anything.
    if (!_glfw.win32.dragDropSession.pending || _glfw.win32.dragDropSession.active)
        return GLFW_FALSE;
    if (_glfw.win32.dragDropSession.window != window)
        return GLFW_FALSE;

    void* bytes = NULL;
    size_t bytesSize = 0;
    const UINT format = renderPayload(mime, data, size, &bytes, &bytesSize);
    if (!format)
        return GLFW_FALSE;

    for (int i = 0;  i < _glfw.win32.dragDropSession.payloadCount;  i++)
    {
        _GLFWdragpayloadWin32* existing = &_glfw.win32.dragDropSession.payloads[i];
        if (existing->format != format)
            continue;
        _glfw_free(existing->bytes);
        existing->bytes = bytes;
        existing->size = bytesSize;
        return GLFW_TRUE;
    }

    const int count = _glfw.win32.dragDropSession.payloadCount;
    _GLFWdragpayloadWin32* grown = _glfw_realloc(_glfw.win32.dragDropSession.payloads,
        sizeof(_GLFWdragpayloadWin32) * (size_t) (count + 1));
    if (!grown)
    {
        _glfw_free(bytes);
        return GLFW_FALSE;
    }
    grown[count].format = format;
    grown[count].bytes = bytes;
    grown[count].size = bytesSize;
    _glfw.win32.dragDropSession.payloads = grown;
    _glfw.win32.dragDropSession.payloadCount = count + 1;
    return GLFW_TRUE;
}

#endif // _GLFW_WIN32
