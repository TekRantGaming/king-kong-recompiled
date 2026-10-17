#include "system_notify.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "notify_logic.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>
#else
#include <dlfcn.h>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace kk::notify {
namespace {

struct Job {
  enum Type { kShow, kHistory } type = kShow;
  std::string title, body, tag;
};

struct State {
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<Job> jobs;
  bool stop = false;
  bool running = false;
  std::thread thread;
  std::function<void()> on_click;
  LogFn log;
#if defined(_WIN32)
  HANDLE event = nullptr;  // jobs waiting (the worker also pumps the balloon window's messages)
#endif
};
// Never destroyed: a worker still running when the process exits must not meet a destroyed std::thread.
State& S() {
  static State* s = new State;
  return *s;
}

void Log(bool warning, const std::string& line) {
  LogFn log;
  {
    std::lock_guard lock(S().mutex);
    log = S().log;
  }
  if (log) log(warning, "KK notify: " + line);
}

void Clicked(const char* how) {
  std::function<void()> f;
  {
    std::lock_guard lock(S().mutex);
    f = S().on_click;
  }
  Log(false, std::string("notification clicked (") + how + ")");
  if (f) f();
}

[[maybe_unused]] std::string Hex(long v) {
  char b[16];
  std::snprintf(b, sizeof(b), "0x%08lX", static_cast<unsigned long>(v));
  return b;
}

#if defined(_WIN32)
// ------------------------------------------------------------------------------------------ Windows ---
using namespace ABI::Windows::UI::Notifications;
using ABI::Windows::Data::Xml::Dom::IXmlDocument;
using ABI::Windows::Data::Xml::Dom::IXmlDocumentIO;

std::wstring Wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

struct HStr {
  HSTRING h = nullptr;
  explicit HStr(const std::wstring& s) { WindowsCreateString(s.c_str(), UINT32(s.size()), &h); }
  ~HStr() {
    if (h) WindowsDeleteString(h);
  }
  HStr(const HStr&) = delete;
  HStr& operator=(const HStr&) = delete;
};

template <class T>
struct Com {
  T* p = nullptr;
  Com() = default;
  ~Com() { reset(); }
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  T** put() {
    reset();
    return &p;
  }
  void reset() {
    if (p) p->Release();
    p = nullptr;
  }
  T* operator->() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

const char* SettingName(NotificationSetting s) {
  switch (s) {
    case NotificationSetting_Enabled: return "enabled";
    case NotificationSetting_DisabledForApplication: return "turned off for this app in Windows Settings";
    case NotificationSetting_DisabledForUser: return "turned off for this user in Windows Settings";
    case NotificationSetting_DisabledByGroupPolicy: return "turned off by group policy";
    case NotificationSetting_DisabledByManifest: return "not allowed for this app ID (registration missing)";
  }
  return "unknown";
}

// The toast's click and failure events (in-process: they reach the launcher while it runs).
using ActivatedHandler = ABI::Windows::Foundation::ITypedEventHandler<ToastNotification*, IInspectable*>;
using FailedHandler = ABI::Windows::Foundation::ITypedEventHandler<ToastNotification*, ToastFailedEventArgs*>;

template <class I>
class Handler : public I {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (iid == IID_IUnknown || iid == __uuidof(I) || iid == __uuidof(IAgileObject)) {
      *out = static_cast<I*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --refs_;
    if (!left) delete this;
    return left;
  }

 protected:
  virtual ~Handler() = default;

 private:
  std::atomic<ULONG> refs_{1};
};

class OnActivated final : public Handler<ActivatedHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(IToastNotification*, IInspectable*) override {
    Clicked("toast");
    return S_OK;
  }
};
class OnFailed final : public Handler<FailedHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(IToastNotification*, IToastFailedEventArgs* args) override {
    HRESULT code = S_OK;
    if (args) args->get_ErrorCode(&code);
    Log(true, "toast failed after Show (HRESULT " + Hex(code) + ")");
    return S_OK;
  }
};

struct Windows {
  bool ro = false;            // RoInitialize done on the worker
  bool toast_tried = false;
  Com<IToastNotificationManagerStatics> manager;
  Com<IToastNotifier> notifier;
  std::deque<IToastNotification*> shown;  // kept so their click event still arrives
  HWND balloon_window = nullptr;
  bool balloon_icon = false;
} g_win;

// The app ID's per-user registration: its display name, so Windows accepts and labels the toasts without
// a Start menu shortcut. Only HKCU, only this key; written when missing or different.
bool RegisterAppId(std::string& detail) {
  const std::wstring key = std::wstring(L"Software\\Classes\\AppUserModelId\\") + kAppIdW;
  HKEY hkey = nullptr;
  LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr,
                           &hkey, nullptr);
  if (r != ERROR_SUCCESS) {
    detail = "registry key not created (" + std::to_string(r) + ")";
    return false;
  }
  const std::wstring name = Wide(kAppName);
  wchar_t current[128] = {};
  DWORD size = sizeof(current), type = 0;
  const bool same = RegQueryValueExW(hkey, L"DisplayName", nullptr, &type, reinterpret_cast<BYTE*>(current), &size) ==
                        ERROR_SUCCESS &&
                    type == REG_SZ && name == current;
  if (!same)
    r = RegSetValueExW(hkey, L"DisplayName", 0, REG_SZ, reinterpret_cast<const BYTE*>(name.c_str()),
                       DWORD((name.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(hkey);
  detail = same ? "app ID registered" : (r == ERROR_SUCCESS ? "app ID registered now" : "DisplayName not written");
  return r == ERROR_SUCCESS;
}

bool PrepareToasts() {
  if (g_win.toast_tried) return bool(g_win.notifier);
  g_win.toast_tried = true;
  std::string reg;
  RegisterAppId(reg);
  HRESULT hr = RoGetActivationFactory(HStr(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).h,
                                      __uuidof(IToastNotificationManagerStatics),
                                      reinterpret_cast<void**>(g_win.manager.put()));
  if (SUCCEEDED(hr)) hr = g_win.manager->CreateToastNotifierWithId(HStr(kAppIdW).h, g_win.notifier.put());
  if (FAILED(hr)) {
    Log(true, "toasts not available (HRESULT " + Hex(hr) + "); " + reg);
    g_win.notifier.reset();
    return false;
  }
  NotificationSetting setting = NotificationSetting_Enabled;
  g_win.notifier->get_Setting(&setting);
  Log(false, std::string("toasts ready (") + reg + "; notifications " + SettingName(setting) + ")");
  return true;
}

// The toast. False (with the reason logged) when the balloon should be tried instead.
bool ShowToast(const Job& job, bool& user_turned_off) {
  if (!PrepareToasts()) return false;
  NotificationSetting setting = NotificationSetting_Enabled;
  HRESULT hr = g_win.notifier->get_Setting(&setting);
  if (SUCCEEDED(hr) && setting != NotificationSetting_Enabled) {
    Log(true, std::string("toast not sent: notifications are ") + SettingName(setting));
    // Off by the player or a policy: respected (no balloon instead). Only a missing registration falls back.
    user_turned_off = setting != NotificationSetting_DisabledByManifest;
    return false;
  }
  Com<IInspectable> inspectable;
  Com<IXmlDocumentIO> io;
  Com<IXmlDocument> doc;
  hr = RoActivateInstance(HStr(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).h, inspectable.put());
  if (SUCCEEDED(hr)) hr = inspectable->QueryInterface(__uuidof(IXmlDocumentIO), reinterpret_cast<void**>(io.put()));
  if (SUCCEEDED(hr)) hr = io->LoadXml(HStr(Wide(ToastXml(job.title, job.body))).h);
  if (SUCCEEDED(hr)) hr = io->QueryInterface(__uuidof(IXmlDocument), reinterpret_cast<void**>(doc.put()));
  Com<IToastNotificationFactory> factory;
  Com<IToastNotification> toast;
  if (SUCCEEDED(hr))
    hr = RoGetActivationFactory(HStr(RuntimeClass_Windows_UI_Notifications_ToastNotification).h,
                                __uuidof(IToastNotificationFactory), reinterpret_cast<void**>(factory.put()));
  if (SUCCEEDED(hr)) hr = factory->CreateToastNotification(doc.p, toast.put());
  if (FAILED(hr)) {
    Log(true, "toast not made (HRESULT " + Hex(hr) + ")");
    return false;
  }
  if (!job.tag.empty()) {
    Com<IToastNotification2> toast2;
    if (SUCCEEDED(toast->QueryInterface(__uuidof(IToastNotification2), reinterpret_cast<void**>(toast2.put())))) {
      toast2->put_Tag(HStr(Wide(job.tag)).h);
      toast2->put_Group(HStr(L"content").h);
    }
  }
  EventRegistrationToken token{};
  auto* activated = new OnActivated;
  toast->add_Activated(activated, &token);
  activated->Release();
  auto* failed = new OnFailed;
  toast->add_Failed(failed, &token);
  failed->Release();
  hr = g_win.notifier->Show(toast.p);
  if (FAILED(hr)) {
    Log(true, "toast Show failed (HRESULT " + Hex(hr) + ")");
    return false;
  }
  toast->AddRef();
  g_win.shown.push_back(toast.p);
  while (g_win.shown.size() > 8) {
    g_win.shown.front()->Release();
    g_win.shown.pop_front();
  }
  Log(false, "shown as toast (Show HRESULT " + Hex(hr) + ", notifications " + SettingName(setting) + ", tag '" +
                 job.tag + "'): " + job.title);
  return true;
}

constexpr UINT kBalloonMessage = WM_APP + 7;

void RemoveBalloonIcon() {
  if (!g_win.balloon_icon) return;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g_win.balloon_window;
  nid.uID = 1;
  Shell_NotifyIconW(NIM_DELETE, &nid);
  g_win.balloon_icon = false;
}

LRESULT CALLBACK BalloonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == kBalloonMessage) {
    switch (LOWORD(lp)) {
      case NIN_BALLOONUSERCLICK:
      case NIN_SELECT:
      case NIN_KEYSELECT:
        RemoveBalloonIcon();
        Clicked("balloon");
        return 0;
      case NIN_BALLOONTIMEOUT:
        RemoveBalloonIcon();
        return 0;
    }
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

bool ShowBalloon(const Job& job) {
  if (!g_win.balloon_window) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = BalloonProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"KingKongRecompiledNotify";
    RegisterClassW(&wc);
    // Never shown: it only receives the notification icon's messages.
    g_win.balloon_window = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                           wc.hInstance, nullptr);
    if (!g_win.balloon_window) {
      Log(true, "balloon window not made (" + std::to_string(GetLastError()) + ")");
      return false;
    }
  }
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g_win.balloon_window;
  nid.uID = 1;
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_INFO | NIF_SHOWTIP;
  nid.uCallbackMessage = kBalloonMessage;
  nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
  if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
  wcsncpy_s(nid.szTip, Wide(kAppName).c_str(), _TRUNCATE);
  wcsncpy_s(nid.szInfoTitle, Wide(job.title).c_str(), _TRUNCATE);
  wcsncpy_s(nid.szInfo, Wide(job.body.empty() ? job.title : job.body).c_str(), _TRUNCATE);
  nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;  // the launcher plays its own ding
  RemoveBalloonIcon();
  BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);
  if (ok) {
    g_win.balloon_icon = true;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
  }
  Log(!ok, ok ? "shown as balloon: " + job.title : "balloon not shown (Shell_NotifyIcon failed)");
  return ok != FALSE;
}

void DoShow(const Job& job) {
  bool user_turned_off = false;
#if defined(KK_DEV_TOOLS)
  static const bool force_balloon = std::getenv("KK_DEV_NOTIFY_BALLOON") != nullptr;
#else
  constexpr bool force_balloon = false;
#endif
  if (!force_balloon && ShowToast(job, user_turned_off)) return;
  if (user_turned_off) return;
  ShowBalloon(job);
}

void DoHistory() {
  if (!PrepareToasts()) return;
  Com<IToastNotificationManagerStatics2> manager2;
  Com<IToastNotificationHistory> history;
  Com<IToastNotificationHistory2> history2;
  Com<ABI::Windows::Foundation::Collections::IVectorView<ToastNotification*>> list;
  HRESULT hr = g_win.manager->QueryInterface(__uuidof(IToastNotificationManagerStatics2),
                                             reinterpret_cast<void**>(manager2.put()));
  if (SUCCEEDED(hr)) hr = manager2->get_History(history.put());
  if (SUCCEEDED(hr))
    hr = history->QueryInterface(__uuidof(IToastNotificationHistory2), reinterpret_cast<void**>(history2.put()));
  if (SUCCEEDED(hr)) hr = history2->GetHistoryWithId(HStr(kAppIdW).h, list.put());
  unsigned count = 0;
  if (SUCCEEDED(hr)) hr = list->get_Size(&count);
  if (FAILED(hr)) {
    Log(true, "notification centre history not read (HRESULT " + Hex(hr) + ")");
    return;
  }
  std::string tags;
  for (unsigned i = 0; i < count; ++i) {
    Com<IToastNotification> t;
    Com<IToastNotification2> t2;
    HSTRING tag = nullptr;
    if (SUCCEEDED(list->GetAt(i, t.put())) &&
        SUCCEEDED(t->QueryInterface(__uuidof(IToastNotification2), reinterpret_cast<void**>(t2.put()))) &&
        SUCCEEDED(t2->get_Tag(&tag))) {
      UINT32 n = 0;
      const wchar_t* w = WindowsGetStringRawBuffer(tag, &n);
      tags += (tags.empty() ? "" : ", ") + std::string(w, w + n);  // tags are ASCII
      WindowsDeleteString(tag);
    }
  }
  Log(false, "notification centre holds " + std::to_string(count) + " of this app's notifications (tags: " + tags + ")");
}

void WorkerStart() {
  g_win.ro = SUCCEEDED(RoInitialize(RO_INIT_MULTITHREADED));
}
void WorkerEnd() {
  RemoveBalloonIcon();
  if (g_win.balloon_window) DestroyWindow(g_win.balloon_window);
  g_win.balloon_window = nullptr;
  for (auto* t : g_win.shown) t->Release();
  g_win.shown.clear();
  g_win.notifier.reset();
  g_win.manager.reset();
  g_win.toast_tried = false;
  if (g_win.ro) RoUninitialize();
  g_win.ro = false;
}
// Waits for a job while dispatching the balloon window's messages.
void WorkerWait() {
  const DWORD r = MsgWaitForMultipleObjectsEx(1, &S().event, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
  if (r == WAIT_OBJECT_0 + 1) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
}
void Wake() { SetEvent(S().event); }

#else
// -------------------------------------------------------------------------------------------- Linux ---
// libdbus, loaded at run time (no build or package dependency). The few types it needs, laid out as in
// dbus-errors.h / dbus-message.h (the iterator only as storage of at least its size).
struct DBusError {
  const char* name;
  const char* message;
  unsigned int dummy;
  void* padding1;
};
struct alignas(8) DBusMessageIter {
  unsigned char storage[128];
};
struct DBusConnection;
struct DBusMessage;
constexpr int kTypeInvalid = 0, kTypeByte = 'y', kTypeInt32 = 'i', kTypeUint32 = 'u', kTypeString = 's',
              kTypeArray = 'a', kTypeVariant = 'v', kTypeDictEntry = 'e';

struct DBus {
  void* lib = nullptr;
  void (*error_init)(DBusError*);
  void (*error_free)(DBusError*);
  DBusConnection* (*bus_get_private)(int, DBusError*);
  void (*set_exit_on_disconnect)(DBusConnection*, unsigned);
  void (*connection_close)(DBusConnection*);
  void (*connection_unref)(DBusConnection*);
  DBusMessage* (*new_method_call)(const char*, const char*, const char*, const char*);
  void (*message_unref)(DBusMessage*);
  void (*iter_init_append)(DBusMessage*, DBusMessageIter*);
  unsigned (*iter_append_basic)(DBusMessageIter*, int, const void*);
  unsigned (*iter_open_container)(DBusMessageIter*, int, const char*, DBusMessageIter*);
  unsigned (*iter_close_container)(DBusMessageIter*, DBusMessageIter*);
  DBusMessage* (*send_with_reply_and_block)(DBusConnection*, DBusMessage*, int, DBusError*);
  unsigned (*get_args)(DBusMessage*, DBusError*, int, ...);
  void (*add_match)(DBusConnection*, const char*, DBusError*);
  unsigned (*read_write)(DBusConnection*, int);
  DBusMessage* (*pop_message)(DBusConnection*);
  unsigned (*is_signal)(DBusMessage*, const char*, const char*);
  DBusConnection* conn = nullptr;
  bool tried = false;
  std::set<uint32_t> waiting;  // notifications shown with a click action, not closed yet
  std::chrono::steady_clock::time_point last_shown;
} g_dbus;

template <class F>
bool Sym(F& f, const char* name) {
  f = reinterpret_cast<F>(dlsym(g_dbus.lib, name));
  return f != nullptr;
}

bool PrepareDBus() {
  if (g_dbus.tried) return g_dbus.conn != nullptr;
  g_dbus.tried = true;
  g_dbus.lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
  if (!g_dbus.lib) {
    Log(false, "libdbus not found: notify-send is used instead");
    return false;
  }
  DBus& d = g_dbus;
  const bool ok = Sym(d.error_init, "dbus_error_init") && Sym(d.error_free, "dbus_error_free") &&
                  Sym(d.bus_get_private, "dbus_bus_get_private") &&
                  Sym(d.set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect") &&
                  Sym(d.connection_close, "dbus_connection_close") &&
                  Sym(d.connection_unref, "dbus_connection_unref") &&
                  Sym(d.new_method_call, "dbus_message_new_method_call") &&
                  Sym(d.message_unref, "dbus_message_unref") &&
                  Sym(d.iter_init_append, "dbus_message_iter_init_append") &&
                  Sym(d.iter_append_basic, "dbus_message_iter_append_basic") &&
                  Sym(d.iter_open_container, "dbus_message_iter_open_container") &&
                  Sym(d.iter_close_container, "dbus_message_iter_close_container") &&
                  Sym(d.send_with_reply_and_block, "dbus_connection_send_with_reply_and_block") &&
                  Sym(d.get_args, "dbus_message_get_args") && Sym(d.add_match, "dbus_bus_add_match") &&
                  Sym(d.read_write, "dbus_connection_read_write") &&
                  Sym(d.pop_message, "dbus_connection_pop_message") && Sym(d.is_signal, "dbus_message_is_signal");
  if (!ok) {
    Log(true, "libdbus is missing functions: notify-send is used instead");
    return false;
  }
  DBusError err;
  d.error_init(&err);
  // A private connection: SDL or other libraries may share the session bus connection.
  d.conn = d.bus_get_private(0 /* DBUS_BUS_SESSION */, &err);
  if (!d.conn) {
    Log(false, std::string("no D-Bus session bus (") + (err.message ? err.message : "?") +
                   "): notify-send is used instead");
    d.error_free(&err);
    return false;
  }
  d.set_exit_on_disconnect(d.conn, 0);
  d.add_match(d.conn, "type='signal',interface='org.freedesktop.Notifications'", &err);
  d.error_free(&err);
  return true;
}

void AppendString(DBusMessageIter* it, const std::string& s) {
  const char* p = s.c_str();
  g_dbus.iter_append_basic(it, kTypeString, &p);
}

bool ShowFreedesktop(const Job& job) {
  if (!PrepareDBus()) return false;
  DBus& d = g_dbus;
  DBusMessage* msg = d.new_method_call("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                       "org.freedesktop.Notifications", "Notify");
  if (!msg) return false;
  DBusMessageIter it, sub, entry, var;
  d.iter_init_append(msg, &it);
  AppendString(&it, kAppName);
  const uint32_t replaces = 0;
  d.iter_append_basic(&it, kTypeUint32, &replaces);
  AppendString(&it, "king_kong");  // the AppImage's icon name (king_kong.desktop)
  AppendString(&it, job.title);
  AppendString(&it, FreedesktopEscape(job.body));
  d.iter_open_container(&it, kTypeArray, "s", &sub);  // actions: the default one is a click on the notification
  AppendString(&sub, "default");
  AppendString(&sub, "Open");
  d.iter_close_container(&it, &sub);
  d.iter_open_container(&it, kTypeArray, "{sv}", &sub);  // hints
  d.iter_open_container(&sub, kTypeDictEntry, nullptr, &entry);
  AppendString(&entry, "desktop-entry");
  d.iter_open_container(&entry, kTypeVariant, "s", &var);
  AppendString(&var, "king_kong");
  d.iter_close_container(&entry, &var);
  d.iter_close_container(&sub, &entry);
  d.iter_open_container(&sub, kTypeDictEntry, nullptr, &entry);
  AppendString(&entry, "urgency");
  d.iter_open_container(&entry, kTypeVariant, "y", &var);
  const unsigned char normal = 1;
  d.iter_append_basic(&var, kTypeByte, &normal);
  d.iter_close_container(&entry, &var);
  d.iter_close_container(&sub, &entry);
  d.iter_close_container(&it, &sub);
  const int32_t expire = -1;
  d.iter_append_basic(&it, kTypeInt32, &expire);
  DBusError err;
  d.error_init(&err);
  DBusMessage* reply = d.send_with_reply_and_block(d.conn, msg, 3000, &err);
  d.message_unref(msg);
  if (!reply) {
    Log(true, std::string("freedesktop notification not sent (") + (err.name ? err.name : "?") + ": " +
                  (err.message ? err.message : "") + ")");
    d.error_free(&err);
    return false;
  }
  uint32_t id = 0;
  d.get_args(reply, &err, kTypeUint32, &id, kTypeInvalid);
  d.error_free(&err);
  d.message_unref(reply);
  d.waiting.insert(id);
  d.last_shown = std::chrono::steady_clock::now();
  Log(false, "shown as freedesktop notification (id " + std::to_string(id) + "): " + job.title);
  return true;
}

// Clicks (ActionInvoked) and closes, while a notification may still be clicked (at most 30 minutes).
void PollDBus() {
  DBus& d = g_dbus;
  if (!d.conn || d.waiting.empty()) return;
  if (std::chrono::steady_clock::now() - d.last_shown > std::chrono::minutes(30)) {
    d.waiting.clear();
    return;
  }
  d.read_write(d.conn, 0);
  while (DBusMessage* m = d.pop_message(d.conn)) {
    DBusError err;
    d.error_init(&err);
    uint32_t id = 0;
    const char* key = nullptr;
    if (d.is_signal(m, "org.freedesktop.Notifications", "ActionInvoked")) {
      if (d.get_args(m, &err, kTypeUint32, &id, kTypeString, &key, kTypeInvalid) && d.waiting.count(id)) {
        d.waiting.erase(id);
        Clicked("freedesktop");
      }
    } else if (d.is_signal(m, "org.freedesktop.Notifications", "NotificationClosed")) {
      uint32_t reason = 0;
      if (d.get_args(m, &err, kTypeUint32, &id, kTypeUint32, &reason, kTypeInvalid)) d.waiting.erase(id);
    }
    d.error_free(&err);
    d.message_unref(m);
  }
}

bool ShowNotifySend(const Job& job) {
  const std::string app = std::string("--app-name=") + kAppName;
  std::vector<char*> argv = {const_cast<char*>("notify-send"), const_cast<char*>(app.c_str()),
                             const_cast<char*>("--icon=king_kong"), const_cast<char*>(job.title.c_str()),
                             const_cast<char*>(job.body.c_str()), nullptr};
  pid_t pid = 0;
  if (posix_spawnp(&pid, "notify-send", nullptr, nullptr, argv.data(), environ) != 0) {
    Log(false, "no notification service (no D-Bus notifications, no notify-send): the launcher's pop-up only");
    return false;
  }
  int status = 0;
  waitpid(pid, &status, 0);
  const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  Log(!ok, ok ? "shown with notify-send: " + job.title
              : "notify-send failed (exit " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")");
  return ok;
}

void DoShow(const Job& job) {
  if (ShowFreedesktop(job)) return;
  ShowNotifySend(job);
}
void DoHistory() { Log(false, "notification history is not available on this system"); }
void WorkerStart() {}
void WorkerEnd() {
  if (g_dbus.conn) {
    g_dbus.connection_close(g_dbus.conn);
    g_dbus.connection_unref(g_dbus.conn);
  }
  g_dbus.conn = nullptr;
  g_dbus.tried = false;
  g_dbus.waiting.clear();
}
void WorkerWait() {
  std::unique_lock lock(S().mutex);
  if (!S().jobs.empty() || S().stop) return;
  // While a notification can be clicked, look for its click four times a second.
  if (g_dbus.waiting.empty()) S().wake.wait(lock);
  else S().wake.wait_for(lock, std::chrono::milliseconds(250));
  lock.unlock();
  PollDBus();
}
void Wake() { S().wake.notify_all(); }
#endif

void Worker() {
  WorkerStart();
  for (;;) {
    std::deque<Job> jobs;
    bool stop;
    {
      std::lock_guard lock(S().mutex);
      jobs.swap(S().jobs);
      stop = S().stop;
    }
    for (const Job& job : jobs) {
      if (job.type == Job::kShow) DoShow(job);
      else DoHistory();
    }
    if (stop) break;
    WorkerWait();
  }
  WorkerEnd();
}

void Queue(Job job) {
  {
    std::lock_guard lock(S().mutex);
    if (!S().running) return;
    S().jobs.push_back(std::move(job));
  }
  Wake();
}

}  // namespace

void Start(std::function<void()> on_click, LogFn log) {
  std::lock_guard lock(S().mutex);
  S().on_click = std::move(on_click);
  S().log = std::move(log);
  if (S().running) return;
  S().stop = false;
  S().running = true;
#if defined(_WIN32)
  if (!S().event) S().event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
#endif
  S().thread = std::thread(Worker);
}

void Show(std::string title, std::string body, std::string tag) {
  Queue({Job::kShow, std::move(title), std::move(body), std::move(tag)});
}

void LogHistory() { Queue({Job::kHistory, {}, {}, {}}); }

void Stop() {
  std::thread thread;
  {
    std::lock_guard lock(S().mutex);
    if (!S().running) return;
    S().stop = true;
    S().running = false;
    S().on_click = nullptr;
    thread = std::move(S().thread);
  }
  Wake();
  if (thread.joinable()) thread.join();
}

}  // namespace kk::notify
