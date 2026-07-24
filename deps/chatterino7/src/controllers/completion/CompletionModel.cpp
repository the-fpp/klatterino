// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/completion/CompletionModel.hpp"

#include "controllers/completion/sources/Source.hpp"

namespace chatterino {

CompletionModel::CompletionModel(QObject *parent)
    : GenericListModel(parent)
{
}

void CompletionModel::setSource(std::unique_ptr<completion::Source> source)
{
    this->source_ = std::move(source);
}

bool CompletionModel::hasSource() const
{
    return this->source_ != nullptr;
}

void CompletionModel::updateResults(const QString &query, size_t maxCount,
                                    const QString &inputContext)
{
    if (this->source_)
    {
        this->source_->setInputContext(inputContext);
        this->source_->update(query);

        // Copy results to this model
        this->clear();
        this->source_->addToListModel(*this, maxCount);
    }
}

void CompletionModel::updateResults(const QString &query, size_t maxCount,
                                    const MessageDraft &inputContext)
{
    if (this->source_)
    {
        this->source_->setInputContext(inputContext);
        this->source_->update(query);

        this->clear();
        this->source_->addToListModel(*this, maxCount);
    }
}

}  // namespace chatterino
