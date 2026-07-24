#include "controllers/plugins/api/WindowManager.hpp"

#ifdef CHATTERINO_HAVE_PLUGINS

#    include "controllers/plugins/api/ChannelRef.hpp"
#    include "controllers/plugins/SolTypes.hpp"  // IWYU pragma: keep
#    include "messages/Message.hpp"
#    include "singletons/WindowManager.hpp"
#    include "util/MultiChannel.hpp"
#    include "util/WeakPtrHelpers.hpp"
#    include "widgets/Notebook.hpp"
#    include "widgets/splits/Split.hpp"
#    include "widgets/splits/SplitContainer.hpp"
#    include "widgets/Window.hpp"

#    include <algorithm>
#    include <memory>
#    include <vector>

namespace {

using namespace chatterino;

QString normalizedSourceChannelName(QString name)
{
    if (name.startsWith(QStringLiteral(":kick:")))
    {
        name.remove(0, 6);
    }
    if (name.startsWith(QStringLiteral("#")))
    {
        name.remove(0, 1);
    }
    return name.toLower();
}

bool messageMatchesChild(const Message &message,
                         const MultiChannel::ChildChannel &child)
{
    if (!child.channel)
    {
        return false;
    }

    const auto sourceName = normalizedSourceChannelName(message.channelName);
    return multiChannelChildMatches(child, message.platform, sourceName);
}

bool autoSelectMultiChannelContextByRecentMessages(Split &split, size_t limit)
{
    auto multi = std::dynamic_pointer_cast<MultiChannel>(split.getChannel());
    if (!multi || !multi->initialHistorySettled())
    {
        return false;
    }

    auto children = multi->channels();
    if (children.empty())
    {
        return false;
    }

    const bool anyLive = std::any_of(
        children.begin(), children.end(), [](const MultiChannel::ChildChannel &child) {
            return child.channel && child.channel->isLive();
        });

    std::vector<bool> eligible(children.size(), false);
    for (size_t i = 0; i < children.size(); ++i)
    {
        const auto &child = children[i];
        eligible[i] = child.channel && (!anyLive || child.channel->isLive());
    }

    const auto activeIndex = multi->activeChannelIndex();
    size_t bestIndex = children.size();
    if (activeIndex < children.size() && eligible[activeIndex])
    {
        bestIndex = activeIndex;
    }
    else
    {
        for (size_t i = 0; i < children.size(); ++i)
        {
            if (eligible[i])
            {
                bestIndex = i;
                break;
            }
        }
    }

    if (bestIndex == children.size())
    {
        return false;
    }

    std::vector<size_t> counts(children.size(), 0);
    const auto messages = multi->getMessageSnapshot(limit);
    for (const auto &message : messages)
    {
        if (!message)
        {
            continue;
        }

        for (size_t i = 0; i < children.size(); ++i)
        {
            if (!eligible[i])
            {
                continue;
            }
            if (messageMatchesChild(*message, children[i]))
            {
                ++counts[i];
                break;
            }
        }
    }

    for (size_t i = 0; i < children.size(); ++i)
    {
        if (!eligible[i])
        {
            continue;
        }
        if (counts[i] > counts[bestIndex])
        {
            bestIndex = i;
        }
    }

    if (bestIndex == activeIndex)
    {
        return false;
    }

    multi->setActiveChannelIndex(bestIndex);
    return true;
}

/// Create a table with all items from `items` wrapped in a `QPointer`.
sol::table qPointerWrapped(const auto &items, sol::this_state state)
{
    auto tbl =
        sol::state_view(state).create_table(static_cast<int>(items.size()), 0);

    for (size_t idx = 0; idx < items.size(); idx++)
    {
        tbl[static_cast<int>(idx + 1)] = QPointer(items[idx]);
    }
    return tbl;
}

/// Wraps a `std::weak_ptr<SplitContainer::Node>` and adds convenience functions
/// and an `operator==`.
///
/// In Lua, all nodes have this type instead of a raw `weak_ptr`. In Chatterino,
/// nodes are `std::shared_ptr`s and often nodes are passed around as raw
/// pointers.
struct SplitContainerNodeWrap {
    SplitContainerNodeWrap(std::weak_ptr<SplitContainer::Node> ptr)
        : ptr(std::move(ptr))
    {
    }

    sol::table children(sol::this_state state) const
    {
        const auto &nodes = this->strong()->getChildren();
        auto tbl = sol::state_view(state).create_table(
            static_cast<int>(nodes.size()), 0);

        for (size_t idx = 0; idx < nodes.size(); idx++)
        {
            tbl[static_cast<int>(idx + 1)] = SplitContainerNodeWrap{nodes[idx]};
        }
        return tbl;
    }

    bool isValid() const
    {
        return !this->ptr.expired();
    }

    std::shared_ptr<SplitContainer::Node> strong() const
    {
        auto locked = this->ptr.lock();
        if (locked)
        {
            return locked;
        }
        throw std::runtime_error("Split container node does not exist anymore");
    }

    bool operator==(const SplitContainerNodeWrap &other) const
    {
        return weakOwnerEquals(this->ptr, other.ptr);
    }

    static std::optional<SplitContainerNodeWrap> fromPtr(
        SplitContainer::Node *ptr)
    {
        if (ptr)
        {
            return SplitContainerNodeWrap(ptr->weak_from_this());
        }
        return std::nullopt;
    }

private:
    std::weak_ptr<SplitContainer::Node> ptr;
};

}  // namespace

namespace chatterino::lua::api::windowmanager {

void createUserTypes(sol::table &c2)
{
    c2.new_usertype<Split>("Split", sol::no_constructor,  //
                           "channel",
                           sol::readonly_property([](const Split &self) {
                               return ChannelRef(self.getChannel());
                           }),
                           "selected_channel",
                           sol::readonly_property([](const Split &self) {
                               return ChannelRef(self.getSelectedChannel());
                           }),
                           "selected_platform",
                           sol::readonly_property([](const Split &self) {
                               auto channel = self.getSelectedChannel();
                               if (!channel)
                               {
                                   return QString();
                               }
                               if (channel->isKickChannel())
                               {
                                   return QStringLiteral("kick");
                               }
                               if (channel->isTwitchChannel())
                               {
                                   return QStringLiteral("twitch");
                               }
                               if (channel->isRumbleChannel())
                               {
                                   return QStringLiteral("rumble");
                               }
                               return QString();
                           }),
                           "selected_locator",
                           sol::readonly_property([](const Split &self) {
                               return self.getSelectedLocator();
                           }),
                           "selected_is_live",
                           sol::readonly_property([](const Split &self) {
                               auto channel = self.getSelectedChannel();
                               return channel && channel->isLive();
                           }),
                           "auto_select_context_by_recent_messages",
                           [](Split &self, size_t limit) {
                               return autoSelectMultiChannelContextByRecentMessages(
                                   self, limit);
                           });

    c2.new_usertype<SplitContainerNodeWrap>(
        "SplitContainerNode", sol::no_constructor,     //
        "is_valid", &SplitContainerNodeWrap::isValid,  //
        "type", sol::readonly_property([](const SplitContainerNodeWrap &self) {
            return self.strong()->getType();
        }),
        "split", sol::readonly_property([](const SplitContainerNodeWrap &self) {
            return QPointer(self.strong()->getSplit());
        }),
        "parent",
        sol::readonly_property([](const SplitContainerNodeWrap &self) {
            return SplitContainerNodeWrap::fromPtr(self.strong()->getParent());
        }),
        "horizontal_flex",
        sol::readonly_property([](const SplitContainerNodeWrap &self) {
            return self.strong()->getHorizontalFlex();
        }),
        "vertical_flex",
        sol::readonly_property([](const SplitContainerNodeWrap &self) {
            return self.strong()->getVerticalFlex();
        }),
        "children", &SplitContainerNodeWrap::children);

    c2.new_usertype<SplitContainer>(
        "SplitContainer", sol::no_constructor,  //
        "selected_split",
        sol::property(
            [](const SplitContainer &self) {
                return QPointer(self.getSelectedSplit());
            },
            [](SplitContainer &self, Split &split) {
                self.setSelected(&split);
            }),
        "base_node", sol::readonly_property([](SplitContainer &self) {
            return SplitContainerNodeWrap::fromPtr(self.getBaseNode());
        }),  //
        "splits", [](SplitContainer &self, sol::this_state state) {
            return qPointerWrapped(self.getSplits(), state);
        });

    c2.new_usertype<SplitNotebook>(
        "SplitNotebook", sol::no_constructor,  //
        "selected_page", sol::readonly_property([](SplitNotebook &self) {
            return QPointer(self.getSelectedPage());
        }),
        "page_count", sol::readonly_property(&SplitNotebook::getPageCount),
        "page_at", [](const SplitNotebook &self, int index) {
            if (index < 0 || index >= self.getPageCount())
            {
                return QPointer<SplitContainer>(nullptr);
            }
            return QPointer(
                dynamic_cast<SplitContainer *>(self.getPageAt(index)));
        });

    c2.new_usertype<Window>("Window", sol::no_constructor,  //
                            "notebook",
                            sol::readonly_property([](Window &self) {
                                return QPointer(&self.getNotebook());
                            }),
                            "type", sol::readonly_property([](Window &self) {
                                return self.getType();
                            }));

    c2.new_usertype<WindowManager>(
        "WindowManager", sol::no_constructor,  //
        "main_window", sol::readonly_property([](WindowManager &self) {
            return QPointer(&self.getMainWindow());
        }),
        "last_selected_window",
        sol::readonly_property([](const WindowManager &self) {
            return QPointer(self.getLastSelectedWindow());
        }),
        "all", [](const WindowManager &self, sol::this_state state) {
            return qPointerWrapped(self.windows(), state);
        });
}

}  // namespace chatterino::lua::api::windowmanager

#endif
