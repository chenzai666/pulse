#pragma once
#include "../ui/folder_picker_dialog.h"
#include <memory>

namespace pulse::app {
struct NativeDialogAppearance {
    ui::FolderPickerSpec style;
    bool dark = false;
    uint32_t accent = 0x0078D4;
    std::wstring language = L"system";
};

class NativeDialogTakeover {
public:
    explicit NativeDialogTakeover(NativeDialogAppearance appearance, HDESK desktop = nullptr,
        std::wstring desktop_name = {}, std::function<void()> failure = {});
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
