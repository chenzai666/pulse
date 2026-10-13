#pragma once

#include "fluent_components.h"
#include "ui_compositor.h"
#include "../app/status_center_model.h"
#include <functional>
#include <vector>

namespace pulse::ui {

struct StatusCenterCallbacks {
    std::function<void(app::StatusTaskKey)> cancel;
    std::function<void(uint64_t)> operation_details;
    std::function<void()> clear_completed;
};

// Event-driven, modeless flyout. It owns no task engine or animation timer.
class StatusCenterWindow {
public:
    StatusCenterWindow();
    ~StatusCenterWindow();
    bool Create(HWND owner, StatusCenterCallbacks callbacks);
    void Destroy();
    void SetTheme(bool dark, D2D1_COLOR_F accent);
    void Update(const std::vector<app::StatusTaskRecord>& records);
    void Show(POINT anchor);
    HWND owner() const noexcept { return owner_; }
    bool visible() const;

private:
    friend struct StatusCenterWindowTestAccess;
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    void Render(bool present = true);
    int HitTest(float x, float y) const;
    void Activate(int control);
    void Scroll(float amount);
    float MaxScroll() const;
    D2D1_RECT_F ButtonRect(int control) const;
    bool Enabled(int control) const;
    void FocusNext(bool backwards);

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    StatusCenterCallbacks callbacks_;
    std::vector<app::StatusTaskRecord> records_;
    bool dark_ = false;
    bool backdrop_ = false;
    float scale_ = 1.0f;
    float scroll_ = 0.0f;
    int hover_ = 0;
    int pressed_ = 0;
    int focus_ = 1;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
};

} // namespace pulse::ui
