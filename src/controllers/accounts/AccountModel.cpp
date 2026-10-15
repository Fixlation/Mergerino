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
}

int AccountModel::getSortedModelIndex(
    const std::shared_ptr<Account> &item) const
{
    int modelIndex = 0;
    for (const auto &existingRow : this->rows())
    {
        if (existingRow.isCustomRow)
        {
            const auto category =
                existingRow.items[0]->data(Qt::EditRole).toString();
            if (item->getCategory() < category)
            {
                return modelIndex;
            }
        }
        else
        {
            assert(existingRow.original.has_value());
            if (!existingRow.original.value()->operator<(*item))
            {
                return modelIndex;
            }
        }

        modelIndex++;
    }

    return modelIndex;
}

int AccountModel::beforeInsert(const std::shared_ptr<Account> &item,
                               std::vector<QStandardItem *> &row,
                               int proposedIndex)
{
    (void)row;
    (void)proposedIndex;

    // The generic proposed index can include a later category header. Derive
    // the row from the same ordering used by AccountController instead.
    const int sortedIndex = this->getSortedModelIndex(item);

    if (this->categoryCount_[item->getCategory()]++ == 0)
    {
        auto newRow = this->createRow();

        setStringItem(newRow[0], item->getCategory(), false, false);
        newRow[0]->setData(QFont("Segoe UI Light", 16), Qt::FontRole);

        this->insertCustomRow(std::move(newRow), sortedIndex);

        return sortedIndex + 1;
    }

    return sortedIndex;
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
