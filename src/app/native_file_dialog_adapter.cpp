#include "native_file_dialog_adapter.h"

#include <objbase.h>
#include <UIAutomation.h>
#include <dlgs.h>
#include <commctrl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <string_view>

namespace pulse::app {
namespace {

using Microsoft::WRL::ComPtr;
constexpr UINT kMessageTimeout = 250;
constexpr int kMaximumItems = 128;
constexpr size_t kMaximumText = 32768;

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};

struct TokenIdentity {
    std::vector<unsigned char> user;
    DWORD integrity = 0;
    DWORD session = 0;
};

struct Controls {
    HWND shell = nullptr;
    HWND direct = nullptr;
    HWND browse = nullptr;
    HWND tree = nullptr;
    HWND filename = nullptr;
    HWND filter = nullptr;
    HWND address = nullptr;
    HWND address_toolbar = nullptr;
    bool ambiguous_filter = false;
    bool standard_filter = false;
    HWND confirm = nullptr;
    HWND cancel = nullptr;
};

bool Fail(std::wstring& reason, const wchar_t* message) {
    reason = message;
    return false;
}

bool Cancelled(const std::function<bool()>& cancelled, std::wstring& reason) {
    if (!cancelled || !cancelled()) return false;
    reason = L"接管已取消；未发送原生确认请求。";
    return true;
}

std::wstring ClassName(HWND window) {
    wchar_t text[128]{};
    const int length = GetClassNameW(window, text, static_cast<int>(std::size(text)));
    return length > 0 ? std::wstring(text, static_cast<size_t>(length)) : std::wstring();
}

bool Send(HWND window, UINT message, WPARAM wparam, LPARAM lparam, DWORD_PTR& result) {
    if (!window || message >= WM_USER) return false;
    return SendMessageTimeoutW(window, message, wparam, lparam,
        SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, kMessageTimeout, &result) != 0;
}

bool WindowText(HWND window, std::wstring& result) {
    std::vector<wchar_t> text(kMaximumText);
    DWORD_PTR length = 0;
    if (!Send(window, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data()), length) ||
        length >= text.size() - 1) return false;
    result.assign(text.data(), static_cast<size_t>(length));
    return true;
}

std::wstring WindowText(HWND window) {
    std::wstring result;
    WindowText(window, result);
    return result;
}

bool ReadToken(HANDLE process, TokenIdentity& identity) {
    Handle token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) return false;
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    if (!size || size > 65536) return false;
    identity.user.resize(size);
    if (!GetTokenInformation(token.value, TokenUser, identity.user.data(), size, &size)) return false;
    size = 0;
    GetTokenInformation(token.value, TokenIntegrityLevel, nullptr, 0, &size);
    if (!size || size > 65536) return false;
    std::vector<unsigned char> integrity(size);
    if (!GetTokenInformation(token.value, TokenIntegrityLevel, integrity.data(), size, &size)) return false;
    const auto label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(integrity.data());
    if (!IsValidSid(label->Label.Sid)) return false;
    const UCHAR count = *GetSidSubAuthorityCount(label->Label.Sid);
    if (!count) return false;
    identity.integrity = *GetSidSubAuthority(label->Label.Sid, count - 1);
    return GetTokenInformation(token.value, TokenSessionId, &identity.session,
        sizeof(identity.session), &size) != FALSE;
}

bool WindowIdentity(HWND window, DWORD& process_id, DWORD& thread_id,
    FILETIME& created, std::wstring& reason) {
    if (!IsWindow(window) || GetAncestor(window, GA_ROOT) != window ||
        ClassName(window) != L"#32770") return Fail(reason, L"窗口不是标准顶层文件对话框。");
    thread_id = GetWindowThreadProcessId(window, &process_id);
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id)};
    if (!thread_id || !process.value) return Fail(reason, L"无法核实对话框所属进程。");
    TokenIdentity own, target;
    if (!ReadToken(GetCurrentProcess(), own) || !ReadToken(process.value, target))
        return Fail(reason, L"无法核实进程权限；保留原生对话框。");
    const auto own_user = reinterpret_cast<const TOKEN_USER*>(own.user.data());
    const auto target_user = reinterpret_cast<const TOKEN_USER*>(target.user.data());
    if (!EqualSid(own_user->User.Sid, target_user->User.Sid) || own.session != target.session ||
        own.integrity != target.integrity)
        return Fail(reason, L"用户、会话或完整性级别不一致；不绕过 UIPI。");
    FILETIME exited{}, kernel{}, user{};
    if (!GetProcessTimes(process.value, &created, &exited, &kernel, &user))
        return Fail(reason, L"无法核实进程创建时间。");
    // GetThreadDesktop cannot reliably query threads in other processes on all Windows
    // versions; instead verify the target window is enumerable on the current thread's
    // desktop, which must already have been switched to the correct desktop by the caller.
    const HDESK own_desktop = GetThreadDesktop(GetCurrentThreadId());
    if (!own_desktop) return Fail(reason, L"无法获取当前工作线程的桌面。");
    bool window_on_desktop = false;
    struct DesktopSearch { HWND target; bool& found; };
    DesktopSearch desktop_ctx{window, window_on_desktop};
    EnumDesktopWindows(own_desktop, [](HWND hwnd, LPARAM lp) -> BOOL {
        auto& ctx = *reinterpret_cast<DesktopSearch*>(lp);
        if (hwnd == ctx.target) { ctx.found = true; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&desktop_ctx));
    if (!window_on_desktop)
        return Fail(reason, L"窗口不在当前工作线程的桌面上。");
    if (!IsWindowEnabled(window)) return Fail(reason, L"原生对话框当前被其他模态窗口阻塞。");
    return true;
}

Controls FindControls(HWND window) {
    Controls controls;
    controls.confirm = GetDlgItem(window, IDOK);
    controls.cancel = GetDlgItem(window, IDCANCEL);
    struct Context { Controls* controls; size_t count = 0; } context{&controls};
    EnumChildWindows(window, [](HWND child, LPARAM data) -> BOOL {
        auto& state = *reinterpret_cast<Context*>(data);
        if (++state.count > 512) return FALSE;
        auto& result = *state.controls;
        const auto name = ClassName(child);
        if (name == L"SHELLDLL_DefView") result.shell = child;
        if (name == L"DirectUIHWND") result.direct = child;
        if (name == L"SHBrowseForFolder ShellNameSpace Control") result.browse = child;
        if (name == L"SysTreeView32") result.tree = child;
        const int identifier = GetDlgCtrlID(child);
        if (name == L"ComboBox" && IsWindowVisible(child) && identifier == cmb1) {
            if (result.standard_filter) {
                if (result.filter != child) result.ambiguous_filter = true;
            } else {
                result.filter = child;
                result.standard_filter = true;
                result.ambiguous_filter = false;
            }
        } else if (name == L"ComboBox" && IsWindowVisible(child) && !result.standard_filter &&
            identifier != cmb13 && identifier != 41477 &&
            !FindWindowExW(child, nullptr, L"Edit", nullptr) &&
            (GetWindowLongPtrW(child, GWL_STYLE) & 3) == CBS_DROPDOWNLIST) {
            if (result.filter && result.filter != child) result.ambiguous_filter = true;
            result.filter = child;
        }
        if ((identifier == edt1 || identifier == cmb13 || identifier == 14148 ||
             (identifier == 1001 && ClassName(GetParent(child)) == L"ComboBox")) &&
            name == L"Edit" && IsWindowVisible(child))
            result.filename = child;
        if (identifier == cmb13 && name == L"ComboBox" && !result.filename)
            result.filename = FindWindowExW(child, nullptr, L"Edit", nullptr);
        if (name == L"Edit" && (identifier == 41477 || identifier == 1137)) result.address = child;
        if (name == L"ToolbarWindow32" && identifier == 1001) result.address_toolbar = child;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return controls;
}

bool Automation(ComPtr<IUIAutomation>& automation, std::wstring& reason) {
    ComPtr<IUIAutomation2> timed;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&timed))) ||
        FAILED(timed->put_ConnectionTimeout(500)) || FAILED(timed->put_TransactionTimeout(500)))
        return Fail(reason, L"无法创建带超时限制的 UI Automation；工作线程须初始化 COM。");
    return SUCCEEDED(timed.As(&automation));
}

ComPtr<IUIAutomationElement> Element(IUIAutomation* automation, HWND window) {
    ComPtr<IUIAutomationElement> element;
    if (window) automation->ElementFromHandle(window, &element);
    return element;
}

std::wstring Name(IUIAutomationElement* element) {
    BSTR name = nullptr;
    if (!element || FAILED(element->get_CurrentName(&name))) return {};
    std::wstring result;
    if (name && SysStringLen(name) < kMaximumText) result.assign(name, SysStringLen(name));
    SysFreeString(name);
    return result;
}

std::wstring ButtonWord(std::wstring_view text) {
    std::wstring result;
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'(' && index + 3 < text.size() && text[index + 1] == L'&' &&
            text[index + 3] == L')') { index += 3; continue; }
        if (text[index] != L'&' && !std::iswspace(text[index]))
            result.push_back(static_cast<wchar_t>(std::towlower(text[index])));
    }
    return result;
}

bool AbsolutePath(std::wstring_view path) {
    if (path.empty() || path.size() >= kMaximumText) return false;
    std::wstring extended_unc;
    if (path.starts_with(L"\\\\?\\UNC\\")) {
        extended_unc = L"\\\\";
        extended_unc += path.substr(8);
        path = extended_unc;
    }
    else if (path.starts_with(L"\\\\?\\")) path.remove_prefix(4);
    if (std::any_of(path.begin(), path.end(), [](wchar_t character) {
        return character < L' ' || character == L'"' || character == L'*' || character == L'?';
    })) return false;
    if (path.size() > 2 && std::iswalpha(path[0]) && path[1] == L':' && path[2] == L'\\') return true;
    if (path.size() < 5 || path.substr(0, 2) != L"\\\\" || path[2] == L'.') return false;
    const size_t server_end = path.find(L'\\', 2);
    return server_end != std::wstring_view::npos && server_end > 2 && server_end + 1 < path.size() &&
        path[server_end + 1] != L'\\';
}

bool ParseFilter(std::wstring label, ui::PickerFilter& filter) {
    const size_t close = label.rfind(L')');
    const size_t open = close == std::wstring::npos ? std::wstring::npos : label.rfind(L'(', close);
    std::wstring pattern = open == std::wstring::npos ? label : label.substr(open + 1, close - open - 1);
    while (!pattern.empty() && std::iswspace(pattern.front())) pattern.erase(pattern.begin());
    while (!pattern.empty() && std::iswspace(pattern.back())) pattern.pop_back();
    if (pattern.empty() || pattern.size() > 4096 || pattern.find_first_of(L"*?") == std::wstring::npos ||
        pattern.find_first_of(L"\\/\r\n\"()") != std::wstring::npos ||
        (open == std::wstring::npos && pattern.find_first_of(L" \t") != std::wstring::npos)) return false;
    filter = {std::move(label), std::move(pattern)};
    return true;
}

bool NativeFilters(HWND combo, std::vector<ui::PickerFilter>& filters, size_t& selected) {
    if (ClassName(combo) != L"ComboBox") return false;
    const auto style = GetWindowLongPtrW(combo, GWL_STYLE);
    if ((style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) && !(style & CBS_HASSTRINGS)) return false;
    DWORD_PTR count = 0, selection = 0;
    if (!Send(combo, CB_GETCOUNT, 0, 0, count) || count == 0 || count > static_cast<DWORD_PTR>(kMaximumItems) ||
        !Send(combo, CB_GETCURSEL, 0, 0, selection) || selection >= count) return false;
    std::vector<ui::PickerFilter> result;
    const ULONGLONG deadline = GetTickCount64() + 2000;
    for (DWORD_PTR index = 0; index < count; ++index) {
        if (GetTickCount64() >= deadline) return false;
        // These messages are below WM_USER: user32 marshals their output buffer.
        // Use the full bounded buffer so a changed label cannot overflow it.
        DWORD_PTR length = 0, copied = 0;
        std::vector<wchar_t> label(kMaximumText);
        ui::PickerFilter filter;
        if (!Send(combo, CB_GETLBTEXTLEN, index, 0, length) || !length || length >= label.size() - 1 ||
            !Send(combo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(label.data()), copied) ||
            copied != length || !ParseFilter(std::wstring(label.data(), static_cast<size_t>(copied)), filter)) return false;
        result.push_back(std::move(filter));
    }
    DWORD_PTR final_count = 0, final_selection = 0;
    if (!Send(combo, CB_GETCOUNT, 0, 0, final_count) || final_count != count ||
        !Send(combo, CB_GETCURSEL, 0, 0, final_selection) || final_selection != selection) return false;
    filters = std::move(result);
    selected = static_cast<size_t>(selection);
    return true;
}

std::wstring Directory(const Controls& controls, const std::wstring& filename,
    IUIAutomation* automation = nullptr) {
    std::wstring text;
    if (controls.address && WindowText(controls.address, text) && AbsolutePath(text)) return text;
    if (AbsolutePath(filename)) {
        const size_t separator = filename.rfind(L'\\');
        return filename.substr(0, separator == 2 ? 3 : separator);
    }
    if (!automation) return {};
    auto toolbar = Element(automation, controls.address_toolbar);
    text = Name(toolbar.Get());
    for (const wchar_t* prefix : {L"Address: ", L"地址: ", L"地址："}) {
        const std::wstring_view start(prefix);
        if (text.starts_with(start) && AbsolutePath(std::wstring_view(text).substr(start.size())))
            return text.substr(start.size());
    }
    return {};
}

bool MultipleSelection(IUIAutomation* automation, const Controls& controls) {
    const HWND list = FindWindowExW(controls.shell, nullptr, WC_LISTVIEWW, nullptr);
    if (list) return !(GetWindowLongPtrW(list, GWL_STYLE) & LVS_SINGLESEL);
    // Inspect only the verified shell view and its immediate UIA children.
    // A full dialog tree includes namespace providers that can stall traversal.
    const HWND direct = FindWindowExW(controls.shell, nullptr, L"DirectUIHWND", nullptr);
    auto view = Element(automation, direct ? direct : controls.shell);
    if (!view) return false;
    ComPtr<IUIAutomationSelectionPattern> pattern;
    BOOL allowed = FALSE;
    if (SUCCEEDED(view->GetCurrentPatternAs(UIA_SelectionPatternId, IID_PPV_ARGS(&pattern))))
        return SUCCEEDED(pattern->get_CurrentCanSelectMultiple(&allowed)) && allowed;
    ComPtr<IUIAutomationCondition> condition;
    VARIANT value{}; value.vt = VT_I4; value.lVal = UIA_ListControlTypeId;
    ComPtr<IUIAutomationElement> child;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value, &condition)) ||
        FAILED(view->FindFirst(TreeScope_Children, condition.Get(), &child)) || !child) return false;
    return SUCCEEDED(child->GetCurrentPatternAs(UIA_SelectionPatternId, IID_PPV_ARGS(&pattern))) &&
        SUCCEEDED(pattern->get_CurrentCanSelectMultiple(&allowed)) && allowed;
}

bool WriteFilename(HWND window, const std::wstring& text, bool legacy, const std::function<bool()>& cancelled) {
    if ((cancelled && cancelled()) || ClassName(window) != L"Edit" ||
        (GetWindowLongPtrW(window, GWL_STYLE) & ES_READONLY)) return false;
    if (legacy) {
        // The legacy browse provider also synchronizes the tree/PIDL selection.
        // Keep that established path, scoped to the already verified edit HWND.
        ComPtr<IUIAutomation> automation;
        std::wstring reason;
        if (!Automation(automation, reason)) return false;
        auto edit = Element(automation.Get(), window);
        ComPtr<IUIAutomationValuePattern> value;
        if (!edit || FAILED(edit->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value)))) return false;
        BSTR input = SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
        if (!input) return false;
        const HRESULT updated = (cancelled && cancelled()) ? E_ABORT : value->SetValue(input);
        SysFreeString(input);
        if (FAILED(updated)) return false;
    } else {
        DWORD_PTR written = 0;
        if (!Send(window, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()), written) || !written) return false;
    }
    if (cancelled && cancelled()) return false;
    std::wstring actual;
    return WindowText(window, actual) && actual == text;
}

bool SelectFilter(const Controls& controls, size_t index, const std::vector<ui::PickerFilter>& filters,
    const std::function<bool()>& cancelled) {
    if (index >= filters.size() || (cancelled && cancelled()) || ClassName(controls.filter) != L"ComboBox") return false;
    DWORD_PTR selected = 0;
    if (!Send(controls.filter, CB_SETCURSEL, index, 0, selected) || selected != index) return false;
    if (cancelled && cancelled()) return false;
    DWORD_PTR result = 0;
    if (!Send(GetParent(controls.filter), WM_COMMAND,
        MAKEWPARAM(GetDlgCtrlID(controls.filter), CBN_SELCHANGE), reinterpret_cast<LPARAM>(controls.filter), result)) return false;
    return Send(controls.filter, CB_GETCURSEL, 0, 0, selected) && selected == index;
}

bool SameIdentity(const NativeFileDialog& original, const NativeFileDialog& current) {
    return original.window == current.window && original.process_id == current.process_id &&
        original.window_thread_id == current.window_thread_id && original.kind == current.kind &&
        CompareFileTime(&original.process_created, &current.process_created) == 0 &&
        original.filename_window == current.filename_window && original.confirm_window == current.confirm_window &&
        original.legacy_folder == current.legacy_folder;
}

bool IdentityBeforeMutation(const NativeFileDialog& dialog, std::wstring& reason) {
    DWORD process = 0, thread = 0;
    FILETIME created{};
    if (!WindowIdentity(dialog.window, process, thread, created, reason)) return false;
    if (process != dialog.process_id || thread != dialog.window_thread_id ||
        CompareFileTime(&created, &dialog.process_created) != 0 ||
        !IsChild(dialog.window, dialog.filename_window) || ClassName(dialog.filename_window) != L"Edit" ||
        GetDlgItem(dialog.window, IDOK) != dialog.confirm_window || ClassName(dialog.confirm_window) != L"Button")
        return Fail(reason, L"操作前原生窗口或标准控件身份发生变化。");
    return true;
}

bool SameFilters(const std::vector<ui::PickerFilter>& original, const std::vector<ui::PickerFilter>& current) {
    if (original.size() != current.size()) return false;
    for (size_t index = 0; index < original.size(); ++index) {
        if (original[index].label != current[index].label || original[index].pattern != current[index].pattern) return false;
    }
    return true;
}

bool SelectionText(const NativeFileDialog& dialog, const std::vector<std::wstring>& paths,
    std::wstring& text, std::wstring& reason) {
    if (paths.empty() || paths.size() > kMaximumItems) return Fail(reason, L"选择结果为空或项目过多。");
    for (const auto& path : paths) {
        if (!AbsolutePath(path)) return Fail(reason, L"仅接受无引号、通配符及控制字符的完整路径。");
    }
    if (paths.size() == 1) { text = paths.front(); return true; }
    if (dialog.kind != NativeDialogKind::Open || !dialog.allow_multiselect)
        return Fail(reason, L"原生对话框未明确允许多选。");
    std::wstring directory = dialog.initial_path;
    while (directory.size() > 3 && directory.back() == L'\\') directory.pop_back();
    if (directory.empty()) return Fail(reason, L"无法核实原生当前目录，不能回填相对多选文件名。");
    for (const auto& path : paths) {
        const size_t separator = path.rfind(L'\\');
        const std::wstring current = path.substr(0, separator == 2 ? 3 : separator);
        if (CompareStringOrdinal(directory.c_str(), -1, current.c_str(), -1, TRUE) != CSTR_EQUAL)
            return Fail(reason, L"多选仅支持原生当前目录中的文件；跨目录结果保留原生窗口。");
        if (separator + 1 == path.size()) return Fail(reason, L"多选结果必须包含文件名。");
        if (!text.empty()) text += L' ';
        text += L'"';
        text += path.substr(separator + 1);
        text += L'"';
    }
    if (text.size() >= kMaximumText) return Fail(reason, L"多选文件名表达式过长。");
    return true;
}

bool InvokeButton(const NativeFileDialog& dialog, HWND button, int identifier,
    std::wstring& reason, const std::function<bool()>& cancelled = {}) {
    ComPtr<IUIAutomation> automation;
    std::wstring automation_reason;
    ComPtr<IUIAutomationInvokePattern> invoke;
    if (Automation(automation, automation_reason)) {
        auto element = Element(automation.Get(), button);
        if (element) element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke));
    }
    if (Cancelled(cancelled, reason) || !IdentityBeforeMutation(dialog, reason) ||
        GetDlgItem(dialog.window, identifier) != button || !IsWindowEnabled(button)) return false;
    if (Cancelled(cancelled, reason)) return false;
    if (invoke) {
        if (SUCCEEDED(invoke->Invoke())) return true;
        return Fail(reason, L"原生按钮调用失败；不重复触发可能已执行的请求。");
    }
    // Let the real Button route its command to its actual parent (a modern
    // dialog may use a notification sink below the top-level #32770).
    if (PostMessageW(button, BM_CLICK, 0, 0)) return true;
    return Fail(reason, L"无法向原生对话框发送按钮请求。");
}

} // namespace

bool InspectNativeFileDialog(HWND window, NativeFileDialog& dialog, std::wstring& reason) {
    reason.clear();
    dialog = {};
    NativeFileDialog result;
    result.window = window;
    if (!WindowIdentity(window, result.process_id, result.window_thread_id, result.process_created, reason)) return false;
    Controls controls = FindControls(window);
    if (!controls.confirm || !controls.cancel || ClassName(controls.confirm) != L"Button" ||
        ClassName(controls.cancel) != L"Button") return Fail(reason, L"缺少标准确认或取消按钮。");
    const bool legacy = controls.browse && controls.tree && !controls.shell && !controls.direct;
    if (!legacy && (!controls.shell || (!controls.direct && !controls.filter)))
        return Fail(reason, L"未识别到标准 DirectUI 或 Explorer 文件对话框结构。");
    if (!controls.filename || !IsChild(window, controls.filename) || ClassName(controls.filename) != L"Edit" ||
        !(GetWindowLongPtrW(controls.filename, GWL_STYLE) & WS_VISIBLE) ||
        (GetWindowLongPtrW(controls.filename, GWL_STYLE) & ES_READONLY) ||
        !WindowText(controls.filename, result.filename))
        return Fail(reason, L"找不到已核实的标准文件名或目录输入框。");
    const auto word = ButtonWord(WindowText(controls.confirm));
    if (legacy) result.kind = NativeDialogKind::Folder;
    else if (word == L"save" || word == L"保存") result.kind = NativeDialogKind::Save;
    else if (word == L"open" || word == L"打开") result.kind = NativeDialogKind::Open;
    else if (controls.direct && !controls.filter &&
        (word == L"selectfolder" || word == L"选择文件夹" || word == L"选择文件夾")) result.kind = NativeDialogKind::Folder;
    else return Fail(reason, L"无法可靠区分打开、保存与目录模式；保留原生对话框。");
    if (controls.ambiguous_filter) return Fail(reason, L"存在多个文件类型控件；保留原生窗口。");
    if (result.kind != NativeDialogKind::Folder) {
        if (!NativeFilters(controls.filter, result.filters, result.filter_index))
            return Fail(reason, L"无法读取完整原生文件类型过滤器及选中项；不使用猜测的通配符。");
    } else if (controls.filter) return Fail(reason, L"目录模式与文件类型控件不一致。");
    result.title = WindowText(window);
    result.initial_path = Directory(controls, result.filename);
    if (result.kind == NativeDialogKind::Open) {
        ComPtr<IUIAutomation> automation;
        std::wstring automation_reason;
        if (Automation(automation, automation_reason)) {
            result.allow_multiselect = MultipleSelection(automation.Get(), controls);
            if (result.allow_multiselect && result.initial_path.empty())
                result.initial_path = Directory(controls, result.filename, automation.Get());
        }
    }
    result.filename_window = controls.filename;
    result.confirm_window = controls.confirm;
    result.legacy_folder = legacy;
    DWORD current_process = 0;
    if (GetWindowThreadProcessId(window, &current_process) != result.window_thread_id ||
        current_process != result.process_id || !IsChild(window, controls.filename))
        return Fail(reason, L"读取过程中原生对话框身份发生变化。");
    dialog = std::move(result);
    return true;
}

bool NativeDialogStillMatches(const NativeFileDialog& dialog) {
    NativeFileDialog current;
    std::wstring reason;
    return InspectNativeFileDialog(dialog.window, current, reason) && SameIdentity(dialog, current);
}

bool CommitNativeDialogSelection(const NativeFileDialog& dialog,
    const std::vector<std::wstring>& paths, size_t filter_index, std::wstring& reason,
    const std::function<bool()>& cancelled) {
    reason.clear();
    if (Cancelled(cancelled, reason)) return false;
    NativeFileDialog current;
    if (!InspectNativeFileDialog(dialog.window, current, reason)) return false;
    if (!SameIdentity(dialog, current)) return Fail(reason, L"原生对话框已关闭、替换或改变模式。");
    if (!SameFilters(dialog.filters, current.filters)) return Fail(reason, L"原生过滤器已变化；取消回填。");
    if ((!current.filters.empty() && filter_index >= current.filters.size()) ||
        (current.filters.empty() && filter_index != 0)) return Fail(reason, L"文件类型索引无效。");
    if (!IsWindowEnabled(current.confirm_window)) return Fail(reason, L"原生确认按钮不可用。");
    std::wstring text;
    if (!SelectionText(current, paths, text, reason)) return false;
    Controls controls = FindControls(current.window);
    const bool changing_filter = !current.filters.empty() && filter_index != current.filter_index;
    if (Cancelled(cancelled, reason) || !IdentityBeforeMutation(current, reason)) return false;
    if (changing_filter && !SelectFilter(controls, filter_index, current.filters, cancelled))
        return Fail(reason, L"原生文件类型无法安全切换；未确认结果。");
    if (Cancelled(cancelled, reason) || !IdentityBeforeMutation(current, reason)) return false;
    if (!WriteFilename(current.filename_window, text, current.legacy_folder, cancelled))
        return Fail(reason, L"原生输入框未接受选择结果；未发送确认请求。");
    if (Cancelled(cancelled, reason)) return false;
    NativeFileDialog prepared;
    if (!InspectNativeFileDialog(current.window, prepared, reason) || !SameIdentity(current, prepared) ||
        !SameFilters(current.filters, prepared.filters) || prepared.filename != text ||
        prepared.filter_index != filter_index || !IsWindowEnabled(current.confirm_window))
        return Fail(reason, L"回填后窗口身份、文件名、类型或确认状态发生变化。");
    if (Cancelled(cancelled, reason)) return false;
    return InvokeButton(current, current.confirm_window, IDOK, reason, cancelled);
}

bool CancelNativeDialog(const NativeFileDialog& dialog, std::wstring& reason,
    const std::function<bool()>& cancelled) {
    reason.clear();
    if (Cancelled(cancelled, reason)) return false;
    NativeFileDialog current;
    if (!InspectNativeFileDialog(dialog.window, current, reason)) return false;
    if (!SameIdentity(dialog, current)) return Fail(reason, L"原生对话框身份已改变；未发送取消请求。");
    HWND cancel = GetDlgItem(current.window, IDCANCEL);
    if (ClassName(cancel) != L"Button" || !IsWindowEnabled(cancel)) return Fail(reason, L"原生取消按钮不可用。");
    return InvokeButton(current, cancel, IDCANCEL, reason, cancelled);
}

} // namespace pulse::app
