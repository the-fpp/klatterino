// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/helper/CompactTabStatus.hpp"

#include <QString>

namespace chatterino {

class LabelButton;

/// Non-owning handles to one compact tab control group.
struct CompactTabControlButtons {
    LabelButton *previousLive = nullptr;
    LabelButton *previous = nullptr;
    LabelButton *status = nullptr;
    LabelButton *next = nullptr;
    LabelButton *nextLive = nullptr;

    [[nodiscard]] bool complete() const noexcept
    {
        return this->previousLive != nullptr && this->previous != nullptr &&
               this->status != nullptr && this->next != nullptr &&
               this->nextLive != nullptr;
    }
};

[[nodiscard]] QString compactTabStatusText(const CompactTabStatus &status);

/// Applies invariant setup shared by notebook-hosted and titlebar-hosted
/// compact controls.
void initializeCompactTabControlButtons(
    const CompactTabControlButtons &buttons);

/// Updates text, enablement, accessibility, tooltip, and visibility together.
void applyCompactTabControlState(const CompactTabControlButtons &buttons,
                                 const CompactTabStatus &status,
                                 const QString &selectedTitle, bool visible);

}  // namespace chatterino
