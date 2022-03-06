/**
 * @file
 * Certificate Verification Dialog
 *
 * @authors
 * Copyright (C) 2017 Richard Russon <rich@flatcap.org>
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
 * @page conn_dlg_verifycert Certificate Verification Dialog
 *
 * The Certificate Verification Dialog lets the user check the details of a
 * certificate.
 *
 * This is a @ref gui_simple
 *
 * ## Windows
 *
 * | Name                            | Type               | See Also                 |
 * | :------------------------------ | :----------------- | :----------------------- |
 * | Certificate Verification Dialog | WT_DLG_CERTIFICATE | dlg_verify_certificate() |
 *
 * **Parent**
 * - @ref gui_dialog
 *
 * **Children**
 * - See: @ref gui_simple
 *
 * ## Data
 *
 * None.
 *
 * ## Events
 *
 * None.  Once constructed, the events are handled by the Menu
 * (part of the @ref gui_simple).
 */

#include "config.h"
#include <stdbool.h>
#include <stdio.h>
#include "mutt/lib.h"
#include "gui/lib.h"
#include "index/lib.h"
#include "menu/lib.h"
#include "opcodes.h"
#include "options.h"
#include "ssl.h"

#ifdef USE_SSL
/// Help Bar for the Certificate Verification dialog
static const struct Mapping VerifyHelp[] = {
  // clang-format off
  { N_("Exit"), OP_EXIT },
  { N_("Help"), OP_HELP },
  { NULL, 0 },
  // clang-format on
};
#endif

/**
 * menu_dialog_dokey - Check if there are any menu key events to process
 * @param menu Current Menu
 * @param ip   KeyEvent ID
 * @retval  0 An event occurred for the menu, or a timeout
 * @retval -1 There was an event, but not for menu
 */
static int menu_dialog_dokey(struct Menu *menu, int *ip)
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
static int menu_dialog_translate_op(int i)
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

#ifdef USE_SSL
/**
 * dlg_verify_certificate - Ask the user to validate the certificate
 * @param title        Menu title
 * @param list         Certificate text to display
 * @param allow_always If true, allow the user to always accept the certificate
 * @param allow_skip   If true, allow the user to skip the verification
 * @retval 1 Reject certificate (or menu aborted)
 * @retval 2 Accept certificate once
 * @retval 3 Accept certificate always/skip (see notes)
 * @retval 4 Accept certificate skip
 *
 * The possible retvals will depend on the parameters.
 * The options are given in the order: Reject, Once, Always, Skip.
 * The retval represents the chosen option.
 */
int dlg_verify_certificate(const char *title, struct ListHead *list,
                           bool allow_always, bool allow_skip)
{
  struct MuttWindow *dlg = simple_dialog_new(MENU_GENERIC, WT_DLG_CERTIFICATE, VerifyHelp);

  struct Menu *menu = dlg->wdata;

  struct MuttWindow *sbar = window_find_child(dlg, WT_STATUS_BAR);
  sbar_set_title(sbar, title);

  struct ListNode *np = NULL;
  STAILQ_FOREACH(np, list, entries)
  {
    menu_add_dialog_row(menu, NONULL(np->data));
  }

  if (allow_always)
  {
    if (allow_skip)
    {
      menu->prompt = _("(r)eject, accept (o)nce, (a)ccept always, (s)kip");
      /* L10N: The letters correspond to the choices in the string:
         "(r)eject, accept (o)nce, (a)ccept always, (s)kip"
         This is an interactive certificate confirmation prompt for an SSL connection. */
      menu->keys = _("roas");
    }
    else
    {
      menu->prompt = _("(r)eject, accept (o)nce, (a)ccept always");
      /* L10N: The letters correspond to the choices in the string:
         "(r)eject, accept (o)nce, (a)ccept always"
         This is an interactive certificate confirmation prompt for an SSL connection. */
      menu->keys = _("roa");
    }
  }
  else
  {
    if (allow_skip)
    {
      menu->prompt = _("(r)eject, accept (o)nce, (s)kip");
      /* L10N: The letters correspond to the choices in the string:
         "(r)eject, accept (o)nce, (s)kip"
         This is an interactive certificate confirmation prompt for an SSL connection. */
      menu->keys = _("ros");
    }
    else
    {
      menu->prompt = _("(r)eject, accept (o)nce");
      /* L10N: The letters correspond to the choices in the string:
         "(r)eject, accept (o)nce"
         This is an interactive certificate confirmation prompt for an SSL connection. */
      menu->keys = _("ro");
    }
  }

  bool old_ime = OptIgnoreMacroEvents;
  OptIgnoreMacroEvents = true;

  // ---------------------------------------------------------------------------
  // Event Loop
  int rc = 0;
  int op = OP_NULL;
  do
  {
    window_redraw(NULL);

    op = km_dokey(menu->type);
    mutt_debug(LL_DEBUG1, "Got op %s (%d)\n", opcodes_get_name(op), op);

    // Try to catch dialog keys before ops
    if (menu_dialog_dokey(menu, &op) == 0)
      continue;

    // Convert menubar movement to scrolling
    op = menu_dialog_translate_op(op);

    rc = menu_function_dispatcher(menu->win, op);
    if (rc == IR_SUCCESS)
      continue;

    switch (op)
    {
      case -1:         // Abort: Ctrl-G
      case OP_EXIT:    // Q)uit
      case OP_MAX + 1: // R)eject
        // rc = 1;
        sbar_set_title(sbar, "ABORT/QUIT/REJECT");
        break;
      case OP_MAX + 2: // O)nce
        // rc = 2;
        sbar_set_title(sbar, "ONCE");
        break;
      case OP_MAX + 3: // A)lways / S)kip
        // rc = 3;
        sbar_set_title(sbar, "ALWAYS/SKIP");
        break;
      case OP_MAX + 4: // S)kip
        // rc = 4;
        sbar_set_title(sbar, "SKIP");
        break;
    }
  }
  while (true);
  // while (rc != IR_DONE);
  // ---------------------------------------------------------------------------

  OptIgnoreMacroEvents = old_ime;

  simple_dialog_free(&dlg);

  return rc;
}
#endif
