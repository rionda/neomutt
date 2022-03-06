/**
 * @file
 * Browser functions
 *
 * @authors
 * Copyright (C) 2021 Richard Russon <rich@flatcap.org>
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
 * @page browser_functions Browser functions
 *
 * Browser functions
 */

#include "config.h"
#include <sys/stat.h>
#include "mutt/lib.h"
#include "email/lib.h"
#include "gui/lib.h"
#include "mutt.h"
#include "functions.h"
#include "attach/lib.h"
#include "index/lib.h"
#include "question/lib.h"
#include "send/lib.h"
#include "mutt_globals.h"
#include "mutt_mailbox.h"
#include "muttlib.h"
#include "opcodes.h"
#include "options.h"
#include "private_data.h"
#ifdef USE_IMAP
#include "imap/lib.h"
#endif
#ifdef USE_NNTP
#include "nntp/lib.h"
#include "nntp/adata.h" // IWYU pragma: keep
#include "nntp/mdata.h" // IWYU pragma: keep
#endif

static const char *Not_available_in_this_menu =
    N_("Not available in this menu");

static int op_subscribe_pattern(struct BrowserPrivateData *priv, int op);

/**
 * destroy_state - Free the BrowserState
 * @param state State to free
 *
 * Frees up the memory allocated for the local-global variables.
 */
void destroy_state(struct BrowserState *state)
{
  struct FolderFile *ff = NULL;
  ARRAY_FOREACH(ff, &state->entry)
  {
    FREE(&ff->name);
    FREE(&ff->desc);
  }
  ARRAY_FREE(&state->entry);

#ifdef USE_IMAP
  FREE(&state->folder);
#endif
}

/**
 * op_browser_new_file - Select a new file in this directory - Implements ::browser_function_t - @ingroup browser_function_api
 */
static int op_browser_new_file(struct BrowserPrivateData *priv, int op)
{
  struct Buffer *buf = mutt_buffer_pool_get();
  mutt_buffer_printf(buf, "%s/", mutt_buffer_string(&LastDir));

  const int rc = mutt_buffer_get_field(_("New file name: "), buf,
                                       MUTT_COMP_FILE, false, NULL, NULL, NULL);
  if (rc != 0)
  {
    mutt_buffer_pool_release(&buf);
    return IR_NO_ACTION;
  }

  mutt_buffer_copy(priv->file, buf);
  mutt_buffer_pool_release(&buf);
  return IR_DONE; // QWQ goto bail;
}

#if defined(USE_IMAP) || defined(USE_NNTP)
/**
 * op_browser_subscribe - Subscribe to current mbox (IMAP/NNTP only) - Implements ::browser_function_t - @ingroup browser_function_api
 *
 * This function handles:
 * - OP_BROWSER_SUBSCRIBE
 * - OP_BROWSER_UNSUBSCRIBE
 */
static int op_browser_subscribe(struct BrowserPrivateData *priv, int op)
{
  if (OptNews)
    return op_subscribe_pattern(priv, op);

  char tmp[256];
  int index = menu_get_index(priv->menu);
  struct FolderFile *ff = ARRAY_GET(&priv->state.entry, index);
  mutt_str_copy(tmp, ff->name, sizeof(tmp));
  mutt_expand_path(tmp, sizeof(tmp));
  imap_subscribe(tmp, (op == OP_BROWSER_SUBSCRIBE));

  return IR_SUCCESS;
}
#endif

/**
 * op_browser_tell - Display the currently selected file's name - Implements ::browser_function_t - @ingroup browser_function_api
 */
static int op_browser_tell(struct BrowserPrivateData *priv, int op)
{
  int index = menu_get_index(priv->menu);
  if (ARRAY_EMPTY(&priv->state.entry))
    return IR_ERROR;

  mutt_message("%s", ARRAY_GET(&priv->state.entry, index)->name);
  return IR_SUCCESS;
}

#ifdef USE_IMAP
/**
 * op_browser_toggle_lsub - Toggle view all/subscribed mailboxes (IMAP only) - Implements ::browser_function_t - @ingroup browser_function_api
 */
static int op_browser_toggle_lsub(struct BrowserPrivateData *priv, int op)
{
  bool_str_toggle(NeoMutt->sub, "imap_list_subscribed", NULL);

  mutt_unget_event(0, OP_CHECK_NEW);
  return IR_SUCCESS;
}
#endif

/**
 * op_browser_view_file - View file - Implements ::browser_function_t - @ingroup browser_function_api
 */
static int op_browser_view_file(struct BrowserPrivateData *priv, int op)
{
  if (ARRAY_EMPTY(&priv->state.entry))
  {
    mutt_error(_("No files match the file mask"));
    return IR_ERROR;
  }

  int index = menu_get_index(priv->menu);
  struct FolderFile *ff = ARRAY_GET(&priv->state.entry, index);
#ifdef USE_IMAP
  if (ff->selectable)
  {
    mutt_buffer_strcpy(priv->file, ff->name);
    return IR_DONE; // QWQ goto bail;
  }
  else
#endif
      if (S_ISDIR(ff->mode) ||
          (S_ISLNK(ff->mode) && link_is_dir(mutt_buffer_string(&LastDir), ff->name)))
  {
    mutt_error(_("Can't view a directory"));
    return IR_ERROR;
  }
  else
  {
    char buf2[PATH_MAX];

    mutt_path_concat(buf2, mutt_buffer_string(&LastDir), ff->name, sizeof(buf2));
    struct Body *b = mutt_make_file_attach(buf2, NeoMutt->sub);
    if (b)
    {
      mutt_view_attachment(NULL, b, MUTT_VA_REGULAR, NULL, NULL, priv->menu->win);
      mutt_body_free(&b);
      menu_queue_redraw(priv->menu, MENU_REDRAW_FULL);
    }
    else
    {
      mutt_error(_("Error trying to view file"));
    }
  }
  return IR_ERROR;
}

#ifdef USE_NNTP
/**
 * op_catchup - Mark all articles in newsgroup as read - Implements ::browser_function_t - @ingroup browser_function_api
 */
static int op_catchup(struct BrowserPrivateData *priv, int op)
{
  if (!OptNews)
    return IR_NOT_IMPL;

  struct NntpMboxData *mdata = NULL;

  int rc = nntp_newsrc_parse(CurrentNewsSrv);
  if (rc < 0)
    return IR_ERROR;

  int index = menu_get_index(priv->menu);
  struct FolderFile *ff = ARRAY_GET(&priv->state.entry, index);
  if (op == OP_CATCHUP)
    mdata = mutt_newsgroup_catchup(priv->mailbox, CurrentNewsSrv, ff->name);
  else
    mdata = mutt_newsgroup_uncatchup(priv->mailbox, CurrentNewsSrv, ff->name);

  if (mdata)
  {
    nntp_newsrc_update(CurrentNewsSrv);
    index = menu_get_index(priv->menu) + 1;
    if (index < priv->menu->max)
      menu_set_index(priv->menu, index);
  }

  if (rc != 0)
    menu_queue_redraw(priv->menu, MENU_REDRAW_INDEX);

  nntp_newsrc_close(CurrentNewsSrv);
  return IR_ERROR;
}
#endif

/**
 * op_change_directory - Change directories - Implements ::browser_function_t - @ingroup browser_function_api
 *
 * This function handles:
 * - OP_GOTO_PARENT
 * - OP_CHANGE_DIRECTORY
 */
static int op_change_directory(struct BrowserPrivateData *priv, int op)
{
#ifdef USE_NNTP
  if (OptNews)
    return IR_NOT_IMPL;
#endif

  struct Buffer *buf = mutt_buffer_pool_get();
  mutt_buffer_copy(buf, &LastDir);
#ifdef USE_IMAP
  if (!priv->state.imap_browse)
#endif
  {
    /* add '/' at the end of the directory name if not already there */
    size_t len = mutt_buffer_len(buf);
    if ((len > 0) && (mutt_buffer_string(&LastDir)[len - 1] != '/'))
      mutt_buffer_addch(buf, '/');
  }

  if (op == OP_CHANGE_DIRECTORY)
  {
    int rc = mutt_buffer_get_field(_("Chdir to: "), buf, MUTT_COMP_FILE, false,
                                   NULL, NULL, NULL);
    if ((rc != 0) && mutt_buffer_is_empty(buf))
    {
      mutt_buffer_pool_release(&buf);
      return IR_NO_ACTION;
    }
  }
  else if (op == OP_GOTO_PARENT)
  {
    mutt_get_parent_path(mutt_buffer_string(buf), buf->data, buf->dsize);
  }

  if (!mutt_buffer_is_empty(buf))
  {
    priv->state.is_mailbox_list = false;
    mutt_buffer_expand_path(buf);
#ifdef USE_IMAP
    if (imap_path_probe(mutt_buffer_string(buf), NULL) == MUTT_IMAP)
    {
      mutt_buffer_copy(&LastDir, buf);
      destroy_state(&priv->state);
      init_state(&priv->state, NULL);
      priv->state.imap_browse = true;
      imap_browse(mutt_buffer_string(&LastDir), &priv->state);
      browser_sort(&priv->state);
      priv->menu->mdata = &priv->state.entry;
      browser_highlight_default(&priv->state, priv->menu);
      init_menu(&priv->state, priv->menu, priv->mailbox, priv->sbar);
    }
    else
#endif
    {
      if (mutt_buffer_string(buf)[0] != '/')
      {
        /* in case dir is relative, make it relative to LastDir,
         * not current working dir */
        struct Buffer *tmp = mutt_buffer_pool_get();
        mutt_buffer_concat_path(tmp, mutt_buffer_string(&LastDir), mutt_buffer_string(buf));
        mutt_buffer_copy(buf, tmp);
        mutt_buffer_pool_release(&tmp);
      }
      /* Resolve path from <chdir>
       * Avoids buildup such as /a/b/../../c
       * Symlinks are always unraveled to keep code simple */
      if (mutt_path_realpath(buf->data) == 0)
      {
        mutt_buffer_pool_release(&buf);
        return IR_DONE; // QWQ break;
      }

      struct stat st = { 0 };
      if (stat(mutt_buffer_string(buf), &st) == 0)
      {
        if (S_ISDIR(st.st_mode))
        {
          destroy_state(&priv->state);
          if (examine_directory(priv->mailbox, priv->menu, &priv->state,
                                mutt_buffer_string(buf),
                                mutt_buffer_string(priv->prefix)) == 0)
          {
            mutt_buffer_copy(&LastDir, buf);
          }
          else
          {
            mutt_error(_("Error scanning directory"));
            if (examine_directory(priv->mailbox, priv->menu, &priv->state,
                                  mutt_buffer_string(&LastDir),
                                  mutt_buffer_string(priv->prefix)) == -1)
            {
              return IR_DONE; // QWQ goto bail;
            }
          }
          browser_highlight_default(&priv->state, priv->menu);
          init_menu(&priv->state, priv->menu, priv->mailbox, priv->sbar);
        }
        else
        {
          mutt_error(_("%s is not a directory"), mutt_buffer_string(buf));
        }
      }
      else
      {
        mutt_perror(mutt_buffer_string(buf));
      }
    }
  }
  mutt_buffer_pool_release(&buf);
  return IR_ERROR;
}

#ifdef USE_IMAP
/**
 * op_create_mailbox - Create a new mailbox (IMAP only) - Implements ::browser_function_t - @ingroup browser_function_api
 */
static int op_create_mailbox(struct BrowserPrivateData *priv, int op)
{
  if (!priv->state.imap_browse)
  {
    mutt_error(_("Create is only supported for IMAP mailboxes"));
    return IR_ERROR;
  }

  if (imap_mailbox_create(mutt_buffer_string(&LastDir)) != 0)
    return IR_ERROR;

  /* TODO: find a way to detect if the new folder would appear in
    *   this window, and insert it without starting over. */
  destroy_state(&priv->state);
  init_state(&priv->state, NULL);
  priv->state.imap_browse = true;
  imap_browse(mutt_buffer_string(&LastDir), &priv->state);
  browser_sort(&priv->state);
  priv->menu->mdata = &priv->state.entry;
  browser_highlight_default(&priv->state, priv->menu);
  init_menu(&priv->state, priv->menu, priv->mailbox, priv->sbar);

  return IR_SUCCESS;
}
#endif

#ifdef USE_IMAP
/**
 * op_delete_mailbox - Delete the current mailbox (IMAP only) - Implements ::browser_function_t - @ingroup browser_function_api
 */
static int op_delete_mailbox(struct BrowserPrivateData *priv, int op)
{
  int index = menu_get_index(priv->menu);
  struct FolderFile *ff = ARRAY_GET(&priv->state.entry, index);
  if (!ff->imap)
  {
    mutt_error(_("Delete is only supported for IMAP mailboxes"));
    return IR_ERROR;
  }

  char msg[128];

  // TODO(sileht): It could be better to select INBOX instead. But I
  // don't want to manipulate Context/Mailboxes/mailbox->account here for now.
  // Let's just protect neomutt against crash for now. #1417
  if (mutt_str_equal(mailbox_path(priv->mailbox), ff->name))
  {
    mutt_error(_("Can't delete currently selected mailbox"));
    return IR_ERROR;
  }

  snprintf(msg, sizeof(msg), _("Really delete mailbox \"%s\"?"), ff->name);
  if (mutt_yesorno(msg, MUTT_NO) != MUTT_YES)
  {
    mutt_message(_("Mailbox not deleted"));
    return IR_NO_ACTION;
  }

  if (imap_delete_mailbox(priv->mailbox, ff->name) != 0)
  {
    mutt_error(_("Mailbox deletion failed"));
    return IR_ERROR;
  }

  /* free the mailbox from the browser */
  FREE(&ff->name);
  FREE(&ff->desc);
  /* and move all other entries up */
  ARRAY_REMOVE(&priv->state.entry, ff);
  mutt_message(_("Mailbox deleted"));
  init_menu(&priv->state, priv->menu, priv->mailbox, priv->sbar);

  return IR_SUCCESS;
}
#endif

/**
 * op_enter_mask - Enter a file mask - Implements ::browser_function_t - @ingroup browser_function_api
 */
static int op_enter_mask(struct BrowserPrivateData *priv, int op)
{
  const struct Regex *c_mask = cs_subset_regex(NeoMutt->sub, "mask");
  struct Buffer *buf = mutt_buffer_pool_get();
  mutt_buffer_strcpy(buf, c_mask ? c_mask->pattern : NULL);
  if (mutt_buffer_get_field(_("File Mask: "), buf, MUTT_COMP_NO_FLAGS, false,
                            NULL, NULL, NULL) != 0)
  {
    mutt_buffer_pool_release(&buf);
    return IR_NO_ACTION;
  }

  mutt_buffer_fix_dptr(buf);

  priv->state.is_mailbox_list = false;
  /* assume that the user wants to see everything */
  if (mutt_buffer_is_empty(buf))
    mutt_buffer_strcpy(buf, ".");

  struct Buffer errmsg = mutt_buffer_make(256);
  int rc = cs_subset_str_string_set(NeoMutt->sub, "mask", mutt_buffer_string(buf), &errmsg);
  mutt_buffer_pool_release(&buf);
  if (CSR_RESULT(rc) != CSR_SUCCESS)
  {
    if (!mutt_buffer_is_empty(&errmsg))
    {
      mutt_error("%s", mutt_buffer_string(&errmsg));
      mutt_buffer_dealloc(&errmsg);
    }
    return IR_DONE; // QWQ break;
  }
  mutt_buffer_dealloc(&errmsg);

  destroy_state(&priv->state);
#ifdef USE_IMAP
  if (priv->state.imap_browse)
  {
    init_state(&priv->state, NULL);
    priv->state.imap_browse = true;
    imap_browse(mutt_buffer_string(&LastDir), &priv->state);
    browser_sort(&priv->state);
    priv->menu->mdata = &priv->state.entry;
    init_menu(&priv->state, priv->menu, priv->mailbox, priv->sbar);
  }
  else
#endif
      if (examine_directory(priv->mailbox, priv->menu, &priv->state,
                            mutt_buffer_string(&LastDir), NULL) == 0)
  {
    init_menu(&priv->state, priv->menu, priv->mailbox, priv->sbar);
  }
  else
  {
    mutt_error(_("Error scanning directory"));
    return IR_DONE; // QWQ goto bail;
  }
  priv->kill_prefix = false;
  if (ARRAY_EMPTY(&priv->state.entry))
  {
    mutt_error(_("No files match the file mask"));
    return IR_DONE; // QWQ break;
  }
  return IR_ERROR;
}

/**
 * op_exit - Exit this menu - Implements ::browser_function_t - @ingroup browser_function_api
 */
static int op_exit(struct BrowserPrivateData *priv, int op)
{
  if (priv->multiple)
  {
    char **tfiles = NULL;

    if (priv->menu->num_tagged)
    {
      *priv->numfiles = priv->menu->num_tagged;
      tfiles = mutt_mem_calloc(*priv->numfiles, sizeof(char *));
      size_t j = 0;
      struct FolderFile *ff = NULL;
      ARRAY_FOREACH(ff, &priv->state.entry)
      {
        if (ff->tagged)
        {
          struct Buffer *buf = mutt_buffer_pool_get();
          mutt_buffer_concat_path(buf, mutt_buffer_string(&LastDir), ff->name);
          mutt_buffer_expand_path(buf);
          tfiles[j++] = mutt_buffer_strdup(buf);
          mutt_buffer_pool_release(&buf);
        }
      }
      *priv->files = tfiles;
    }
    else if (!mutt_buffer_is_empty(priv->file)) /* no tagged entries. return selected entry */
    {
      *priv->numfiles = 1;
      tfiles = mutt_mem_calloc(*priv->numfiles, sizeof(char *));
      mutt_buffer_expand_path(priv->file);
      tfiles[0] = mutt_buffer_strdup(priv->file);
      *priv->files = tfiles;
    }
  }

  return IR_DONE; // QWQ goto bail;
  return IR_ERROR;
}

/**
 * op_generic_select_entry - Select the current entry - Implements ::browser_function_t - @ingroup browser_function_api
 *
 * This function handles:
 * - OP_DESCEND_DIRECTORY
 * - OP_GENERIC_SELECT_ENTRY
 */
static int op_generic_select_entry(struct BrowserPrivateData *priv, int op)
{
  if (ARRAY_EMPTY(&priv->state.entry))
  {
    mutt_error(_("No files match the file mask"));
    return IR_ERROR;
  }

  int index = menu_get_index(priv->menu);
  struct FolderFile *ff = ARRAY_GET(&priv->state.entry, index);
  if (S_ISDIR(ff->mode) ||
      (S_ISLNK(ff->mode) && link_is_dir(mutt_buffer_string(&LastDir), ff->name))
#ifdef USE_IMAP
      || ff->inferiors
#endif
  )
  {
    /* make sure this isn't a MH or maildir mailbox */
    struct Buffer *buf = mutt_buffer_pool_get();
    if (priv->state.is_mailbox_list)
    {
      mutt_buffer_strcpy(buf, ff->name);
      mutt_buffer_expand_path(buf);
    }
#ifdef USE_IMAP
    else if (priv->state.imap_browse)
    {
      mutt_buffer_strcpy(buf, ff->name);
    }
#endif
    else
    {
      mutt_buffer_concat_path(buf, mutt_buffer_string(&LastDir), ff->name);
    }

    enum MailboxType type = mx_path_probe(mutt_buffer_string(buf));
    mutt_buffer_pool_release(&buf);

    if ((op == OP_DESCEND_DIRECTORY) || (type == MUTT_MAILBOX_ERROR) || (type == MUTT_UNKNOWN)
#ifdef USE_IMAP
        || ff->inferiors
#endif
    )
    {
      /* save the old directory */
      mutt_buffer_copy(priv->OldLastDir, &LastDir);

      if (mutt_str_equal(ff->name, ".."))
      {
        size_t lastdirlen = mutt_buffer_len(&LastDir);
        if ((lastdirlen > 1) &&
            mutt_str_equal("..", mutt_buffer_string(&LastDir) + lastdirlen - 2))
        {
          mutt_buffer_addstr(&LastDir, "/..");
        }
        else
        {
          char *p = NULL;
          if (lastdirlen > 1)
            p = strrchr(LastDir.data + 1, '/');

          if (p)
          {
            *p = '\0';
            mutt_buffer_fix_dptr(&LastDir);
          }
          else
          {
            if (mutt_buffer_string(&LastDir)[0] == '/')
              mutt_buffer_strcpy(&LastDir, "/");
            else
              mutt_buffer_addstr(&LastDir, "/..");
          }
        }
      }
      else if (priv->state.is_mailbox_list)
      {
        mutt_buffer_strcpy(&LastDir, ff->name);
        mutt_buffer_expand_path(&LastDir);
      }
#ifdef USE_IMAP
      else if (priv->state.imap_browse)
      {
        mutt_buffer_strcpy(&LastDir, ff->name);
        /* tack on delimiter here */

        /* special case "" needs no delimiter */
        struct Url *url = url_parse(ff->name);
        if (url && url->path && (ff->delim != '\0'))
        {
          mutt_buffer_addch(&LastDir, ff->delim);
        }
        url_free(&url);
      }
#endif
      else
      {
        struct Buffer *tmp = mutt_buffer_pool_get();
        mutt_buffer_concat_path(tmp, mutt_buffer_string(&LastDir), ff->name);
        mutt_buffer_copy(&LastDir, tmp);
        mutt_buffer_pool_release(&tmp);
      }

      destroy_state(&priv->state);
      if (priv->kill_prefix)
      {
        mutt_buffer_reset(priv->prefix);
        priv->kill_prefix = false;
      }
      priv->state.is_mailbox_list = false;
#ifdef USE_IMAP
      if (priv->state.imap_browse)
      {
        init_state(&priv->state, NULL);
        priv->state.imap_browse = true;
        imap_browse(mutt_buffer_string(&LastDir), &priv->state);
        browser_sort(&priv->state);
        priv->menu->mdata = &priv->state.entry;
      }
      else
#endif
      {
        if (examine_directory(priv->mailbox, priv->menu, &priv->state,
                              mutt_buffer_string(&LastDir),
                              mutt_buffer_string(priv->prefix)) == -1)
        {
          /* try to restore the old values */
          mutt_buffer_copy(&LastDir, priv->OldLastDir);
          if (examine_directory(priv->mailbox, priv->menu, &priv->state,
                                mutt_buffer_string(&LastDir),
                                mutt_buffer_string(priv->prefix)) == -1)
          {
            mutt_buffer_strcpy(&LastDir, NONULL(HomeDir));
            return IR_DONE; // QWQ goto bail;
          }
        }
        /* resolve paths navigated from GUI */
        if (mutt_path_realpath(LastDir.data) == 0)
          return IR_DONE; // QWQ break;
      }

      browser_highlight_default(&priv->state, priv->menu);
      init_menu(&priv->state, priv->menu, priv->mailbox, priv->sbar);
      priv->goto_swapper[0] = '\0';
      return IR_DONE; // QWQ break;
    }
  }
  else if (op == OP_DESCEND_DIRECTORY)
  {
    mutt_error(_("%s is not a directory"), ARRAY_GET(&priv->state.entry, index)->name);
    return IR_ERROR;
  }

  if (priv->state.is_mailbox_list || OptNews) /* USE_NNTP */
  {
    mutt_buffer_strcpy(priv->file, ff->name);
    mutt_buffer_expand_path(priv->file);
  }
#ifdef USE_IMAP
  else if (priv->state.imap_browse)
    mutt_buffer_strcpy(priv->file, ff->name);
#endif
  else
  {
    mutt_buffer_concat_path(priv->file, mutt_buffer_string(&LastDir), ff->name);
  }

  return op_exit(priv, op); //QWQ fallthrough to OP_EXIT
}

#ifdef USE_NNTP
/**
 * op_load_active - Load list of all newsgroups from NNTP server - Implements ::browser_function_t - @ingroup browser_function_api
 */
static int op_load_active(struct BrowserPrivateData *priv, int op)
{
  if (!OptNews)
    return IR_NOT_IMPL;

  struct NntpAccountData *adata = CurrentNewsSrv;

  if (nntp_newsrc_parse(adata) < 0)
    return IR_ERROR;

  for (size_t i = 0; i < adata->groups_num; i++)
  {
    struct NntpMboxData *mdata = adata->groups_list[i];
    if (mdata)
      mdata->deleted = true;
  }
  nntp_active_fetch(adata, true);
  nntp_newsrc_update(adata);
  nntp_newsrc_close(adata);

  destroy_state(&priv->state);
  if (priv->state.is_mailbox_list)
  {
    examine_mailboxes(priv->mailbox, priv->menu, &priv->state);
  }
  else
  {
    if (examine_directory(priv->mailbox, priv->menu, &priv->state, NULL, NULL) == -1)
      return IR_DONE; // QWQ break;
  }
  init_menu(&priv->state, priv->menu, priv->mailbox, priv->sbar);
  return IR_ERROR;
}
#endif

/**
 * op_mailbox_list - List mailboxes with new mail - Implements ::browser_function_t - @ingroup browser_function_api
 */
static int op_mailbox_list(struct BrowserPrivateData *priv, int op)
{
  mutt_mailbox_list();
  return IR_SUCCESS;
}

#ifdef USE_IMAP
/**
 * op_rename_mailbox - Rename the current mailbox (IMAP only) - Implements ::browser_function_t - @ingroup browser_function_api
 */
static int op_rename_mailbox(struct BrowserPrivateData *priv, int op)
{
  int index = menu_get_index(priv->menu);
  struct FolderFile *ff = ARRAY_GET(&priv->state.entry, index);
  if (!ff->imap)
  {
    mutt_error(_("Rename is only supported for IMAP mailboxes"));
    return IR_ERROR;
  }

  if (imap_mailbox_rename(ff->name) < 0)
    return IR_ERROR;

  destroy_state(&priv->state);
  init_state(&priv->state, NULL);
  priv->state.imap_browse = true;
  imap_browse(mutt_buffer_string(&LastDir), &priv->state);
  browser_sort(&priv->state);
  priv->menu->mdata = &priv->state.entry;
  browser_highlight_default(&priv->state, priv->menu);
  init_menu(&priv->state, priv->menu, priv->mailbox, priv->sbar);

  return IR_SUCCESS;
}
#endif

/**
 * op_sort - Sort messages - Implements ::browser_function_t - @ingroup browser_function_api
 *
 * This function handles:
 * - OP_SORT
 * - OP_SORT_REVERSE
 */
static int op_sort(struct BrowserPrivateData *priv, int op)
{
  bool resort = true;
  int sort = -1;
  int reverse = (op == OP_SORT_REVERSE);

  switch (mutt_multi_choice(
      (reverse) ?
          /* L10N: The highlighted letters must match the "Sort" options */
          _("Reverse sort by (d)ate, (a)lpha, si(z)e, d(e)scription, "
            "(c)ount, ne(w) count, or do(n)'t sort?") :
          /* L10N: The highlighted letters must match the "Reverse Sort" options */
          _("Sort by (d)ate, (a)lpha, si(z)e, d(e)scription, (c)ount, "
            "ne(w) count, or do(n)'t sort?"),
      /* L10N: These must match the highlighted letters from "Sort" and "Reverse Sort" */
      _("dazecwn")))
  {
    case -1: /* abort */
      resort = false;
      break;

    case 1: /* (d)ate */
      sort = SORT_DATE;
      break;

    case 2: /* (a)lpha */
      sort = SORT_SUBJECT;
      break;

    case 3: /* si(z)e */
      sort = SORT_SIZE;
      break;

    case 4: /* d(e)scription */
      sort = SORT_DESC;
      break;

    case 5: /* (c)ount */
      sort = SORT_COUNT;
      break;

    case 6: /* ne(w) count */
      sort = SORT_UNREAD;
      break;

    case 7: /* do(n)'t sort */
      sort = SORT_ORDER;
      break;
  }

  if (!resort)
    return IR_NO_ACTION;

  sort |= reverse ? SORT_REVERSE : 0;
  cs_subset_str_native_set(NeoMutt->sub, "sort_browser", sort, NULL);
  browser_sort(&priv->state);
  browser_highlight_default(&priv->state, priv->menu);
  menu_queue_redraw(priv->menu, MENU_REDRAW_FULL);
  return IR_SUCCESS;
}

#ifdef USE_NNTP
/**
 * op_subscribe_pattern - Subscribe to newsgroups matching a pattern - Implements ::browser_function_t - @ingroup browser_function_api
 *
 * This function handles:
 * - OP_BROWSER_SUBSCRIBE
 * - OP_SUBSCRIBE_PATTERN
 * - OP_BROWSER_UNSUBSCRIBE
 * - OP_UNSUBSCRIBE_PATTERN
 */
static int op_subscribe_pattern(struct BrowserPrivateData *priv, int op)
{
  if (!OptNews)
    return IR_NOT_IMPL;

  struct NntpAccountData *adata = CurrentNewsSrv;
  regex_t rx = { 0 };
  int index = menu_get_index(priv->menu);

  if ((op == OP_SUBSCRIBE_PATTERN) || (op == OP_UNSUBSCRIBE_PATTERN))
  {
    char tmp2[256];

    struct Buffer *buf = mutt_buffer_pool_get();
    if (op == OP_SUBSCRIBE_PATTERN)
      snprintf(tmp2, sizeof(tmp2), _("Subscribe pattern: "));
    else
      snprintf(tmp2, sizeof(tmp2), _("Unsubscribe pattern: "));
    /* buf comes from the buffer pool, so defaults to size 1024 */
    if ((mutt_buffer_get_field(tmp2, buf, MUTT_COMP_PATTERN, false, NULL, NULL, NULL) != 0) ||
        mutt_buffer_is_empty(buf))
    {
      mutt_buffer_pool_release(&buf);
      return IR_DONE; // QWQ break;
    }

    int err = REG_COMP(&rx, buf->data, REG_NOSUB);
    if (err != 0)
    {
      regerror(err, &rx, buf->data, buf->dsize);
      regfree(&rx);
      mutt_error("%s", mutt_buffer_string(buf));
      mutt_buffer_pool_release(&buf);
      return IR_DONE; // QWQ break;
    }
    menu_queue_redraw(priv->menu, MENU_REDRAW_FULL);
    index = 0;
    mutt_buffer_pool_release(&buf);
  }
  else if (ARRAY_EMPTY(&priv->state.entry))
  {
    mutt_error(_("No newsgroups match the mask"));
    return IR_DONE; // QWQ break;
  }

  int rc = nntp_newsrc_parse(adata);
  if (rc < 0)
    return IR_DONE; // QWQ break;

  struct FolderFile *ff = NULL;
  ARRAY_FOREACH_FROM(ff, &priv->state.entry, index)
  {
    if ((op == OP_BROWSER_SUBSCRIBE) || (op == OP_BROWSER_UNSUBSCRIBE) ||
        (regexec(&rx, ff->name, 0, NULL, 0) == 0))
    {
      if ((op == OP_BROWSER_SUBSCRIBE) || (op == OP_SUBSCRIBE_PATTERN))
        mutt_newsgroup_subscribe(adata, ff->name);
      else
        mutt_newsgroup_unsubscribe(adata, ff->name);
    }
    if ((op == OP_BROWSER_SUBSCRIBE) || (op == OP_BROWSER_UNSUBSCRIBE))
    {
      if ((index + 1) < priv->menu->max)
        menu_set_index(priv->menu, index + 1);
      return IR_DONE; // QWQ break;
    }
  }

  if (op == OP_SUBSCRIBE_PATTERN)
  {
    for (size_t j = 0; adata && (j < adata->groups_num); j++)
    {
      struct NntpMboxData *mdata = adata->groups_list[j];
      if (mdata && mdata->group && !mdata->subscribed)
      {
        if (regexec(&rx, mdata->group, 0, NULL, 0) == 0)
        {
          mutt_newsgroup_subscribe(adata, mdata->group);
          browser_add_folder(priv->menu, &priv->state, mdata->group, NULL, NULL, NULL, mdata);
        }
      }
    }
    init_menu(&priv->state, priv->menu, priv->mailbox, priv->sbar);
  }
  if (rc > 0)
    menu_queue_redraw(priv->menu, MENU_REDRAW_FULL);
  nntp_newsrc_update(adata);
  nntp_clear_cache(adata);
  nntp_newsrc_close(adata);
  if ((op != OP_BROWSER_SUBSCRIBE) && (op != OP_BROWSER_UNSUBSCRIBE))
    regfree(&rx);

  return IR_ERROR;
}
#endif

/**
 * op_toggle_mailboxes - Toggle whether to browse mailboxes or all files - Implements ::browser_function_t - @ingroup browser_function_api
 *
 * This function handles:
 * - OP_CHECK_NEW
 * - OP_TOGGLE_MAILBOXES
 */
static int op_toggle_mailboxes(struct BrowserPrivateData *priv, int op)
{
  if (priv->state.is_mailbox_list)
  {
    priv->last_selected_mailbox = priv->menu->current;
  }

  if (op == OP_TOGGLE_MAILBOXES)
  {
    priv->state.is_mailbox_list = !priv->state.is_mailbox_list;
  }

  if (op == OP_BROWSER_GOTO_FOLDER)
  {
    /* When in mailboxes mode, disables this feature */
    const char *const c_folder = cs_subset_string(NeoMutt->sub, "folder");
    if (c_folder)
    {
      mutt_debug(LL_DEBUG3, "= hit! Folder: %s, LastDir: %s\n", c_folder,
                 mutt_buffer_string(&LastDir));
      if (priv->goto_swapper[0] == '\0')
      {
        if (!mutt_str_equal(mutt_buffer_string(&LastDir), c_folder))
        {
          /* Stores into goto_swapper LastDir, and swaps to `$folder` */
          mutt_str_copy(priv->goto_swapper, mutt_buffer_string(&LastDir),
                        sizeof(priv->goto_swapper));
          mutt_buffer_copy(&LastDirBackup, &LastDir);
          mutt_buffer_strcpy(&LastDir, c_folder);
        }
      }
      else
      {
        mutt_buffer_copy(&LastDirBackup, &LastDir);
        mutt_buffer_strcpy(&LastDir, priv->goto_swapper);
        priv->goto_swapper[0] = '\0';
      }
    }
  }
  destroy_state(&priv->state);
  mutt_buffer_reset(priv->prefix);
  priv->kill_prefix = false;

  if (priv->state.is_mailbox_list)
  {
    examine_mailboxes(priv->mailbox, priv->menu, &priv->state);
  }
#ifdef USE_IMAP
  else if (imap_path_probe(mutt_buffer_string(&LastDir), NULL) == MUTT_IMAP)
  {
    init_state(&priv->state, NULL);
    priv->state.imap_browse = true;
    imap_browse(mutt_buffer_string(&LastDir), &priv->state);
    browser_sort(&priv->state);
    priv->menu->mdata = &priv->state.entry;
  }
#endif
  else if (examine_directory(priv->mailbox, priv->menu, &priv->state,
                             mutt_buffer_string(&LastDir),
                             mutt_buffer_string(priv->prefix)) == -1)
  {
    return IR_DONE; // QWQ goto bail;
  }
  init_menu(&priv->state, priv->menu, priv->mailbox, priv->sbar);
  return IR_ERROR;
}

/**
 * BrowserFunctions - All the NeoMutt functions that the Browser supports
 */
struct BrowserFunction BrowserFunctions[] = {
  // clang-format off
  { OP_BROWSER_GOTO_FOLDER,  op_toggle_mailboxes },
  { OP_BROWSER_NEW_FILE,     op_browser_new_file },
#if defined(USE_IMAP) || defined(USE_NNTP)
  { OP_BROWSER_SUBSCRIBE,    op_browser_subscribe },
#endif
  { OP_BROWSER_TELL,         op_browser_tell },
#ifdef USE_IMAP
  { OP_BROWSER_TOGGLE_LSUB,  op_browser_toggle_lsub },
#endif
#if defined(USE_IMAP) || defined(USE_NNTP)
  { OP_BROWSER_UNSUBSCRIBE,  op_browser_subscribe },
#endif
  { OP_BROWSER_VIEW_FILE,    op_browser_view_file },
#ifdef USE_NNTP
  { OP_CATCHUP,              op_catchup },
#endif
  { OP_CHANGE_DIRECTORY,     op_change_directory },
  { OP_CHECK_NEW,            op_toggle_mailboxes },
#ifdef USE_IMAP
  { OP_CREATE_MAILBOX,       op_create_mailbox },
  { OP_DELETE_MAILBOX,       op_delete_mailbox },
#endif
  { OP_DESCEND_DIRECTORY,    op_generic_select_entry },
  { OP_ENTER_MASK,           op_enter_mask },
  { OP_EXIT,                 op_exit },
  { OP_GENERIC_SELECT_ENTRY, op_generic_select_entry },
  { OP_GOTO_PARENT,          op_change_directory },
#ifdef USE_NNTP
  { OP_LOAD_ACTIVE,          op_load_active },
#endif
  { OP_MAILBOX_LIST,         op_mailbox_list },
#ifdef USE_IMAP
  { OP_RENAME_MAILBOX,       op_rename_mailbox },
#endif
  { OP_SORT,                 op_sort },
  { OP_SORT_REVERSE,         op_sort },
#ifdef USE_NNTP
  { OP_SUBSCRIBE_PATTERN,    op_subscribe_pattern },
#endif
  { OP_TOGGLE_MAILBOXES,     op_toggle_mailboxes },
#ifdef USE_NNTP
  { OP_UNCATCHUP,            op_catchup },
  { OP_UNSUBSCRIBE_PATTERN,  op_subscribe_pattern },
#endif
  { 0, NULL },
  // clang-format on
};

/**
 * browser_function_dispatcher - Perform a Browser function
 * @param win_browser Window for the Index
 * @param op          Operation to perform, e.g. OP_GOTO_PARENT
 * @retval num #IndexRetval, e.g. #IR_SUCCESS
 */
int browser_function_dispatcher(struct MuttWindow *win_browser, int op)
{
  if (!win_browser)
  {
    mutt_error(_(Not_available_in_this_menu));
    return IR_ERROR;
  }

  struct BrowserPrivateData *priv = win_browser->parent->wdata;
  if (!priv)
    return IR_ERROR;

  int rc = IR_UNKNOWN;
  for (size_t i = 0; BrowserFunctions[i].op != OP_NULL; i++)
  {
    const struct BrowserFunction *fn = &BrowserFunctions[i];
    if (fn->op == op)
    {
      rc = fn->function(priv, op);
      break;
    }
  }

  return rc;
}
