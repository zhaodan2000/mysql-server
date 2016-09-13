/* Copyright (c) 2013, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "auth_acls.h"
#include "derror.h"
#include "handler.h"
#include "item_create.h"
#include "m_string.h"
#include "my_sqlcommand.h"
#include "my_sys.h"
#include "mysqld_error.h"
#include "parse_tree_helpers.h"
#include "sp_head.h"
#include "sp_instr.h"
#include "sp_pcontext.h"
#include "sql_class.h"
#include "sql_error.h"
#include "sql_lex.h"
#include "sql_plugin.h"
#include "sql_plugin_ref.h"
#include "system_variables.h"
#include "trigger_def.h"


/**
  Create an object to represent a SP variable in the Item-hierarchy.

  @param thd              The current thread.
  @param name             The SP variable name.
  @param spv              The SP variable (optional).
  @param query_start_ptr  Start of the SQL-statement query string (optional).
  @param start            Start position of the SP variable name in the query.
  @param end              End position of the SP variable name in the query.

  @remark If spv is not specified, the name is used to search for the
          variable in the parse-time context. If the variable does not
          exist, a error is set and NULL is returned to the caller.

  @return An Item_splocal object representing the SP variable, or NULL on error.
*/
Item_splocal* create_item_for_sp_var(THD *thd,
                                     LEX_STRING name,
                                     sp_variable *spv,
                                     const char *query_start_ptr,
                                     const char *start,
                                     const char *end)
{
  LEX *lex= thd->lex;
  size_t spv_pos_in_query= 0;
  size_t spv_len_in_query= 0;
  sp_pcontext *pctx= lex->get_sp_current_parsing_ctx();

  /* If necessary, look for the variable. */
  if (pctx && !spv)
    spv= pctx->find_variable(name, false);

  if (!spv)
  {
    my_error(ER_SP_UNDECLARED_VAR, MYF(0), name.str);
    return NULL;
  }

  DBUG_ASSERT(pctx && spv);

  if (query_start_ptr)
  {
    /* Position and length of the SP variable name in the query. */
    spv_pos_in_query= start - query_start_ptr;
    spv_len_in_query= end - start;
  }

  Item_splocal *item=
    new (thd->mem_root) Item_splocal(
      name, spv->offset, spv->type, spv_pos_in_query, spv_len_in_query);

#ifndef DBUG_OFF
  if (item)
    item->m_sp= lex->sphead;
#endif

  return item;
}


bool find_sys_var_null_base(THD *thd, struct sys_var_with_base *tmp)
{
  tmp->var= find_sys_var(thd, tmp->base_name.str, tmp->base_name.length);

  if (tmp->var == NULL)
    my_error(ER_UNKNOWN_SYSTEM_VARIABLE, MYF(0), tmp->base_name.str);
  else
    tmp->base_name= null_lex_str;

  return thd->is_error();
}


/**
  Helper action for a SET statement.
  Used to push a system variable into the assignment list.

  @param thd      the current thread
  @param var_with_base  the system variable with base name
  @param var_type the scope of the variable
  @param val      the value being assigned to the variable

  @return TRUE if error, FALSE otherwise.
*/

bool
set_system_variable(THD *thd, struct sys_var_with_base *var_with_base,
                    enum enum_var_type var_type, Item *val)
{
  set_var *var;
  LEX *lex= thd->lex;
  sp_head *sp= lex->sphead;
  sp_pcontext *pctx= lex->get_sp_current_parsing_ctx();

  /* No AUTOCOMMIT from a stored function or trigger. */
  if (pctx && var_with_base->var == Sys_autocommit_ptr)
    sp->m_flags|= sp_head::HAS_SET_AUTOCOMMIT_STMT;

#ifdef HAVE_REPLICATION
  if (lex->uses_stored_routines() &&
      ((var_with_base->var == Sys_gtid_next_ptr
#ifdef HAVE_GTID_NEXT_LIST
       || var_with_base->var == Sys_gtid_next_list_ptr
#endif
       ) ||
       Sys_gtid_purged_ptr == var_with_base->var))
  {
    my_error(ER_SET_STATEMENT_CANNOT_INVOKE_FUNCTION, MYF(0),
             var_with_base->var->name.str);
    return TRUE;
  }
#endif

  if (val && val->type() == Item::FIELD_ITEM &&
      ((Item_field*)val)->table_name)
  {
    my_error(ER_WRONG_TYPE_FOR_VAR, MYF(0), var_with_base->var->name.str);
    return TRUE;
  }

  if (! (var= new set_var(var_type, var_with_base->var,
         &var_with_base->base_name, val)))
    return TRUE;

  return lex->var_list.push_back(var);
}


/**
  Make a new string allocated on THD's mem-root.

  @param thd        thread handler.
  @param start_ptr  start of the new string.
  @param end_ptr    end of the new string.

  @return LEX_STRING object, containing a pointer to a newly
  constructed/allocated string, and its length. The pointer is NULL
  in case of out-of-memory error.
*/
LEX_STRING make_string(THD *thd, const char *start_ptr, const char *end_ptr)
{
  LEX_STRING s;

  s.length= end_ptr - start_ptr;
  s.str= (char *) thd->alloc(s.length + 1);

  if (s.str)
    strmake(s.str, start_ptr, s.length);

  return s;
}


/**
  Helper action for a SET statement.
  Used to SET a field of NEW row.

  @param pc                 the parse context
  @param trigger_field_name the NEW-row field name
  @param expr_item          the value expression being assigned
  @param expr_query         the value expression query

  @return error status (true if error, false otherwise).
*/

bool set_trigger_new_row(Parse_context *pc,
                         LEX_STRING trigger_field_name,
                         Item *expr_item,
                         LEX_STRING expr_query)
{
  THD *thd= pc->thd;
  LEX *lex= thd->lex;
  sp_head *sp= lex->sphead;

  DBUG_ASSERT(expr_item);
  DBUG_ASSERT(sp->m_trg_chistics.action_time == TRG_ACTION_BEFORE &&
              (sp->m_trg_chistics.event == TRG_EVENT_INSERT ||
               sp->m_trg_chistics.event == TRG_EVENT_UPDATE));

  Item_trigger_field *trg_fld=
    new (pc->mem_root) Item_trigger_field(POS(),
                                          TRG_NEW_ROW,
                                          trigger_field_name.str,
                                          UPDATE_ACL, false);

  if (trg_fld == NULL || trg_fld->itemize(pc, (Item **) &trg_fld))
    return true;
  DBUG_ASSERT(trg_fld->type() == Item::TRIGGER_FIELD_ITEM);

  sp_instr_set_trigger_field *i=
    new (pc->mem_root)
      sp_instr_set_trigger_field(sp->instructions(),
                                 lex,
                                 trigger_field_name,
                                 trg_fld, expr_item,
                                 expr_query);

  if (!i)
    return true;

  /*
    Let us add this item to list of all Item_trigger_field
    objects in trigger.
  */
  sp->m_cur_instr_trig_field_items.link_in_list(trg_fld,
                                                &trg_fld->next_trg_field);

  return sp->add_instr(thd, i);
}


void sp_create_assignment_lex(THD *thd, const char *option_ptr)
{
  sp_head *sp= thd->lex->sphead;

  /*
    We can come here in the following cases:

      1. it's a regular SET statement outside stored programs
        (thd->lex->sphead is NULL);

      2. we're parsing a stored program normally (loading from mysql.proc, ...);

      3. we're re-parsing SET-statement with a user variable after meta-data
        change. It's guaranteed, that:
        - this SET-statement deals with a user/system variable (otherwise, it
          would be a different SP-instruction, and we would parse an expression);
        - this SET-statement has a single user/system variable assignment
          (that's how we generate sp_instr_stmt-instructions for SET-statements).
        So, in this case, even if thd->lex->sphead is set, we should not process
        further.
  */

  if (!sp ||            // case #1
      sp->is_invoked()) // case #3
  {
    return;
  }

  LEX *old_lex= thd->lex;
  sp->reset_lex(thd);
  LEX * const lex= thd->lex;

  /* Set new LEX as if we at start of set rule. */
  lex->sql_command= SQLCOM_SET_OPTION;
  lex->var_list.empty();
  lex->autocommit= false;

  /*
    It's a SET statement within SP. It will be either translated
    into one or more sp_instr_stmt instructions, or it will be
    sp_instr_set / sp_instr_set_trigger_field instructions.
    In any case, position of SP-variable can not be determined
    reliably. So, we set the start pointer of the current statement
    to NULL.
  */
  sp->m_parser_data.set_current_stmt_start_ptr(NULL);
  sp->m_parser_data.set_option_start_ptr(option_ptr);

  /* Inherit from outer lex. */
  lex->option_type= old_lex->option_type;
}


/**
  Create a SP instruction for a SET assignment.

  @see sp_create_assignment_lex

  @param thd           Thread context
  @param expr_end_ptr  Option-value-expression end pointer

  @return false if success, true otherwise.
*/

bool sp_create_assignment_instr(THD *thd, const char *expr_end_ptr)
{
  LEX *lex= thd->lex;
  sp_head *sp= lex->sphead;

  /*
    We can come here in the following cases:

      1. it's a regular SET statement outside stored programs
        (lex->sphead is NULL);

      2. we're parsing a stored program normally (loading from mysql.proc, ...);

      3. we're re-parsing SET-statement with a user variable after meta-data
        change. It's guaranteed, that:
        - this SET-statement deals with a user/system variable (otherwise, it
          would be a different SP-instruction, and we would parse an expression);
        - this SET-statement has a single user/system variable assignment
          (that's how we generate sp_instr_stmt-instructions for SET-statements).
        So, in this case, even if lex->sphead is set, we should not process
        further.
  */

  if (!sp ||            // case #1
      sp->is_invoked()) // case #3
  {
    return false;
  }

  if (!lex->var_list.is_empty())
  {
    /* Extract expression string. */

    const char *expr_start_ptr= sp->m_parser_data.get_option_start_ptr();

    LEX_STRING expr;
    expr.str= (char *) expr_start_ptr;
    expr.length= expr_end_ptr - expr_start_ptr;

    /* Construct SET-statement query. */

    LEX_STRING set_stmt_query;

    set_stmt_query.length= expr.length + 3;
    set_stmt_query.str= (char *) thd->alloc(set_stmt_query.length + 1);

    if (!set_stmt_query.str)
      return true;

    strmake(strmake(set_stmt_query.str, "SET", 3),
            expr.str, expr.length);

    /*
      We have assignment to user or system variable or option setting, so we
      should construct sp_instr_stmt for it.
    */

    sp_instr_stmt *i=
      new (thd->mem_root)
        sp_instr_stmt(sp->instructions(), lex, set_stmt_query);

    if (!i || sp->add_instr(thd, i))
      return true;
  }

  /* Remember option_type of the currently parsed LEX. */
  enum_var_type inner_option_type= lex->option_type;

  if (sp->restore_lex(thd))
    return true;

  /* Copy option_type to outer lex in case it has changed. */
  thd->lex->option_type= inner_option_type;

  return false;
}

/**
  Resolve engine by its name

  @param        thd            Thread handler.
  @param        name           Engine's name.
  @param        is_temp_table  True if temporary table.
  @param        strict         Force error if engine is unknown(*).
  @param[out]   ret            Engine object or NULL(**).

  @returns true if error is reported(**), otherwise false.

  @note *) NO_ENGINE_SUBSTITUTION sql_mode overrides the @c strict parameter.
  @note **) If @c strict if false and engine is unknown, the function outputs
            a warning, sets @c ret to NULL and returns false (success).
*/
bool resolve_engine(THD *thd,
                    const LEX_STRING &name,
                    bool is_temp_table,
                    bool strict,
                    handlerton **ret)
{
  plugin_ref plugin= ha_resolve_by_name(thd, &name, is_temp_table);
  if (plugin)
  {
    *ret= plugin_data<handlerton*>(plugin);
    return false;
  }

  if (strict || (thd->variables.sql_mode & MODE_NO_ENGINE_SUBSTITUTION))
  {
    my_error(ER_UNKNOWN_STORAGE_ENGINE, MYF(0), name.str);
    return true;
  }
  push_warning_printf(thd, Sql_condition::SL_WARNING,
                      ER_UNKNOWN_STORAGE_ENGINE,
                      ER_THD(thd, ER_UNKNOWN_STORAGE_ENGINE),
                      name.str);
  *ret= NULL;
  return false;
}
