#include "flutter_window.h"

#include <optional>

#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>
#include <variant>

#include <shlobj.h>
#include <windowsx.h>

#include "flutter/generated_plugin_registrant.h"
#include "drag_out.h"
#include "drop_target.h"

FlutterWindow* FlutterWindow::active_window_ = nullptr;

namespace
{

std::wstring toWide (const std::string& s)
{
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar (CP_UTF8, 0, s.c_str(), (int) s.size(),
                                         nullptr, 0);
    std::wstring w ((size_t) n, L'\0');
    ::MultiByteToWideChar (CP_UTF8, 0, s.c_str(), (int) s.size(), w.data(), n);
    return w;
}

std::string toUtf8 (const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte (CP_UTF8, 0, w.c_str(), (int) w.size(),
                                         nullptr, 0, nullptr, nullptr);
    std::string s ((size_t) n, '\0');
    ::WideCharToMultiByte (CP_UTF8, 0, w.c_str(), (int) w.size(), s.data(), n,
                           nullptr, nullptr);
    return s;
}

} // namespace

FlutterWindow::FlutterWindow(const flutter::DartProject& project)
    : project_(project) {}

FlutterWindow::~FlutterWindow() {}

bool FlutterWindow::OnCreate() {
  if (!Win32Window::OnCreate()) {
    return false;
  }

  RECT frame = GetClientArea();

  // The size here must match the window dimensions to avoid unnecessary surface
  // creation / destruction in the startup path.
  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project_);
  // Ensure that basic setup of the controller was successful.
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());
  SetChildContent(flutter_controller_->view()->GetNativeWindow());

  // Подкласс HWND Flutter-view: без этого курсор над клиентом не доходит
  // до родителя — ресайз краёв и невидимый титул не работают.
  view_hwnd_ = flutter_controller_->view()->GetNativeWindow();
  if (view_hwnd_ != nullptr) {
    active_window_ = this;
    default_view_proc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtr(
        view_hwnd_, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&FlutterWindow::ViewProcThunk)));
  }

  // Канал kload/native: выбор папки, drag-out файлов, кнопки окна.
  channel_ = std::make_unique<
      flutter::MethodChannel<flutter::EncodableValue>>(
      flutter_controller_->engine()->messenger(), "kload/native",
      &flutter::StandardMethodCodec::GetInstance());
  channel_->SetMethodCallHandler(
      [this](const auto& call, auto result) {
        HandleMethodCall(call, std::move(result));
      });

  // Приём перетаскивания (ссылки из браузеров, текст, ярлыки .url/.webloc):
  // OLE drop target на HWND Flutter-view; события — в Dart через канал.
  if (view_hwnd_ != nullptr) {
    drop_target_ = new kload::DropTarget(
        [this](bool hover) {
          channel_->InvokeMethod(
              "dropHover", std::make_unique<flutter::EncodableValue>(hover));
        },
        [this](const std::string& payload) {
          channel_->InvokeMethod(
              "dropPayload", std::make_unique<flutter::EncodableValue>(payload));
        });
    ::RegisterDragDrop(view_hwnd_, drop_target_);
  }

  flutter_controller_->engine()->SetNextFrameCallback([&]() {
    this->Show();
  });

  // Flutter can complete the first frame before the "show window" callback is
  // registered. The following call ensures a frame is pending to ensure the
  // window is shown. It is a no-op if the first frame hasn't completed yet.
  flutter_controller_->ForceRedraw();

  return true;
}

void FlutterWindow::OnDestroy() {
  // Drop target отвязываем, пока окно и канал живы.
  if (view_hwnd_ != nullptr && drop_target_ != nullptr) {
    ::RevokeDragDrop(view_hwnd_);
    drop_target_->Release();
    drop_target_ = nullptr;
  }
  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }
  channel_ = nullptr;
  if (active_window_ == this) {
    active_window_ = nullptr;
  }

  Win32Window::OnDestroy();
}

LRESULT CALLBACK FlutterWindow::ViewProcThunk(HWND hwnd, UINT message,
                                              WPARAM wparam, LPARAM lparam) {
  auto* self = active_window_;
  if (self != nullptr) {
    if (message == WM_NCHITTEST) {
      return self->ViewHitTest(hwnd, lparam);
    }
    if (message == WM_NCLBUTTONDBLCLK) {
      // Двойной клик по титулу не разворачивает окно (паритет с macOS).
      return 0;
    }
    if (message == WM_WINDOWPOSCHANGING) {
      // View занимает клиентскую область целиком и всегда в (0,0):
      // что-то в процессе старта сдвигало его внутрь окна, из-за чего
      // контент рисовался со сдвигом и обрезкой (см. отчёт, правка 2).
      auto* wp = reinterpret_cast<WINDOWPOS*>(lparam);
      if (wp != nullptr && (wp->x != 0 || wp->y != 0)) {
        wp->x = 0;
        wp->y = 0;
      }
    }
  }
  return ::CallWindowProc(self ? self->default_view_proc_ : nullptr, hwnd,
                          message, wparam, lparam);
}

LRESULT FlutterWindow::ViewHitTest(HWND hwnd, LPARAM lparam) {
  const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
  POINT client = pt;
  ::ScreenToClient(hwnd, &client);
  RECT rc{};
  ::GetClientRect(hwnd, &rc);

  // Края окна — ресайз.
  const UINT dpi = ::GetDpiForWindow(hwnd);
  const LONG border = MulDiv(8, static_cast<int>(dpi), 96);
  const bool left = client.x < border;
  const bool right = client.x >= rc.right - border;
  const bool top = client.y < border;
  const bool bottom = client.y >= rc.bottom - border;
  if (top && left) return HTTOPLEFT;
  if (top && right) return HTTOPRIGHT;
  if (bottom && left) return HTBOTTOMLEFT;
  if (bottom && right) return HTBOTTOMRIGHT;
  if (left) return HTLEFT;
  if (right) return HTRIGHT;
  if (top) return HTTOP;
  if (bottom) return HTBOTTOM;

  // Курсор → координаты канваса 560×670 (cover-масштаб, как на macOS).
  const double scale = (rc.right > 0 && rc.bottom > 0)
                           ? (rc.right / 560.0 > rc.bottom / 670.0
                                  ? rc.right / 560.0
                                  : rc.bottom / 670.0)
                           : 1.0;
  const double off_x = (rc.right - 560.0 * scale) / 2.0;
  const double off_y = (rc.bottom - 670.0 * scale) / 2.0;
  const double cx = (client.x - off_x) / scale;
  const double cy = (client.y - off_y) / scale;

  // Кнопки окна — пропускаем во Flutter (клики), мимо них верхняя полоса —
  // невидимый титул (перетаскивание).
  for (const ChromeRect& r : chrome_rects_) {
    if (cx >= r.x && cy >= r.y && cx <= r.x + r.w && cy <= r.y + r.h) {
      return ::CallWindowProc(default_view_proc_, hwnd, WM_NCHITTEST, 0, lparam);
    }
  }
  if (cy < 44.0) return HTCAPTION;

  return ::CallWindowProc(default_view_proc_, hwnd, WM_NCHITTEST, 0, lparam);
}

void FlutterWindow::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const std::string& method = call.method_name();

  if (method == "chooseFolder") {
    const std::string folder = PickFolder(GetHandle());
    if (folder.empty())
      result->Success();
    else
      result->Success(flutter::EncodableValue(folder));
    return;
  }

  if (method == "setZones") {
    // На Windows зоны не нужны: drag стартует из Dart-жестов (beginDrag).
    result->Success();
    return;
  }

  if (method == "chromeRects") {
    // Кнопки окна в координатах канваса 560×670: hit-test не отдаёт их
    // титулу, и клики доходят до Flutter. Плоский список [x,y,w,h, …].
    chrome_rects_.clear();
    if (const auto* list = std::get_if<flutter::EncodableList>(call.arguments())) {
      auto num = [&list](size_t idx) -> double {
        const auto& v = (*list)[idx];
        if (const auto* d = std::get_if<double>(&v)) return *d;
        if (const auto* i = std::get_if<int32_t>(&v))
          return static_cast<double>(*i);
        if (const auto* i64 = std::get_if<int64_t>(&v))
          return static_cast<double>(*i64);
        return 0.0;
      };
      for (size_t i = 0; i + 3 < list->size(); i += 4)
        chrome_rects_.push_back({num(i), num(i + 1), num(i + 2), num(i + 3)});
    }
    result->Success();
    return;
  }

  if (method == "beginDrag") {
    const auto* path = std::get_if<std::string>(call.arguments());
    if (path == nullptr || path->empty()) {
      result->Error("args", "ожидался путь файла");
      return;
    }
    // DoDragDrop работает, только пока зажата левая кнопка; Dart вызывает
    // из жеста нажатия. Блокирует платформенный поток — так и задумано:
    // модальный цикл сам качает сообщения.
    const HRESULT hr = kload::DragOutFile (toWide (*path));
    result->Success(flutter::EncodableValue((int64_t) hr));
    return;
  }

  if (method == "appVersion") {
    // Версия из pubspec (flutter подставляет её в FLUTTER_VERSION).
    result->Success(flutter::EncodableValue(std::string(FLUTTER_VERSION)));
    return;
  }

  if (method == "minimize") {
    ::ShowWindow(GetHandle(), SW_MINIMIZE);
    result->Success();
    return;
  }

  if (method == "close") {
    ::PostMessage(GetHandle(), WM_CLOSE, 0, 0);
    result->Success();
    return;
  }

  result->NotImplemented();
}

std::string FlutterWindow::PickFolder(HWND owner)
{
    std::string out;
    IFileDialog* dialog = nullptr;
    if (SUCCEEDED (::CoCreateInstance (CLSID_FileOpenDialog, nullptr,
                                       CLSCTX_INPROC_SERVER, IID_IFileDialog,
                                       reinterpret_cast<void**> (&dialog))))
    {
        DWORD options = 0;
        dialog->GetOptions (&options);
        dialog->SetOptions (options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->SetTitle (L"Выберите папку для загрузок");
        if (SUCCEEDED (dialog->Show (owner)))
        {
            IShellItem* item = nullptr;
            if (SUCCEEDED (dialog->GetResult (&item)))
            {
                PWSTR path = nullptr;
                if (SUCCEEDED (item->GetDisplayName (SIGDN_FILESYSPATH, &path)))
                {
                    out = toUtf8 (path);
                    ::CoTaskMemFree (path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    return out;
}

LRESULT
FlutterWindow::MessageHandler(HWND hwnd, UINT const message,
                              WPARAM const wparam,
                              LPARAM const lparam) noexcept {
  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_controller_) {
    std::optional<LRESULT> result =
        flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam,
                                                      lparam);
    if (result) {
      return *result;
    }
  }

  switch (message) {
    case WM_FONTCHANGE:
      flutter_controller_->engine()->ReloadSystemFonts();
      break;
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}
