#pragma once
#include "../ui/folder_picker_dialog.h"
#include <cstdint>
#include <memory>

namespace pulse::app {
inline constexpr wchar_t kNativeDialogSessionProperty[] = L"Pulse.NativeDialogTakeover.Session.v1";
struct NativeDialogRecovery {
    uintptr_t nonce = 0;
    bool added_layered = false;
    COLORREF color_key = 0;
    BYTE alpha = 255;
    DWORD layered_flags = LWA_ALPHA;
    bool owner_enabled = true;
};
struct NativeDialogAppearance {
    ui::FolderPickerSpec style;
    bool dark = false;
    uint32_t accent = 0x0078D4;
    std::wstring language = L"system";
};

class NativeDialogTakeover {
public:
    explicit NativeDialogTakeover(NativeDialogAppearance appearance, HDESK desktop = nullptr, std::wstring desktop_name = {});
    ~NativeDialogTakeover();
    void UpdateAppearance(NativeDialogAppearance appearance);
    void Stop();
    struct Shared;
private:
    std::shared_ptr<Shared> shared_;
    HANDLE thread_ = nullptr;
};

// 独立选择窗口使用仅显式继承给它的匿名管道；不修改宿主程序或注册系统 COM 类。
int RunNativeDialogPicker(HANDLE input, HANDLE output, HANDLE cancelled);
}
