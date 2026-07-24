// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleModeration.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace chatterino::rumble {

ModerationIdentity::ModerationIdentity(QString value)
    : value_(std::move(value))
{
}

Expected<ModerationIdentity, ModerationIdentityError>
    ModerationIdentity::fromProvider(QString value)
{
    if (value.isEmpty())
        return makeUnexpected(ModerationIdentityError::Empty);
    if (value != value.trimmed() || std::ranges::any_of(value, [](QChar ch) {
            return ch.unicode() == 0 || ch.category() == QChar::Other_Control;
        }))
        return makeUnexpected(ModerationIdentityError::NotNormalized);
    return ModerationIdentity(std::move(value));
}

const QString &ModerationIdentity::value() const noexcept
{
    return value_;
}

ModerationAvailability ModerationCapabilities::get(
    ModerationCapability capability) const
{
    return values.value(capability, ModerationAvailability::Unsupported);
}

ModerationState::ModerationState(ModerationScope scope,
                                 std::size_t stateCapacity)
    : scope_(std::move(scope))
    , stateCapacity_(std::max<std::size_t>(1, stateCapacity))
    , replayCapacity_(stateCapacity_ >
                              std::numeric_limits<std::size_t>::max() / 2
                          ? stateCapacity_
                          : stateCapacity_ * 2)
{
}

std::optional<ModerationEvent> moderationEventFromStateOperation(
    const StateOperation &operation, const ModerationScope &scope,
    std::uint64_t revision)
{
    return std::visit(
        [&](const auto &typed) -> std::optional<ModerationEvent> {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, PinMessageEvent>)
            {
                auto id = ModerationIdentity::fromProvider(typed.message.id);
                if (!id)
                    return std::nullopt;
                return ModerationEvent{
                    revision,
                    ModerationPinChanged{scope, std::move(*id)}};
            }
            else
            {
                if constexpr (std::is_same_v<T,
                                             DeleteNonRantMessagesEvent>)
                {
                    if (!typed.clearNonRant)
                        return std::nullopt;
                }
                std::vector<ModerationIdentity> ids;
                ids.reserve(static_cast<std::size_t>(typed.messageIds.size()));
                for (const auto &raw : typed.messageIds)
                {
                    auto id = ModerationIdentity::fromProvider(raw);
                    if (id)
                        ids.push_back(std::move(*id));
                }
                return ModerationEvent{
                    revision,
                    ModerationMessageDeleted{scope, std::move(ids)}};
            }
        },
        operation);
}

ModerationApplyResult ModerationState::apply(const ModerationEvent &event)
{
    const auto *eventScope = std::visit(
        [](const auto &payload) { return &payload.scope; }, event.payload);
    if (*eventScope != scope_)
        return ModerationApplyResult::WrongScope;
    if (revision_ && event.revision <= *revision_)
        return ModerationApplyResult::Stale;

    bool changed = false;
    const auto remember = [this](QString key) {
        if (replayKeys_.contains(key))
            return false;
        if (replayKeyFifo_.size() == replayCapacity_)
        {
            replayKeys_.remove(replayKeyFifo_.front());
            replayKeyFifo_.pop_front();
        }
        replayKeyFifo_.push_back(key);
        replayKeys_.insert(std::move(key));
        return true;
    };
    std::visit(
        [this, &changed, &remember](const auto &payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, ModerationRoleSnapshot>)
            {
                if (roles_ != payload.roles)
                {
                    roles_ = payload.roles;
                    changed = true;
                }
            }
            else if constexpr (std::is_same_v<T,
                                              ModerationMessageDeleted>)
            {
                for (const auto &messageId : payload.messageIds)
                {
                    if (!remember(QStringLiteral("delete:") +
                                  messageId.value()))
                        continue;
                    if (deletedMessageIds_.contains(messageId.value()))
                        continue;
                    if (deletedMessageIdFifo_.size() == stateCapacity_)
                    {
                        deletedMessageIds_.remove(
                            deletedMessageIdFifo_.front());
                        deletedMessageIdFifo_.pop_front();
                    }
                    deletedMessageIdFifo_.push_back(messageId.value());
                    deletedMessageIds_.insert(messageId.value());
                    changed = true;
                }
            }
            else if constexpr (std::is_same_v<T, ModerationPinChanged>)
            {
                const auto next = payload.messageId
                                      ? std::optional<QString>(
                                            payload.messageId->value())
                                      : std::nullopt;
                if (pinnedMessageId_ != next)
                {
                    pinnedMessageId_ = next;
                    changed = true;
                }
            }
            else if constexpr (std::is_same_v<T, ModerationMuteChanged>)
            {
                if (payload.muted)
                {
                    if (mutedUserIds_.contains(payload.userId.value()))
                        return;
                    if (mutedUserIdFifo_.size() == stateCapacity_)
                    {
                        mutedUserIds_.remove(mutedUserIdFifo_.front());
                        mutedUserIdFifo_.pop_front();
                    }
                    mutedUserIdFifo_.push_back(payload.userId.value());
                    mutedUserIds_.insert(payload.userId.value());
                    changed = true;
                }
                else
                {
                    if (mutedUserIds_.remove(payload.userId.value()))
                    {
                        std::erase(mutedUserIdFifo_, payload.userId.value());
                        changed = true;
                    }
                }
            }
        },
        event.payload);
    revision_ = event.revision;
    return changed ? ModerationApplyResult::Applied
                   : ModerationApplyResult::Duplicate;
}

const ModerationScope &ModerationState::scope() const noexcept
{
    return scope_;
}
const ModerationRoles &ModerationState::roles() const noexcept
{
    return roles_;
}
const QSet<QString> &ModerationState::deletedMessageIds() const noexcept
{
    return deletedMessageIds_;
}
const QSet<QString> &ModerationState::mutedUserIds() const noexcept
{
    return mutedUserIds_;
}
const std::optional<QString> &ModerationState::pinnedMessageId() const noexcept
{
    return pinnedMessageId_;
}
bool ModerationState::isMuted(const ModerationIdentity &userId) const
{
    return mutedUserIds_.contains(userId.value());
}

ModerationCapabilities ModerationState::capabilities() const
{
    ModerationCapabilities result;
    result.values.insert(ModerationCapability::ObserveDeletes,
                         ModerationAvailability::Available);
    result.values.insert(ModerationCapability::ObservePins,
                         ModerationAvailability::Available);
    result.values.insert(ModerationCapability::ObserveMutes,
                         ModerationAvailability::Available);

    const bool privileged = roles_.contains(ModerationRole::Broadcaster) ||
                            roles_.contains(ModerationRole::Moderator);
    const auto outbound = privileged ? ModerationAvailability::Unsupported
                                     : ModerationAvailability::Unauthorized;
    for (const auto capability : {
             ModerationCapability::DeleteMessage,
             ModerationCapability::PinMessage,
             ModerationCapability::UnpinMessage,
             ModerationCapability::MuteUser,
             ModerationCapability::UnmuteUser,
             ModerationCapability::BanUser,
             ModerationCapability::UnbanUser,
         })
        result.values.insert(capability, outbound);
    return result;
}

Expected<void, ModerationMutationError> requestModerationMutation(
    ModerationCapability)
{
    return makeUnexpected(ModerationMutationError::Unsupported);
}

ModerationSnapshot moderationSnapshot(const ModerationState &state)
{
    return {state.scope(), state.roles(), state.deletedMessageIds(),
            state.pinnedMessageId(), state.mutedUserIds(),
            state.capabilities()};
}

}  // namespace chatterino::rumble
