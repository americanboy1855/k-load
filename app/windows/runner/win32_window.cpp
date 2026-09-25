#include "win32_window.h"

#include <dwmapi.h>
#include <flutter_windows.h>
#include <windowsx.h>

#include "resource.h"

namespace {

/// Window attribute that enables dark mode window decorations.
///
/// Redefined in case the developer's machine has a Windows SDK older than
/// version 10.0.22000.0.
/// See: https://docs.microsoft.com/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

constexpr const wchar_t kWindowClassName[] = L"FLUTTER_RUNNER_WIN32_WINDOW";

/// Registry key for app theme preference.
///
/// A value of 0 indicates apps should use dark mode. A non-zero or missing
/// value indicates apps should use light mode.
constexpr const wchar_t kGetPreferredBrightnessRegKey[] =
  L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr const wchar_t kGetPreferredBrightnessRegValue[] = L"AppsUseLightTheme";

// The number of Win32Window objects that currently exist.
static int g_active_window_count = 0;

using EnableNonClientDpiScaling = BOOL __stdcall(HWND hwnd);

// Scale helper to convert logical scaler values to physical using passed in
// scale factor
int Scale(int source, double scale_factor) {
  return static_cast<int>(source * scale_factor);
}

// Dynamically loads the |EnableNonClientDpiScaling| from the User32 module.
// This API is only needed for PerMonitor V1 awareness mode.
void EnableFullDpiSupportIfAvailable(HWND hwnd) {
  HMODULE user32_module = LoadLibraryA("User32.dll");
  if (!user32_module) {
    return;
  }
  auto enable_non_client_dpi_scaling =
      reinterpret_cast<EnableNonClientDpiScaling*>(
          GetProcAddress(user32_module, "EnableNonClientDpiScaling"));
  if (enable_non_client_dpi_scaling != nullptr) {
    enable_non_client_dpi_scaling(hwnd);
  }
  FreeLibrary(user32_module);
}

// Пропорция 560:670 и лимиты 0.8×–1.4×, применённые к тянущемуся краю.
// edge приходит и как HTLEFT..HTBOTTOMRIGHT (10..17, наш цикл ресайза),
// и как WMSZ_* (1..8, системный WM_SIZING) — числа НЕ совпадают, поэтому
// сначала нормализуем в HT-пространство. Сторона, за которую тянут,
// следует за мышью. Рост перпендикулярной оси у краёв идёт от центра —
// иначе при тяге левого края окно уползало вверх за экран (репорт
// «интерфейс плавает при resize слева/сверху»). У углов противоположный
// угол неподвижен, как принято в Windows.
void ApplySizeConstraints(RECT* rc, UINT edge, UINT dpi) {
  if (edge >= WMSZ_LEFT && edge <= WMSZ_BOTTOMRIGHT) {
    edge = edge + (HTSIZEFIRST - WMSZ_LEFT);  // WMSZ_* -> HT*
  }
  const double aspect = 560.0 / 670.0;
  const LONG min_h = MulDiv(536, static_cast<int>(dpi), 96);
  const LONG max_h = MulDiv(938, static_cast<int>(dpi), 96);
  LONG w = rc->right - rc->left;
  LONG h = rc->bottom - rc->top;
  if (edge == HTTOP || edge == HTBOTTOM) {
    w = static_cast<LONG>(h * aspect + 0.5);
  } else {
    h = static_cast<LONG>(w / aspect + 0.5);
  }
  if (h < min_h) {
    h = min_h;
    w = static_cast<LONG>(h * aspect + 0.5);
  } else if (h > max_h) {
    h = max_h;
    w = static_cast<LONG>(h * aspect + 0.5);
  }
  const bool hold_left =
      edge == HTLEFT || edge == HTTOPLEFT || edge == HTBOTTOMLEFT;
  const bool hold_top =
      edge == HTTOP || edge == HTTOPLEFT || edge == HTTOPRIGHT;
  if (edge == HTLEFT || edge == HTRIGHT) {
    // Горизонтальная сторона — за мышью (при упоре в лимит держим
    // противоположную), вертикаль — от центра.
    if (w != rc->right - rc->left) {
      if (hold_left) {
        rc->left = rc->right - w;
      } else {
        rc->right = rc->left + w;
      }
    }
    const LONG cy = (rc->top + rc->bottom) / 2;
    rc->top = cy - h / 2;
    rc->bottom = rc->top + h;
  } else if (edge == HTTOP || edge == HTBOTTOM) {
    // Вертикальная сторона — за мышью, горизонталь — от центра.
    if (h != rc->bottom - rc->top) {
      if (hold_top) {
        rc->top = rc->bottom - h;
      } else {
        rc->bottom = rc->top + h;
      }
    }
    const LONG cx = (rc->left + rc->right) / 2;
    rc->left = cx - w / 2;
    rc->right = rc->left + w;
  } else {
    if (hold_left) {
      rc->right = rc->left + w;
    } else {
      rc->left = rc->right - w;
    }
    if (hold_top) {
      rc->bottom = rc->top + h;
    } else {
      rc->top = rc->bottom - h;
    }
  }
}

// Цикл ресайза активен: Flutter-view держит максимальный размер (784×938),
// WM_SIZE в это время ребёнка не трогает — реальный размер уходит в Dart
// каналом liveSize.
bool g_in_size_loop = false;

void ResizeViewToClient(HWND hwnd) {
  const HWND view = ::GetWindow(hwnd, GW_CHILD);
  if (view == nullptr) {
    return;
  }
  RECT cr{};
  ::GetClientRect(hwnd, &cr);
  ::MoveWindow(view, 0, 0, cr.right, cr.bottom, TRUE);
}

// Собственный цикл ресайза вместо системного (DefWindowProc на
// WM_NCLBUTTONDOWN запускает модальный SC_SIZE-цикл, который менял размер
// ступенями ~16 Гц и терял до половины движений мыши — растягивание выглядело
// рваным). Здесь рамка окна следует за курсором на каждое движение мыши
// (SetWindowPos стоит ~1 мс). Секрет плавности контента: Flutter-view на всё
// время жеста растягивается до максимума (784×938) и НЕ пересоздаёт свап-чейн
// (~27 мс за каждый ресайз — это и резало кадры), а реальный размер окна
// уходит в Dart каналом (OnLiveSize): контент перерисовывается каждый кадр
// под фактическую рамку. По отпусканию кнопки один точный ресайз view —
// без скачка: размеры совпадают. Обычный насос сообщений держит Flutter
// живым, Escape отменяет.
void RunSizeLoop(HWND hwnd, UINT edge, Win32Window* self) {
  // Один цикл за раз: вложенный (клик по другому краю при живом жесте)
  // ломал бы захват и размеры.
  if (g_in_size_loop) {
    return;
  }
  RECT start{};
  ::GetWindowRect(hwnd, &start);
  POINT press{};
  ::GetCursorPos(&press);
  const UINT dpi = ::GetDpiForWindow(hwnd);

  const wchar_t* cursor_id = IDC_ARROW;
  switch (edge) {
    case HTLEFT:
    case HTRIGHT:
      cursor_id = IDC_SIZEWE;
      break;
    case HTTOP:
    case HTBOTTOM:
      cursor_id = IDC_SIZENS;
      break;
    case HTTOPLEFT:
    case HTBOTTOMRIGHT:
      cursor_id = IDC_SIZENWSE;
      break;
    case HTTOPRIGHT:
    case HTBOTTOMLEFT:
      cursor_id = IDC_SIZENESW;
      break;
  }
  const HCURSOR size_cursor = ::LoadCursor(nullptr, cursor_id);

  auto send_live_size = [&]() {
    if (self == nullptr) {
      return;
    }
    RECT cr{};
    ::GetClientRect(hwnd, &cr);
    self->OnLiveSize((cr.right - cr.left) * 96.0 / dpi,
                     (cr.bottom - cr.top) * 96.0 / dpi);
  };

  // Вид — на максимум до начала жеста: Dart получает live-размер раньше
  // смены метрик, первый кадр жеста сразу правильный.
  send_live_size();
  if (const HWND view = ::GetWindow(hwnd, GW_CHILD); view != nullptr) {
    ::MoveWindow(view, 0, 0, MulDiv(784, static_cast<int>(dpi), 96),
                 MulDiv(938, static_cast<int>(dpi), 96), TRUE);
  }

  g_in_size_loop = true;
  ::SetCapture(hwnd);
  bool done = false;
  bool cancelled = false;
  while (!done && ::IsWindow(hwnd)) {
    MSG msg;
    const BOOL got = ::GetMessage(&msg, nullptr, 0, 0);
    if (got <= 0) {
      if (got == 0) {
        ::PostQuitMessage(static_cast<int>(msg.wParam));
      }
      break;
    }
    switch (msg.message) {
      case WM_MOUSEMOVE: {
        POINT cur{};
        ::GetCursorPos(&cur);
        RECT rc = start;
        const LONG dx = cur.x - press.x;
        const LONG dy = cur.y - press.y;
        if (edge == HTLEFT || edge == HTTOPLEFT || edge == HTBOTTOMLEFT) {
          rc.left += dx;
        }
        if (edge == HTRIGHT || edge == HTTOPRIGHT || edge == HTBOTTOMRIGHT) {
          rc.right += dx;
        }
        if (edge == HTTOP || edge == HTTOPLEFT || edge == HTTOPRIGHT) {
          rc.top += dy;
        }
        if (edge == HTBOTTOM || edge == HTBOTTOMLEFT ||
            edge == HTBOTTOMRIGHT) {
          rc.bottom += dy;
        }
        ApplySizeConstraints(&rc, edge, dpi);
        ::SetWindowPos(hwnd, nullptr, rc.left, rc.top,
                       rc.right - rc.left, rc.bottom - rc.top,
                       SWP_NOACTIVATE | SWP_NOZORDER);
        send_live_size();
        break;
      }
      case WM_LBUTTONUP:
        done = true;
        break;
      case WM_KEYDOWN:
        if (msg.wParam == VK_ESCAPE) {
          cancelled = true;
          done = true;
        }
        break;
      case WM_CANCELMODE:
      case WM_CAPTURECHANGED:
        done = true;
        break;
      case WM_SETCURSOR:
        // Курсор — размерный на весь жест: hit-test по увеличенному view
        // давал бы обычную стрелку.
        ::SetCursor(size_cursor);
        break;
      default:
        ::TranslateMessage(&msg);
        ::DispatchMessage(&msg);
    }
  }
  g_in_size_loop = false;
  ::ReleaseCapture();
  if (!::IsWindow(hwnd)) {
    return;
  }
  if (cancelled) {
    ::SetWindowPos(hwnd, nullptr, start.left, start.top,
                   start.right - start.left, start.bottom - start.top,
                   SWP_NOACTIVATE | SWP_NOZORDER);
  }
  // Сначала точный ресайз view (метрики и live-размер совпадают — кадра
  // со скачком нет), потом снимаем live-режим в Dart.
  ResizeViewToClient(hwnd);
  if (self != nullptr) {
    self->OnLiveSizeEnd();
  }
}

}  // namespace

// Manages the Win32Window's window class registration.
class WindowClassRegistrar {
 public:
  ~WindowClassRegistrar() = default;

  // Returns the singleton registrar instance.
  static WindowClassRegistrar* GetInstance() {
    if (!instance_) {
      instance_ = new WindowClassRegistrar();
    }
    return instance_;
  }

  // Returns the name of the window class, registering the class if it hasn't
  // previously been registered.
  const wchar_t* GetWindowClass();

  // Unregisters the window class. Should only be called if there are no
  // instances of the window.
  void UnregisterWindowClass();

 private:
  WindowClassRegistrar() = default;

  static WindowClassRegistrar* instance_;

  bool class_registered_ = false;
};

WindowClassRegistrar* WindowClassRegistrar::instance_ = nullptr;

const wchar_t* WindowClassRegistrar::GetWindowClass() {
  if (!class_registered_) {
    WNDCLASS window_class{};
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClassName;
    // Без CS_HREDRAW|CS_VREDRAW: полная перерисовка на каждый шаг ресайза
    // усугубляла «рваное» растягивание окна.
    window_class.style = 0;
    window_class.cbClsExtra = 0;
    window_class.cbWndExtra = 0;
    window_class.hInstance = GetModuleHandle(nullptr);
    window_class.hIcon =
        LoadIcon(window_class.hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    window_class.hbrBackground = CreateSolidBrush(RGB(0x16, 0x14, 0x13));
    // Кисть цвета пластика корпуса (#161413): даже если системе понадобится
    // залить фон до первого кадра Flutter, вспышка будет тёмной, не белой.
    window_class.lpszMenuName = nullptr;
    window_class.lpfnWndProc = Win32Window::WndProc;
    RegisterClass(&window_class);
    class_registered_ = true;
  }
  return kWindowClassName;
}

void WindowClassRegistrar::UnregisterWindowClass() {
  UnregisterClass(kWindowClassName, nullptr);
  class_registered_ = false;
}

Win32Window::Win32Window() {
  ++g_active_window_count;
}

Win32Window::~Win32Window() {
  --g_active_window_count;
  Destroy();
}

bool Win32Window::Create(const std::wstring& title,
                         const Point& origin,
                         const Size& size) {
  Destroy();

  const wchar_t* window_class =
      WindowClassRegistrar::GetInstance()->GetWindowClass();

  const POINT target_point = {static_cast<LONG>(origin.x),
                              static_cast<LONG>(origin.y)};
  HMONITOR monitor = MonitorFromPoint(target_point, MONITOR_DEFAULTTONEAREST);
  UINT dpi = FlutterDesktopGetDpiForMonitor(monitor);
  double scale_factor = dpi / 96.0;

  // Безрамочное окно (паритет с macOS): WS_THICKFRAME — тень DWM и ресайз,
  // WS_CAPTION нет — клиент занимает всё окно (WM_NCCALCSIZE), ресайз и
  // перетаскивание за верхнюю полосу — вручную в WM_NCHITTEST.
  // WS_SYSMENU|WS_MINIMIZEBOX обязательны: без системного меню Windows не
  // стартует цикл перетаскивания окна за HTCAPTION (репорт «окно не
  // двигается», v1.1.x). Автозаголовок, который Win10 добавляет вместе с
  // WS_SYSMENU, срезается сразу после Create (блок ниже).
  HWND window = CreateWindow(
      window_class, title.c_str(),
      WS_OVERLAPPED | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX,
      Scale(origin.x, scale_factor), Scale(origin.y, scale_factor),
      Scale(size.width, scale_factor), Scale(size.height, scale_factor),
      nullptr, nullptr, GetModuleHandle(nullptr), this);

  if (!window) {
    return false;
  }

  // Тень вокруг безрамочного окна.
  const MARGINS shadow = {0, 0, 1, 0};
  DwmExtendFrameIntoClientArea(window, &shadow);

  UpdateTheme(window);

  return OnCreate();
}

bool Win32Window::Show() {
  return ShowWindow(window_handle_, SW_SHOWNORMAL);
}

// static
LRESULT CALLBACK Win32Window::WndProc(HWND const window,
                                      UINT const message,
                                      WPARAM const wparam,
                                      LPARAM const lparam) noexcept {
  if (message == WM_NCCREATE) {
    auto window_struct = reinterpret_cast<CREATESTRUCT*>(lparam);
    SetWindowLongPtr(window, GWLP_USERDATA,
                     reinterpret_cast<LONG_PTR>(window_struct->lpCreateParams));

    auto that = static_cast<Win32Window*>(window_struct->lpCreateParams);
    EnableFullDpiSupportIfAvailable(window);
    that->window_handle_ = window;
  } else if (Win32Window* that = GetThisFromHandle(window)) {
    return that->MessageHandler(window, message, wparam, lparam);
  }

  return DefWindowProc(window, message, wparam, lparam);
}

LRESULT
Win32Window::MessageHandler(HWND hwnd,
                            UINT const message,
                            WPARAM const wparam,
                            LPARAM const lparam) noexcept {
  switch (message) {
    case WM_DESTROY:
      window_handle_ = nullptr;
      Destroy();
      if (quit_on_close_) {
        PostQuitMessage(0);
      }
      return 0;

    case WM_ERASEBKGND:
      // Фон рисует сам Flutter (первый кадр готов до Show). Запрет
      // стирания убирает белую вспышку при активации из панели задач,
      // Alt+Tab и восстановлении из свёрнутого.
      return 1;

    case WM_NCACTIVATE:
      // Активация borderless с WS_SYSMENU перерисовывает «шапку»
      // стандартной рамкой — видна белая полоса на кадр. Приём против
      // мигания: lParam = -1 просит систему не перерисовывать
      // неклиентскую область при смене активности.
      return DefWindowProc(hwnd, message, wparam, static_cast<LPARAM>(-1));

    case WM_NCCALCSIZE:
      // Безрамочное окно: клиентская область = всё окно. Края ресайза
      // обрабатываются вручную в WM_NCHITTEST.
      if (wparam) {
        return 0;
      }
      break;

    case WM_NCHITTEST: {
      // Края — ресайз; верхняя полоса — невидимый титул (перетаскивание),
      // как у macOS с fullSizeContentView.
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ::ScreenToClient(hwnd, &pt);
      const RECT rc = GetClientArea();
      const UINT dpi = ::GetDpiForWindow(hwnd);
      const LONG border = MulDiv(8, static_cast<int>(dpi), 96);
      const LONG caption = MulDiv(44, static_cast<int>(dpi), 96);
      const bool left = pt.x < border;
      const bool right = pt.x >= rc.right - border;
      const bool top = pt.y < border;
      const bool bottom = pt.y >= rc.bottom - border;
      if (top && left) return HTTOPLEFT;
      if (top && right) return HTTOPRIGHT;
      if (bottom && left) return HTBOTTOMLEFT;
      if (bottom && right) return HTBOTTOMRIGHT;
      if (left) return HTLEFT;
      if (right) return HTRIGHT;
      if (top) return HTTOP;
      if (bottom) return HTBOTTOM;
      if (pt.y < caption) return HTCAPTION;
      break;
    }

    case WM_NCLBUTTONDBLCLK:
      // Двойной клик по титулу не разворачивает окно (паритет с macOS).
      return 0;

    case WM_GETMINMAXINFO: {
      // Габариты: 560×670 логических, ресайз 0.8×–1.4× (паритет с macOS).
      auto* mmi = reinterpret_cast<MINMAXINFO*>(lparam);
      const UINT dpi = ::GetDpiForWindow(hwnd);
      mmi->ptMinTrackSize.x = MulDiv(448, static_cast<int>(dpi), 96);
      mmi->ptMinTrackSize.y = MulDiv(536, static_cast<int>(dpi), 96);
      mmi->ptMaxTrackSize.x = MulDiv(784, static_cast<int>(dpi), 96);
      mmi->ptMaxTrackSize.y = MulDiv(938, static_cast<int>(dpi), 96);
      return 0;
    }

    case WM_DPICHANGED: {
      auto newRectSize = reinterpret_cast<RECT*>(lparam);
      LONG newWidth = newRectSize->right - newRectSize->left;
      LONG newHeight = newRectSize->bottom - newRectSize->top;

      SetWindowPos(hwnd, nullptr, newRectSize->left, newRectSize->top, newWidth,
                   newHeight, SWP_NOZORDER | SWP_NOACTIVATE);

      return 0;
    }
    case WM_NCLBUTTONDOWN: {
      // Ресайз за края — своим циклом (RunSizeLoop): системный SC_SIZE-цикл
      // менял размер рваными ступенями. Значения HTLEFT..HTBOTTOMRIGHT
      // совпадают с WMSZ_*.
      const UINT edge = static_cast<UINT>(wparam);
      if (edge >= HTSIZEFIRST && edge <= HTSIZELAST) {
        RunSizeLoop(hwnd, edge, this);
        return 0;
      }
      break;
    }

    case WM_SIZING: {
      // Fallback (программный ресайз снаружи): пропорция 560:670, лимиты
      // 0.8×–1.4× — см. ApplySizeConstraints.
      ApplySizeConstraints(reinterpret_cast<RECT*>(lparam),
                           static_cast<UINT>(wparam),
                           ::GetDpiForWindow(hwnd));
      return TRUE;
    }

    case WM_SIZE: {
      if (g_in_size_loop) {
        // Размером Flutter-view в это время управляет RunSizeLoop
        // (throttled-синхронизация): иначе каждый шаг ждал пересоздания
        // свап-чейна и рамка отставала от мыши.
        return 0;
      }
      RECT rect = GetClientArea();
      if (child_content_ != nullptr) {
        // Size and position the child window.
        MoveWindow(child_content_, rect.left, rect.top, rect.right - rect.left,
                   rect.bottom - rect.top, TRUE);
      }
      return 0;
    }

    case WM_ACTIVATE:
      if (child_content_ != nullptr) {
        SetFocus(child_content_);
      }
      return 0;

    case WM_DWMCOLORIZATIONCOLORCHANGED:
      UpdateTheme(hwnd);
      return 0;
  }

  return DefWindowProc(window_handle_, message, wparam, lparam);
}

void Win32Window::Destroy() {
  OnDestroy();

  if (window_handle_) {
    DestroyWindow(window_handle_);
    window_handle_ = nullptr;
  }
  if (g_active_window_count == 0) {
    WindowClassRegistrar::GetInstance()->UnregisterWindowClass();
  }
}

Win32Window* Win32Window::GetThisFromHandle(HWND const window) noexcept {
  return reinterpret_cast<Win32Window*>(
      GetWindowLongPtr(window, GWLP_USERDATA));
}

void Win32Window::SetChildContent(HWND content) {
  child_content_ = content;
  SetParent(content, window_handle_);
  RECT frame = GetClientArea();

  MoveWindow(content, frame.left, frame.top, frame.right - frame.left,
             frame.bottom - frame.top, true);

  SetFocus(child_content_);
}

RECT Win32Window::GetClientArea() {
  RECT frame;
  GetClientRect(window_handle_, &frame);
  return frame;
}

HWND Win32Window::GetHandle() {
  return window_handle_;
}

void Win32Window::SetQuitOnClose(bool quit_on_close) {
  quit_on_close_ = quit_on_close;
}

bool Win32Window::OnCreate() {
  // No-op; provided for subclasses.
  return true;
}

void Win32Window::OnDestroy() {
  // No-op; provided for subclasses.
}

void Win32Window::UpdateTheme(HWND const window) {
  DWORD light_mode;
  DWORD light_mode_size = sizeof(light_mode);
  LSTATUS result = RegGetValue(HKEY_CURRENT_USER, kGetPreferredBrightnessRegKey,
                               kGetPreferredBrightnessRegValue,
                               RRF_RT_REG_DWORD, nullptr, &light_mode,
                               &light_mode_size);

  if (result == ERROR_SUCCESS) {
    BOOL enable_dark_mode = light_mode == 0;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE,
                          &enable_dark_mode, sizeof(enable_dark_mode));
  }
}
