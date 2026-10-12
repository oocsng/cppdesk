#pragma once

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#endif

#include "../core/protocol.hpp"

#include <vector>
#include <string>
#include <functional>
#include <atomic>
#include <mutex>

namespace cppdesk {

// Callback for reading on-demand chunk data: returns true on success
using VirtualChunkReadFn = std::function<bool(uint32_t fileIndex, uint64_t offset, uint32_t length, std::vector<uint8_t>& outData)>;

#if defined(_WIN32)

class ShellClipboard {
public:
    static bool initializeOle();
    static void uninitializeOle();

    static UINT getFileGroupDescriptorWFormat();
    static UINT getFileContentsFormat();
    static UINT getPreferredDropEffectFormat();

    // Query physical files currently on local clipboard (CF_HDROP)
    static bool getLocalClipboardFiles(std::vector<VirtualFileEntry>& outFiles, std::vector<std::wstring>& outFullPaths);

    // Set virtual files on local clipboard via COM IDataObject
    static bool setVirtualClipboardFiles(const std::vector<VirtualFileEntry>& files, VirtualChunkReadFn chunkReader);

    // Clear local clipboard
    static void clearClipboard();

    // Conflict resolution helpers: appends (1), (2), etc. if destination already exists
    static std::wstring resolveConflictFilenameW(const std::wstring& directory, const std::wstring& filename);
    static std::string resolveConflictFilename(const std::string& directory, const std::string& filename);
};

// COM IStream implementation for virtual file contents
class VirtualFileStream : public IStream {
public:
    VirtualFileStream(const VirtualFileEntry& desc, VirtualChunkReadFn chunkReader);
    virtual ~VirtualFileStream() = default;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ISequentialStream
    HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead) override;
    HRESULT STDMETHODCALLTYPE Write(const void* pv, ULONG cb, ULONG* pcbWritten) override;

    // IStream
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER* plibNewPosition) override;
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER libNewSize) override;
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* pstm, ULARGE_INTEGER cb, ULARGE_INTEGER* pcbRead, ULARGE_INTEGER* pcbWritten) override;
    HRESULT STDMETHODCALLTYPE Commit(DWORD grfCommitFlags) override;
    HRESULT STDMETHODCALLTYPE Revert() override;
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType) override;
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType) override;
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* pstatstg, DWORD grfStatFlag) override;
    HRESULT STDMETHODCALLTYPE Clone(IStream** ppstm) override;

private:
    std::atomic<ULONG> refCount_{1};
    VirtualFileEntry   desc_;
    VirtualChunkReadFn chunkReader_;
    uint64_t           currentOffset_ = 0;
};

// COM IEnumFORMATETC implementation
class ShellEnumFormatEtc : public IEnumFORMATETC {
public:
    explicit ShellEnumFormatEtc(const std::vector<FORMATETC>& formats);
    virtual ~ShellEnumFormatEtc() = default;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IEnumFORMATETC
    HRESULT STDMETHODCALLTYPE Next(ULONG celt, FORMATETC* rgelt, ULONG* pceltFetched) override;
    HRESULT STDMETHODCALLTYPE Skip(ULONG celt) override;
    HRESULT STDMETHODCALLTYPE Reset() override;
    HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** ppenum) override;

private:
    std::atomic<ULONG>     refCount_{1};
    std::vector<FORMATETC> formats_;
    size_t                 currentIndex_ = 0;
};

// COM IDataObject implementation supporting CFSTR_FILEDESCRIPTORW and CFSTR_FILECONTENTS
class ShellDataObject : public IDataObject {
public:
    ShellDataObject(const std::vector<VirtualFileEntry>& files, VirtualChunkReadFn chunkReader);
    virtual ~ShellDataObject() = default;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDataObject
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* pformatetcIn, STGMEDIUM* pmedium) override;
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC* pformatetc, STGMEDIUM* pmedium) override;
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* pformatetc) override;
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC* pformatectIn, FORMATETC* pformatetcOut) override;
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* pformatetc, STGMEDIUM* pmedium, BOOL fRelease) override;
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC** ppenumFormatEtc) override;
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC* pformatetc, DWORD advf, IAdviseSink* pAdvSink, DWORD* pdwConnection) override;
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD dwConnection) override;
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA** ppenumAdvise) override;

private:
    std::atomic<ULONG>            refCount_{1};
    std::vector<VirtualFileEntry> files_;
    VirtualChunkReadFn            chunkReader_;
};

#else

class ShellClipboard {
public:
    static bool initializeOle() { return true; }
    static void uninitializeOle() {}

    static uint32_t getFileGroupDescriptorWFormat() { return 0; }
    static uint32_t getFileContentsFormat() { return 0; }
    static uint32_t getPreferredDropEffectFormat() { return 0; }

    static bool getLocalClipboardFiles(std::vector<VirtualFileEntry>&, std::vector<std::wstring>&) { return false; }
    static bool setVirtualClipboardFiles(const std::vector<VirtualFileEntry>&, VirtualChunkReadFn) { return false; }
    static void clearClipboard() {}

    static std::wstring resolveConflictFilenameW(const std::wstring& directory, const std::wstring& filename);
    static std::string resolveConflictFilename(const std::string& directory, const std::string& filename);
};

#endif

} // namespace cppdesk
