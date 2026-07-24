#pragma once

#include <pajlada/signals/signal.hpp>
#include <QString>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

namespace chatterino {

class Channel;
using ChannelPtr = std::shared_ptr<Channel>;

namespace messagehistory {

enum class State : std::uint8_t {
    NotStarted,
    Loading,
    Loaded,
    Failed,
};

class Registry final
{
public:
    static Registry &instance()
    {
        static Registry registry;
        return registry;
    }

    State state(const ChannelPtr &channel) const
    {
        const auto it = this->entries_.find(channel.get());
        if (it == this->entries_.end())
        {
            return State::NotStarted;
        }
        return it->second->state;
    }

    QString error(const ChannelPtr &channel) const
    {
        const auto it = this->entries_.find(channel.get());
        if (it == this->entries_.end())
        {
            return {};
        }
        return it->second->error;
    }

    auto connect(const ChannelPtr &channel,
                 std::function<void(State, const QString &)> callback)
    {
        return this->entryFor(channel)->changed.connect(std::move(callback));
    }

    void setLoading(const ChannelPtr &channel)
    {
        this->set(channel, State::Loading, {});
    }

    void setLoaded(const ChannelPtr &channel)
    {
        this->set(channel, State::Loaded, {});
    }

    void setFailed(const ChannelPtr &channel, QString error)
    {
        this->set(channel, State::Failed, std::move(error));
    }

private:
    struct Entry {
        std::weak_ptr<Channel> channel;
        State state = State::NotStarted;
        QString error;
        pajlada::Signals::Signal<State, const QString &> changed;
    };

    std::shared_ptr<Entry> entryFor(const ChannelPtr &channel)
    {
        auto &entry = this->entries_[channel.get()];
        if (!entry)
        {
            entry = std::make_shared<Entry>();
            entry->channel = channel;
        }
        return entry;
    }

    void set(const ChannelPtr &channel, State state, QString error)
    {
        if (!channel)
        {
            return;
        }

        auto entry = this->entryFor(channel);
        if (entry->state == state && entry->error == error)
        {
            return;
        }

        entry->state = state;
        entry->error = std::move(error);
        entry->changed.invoke(entry->state, entry->error);
        this->removeExpired();
    }

    void removeExpired()
    {
        std::erase_if(this->entries_, [](const auto &item) {
            return item.second->channel.expired();
        });
    }

    std::unordered_map<Channel *, std::shared_ptr<Entry>> entries_;
};

}  // namespace messagehistory
}  // namespace chatterino
