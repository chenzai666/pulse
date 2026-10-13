#pragma once

#include "../ui/confirm_dialog.h"

namespace pulse::app {

enum class DeleteDisposition { Cancel, Recycle, Permanent };

struct DeleteConfirmationPlan {
    bool show_dialog = false;
    bool permanent = false;
};

constexpr DeleteConfirmationPlan PlanDeleteConfirmation(bool permanent, bool force_confirm,
                                                        bool confirm_recycle) {
    return {permanent || force_confirm || confirm_recycle, permanent};
}

inline DeleteDisposition ResolveDeleteConfirmation(const DeleteConfirmationPlan& plan,
                                                   const ui::ConfirmDialogResult& result) {
    if (!plan.show_dialog) return DeleteDisposition::Recycle;
    if (result.choice != ui::ConfirmChoice::Confirm) return DeleteDisposition::Cancel;
    return plan.permanent || result.checkbox_checked ? DeleteDisposition::Permanent
                                                     : DeleteDisposition::Recycle;
}

} // namespace pulse::app
