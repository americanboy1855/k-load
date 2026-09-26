#include "drag_out.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <ctime>
#include <fstream>
#include <string>

namespace {

// Диагностика drag-out → engine.log (в %APPDATA%\K LOAD): каждый запуск
// переноса пишет результат и эффект — «не тянется» всегда диагностируемо.
void LogDrag (const std::wstring& path, HRESULT hr, DWORD effect)
{
    char* appdata = nullptr;
    size_t len = 0;
    if (_dupenv_s (&appdata, &len, "APPDATA") == 0 && appdata != nullptr)
    {
        std::ofstream out (std::string (appdata) + "\\K LOAD\\engine.log",
                           std::ios::app);
        if (out.good())
        {
            char stamp[32] = {};
            const std::time_t t = std::time (nullptr);
            std::tm tm {};
            localtime_s (&tm, &t);
            std::strftime (stamp, sizeof (stamp), "%Y-%m-%d %H:%M:%S", &tm);
            char line[512] = {};
            std::string narrow;
            narrow.reserve (path.size());
            for (wchar_t wc : path)
                narrow.push_back (wc >= 0 && wc <= 0x7f ? static_cast<char> (wc)
                                                        : '?');
            std::snprintf (line, sizeof (line),
                           "[%s] drag-out: hr=0x%08lX effect=%lu file=%s\n",
                           stamp, static_cast<unsigned long> (hr),
                           static_cast<unsigned long> (effect),
                           narrow.c_str());
            out << line;
        }
        free (appdata);
    }
}

// Стандартный источник: левая кнопка отпущена — дроп, Esc — отмена.
class DropSource : public IDropSource
{
public:
    HRESULT STDMETHODCALLTYPE QueryContinueDrag (BOOL escapePressed, DWORD keys) override
    {
        if (escapePressed) return DRAGDROP_S_CANCEL;
        if (! (keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GiveFeedback (DWORD) override
    { return DRAGDROP_S_USEDEFAULTCURSORS; }

    HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void** out) override
    {
        if (out == nullptr) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropSource)
        {
            *out = static_cast<IDropSource*> (this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --ref_;
        if (left == 0) delete this;
        return left;
    }

private:
    long ref_ = 1;
};

} // namespace

HRESULT kload::DragOutFile (const std::wstring& path)
{
    // Нативный shell-объект файла (тот же, что создаёт Проводник при
    // перетаскивании): цели-оболочки (рабочий стол, папки, браузеры) и
    // DAW принимают его без ограничений. Самодельный CF_HDROP-объект
    // отвергался целями-оболочками (перечёркнутый круг).
    IShellItem* item = nullptr;
    HRESULT hr = ::SHCreateItemFromParsingName (path.c_str(), nullptr,
                                                IID_PPV_ARGS (&item));
    IDataObject* data = nullptr;
    if (SUCCEEDED (hr) && item != nullptr)
    {
        hr = item->BindToHandler (nullptr, BHID_DataObject,
                                  IID_PPV_ARGS (&data));
        item->Release();
        item = nullptr;
    }

    if (FAILED (hr) || data == nullptr)
    {
        if (item != nullptr) item->Release();
        LogDrag (path, FAILED (hr) ? hr : E_FAIL, 0);
        return FAILED (hr) ? hr : E_FAIL;
    }

    auto* source = new DropSource();
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT hr_drop = ::DoDragDrop (data, source, DROPEFFECT_COPY,
                                          &effect);
    LogDrag (path, hr_drop, effect);
    data->Release();
    source->Release();
    return hr_drop;
}
