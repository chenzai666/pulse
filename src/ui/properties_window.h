#pragma once

#include "fluent_components.h"
#include "ui_compositor.h"
#include "../fs/item_properties.h"

#include <functional>
#include <memory>
#include <vector>

namespace pulse::ui {

// Modeless overview. The native property sheet remains the editing surface.
class PropertiesWindow {
public:
    PropertiesWindow();
    ~PropertiesWindow();
    PropertiesWindow(const PropertiesWindow&) = delete;
    PropertiesWindow& operator=(const PropertiesWindow&) = delete;
    bool Create(HWND owner, std::function<void(const std::vector<std::wstring>&)> system_properties);
    void SetTheme(bool dark, D2D1_COLOR_F accent);
    void Show(const std::vector<std::wstring>& paths);
    void Destroy();
    HWND owner() const noexcept { return owner_; }

private:
    friend struct PropertiesWindowTestAccess;
    struct Request;
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    void CancelRead();
    void Close();
    void TryStartRead();
    void Poll();
    void Render(bool present = true);
    float ContentHeight() const;
    float MaxScroll() const;
    void Scroll(float amount);
    D2D1_RECT_F ButtonRect(int control) const;
    int HitTest(float x, float y) const;
    void Activate(int control);

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    std::function<void(const std::vector<std::wstring>&)> system_properties_;
    std::vector<std::wstring> paths_;
    std::shared_ptr<Request> request_;
    fs::ItemProperties properties_;
    bool loading_ = false;
    bool dark_ = false;
    bool backdrop_ = false;
    int contrast_override_ = -1;
    float scale_ = 1.0f;
    float scroll_ = 0.0f;
    int hover_ = 0;
    int pressed_ = 0;
    int focus_ = 3;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
};

} // namespace pulse::ui
