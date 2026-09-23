#include "drop_target.h"

#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <cwctype>
#include <string>

namespace kload {
namespace {

// STGMEDIUM с гарантированным освобождением.
struct Medium {
  STGMEDIUM value{};
  ~Medium() { ::ReleaseStgMedium(&value); }
};

std::string WideToUtf8(const wchar_t* w, size_t chars) {
  if (w == nullptr || chars == 0) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(chars),
                                      nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string s(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(chars), s.data(), n,
                        nullptr, nullptr);
  return s;
}

// Широкая строка из глобального блока (последний '\0' не входит).
std::string GlobalWide(HGLOBAL h) {
  if (h == nullptr) return {};
  const wchar_t* p = static_cast<const wchar_t*>(::GlobalLock(h));
  if (p == nullptr) return {};
  const size_t bytes = ::GlobalSize(h);
  size_t chars = bytes / sizeof(wchar_t);
  while (chars > 0 && p[chars - 1] == L'\0') --chars;
  std::string out = WideToUtf8(p, chars);
  ::GlobalUnlock(h);
  return out;
}

std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  const auto is_ws = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  while (a < b && is_ws(s[a])) ++a;
  while (b > a && is_ws(s[b - 1])) --b;
  return s.substr(a, b - a);
}

std::string ReadFileBytes(const std::wstring& path) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr) return {};
  std::string out;
  char buf[4096];
  size_t n = 0;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  fclose(f);
  return out;
}

// URL= из INI-ярлыка .url. Файл бывает ANSI и UTF-16LE — ищем оба варианта.
std::string UrlFromIni(const std::string& b) {
  // ANSI: регистр не важен.
  {
    std::string low(b.size(), '\0');
    for (size_t i = 0; i < b.size(); ++i) {
      low[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
    }
    const size_t p = low.find("url=");
    if (p != std::string::npos) {
      const std::string v = Trim(b.substr(p + 4));
      if (!v.empty()) return v;
    }
  }
  // UTF-16LE: 'U',0,'R',0,'L',0.
  for (size_t i = 0; i + 5 < b.size(); ++i) {
    if (b[i] == 'U' && b[i + 1] == '\0' && b[i + 2] == 'R' &&
        b[i + 3] == '\0' && b[i + 4] == 'L' && b[i + 5] == '\0') {
      size_t j = i + 6;
      while (j + 1 < b.size() && b[j] == ' ' && b[j + 1] == '\0') j += 2;
      if (j + 1 >= b.size() || b[j] != '=') continue;
      j += 2;
      std::string v;
      while (j < b.size() && b[j] != '\r' && b[j] != '\n' && b[j] != '\0') {
        v.push_back(b[j]);  // URL — ASCII; старший байт (UTF-16) отбрасываем
        j += 2;
      }
      const std::string t = Trim(v);
      if (!t.empty()) return t;
    }
  }
  return {};
}

// URL из .webloc (XML-plist Finder: <key>URL</key><string>…</string>).
std::string UrlFromWebloc(const std::string& b) {
  const size_t p = b.find("<string>");
  if (p == std::string::npos) return {};
  const size_t e = b.find("</string>", p);
  if (e == std::string::npos) return {};
  return Trim(b.substr(p + 8, e - p - 8));
}

}  // namespace

DropTarget::DropTarget(HoverFn on_hover, PayloadFn on_payload)
    : on_hover_(std::move(on_hover)), on_payload_(std::move(on_payload)) {}

IFACEMETHODIMP DropTarget::QueryInterface(REFIID riid, void** out) {
  if (out == nullptr) return E_POINTER;
  if (riid == IID_IUnknown || riid == IID_IDropTarget) {
    *out = static_cast<IDropTarget*>(this);
    AddRef();
    return S_OK;
  }
  *out = nullptr;
  return E_NOINTERFACE;
}

// Вне класса virtual в определении не повторяют — STDMETHODIMP_ без него.
STDMETHODIMP_(ULONG) DropTarget::AddRef() {
  return ::InterlockedIncrement(&ref_);
}

STDMETHODIMP_(ULONG) DropTarget::Release() {
  const ULONG n = ::InterlockedDecrement(&ref_);
  if (n == 0) delete this;
  return n;
}

IFACEMETHODIMP DropTarget::DragEnter(IDataObject*, DWORD, POINTL,
                                     DWORD* effect) {
  if (effect != nullptr) *effect = DROPEFFECT_COPY;
  if (on_hover_) on_hover_(true);
  return S_OK;
}

IFACEMETHODIMP DropTarget::DragOver(DWORD, POINTL, DWORD* effect) {
  if (effect != nullptr) *effect = DROPEFFECT_COPY;
  return S_OK;
}

IFACEMETHODIMP DropTarget::DragLeave() {
  if (on_hover_) on_hover_(false);
  return S_OK;
}

IFACEMETHODIMP DropTarget::Drop(IDataObject* data, DWORD, POINTL,
                                DWORD* effect) {
  if (effect != nullptr) *effect = DROPEFFECT_COPY;
  if (on_hover_) on_hover_(false);

  std::string payload;
  if (data != nullptr) {
    // 1) Файлы: .url/.webloc — ярлыки-ссылки; прочие файлы молча игнорируем.
    FORMATETC ft{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    Medium m;
    if (SUCCEEDED(data->GetData(&ft, &m.value))) {
      const HDROP hdrop = static_cast<HDROP>(m.value.hGlobal);
      if (hdrop != nullptr) {
        const UINT count = ::DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count && payload.empty(); ++i) {
          wchar_t path[MAX_PATH * 2];
          if (::DragQueryFileW(hdrop, i, path, ARRAYSIZE(path)) == 0) continue;
          std::wstring ext = path;
          const size_t dot = ext.rfind(L'.');
          ext = dot == std::wstring::npos ? L"" : ext.substr(dot + 1);
          for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
          if (ext == L"url") {
            payload = UrlFromIni(ReadFileBytes(path));
          } else if (ext == L"webloc") {
            payload = UrlFromWebloc(ReadFileBytes(path));
          }
        }
      }
    }
    // 2) Веб-адрес из браузера (UniformResourceLocatorW, широкая строка).
    if (payload.empty()) {
      FORMATETC ft2{static_cast<CLIPFORMAT>(
                        ::RegisterClipboardFormatW(CFSTR_INETURLW)),
                    nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
      Medium m2;
      if (SUCCEEDED(data->GetData(&ft2, &m2.value))) {
        payload = GlobalWide(m2.value.hGlobal);
      }
    }
    // 3) Простой текст: ссылка или название трека — решит приложение.
    if (payload.empty()) {
      FORMATETC ft3{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1,
                    TYMED_HGLOBAL};
      Medium m3;
      if (SUCCEEDED(data->GetData(&ft3, &m3.value))) {
        payload = GlobalWide(m3.value.hGlobal);
      }
    }
  }

  if (!payload.empty() && on_payload_) on_payload_(payload);
  return S_OK;
}

}  // namespace kload
