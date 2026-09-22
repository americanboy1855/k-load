#ifndef RUNNER_DROP_TARGET_H_
#define RUNNER_DROP_TARGET_H_

#include <windows.h>

#include <functional>
#include <string>

namespace kload {

// OLE-приёмник перетаскивания для окна Flutter: веб-адрес из браузеров
// (UniformResourceLocatorW), файлы-ярлыки .url/.webloc (CF_HDROP) и простой
// текст (CF_UNICODETEXT). Прочие файлы молча игнорируются. События уходят
// в колбэки (наведение — для подсветки зоны, полезные данные — текст).
class DropTarget : public IDropTarget {
 public:
  using HoverFn = std::function<void(bool)>;
  using PayloadFn = std::function<void(const std::string&)>;

  DropTarget(HoverFn on_hover, PayloadFn on_payload);

  // IUnknown
  IFACEMETHOD(QueryInterface)(REFIID riid, void** out) override;
  IFACEMETHOD_(ULONG, AddRef)() override;
  IFACEMETHOD_(ULONG, Release)() override;

  // IDropTarget
  IFACEMETHOD(DragEnter)(IDataObject* data, DWORD key_state, POINTL pt,
                         DWORD* effect) override;
  IFACEMETHOD(DragOver)(DWORD key_state, POINTL pt, DWORD* effect) override;
  IFACEMETHOD(DragLeave)() override;
  IFACEMETHOD(Drop)(IDataObject* data, DWORD key_state, POINTL pt,
                    DWORD* effect) override;

 private:
  LONG ref_ = 1;
  HoverFn on_hover_;
  PayloadFn on_payload_;
};

}  // namespace kload

#endif  // RUNNER_DROP_TARGET_H_
