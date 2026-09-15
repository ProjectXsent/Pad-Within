// =============================================================================
//  hook_di.cpp  -  vtable interception for IDirectInput8 / IDirectInputDevice8
//
//  Hooks on IDirectInput8:        CreateDevice        (vtable 3)
//  Hooks on IDirectInputDevice8:  GetDeviceState      (vtable 9)
//                                 GetDeviceData       (vtable 10)
//
//  Some titles poll the joystick with GetDeviceState (immediate mode); others
//  use GetDeviceData (buffered mode). WW/Warrior Within turned out to use
//  buffered data for the pad in some configurations, so we must handle both.
//  For the joystick device we synthesize immediate state from SDL; for buffered
//  reads we currently just LOG in diagnostic mode so we can see the format.
//
//  Mouse and keyboard always pass through untouched.
//
//  DIAGNOSTIC MODE: define POP2_DIAG to log which methods the game calls on the
//  joystick (and the raw SDL state) instead of overwriting anything.
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include "proxy.h"
#include "log.h"
#include "config.h"

// Diagnostic build: log methods/formats, do not overwrite state.
// Leave commented for the real (state-overwriting) build.
// #define POP2_DIAG 1

// ---- saved originals --------------------------------------------------------
using CreateDevice_t   = HRESULT (STDMETHODCALLTYPE*)(IDirectInput8*, REFGUID, LPDIRECTINPUTDEVICE8*, LPUNKNOWN);
using GetDeviceState_t = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8*, DWORD, LPVOID);
using GetDeviceData_t  = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using SetDataFormat_t  = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8*, LPCDIDATAFORMAT);
using GetProperty_t    = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8*, REFGUID, LPDIPROPHEADER);
using EnumObjects_t    = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8*, LPDIENUMDEVICEOBJECTSCALLBACKA, LPVOID, DWORD);
using SetProperty_t    = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8*, REFGUID, LPCDIPROPHEADER);
using Poll_t           = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8*);
using Acquire_t        = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8*);
using SetEventNotif_t  = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8*, HANDLE);

static CreateDevice_t g_origCreateDevice = nullptr;
using EnumDevices_t = HRESULT (STDMETHODCALLTYPE*)(IDirectInput8*, DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD);
static EnumDevices_t g_origEnumDevices = nullptr;

// Per-vtable originals (mouse/keyboard/joystick have different vtables).
struct DevHook {
    void**            vtbl;
    GetDeviceState_t  origState;
    GetDeviceData_t   origData;
    SetDataFormat_t   origSetFmt;
    GetProperty_t     origGetProp;
    EnumObjects_t     origEnum;
    SetProperty_t     origSetProp;
    Poll_t            origPoll;
    Acquire_t         origAcquire;
    SetEventNotif_t   origSetEvent;
    bool              isJoystick;   // true if created with a non-mouse/kbd GUID
};
static DevHook g_devHooks[8] = {};
static int     g_devHookCount = 0;

// Per-device-POINTER axis offset maps. Multiple joystick devices can share a
// vtable but expose different object layouts, so we key by the device pointer.
struct DevAxisMap { IDirectInputDevice8* dev; AxisOffsets map; bool valid; };
static DevAxisMap g_devMaps[8] = {};
static int        g_devMapCount = 0;

static DevAxisMap* GetOrAddDevMap(IDirectInputDevice8* dev)
{
    for (int i = 0; i < g_devMapCount; ++i)
        if (g_devMaps[i].dev == dev) return &g_devMaps[i];
    if (g_devMapCount < 8) {
        g_devMaps[g_devMapCount].dev = dev;
        g_devMaps[g_devMapCount].valid = false;
        return &g_devMaps[g_devMapCount++];
    }
    return nullptr;
}

// We no longer rely on a single "the" joystick device: the game may create
// several (e.g. DualSense enumerates as multiple DirectInput devices). We
// synthesize for ANY joystick device the game polls. Tracking is per-device
// object pointer so we can flag the right vtable entries.
static IDirectInputDevice8* g_joyDevices[8] = {};
static int g_joyDeviceCount = 0;

static bool IsTaggedJoystick(IDirectInputDevice8* dev)
{
    for (int i = 0; i < g_joyDeviceCount; ++i)
        if (g_joyDevices[i] == dev) return true;
    return false;
}

// DIAG/FIX: the game creates multiple joystick devices. Feeding identical
// synthesized data to all of them may confuse the binding-detection screen
// (which seems to scan across devices). This restricts synthesis to only the
// FIRST enumerated joystick device; other joystick devices are left native
// (effectively zeroed, since no real second controller exists).
static bool IsPrimaryJoystick(IDirectInputDevice8* dev)
{
    return g_joyDeviceCount > 0 && g_joyDevices[0] == dev;
}

static DevHook* FindHook(void** vtbl)
{
    for (int i = 0; i < g_devHookCount; ++i)
        if (g_devHooks[i].vtbl == vtbl) return &g_devHooks[i];
    return nullptr;
}

// ---------------------------------------------------------------------------
static void* PatchVtblSlot(void** vtbl, int index, void* newFn)
{
    void* old = vtbl[index];
    DWORD oldProt;
    if (VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) {
        vtbl[index] = newFn;
        VirtualProtect(&vtbl[index], sizeof(void*), oldProt, &oldProt);
        return old;
    }
    LOG("VirtualProtect failed patching slot %d", index);
    return nullptr;
}

// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE Hook_GetDeviceState(
    IDirectInputDevice8* self, DWORD cbData, LPVOID lpvData)
{
    void** vtbl = *reinterpret_cast<void***>(self);
    DevHook* h = FindHook(vtbl);
    HRESULT hr = (h && h->origState) ? h->origState(self, cbData, lpvData) : DIERR_NOTINITIALIZED;

#ifdef POP2_DIAG
    if (IsTaggedJoystick(self)) {
        static DWORD lastLog = 0;
        DWORD now = GetTickCount();
        if (now - lastLog > 250) {
            lastLog = now;
            LOG("GetDeviceState[JOY dev=%p]: cbData=%lu", (void*)self, cbData);
            Proxy_LogRawPad();
        }
    }
#else
    if (IsPrimaryJoystick(self) && lpvData) {
        extern bool Proxy_LogEnabled();
        extern bool Proxy_Passthrough();

        // PASSTHROUGH DIAGNOSTIC: leave the native buffer intact and dump all
        // 80 bytes, so we can capture the working layout of a real DI pad.
        if (Proxy_Passthrough()) {
            if (Proxy_LogEnabled() && cbData == sizeof(DIJOYSTATE)) {
                static DWORD lastP = 0; DWORD now = GetTickCount();
                if (now - lastP > 400) {
                    lastP = now;
                    const long* a = reinterpret_cast<const long*>(lpvData);
                    LOG("NATIVE 80B: X=%ld Y=%ld Z=%ld Rx=%ld Ry=%ld Rz=%ld "
                        "S0=%ld S1=%ld POV0=%lu",
                        a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                        *reinterpret_cast<const DWORD*>((const char*)lpvData + 32));
                }
            }
            return hr;   // do NOT synthesize
        }

        if (Proxy_LogEnabled()) {
            static DWORD lastLog = 0; DWORD now = GetTickCount();
            if (now - lastLog > 500) {
                lastLog = now;
                LOG("poll JOY dev=%p cbData=%lu fill=%d",
                    (void*)self, cbData, (int)(cbData == sizeof(DIJOYSTATE)));
            }
        }
        if (cbData == sizeof(DIJOYSTATE)) {
            // Diagnostic: capture what the REAL dinput read, before we overwrite,
            // so we can compare native vs synthesized axes.
            if (Proxy_LogEnabled()) {
                static DWORD lastN = 0; DWORD now = GetTickCount();
                if (now - lastN > 500) {
                    lastN = now;
                    const DIJOYSTATE* n = reinterpret_cast<const DIJOYSTATE*>(lpvData);
                    LOG("  native pre-overwrite: lX=%ld lY=%ld lZ=%ld lRx=%ld lRy=%ld lRz=%ld",
                        n->lX, n->lY, n->lZ, n->lRx, n->lRy, n->lRz);
                }
            }
            // Load THIS device's captured offset map so the fill writes to the
            // offsets this specific device uses (devices can differ).
            for (int i = 0; i < g_devMapCount; ++i) {
                if (g_devMaps[i].dev == self && g_devMaps[i].valid) {
                    g_axisOfs = g_devMaps[i].map;
                    break;
                }
            }
            Proxy_FillJoyState(reinterpret_cast<DIJOYSTATE*>(lpvData));
            hr = DI_OK;
        }
    }
#endif
    return hr;
}

// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE Hook_GetDeviceData(
    IDirectInputDevice8* self, DWORD cbObjData, LPDIDEVICEOBJECTDATA rgdod,
    LPDWORD pdwInOut, DWORD dwFlags)
{
    void** vtbl = *reinterpret_cast<void***>(self);
    DevHook* h = FindHook(vtbl);
    HRESULT hr = (h && h->origData)
        ? h->origData(self, cbObjData, rgdod, pdwInOut, dwFlags)
        : DIERR_NOTINITIALIZED;

#ifdef POP2_DIAG
    if (IsTaggedJoystick(self)) {
        static DWORD lastLog = 0;
        DWORD now = GetTickCount();
        if (now - lastLog > 250) {
            lastLog = now;
            DWORD cnt = pdwInOut ? *pdwInOut : 0;
            LOG("GetDeviceData[JOY]: cbObjData=%lu count=%lu flags=0x%lX hr=0x%08lX",
                cbObjData, cnt, dwFlags, hr);
            // dump each buffered element: dwOfs identifies the axis/button
            if (rgdod && !(dwFlags & DIGDD_PEEK)) {
                for (DWORD i = 0; i < cnt && i < 8; ++i)
                    LOG("    dod[%lu] dwOfs=%lu dwData=%ld", i,
                        rgdod[i].dwOfs, (long)rgdod[i].dwData);
            }
            Proxy_LogRawPad();
        }
    }
#endif
    return hr;
}

// ---------------------------------------------------------------------------
// EnumObjects interception. The game calls this to discover what axes/buttons
// the device has (this is how WW builds its axis list, NOT from GetDeviceState).
// We wrap the game's callback so we can log every object AND record the byte
// offset each axis GUID lives at — because some devices (e.g. DualSense via
// DirectInput) enumerate axes at NON-standard offsets (X and Y swapped!), and
// we must write our synthesized values at those real offsets, not the default
// DIJOYSTATE field offsets.
static const char* DofGuidName(const GUID* g);   // fwd (defined below)

// Axis byte-offset map, filled from EnumObjects. -1 = not present.
// Indices: 0=X 1=Y 2=Z 3=Rx 4=Ry 5=Rz
AxisOffsets g_axisOfs = {
    { -1,-1,-1,-1,-1,-1 }, -1,
    { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 }
};

static LPDIENUMDEVICEOBJECTSCALLBACKA g_gameEnumCb = nullptr;
static LPVOID                         g_gameEnumRef = nullptr;
static AxisOffsets*                   g_enumTarget = nullptr;

static BOOL CALLBACK EnumObjThunk(LPCDIDEVICEOBJECTINSTANCEA o, LPVOID ref)
{
    if (g_enumTarget && o && (o->dwType & DIDFT_AXIS)) {
        int idx = -1;
        if      (IsEqualGUID(o->guidType, GUID_XAxis))  idx = 0;
        else if (IsEqualGUID(o->guidType, GUID_YAxis))  idx = 1;
        else if (IsEqualGUID(o->guidType, GUID_ZAxis))  idx = 2;
        else if (IsEqualGUID(o->guidType, GUID_RxAxis)) idx = 3;
        else if (IsEqualGUID(o->guidType, GUID_RyAxis)) idx = 4;
        else if (IsEqualGUID(o->guidType, GUID_RzAxis)) idx = 5;
        if (idx >= 0) g_enumTarget->ofs[idx] = (int)o->dwOfs;
    }
    if (g_enumTarget && o && (o->dwType & DIDFT_POV) && g_enumTarget->povOfs < 0)
        g_enumTarget->povOfs = (int)o->dwOfs;
    if (g_enumTarget && o && (o->dwType & DIDFT_BUTTON)) {
        DWORD inst = DIDFT_GETINSTANCE(o->dwType);
        if (inst < 32) g_enumTarget->btnOfs[inst] = (int)o->dwOfs;
    }
    // forward to the game unchanged
    return g_gameEnumCb ? g_gameEnumCb(o, g_gameEnumRef) : DIENUM_CONTINUE;
}


static HRESULT STDMETHODCALLTYPE Hook_EnumObjects(
    IDirectInputDevice8* self, LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD flags)
{
    void** vtbl = *reinterpret_cast<void***>(self);
    DevHook* h = FindHook(vtbl);
    if (!h || !h->origEnum) return DIERR_NOTINITIALIZED;

    if (IsTaggedJoystick(self)) {
        LOG("EnumObjects dev=%p flags=0x%lX  -> capture offsets, then native",
            (void*)self, flags);
        DevAxisMap* dm = GetOrAddDevMap(self);
        if (dm && !dm->valid) {
            // Snapshot offsets with our OWN internal enumeration (game callback
            // not involved), so the game's own EnumObjects below is byte-for-byte
            // native — identical to passthrough, which is known to work.
            for (int i = 0; i < 6; ++i) dm->map.ofs[i] = -1;
            dm->map.povOfs = -1;
            for (int i = 0; i < 32; ++i) dm->map.btnOfs[i] = -1;
            g_gameEnumCb = nullptr;      // thunk won't forward; capture only
            g_gameEnumRef = nullptr;
            g_enumTarget = &dm->map;
            h->origEnum(self, &EnumObjThunk, nullptr, DIDFT_ALL);
            g_enumTarget = nullptr;
            dm->valid = true;
            LOG("  dev=%p offsets: X=%d Y=%d Z=%d Rx=%d Ry=%d POV=%d",
                (void*)self, dm->map.ofs[0], dm->map.ofs[1], dm->map.ofs[2],
                dm->map.ofs[3], dm->map.ofs[4], dm->map.povOfs);
        }
        // Now give the game its native enumeration, completely untouched.
        return h->origEnum(self, cb, ref, flags);
    }
    return h->origEnum(self, cb, ref, flags);
}

// ---------------------------------------------------------------------------
// SetProperty trampoline: log DIPROP_RANGE so we know what axis range WW wants.
static HRESULT STDMETHODCALLTYPE Hook_SetProperty(
    IDirectInputDevice8* self, REFGUID rguidProp, LPCDIPROPHEADER pdiph)
{
    void** vtbl = *reinterpret_cast<void***>(self);
    DevHook* h = FindHook(vtbl);
    HRESULT hr = (h && h->origSetProp) ? h->origSetProp(self, rguidProp, pdiph)
                                       : DIERR_NOTINITIALIZED;
    if (IsTaggedJoystick(self) && pdiph && &rguidProp == &DIPROP_RANGE
        && pdiph->dwSize >= sizeof(DIPROPRANGE)) {
        const DIPROPRANGE* r = reinterpret_cast<const DIPROPRANGE*>(pdiph);
        LOG("SetProperty RANGE dev=%p obj=%lu how=%lu  lMin=%ld lMax=%ld",
            (void*)self, pdiph->dwObj, pdiph->dwHow, r->lMin, r->lMax);
    }
    return hr;
}

// ---------------------------------------------------------------------------
// Poll trampoline: DirectInput's "sample now" call for polled (non-event)
// devices. We forward to the real device (harmless/no-op for most drivers)
// and log it, since we haven't verified whether WW's binding screen calls
// this before GetDeviceState in a way that matters.
static HRESULT STDMETHODCALLTYPE Hook_Poll(IDirectInputDevice8* self)
{
    extern bool Proxy_LogEnabled();
    void** vtbl = *reinterpret_cast<void***>(self);
    DevHook* h = FindHook(vtbl);
    HRESULT hr = (h && h->origPoll) ? h->origPoll(self) : DIERR_NOTINITIALIZED;
    if (IsTaggedJoystick(self) && Proxy_LogEnabled()) {
        static DWORD last = 0; DWORD now = GetTickCount();
        if (now - last > 300) { last = now; LOG("Poll dev=%p hr=0x%08lX", (void*)self, hr); }
    }
    return hr;
}

// ---------------------------------------------------------------------------
// Acquire trampoline: logs whenever the game (re)acquires the device — this
// often happens when entering/leaving a binding menu (cooperative level
// changes, exclusive vs non-exclusive re-acquire).
static HRESULT STDMETHODCALLTYPE Hook_Acquire(IDirectInputDevice8* self)
{
    void** vtbl = *reinterpret_cast<void***>(self);
    DevHook* h = FindHook(vtbl);
    HRESULT hr = (h && h->origAcquire) ? h->origAcquire(self) : DIERR_NOTINITIALIZED;
    if (IsTaggedJoystick(self))
        LOG("Acquire dev=%p hr=0x%08lX", (void*)self, hr);
    return hr;
}

// ---------------------------------------------------------------------------
// SetEventNotification (slot 12): registers a Win32 event the driver signals
// on data change, for event-driven (as opposed to polled) buffered reads.
// If WW's binding-detection screen waits on this event rather than just
// re-polling GetDeviceState in a loop, our synthesized updates would never
// wake it up (we never SetEvent), which would explain "always sees the same
// stale axis". THIS IS THE KEY THING TO OBSERVE.
static HRESULT STDMETHODCALLTYPE Hook_SetEventNotification(
    IDirectInputDevice8* self, HANDLE hEvent)
{
    void** vtbl = *reinterpret_cast<void***>(self);
    DevHook* h = FindHook(vtbl);
    HRESULT hr = (h && h->origSetEvent) ? h->origSetEvent(self, hEvent)
                                        : DIERR_NOTINITIALIZED;
    if (IsTaggedJoystick(self))
        LOG("SetEventNotification dev=%p hEvent=%p hr=0x%08lX",
            (void*)self, (void*)hEvent, hr);
    return hr;
}
// ---------------------------------------------------------------------------
// GetProperty trampoline. When the game asks a joystick for its VID/PID (to
// pick an axis-remap profile from Gamepads.DAT), we return a spoofed VID/PID
// whose profile uses straight axis order, so our axes pass through un-remapped.
static HRESULT STDMETHODCALLTYPE Hook_GetProperty(
    IDirectInputDevice8* self, REFGUID rguidProp, LPDIPROPHEADER pdiph)
{
    void** vtbl = *reinterpret_cast<void***>(self);
    DevHook* h = FindHook(vtbl);
    HRESULT hr = (h && h->origGetProp) ? h->origGetProp(self, rguidProp, pdiph)
                                       : DIERR_NOTINITIALIZED;

    // DIPROP_* are MAKEDIPROP() pseudo-GUIDs: integer values cast to REFGUID.
    // Compare by the pointer/value identity the SDK uses, via &rguidProp.
    if (g_cfg.spoofVidPid && IsTaggedJoystick(self)
        && &rguidProp == &DIPROP_VIDPID && pdiph
        && pdiph->dwSize >= sizeof(DIPROPDWORD)) {
        DIPROPDWORD* pd = reinterpret_cast<DIPROPDWORD*>(pdiph);
        DWORD oldVal = pd->dwData;
        // VIDPID dword layout: LOWORD = VID, HIWORD = PID.
        WORD vid = (WORD)g_cfg.spoofVID;
        WORD pid = (WORD)(g_cfg.spoofPID ? g_cfg.spoofPID : HIWORD(oldVal));
        pd->dwData = MAKELONG(vid, pid);
        hr = DI_OK;
        LOG("GetProperty VIDPID spoof dev=%p: 0x%08lX -> 0x%08lX",
            (void*)self, oldVal, pd->dwData);
    }
    return hr;
}

// ---------------------------------------------------------------------------
// Decode a data-format object GUID to a short axis/button name.
static const char* DofGuidName(const GUID* g)
{
    if (!g) return "any";
    if (IsEqualGUID(*g, GUID_XAxis))  return "X";
    if (IsEqualGUID(*g, GUID_YAxis))  return "Y";
    if (IsEqualGUID(*g, GUID_ZAxis))  return "Z";
    if (IsEqualGUID(*g, GUID_RxAxis)) return "Rx";
    if (IsEqualGUID(*g, GUID_RyAxis)) return "Ry";
    if (IsEqualGUID(*g, GUID_RzAxis)) return "Rz";
    if (IsEqualGUID(*g, GUID_Slider)) return "Slider";
    if (IsEqualGUID(*g, GUID_POV))    return "POV";
    if (IsEqualGUID(*g, GUID_Button)) return "Button";
    return "?";
}

// SetDataFormat trampoline: logs the exact layout WW asks for (axis -> offset),
// which tells us where to write each axis in GetDeviceState.
static HRESULT STDMETHODCALLTYPE Hook_SetDataFormat(
    IDirectInputDevice8* self, LPCDIDATAFORMAT fmt)
{
    void** vtbl = *reinterpret_cast<void***>(self);
    DevHook* h = FindHook(vtbl);
    HRESULT hr = (h && h->origSetFmt) ? h->origSetFmt(self, fmt) : DIERR_NOTINITIALIZED;

    if (fmt) {
        LOG("SetDataFormat dev=%p: dwDataSize=%lu numObjs=%lu flags=0x%lX",
            (void*)self, fmt->dwDataSize, fmt->dwNumObjs, fmt->dwFlags);
        for (DWORD i = 0; i < fmt->dwNumObjs; ++i) {
            const DIOBJECTDATAFORMAT& o = fmt->rgodf[i];
            DWORD type = DIDFT_GETTYPE(o.dwType);
            const char* kind = (type & DIDFT_AXIS)   ? "AXIS"   :
                               (type & DIDFT_BUTTON) ? "BUTTON" :
                               (type & DIDFT_POV)    ? "POV"    : "?";
            LOG("   obj[%2lu] ofs=%3lu %-6s guid=%-6s inst=%lu",
                i, o.dwOfs, kind, DofGuidName(o.pguid),
                (unsigned long)DIDFT_GETINSTANCE(o.dwType));
        }
    }
    return hr;
}

// ---------------------------------------------------------------------------
static const char* GuidName(REFGUID g)
{
    if (IsEqualGUID(g, GUID_SysMouse))    return "SysMouse";
    if (IsEqualGUID(g, GUID_SysKeyboard)) return "SysKeyboard";
    return "other/joystick";
}
static bool IsJoystickGuid(REFGUID g)
{
    return !IsEqualGUID(g, GUID_SysMouse) && !IsEqualGUID(g, GUID_SysKeyboard);
}

// =============================================================================
//  Fully synthetic joystick device
//
//  If the game enumerates DirectInput BEFORE any physical controller is
//  plugged in, the real dinput8.dll reports zero joystick-class devices, so
//  Hook_CreateDevice never gets a real device to patch and the game never
//  polls anything for the rest of the session (EnumDevicesThunk below is
//  what notices this and injects GUID_ProxyVirtualJoystick as a fake
//  enumerated device so the game always gets SOME joystick object).
//
//  This device only speaks the plain DIJOYSTATE format (offsets 0/4/8/12/
//  16/20 for X/Y/Z/Rx/Ry/Rz, 32 for POV, 48+i for buttons) - the same
//  layout Proxy_FillJoyState() already assumes for the real-device path via
//  g_axisOfs. Since there's no real hardware behind this device, we DEFINE
//  that as its native layout via our own EnumObjects and never need to
//  capture/remap anything, unlike the DevAxisMap dance used for real
//  devices with non-standard layouts (see Hook_EnumObjects above).
//
//  Deliberately unsupported / stubbed: force feedback, buffered
//  GetDeviceData (same limitation as the real-device path), action mapping,
//  image info. None of these matter for a simple polled DIJOYSTATE reader.
// =============================================================================

// {5B1E3A2E-7C1B-4E58-9F5B-6D6E1C0B2AA1} - arbitrary but fixed; only needs to
// be a value EnumDevicesThunk and Hook_CreateDevice agree on.
static const GUID GUID_ProxyVirtualJoystick =
{ 0x5b1e3a2e, 0x7c1b, 0x4e58, { 0x9f, 0x5b, 0x6d, 0x6e, 0x1c, 0x0b, 0x2a, 0xa1 } };

static void FillStandardAxisOffsets(AxisOffsets& m)
{
    static const int stdOfs[6] = { 0, 4, 8, 12, 16, 20 };
    for (int i = 0; i < 6; ++i) m.ofs[i] = stdOfs[i];
    m.povOfs = 32;
    for (int i = 0; i < 32; ++i) m.btnOfs[i] = 48 + i;
}

class VirtualJoystickDevice : public IDirectInputDevice8A
{
public:
    VirtualJoystickDevice() : m_refCount(1), m_acquired(false), m_dataSize(0),
                               m_bufferSize(0), m_hEvent(nullptr) {}

    // --- IUnknown ---
    STDMETHODIMP QueryInterface(REFIID riid, LPVOID* ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_IDirectInputDeviceA) ||
            IsEqualIID(riid, IID_IDirectInputDevice2A) ||
            IsEqualIID(riid, IID_IDirectInputDevice7A) ||
            IsEqualIID(riid, IID_IDirectInputDevice8A)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_refCount); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&m_refCount);
        if (r == 0) delete this;
        return (ULONG)r;
    }

    // --- capabilities / enumeration ---
    STDMETHODIMP GetCapabilities(LPDIDEVCAPS caps) override
    {
        if (!caps || (caps->dwSize != sizeof(DIDEVCAPS))) return DIERR_INVALIDPARAM;
        ZeroMemory(caps, sizeof(DIDEVCAPS));
        caps->dwSize    = sizeof(DIDEVCAPS);
        caps->dwFlags   = DIDC_ATTACHED | DIDC_EMULATED;
        caps->dwDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);
        caps->dwAxes    = 6;
        caps->dwButtons = 32;
        caps->dwPOVs    = 1;
        return DI_OK;
    }

    STDMETHODIMP EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ctx, DWORD flags) override
    {
        if (!cb) return DIERR_INVALIDPARAM;
        struct { REFGUID guid; DWORD ofs; const char* name; } axes[] = {
            { GUID_XAxis, 0,  "X Axis"  }, { GUID_YAxis,  4,  "Y Axis"  },
            { GUID_ZAxis, 8,  "Z Axis"  }, { GUID_RxAxis, 12, "X Rotation" },
            { GUID_RyAxis,16, "Y Rotation" }, { GUID_RzAxis, 20, "Z Rotation" },
        };
        if (flags == DIDFT_ALL || (flags & DIDFT_AXIS)) {
            for (int i = 0; i < 6; ++i) {
                DIDEVICEOBJECTINSTANCEA o = {};
                o.dwSize = sizeof(o);
                o.guidType = axes[i].guid;
                o.dwOfs = axes[i].ofs;
                o.dwType = DIDFT_MAKEINSTANCE(i) | DIDFT_ABSAXIS;
                lstrcpynA(o.tszName, axes[i].name, sizeof(o.tszName));
                if (cb(&o, ctx) == DIENUM_STOP) return DI_OK;
            }
        }
        if (flags == DIDFT_ALL || (flags & DIDFT_POV)) {
            DIDEVICEOBJECTINSTANCEA o = {};
            o.dwSize = sizeof(o);
            o.guidType = GUID_POV;
            o.dwOfs = 32;
            o.dwType = DIDFT_MAKEINSTANCE(0) | DIDFT_POV;
            lstrcpynA(o.tszName, "Hat Switch", sizeof(o.tszName));
            if (cb(&o, ctx) == DIENUM_STOP) return DI_OK;
        }
        if (flags == DIDFT_ALL || (flags & DIDFT_BUTTON)) {
            for (int i = 0; i < 32; ++i) {
                DIDEVICEOBJECTINSTANCEA o = {};
                o.dwSize = sizeof(o);
                o.guidType = GUID_Button;
                o.dwOfs = 48 + i;
                o.dwType = DIDFT_MAKEINSTANCE(i) | DIDFT_PSHBUTTON;
                char name[32]; snprintf(name, sizeof(name), "Button %d", i);
                lstrcpynA(o.tszName, name, sizeof(o.tszName));
                if (cb(&o, ctx) == DIENUM_STOP) return DI_OK;
            }
        }
        return DI_OK;
    }

    STDMETHODIMP GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA, DWORD, DWORD) override
    {
        return DIERR_INVALIDPARAM;   // not exercised by a polled joystick reader
    }

    // --- properties ---
    STDMETHODIMP GetProperty(REFGUID rguidProp, LPDIPROPHEADER pdiph) override
    {
        if (!pdiph) return DIERR_INVALIDPARAM;
        if (&rguidProp == &DIPROP_VIDPID && pdiph->dwSize >= sizeof(DIPROPDWORD)) {
            DIPROPDWORD* pd = reinterpret_cast<DIPROPDWORD*>(pdiph);
            // Same spoof identity Hook_GetProperty uses for real devices -
            // fall back to a real Xbox 360 controller's VID/PID even with
            // SpoofVidPid off, since this device has no "real" id to keep.
            WORD vid = g_cfg.spoofVidPid ? (WORD)g_cfg.spoofVID : 0x045E;
            WORD pid = g_cfg.spoofVidPid && g_cfg.spoofPID ? (WORD)g_cfg.spoofPID : 0x028E;
            pd->dwData = MAKELONG(vid, pid);
            return DI_OK;
        }
        if (&rguidProp == &DIPROP_RANGE && pdiph->dwSize >= sizeof(DIPROPRANGE)) {
            DIPROPRANGE* pr = reinterpret_cast<DIPROPRANGE*>(pdiph);
            pr->lMin = -32768; pr->lMax = 32767;
            return DI_OK;
        }
        return DIERR_UNSUPPORTED;
    }
    STDMETHODIMP SetProperty(REFGUID rguidProp, LPCDIPROPHEADER pdiph) override
    {
        // Range/deadzone are meaningless here (Proxy_FillJoyState applies its
        // own), accept and ignore. Buffer size is tracked purely so
        // GetDeviceData can distinguish "unbuffered" from "buffered, nothing
        // pending" below.
        if (&rguidProp == &DIPROP_BUFFERSIZE && pdiph && pdiph->dwSize >= sizeof(DIPROPDWORD))
            m_bufferSize = reinterpret_cast<const DIPROPDWORD*>(pdiph)->dwData;
        return DI_OK;
    }

    // --- acquisition ---
    STDMETHODIMP Acquire() override
    {
        m_acquired = true;
        LOG("Acquire dev=%p (VIRTUAL) hr=DI_OK", (void*)this);
        return DI_OK;
    }
    STDMETHODIMP Unacquire() override { m_acquired = false; return DI_OK; }
    STDMETHODIMP SetCooperativeLevel(HWND, DWORD) override { return DI_OK; }
    STDMETHODIMP Initialize(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }

    // --- state ---
    STDMETHODIMP GetDeviceState(DWORD cbData, LPVOID lpvData) override
    {
        if (!lpvData) return DIERR_INVALIDPARAM;
#ifdef POP2_DIAG
        // Diagnostic build: same "log, never overwrite" contract the real-
        // device path uses - nothing to compare against here since there's
        // no native hardware backing this device, but keep the behavior
        // symmetric rather than silently synthesizing in a diag build.
        static DWORD lastLog = 0; DWORD now = GetTickCount();
        if (now - lastLog > 250) {
            lastLog = now;
            LOG("GetDeviceState[VIRTUAL JOY dev=%p]: cbData=%lu", (void*)this, (unsigned long)cbData);
        }
        return DI_OK;
#else
        if (!m_acquired) return DIERR_NOTACQUIRED;
        extern bool Proxy_LogEnabled();
        if (Proxy_LogEnabled()) {
            static DWORD lastLog = 0; DWORD now = GetTickCount();
            if (now - lastLog > 500) { lastLog = now; LOG("poll VIRTUAL JOY dev=%p cbData=%lu", (void*)this, (unsigned long)cbData); }
        }
        ZeroMemory(lpvData, cbData);
        if (cbData != sizeof(DIJOYSTATE)) return DI_OK;   // this device only speaks DIJOYSTATE
        // This device advertised the plain/standard layout via EnumObjects
        // above (there's no real hardware to capture offsets from), so make
        // sure Proxy_FillJoyState writes to exactly that layout.
        FillStandardAxisOffsets(g_axisOfs);
        Proxy_FillJoyState(reinterpret_cast<DIJOYSTATE*>(lpvData));
        return DI_OK;
#endif
    }
    STDMETHODIMP GetDeviceData(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD pdwInOut, DWORD) override
    {
        // No buffered-mode event synthesis (same limitation noted on the
        // real-device GetDeviceData hook). Fail per the real API contract if
        // buffered mode was never turned on; otherwise "nothing pending".
        if (m_bufferSize == 0) return DIERR_NOTBUFFERED;
        if (pdwInOut) *pdwInOut = 0;
        return DI_OK;
    }
    STDMETHODIMP SetDataFormat(LPCDIDATAFORMAT fmt) override
    {
        if (!fmt) return DIERR_INVALIDPARAM;
        m_dataSize = fmt->dwDataSize;
        LOG("VirtualJoystickDevice::SetDataFormat dev=%p: dwDataSize=%lu numObjs=%lu",
            (void*)this, (unsigned long)fmt->dwDataSize, (unsigned long)fmt->dwNumObjs);
        return DI_OK;
    }
    STDMETHODIMP SetEventNotification(HANDLE hEvent) override
    {
        m_hEvent = hEvent;
        LOG("SetEventNotification dev=%p (VIRTUAL) hEvent=%p", (void*)this, (void*)hEvent);
        return DI_OK;
    }
    STDMETHODIMP Poll() override { return DI_OK; }

    STDMETHODIMP GetDeviceInfo(LPDIDEVICEINSTANCEA info) override
    {
        if (!info || info->dwSize != sizeof(DIDEVICEINSTANCEA)) return DIERR_INVALIDPARAM;
        FillInstance(*info);
        return DI_OK;
    }
    STDMETHODIMP RunControlPanel(HWND, DWORD) override { return DI_OK; }

    // --- unsupported: force feedback / buffered file I/O / action mapping ---
    STDMETHODIMP CreateEffect(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT*, LPUNKNOWN) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP EnumEffects(LPDIENUMEFFECTSCALLBACKA, LPVOID, DWORD) override { return DI_OK; }
    STDMETHODIMP GetEffectInfo(LPDIEFFECTINFOA, REFGUID) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP GetForceFeedbackState(LPDWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP SendForceFeedbackCommand(DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override { return DI_OK; }
    STDMETHODIMP Escape(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP EnumEffectsInFile(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP WriteEffectToFile(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP BuildActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP SetActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA) override { return DIERR_UNSUPPORTED; }

    static void FillInstance(DIDEVICEINSTANCEA& inst)
    {
        ZeroMemory(&inst, sizeof(inst));
        inst.dwSize = sizeof(inst);
        inst.guidInstance = GUID_ProxyVirtualJoystick;
        inst.guidProduct  = GUID_ProxyVirtualJoystick;
        inst.dwDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);
        lstrcpynA(inst.tszInstanceName, "Proxy Virtual Controller", sizeof(inst.tszInstanceName));
        const char* name = g_cfg.spoofVidPid ? "Controller (XBOX 360 For Windows)" : "Proxy Virtual Controller";
        lstrcpynA(inst.tszProductName, name, sizeof(inst.tszProductName));
    }

private:
    LONG   m_refCount;
    bool   m_acquired;
    DWORD  m_dataSize;
    DWORD  m_bufferSize;
    HANDLE m_hEvent;
};

// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(
    IDirectInput8* self, REFGUID rguid, LPDIRECTINPUTDEVICE8* out, LPUNKNOWN outer)
{
    if (out && IsEqualGUID(rguid, GUID_ProxyVirtualJoystick)) {
        // No real device backs this GUID - it was injected by EnumDevicesThunk
        // because nothing was physically attached at enumeration time. Hand
        // back our own COM object instead of forwarding to the real
        // CreateDevice (which has never heard of this GUID and would fail).
        VirtualJoystickDevice* vdev = new VirtualJoystickDevice();
        *out = vdev;
        if (g_joyDeviceCount < 8) {
            g_joyDevices[g_joyDeviceCount++] = vdev;
            LOG("CreateDevice: fabricated VIRTUAL joystick device %p (total joy devices=%d)",
                (void*)vdev, g_joyDeviceCount);
        }
        return DI_OK;
    }

    HRESULT hr = g_origCreateDevice(self, rguid, out, outer);
    LOG("CreateDevice guid=%s -> hr=0x%08lX dev=%p",
        GuidName(rguid), hr, (out ? *out : nullptr));

    if (SUCCEEDED(hr) && out && *out) {
        bool isJoy = IsJoystickGuid(rguid);
        if (isJoy && g_joyDeviceCount < 8) {
            g_joyDevices[g_joyDeviceCount++] = *out;
            LOG("  -> tagged joystick device %p (total joy devices=%d)",
                (void*)*out, g_joyDeviceCount);
        }
        void** devVtbl = *reinterpret_cast<void***>(*out);
        if (!FindHook(devVtbl) && g_devHookCount < 8) {
            GetDeviceState_t oS = reinterpret_cast<GetDeviceState_t>(
                PatchVtblSlot(devVtbl, 9,  reinterpret_cast<void*>(&Hook_GetDeviceState)));
            GetDeviceData_t  oD = reinterpret_cast<GetDeviceData_t>(
                PatchVtblSlot(devVtbl, 10, reinterpret_cast<void*>(&Hook_GetDeviceData)));
            SetDataFormat_t  oF = reinterpret_cast<SetDataFormat_t>(
                PatchVtblSlot(devVtbl, 11, reinterpret_cast<void*>(&Hook_SetDataFormat)));
            GetProperty_t    oP = reinterpret_cast<GetProperty_t>(
                PatchVtblSlot(devVtbl, 5,  reinterpret_cast<void*>(&Hook_GetProperty)));
            EnumObjects_t    oE = reinterpret_cast<EnumObjects_t>(
                PatchVtblSlot(devVtbl, 4,  reinterpret_cast<void*>(&Hook_EnumObjects)));
            SetProperty_t    oSP = reinterpret_cast<SetProperty_t>(
                PatchVtblSlot(devVtbl, 6,  reinterpret_cast<void*>(&Hook_SetProperty)));
            Poll_t           oPo = reinterpret_cast<Poll_t>(
                PatchVtblSlot(devVtbl, 25, reinterpret_cast<void*>(&Hook_Poll)));
            Acquire_t        oAc = reinterpret_cast<Acquire_t>(
                PatchVtblSlot(devVtbl, 7,  reinterpret_cast<void*>(&Hook_Acquire)));
            SetEventNotif_t  oSE = reinterpret_cast<SetEventNotif_t>(
                PatchVtblSlot(devVtbl, 12, reinterpret_cast<void*>(&Hook_SetEventNotification)));
            g_devHooks[g_devHookCount++] = { devVtbl, oS, oD, oF, oP, oE, oSP, oPo, oAc, oSE, isJoy };
            LOG("  patched vtable %p (hooks=%d)", (void*)devVtbl, g_devHookCount);
        }
    }
    return hr;
}

// ---------------------------------------------------------------------------
// IDirectInput8::EnumDevices interception. This is the FIRST place the game
// sees each device's identity (DIDEVICEINSTANCE.guidProduct, whose first DWORD
// encodes VID/PID for HID devices; and tszProductName). If WW determines its
// Gamepads.DAT axis-remap profile from THIS identity rather than from a later
// GetProperty(DIPROP_VIDPID) call, our device-level spoof (below) is the one
// that actually matters — presenting as a well-known Xbox 360 controller,
// exactly what Xidi/Steam Input do, since that's the identity this whole
// modding ecosystem (and presumably Gamepads.DAT) was validated against.
static LPDIENUMDEVICESCALLBACKA g_gameEnumDevCb = nullptr;
static LPVOID                   g_gameEnumDevRef = nullptr;
static int                      g_realJoyCountThisEnum = 0;
static bool                     g_enumStoppedByGame    = false;

static BOOL CALLBACK EnumDevicesThunk(LPCDIDEVICEINSTANCEA inst, LPVOID ref)
{
    DIDEVICEINSTANCEA spoofed = *inst;
    BYTE devClass = (BYTE)GET_DIDEVICE_TYPE(inst->dwDevType);
    bool isJoy = (devClass != DI8DEVTYPE_KEYBOARD) && (devClass != DI8DEVTYPE_MOUSE);
    if (isJoy) ++g_realJoyCountThisEnum;

    if (g_cfg.spoofVidPid && isJoy) {
        // Overwrite only the leading VID/PID DWORD of guidProduct, keep the
        // rest of the GUID intact (standard DirectInput HID product-GUID
        // template occupies the remaining bytes).
        DWORD orig = *reinterpret_cast<DWORD*>(&spoofed.guidProduct);
        DWORD vidpid = MAKELONG((WORD)g_cfg.spoofVID, (WORD)g_cfg.spoofPID);
        *reinterpret_cast<DWORD*>(&spoofed.guidProduct) = vidpid;
        lstrcpynA(spoofed.tszProductName, "Controller (XBOX 360 For Windows)",
                  sizeof(spoofed.tszProductName));
        LOG("EnumDevices spoof: '%s' guidProduct 0x%08lX -> 0x%08lX",
            inst->tszProductName, orig, vidpid);
    }
    BOOL ret = g_gameEnumDevCb ? g_gameEnumDevCb(&spoofed, g_gameEnumDevRef) : DIENUM_CONTINUE;
    if (ret == DIENUM_STOP) g_enumStoppedByGame = true;
    return ret;
}

static HRESULT STDMETHODCALLTYPE Hook_EnumDevices(
    IDirectInput8* self, DWORD dwDevType, LPDIENUMDEVICESCALLBACKA cb,
    LPVOID ref, DWORD flags)
{
    if (!g_origEnumDevices) return DIERR_NOTINITIALIZED;
    g_gameEnumDevCb  = cb;
    g_gameEnumDevRef = ref;
    g_realJoyCountThisEnum = 0;
    g_enumStoppedByGame    = false;
    HRESULT hr = g_origEnumDevices(self, dwDevType, &EnumDevicesThunk, nullptr, flags);
    g_gameEnumDevCb = nullptr;

    // No real joystick-class device was found in this enumeration - inject
    // our fully synthetic one so the game creates SOME joystick device
    // regardless of launch order (see VirtualJoystickDevice's file header).
    bool wantsGameCtrl = (dwDevType == 0 /*DI8DEVCLASS_ALL*/ || dwDevType == DI8DEVCLASS_GAMECTRL);
    if (SUCCEEDED(hr) && wantsGameCtrl && g_realJoyCountThisEnum == 0 && !g_enumStoppedByGame && cb) {
        DIDEVICEINSTANCEA fake;
        VirtualJoystickDevice::FillInstance(fake);
        LOG("EnumDevices: no real joystick present, injecting synthetic device '%s'", fake.tszProductName);
        cb(&fake, ref);
    }
    return hr;
}

// ---------------------------------------------------------------------------
void Proxy_HookDirectInput8(void** ppvOut)
{
    IDirectInput8* di = reinterpret_cast<IDirectInput8*>(*ppvOut);
    void** vtbl = *reinterpret_cast<void***>(di);

    if (!g_origCreateDevice) {
        g_origCreateDevice = reinterpret_cast<CreateDevice_t>(
            PatchVtblSlot(vtbl, 3, reinterpret_cast<void*>(&Hook_CreateDevice)));
        g_origEnumDevices = reinterpret_cast<EnumDevices_t>(
            PatchVtblSlot(vtbl, 4, reinterpret_cast<void*>(&Hook_EnumDevices)));
        LOG("patched IDirectInput8 vtable %p, CreateDevice=%p EnumDevices=%p",
            (void*)vtbl, (void*)g_origCreateDevice, (void*)g_origEnumDevices);
    }
}
