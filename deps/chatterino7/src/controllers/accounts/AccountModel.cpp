// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/accounts/AccountModel.hpp"

#include "controllers/accounts/Account.hpp"
#include "util/StandardItemHelper.hpp"

namespace chatterino {

AccountModel::AccountModel(QObject *parent)
    : SignalVectorModel<std::shared_ptr<Account>>(1, parent)
{
}

// turn a vector item into a model row
std::shared_ptr<Account> AccountModel::getItemFromRow(
    std::vector<QStandardItem *> &, const std::shared_ptr<Account> &original)
{
    return original;
}

// turns a row in the model into a vector item
void AccountModel::getRowFromItem(const std::shared_ptr<Account> &item,
                                  std::vector<QStandardItem *> &row)
{
    setStringItem(row[0], item->toString(), false);
    row[0]->setData(QFont("Segoe UI", 10), Qt::FontRole);
    row[0]->setData(static_cast<int>(item->getProviderId()), ProviderIdRole);
}

int AccountModel::beforeInsert(const std::shared_ptr<Account> &item,
                               std::vector<QStandardItem *> &row,
                               int proposedIndex)
{
    (void)row;
    (void)proposedIndex;

    // SignalVectorModel cannot associate a sorted vector insertion with the
    // custom heading immediately preceding its destination account. Derive
    // both parts of the model index from the provider blocks instead.
    const auto &category = item->getCategory();
    int categoryStart = 0;
    for (const auto &[existingCategory, count] : this->categoryCount_)
    {
        if (existingCategory < category)
        {
            categoryStart += count + 1;
        }
    }

    auto count = this->categoryCount_.find(category);
    if (count == this->categoryCount_.end())
    {
        auto newRow = this->createRow();

        setStringItem(newRow[0], category, false, false);
        newRow[0]->setData(QFont("Segoe UI Light", 16), Qt::FontRole);
        newRow[0]->setData(static_cast<int>(item->getProviderId()),
                           ProviderIdRole);

        this->insertCustomRow(std::move(newRow), categoryStart);
        this->categoryCount_.emplace(category, 1);

        return categoryStart + 1;
    }

    int accountIndex = categoryStart + 1;
    for (const auto &existingRow : this->rows())
    {
        if (!existingRow.original)
        {
            continue;
        }
        const auto &existing = *existingRow.original;
        if (existing->getCategory() == category && *existing < *item)
        {
            ++accountIndex;
        }
    }
    ++count->second;
    return accountIndex;
}

void AccountModel::afterRemoved(const std::shared_ptr<Account> &item,
                                std::vector<QStandardItem *> &row, int index)
{
    auto it = this->categoryCount_.find(item->getCategory());
    assert(it != this->categoryCount_.end());

    if (it->second <= 1)
    {
        this->categoryCount_.erase(it);
        this->removeCustomRow(index - 1);
    }
    else
    {
        it->second--;
    }
}

}  // namespace chatterino
