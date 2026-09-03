//========================================================================
// GLFW 3.x Win32 - native drag and drop
//------------------------------------------------------------------------
// Implements glfwStartDragDrop / glfwSetDragDropIcon on Windows using OLE
// (DoDragDrop + IDropSource + IDataObject, and an IDropTarget on every
// window). Mirrors the Cocoa and Wayland backends: no payload is
// transferred, the session exists only so the OS routes hover/drop events
// (GLFW_DRAGDROP_ENTER/MOTION/LEAVE/DROP) to whichever of the app's own
// windows the cursor is over, plus a single GLFW_DRAGDROP end.
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

#include <stdlib.h>
#include <string.h>
#include <shellapi.h>   // HDROP, DragQueryFileW for external file drops

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
// IEnumFORMATETC over a single format (our session marker)
//========================================================================

typedef struct
{
    IEnumFORMATETC  iface;
    LONG            refs;
    ULONG           index;   // 0 before the (single) element has been read, 1 after
} FormatEnum;

static FormatEnum* createFormatEnum(ULONG index);

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
        _glfw_free(e);
    return refs;
}

static HRESULT STDMETHODCALLTYPE FormatEnum_Next(IEnumFORMATETC* self, ULONG celt,
                                                 FORMATETC* rgelt, ULONG* pceltFetched)
{
    FormatEnum* e = (FormatEnum*) self;
    ULONG fetched = 0;
    while (fetched < celt && e->index < 1)
    {
        FORMATETC fmt;
        fmt.cfFormat = (CLIPFORMAT) g_dragFormat;
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
    if (e->index + celt > 1)
    {
        e->index = 1;
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
    FormatEnum* clone = createFormatEnum(e->index);
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

static FormatEnum* createFormatEnum(ULONG index)
{
    FormatEnum* e = _glfw_calloc(1, sizeof(FormatEnum));
    if (!e)
        return NULL;
    e->iface.lpVtbl = &g_formatEnumVtbl;
    e->refs = 1;
    e->index = index;
    return e;
}

//========================================================================
// IDataObject (payload-less; advertises only the session marker format)
//========================================================================

typedef struct
{
    IDataObject iface;
    LONG        refs;
} DataObject;

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
        _glfw_free(o);
    return refs;
}

static HRESULT STDMETHODCALLTYPE DataObject_GetData(IDataObject* self, FORMATETC* fmt, STGMEDIUM* med)
{
    // No payload is ever transferred; the session marker carries no data.
    return DV_E_FORMATETC;
}

static HRESULT STDMETHODCALLTYPE DataObject_GetDataHere(IDataObject* self, FORMATETC* fmt, STGMEDIUM* med)
{
    return DV_E_FORMATETC;
}

static HRESULT STDMETHODCALLTYPE DataObject_QueryGetData(IDataObject* self, FORMATETC* fmt)
{
    if (fmt && fmt->cfFormat == (CLIPFORMAT) g_dragFormat && (fmt->tymed & TYMED_HGLOBAL))
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
    if (dir != DATADIR_GET)
        return E_NOTIMPL;
    if (!ppenum)
        return E_INVALIDARG;
    FormatEnum* e = createFormatEnum(0);
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

static HRESULT STDMETHODCALLTYPE DropSource_QueryContinueDrag(IDropSource* self, BOOL escape, DWORD keys)
{
    if (escape)
        return DRAGDROP_S_CANCEL;
    // The drag was started with the left button held; releasing it drops.
    if (!(keys & MK_LBUTTON))
        return DRAGDROP_S_DROP;
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
        *effect = DROPEFFECT_MOVE;
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
        *effect = DROPEFFECT_MOVE;
    }
    else if (_glfw.win32.dragDropSession.active)
        *effect = DROPEFFECT_MOVE;
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
        *effect = DROPEFFECT_MOVE;
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
        return;
    }

    _glfw.win32.dragDropSession.pending = GLFW_FALSE;
    _glfw.win32.dragDropSession.active = GLFW_TRUE;

    // Blocks in its own modal loop until the drag ends. The engine keeps
    // rendering meanwhile via the live-resize pump it armed in StartDragDrop.
    DWORD effect = 0;
    DoDragDrop(data, source, DROPEFFECT_MOVE, &effect);

    const GLFWbool consumed = _glfw.win32.dragDropSession.dropReceived;

    data->lpVtbl->Release(data);
    source->lpVtbl->Release(source);
    destroyIconWindow();

    _GLFWwindow* originator = _glfw.win32.dragDropSession.window;
    _glfw_free(_glfw.win32.dragDropSession.type);
    memset(&_glfw.win32.dragDropSession, 0, sizeof(_glfw.win32.dragDropSession));

    // Match Cocoa/Wayland: report the session end, then synthesize the button
    // release DoDragDrop's modal loop swallowed so callers still see it.
    _glfwInputDragEnd(originator, consumed);
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
    memset(&_glfw.win32.dragDropSession, 0, sizeof(_glfw.win32.dragDropSession));

    // Report the aborted session so the engine's platform-drag bookkeeping resets (nothing
    // was consumed). No synthesized release: the real button-up is being delivered normally.
    _glfwInputDragEnd(window, GLFW_FALSE);
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

#endif // _GLFW_WIN32
