/* Copyright (c) 2021, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/join_optimizer/replace_item.h"
#include "sql/item.h"
#include "sql/sql_resolver.h"
#include "sql/temp_table_param.h"

/**
  Check what field the given item will be materialized into under the given
  temporary table parameters.

  If the item is materialized (ie., found in items_to_copy), we return a
  canonical Item_field for that field; ie., the same every time. This means
  that you can do the same replacement in a SELECT list and then in
  items_to_copy itself, and still have them match. This is used in particular
  when updating Temp_table_param itself, in FinalizePlanForQueryBlock().

  Normally, we want to search for only the same item, up to references
  (need_exact_match=true). However, in ORDER BY specifications of windows,
  we can sometimes have the same field referred to by different Item_field,
  and the callers may need to set need_exact_match=false, which compares
  using Item::eq() instead. This also disables the behavior of checking
  and propagating Item::hidden.
 */
static Item *FindReplacementItem(Item *item,
                                 const Func_ptr_array &items_to_copy,
                                 bool need_exact_match) {
  if (item->const_for_execution()) {
    // Stop traversing (which we do with a fake replacement with ourselves).
    // This is the only case where we can return an Item that is not an
    // Item_field.
    return item;
  }

  for (const Func_ptr &func : items_to_copy) {
    bool match;
    if (need_exact_match) {
      // For nearly all cases, just comparing the items (by pointer) would
      // be sufficent, but in rare cases involving CTEs (see e.g. the test for
      // bug #26907753), we can have a ref in func.func(), so we need to call
      // real_item() before comparing.
      match = func.func()->hidden == item->hidden &&
              func.func()->real_item() == item->real_item();
    } else {
      match = func.func()->eq(item->real_item(), /*binary_cmp=*/true);
    }
    if (match) {
      Item_field *item_field = func.result_item();
      if (item_field == nullptr) return nullptr;
      if (need_exact_match) {
        item_field->hidden = item->hidden;
      }
      return item_field;
    }
  }
  return nullptr;
}

Item *FindReplacementOrReplaceMaterializedItems(
    THD *thd, Item *item, const Func_ptr_array &items_to_copy,
    bool need_exact_match) {
  Item *replacement =
      FindReplacementItem(item, items_to_copy, need_exact_match);
  if (replacement != nullptr) {
    return replacement;
  } else {
    ReplaceMaterializedItems(thd, item, items_to_copy, need_exact_match);
    return item;
  }
}

void ReplaceMaterializedItems(THD *thd, Item *item,
                              const Func_ptr_array &items_to_copy,
                              bool need_exact_match) {
  const auto replace_functor = [&items_to_copy, need_exact_match](
                                   Item *sub_item, Item *,
                                   unsigned) -> ReplaceResult {
    Item *replacement = FindReplacementItem(sub_item->real_item(),
                                            items_to_copy, need_exact_match);
    if (replacement != nullptr) {
      return {ReplaceResult::REPLACE, replacement};
    } else {
      return {ReplaceResult::KEEP_TRAVERSING, nullptr};
    }
  };
  WalkAndReplace(thd, item, std::move(replace_functor));
}
