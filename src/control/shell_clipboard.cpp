#include "shell_clipboard.hpp"

#include <cstring>
#include <algorithm>
#include <filesystem>
#include <type_traits>

namespace cppdesk {

#if defined(_WIN32)

bool ShellClipboard::initializeOle() {
    HRESULT hr = OleInitialize(nullptr);
    return SUCCEEDED(hr);
}

void ShellClipboard::uninitializeOle() {
    OleUninitialize();
}

UINT ShellClipboard::getFileGroupDescriptorWFormat() {
    static UINT cf = RegisterClipboardFormatW(L"FileGroupDescriptorW");
    return cf;
}

UINT ShellClipboard::getFileContentsFormat() {
    static UINT cf = RegisterClipboardFormatW(L"FileContents");
    return cf;
}

UINT ShellClipboard::getPreferredDropEffectFormat() {
    static UINT cf = RegisterClipboardFormatW(L"Preferred DropEffect");
    return cf;
}

bool ShellClipboard::getLocalClipboardFiles(std::vector<VirtualFileEntry>& outFiles, std::vector<std::wstring>& outFullPaths) {
    outFiles.clear();
    outFullPaths.clear();
    if (!IsClipboardFormatAvailable(CF_HDROP)) return false;
    if (!OpenClipboard(nullptr)) return false;

    HANDLE hDrop = GetClipboardData(CF_HDROP);
    if (!hDrop) {
        CloseClipboard();
        return false;
    }

    HDROP h = static_cast<HDROP>(hDrop);
    UINT count = DragQueryFileW(h, 0xFFFFFFFF, nullptr, 0);
    uint32_t fileIndex = 0;
    for (UINT i = 0; i < count; ++i) {
        wchar_t pathBuf[MAX_PATH] = {};
        if (DragQueryFileW(h, i, pathBuf, MAX_PATH) > 0) {
            std::filesystem::path p(pathBuf);
            std::error_code ec;
            if (std::filesystem::is_regular_file(p, ec)) {
                uint64_t sz = std::filesystem::file_size(p, ec);
                WIN32_FILE_ATTRIBUTE_DATA attr{};
                DWORD fileAttr = FILE_ATTRIBUTE_NORMAL;
                if (GetFileAttributesExW(pathBuf, GetFileExInfoStandard, &attr)) {
                    fileAttr = attr.dwFileAttributes;
                }
                VirtualFileEntry entry{};
                entry.fileIndex = fileIndex++;
                entry.fileName = p.filename().wstring();
                entry.fileSize = sz;
                entry.fileAttributes = fileAttr;
                outFiles.push_back(std::move(entry));
                outFullPaths.push_back(pathBuf);
            }
        }
    }
    CloseClipboard();
    return !outFiles.empty();
}

bool ShellClipboard::setVirtualClipboardFiles(const std::vector<VirtualFileEntry>& files, VirtualChunkReadFn chunkReader) {
    if (files.empty()) return false;
    ShellDataObject* obj = new ShellDataObject(files, std::move(chunkReader));
    HRESULT hr = OleSetClipboard(obj);
    obj->Release();
    return SUCCEEDED(hr);
}

void ShellClipboard::clearClipboard() {
    OleSetClipboard(nullptr);
}

#endif

namespace {

template <typename StringType>
StringType resolveConflictFilenameImpl(const StringType& directory, const StringType& filename) {
    std::filesystem::path dir(directory);
    std::filesystem::path target = dir / filename;
    if (!std::filesystem::exists(target)) {
        return filename;
    }
    const auto stem = target.stem();
    const auto ext = target.extension();
    for (int i = 1; i <= 9999; ++i) {
        StringType candidate;
        if constexpr (std::is_same_v<StringType, std::wstring>) {
            candidate = stem.wstring() + L" (" + std::to_wstring(i) + L")" + ext.wstring();
        } else {
            candidate = stem.string() + " (" + std::to_string(i) + ")" + ext.string();
        }
        if (!std::filesystem::exists(dir / candidate)) {
            return candidate;
        }
    }
    return filename;
}

} // namespace

std::wstring ShellClipboard::resolveConflictFilenameW(const std::wstring& directory, const std::wstring& filename) {
    return resolveConflictFilenameImpl(directory, filename);
}

std::string ShellClipboard::resolveConflictFilename(const std::string& directory, const std::string& filename) {
    return resolveConflictFilenameImpl(directory, filename);
}

#if defined(_WIN32)

// ---------------- VirtualFileStream ----------------

VirtualFileStream::VirtualFileStream(const VirtualFileEntry& desc, VirtualChunkReadFn chunkReader)
    : desc_(desc), chunkReader_(std::move(chunkReader)) {}

HRESULT STDMETHODCALLTYPE VirtualFileStream::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IStream || riid == IID_ISequentialStream) {
        *ppvObject = static_cast<IStream*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE VirtualFileStream::AddRef() {
    return refCount_.fetch_add(1) + 1;
}

ULONG STDMETHODCALLTYPE VirtualFileStream::Release() {
    ULONG r = refCount_.fetch_sub(1) - 1;
    if (r == 0) delete this;
    return r;
}

HRESULT STDMETHODCALLTYPE VirtualFileStream::Read(void* pv, ULONG cb, ULONG* pcbRead) {
    if (!pv) return E_POINTER;
    if (cb == 0 || currentOffset_ >= desc_.fileSize) {
        if (pcbRead) *pcbRead = 0;
        return S_OK;
    }
    uint64_t remaining = desc_.fileSize - currentOffset_;
    uint32_t toRead = static_cast<uint32_t>(std::min<uint64_t>(cb, remaining));

    std::vector<uint8_t> chunkData;
    if (!chunkReader_ || !chunkReader_(desc_.fileIndex, currentOffset_, toRead, chunkData)) {
        if (pcbRead) *pcbRead = 0;
        return E_FAIL;
    }

    size_t actual = std::min<size_t>(toRead, chunkData.size());
    std::memcpy(pv, chunkData.data(), actual);
    currentOffset_ += actual;
    if (pcbRead) *pcbRead = static_cast<ULONG>(actual);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE VirtualFileStream::Write(const void* /*pv*/, ULONG /*cb*/, ULONG* /*pcbWritten*/) {
    return STG_E_ACCESSDENIED;
}

HRESULT STDMETHODCALLTYPE VirtualFileStream::Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER* plibNewPosition) {
    int64_t newPos = 0;
    switch (dwOrigin) {
        case STREAM_SEEK_SET: newPos = dlibMove.QuadPart; break;
        case STREAM_SEEK_CUR: newPos = static_cast<int64_t>(currentOffset_) + dlibMove.QuadPart; break;
        case STREAM_SEEK_END: newPos = static_cast<int64_t>(desc_.fileSize) + dlibMove.QuadPart; break;
        default: return STG_E_INVALIDFUNCTION;
    }
    if (newPos < 0) return STG_E_INVALIDFUNCTION;
    currentOffset_ = static_cast<uint64_t>(newPos);
    if (plibNewPosition) plibNewPosition->QuadPart = currentOffset_;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE VirtualFileStream::SetSize(ULARGE_INTEGER /*libNewSize*/) {
    return STG_E_ACCESSDENIED;
}

HRESULT STDMETHODCALLTYPE VirtualFileStream::CopyTo(IStream* /*pstm*/, ULARGE_INTEGER /*cb*/, ULARGE_INTEGER* /*pcbRead*/, ULARGE_INTEGER* /*pcbWritten*/) {
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE VirtualFileStream::Commit(DWORD /*grfCommitFlags*/) {
    return S_OK;
}

HRESULT STDMETHODCALLTYPE VirtualFileStream::Revert() {
    return S_OK;
}

HRESULT STDMETHODCALLTYPE VirtualFileStream::LockRegion(ULARGE_INTEGER /*libOffset*/, ULARGE_INTEGER /*cb*/, DWORD /*dwLockType*/) {
    return STG_E_INVALIDFUNCTION;
}

HRESULT STDMETHODCALLTYPE VirtualFileStream::UnlockRegion(ULARGE_INTEGER /*libOffset*/, ULARGE_INTEGER /*cb*/, DWORD /*dwLockType*/) {
    return STG_E_INVALIDFUNCTION;
}

HRESULT STDMETHODCALLTYPE VirtualFileStream::Stat(STATSTG* pstatstg, DWORD grfStatFlag) {
    if (!pstatstg) return E_POINTER;
    std::memset(pstatstg, 0, sizeof(STATSTG));
    pstatstg->type = STGTY_STREAM;
    pstatstg->cbSize.QuadPart = desc_.fileSize;
    if (!(grfStatFlag & STATFLAG_NONAME)) {
        size_t len = desc_.fileName.size();
        pstatstg->pwcsName = static_cast<LPOLESTR>(CoTaskMemAlloc((len + 1) * sizeof(wchar_t)));
        if (pstatstg->pwcsName) {
            wcscpy(pstatstg->pwcsName, desc_.fileName.c_str());
        }
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE VirtualFileStream::Clone(IStream** /*ppstm*/) {
    return E_NOTIMPL;
}

// ---------------- ShellEnumFormatEtc ----------------

ShellEnumFormatEtc::ShellEnumFormatEtc(const std::vector<FORMATETC>& formats)
    : formats_(formats) {}

HRESULT STDMETHODCALLTYPE ShellEnumFormatEtc::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IEnumFORMATETC) {
        *ppvObject = static_cast<IEnumFORMATETC*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE ShellEnumFormatEtc::AddRef() {
    return refCount_.fetch_add(1) + 1;
}

ULONG STDMETHODCALLTYPE ShellEnumFormatEtc::Release() {
    ULONG r = refCount_.fetch_sub(1) - 1;
    if (r == 0) delete this;
    return r;
}

HRESULT STDMETHODCALLTYPE ShellEnumFormatEtc::Next(ULONG celt, FORMATETC* rgelt, ULONG* pceltFetched) {
    if (!rgelt) return E_POINTER;
    if (celt > 1 && !pceltFetched) return E_INVALIDARG;

    ULONG fetched = 0;
    while (currentIndex_ < formats_.size() && fetched < celt) {
        rgelt[fetched] = formats_[currentIndex_];
        ++currentIndex_;
        ++fetched;
    }
    if (pceltFetched) *pceltFetched = fetched;
    return (fetched == celt) ? S_OK : S_FALSE;
}

HRESULT STDMETHODCALLTYPE ShellEnumFormatEtc::Skip(ULONG celt) {
    currentIndex_ = std::min(formats_.size(), currentIndex_ + celt);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ShellEnumFormatEtc::Reset() {
    currentIndex_ = 0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ShellEnumFormatEtc::Clone(IEnumFORMATETC** ppenum) {
    if (!ppenum) return E_POINTER;
    *ppenum = new ShellEnumFormatEtc(formats_);
    return S_OK;
}

// ---------------- ShellDataObject ----------------

ShellDataObject::ShellDataObject(const std::vector<VirtualFileEntry>& files, VirtualChunkReadFn chunkReader)
    : files_(files), chunkReader_(std::move(chunkReader)) {}

HRESULT STDMETHODCALLTYPE ShellDataObject::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDataObject) {
        *ppvObject = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE ShellDataObject::AddRef() {
    return refCount_.fetch_add(1) + 1;
}

ULONG STDMETHODCALLTYPE ShellDataObject::Release() {
    ULONG r = refCount_.fetch_sub(1) - 1;
    if (r == 0) delete this;
    return r;
}

HRESULT STDMETHODCALLTYPE ShellDataObject::GetData(FORMATETC* pformatetcIn, STGMEDIUM* pmedium) {
    if (!pformatetcIn || !pmedium) return E_INVALIDARG;
    std::memset(pmedium, 0, sizeof(STGMEDIUM));

    UINT cfDesc = ShellClipboard::getFileGroupDescriptorWFormat();
    UINT cfContents = ShellClipboard::getFileContentsFormat();
    UINT cfDropEffect = ShellClipboard::getPreferredDropEffectFormat();

    if (pformatetcIn->cfFormat == cfDesc && (pformatetcIn->tymed & TYMED_HGLOBAL)) {
        size_t count = files_.size();
        size_t allocSize = sizeof(FILEGROUPDESCRIPTORW) + (count > 0 ? (count - 1) : 0) * sizeof(FILEDESCRIPTORW);
        HGLOBAL hGlobal = GlobalAlloc(GHND, allocSize);
        if (!hGlobal) return E_OUTOFMEMORY;

        FILEGROUPDESCRIPTORW* pGroup = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(hGlobal));
        if (!pGroup) {
            GlobalFree(hGlobal);
            return E_OUTOFMEMORY;
        }

        pGroup->cItems = static_cast<UINT>(count);
        for (size_t i = 0; i < count; ++i) {
            FILEDESCRIPTORW& desc = pGroup->fgd[i];
            desc.dwFlags = FD_FILESIZE | FD_ATTRIBUTES | FD_PROGRESSUI;
            desc.dwFileAttributes = files_[i].fileAttributes;
            desc.nFileSizeLow = static_cast<DWORD>(files_[i].fileSize & 0xFFFFFFFF);
            desc.nFileSizeHigh = static_cast<DWORD>(files_[i].fileSize >> 32);
            wcsncpy(desc.cFileName, files_[i].fileName.c_str(), MAX_PATH - 1);
            desc.cFileName[MAX_PATH - 1] = L'\0';
        }
        GlobalUnlock(hGlobal);

        pmedium->tymed = TYMED_HGLOBAL;
        pmedium->hGlobal = hGlobal;
        pmedium->pUnkForRelease = nullptr;
        return S_OK;
    }

    if (pformatetcIn->cfFormat == cfContents && (pformatetcIn->tymed & TYMED_ISTREAM)) {
        LONG idx = pformatetcIn->lindex;
        if (idx < 0 || static_cast<size_t>(idx) >= files_.size()) {
            return DV_E_LINDEX;
        }
        VirtualFileStream* stm = new VirtualFileStream(files_[idx], chunkReader_);
        pmedium->tymed = TYMED_ISTREAM;
        pmedium->pstm = stm;
        pmedium->pUnkForRelease = nullptr;
        return S_OK;
    }

    if (pformatetcIn->cfFormat == cfDropEffect && (pformatetcIn->tymed & TYMED_HGLOBAL)) {
        HGLOBAL hGlobal = GlobalAlloc(GHND, sizeof(DWORD));
        if (!hGlobal) return E_OUTOFMEMORY;
        DWORD* pEffect = static_cast<DWORD*>(GlobalLock(hGlobal));
        if (pEffect) {
            *pEffect = DROPEFFECT_COPY;
            GlobalUnlock(hGlobal);
        }
        pmedium->tymed = TYMED_HGLOBAL;
        pmedium->hGlobal = hGlobal;
        pmedium->pUnkForRelease = nullptr;
        return S_OK;
    }

    return DV_E_FORMATETC;
}

HRESULT STDMETHODCALLTYPE ShellDataObject::GetDataHere(FORMATETC* /*pformatetc*/, STGMEDIUM* /*pmedium*/) {
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ShellDataObject::QueryGetData(FORMATETC* pformatetc) {
    if (!pformatetc) return E_INVALIDARG;

    UINT cfDesc = ShellClipboard::getFileGroupDescriptorWFormat();
    UINT cfContents = ShellClipboard::getFileContentsFormat();
    UINT cfDropEffect = ShellClipboard::getPreferredDropEffectFormat();

    if (pformatetc->cfFormat == cfDesc && (pformatetc->tymed & TYMED_HGLOBAL)) {
        return S_OK;
    }
    if (pformatetc->cfFormat == cfContents && (pformatetc->tymed & TYMED_ISTREAM)) {
        if (pformatetc->lindex == -1 || (pformatetc->lindex >= 0 && static_cast<size_t>(pformatetc->lindex) < files_.size())) {
            return S_OK;
        }
        return DV_E_LINDEX;
    }
    if (pformatetc->cfFormat == cfDropEffect && (pformatetc->tymed & TYMED_HGLOBAL)) {
        return S_OK;
    }

    return DV_E_FORMATETC;
}

HRESULT STDMETHODCALLTYPE ShellDataObject::GetCanonicalFormatEtc(FORMATETC* /*pformatectIn*/, FORMATETC* pformatetcOut) {
    if (!pformatetcOut) return E_INVALIDARG;
    pformatetcOut->ptd = nullptr;
    return DATA_S_SAMEFORMATETC;
}

HRESULT STDMETHODCALLTYPE ShellDataObject::SetData(FORMATETC* /*pformatetc*/, STGMEDIUM* /*pmedium*/, BOOL /*fRelease*/) {
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ShellDataObject::EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC** ppenumFormatEtc) {
    if (!ppenumFormatEtc) return E_POINTER;
    if (dwDirection == DATADIR_GET) {
        std::vector<FORMATETC> formats;
        FORMATETC f1{};
        f1.cfFormat = ShellClipboard::getFileGroupDescriptorWFormat();
        f1.dwAspect = DVASPECT_CONTENT;
        f1.lindex = -1;
        f1.tymed = TYMED_HGLOBAL;
        formats.push_back(f1);

        FORMATETC f2{};
        f2.cfFormat = ShellClipboard::getFileContentsFormat();
        f2.dwAspect = DVASPECT_CONTENT;
        f2.lindex = -1;
        f2.tymed = TYMED_ISTREAM;
        formats.push_back(f2);

        FORMATETC f3{};
        f3.cfFormat = ShellClipboard::getPreferredDropEffectFormat();
        f3.dwAspect = DVASPECT_CONTENT;
        f3.lindex = -1;
        f3.tymed = TYMED_HGLOBAL;
        formats.push_back(f3);

        *ppenumFormatEtc = new ShellEnumFormatEtc(formats);
        return S_OK;
    }
    *ppenumFormatEtc = nullptr;
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ShellDataObject::DAdvise(FORMATETC* /*pformatetc*/, DWORD /*advf*/, IAdviseSink* /*pAdvSink*/, DWORD* /*pdwConnection*/) {
    return OLE_E_ADVISENOTSUPPORTED;
}

HRESULT STDMETHODCALLTYPE ShellDataObject::DUnadvise(DWORD /*dwConnection*/) {
    return OLE_E_ADVISENOTSUPPORTED;
}

HRESULT STDMETHODCALLTYPE ShellDataObject::EnumDAdvise(IEnumSTATDATA** /*ppenumAdvise*/) {
    return OLE_E_ADVISENOTSUPPORTED;
}

#endif

} // namespace cppdesk
