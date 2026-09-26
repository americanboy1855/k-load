// Тесты CF_HDROP-буфера и порога старта переноса (drag_payload.h).
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "drag_payload.h"

static int g_failed = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
      ++g_failed;                                                      \
    }                                                                  \
  } while (0)

// Локальная копия DROPFILES (DWORD pFiles; BOOL fWide; POINT pt) — 20 байт
// на x64: тест не зависит от раскладки системных заголовков.
struct DfHdr {
  DWORD pFiles;
  BOOL fWide;
  POINT pt;
};

static void SingleFile() {
  const std::wstring path = L"C:\\Music\\track.mp3";
  const auto buf = drag::BuildHdropBuffer(path);
  CHECK(buf.size() == sizeof(DfHdr) + (path.size() + 2) * sizeof(wchar_t));
  const auto* df = reinterpret_cast<const DfHdr*>(buf.data());
  CHECK(df->pFiles == sizeof(DfHdr));
  CHECK(df->fWide == TRUE);
  // путь по смещению pFiles
  const wchar_t* files = reinterpret_cast<const wchar_t*>(buf.data() + df->pFiles);
  CHECK(std::wstring(files) == path);
  // двойной ноль в конце
  const wchar_t* tail = files + path.size();
  CHECK(tail[0] == 0 && tail[1] == 0);
}

static void CyrillicFile() {
  const std::wstring path = L"C:\\Музыка\\песня №1.mp3";
  const auto buf = drag::BuildHdropBuffer(path);
  const auto* df = reinterpret_cast<const DfHdr*>(buf.data());
  CHECK(df->fWide == TRUE);  // UTF-16 — кириллица не портится
  const wchar_t* files = reinterpret_cast<const wchar_t*>(buf.data() + df->pFiles);
  CHECK(std::wstring(files) == path);
  const wchar_t* tail = files + path.size();
  CHECK(tail[0] == 0 && tail[1] == 0);
}

static void MultiplePathsAreNotSupportedBySingleCall() {
  // Наш drag-out переносит ровно один файл (it.files.first) — функция
  // принимает один путь. Просто фиксируем это в тесте-документации.
  const std::wstring path = L"C:\\a.wav";  // 8 символов
  const auto buf = drag::BuildHdropBuffer(path);
  CHECK(buf.size() == sizeof(DfHdr) + 10 * sizeof(wchar_t));
}

static void Threshold() {
  CHECK(drag::DragThresholdReached(0, 0) == false);
  CHECK(drag::DragThresholdReached(7, 0) == false);
  CHECK(drag::DragThresholdReached(0, 7) == false);
  CHECK(drag::DragThresholdReached(5, 5) == false);   // |d| ~ 7.07
  CHECK(drag::DragThresholdReached(8, 0) == true);
  CHECK(drag::DragThresholdReached(0, 8) == true);
  CHECK(drag::DragThresholdReached(6, 6) == true);    // |d| ~ 8.49
  CHECK(drag::DragThresholdReached(-12, 4) == true);
}

int main() {
  SingleFile();
  CyrillicFile();
  MultiplePathsAreNotSupportedBySingleCall();
  Threshold();
  if (g_failed == 0) {
    std::printf("drag payload: все тесты пройдены\n");
    return 0;
  }
  std::printf("drag payload: провалено проверок: %d\n", g_failed);
  return 1;
}
