/**
 * @file
 * Menu functions
 *
 * @authors
 * Copyright (C) 2022 Richard Russon <rich@flatcap.org>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @page menu_functions Menu functions
 *
 * Menu functions
 */

#include "config.h"
#include "mutt/lib.h"
#include "gui/lib.h"
#include "functions.h"
#include "lib.h"
#include "index/lib.h"
#include "mutt_logging.h"
#include "opcodes.h"

/**
 * menu_dialog_dokey - Check if there are any menu key events to process
 * @param menu Current Menu
 * @param ip   KeyEvent ID
 * @retval  0 An event occurred for the menu, or a timeout
 * @retval -1 There was an event, but not for menu
 */
int menu_dialog_dokey(struct Menu *menu, int *ip)
{
  struct KeyEvent ch = { OP_NULL, OP_NULL };
  char *p = NULL;

  enum MuttCursorState cursor = mutt_curses_set_cursor(MUTT_CURSOR_VISIBLE);
  do
  {
    ch = mutt_getch();
  } while (ch.ch == OP_TIMEOUT);
  mutt_curses_set_cursor(cursor);

  if (ch.ch < 0)
  {
    *ip = -1;
    return 0;
  }

  if ((ch.ch != 0) && (p = strchr(menu->keys, ch.ch)))
  {
    *ip = OP_MAX + (p - menu->keys + 1);
    return 0;
  }
  else
  {
    if (ch.op == OP_NULL)
      mutt_unget_event(ch.ch, OP_NULL);
    else
      mutt_unget_event(0, ch.op);
    return -1;
  }
}

/**
 * menu_dialog_translate_op - Convert menubar movement to scrolling
 * @param i Action requested, e.g. OP_NEXT_ENTRY
 * @retval num Action to perform, e.g. OP_NEXT_LINE
 */
int menu_dialog_translate_op(int i)
{
  switch (i)
  {
    case OP_NEXT_ENTRY:
      return OP_NEXT_LINE;
    case OP_PREV_ENTRY:
      return OP_PREV_LINE;
    case OP_CURRENT_TOP:
    case OP_FIRST_ENTRY:
      return OP_TOP_PAGE;
    case OP_CURRENT_BOTTOM:
    case OP_LAST_ENTRY:
      return OP_BOTTOM_PAGE;
    case OP_CURRENT_MIDDLE:
      return OP_MIDDLE_PAGE;
  }

  return i;
}

// -----------------------------------------------------------------------------

/**
 * MenuFunctions - All the NeoMutt functions that the Menu supports
 */
struct MenuFunction MenuFunctions[] = {
  // clang-format off
  { 0, NULL },
  // clang-format on
};

/**
 * menu_function_dispatcher - Perform a Menu function
 * @param win Menu Window
 * @param op  Operation to perform, e.g. OP_MENU_NEXT
 * @retval num #IndexRetval, e.g. #IR_SUCCESS
 */
int menu_function_dispatcher(struct MuttWindow *win, int op)
{
  if (!win || !win->wdata)
    return IR_UNKNOWN;

  struct Menu *menu = win->wdata;

  // Try to catch dialog keys before ops
  if (!ARRAY_EMPTY(&menu->dialog) && (menu_dialog_dokey(menu, &op) == 0))
    return op;

  // Convert menubar movement to scrolling
  if (!ARRAY_EMPTY(&menu->dialog))
    op = menu_dialog_translate_op(op);

  int rc = IR_UNKNOWN;
  for (size_t i = 0; MenuFunctions[i].op != OP_NULL; i++)
  {
    const struct MenuFunction *fn = &MenuFunctions[i];
    if (fn->op == op)
    {
      rc = fn->function(menu, op);
      break;
    }
  }

  if (rc == IR_UNKNOWN) // Not our function
    return rc;

  const char *result = mutt_map_get_name(rc, RetvalNames);
  mutt_debug(LL_DEBUG1, "Handled %s (%d) -> %s\n", opcodes_get_name(op), op, NONULL(result));

  return rc;
}
