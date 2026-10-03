#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <windows.h>

#include "flutter_window.h"
#include "utils.h"

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev,
                      _In_ wchar_t *command_line, _In_ int show_command) {
  // Attach to console when present (e.g., 'flutter run') or create a
  // new console when running with a debugger.
  if (!::AttachConsole(ATTACH_PARENT_PROCESS) && ::IsDebuggerPresent()) {
    CreateAndAttachConsole();
  }

  // OLE поверх COM STA: обязателен для DoDragDrop (drag-out скачанных
  // файлов из диспетчера) и корректен для остальных COM-потребителей.
  ::OleInitialize(nullptr);

  flutter::DartProject project(L"data");

  std::vector<std::string> command_line_arguments =
      GetCommandLineArguments();

  project.set_dart_entrypoint_arguments(std::move(command_line_arguments));

  FlutterWindow window(project);
  // Логические 532×637 — размер зафиксирован по выбору владельца
  // (репорт «зафиксировать окно в текущем размере», замер 532×637).
  const LONG kWindowW = 532;
  const LONG kWindowH = 637;

  // W-3: окно по центру рабочей области монитора курсора (паритет с mac
  // v1.3.0): MonitorFromPoint под мышь → GetMonitorInfo (рабочая область,
  // без панели задач) → центр минус половина размера в физических пикселях
  // (DPI учитывается тем же масштабом, что и в Win32Window::Create).
  POINT cursor{};
  ::GetCursorPos(&cursor);
  HMONITOR monitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!::GetMonitorInfo(monitor, &mi)) {
    mi.rcWork = {0, 0, 1920, 1080};
  }
  const UINT dpi = FlutterDesktopGetDpiForMonitor(monitor);
  const double scale = dpi / 96.0;
  const LONG wPix = static_cast<LONG>(kWindowW * scale);
  const LONG hPix = static_cast<LONG>(kWindowH * scale);
  const int cx = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - wPix) / 2;
  const int cy = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - hPix) / 2;
  Win32Window::Point origin(cx, cy);
  Win32Window::Size size(kWindowW, kWindowH);
  if (!window.Create(L"K LOAD", origin, size)) {
    return EXIT_FAILURE;
  }
  window.SetQuitOnClose(true);

  ::MSG msg;
  while (::GetMessage(&msg, nullptr, 0, 0)) {
    ::TranslateMessage(&msg);
    ::DispatchMessage(&msg);
  }

  ::OleUninitialize();
  return EXIT_SUCCESS;
}
