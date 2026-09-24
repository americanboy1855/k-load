#ifndef RUNNER_FLUTTER_WINDOW_H_
#define RUNNER_FLUTTER_WINDOW_H_

#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <flutter/method_channel.h>
#include <flutter/encodable_value.h>

#include <memory>

#include "win32_window.h"

// A window that does nothing but host a Flutter view.
class FlutterWindow : public Win32Window {
 public:
  // Creates a new FlutterWindow hosting a Flutter view running |project|.
  explicit FlutterWindow(const flutter::DartProject& project);
  virtual ~FlutterWindow();

 protected:
  // Win32Window:
  bool OnCreate() override;
  void OnDestroy() override;
  LRESULT MessageHandler(HWND window, UINT const message, WPARAM const wparam,
                         LPARAM const lparam) noexcept override;

  // Живой ресайз (RunSizeLoop): фактический размер окна → Dart, чтобы
  // контент масштабировался каждый кадр без пересоздания свап-чейна.
  void OnLiveSize(double logical_w, double logical_h) override;
  void OnLiveSizeEnd() override;

 private:
  // Обработчик канала kload/native: chooseFolder, setZones, beginDrag,
  // chromeRects, minimize, close (Windows-реализация macOS-канала).
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  // Модальный выбор папки (IFileDialog). Пусто — отменили.
  std::string PickFolder(HWND owner);

  // Hit-test клиентской части Flutter-view: края окна — ресайз, верхняя
  // полоса — невидимый титул, кнопки окна (chromeRects_) — Flutter.
  LRESULT ViewHitTest(HWND hwnd, LPARAM lparam);

  static LRESULT CALLBACK ViewProcThunk(HWND hwnd, UINT message,
                                        WPARAM wparam, LPARAM lparam);

  // The project to run.
  flutter::DartProject project_;

  // The Flutter instance hosted by this window.
  std::unique_ptr<flutter::FlutterViewController> flutter_controller_;

  // Канал на Dart-сторону (экран).
  std::unique_ptr<
      flutter::MethodChannel<flutter::EncodableValue>>
      channel_;

  // Подкласс HWND Flutter-view (без него родитель не видит курсор
  // над клиентом — ресайз/титул не работают).
  HWND view_hwnd_ = nullptr;
  WNDPROC default_view_proc_ = nullptr;

  // Кнопки окна в координатах канваса 560×670 (приходят из Dart).
  struct ChromeRect {
    double x = 0, y = 0, w = 0, h = 0;
  };
  std::vector<ChromeRect> chrome_rects_;

  // Окно одно на процесс — статический доступ из подклассового wndproc.
  static FlutterWindow* active_window_;
};

#endif  // RUNNER_FLUTTER_WINDOW_H_
