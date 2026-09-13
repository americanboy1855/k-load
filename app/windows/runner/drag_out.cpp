#include "drag_out.h"

#include <shellapi.h>
#include <shlobj.h>

namespace {

// Минимальный IDataObject: один формат CF_HDROP на чтение плюс приём
// «Preferred DropEffect» от приёмника (Проводник пишет его через SetData).
class FileDataObject : public IDataObject
{
public:
    explicit FileDataObject (HGLOBAL hdrop) : hdrop_ (hdrop) {}
    ~FileDataObject() { if (hdrop_) ::GlobalFree (hdrop_); }

    HRESULT STDMETHODCALLTYPE GetData (FORMATETC* format, STGMEDIUM* medium) override
    {
        if (format == nullptr || medium == nullptr) return E_POINTER;
        if (format->cfFormat != CF_HDROP || ! (format->tymed & TYMED_HGLOBAL))
            return DV_E_FORMATETC;
        if (hdrop_ == nullptr) return DV_E_FORMATETC;

        const SIZE_T size = ::GlobalSize (hdrop_);
        HGLOBAL copy = ::GlobalAlloc (GMEM_MOVEABLE, size);
        if (copy == nullptr) return STG_E_MEDIUMFULL;

        const void* src = ::GlobalLock (hdrop_);
        void* dst = ::GlobalLock (copy);
        if (src == nullptr || dst == nullptr)
        {
            if (dst) ::GlobalUnlock (copy);
            if (src) ::GlobalUnlock (hdrop_);
            ::GlobalFree (copy);
            return E_FAIL;
        }
        memcpy (dst, src, size);
        ::GlobalUnlock (copy);
        ::GlobalUnlock (hdrop_);

        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = copy;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere (FORMATETC*, STGMEDIUM*) override
    { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE QueryGetData (FORMATETC* format) override
    {
        if (format == nullptr) return E_POINTER;
        if (format->cfFormat == CF_HDROP && (format->tymed & TYMED_HGLOBAL))
            return S_OK;
        if (format->cfFormat == dropEffect_)
            return S_OK;
        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc (FORMATETC*, FORMATETC* out) override
    {
        if (out != nullptr) out->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }

    HRESULT STDMETHODCALLTYPE SetData (FORMATETC* format, STGMEDIUM* medium, BOOL release) override
    {
        // Эффект от приёмника нам не нужен дальше — принимаем и освобождаем.
        if (format != nullptr && format->cfFormat == dropEffect_
            && medium != nullptr && medium->hGlobal != nullptr)
        {
            if (release) ::ReleaseStgMedium (medium);
            return S_OK;
        }
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc (DWORD, IEnumFORMATETC** out) override
    {
        if (out == nullptr) return E_POINTER;
        *out = nullptr;
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE DAdvise (FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
    { return OLE_E_ADVISENOTSUPPORTED; }

    HRESULT STDMETHODCALLTYPE DUnadvise (DWORD) override
    { return OLE_E_ADVISENOTSUPPORTED; }

    HRESULT STDMETHODCALLTYPE EnumDAdvise (IEnumSTATDATA** out) override
    {
        if (out == nullptr) return E_POINTER;
        *out = nullptr;
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void** out) override
    {
        if (out == nullptr) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDataObject)
        {
            *out = static_cast<IDataObject*> (this);
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
    HGLOBAL hdrop_ = nullptr;
    long ref_ = 1;
    const CLIPFORMAT dropEffect_
        = static_cast<CLIPFORMAT> (::RegisterClipboardFormatW (CFSTR_PREFERREDDROPEFFECT));
};

// Стандартный источник: левая кнопка отпущена — сброс, Esc — отмена.
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
    // DROPFILES + wide-строка пути + двойной нуль-терминатор.
    const SIZE_T pathBytes = (path.size() + 1) * sizeof (wchar_t);
    const SIZE_T total = sizeof (DROPFILES) + pathBytes + sizeof (wchar_t);
    HGLOBAL hdrop = ::GlobalAlloc (GMEM_MOVEABLE | GMEM_ZEROINIT, total);
    if (hdrop == nullptr) return E_OUTOFMEMORY;

    auto* df = static_cast<DROPFILES*> (::GlobalLock (hdrop));
    if (df == nullptr)
    {
        ::GlobalFree (hdrop);
        return E_FAIL;
    }
    df->pFiles = sizeof (DROPFILES);
    df->fWide = TRUE;
    memcpy (reinterpret_cast<char*> (df) + sizeof (DROPFILES), path.c_str(), pathBytes);
    ::GlobalUnlock (hdrop);

    auto* data = new FileDataObject (hdrop); // владение hdrop переходит
    auto* source = new DropSource();
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT hr = ::DoDragDrop (data, source, DROPEFFECT_COPY, &effect);
    data->Release();
    source->Release();
    return hr;
}
