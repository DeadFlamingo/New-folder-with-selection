#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <ocidl.h>
#include <propkey.h>
#include <propsys.h>
#include <unknwn.h>

#include <atomic>
#include <cwctype>
#include <filesystem>
#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

namespace fs = std::filesystem;

static const CLSID CLSID_NewFolderWithSelection = {
    0xe4b97a12, 0x8c4e, 0x4f39, {0x9a, 0x2d, 0x6d, 0x81, 0xc5, 0xf2, 0xe1, 0x01}
};

static const CLSID CLSID_UndoNode = {
    0x078759d3, 0x423b, 0x48ad, {0xab, 0x6a, 0x56, 0x38, 0xc2, 0x88, 0x4d, 0xbe}
};

static constexpr wchar_t kClsidStr[] = L"{E4B97A12-8C4E-4F39-9A2D-6D81C5F2E101}";
static constexpr wchar_t kVerbName[] = L"NewFolderWithSelection";
static constexpr wchar_t kExtensionDesc[] = L"New Folder With Selection Shell Extension";
static constexpr wchar_t kMenuTitle[] = L"New folder with selection";
static constexpr wchar_t kMenuTooltip[] = L"Create a new folder with selected files";
static constexpr wchar_t kDefaultMultiFolderName[] = L"New Folder With Items";

static HMODULE g_hModule = nullptr;
static wchar_t g_iconLocation[MAX_PATH + 16]{};
static std::atomic<long> g_cDllRef{0};

using PFN_SHGetThreadUndoManager = HRESULT(WINAPI*)(void** ppum, BOOL fCreate);

template <typename T>
class ComPtr {
public:
    ComPtr() noexcept = default;
    ~ComPtr() noexcept { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    void Reset() noexcept {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

    T** operator&() noexcept {
        Reset();
        return &ptr_;
    }

    T* operator->() const noexcept { return ptr_; }
    T* Get() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

struct SelectionPlan {
    fs::path parentDir;
    std::wstring folderName;
    fs::path folderPath;
};

static bool GetItemFileSystemPath(IShellItem* pItem, std::wstring* outPath) {
    if (!pItem || !outPath) {
        return false;
    }

    ComPtr<IShellItem2> pItem2;
    if (SUCCEEDED(pItem->QueryInterface(IID_PPV_ARGS(&pItem2))) && pItem2) {
        LPWSTR pszParsingPath = nullptr;
        if (SUCCEEDED(pItem2->GetString(PKEY_ParsingPath, &pszParsingPath)) && pszParsingPath) {
            *outPath = pszParsingPath;
            CoTaskMemFree(pszParsingPath);
            return true;
        }
    }

    LPWSTR pszFileSysPath = nullptr;
    if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFileSysPath)) && pszFileSysPath) {
        *outPath = pszFileSysPath;
        CoTaskMemFree(pszFileSysPath);
        return true;
    }

    return false;
}

static bool BuildSelectionPlan(IShellItemArray* psiItemArray, SelectionPlan* outPlan) {
    if (!psiItemArray || !outPlan) {
        return false;
    }

    DWORD count = 0;
    if (FAILED(psiItemArray->GetCount(&count)) || count == 0) {
        return false;
    }

    ComPtr<IShellItem> pFirstItem;
    if (FAILED(psiItemArray->GetItemAt(0, &pFirstItem)) || !pFirstItem) {
        return false;
    }

    std::wstring firstPathStr;
    if (!GetItemFileSystemPath(pFirstItem.Get(), &firstPathStr) || firstPathStr.empty()) {
        return false;
    }

    fs::path firstPath(firstPathStr);
    fs::path parentDir = firstPath.parent_path();
    if (parentDir.empty() || parentDir == firstPath) {
        return false;
    }

    std::wstring baseName = kDefaultMultiFolderName;
    if (count == 1) {
        std::error_code ec;
        std::wstring candidate = fs::is_directory(firstPath, ec)
            ? firstPath.filename().wstring()
            : firstPath.stem().wstring();
        if (candidate.empty()) {
            candidate = firstPath.filename().wstring();
        }
        if (!candidate.empty()) {
            baseName = candidate;
        }
    }

    std::wstring chosenName;
    fs::path fullPath;
    for (int counter = 0;; ++counter) {
        chosenName = (counter == 0) ? baseName : (baseName + L" " + std::to_wstring(counter));
        fullPath = parentDir / chosenName;
        if (GetFileAttributesW(fullPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            break;
        }
    }

    outPlan->parentDir = std::move(parentDir);
    outPlan->folderName = std::move(chosenName);
    outPlan->folderPath = std::move(fullPath);
    return true;
}

static bool DoesBrowserMatchDirectory(IShellBrowser* pShellBrowser, const fs::path& targetDir) {
    if (!pShellBrowser) {
        return false;
    }

    ComPtr<IShellView> pShellView;
    if (FAILED(pShellBrowser->QueryActiveShellView(&pShellView)) || !pShellView) {
        return false;
    }

    ComPtr<IFolderView> pFolderView;
    if (FAILED(pShellView->QueryInterface(IID_PPV_ARGS(&pFolderView))) || !pFolderView) {
        return false;
    }

    ComPtr<IPersistFolder2> pPersistFolder;
    if (FAILED(pFolderView->GetFolder(IID_PPV_ARGS(&pPersistFolder))) || !pPersistFolder) {
        return false;
    }

    PIDLIST_ABSOLUTE pidlFolder = nullptr;
    if (FAILED(pPersistFolder->GetCurFolder(&pidlFolder)) || !pidlFolder) {
        return false;
    }

    wchar_t folderPath[MAX_PATH]{};
    bool ok = SHGetPathFromIDListW(pidlFolder, folderPath) != FALSE;
    CoTaskMemFree(pidlFolder);
    if (!ok) {
        return false;
    }

    std::error_code ec;
    return fs::equivalent(fs::path(folderPath), targetDir, ec);
}

static bool SelectAndEditInShellBrowser(IShellBrowser* pShellBrowser, const std::wstring& folderName) {
    if (!pShellBrowser) {
        return false;
    }

    ComPtr<IShellView> pShellView;
    if (FAILED(pShellBrowser->QueryActiveShellView(&pShellView)) || !pShellView) {
        return false;
    }

    ComPtr<IFolderView> pFolderView;
    if (FAILED(pShellView->QueryInterface(IID_PPV_ARGS(&pFolderView))) || !pFolderView) {
        return false;
    }

    ComPtr<IShellFolder> pShellFolder;
    if (FAILED(pFolderView->GetFolder(IID_PPV_ARGS(&pShellFolder))) || !pShellFolder) {
        return false;
    }

    for (int attempt = 0; attempt < 10; ++attempt) {
        PIDLIST_RELATIVE pidlChild = nullptr;
        HRESULT hrParse = pShellFolder->ParseDisplayName(
            nullptr, nullptr, const_cast<LPWSTR>(folderName.c_str()), nullptr, &pidlChild, nullptr
        );
        if (SUCCEEDED(hrParse) && pidlChild) {
            HRESULT hrSelect = pShellView->SelectItem(
                pidlChild, SVSI_SELECT | SVSI_EDIT | SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE | SVSI_FOCUSED
            );
            CoTaskMemFree(pidlChild);
            if (SUCCEEDED(hrSelect)) {
                return true;
            }
        }
        Sleep(25);
    }

    return false;
}

static bool TrySelectFolderInExplorerWindow(
    IDispatch* pDisp,
    HWND fgWnd,
    const fs::path& parentDir,
    const std::wstring& folderName,
    bool requireFgMatch
) {
    if (!pDisp) {
        return false;
    }

    ComPtr<IWebBrowserApp> pBrowser;
    if (FAILED(pDisp->QueryInterface(IID_PPV_ARGS(&pBrowser))) || !pBrowser) {
        return false;
    }

    if (requireFgMatch) {
        SHANDLE_PTR browserHwnd = 0;
        if (FAILED(pBrowser->get_HWND(&browserHwnd)) || reinterpret_cast<HWND>(browserHwnd) != fgWnd) {
            return false;
        }
    }

    ComPtr<IServiceProvider> pSp;
    if (FAILED(pBrowser->QueryInterface(IID_PPV_ARGS(&pSp))) || !pSp) {
        return false;
    }

    ComPtr<IShellBrowser> pShellBrowser;
    if (FAILED(pSp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&pShellBrowser))) || !pShellBrowser) {
        return false;
    }

    if (!DoesBrowserMatchDirectory(pShellBrowser.Get(), parentDir)) {
        return false;
    }

    return SelectAndEditInShellBrowser(pShellBrowser.Get(), folderName);
}

static bool ExecuteUndoableFolderAndMove(
    IShellItemArray* psiItemArray,
    const SelectionPlan& plan,
    HWND ownerHwnd
) {
    ComPtr<IShellItem> psiParent;
    if (FAILED(SHCreateItemFromParsingName(plan.parentDir.c_str(), nullptr, IID_PPV_ARGS(&psiParent))) || !psiParent) {
        return false;
    }

    ComPtr<IOleUndoManager> pUndoMgr;
    ComPtr<IOleParentUndoUnit> pOuterUndo;
    HMODULE hShell32 = GetModuleHandleW(L"shell32.dll");
    if (!hShell32) {
        hShell32 = LoadLibraryW(L"shell32.dll");
    }
    if (hShell32) {
        auto pfnGetUndo = reinterpret_cast<PFN_SHGetThreadUndoManager>(
            GetProcAddress(hShell32, "SHELL32_SHGetThreadUndoManager")
        );
        if (pfnGetUndo && SUCCEEDED(pfnGetUndo(reinterpret_cast<void**>(&pUndoMgr), TRUE)) && pUndoMgr) {
            if (SUCCEEDED(CoCreateInstance(CLSID_UndoNode, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pOuterUndo))) &&
                pOuterUndo) {
                if (FAILED(pUndoMgr->Open(pOuterUndo.Get()))) {
                    pOuterUndo.Reset();
                }
            }
        }
    }

    constexpr DWORD kOpFlags = FOF_ALLOWUNDO | FOFX_ADDUNDORECORD | FOF_NOCONFIRMMKDIR;

    ComPtr<IFileOperation> pCreateOp;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pCreateOp));
    if (SUCCEEDED(hr) && pCreateOp) {
        if (ownerHwnd) {
            pCreateOp->SetOwnerWindow(ownerHwnd);
        }
        pCreateOp->SetOperationFlags(kOpFlags | FOF_SILENT | FOF_NOERRORUI);
        hr = pCreateOp->NewItem(psiParent.Get(), FILE_ATTRIBUTE_DIRECTORY, plan.folderName.c_str(), nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            hr = pCreateOp->PerformOperations();
        }
    }

    if (FAILED(hr) || GetFileAttributesW(plan.folderPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (pOuterUndo && pUndoMgr) {
            pUndoMgr->Close(pOuterUndo.Get(), FALSE);
        }
        return false;
    }

    BOOL anyAborted = FALSE;
    ComPtr<IShellItem> psiDestFolder;
    hr = SHCreateItemFromParsingName(plan.folderPath.c_str(), nullptr, IID_PPV_ARGS(&psiDestFolder));
    if (SUCCEEDED(hr) && psiDestFolder) {
        ComPtr<IFileOperation> pMoveOp;
        hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pMoveOp));
        if (SUCCEEDED(hr) && pMoveOp) {
            if (ownerHwnd) {
                pMoveOp->SetOwnerWindow(ownerHwnd);
            }
            pMoveOp->SetOperationFlags(kOpFlags);
            hr = pMoveOp->MoveItems(psiItemArray, psiDestFolder.Get());
            if (SUCCEEDED(hr)) {
                hr = pMoveOp->PerformOperations();
                pMoveOp->GetAnyOperationsAborted(&anyAborted);
            }
        }
    }

    const bool moveSucceeded = SUCCEEDED(hr) && !anyAborted;
    if (!moveSucceeded) {
        RemoveDirectoryW(plan.folderPath.c_str());
    }

    if (pOuterUndo && pUndoMgr) {
        pUndoMgr->Close(pOuterUndo.Get(), moveSucceeded ? TRUE : FALSE);
    }

    return moveSucceeded;
}

static void CreateFolderWithItemsInternal(IShellItemArray* psiItemArray, IUnknown* pSite) {
    SelectionPlan plan;
    if (!BuildSelectionPlan(psiItemArray, &plan)) {
        return;
    }

    HWND fgWnd = GetForegroundWindow();
    if (!ExecuteUndoableFolderAndMove(psiItemArray, plan, fgWnd)) {
        return;
    }

    SHChangeNotify(0, SHCNF_FLUSH, nullptr, nullptr);

    if (pSite) {
        ComPtr<IShellBrowser> pSiteBrowser;
        if (SUCCEEDED(IUnknown_QueryService(pSite, SID_STopLevelBrowser, IID_PPV_ARGS(&pSiteBrowser))) && pSiteBrowser) {
            if (SelectAndEditInShellBrowser(pSiteBrowser.Get(), plan.folderName)) {
                return;
            }
        }
    }

    ComPtr<IShellWindows> pShellWindows;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&pShellWindows))) &&
        pShellWindows) {
        long count = 0;
        if (SUCCEEDED(pShellWindows->get_Count(&count))) {
            for (long i = 0; i < count; ++i) {
                VARIANT idx{};
                idx.vt = VT_I4;
                idx.lVal = i;
                ComPtr<IDispatch> pDisp;
                if (SUCCEEDED(pShellWindows->Item(idx, &pDisp)) && pDisp) {
                    if (TrySelectFolderInExplorerWindow(pDisp.Get(), fgWnd, plan.parentDir, plan.folderName, true)) {
                        return;
                    }
                }
            }
            for (long i = 0; i < count; ++i) {
                VARIANT idx{};
                idx.vt = VT_I4;
                idx.lVal = i;
                ComPtr<IDispatch> pDisp;
                if (SUCCEEDED(pShellWindows->Item(idx, &pDisp)) && pDisp) {
                    if (TrySelectFolderInExplorerWindow(pDisp.Get(), fgWnd, plan.parentDir, plan.folderName, false)) {
                        return;
                    }
                }
            }
        }
    }
}

class NewFolderCommand final : public IExplorerCommand, public IObjectWithSite {
public:
    NewFolderCommand() noexcept { ++g_cDllRef; }
    ~NewFolderCommand() noexcept { --g_cDllRef; }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        static const QITAB qit[] = {
            QITABENT(NewFolderCommand, IExplorerCommand),
            QITABENT(NewFolderCommand, IObjectWithSite),
            {0},
        };
        return QISearch(this, qit, riid, ppv);
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refCount_; }

    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG c = --refCount_;
        if (c == 0) {
            delete this;
        }
        return c;
    }

    IFACEMETHODIMP SetSite(IUnknown* pUnkSite) override {
        site_.Reset();
        if (pUnkSite) {
            pUnkSite->QueryInterface(IID_PPV_ARGS(&site_));
        }
        return S_OK;
    }

    IFACEMETHODIMP GetSite(REFIID riid, void** ppvSite) override {
        if (!ppvSite) {
            return E_POINTER;
        }
        *ppvSite = nullptr;
        if (!site_) {
            return E_FAIL;
        }
        return site_->QueryInterface(riid, ppvSite);
    }

    IFACEMETHODIMP GetTitle(IShellItemArray*, LPWSTR* ppszName) override {
        return SHStrDupW(kMenuTitle, ppszName);
    }

    IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* ppszIcon) override {
        if (!ppszIcon) {
            return E_POINTER;
        }
        return SHStrDupW(g_iconLocation, ppszIcon);
    }

    IFACEMETHODIMP GetToolTip(IShellItemArray*, LPWSTR* ppszInfotip) override {
        return SHStrDupW(kMenuTooltip, ppszInfotip);
    }

    IFACEMETHODIMP GetCanonicalName(GUID* pguidCommandName) override {
        if (!pguidCommandName) {
            return E_POINTER;
        }
        *pguidCommandName = CLSID_NewFolderWithSelection;
        return S_OK;
    }

    IFACEMETHODIMP GetState(IShellItemArray* psiItemArray, BOOL, EXPCMDSTATE* pCmdState) override {
        if (!pCmdState) {
            return E_POINTER;
        }
        if (!psiItemArray) {
            *pCmdState = ECS_HIDDEN;
            return S_OK;
        }
        DWORD count = 0;
        if (FAILED(psiItemArray->GetCount(&count)) || count == 0) {
            *pCmdState = ECS_HIDDEN;
            return S_OK;
        }
        ComPtr<IShellItem> pFirstItem;
        SFGAOF attribs = 0;
        constexpr SFGAOF kRequired = SFGAO_FILESYSTEM | SFGAO_CANMOVE;
        if (FAILED(psiItemArray->GetItemAt(0, &pFirstItem)) || !pFirstItem ||
            FAILED(pFirstItem->GetAttributes(kRequired, &attribs)) || (attribs & kRequired) != kRequired) {
            *pCmdState = ECS_HIDDEN;
            return S_OK;
        }
        *pCmdState = ECS_ENABLED;
        return S_OK;
    }

    IFACEMETHODIMP Invoke(IShellItemArray* psiItemArray, IBindCtx*) override {
        if (psiItemArray) {
            CreateFolderWithItemsInternal(psiItemArray, site_.Get());
        }
        return S_OK;
    }

    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* pFlags) override {
        if (!pFlags) {
            return E_POINTER;
        }
        *pFlags = ECF_DEFAULT;
        return S_OK;
    }

    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** ppEnum) override {
        if (ppEnum) {
            *ppEnum = nullptr;
        }
        return E_NOTIMPL;
    }

private:
    std::atomic<ULONG> refCount_{1};
    ComPtr<IUnknown> site_;
};

class ClassFactory final : public IClassFactory {
public:
    ClassFactory() noexcept { ++g_cDllRef; }
    ~ClassFactory() noexcept { --g_cDllRef; }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        static const QITAB qit[] = {
            QITABENT(ClassFactory, IClassFactory),
            {0},
        };
        return QISearch(this, qit, riid, ppv);
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refCount_; }

    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG c = --refCount_;
        if (c == 0) {
            delete this;
        }
        return c;
    }

    IFACEMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        *ppv = nullptr;
        if (pUnkOuter) {
            return CLASS_E_NOAGGREGATION;
        }
        auto* cmd = new (std::nothrow) NewFolderCommand();
        if (!cmd) {
            return E_OUTOFMEMORY;
        }
        HRESULT hr = cmd->QueryInterface(riid, ppv);
        cmd->Release();
        return hr;
    }

    IFACEMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) {
            ++g_cDllRef;
        } else {
            --g_cDllRef;
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> refCount_{1};
};

static LSTATUS SetRegStringValue(HKEY hRoot, const wchar_t* subKey, const wchar_t* valueName, const wchar_t* data) {
    HKEY hKey = nullptr;
    LSTATUS st = RegCreateKeyExW(
        hRoot, subKey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hKey, nullptr
    );
    if (st != ERROR_SUCCESS) {
        return st;
    }
    const DWORD cbData = static_cast<DWORD>((wcslen(data) + 1) * sizeof(wchar_t));
    st = RegSetValueExW(hKey, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(data), cbData);
    RegCloseKey(hKey);
    return st;
}

static HRESULT RegisterServerImpl(bool perUser) {
    HKEY hRoot = perUser ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;

    wchar_t modulePath[MAX_PATH]{};
    if (GetModuleFileNameW(g_hModule, modulePath, MAX_PATH) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    std::wstring clsidBase = std::wstring(L"Software\\Classes\\CLSID\\") + kClsidStr;
    std::wstring inprocKey = clsidBase + L"\\InprocServer32";
    std::wstring versionKey = clsidBase + L"\\Version";

    LSTATUS st = SetRegStringValue(hRoot, clsidBase.c_str(), nullptr, kExtensionDesc);
    if (st != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(st);
    }

    st = SetRegStringValue(hRoot, inprocKey.c_str(), nullptr, modulePath);
    if (st != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(st);
    }

    st = SetRegStringValue(hRoot, inprocKey.c_str(), L"ThreadingModel", L"Apartment");
    if (st != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(st);
    }

    st = SetRegStringValue(hRoot, versionKey.c_str(), nullptr, L"1.0");
    if (st != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(st);
    }

    std::wstring shellKey = std::wstring(L"Software\\Classes\\AllFileSystemObjects\\shell\\") + kVerbName;
    st = SetRegStringValue(hRoot, shellKey.c_str(), nullptr, kMenuTitle);
    if (st != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(st);
    }

    st = SetRegStringValue(hRoot, shellKey.c_str(), L"MUIVerb", kMenuTitle);
    if (st != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(st);
    }

    st = SetRegStringValue(hRoot, shellKey.c_str(), L"Icon", g_iconLocation);
    if (st != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(st);
    }

    st = SetRegStringValue(hRoot, shellKey.c_str(), L"ExplorerCommandHandler", kClsidStr);
    if (st != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(st);
    }

    st = SetRegStringValue(hRoot, shellKey.c_str(), L"MultiSelectModel", L"Player");
    if (st != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(st);
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

static HRESULT UnregisterServerImpl(bool perUser) {
    HKEY hRoot = perUser ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;

    std::wstring shellKey = std::wstring(L"Software\\Classes\\AllFileSystemObjects\\shell\\") + kVerbName;
    RegDeleteTreeW(hRoot, shellKey.c_str());

    std::wstring clsidBase = std::wstring(L"Software\\Classes\\CLSID\\") + kClsidStr;
    RegDeleteTreeW(hRoot, clsidBase.c_str());

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        g_hModule = hInstance;
        DisableThreadLibraryCalls(hInstance);
        wchar_t modulePath[MAX_PATH]{};
        if (GetModuleFileNameW(hInstance, modulePath, MAX_PATH) > 0) {
            wsprintfW(g_iconLocation, L"%s,0", modulePath);
        }
    }
    return TRUE;
}

extern "C" STDAPI DllCanUnloadNow() {
    return (g_cDllRef.load() == 0) ? S_OK : S_FALSE;
}

extern "C" STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    *ppv = nullptr;
    if (!IsEqualCLSID(rclsid, CLSID_NewFolderWithSelection)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    auto* factory = new (std::nothrow) ClassFactory();
    if (!factory) {
        return E_OUTOFMEMORY;
    }
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

extern "C" STDAPI DllRegisterServer() {
    HRESULT hr = RegisterServerImpl(false);
    if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) {
        hr = RegisterServerImpl(true);
    }
    return hr;
}

extern "C" STDAPI DllUnregisterServer() {
    UnregisterServerImpl(true);
    UnregisterServerImpl(false);
    return S_OK;
}

extern "C" STDAPI DllInstall(BOOL bInstall, LPCWSTR pszCmdLine) {
    bool perUser = false;
    if (pszCmdLine) {
        std::wstring cmd(pszCmdLine);
        for (auto& ch : cmd) {
            ch = static_cast<wchar_t>(std::towlower(ch));
        }
        if (cmd.find(L"user") != std::wstring::npos) {
            perUser = true;
        }
    }
    return bInstall ? RegisterServerImpl(perUser) : UnregisterServerImpl(perUser);
}
