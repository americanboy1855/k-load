#pragma once
// Построение CF_HDROP-буфера для системного перетаскивания файлов.
// Требования оболочки (рабочий стол / Проводник строгие):
//  - заголовок DROPFILES: pFiles = sizeof(DROPFILES), fWide = TRUE (UTF-16);
//  - список путей в UTF-16, каждый завершён нулём;
//  - список завершается ДОПОЛНИТЕЛЬНЫМ нулевым wchar_t (двойной ноль).
// Заголовок самодостаточный, покрывается тестами (run-geometry-tests.cmd).
#include <windows.h>

#include <vector>
#include <string>

namespace drag {

// Локальная копия WIN32 FIND DATA-независимого заголовка DROPFILES
// (DWORD pFiles; BOOL fWide; POINT pt) — 20 байт на x64.
struct HdropHeader {
  DWORD pFiles;
  BOOL fWide;
  POINT pt;
};
static_assert(sizeof(HdropHeader) == 16, "unexpected DROPFILES layout");

inline std::vector<BYTE> BuildHdropBuffer(const std::wstring& path) {
  std::vector<BYTE> out(sizeof(HdropHeader) + (path.size() + 2) * sizeof(wchar_t),
                        0);
  auto* df = reinterpret_cast<HdropHeader*>(out.data());
  df->pFiles = sizeof(HdropHeader);
  df->fWide = TRUE;
  memcpy(out.data() + sizeof(HdropHeader), path.c_str(),
         (path.size() + 1) * sizeof(wchar_t));
  // последний wchar в буфере остаётся нулём (zero-init) — двойной ноль
  return out;
}

// Порог старта переноса: суммарный сдвиг курсора ≥ 8 px в любом
// направлении (репорт «вертикальная тяна на стол не начинала перенос»).
inline bool DragThresholdReached(LONG dx, LONG dy) {
  return dx * dx + dy * dy >= 64;
}

}  // namespace drag
