#pragma once

// Перетаскивание файла из приложения в Проводник (и любой приёмник
// OLE-дропа). Windows-аналог macOS DragOutHelper.

#include <windows.h>
#include <string>

namespace kload
{

// Запуск драга одного файла (CF_HDROP). Вызывать, пока зажата левая
// кнопка мыши: DoDragDrop захватывает мышь и крутит собственный цикл.
// Возвращает S_OK, если файл сброшен в приёмник.
HRESULT DragOutFile (const std::wstring& path);

} // namespace kload
