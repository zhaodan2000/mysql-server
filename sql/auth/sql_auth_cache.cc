/* Copyright (c) 2000, 2016, Oracle and/or its affiliates. All rights reserved.

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

#include "sql_auth_cache.h"

#include <stdarg.h>
#include <stdlib.h>

#include "auth_acls.h"
#include "auth_common.h"        // ACL_internal_schema_access
#include "auth_internal.h"      // auth_plugin_is_built_in
#include "current_thd.h"        // current_thd
#include "debug_sync.h"
#include "error_handler.h"      // Internal_error_handler
#include "field.h"              // Field
#include "item_func.h"          // mqh_used
#include "log.h"                // sql_print_warning
#include "m_ctype.h"
#include "m_string.h"           // LEX_CSTRING
#include "mdl.h"
#include "my_base.h"
#include "my_compiler.h"
#include "my_config.h"
#include "my_dbug.h"
#include "my_decimal.h"
#include "mysql/plugin.h"
#include "mysql/plugin_auth.h"  // st_mysql_auth
#include "mysql/psi/mysql_mutex.h"
#include "mysql/psi/psi_base.h"
#include "mysql/psi/psi_mutex.h"
#include "mysql/service_mysql_alloc.h"
#include "mysqld.h"             // my_localhost
#include "mysqld_error.h"
#include "psi_memory_key.h"     // key_memory_acl_mem
#include "records.h"            // READ_RECORD
#include "session_tracker.h"
#include "set_var.h"
#include "sql_authentication.h" // sha256_password_plugin_name
#include "sql_base.h"           // open_and_lock_tables
#include "sql_class.h"          // THD
#include "sql_const.h"
#include "sql_error.h"
#include "sql_lex.h"
#include "sql_plugin.h"         // my_plugin_lock_by_name
#include "sql_security_ctx.h"
#include "sql_servers.h"
#include "sql_string.h"
#include "sql_thd_internal_api.h"  // create_thd
#include "sql_time.h"           // str_to_time_with_warn
#include "sql_user_table.h"
#include "system_variables.h"
#include "table.h"              // TABLE
#include "template_utils.h"
#include "thr_lock.h"
#include "thr_malloc.h"
#include "thr_mutex.h"
#include "xa.h"

#define INVALID_DATE "0000-00-00 00:00:00"

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

using std::min;

#ifndef NO_EMBEDDED_ACCESS_CHECKS
PSI_mutex_key key_LOCK_acl_cache_flush;
PSI_mutex_info all_acl_cache_mutexes[]=
{ {&key_LOCK_acl_cache_flush, "LOCK_acl_cache_flush", PSI_FLAG_GLOBAL, 0} };
Acl_cache *g_acl_cache= NULL;
Acl_cache *get_global_acl_cache() { return g_acl_cache; }
ulong get_global_acl_cache_size() { return g_acl_cache->size(); }
void init_acl_cache();
extern Role_index_map *g_authid_to_vertex;
extern Granted_roles_graph *g_granted_roles;
#include <boost/property_map/property_map.hpp>
#endif

struct ACL_internal_schema_registry_entry
{
  const LEX_STRING *m_name;
  const ACL_internal_schema_access *m_access;
};
/**
  Internal schema registered.
  Currently, this is only:
  - performance_schema
  - information_schema,
  This can be reused later for:
  - mysql
*/
static ACL_internal_schema_registry_entry registry_array[2];
static uint m_registry_array_size= 0;

#ifndef NO_EMBEDDED_ACCESS_CHECKS
MEM_ROOT global_acl_memory;
MEM_ROOT memex;
Prealloced_array<ACL_USER, ACL_PREALLOC_SIZE> *acl_users= NULL;
Prealloced_array<ACL_PROXY_USER, ACL_PREALLOC_SIZE> *acl_proxy_users= NULL;
Prealloced_array<ACL_DB, ACL_PREALLOC_SIZE> *acl_dbs= NULL;
Prealloced_array<ACL_HOST_AND_IP, ACL_PREALLOC_SIZE> *acl_wild_hosts= NULL;
Db_access_map acl_db_map;
Default_roles *g_default_roles= NULL;

HASH column_priv_hash, proc_priv_hash, func_priv_hash;
HASH db_cache;
HASH acl_check_hosts;

bool initialized=0;
bool acl_cache_initialized= false;
bool allow_all_hosts=1;
uint grant_version=0; /* Version of priv tables */
my_bool validate_user_plugins= TRUE;

#define IP_ADDR_STRLEN (3 + 1 + 3 + 1 + 3 + 1 + 3)
#define ACL_KEY_LENGTH (IP_ADDR_STRLEN + 1 + NAME_LEN + \
                        1 + USERNAME_LENGTH + 1)

#endif /* NO_EMBEDDED_ACCESS_CHECKS */

/**
  Add an internal schema to the registry.
  @param name the schema name
  @param access the schema ACL specific rules
*/
void ACL_internal_schema_registry::register_schema
  (const LEX_STRING &name, const ACL_internal_schema_access *access)
{
  DBUG_ASSERT(m_registry_array_size < array_elements(registry_array));

  /* Not thread safe, and does not need to be. */
  registry_array[m_registry_array_size].m_name= &name;
  registry_array[m_registry_array_size].m_access= access;
  m_registry_array_size++;
}


/**
  Search per internal schema ACL by name.
  @param name a schema name
  @return per schema rules, or NULL
*/
const ACL_internal_schema_access *
ACL_internal_schema_registry::lookup(const char *name)
{
  DBUG_ASSERT(name != NULL);

  uint i;

  for (i= 0; i<m_registry_array_size; i++)
  {
    if (my_strcasecmp(system_charset_info, registry_array[i].m_name->str,
                      name) == 0)
      return registry_array[i].m_access;
  }
  return NULL;
}


const char *
ACL_HOST_AND_IP::calc_ip(const char *ip_arg, long *val, char end)
{
  long ip_val,tmp;
  if (!(ip_arg=str2int(ip_arg,10,0,255,&ip_val)) || *ip_arg != '.')
    return 0;
  ip_val<<=24;
  if (!(ip_arg=str2int(ip_arg+1,10,0,255,&tmp)) || *ip_arg != '.')
    return 0;
  ip_val+=tmp<<16;
  if (!(ip_arg=str2int(ip_arg+1,10,0,255,&tmp)) || *ip_arg != '.')
    return 0;
  ip_val+=tmp<<8;
  if (!(ip_arg=str2int(ip_arg+1,10,0,255,&tmp)) || *ip_arg != end)
    return 0;
  *val=ip_val+tmp;
  return ip_arg;
}

/**
  @brief Update the hostname. Updates ip and ip_mask accordingly.

  @param host_arg Value to be stored
 */
void
ACL_HOST_AND_IP::update_hostname(const char *host_arg)
{
  hostname=(char*) host_arg;     // This will not be modified!
  hostname_length= hostname ? strlen( hostname ) : 0;
  if (!host_arg ||
      (!(host_arg=(char*) calc_ip(host_arg,&ip,'/')) ||
       !(host_arg=(char*) calc_ip(host_arg+1,&ip_mask,'\0'))))
  {
    ip= ip_mask=0;               // Not a masked ip
  }
}

/*
   @brief Comparing of hostnames.

   @TODO This function should ideally only
   be called during authentication and not from authorization code. You may
   authenticate with a hostmask, but all authentication should be against a
   specific security context with a specific authentication ID.

   @param  host_arg    Hostname to be compared with
   @param  ip_arg      IP address to be compared with

   @notes
   A hostname may be of type:
   1) hostname   (May include wildcards);   monty.pp.sci.fi
   2) ip     (May include wildcards);   192.168.0.0
   3) ip/netmask                        192.168.0.0/255.255.255.0
   A net mask of 0.0.0.0 is not allowed.

   @return
   true   if matched
   false  if not matched
 */

bool
ACL_HOST_AND_IP::compare_hostname(const char *host_arg, const char *ip_arg)
{
  long tmp;
  if (ip_mask && ip_arg && calc_ip(ip_arg,&tmp,'\0'))
  {
    return (tmp & ip_mask) == ip;
  }
  return (!hostname ||
      (host_arg &&
       !wild_case_compare(system_charset_info, host_arg, hostname)) ||
      (ip_arg && !wild_compare(ip_arg, hostname, 0)));
}

ACL_USER *
ACL_USER::copy(MEM_ROOT *root)
{
  ACL_USER *dst= (ACL_USER *) alloc_root(root, sizeof(ACL_USER));
  if (!dst)
    return 0;
  *dst= *this;
  dst->user= safe_strdup_root(root, user);
  dst->ssl_cipher= safe_strdup_root(root, ssl_cipher);
  dst->x509_issuer= safe_strdup_root(root, x509_issuer);
  dst->x509_subject= safe_strdup_root(root, x509_subject);
  /*
     If the plugin is built in we don't need to reallocate the name of the
     plugin.
   */
  if (auth_plugin_is_built_in(dst->plugin.str))
    dst->plugin= plugin;
  else
  {
    dst->plugin.str= strmake_root(root, plugin.str, plugin.length);
    dst->plugin.length= plugin.length;
  }
  dst->auth_string.str= safe_strdup_root(root, auth_string.str);
  dst->host.update_hostname(safe_strdup_root(root, host.get_host()));
  return dst;
}

void
ACL_PROXY_USER::init(const char *host_arg, const char *user_arg,
                     const char *proxied_host_arg,
                     const char *proxied_user_arg, bool with_grant_arg)
{
  user= (user_arg && *user_arg) ? user_arg : NULL;
  host.update_hostname ((host_arg && *host_arg) ? host_arg : NULL);
  proxied_user= (proxied_user_arg && *proxied_user_arg) ?
    proxied_user_arg : NULL;
  proxied_host.update_hostname ((proxied_host_arg && *proxied_host_arg) ?
      proxied_host_arg : NULL);
  with_grant= with_grant_arg;
  sort= get_sort(4, host.get_host(), user,
      proxied_host.get_host(), proxied_user);
}

void
ACL_PROXY_USER::init(MEM_ROOT *mem, const char *host_arg, const char *user_arg,
                     const char *proxied_host_arg,
                     const char *proxied_user_arg, bool with_grant_arg)
{
  init ((host_arg && *host_arg) ? strdup_root (mem, host_arg) : NULL,
      (user_arg && *user_arg) ? strdup_root (mem, user_arg) : NULL,
      (proxied_host_arg && *proxied_host_arg) ?
      strdup_root (mem, proxied_host_arg) : NULL,
      (proxied_user_arg && *proxied_user_arg) ?
      strdup_root (mem, proxied_user_arg) : NULL,
      with_grant_arg);
}

void
ACL_PROXY_USER::init(TABLE *table, MEM_ROOT *mem)
{
  init (get_field(mem, table->field[MYSQL_PROXIES_PRIV_HOST]),
        get_field(mem, table->field[MYSQL_PROXIES_PRIV_USER]),
        get_field(mem, table->field[MYSQL_PROXIES_PRIV_PROXIED_HOST]),
        get_field(mem, table->field[MYSQL_PROXIES_PRIV_PROXIED_USER]),
                  table->field[MYSQL_PROXIES_PRIV_WITH_GRANT]->val_int() != 0);
}

bool
ACL_PROXY_USER::check_validity(bool check_no_resolve)
{
  if (check_no_resolve &&
      (hostname_requires_resolving(host.get_host()) ||
       hostname_requires_resolving(proxied_host.get_host())))
  {
    sql_print_warning("'proxies_priv' entry '%s@%s %s@%s' "
                      "ignored in --skip-name-resolve mode.",
                      proxied_user ? proxied_user : "",
                      proxied_host.get_host() ? proxied_host.get_host() : "",
                      user ? user : "",
                      host.get_host() ? host.get_host() : "");
  }
  return FALSE;
}

bool
ACL_PROXY_USER::matches(const char *host_arg, const char *user_arg,
                        const char *ip_arg, const char *proxied_user_arg,
						bool any_proxy_user)
{
  DBUG_ENTER("ACL_PROXY_USER::matches");
  DBUG_PRINT("info", ("compare_hostname(%s,%s,%s) &&"
             "compare_hostname(%s,%s,%s) &&"
             "wild_compare (%s,%s) &&"
             "wild_compare (%s,%s)",
             host.get_host() ? host.get_host() : "<NULL>",
             host_arg ? host_arg : "<NULL>",
             ip_arg ? ip_arg : "<NULL>",
             proxied_host.get_host() ? proxied_host.get_host() : "<NULL>",
             host_arg ? host_arg : "<NULL>",
             ip_arg ? ip_arg : "<NULL>",
             user_arg ? user_arg : "<NULL>",
             user ? user : "<NULL>",
             proxied_user_arg ? proxied_user_arg : "<NULL>",
             proxied_user ? proxied_user : "<NULL>"));
  DBUG_RETURN(host.compare_hostname(host_arg, ip_arg) &&
              proxied_host.compare_hostname(host_arg, ip_arg) &&
              (!user ||
               (user_arg && !wild_compare(user_arg, user, TRUE))) &&
              (any_proxy_user || !proxied_user ||
               (proxied_user && !wild_compare(proxied_user_arg, proxied_user,
                                              TRUE))));
}

bool
ACL_PROXY_USER::pk_equals(ACL_PROXY_USER *grant)
{
  DBUG_ENTER("pk_equals");
  DBUG_PRINT("info", ("strcmp(%s,%s) &&"
             "strcmp(%s,%s) &&"
             "wild_compare (%s,%s) &&"
             "wild_compare (%s,%s)",
             user ? user : "<NULL>",
             grant->user ? grant->user : "<NULL>",
             proxied_user ? proxied_user : "<NULL>",
             grant->proxied_user ? grant->proxied_user : "<NULL>",
             host.get_host() ? host.get_host() : "<NULL>",
             grant->host.get_host() ? grant->host.get_host() : "<NULL>",
             proxied_host.get_host() ? proxied_host.get_host() : "<NULL>",
             grant->proxied_host.get_host() ?
             grant->proxied_host.get_host() : "<NULL>"));

  DBUG_RETURN(auth_element_equals(user, grant->user) &&
              auth_element_equals(proxied_user, grant->proxied_user) &&
              auth_element_equals(host.get_host(), grant->host.get_host()) &&
              auth_element_equals(proxied_host.get_host(),
                                  grant->proxied_host.get_host()));
}

void
ACL_PROXY_USER::print_grant(String *str)
{
  str->append(STRING_WITH_LEN("GRANT PROXY ON '"));
  if (proxied_user)
    str->append(proxied_user, strlen(proxied_user));
  str->append(STRING_WITH_LEN("'@'"));
  if (proxied_host.get_host())
    str->append(proxied_host.get_host(), strlen(proxied_host.get_host()));
  str->append(STRING_WITH_LEN("' TO '"));
  if (user)
    str->append(user, strlen(user));
  str->append(STRING_WITH_LEN("'@'"));
  if (host.get_host())
    str->append(host.get_host(), strlen(host.get_host()));
  str->append(STRING_WITH_LEN("'"));
  if (with_grant)
    str->append(STRING_WITH_LEN(" WITH GRANT OPTION"));
}

int
ACL_PROXY_USER::store_pk(TABLE *table,
                         const LEX_CSTRING &host,
                         const LEX_CSTRING &user,
                         const LEX_CSTRING &proxied_host,
                         const LEX_CSTRING &proxied_user)
{
  DBUG_ENTER("ACL_PROXY_USER::store_pk");
  DBUG_PRINT("info", ("host=%s, user=%s, proxied_host=%s, proxied_user=%s",
                      host.str ? host.str : "<NULL>",
                      user.str ? user.str : "<NULL>",
                      proxied_host.str ? proxied_host.str : "<NULL>",
                      proxied_user.str ? proxied_user.str : "<NULL>"));
  if (table->field[MYSQL_PROXIES_PRIV_HOST]->store(host.str,
                                                   host.length,
                                                   system_charset_info))
    DBUG_RETURN(TRUE);
  if (table->field[MYSQL_PROXIES_PRIV_USER]->store(user.str,
                                                   user.length,
                                                   system_charset_info))
    DBUG_RETURN(TRUE);
  if (table->field[MYSQL_PROXIES_PRIV_PROXIED_HOST]->store(proxied_host.str,
                                                           proxied_host.length,
                                                           system_charset_info))
    DBUG_RETURN(TRUE);
  if (table->field[MYSQL_PROXIES_PRIV_PROXIED_USER]->store(proxied_user.str,
                                                           proxied_user.length,
                                                           system_charset_info))
    DBUG_RETURN(TRUE);

  DBUG_RETURN(FALSE);
}

int
ACL_PROXY_USER::store_with_grant(TABLE * table,
                                 bool with_grant)
{
  DBUG_ENTER("ACL_PROXY_USER::store_with_grant");
  DBUG_PRINT("info", ("with_grant=%s", with_grant ? "TRUE" : "FALSE"));
  if (table->field[MYSQL_PROXIES_PRIV_WITH_GRANT]->store(with_grant ? 1 : 0,
                                                         TRUE))
    DBUG_RETURN(TRUE);

  DBUG_RETURN(FALSE);
}

int
ACL_PROXY_USER::store_data_record(TABLE *table,
                                  const LEX_CSTRING &host,
                                  const LEX_CSTRING &user,
                                  const LEX_CSTRING &proxied_host,
                                  const LEX_CSTRING &proxied_user,
                                  bool with_grant,
                                  const char *grantor)
{
  DBUG_ENTER("ACL_PROXY_USER::store_pk");
  if (store_pk(table,  host, user, proxied_host, proxied_user))
    DBUG_RETURN(TRUE);
  if (store_with_grant(table, with_grant))
    DBUG_RETURN(TRUE);
  if (table->field[MYSQL_PROXIES_PRIV_GRANTOR]->store(grantor,
                                                      strlen(grantor),
                                                      system_charset_info))
    DBUG_RETURN(TRUE);

  DBUG_RETURN(FALSE);
}

int wild_case_compare(CHARSET_INFO *cs, const char *str, size_t str_len,
                      const char *wildstr, size_t wildstr_len)
{
  int flag;
  DBUG_ENTER("wild_case_compare");
  DBUG_PRINT("enter",("str: '%s'  wildstr: '%s'",str,wildstr));
  const char *wildstr_end= wildstr + wildstr_len;
  const char *str_end= str + str_len;
  while (wildstr != wildstr_end && str != str_end)
  {
    while (wildstr != wildstr_end &&
           *wildstr != wild_many && *wildstr != wild_one)
    {
      if (*wildstr == wild_prefix && wildstr[1])
        wildstr++;
      if (my_toupper(cs, *wildstr++) != my_toupper(cs, *str++))
        DBUG_RETURN(1);
    }
    if (wildstr == wildstr_end) DBUG_RETURN (str != str_end);
    if (*wildstr++ == wild_one)
    {
      ++str;
      if (str == str_end) DBUG_RETURN (wildstr != wildstr_end);    /* One char; skip */
    }
    else
    {                                           /* Found '*' */
      if (wildstr == wildstr_end) DBUG_RETURN(0);            /* '*' as last char: OK */
      flag=(*wildstr != wild_many && *wildstr != wild_one);
      do
      {
        if (flag)
        {
          char cmp;
          if ((cmp= *wildstr) == wild_prefix && wildstr[1])
            cmp=wildstr[1];
          cmp=my_toupper(cs, cmp);
          while (str != str_end && my_toupper(cs, *str) != cmp)
            str++;
          if (str == str_end) DBUG_RETURN (1);
        }
        if (wild_case_compare(cs, str, str_end - str, wildstr,
                              wildstr_end - wildstr) == 0)
          DBUG_RETURN (0);
        ++str;
      } while (str != str_end);
      DBUG_RETURN(1);
    }
  }
  DBUG_RETURN (str != str_end);
}

int wild_case_compare(CHARSET_INFO *cs, const char *str,const char *wildstr)
{
  return wild_case_compare(cs, str, strlen(str), wildstr, strlen(wildstr));
}


/*
  Return a number which, if sorted 'desc', puts strings in this order:
    no wildcards
    strings containg wildcards and non-wildcard characters
    single muilt-wildcard character('%')
    empty string
*/

ulong get_sort(uint count,...)
{
  va_list args;
  va_start(args,count);
  ulong sort=0;

  /* Should not use this function with more than 4 arguments for compare. */
  DBUG_ASSERT(count <= 4);

  while (count--)
  {
    char *start, *str= va_arg(args,char*);
    uint chars= 0;
    uint wild_pos= 0;

    /*
      wild_pos
        0                            if string is empty
        1                            if string is a single muilt-wildcard
                                     character('%')
        first wildcard position + 1  if string containg wildcards and
                                     non-wildcard characters
    */

    if ((start= str))
    {
      for (; *str ; str++)
      {
        if (*str == wild_prefix && str[1])
          str++;
        else if (*str == wild_many || *str == wild_one)
        {
          wild_pos= (uint) (str - start) + 1;
          if (!(wild_pos == 1 && *str == wild_many && *(++str) == '\0'))
            wild_pos++;
          break;
        }
        chars= 128;                             // Marker that chars existed
      }
    }
    sort= (sort << 8) + (wild_pos ? min(wild_pos, 127U) : chars);
  }
  va_end(args);
  return sort;
}


/**
  Check if the given host name needs to be resolved or not.
  Host name has to be resolved if it actually contains *name*.

  For example:
    192.168.1.1               --> FALSE
    192.168.1.0/255.255.255.0 --> FALSE
    %                         --> FALSE
    192.168.1.%               --> FALSE
    AB%                       --> FALSE

    AAAAFFFF                  --> TRUE (Hostname)
    AAAA:FFFF:1234:5678       --> FALSE
    ::1                       --> FALSE

  This function does not check if the given string is a valid host name or
  not. It assumes that the argument is a valid host name.

  @param hostname   the string to check.

  @return a flag telling if the argument needs to be resolved or not.
  @retval TRUE the argument is a host name and needs to be resolved.
  @retval FALSE the argument is either an IP address, or a patter and
          should not be resolved.
*/

bool hostname_requires_resolving(const char *hostname)
{

  /* called only for --skip-name-resolve */
  DBUG_ASSERT(specialflag & SPECIAL_NO_RESOLVE);

  if (!hostname)
    return FALSE;

  /*
    If the string contains any of {':', '%', '_', '/'}, it is definitely
    not a host name:
      - ':' means that the string is an IPv6 address;
      - '%' or '_' means that the string is a pattern;
      - '/' means that the string is an IPv4 network address;
  */

  for (const char *p= hostname; *p; ++p)
  {
    switch (*p) {
      case ':':
      case '%':
      case '_':
      case '/':
        return FALSE;
    }
  }

  /*
    Now we have to tell a host name (ab.cd, 12.ab) from an IPv4 address
    (12.34.56.78). The assumption is that if the string contains only
    digits and dots, it is an IPv4 address. Otherwise -- a host name.
  */

  for (const char *p= hostname; *p; ++p)
  {
    if (*p != '.' && !my_isdigit(&my_charset_latin1, *p))
      return TRUE; /* a "letter" has been found. */
  }

  return FALSE; /* all characters are either dots or digits. */
}


#ifndef NO_EMBEDDED_ACCESS_CHECKS


static const uchar* get_key_column(const uchar *arg, size_t *length)
{
  const GRANT_COLUMN *buff= pointer_cast<const GRANT_COLUMN*>(arg);
  *length=buff->key_length;
  return (uchar*) buff->column;
}


static const uchar* get_grant_table(const uchar *arg, size_t *length)
{
  const GRANT_NAME *buff= pointer_cast<const GRANT_NAME*>(arg);
  *length=buff->key_length;
  return (uchar*) buff->hash_key;
}


GRANT_COLUMN::GRANT_COLUMN(String &c,  ulong y) :rights (y)
{
  column= (char*) memdup_root(&memex,c.ptr(), key_length=c.length());
}


void GRANT_NAME::set_user_details(const char *h, const char *d,
                                  const char *u, const char *t,
                                  bool is_routine)
{
  /* Host given by user */
  host.update_hostname(strdup_root(&memex, h));
  if (db != d)
  {
    db= strdup_root(&memex, d);
    if (lower_case_table_names)
      my_casedn_str(files_charset_info, db);
  }
  user = strdup_root(&memex,u);
  sort=  get_sort(3,host.get_host(),db,user);
  if (tname != t)
  {
    tname= strdup_root(&memex, t);
    if (lower_case_table_names || is_routine)
      my_casedn_str(files_charset_info, tname);
  }
  key_length= strlen(d) + strlen(u)+ strlen(t)+3;
  hash_key=   (char*) alloc_root(&memex,key_length);
  my_stpcpy(my_stpcpy(my_stpcpy(hash_key,user)+1,db)+1,tname);
}

GRANT_NAME::GRANT_NAME(const char *h, const char *d,const char *u,
                       const char *t, ulong p, bool is_routine)
  :db(0), tname(0), privs(p)
{
  set_user_details(h, d, u, t, is_routine);
}

GRANT_TABLE::GRANT_TABLE(const char *h, const char *d,const char *u,
                         const char *t, ulong p, ulong c)
  :GRANT_NAME(h,d,u,t,p, FALSE), cols(c)
{
  (void) my_hash_init(&hash_columns,system_charset_info,
                      0, 0, get_key_column, nullptr, 0,
                      key_memory_acl_memex);
}


GRANT_NAME::GRANT_NAME(TABLE *form, bool is_routine)
{
  host.update_hostname(get_field(&memex, form->field[0]));
  db=    get_field(&memex,form->field[1]);
  user=  get_field(&memex,form->field[2]);
  if (!user)
    user= (char*) "";
  sort=  get_sort(3, host.get_host(), db, user);
  tname= get_field(&memex,form->field[3]);
  if (!db || !tname) {
    /* Wrong table row; Ignore it */
    privs= 0;
    return;                                     /* purecov: inspected */
  }
  if (lower_case_table_names)
  {
    my_casedn_str(files_charset_info, db);
  }
  if (lower_case_table_names || is_routine)
  {
    my_casedn_str(files_charset_info, tname);
  }
  key_length= (strlen(db) + strlen(user) + strlen(tname) + 3);
  hash_key=   (char*) alloc_root(&memex, key_length);
  my_stpcpy(my_stpcpy(my_stpcpy(hash_key,user)+1,db)+1,tname);
  privs = (ulong) form->field[6]->val_int();
  privs = fix_rights_for_table(privs);
}


GRANT_TABLE::GRANT_TABLE(TABLE *form)
  :GRANT_NAME(form, false)
{
  if (!db || !tname)
  {
    /* Wrong table row; Ignore it */
    cols= 0;
    return;
  }
  cols= (ulong) form->field[7]->val_int();
  cols =  fix_rights_for_column(cols);

  (void) my_hash_init(&hash_columns,system_charset_info,
                      0, 0, get_key_column, nullptr, 0,
                      key_memory_acl_memex);
}


GRANT_TABLE::~GRANT_TABLE()
{
  my_hash_free(&hash_columns);
}


bool GRANT_TABLE::init(TABLE *col_privs)
{
  int error;

  if (cols)
  {
    uchar key[MAX_KEY_LENGTH];
    uint key_prefix_len;

    if (!col_privs->key_info)
    {
      my_error(ER_TABLE_CORRUPT, MYF(0), col_privs->s->db.str,
               col_privs->s->table_name.str);
      return true;
    }

    KEY_PART_INFO *key_part= col_privs->key_info->key_part;
    col_privs->field[0]->store(host.get_host(),
                               host.get_host() ? host.get_host_len() : 0,
                               system_charset_info);
    col_privs->field[1]->store(db, strlen(db), system_charset_info);
    col_privs->field[2]->store(user, strlen(user), system_charset_info);
    col_privs->field[3]->store(tname, strlen(tname), system_charset_info);

    key_prefix_len= (key_part[0].store_length +
                     key_part[1].store_length +
                     key_part[2].store_length +
                     key_part[3].store_length);
    key_copy(key, col_privs->record[0], col_privs->key_info, key_prefix_len);
    col_privs->field[4]->store("", 0, &my_charset_latin1);

    error= col_privs->file->ha_index_init(0, 1);
    DBUG_EXECUTE_IF("wl7158_grant_table_1",
                    col_privs->file->ha_index_end();
                    error= HA_ERR_LOCK_DEADLOCK;);
    if (error)
    {
      acl_print_ha_error(error);
      return true;
    }

    error=
      col_privs->file->ha_index_read_map(col_privs->record[0], (uchar*) key,
                                         (key_part_map)15, HA_READ_KEY_EXACT);
    DBUG_ASSERT(col_privs->file->ht->db_type == DB_TYPE_NDBCLUSTER ||
                error != HA_ERR_LOCK_DEADLOCK);
    DBUG_ASSERT(col_privs->file->ht->db_type == DB_TYPE_NDBCLUSTER ||
                error != HA_ERR_LOCK_WAIT_TIMEOUT);
    DBUG_EXECUTE_IF("wl7158_grant_table_2",
                    error= HA_ERR_LOCK_DEADLOCK;);
    if (error)
    {
      bool ret= false;
      cols= 0;
      if (error != HA_ERR_KEY_NOT_FOUND && error != HA_ERR_END_OF_FILE)
      {
        acl_print_ha_error(error);
        ret= true;
      }
      col_privs->file->ha_index_end();
      return ret;
    }

    do
    {
      String *res,column_name;
      GRANT_COLUMN *mem_check;
      /* As column name is a string, we don't have to supply a buffer */
      res= col_privs->field[4]->val_str(&column_name);
      ulong priv= (ulong) col_privs->field[6]->val_int();
      DBUG_EXECUTE_IF("mysql_grant_table_init_out_of_memory",
                      DBUG_SET("+d,simulate_out_of_memory"););
      if (!(mem_check= new GRANT_COLUMN(*res,
                                        fix_rights_for_column(priv))) ||
            my_hash_insert(&hash_columns, (uchar *) mem_check))
      {
        /* Don't use this entry */
        col_privs->file->ha_index_end();

        DBUG_EXECUTE_IF("mysql_grant_table_init_out_of_memory",
                        DBUG_SET("-d,simulate_out_of_memory"););
        return true;
      }

      error= col_privs->file->ha_index_next(col_privs->record[0]);
      DBUG_ASSERT(col_privs->file->ht->db_type == DB_TYPE_NDBCLUSTER ||
                  error != HA_ERR_LOCK_DEADLOCK);
      DBUG_ASSERT(col_privs->file->ht->db_type == DB_TYPE_NDBCLUSTER ||
                  error != HA_ERR_LOCK_WAIT_TIMEOUT);
      DBUG_EXECUTE_IF("wl7158_grant_table_3",
                      error= HA_ERR_LOCK_DEADLOCK;);
      if (error && error != HA_ERR_END_OF_FILE)
      {
        acl_print_ha_error(error);
        col_privs->file->ha_index_end();
        return true;
      }
    }
    while (!error && !key_cmp_if_same(col_privs,key,0,key_prefix_len));
    col_privs->file->ha_index_end();
  }

  return false;
}

/*
  Find first entry that matches the current user
*/

ACL_USER *
find_acl_user(const char *host, const char *user, my_bool exact)
{
  DBUG_ENTER("find_acl_user");
  DBUG_PRINT("enter",("host: '%s'  user: '%s'",host,user));

  DBUG_ASSERT(assert_acl_cache_read_lock(current_thd));

  if (likely(acl_users))
  {
    for (ACL_USER *acl_user= acl_users->begin();
         acl_user != acl_users->end(); ++acl_user)
    {
      DBUG_PRINT("info",("strcmp('%s','%s'), compare_hostname('%s','%s'),",
                         user, acl_user->user ? acl_user->user : "",
                         host,
                         acl_user->host.get_host() ? acl_user->host.get_host() :
                         ""));
      if ((!acl_user->user && !user[0]) ||
          (acl_user->user && !strcmp(user,acl_user->user)))
      {
        if (exact ? !my_strcasecmp(system_charset_info, host,
                                   acl_user->host.get_host() ?
                                   acl_user->host.get_host() : "") :
            acl_user->host.compare_hostname(host,host))
        {
          DBUG_RETURN(acl_user);
        }
      }
    }
  }
  DBUG_RETURN(0);
}


/*
  Find user in ACL

  SYNOPSIS
    is_acl_user()
    thd                  Handle to THD
    host                 host name
    user                 user name

  RETURN
   FALSE  user not fond
   TRUE   there are such user
*/

bool is_acl_user(THD *thd, const char *host, const char *user)
{
  bool res= true;

  /* --skip-grants */
  if (!initialized)
    return res;

  Acl_cache_lock_guard acl_cache_lock(thd, Acl_cache_lock_mode::READ_MODE);
  if (!acl_cache_lock.lock(false))
    return res;

  res= find_acl_user(host, user, TRUE) != NULL;
  return res;
}


/**
  Validate if a user can proxy as another user

  @param user              the logged in user (proxy user)
  @param host              the hostname part of the logged in userid
  @param ip                the ip of the logged in userid
  @param authenticated_as  the effective user a plugin is trying to
                           impersonate as (proxied user)
  @param [out] proxy_used  True if a proxy is found
  @return                  proxy user definition
    @retval NULL           proxy user definition not found or not applicable
    @retval non-null       the proxy user data
*/

ACL_PROXY_USER *
acl_find_proxy_user(const char *user, const char *host, const char *ip,
                    char *authenticated_as, bool *proxy_used)
{
  /* if the proxied and proxy user are the same return OK */
  DBUG_ENTER("acl_find_proxy_user");
  DBUG_PRINT("info", ("user=%s host=%s ip=%s authenticated_as=%s",
                      user, host, ip, authenticated_as));

  if (!strcmp(authenticated_as, user))
  {
    DBUG_PRINT ("info", ("user is the same as authenticated_as"));
    DBUG_RETURN (NULL);
  }

  bool find_any = check_proxy_users && !*authenticated_as;

  if(!find_any)
    *proxy_used= TRUE;
  for (ACL_PROXY_USER *proxy= acl_proxy_users->begin();
       proxy != acl_proxy_users->end(); ++proxy)
  {
	if (proxy->matches(host, user, ip, authenticated_as, find_any))
	{
      DBUG_PRINT("info", ("proxy matched=%s@%s",
		proxy->get_proxied_user(),
		proxy->get_proxied_host()));
      if (!find_any)
	  {
        DBUG_PRINT("info", ("returning specific match as authenticated_as was specified"));
        *proxy_used = TRUE;
        DBUG_RETURN(proxy);
      }
      else
      {
        // we never use anonymous users when mapping
        // proxy users for internal plugins:
        if (strcmp(proxy->get_proxied_user() ?
          proxy->get_proxied_user() : "", ""))
        {
          if (find_acl_user(
            proxy->get_proxied_host(),
            proxy->get_proxied_user(),
            TRUE))
          {
            DBUG_PRINT("info", ("setting proxy_used to true, as \
              find_all search matched real user=%s host=%s",
              proxy->get_proxied_user(),
              proxy->get_proxied_host()));
            *proxy_used = TRUE;
            strcpy(authenticated_as, proxy->get_proxied_user());
          }
          else
          {
            DBUG_PRINT("info", ("skipping match because ACL user \
              does not exist, looking for next match to map"));
          }
          if (*proxy_used)
          {
            DBUG_PRINT("info", ("returning matching user"));
            DBUG_RETURN(proxy);
          }
        }
      }
	}
  }
  DBUG_PRINT("info", ("No matching users found, returning null"));
  DBUG_RETURN(NULL);
}


static const uchar* get_grant_db(const uchar *arg, size_t *length)
{
  const acl_entry *entry= pointer_cast<const acl_entry*>(arg);
  *length=(uint) entry->length;
  return (uchar*) entry->key;
}


static const uchar* check_get_key(const uchar *arg, size_t *length)
{
  const ACL_USER *buff= pointer_cast<const ACL_USER*>(arg);
  *length=buff->host.get_host_len();
  return (uchar*) buff->host.get_host();
}


void clear_and_init_db_cache()
{
  my_hash_reset(&db_cache);
}


/**
  Insert a new entry in db_cache

  @param [in] thd    Handle to THD object
  @param [in] entry  Entry to be inserted in db_cache
*/

static void
insert_entry_in_db_cache(THD *thd, acl_entry *entry)
{
  DBUG_ENTER("insert_entry_in_db_cache");
  /* Either have WRITE lock or none at all */
  DBUG_ASSERT(assert_acl_cache_write_lock(thd) ||
              !assert_acl_cache_read_lock(thd));

  Acl_cache_lock_guard acl_cache_lock(thd, Acl_cache_lock_mode::WRITE_MODE);

  /*
    In following cases release memory and return
    1. Could not lock cache : This is ok because db_cache
       second level cache anyways.
    2. my_hash_insert returned error. This is likely because
       someone already inserted a similar entry.
  */
  if (!acl_cache_lock.lock(false) ||
      my_hash_insert(&db_cache, (uchar *)entry))
    my_free(entry);
  DBUG_VOID_RETURN;
}


/**
  Get privilege for a host, user, and db
  combination.

  @note db_cache is not used if db_is_pattern
  is set.

  @param thd   Thread handler
  @param host  Host name
  @param ip    Ip
  @param user  user name
  @param db    We look for the ACL of this database
  @param db_is_pattern

  @return Database ACL
*/

ulong acl_get(THD *thd, const char *host, const char *ip,
              const char *user, const char *db, my_bool db_is_pattern)
{
  ulong host_access= ~(ulong)0, db_access= 0;
  size_t key_length, copy_length;
  char key[ACL_KEY_LENGTH],*tmp_db,*end;
  acl_entry *entry;
  DBUG_ENTER("acl_get");

  copy_length= (strlen(ip ? ip : "") +
                strlen(user ? user : "") +
                strlen(db ? db : "")) + 2; /* Added 2 at the end to avoid
                                              buffer overflow at strmov()*/
  /*
    Make sure that my_stpcpy() operations do not result in buffer overflow.
  */
  if (copy_length >= ACL_KEY_LENGTH)
    DBUG_RETURN(0);
  Acl_cache_lock_guard acl_cache_lock(thd, Acl_cache_lock_mode::READ_MODE);
  if (!acl_cache_lock.lock(false))
    DBUG_RETURN(db_access);

  end=my_stpcpy((tmp_db=my_stpcpy(my_stpcpy(key, ip ? ip : "")+1,user)+1),db);
  if (lower_case_table_names)
  {
    my_casedn_str(files_charset_info, tmp_db);
    db=tmp_db;
  }
  key_length= (size_t) (end-key);
  if (!db_is_pattern && (entry=(acl_entry*) my_hash_search(&db_cache,
                                                           (uchar*) key,
                                                           key_length)))
  {
    db_access=entry->access;
    DBUG_PRINT("exit", ("access: 0x%lx", db_access));
    DBUG_RETURN(db_access);
  }

  /*
    Check if there are some access rights for database and user
  */
  for (ACL_DB *acl_db= acl_dbs->begin(); acl_db != acl_dbs->end(); ++acl_db)
  {
    if (!acl_db->user || !strcmp(user,acl_db->user))
    {
      if (acl_db->host.compare_hostname(host,ip))
      {
        if (!acl_db->db || !wild_compare(db,acl_db->db,db_is_pattern))
        {
          db_access=acl_db->access;
          if (acl_db->host.get_host())
            goto exit;                          // Fully specified. Take it
          break; /* purecov: tested */
        }
      }
    }
  }
  if (!db_access)
    goto exit;                                  // Can't be better

exit:
  /* Save entry in cache for quick retrieval */
  if (!db_is_pattern &&
      (entry= (acl_entry*) my_malloc(key_memory_acl_cache,
                                     sizeof(acl_entry)+key_length,
                                     MYF(0))))
  {
    entry->access=(db_access & host_access);
    entry->length=key_length;
    memcpy((uchar*) entry->key,key,key_length);
    acl_cache_lock.unlock();
    insert_entry_in_db_cache(thd, entry);
  }
  DBUG_PRINT("exit", ("access: 0x%lx", db_access & host_access));
  DBUG_RETURN(db_access & host_access);
}


/*
  Check if there are any possible matching entries for this host

  NOTES
    All host names without wild cards are stored in a hash table,
    entries with wildcards are stored in a dynamic array
*/

static void init_check_host(void)
{
  DBUG_ENTER("init_check_host");
  if (acl_wild_hosts != NULL)
    acl_wild_hosts->clear();
  else
    acl_wild_hosts=
      new Prealloced_array<ACL_HOST_AND_IP, ACL_PREALLOC_SIZE>(key_memory_acl_mem);

  size_t acl_users_size= acl_users ? acl_users->size() : 0;

  (void) my_hash_init(&acl_check_hosts,system_charset_info,
                      acl_users_size, 0,
                      check_get_key, nullptr, 0,
                      key_memory_acl_mem);
  if (acl_users_size && !allow_all_hosts)
  {
    for (ACL_USER *acl_user= acl_users->begin();
         acl_user != acl_users->end(); ++acl_user)
    {
      if (acl_user->host.has_wildcard())
      {                                         // Has wildcard
        ACL_HOST_AND_IP *acl= NULL;
        for (acl= acl_wild_hosts->begin(); acl != acl_wild_hosts->end(); ++acl)
        {                                       // Check if host already exists
          if (!my_strcasecmp(system_charset_info,
                             acl_user->host.get_host(), acl->get_host()))
            break;                              // already stored
        }
        if (acl == acl_wild_hosts->end())       // If new
          acl_wild_hosts->push_back(acl_user->host);
      }
      else if (!my_hash_search(&acl_check_hosts,(uchar*)
                               acl_user->host.get_host(),
                               strlen(acl_user->host.get_host())))
      {
        if (my_hash_insert(&acl_check_hosts,(uchar*) acl_user))
        {                                       // End of memory
          allow_all_hosts=1;                    // Should never happen
          DBUG_VOID_RETURN;
        }
      }
    }
  }
  acl_wild_hosts->shrink_to_fit();
  freeze_size(&acl_check_hosts.array);
  DBUG_VOID_RETURN;
}


/*
  Rebuild lists used for checking of allowed hosts

  We need to rebuild 'acl_check_hosts' and 'acl_wild_hosts' after adding,
  dropping or renaming user, since they contain pointers to elements of
  'acl_user' array, which are invalidated by drop operation, and use
  ACL_USER::host::hostname as a key, which is changed by rename.
*/
void rebuild_check_host(void)
{
  delete acl_wild_hosts;
  acl_wild_hosts= NULL;
  my_hash_free(&acl_check_hosts);
  init_check_host();
}


/*
  Gets user credentials without authentication and resource limit checks. This
  function is used to initialized a new Security_context. It's a terrible
  anti-pattern that needs to go.

  SYNOPSIS
    acl_getroot()
      thd                Handle to THD
      sctx               Context which should be initialized
      user               user name
      host               host name
      ip                 IP
      db                 current data base name

  RETURN
    FALSE  OK
    TRUE   Error
*/

bool acl_getroot(THD *thd, Security_context *sctx, char *user, char *host,
                 char *ip, const char *db)
{
  int res= 1;
  ACL_USER *acl_user= 0;
  DBUG_ENTER("acl_getroot");

  DBUG_PRINT("enter", ("Host: '%s', Ip: '%s', User: '%s', db: '%s'",
                       (host ? host : "(NULL)"), (ip ? ip : "(NULL)"),
                       user, (db ? db : "(NULL)")));
  sctx->set_user_ptr(user, user ? strlen(user) : 0);
  sctx->set_host_ptr(host, host ? strlen(host) : 0);
  sctx->set_ip_ptr(ip, ip? strlen(ip) : 0);
  sctx->set_host_or_ip_ptr();

  if (!initialized)
  {
    /*
      here if mysqld's been started with --skip-grant-tables option.
    */
    sctx->skip_grants();
    DBUG_RETURN(FALSE);
  }

  sctx->set_master_access(0);
  sctx->cache_current_db_access(0);
  sctx->assign_priv_user("", 0);
  sctx->assign_priv_host("", 0);

  Acl_cache_lock_guard acl_cache_lock(thd,
                                      Acl_cache_lock_mode::READ_MODE);
  if (!acl_cache_lock.lock(false))
    DBUG_RETURN(TRUE);

  /*
     Find acl entry in user database.
     This is specially tailored to suit the check we do for CALL of
     a stored procedure; user is set to what is actually a
     priv_user, which can be ''.
  */
  for (ACL_USER *acl_user_tmp= acl_users->begin();
       acl_user_tmp != acl_users->end(); ++acl_user_tmp)
  {
    if ((!acl_user_tmp->user && !user[0]) ||
        (acl_user_tmp->user && strcmp(user, acl_user_tmp->user) == 0))
    {
      if (acl_user_tmp->host.compare_hostname(host, ip))
      {
        acl_user= acl_user_tmp;
        res= 0;
        break;
      }
    }
  }

  if (acl_user)
  {
    sctx->clear_active_roles();
    List_of_auth_id_refs default_roles;
    Auth_id_ref authid= create_authid_from(acl_user);
    /* Needs Acl_cache_lock_guard in read mode */
    get_default_roles(authid, &default_roles);
    List_of_auth_id_refs::iterator it= default_roles.begin();
    for(;it != default_roles.end(); ++it)
    {
      if (sctx->activate_role(it->first, it->second))
      {
        sctx->clear_active_roles();
        break;
      }
    }

    if (sctx->get_active_roles()->size() == 0)
    {
      for (ACL_DB *acl_db= acl_dbs->begin(); acl_db != acl_dbs->end(); ++acl_db)
      {
        if (!acl_db->user ||
            (user && user[0] && !strcmp(user, acl_db->user)))
        {
          if (acl_db->host.compare_hostname(host, ip))
          {
            if (!acl_db->db || (db && !wild_compare(db, acl_db->db, 0)))
            {
              sctx->cache_current_db_access(acl_db->access);
              break;
            }
          }
        } // end if
      } // end for
      sctx->set_master_access(acl_user->access);
    } // end if
    sctx->assign_priv_user(user, user ? strlen(user) : 0);
    sctx->assign_priv_host(acl_user->host.get_host(),
                           acl_user->host.get_host() ?
                           strlen(acl_user->host.get_host()) : 0);

    sctx->set_password_expired(acl_user->password_expired);
  } // end if
 
  if (acl_user && sctx->get_active_roles()->size() > 0)
  {
    sctx->checkout_access_maps();
    ulong db_acl= sctx->db_acl({db, strlen(db)});
    sctx->cache_current_db_access(db_acl);
  }
  DBUG_RETURN(res);
}


namespace {

class ACL_compare :
  public std::binary_function<ACL_ACCESS, ACL_ACCESS, bool>
{
public:
  bool operator()(const ACL_ACCESS &a, const ACL_ACCESS &b)
  {
    return a.sort > b.sort;
  }
};

} // namespace


/**
  Convert scrambled password to binary form, according to scramble type,
  Binary form is stored in user.salt.

  @param acl_user The object where to store the salt

  Despite the name of the function it is used when loading ACLs from disk
  to store the password hash in the ACL_USER object.
  Note that it works only for native and "old" mysql authentication built-in
  plugins.

  Assumption : user's authentication plugin information is available.

  @return Password hash validation
    @retval false Hash is of suitable length
    @retval true Hash is of wrong length or format
*/

static bool set_user_salt(ACL_USER *acl_user)
{
  bool result= false;
  plugin_ref plugin= NULL;

  plugin= my_plugin_lock_by_name(0, acl_user->plugin,
                                 MYSQL_AUTHENTICATION_PLUGIN);
  if (plugin)
  {
    st_mysql_auth *auth= (st_mysql_auth *) plugin_decl(plugin)->info;
    result=  auth->set_salt(acl_user->auth_string.str,
                            acl_user->auth_string.length,
                            acl_user->salt,
                            &acl_user->salt_len);
    plugin_unlock(0, plugin);
  }
  return result;
}

/**
  Iterate over the user records and check for irregularities.
  Currently this includes :
   - checking if the plugin referenced is present.
   - if there's sha256 users and there's neither SSL nor RSA configured
*/
static void
validate_user_plugin_records()
{
  DBUG_ENTER("validate_user_plugin_records");
  if (!validate_user_plugins)
    DBUG_VOID_RETURN;

  lock_plugin_data();
  for (ACL_USER *acl_user= acl_users->begin();
       acl_user != acl_users->end(); ++acl_user)
  {
    struct st_plugin_int *plugin;

    if (acl_user->plugin.length)
    {
      /* rule 1 : plugin does exit */
      if (!auth_plugin_is_built_in(acl_user->plugin.str))
      {
        plugin= plugin_find_by_type(acl_user->plugin,
                                    MYSQL_AUTHENTICATION_PLUGIN);

        if (!plugin)
        {
          sql_print_warning("The plugin '%.*s' used to authenticate "
                            "user '%s'@'%.*s' is not loaded."
                            " Nobody can currently login using this account.",
                            (int) acl_user->plugin.length, acl_user->plugin.str,
                            acl_user->user,
                            static_cast<int>(acl_user->host.get_host_len()),
                            acl_user->host.get_host());
        }
      }
      if (acl_user->plugin.str == sha256_password_plugin_name.str &&
          rsa_auth_status() && !ssl_acceptor_fd)
      {
          sql_print_warning("The plugin '%s' is used to authenticate "
                            "user '%s'@'%.*s', "
#if !defined(HAVE_YASSL)
                            "but neither SSL nor RSA keys are "
#else
                            "but no SSL is "
#endif
                            "configured. "
                            "Nobody can currently login using this account.",
                            sha256_password_plugin_name.str,
                            acl_user->user,
                            static_cast<int>(acl_user->host.get_host_len()),
                            acl_user->host.get_host());
      }
    }
  }
  unlock_plugin_data();
  DBUG_VOID_RETURN;
}


/**
  Initialize roles structures from tables.

  This function is called by acl_init and may fail to
  initialize role structures if role_edges and/or
  default_roles are not present.

  @param [in] thd Handle to THD
*/

static
void roles_init(THD *thd)
{
  TABLE_LIST tables[2];
  tables[0].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("role_edges"),
                           "role_edges", TL_READ, MDL_SHARED_READ_ONLY);

  tables[1].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("default_roles"),
                           "default_roles", TL_READ, MDL_SHARED_READ_ONLY);

  tables[0].next_local=
    tables[0].next_global= tables + 1;
  tables[1].next_local=
    tables[1].next_global= 0;
  tables[0].open_type=
  tables[1].open_type= OT_BASE_ONLY;

  bool result= open_and_lock_tables(thd, tables, MYSQL_LOCK_IGNORE_TIMEOUT);
  if (!result)
  {
    check_acl_tables(tables, false);
    commit_and_close_mysql_tables(thd);
    result= roles_init_from_tables(thd);
  }

  if (result)
  {
   sql_print_warning("Could not load mysql.role_edges and "
                     "mysql.default_roles tables. ACL DDLs "
                     "will not work unless mysql_upgrade is "
                     "executed.");
  }
}


/*
  Initialize structures responsible for user/db-level privilege checking and
  load privilege information for them from tables in the 'mysql' database.

  SYNOPSIS
    acl_init()
      dont_read_acl_tables  TRUE if we want to skip loading data from
                            privilege tables and disable privilege checking.

  NOTES
    This function is mostly responsible for preparatory steps, main work
    on initialization and grants loading is done in acl_reload().

  RETURN VALUES
    0   ok
    1   Could not initialize grant's
*/

my_bool acl_init(bool dont_read_acl_tables)
{
  THD  *thd;
  my_bool return_val;
  DBUG_ENTER("acl_init");

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  init_acl_cache();

  (void)my_hash_init(&db_cache, &my_charset_utf8_bin,
                     0, 0, get_grant_db, my_free,
                     0, key_memory_acl_cache);
#endif
  acl_cache_initialized= true;

  /*
    cache built-in native authentication plugins,
    to avoid hash searches and a global mutex lock on every connect
  */
  native_password_plugin= my_plugin_lock_by_name(0,
           native_password_plugin_name, MYSQL_AUTHENTICATION_PLUGIN);
  if (!native_password_plugin)
    DBUG_RETURN(1);

  if (dont_read_acl_tables)
  {
    DBUG_RETURN(0); /* purecov: tested */
  }

  /*
    To be able to run this from boot, we allocate a temporary THD
  */
  if (!(thd=new THD))
    DBUG_RETURN(1); /* purecov: inspected */
  thd->thread_stack= (char*) &thd;
  thd->store_globals();

  /*
    Check storage engine type for every ACL table and output warning
    message in case it's different from supported one (InnoDB). We
    still allow server to start-up if tables are in different SE
    since this is necessary to be able to perform privilege table
    upgrade without extra server restarts. Account management
    statements do their own checks and refuse to operate if privilege
    tables are using unsupported SE.
  */
  return_val= check_engine_type_for_acl_table(thd);

  /*
    It is safe to call acl_reload() since acl_* arrays and hashes which
    will be freed there are global static objects and thus are initialized
    by zeros at startup.
  */
  return_val|= acl_reload(thd);
  roles_init(thd);
  thd->release_resources();
  delete thd;

  DBUG_RETURN(return_val);
}


/*
  Initialize structures responsible for user/db-level privilege checking
  and load information about grants from open privilege tables.

  SYNOPSIS
    acl_load()
      thd     Current thread
      tables  List containing open "mysql.host", "mysql.user",
              "mysql.db" and "mysql.proxies_priv" tables in that order.

  RETURN VALUES
    FALSE  Success
    TRUE   Error
*/

static my_bool acl_load(THD *thd, TABLE_LIST *tables)
{
  TABLE *table;
  READ_RECORD read_record_info;
  my_bool return_val= TRUE;
  bool check_no_resolve= specialflag & SPECIAL_NO_RESOLVE;
  char tmp_name[NAME_LEN+1];
  sql_mode_t old_sql_mode= thd->variables.sql_mode;
  bool password_expired= false;
  bool super_users_with_empty_plugin= false;
  Acl_load_user_table_schema_factory user_table_schema_factory;
  Acl_load_user_table_schema *table_schema = NULL;
  bool is_old_db_layout= false;
  DBUG_ENTER("acl_load");

  DBUG_EXECUTE_IF("wl_9262_set_max_length_hostname",
                    thd->security_context()->assign_priv_host(
                      "oh_my_gosh_this_is_a_long_"
                      "hostname_look_at_it_it_has_60"
                      "_char", 60);
                    thd->security_context()->assign_host(
                      "oh_my_gosh_this_is_a_long_"
                      "hostname_look_at_it_it_has_60"
                      "_char", 60);
                    thd->security_context()->set_host_or_ip_ptr();
                    );

  thd->variables.sql_mode&= ~MODE_PAD_CHAR_TO_FULL_LENGTH;

  grant_version++; /* Privileges updated */

  clear_and_init_db_cache();             // Clear locked hostname cache

  init_sql_alloc(key_memory_acl_mem,
                 &global_acl_memory, ACL_ALLOC_BLOCK_SIZE, 0);
  /*
    Prepare reading from the mysql.user table
  */
  if (init_read_record(&read_record_info, thd, table=tables[0].table,
                       NULL, 1, 1, FALSE))
    goto end;
  table->use_all_columns();
  acl_users->clear();
  /*
   We need to check whether we are working with old database layout. This
   might be the case for instance when we are running mysql_upgrade.
  */
  table_schema= user_table_schema_factory.get_user_table_schema(table);
  is_old_db_layout= user_table_schema_factory.is_old_user_table_schema(table);

  allow_all_hosts=0;
  int read_rec_errcode;
  while (!(read_rec_errcode= read_record_info.read_record(&read_record_info)))
  {
    password_expired= false;
    /* Reading record from mysql.user */
    ACL_USER user;
    memset(&user, 0, sizeof(user));

    /*
      All accounts can authenticate per default. This will change when
      we add a new field to the user table.

      Currently this flag is only set to false when authentication is attempted
      using an unknown user name.
    */
    user.can_authenticate= true;

    /*
      Account is unlocked by default.
    */
    user.account_locked= false;

    /*
      The authorization id isn't a part of the role-graph per default.
      This is true even if CREATE ROLE is used.
    */
    user.is_role= false;

    user.host.update_hostname(get_field(&global_acl_memory,
                                      table->field[table_schema->host_idx()]));
    user.user= get_field(&global_acl_memory,
                         table->field[table_schema->user_idx()]);
    if (check_no_resolve && hostname_requires_resolving(user.host.get_host()))
    {
      sql_print_warning("'user' entry '%s@%s' "
                        "ignored in --skip-name-resolve mode.",
                        user.user ? user.user : "",
                        user.host.get_host() ? user.host.get_host() : "");
    }

    /* Read password from authentication_string field */
    if (table->s->fields > table_schema->authentication_string_idx())
      user.auth_string.str=
        get_field(&global_acl_memory,
                  table->field[table_schema->authentication_string_idx()]);
    else
    {
      sql_print_error("Fatal error: mysql.user table is damaged. "
                      "Please run mysql_upgrade.");

      end_read_record(&read_record_info);
      goto end;
    }
    if(user.auth_string.str)
      user.auth_string.length= strlen(user.auth_string.str);
    else
      user.auth_string= EMPTY_STR;

    {
      uint next_field;
      user.access= get_access(table, table_schema->select_priv_idx(),
                              &next_field) & GLOBAL_ACLS;
      /*
        if it is pre 5.0.1 privilege table then map CREATE privilege on
        CREATE VIEW & SHOW VIEW privileges
      */
      if (table->s->fields <= 31 && (user.access & CREATE_ACL))
        user.access|= (CREATE_VIEW_ACL | SHOW_VIEW_ACL);

      /*
        if it is pre 5.0.2 privilege table then map CREATE/ALTER privilege on
        CREATE PROCEDURE & ALTER PROCEDURE privileges
      */
      if (table->s->fields <= 33 && (user.access & CREATE_ACL))
        user.access|= CREATE_PROC_ACL;
      if (table->s->fields <= 33 && (user.access & ALTER_ACL))
        user.access|= ALTER_PROC_ACL;

      /*
        pre 5.0.3 did not have CREATE_USER_ACL
      */
      if (table->s->fields <= 36 && (user.access & GRANT_ACL))
        user.access|= CREATE_USER_ACL;


      /*
        if it is pre 5.1.6 privilege table then map CREATE privilege on
        CREATE|ALTER|DROP|EXECUTE EVENT
      */
      if (table->s->fields <= 37 && (user.access & SUPER_ACL))
        user.access|= EVENT_ACL;

      /*
        if it is pre 5.1.6 privilege then map TRIGGER privilege on CREATE.
      */
      if (table->s->fields <= 38 && (user.access & SUPER_ACL))
        user.access|= TRIGGER_ACL;

      user.sort= get_sort(2,user.host.get_host(),user.user);

      /* Starting from 4.0.2 we have more fields */
      if (table->s->fields >= 31)
      {
        char *ssl_type=
          get_field(thd->mem_root, table->field[table_schema->ssl_type_idx()]);
        if (!ssl_type)
          user.ssl_type=SSL_TYPE_NONE;
        else if (!strcmp(ssl_type, "ANY"))
          user.ssl_type=SSL_TYPE_ANY;
        else if (!strcmp(ssl_type, "X509"))
          user.ssl_type=SSL_TYPE_X509;
        else  /* !strcmp(ssl_type, "SPECIFIED") */
          user.ssl_type=SSL_TYPE_SPECIFIED;

        user.ssl_cipher=
          get_field(&global_acl_memory,
                    table->field[table_schema->ssl_cipher_idx()]);
        user.x509_issuer=
          get_field(&global_acl_memory,
                    table->field[table_schema->x509_issuer_idx()]);
        user.x509_subject=
          get_field(&global_acl_memory,
                    table->field[table_schema->x509_subject_idx()]);

        char *ptr= get_field(thd->mem_root,
                             table->field[table_schema->max_questions_idx()]);
        user.user_resource.questions=ptr ? atoi(ptr) : 0;
        ptr= get_field(thd->mem_root,
                       table->field[table_schema->max_updates_idx()]);
        user.user_resource.updates=ptr ? atoi(ptr) : 0;
        ptr= get_field(thd->mem_root,
                       table->field[table_schema->max_connections_idx()]);
        user.user_resource.conn_per_hour= ptr ? atoi(ptr) : 0;
        if (user.user_resource.questions || user.user_resource.updates ||
            user.user_resource.conn_per_hour)
          mqh_used=1;

        if (table->s->fields > table_schema->max_user_connections_idx())
        {
          /* Starting from 5.0.3 we have max_user_connections field */
          ptr= get_field(thd->mem_root,
                         table->field[table_schema->max_user_connections_idx()]);
          user.user_resource.user_conn= ptr ? atoi(ptr) : 0;
        }

        if (table->s->fields >= 41)
        {
          /* We may have plugin & auth_String fields */
          const char *tmpstr=
            get_field(&global_acl_memory,
                      table->field[table_schema->plugin_idx()]);

          /* In case we are working with 5.6 db layout we need to make server
             aware of Password field and that the plugin column can be null.
             In case when plugin column is null we use native password plugin
             if we can.
          */
          if (is_old_db_layout && (tmpstr == NULL || strlen(tmpstr) == 0 ||
              my_strcasecmp(system_charset_info, tmpstr,
                            native_password_plugin_name.str) == 0))
          {
            char *password= get_field(&global_acl_memory,
                                    table->field[table_schema->password_idx()]);

            //We only support native hash, we do not support pre 4.1 hashes
            plugin_ref native_plugin= NULL;
            native_plugin= my_plugin_lock_by_name(0,
                                       native_password_plugin_name,
                                       MYSQL_AUTHENTICATION_PLUGIN);
            if (native_plugin)
            {
              uint password_len= password ? strlen(password) : 0;
              st_mysql_auth *auth=
                (st_mysql_auth *) plugin_decl(native_plugin)->info;
              if (auth->validate_authentication_string(password,
                                                       password_len) == 0)
              {
                //auth_string takes precedence over password
                if (user.auth_string.length == 0)
                {
                  user.auth_string.str= password;
                  user.auth_string.length= password_len;
                }
                if (tmpstr == NULL || strlen(tmpstr) == 0)
                  tmpstr= native_password_plugin_name.str;
              }
              else
              {
                if ((user.access & SUPER_ACL) && !super_users_with_empty_plugin
                    && (tmpstr == NULL || strlen(tmpstr) == 0))
                  super_users_with_empty_plugin= true;

                sql_print_warning("User entry '%s'@'%s' has a deprecated "
                "pre-4.1 password. The user will be ignored and no one can "
                "login with this user anymore.",
                user.user ? user.user : "",
                user.host.get_host() ? user.host.get_host() : "");
                plugin_unlock(0, native_plugin);
                continue;
              }
              plugin_unlock(0, native_plugin);
            }
          }

          /*
            Check if the plugin string is blank or null.
            If it is, the user will be skipped.
          */
          if(tmpstr == NULL || strlen(tmpstr) == 0)
          {
            if ((user.access & SUPER_ACL) && !super_users_with_empty_plugin)
                      super_users_with_empty_plugin= true;
            sql_print_warning("User entry '%s'@'%s' has an empty plugin "
      			"value. The user will be ignored and no one can login "
      			"with this user anymore.",
      			user.user ? user.user : "",
      			user.host.get_host() ? user.host.get_host() : "");
            continue;
          }
          /*
            By comparing the plugin with the built in plugins it is possible
            to optimize the string allocation and comparision.
          */
          if (my_strcasecmp(system_charset_info, tmpstr,
                            native_password_plugin_name.str) == 0)
            user.plugin= native_password_plugin_name;
#if defined(HAVE_OPENSSL)
          else
            if (my_strcasecmp(system_charset_info, tmpstr,
                              sha256_password_plugin_name.str) == 0)
              user.plugin= sha256_password_plugin_name;
#endif
          else
            {
              user.plugin.str= tmpstr;
              user.plugin.length= strlen(tmpstr);
            }
        }

        /* Validate the hash string. */
        plugin_ref plugin= NULL;
        plugin= my_plugin_lock_by_name(0, user.plugin,
                                       MYSQL_AUTHENTICATION_PLUGIN);
        if (plugin)
        {
          st_mysql_auth *auth= (st_mysql_auth *) plugin_decl(plugin)->info;
          if (auth->validate_authentication_string(user.auth_string.str,
                                                   user.auth_string.length))
          {
            sql_print_warning("Found invalid password for user: '%s@%s'; "
                              "Ignoring user", user.user ? user.user : "",
                              user.host.get_host() ? user.host.get_host() : "");
            plugin_unlock(0, plugin);
            continue;
          }
          plugin_unlock(0, plugin);
        }

        if (table->s->fields > table_schema->password_expired_idx())
        {
          char *tmpstr= get_field(&global_acl_memory,
                           table->field[table_schema->password_expired_idx()]);
          if (tmpstr && (*tmpstr == 'Y' || *tmpstr == 'y'))
          {
            user.password_expired= true;

            if (!auth_plugin_supports_expiration(user.plugin.str))
            {
              sql_print_warning("'user' entry '%s@%s' has the password ignore "
                                "flag raised, but its authentication plugin "
                                "doesn't support password expiration. "
                                "The user id will be ignored.",
                                user.user ? user.user : "",
                                user.host.get_host() ? user.host.get_host() : "");
              continue;
            }
            password_expired= true;
          }
        }

        if (table->s->fields > table_schema->account_locked_idx())
        {
          char *locked = get_field(&global_acl_memory,
                             table->field[table_schema->account_locked_idx()]);

          if (locked && (*locked == 'Y' || *locked == 'y'))
          {
            user.account_locked= true;
          }
        }

        if (table->s->fields > table_schema->drop_role_priv_idx())
        {
          char *priv=
            get_field(&global_acl_memory,
                      table->field[table_schema->create_role_priv_idx()]);

          if (priv && (*priv == 'Y' || *priv == 'y'))
          {
            user.access |= CREATE_ROLE_ACL;
          }
          
          priv= get_field(&global_acl_memory,
                          table->field[table_schema->drop_role_priv_idx()]);

          if (priv && (*priv == 'Y' || *priv == 'y'))
          {
            user.access |= DROP_ROLE_ACL;
          }
        }

	/*
	   Initalize the values of timestamp and expire after day
	   to error and true respectively.
	*/
	user.password_last_changed.time_type= MYSQL_TIMESTAMP_ERROR;
	user.use_default_password_lifetime= true;
	user.password_lifetime= 0;

	if (table->s->fields > table_schema->password_last_changed_idx())
        {
	  if (!table->field[table_schema->password_last_changed_idx()]->is_null())
	  {
            char *password_last_changed= get_field(&global_acl_memory,
	          table->field[table_schema->password_last_changed_idx()]);

	    if (password_last_changed &&
	        memcmp(password_last_changed, INVALID_DATE, sizeof(INVALID_DATE)))
	    {
	      String str(password_last_changed, &my_charset_bin);
              str_to_time_with_warn(&str,&(user.password_last_changed));
	    }
	  }
	}

        if (table->s->fields > table_schema->password_lifetime_idx())
        {
          if (!table->
              field[table_schema->password_lifetime_idx()]->is_null())
	  {
	    char *ptr= get_field(&global_acl_memory,
		table->field[table_schema->password_lifetime_idx()]);
	    user.password_lifetime= ptr ? atoi(ptr) : 0;
	    user.use_default_password_lifetime= false;
	  }
	}

      } // end if (table->s->fields >= 31)
      else
      {
        user.ssl_type=SSL_TYPE_NONE;
        if (table->s->fields <= 13)
        {                                               // Without grant
          if (user.access & CREATE_ACL)
            user.access|=REFERENCES_ACL | INDEX_ACL | ALTER_ACL;
        }
        /* Convert old privileges */
        user.access|= LOCK_TABLES_ACL | CREATE_TMP_ACL | SHOW_DB_ACL;
        if (user.access & FILE_ACL)
          user.access|= REPL_CLIENT_ACL | REPL_SLAVE_ACL;
        if (user.access & PROCESS_ACL)
          user.access|= SUPER_ACL | EXECUTE_ACL;
      }

      set_user_salt(&user);
      user.password_expired= password_expired;

      acl_users->push_back(user);
      if (user.host.check_allow_all_hosts())
        allow_all_hosts=1;                      // Anyone can connect

    }
  } // END while reading records from the mysql.user table

  end_read_record(&read_record_info);
  if (read_rec_errcode > 0)
    goto end;

  std::sort(acl_users->begin(), acl_users->end(), ACL_compare());
  acl_users->shrink_to_fit();

  if (super_users_with_empty_plugin)
  {
    sql_print_warning("Some of the user accounts with SUPER privileges were "
                      "disabled because of empty mysql.user.plugin value. "
                      "If you are upgrading from MySQL 5.6 to MySQL 5.7 it "
                      "means we were not able to substitute for empty plugin "
                      "column. Probably because of pre 4.1 password hash. "
                      "If your account is disabled you will need to:");
    sql_print_warning("1. Stop the server and restart it with "
                      "--skip-grant-tables.");
    sql_print_warning("2. Run mysql_upgrade.");
    sql_print_warning("3. Restart the server with the parameters you "
                      "normally use.");
    sql_print_warning("For complete instructions on how to upgrade MySQL "
                      "to a new version please see the 'Upgrading MySQL' "
                      "section from the MySQL manual");
  }

  /*
    Prepare reading from the mysql.db table
  */
  if (init_read_record(&read_record_info, thd, table=tables[1].table,
                       NULL, 1, 1, FALSE))
    goto end;
  table->use_all_columns();
  acl_dbs->clear();

  while (!(read_rec_errcode= read_record_info.read_record(&read_record_info)))
  {
    /* Reading record in mysql.db */
    ACL_DB db;
    db.host.update_hostname(get_field(&global_acl_memory,
                            table->field[MYSQL_DB_FIELD_HOST]));
    db.db=get_field(&global_acl_memory, table->field[MYSQL_DB_FIELD_DB]);
    if (!db.db)
    {
      sql_print_warning("Found an entry in the 'db' table with empty database name; Skipped");
      continue;
    }
    db.user=get_field(&global_acl_memory, table->field[MYSQL_DB_FIELD_USER]);
    if (check_no_resolve && hostname_requires_resolving(db.host.get_host()))
    {
      sql_print_warning("'db' entry '%s %s@%s' "
                        "ignored in --skip-name-resolve mode.",
                        db.db,
                        db.user ? db.user : "",
                        db.host.get_host() ? db.host.get_host() : "");
    }
    db.access=get_access(table,3,0);
    db.access=fix_rights_for_db(db.access);
    if (lower_case_table_names)
    {
      /*
        convert db to lower case and give a warning if the db wasn't
        already in lower case
      */
      (void)my_stpcpy(tmp_name, db.db);
      my_casedn_str(files_charset_info, db.db);
      if (strcmp(db.db, tmp_name) != 0)
      {
        sql_print_warning("'db' entry '%s %s@%s' had database in mixed "
                          "case that has been forced to lowercase because "
                          "lower_case_table_names is set. It will not be "
                          "possible to remove this privilege using REVOKE.",
                          db.db,
                          db.user ? db.user : "",
                          db.host.get_host() ? db.host.get_host() : "");
      }
    }
    db.sort=get_sort(3,db.host.get_host(),db.db,db.user);
    if (table->s->fields <=  9)
    {                                           // Without grant
      if (db.access & CREATE_ACL)
        db.access|=REFERENCES_ACL | INDEX_ACL | ALTER_ACL;
    }
    acl_dbs->push_back(db);
  } // END reading records from mysql.db tables

  end_read_record(&read_record_info);
  if (read_rec_errcode > 0)
    goto end;

  std::sort(acl_dbs->begin(), acl_dbs->end(), ACL_compare());
  acl_dbs->shrink_to_fit();

  /* Prepare to read records from the mysql.proxies_priv table */
  acl_proxy_users->clear();

  if (tables[2].table)
  {
    if (init_read_record(&read_record_info, thd, table= tables[2].table,
                         NULL, 1, 1, FALSE))
      goto end;
    table->use_all_columns();
    while (!(read_rec_errcode= read_record_info.read_record(&read_record_info)))
    {
      /* Reading record in mysql.proxies_priv */
      ACL_PROXY_USER proxy;
      proxy.init(table, &global_acl_memory);
      if (proxy.check_validity(check_no_resolve))
        continue;
      if (acl_proxy_users->push_back(proxy))
      {
        end_read_record(&read_record_info);
        goto end;
      }
    } // END reading records from the mysql.proxies_priv table

    end_read_record(&read_record_info);
    if (read_rec_errcode > 0)
      goto end;

    std::sort(acl_proxy_users->begin(), acl_proxy_users->end(), ACL_compare());
  }
  else
  {
    sql_print_error("Missing system table mysql.proxies_priv; "
                    "please run mysql_upgrade to create it");
  }
  acl_proxy_users->shrink_to_fit();
  validate_user_plugin_records();
  init_check_host();

  initialized=1;
  return_val= FALSE;

end:
  thd->variables.sql_mode= old_sql_mode;
  if (table_schema)
    delete table_schema;
  DBUG_EXECUTE_IF("induce_acl_load_failure",
                  return_val= TRUE;);
  DBUG_RETURN(return_val);
}




void acl_free(bool end)
{
  free_root(&global_acl_memory,MYF(0));
  delete acl_users;
  acl_users= NULL;
  delete acl_dbs;
  acl_dbs= NULL;
  delete acl_wild_hosts;
  acl_wild_hosts= NULL;
  delete acl_proxy_users;
  acl_proxy_users= NULL;
  my_hash_free(&acl_check_hosts);
  if (!end)
    clear_and_init_db_cache();
  else
  {
    shutdown_acl_cache();
    if (acl_cache_initialized == true)
    {
      my_hash_free(&db_cache);
      plugin_unlock(0, native_password_plugin);
      acl_cache_initialized= false;
    }
  }
}


bool check_engine_type_for_acl_table(THD *thd)
{
  TABLE_LIST tables[6];

  /*
    Open the following ACL tables to check their consistency.
    Although we don't read here from the tables being opened we still
    request a lock type MDL_SHARED_READ_ONLY for the sake of consistency
    with other code.
  */

  tables[ACL_TABLES::TABLE_USER].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("user"),
                           "user", TL_READ, MDL_SHARED_READ_ONLY);

  tables[ACL_TABLES::TABLE_DB].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("db"),
                           "db", TL_READ, MDL_SHARED_READ_ONLY);

  tables[ACL_TABLES::TABLE_TABLES_PRIV].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("proxies_priv"),
                           "proxies_priv", TL_READ, MDL_SHARED_READ_ONLY);

  tables[ACL_TABLES::TABLE_COLUMNS_PRIV].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("tables_priv"),
                           "tables_priv", TL_READ, MDL_SHARED_READ_ONLY);

  tables[ACL_TABLES::TABLE_PROCS_PRIV].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("columns_priv"),
                           "columns_priv", TL_READ, MDL_SHARED_READ_ONLY);

  tables[ACL_TABLES::TABLE_PROXIES_PRIV].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("procs_priv"),
                           "procs_priv", TL_READ, MDL_SHARED_READ_ONLY);

  tables[ACL_TABLES::TABLE_USER].next_local=
    tables[ACL_TABLES::TABLE_USER].next_global= tables + 1;
  tables[ACL_TABLES::TABLE_DB].next_local=
    tables[ACL_TABLES::TABLE_DB].next_global= tables + 2;
  tables[ACL_TABLES::TABLE_TABLES_PRIV].next_local=
    tables[ACL_TABLES::TABLE_TABLES_PRIV].next_global= tables + 3;
  tables[ACL_TABLES::TABLE_COLUMNS_PRIV].next_local=
    tables[ACL_TABLES::TABLE_COLUMNS_PRIV].next_global= tables + 4;
  tables[ACL_TABLES::TABLE_PROCS_PRIV].next_local=
    tables[ACL_TABLES::TABLE_PROCS_PRIV].next_global= tables + 5;
  tables[ACL_TABLES::TABLE_PROXIES_PRIV].next_local=
    tables[ACL_TABLES::TABLE_PROXIES_PRIV].next_global= 0;

  tables[ACL_TABLES::TABLE_USER].open_type=
    tables[ACL_TABLES::TABLE_DB].open_type=
    tables[ACL_TABLES::TABLE_TABLES_PRIV].open_type=
    tables[ACL_TABLES::TABLE_COLUMNS_PRIV].open_type=
    tables[ACL_TABLES::TABLE_PROCS_PRIV].open_type=
    tables[ACL_TABLES::TABLE_PROXIES_PRIV].open_type= OT_BASE_ONLY;

  bool result= open_and_lock_tables(thd, tables, MYSQL_LOCK_IGNORE_TIMEOUT);
  if (!result)
  {
    check_acl_tables(tables, false);
    commit_and_close_mysql_tables(thd);
  }

  return result;
}


/**
  Small helper function which allows to determine if error which caused
  failure to open and lock privilege tables should not be reported to error
  log (because this is expected or temporary condition).
*/

static bool is_expected_or_transient_error(THD *thd)
{
  return ! thd->get_stmt_da()->is_error() || // Interrupted/no error condition.
         thd->get_stmt_da()->mysql_errno() == ER_TABLE_NOT_LOCKED ||
         thd->get_stmt_da()->mysql_errno() == ER_LOCK_DEADLOCK;
}


/*
  Forget current user/db-level privileges and read new privileges
  from the privilege tables.

  SYNOPSIS
    acl_reload()
      thd  Current thread

  NOTE
    All tables of calling thread which were open and locked by LOCK TABLES
    statement will be unlocked and closed.
    This function is also used for initialization of structures responsible
    for user/db-level privilege checking.

  RETURN VALUE
    FALSE  Success
    TRUE   Failure
*/

my_bool acl_reload(THD *thd)
{
  TABLE_LIST tables[3];

  MEM_ROOT old_mem;
  my_bool return_val= TRUE;
  Prealloced_array<ACL_USER, ACL_PREALLOC_SIZE> *old_acl_users= NULL;
  Prealloced_array<ACL_DB, ACL_PREALLOC_SIZE> *old_acl_dbs= NULL;
  Prealloced_array<ACL_PROXY_USER,
    ACL_PREALLOC_SIZE> *old_acl_proxy_users = NULL;
  Acl_cache_lock_guard acl_cache_lock(thd,
                                      Acl_cache_lock_mode::WRITE_MODE);
 
  DBUG_ENTER("acl_reload");

  /*
    To avoid deadlocks we should obtain table locks before
    obtaining acl_cache->lock mutex.
  */
  tables[0].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("user"),
                           "user", TL_READ, MDL_SHARED_READ_ONLY);
  /*
    For a TABLE_LIST element that is inited with a lock type TL_READ
    the type MDL_SHARED_READ_ONLY of MDL is requested for.
    Acquiring strong MDL lock allows to avoid deadlock and timeout errors
    from SE level.
  */
  tables[1].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("db"),
                           "db", TL_READ, MDL_SHARED_READ_ONLY);

  tables[2].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("proxies_priv"),
                           "proxies_priv", TL_READ, MDL_SHARED_READ_ONLY);

  tables[0].next_local= tables[0].next_global= tables + 1;
  tables[1].next_local= tables[1].next_global= tables + 2;

  tables[0].open_type= tables[1].open_type= tables[2].open_type= OT_BASE_ONLY;

  if (open_and_lock_tables(thd, tables, MYSQL_LOCK_IGNORE_TIMEOUT))
  {
    /*
      Execution might have been interrupted; only print the error message
      if a user error condition has been raised. Also do not print expected/
      transient errors about tables not being locked (occurs when user does
      FLUSH PRIVILEGES under LOCK TABLES) and MDL deadlocks. These errors
      can't occurr at start-up and will be reported to user anyway.
    */
    if (! is_expected_or_transient_error(thd))
    {
      sql_print_error("Fatal error: Can't open and lock privilege tables: %s",
                      thd->get_stmt_da()->message_text());
    }
    goto end;
  }

  if (!acl_cache_lock.lock())
    goto end;

  old_acl_users= acl_users;
  old_acl_dbs= acl_dbs;
  old_acl_proxy_users = acl_proxy_users;

  acl_users= new Prealloced_array<ACL_USER,
                                  ACL_PREALLOC_SIZE>(key_memory_acl_mem);
  acl_dbs= new Prealloced_array<ACL_DB,
                                ACL_PREALLOC_SIZE>(key_memory_acl_mem);
  acl_proxy_users=
    new Prealloced_array<ACL_PROXY_USER,
                         ACL_PREALLOC_SIZE>(key_memory_acl_mem);

  // acl_load() overwrites global_acl_memory, so we need to free it.
  // However, we can't do that immediately, because acl_load() might fail,
  // and then we'd need to keep it.
  old_mem= std::move(global_acl_memory);
  delete acl_wild_hosts;
  acl_wild_hosts= NULL;
  my_hash_free(&acl_check_hosts);

  if ((return_val= acl_load(thd, tables)))
  {                                     // Error. Revert to old list
    DBUG_PRINT("error",("Reverting to old privileges"));
    acl_free();                         /* purecov: inspected */
    acl_users= old_acl_users;
    acl_dbs= old_acl_dbs;
    acl_proxy_users= old_acl_proxy_users;
    global_acl_memory= std::move(old_mem);
    init_check_host();
  }
  else
  {
    free_root(&old_mem,MYF(0));
    delete old_acl_users;
    delete old_acl_dbs;
    delete old_acl_proxy_users;
  }

end:
  commit_and_close_mysql_tables(thd);
  get_global_acl_cache()->increase_version();
  DEBUG_SYNC(thd, "after_acl_reload");
  DBUG_RETURN(return_val);
}


void acl_insert_proxy_user(ACL_PROXY_USER *new_value)
{
  DBUG_ENTER("acl_insert_proxy_user");
  DBUG_ASSERT(assert_acl_cache_write_lock(current_thd));
  acl_proxy_users->push_back(*new_value);
  std::sort(acl_proxy_users->begin(), acl_proxy_users->end(), ACL_compare());
  DBUG_VOID_RETURN;
}


static void free_grant_table(void *arg)
{
  GRANT_TABLE *grant_table= pointer_cast<GRANT_TABLE*>(arg);
  my_hash_free(&grant_table->hash_columns);
}


/* Search after a matching grant. Prefer exact grants before not exact ones */

GRANT_NAME *name_hash_search(HASH *name_hash,
                             const char *host,const char* ip,
                             const char *db,
                             const char *user, const char *tname,
                             bool exact, bool name_tolower)
{
  char helping [NAME_LEN*2+USERNAME_LENGTH+3], *name_ptr;
  uint len;
  GRANT_NAME *grant_name,*found=0;
  HASH_SEARCH_STATE state;

  name_ptr= my_stpcpy(my_stpcpy(helping, user) + 1, db) + 1;
  len  = (uint) (my_stpcpy(name_ptr, tname) - helping) + 1;
  if (name_tolower)
    my_casedn_str(files_charset_info, name_ptr);
  for (grant_name= (GRANT_NAME*) my_hash_first(name_hash, (uchar*) helping,
                                               len, &state);
       grant_name ;
       grant_name= (GRANT_NAME*) my_hash_next(name_hash,(uchar*) helping,
                                              len, &state))
  {
    if (exact)
    {
      if (!grant_name->host.get_host() ||
          (host &&
           !my_strcasecmp(system_charset_info, host,
                          grant_name->host.get_host())) ||
          (ip && !strcmp(ip, grant_name->host.get_host())))
        return grant_name;
    }
    else
    {
      if (grant_name->host.compare_hostname(host, ip) &&
          (!found || found->sort < grant_name->sort))
        found=grant_name;                                       // Host ok
    }
  }
  return found;
}


/* Free grant array if possible */

void  grant_free(void)
{
  DBUG_ENTER("grant_free");
  my_hash_free(&column_priv_hash);
  my_hash_free(&proc_priv_hash);
  my_hash_free(&func_priv_hash);
  free_root(&memex,MYF(0));
  DBUG_VOID_RETURN;
}


/**
  @brief Initialize structures responsible for table/column-level privilege
   checking and load information for them from tables in the 'mysql' database.

  @param skip_grant_tables  true if the command line option
    --skip-grant-tables is specified, else false.

  @return Error status
    @retval false OK
    @retval true  Could not initialize grant subsystem.
*/

bool grant_init(bool skip_grant_tables)
{
  THD  *thd;
  my_bool return_val;
  DBUG_ENTER("grant_init");

  if (skip_grant_tables)
    DBUG_RETURN(false);

  if (!(thd= new THD))
    DBUG_RETURN(1);                             /* purecov: deadcode */
  thd->thread_stack= (char*) &thd;
  thd->store_globals();

  return_val=  grant_reload(thd);

  if (return_val && thd->get_stmt_da()->is_error())
    sql_print_error("Fatal: can't initialize grant subsystem - '%s'",
                    thd->get_stmt_da()->message_text());

  thd->release_resources();
  delete thd;

  DBUG_RETURN(return_val);
}


/**
  @brief Helper function to grant_reload_procs_priv

  Reads the procs_priv table into memory hash.

  @param p_table A pointer to the procs_priv table structure.

  @see grant_reload
  @see grant_reload_procs_priv

  @return Error state
    @retval TRUE An error occurred
    @retval FALSE Success
*/

static my_bool grant_load_procs_priv(TABLE *p_table)
{
  MEM_ROOT *memex_ptr;
  my_bool return_val= 1;
  int error;
  bool check_no_resolve= specialflag & SPECIAL_NO_RESOLVE;
  MEM_ROOT **save_mem_root_ptr= my_thread_get_THR_MALLOC();
  DBUG_ENTER("grant_load_procs_priv");
  (void) my_hash_init(&proc_priv_hash, &my_charset_utf8_bin,
                      0, 0, get_grant_table,
                      nullptr, 0, key_memory_acl_memex);
  (void) my_hash_init(&func_priv_hash, &my_charset_utf8_bin,
                      0, 0, get_grant_table,
                      nullptr, 0, key_memory_acl_memex);
  error= p_table->file->ha_index_init(0, 1);
  DBUG_EXECUTE_IF("wl7158_grant_load_proc_1",
                  p_table->file->ha_index_end();
                  error= HA_ERR_LOCK_DEADLOCK;);
  if (error)
  {
    acl_print_ha_error(error);
    DBUG_RETURN(true);
  }
  p_table->use_all_columns();

  error= p_table->file->ha_index_first(p_table->record[0]);
  DBUG_ASSERT(p_table->file->ht->db_type == DB_TYPE_NDBCLUSTER ||
              error != HA_ERR_LOCK_DEADLOCK);
  DBUG_ASSERT(p_table->file->ht->db_type == DB_TYPE_NDBCLUSTER ||
              error != HA_ERR_LOCK_WAIT_TIMEOUT);
  DBUG_EXECUTE_IF("wl7158_grant_load_proc_2",
                  error= HA_ERR_LOCK_DEADLOCK;);

  if (error)
  {
    if (error == HA_ERR_END_OF_FILE)
      return_val= 0; // Return Ok.
    else
      acl_print_ha_error(error);
  }
  else
  {
    memex_ptr= &memex;
    my_thread_set_THR_MALLOC(&memex_ptr);
    do
    {
      GRANT_NAME *mem_check;
      HASH *hash;
      if (!(mem_check=new (memex_ptr) GRANT_NAME(p_table, TRUE)))
      {
        /* This could only happen if we are out memory */
        goto end_unlock;
      }

      if (check_no_resolve)
      {
        if (hostname_requires_resolving(mem_check->host.get_host()))
        {
          sql_print_warning("'procs_priv' entry '%s %s@%s' "
                            "ignored in --skip-name-resolve mode.",
                            mem_check->tname, mem_check->user,
                            mem_check->host.get_host() ?
                            mem_check->host.get_host() : "");
        }
      }
      const enum_sp_type sp_type= to_sp_type(p_table->field[4]->val_int());
      if (sp_type == enum_sp_type::PROCEDURE)
      {
        hash= &proc_priv_hash;
      }
      else if (sp_type == enum_sp_type::FUNCTION)
      {
        hash= &func_priv_hash;
      }
      else
      {
        sql_print_warning("'procs_priv' entry '%s' "
                          "ignored, bad routine type",
                          mem_check->tname);
        continue;
      }

      mem_check->privs= fix_rights_for_procedure(mem_check->privs);
      if (! mem_check->ok())
        delete mem_check;
      else if (my_hash_insert(hash, (uchar*) mem_check))
      {
        delete mem_check;
        goto end_unlock;
      }
      error= p_table->file->ha_index_next(p_table->record[0]);
      DBUG_ASSERT(p_table->file->ht->db_type == DB_TYPE_NDBCLUSTER ||
                  error != HA_ERR_LOCK_DEADLOCK);
      DBUG_ASSERT(p_table->file->ht->db_type == DB_TYPE_NDBCLUSTER ||
                  error != HA_ERR_LOCK_WAIT_TIMEOUT);
      DBUG_EXECUTE_IF("wl7158_grant_load_proc_3",
                      error= HA_ERR_LOCK_DEADLOCK;);
      if (error)
      {
        if (error == HA_ERR_END_OF_FILE)
          return_val= 0;
        else
          acl_print_ha_error(error);
        goto end_unlock;
      }
    }
    while (true);
  }

end_unlock:
  p_table->file->ha_index_end();
  my_thread_set_THR_MALLOC(save_mem_root_ptr);
  DBUG_RETURN(return_val);
}


/**
  @brief Initialize structures responsible for table/column-level privilege
    checking and load information about grants from open privilege tables.

  @param thd Current thread
  @param tables List containing open "mysql.tables_priv" and
    "mysql.columns_priv" tables.

  @see grant_reload

  @return Error state
    @retval FALSE Success
    @retval TRUE Error
*/

static my_bool grant_load(THD *thd, TABLE_LIST *tables)
{
  MEM_ROOT *memex_ptr;
  my_bool return_val= 1;
  int error;
  TABLE *t_table= 0, *c_table= 0;
  bool check_no_resolve= specialflag & SPECIAL_NO_RESOLVE;
  MEM_ROOT **save_mem_root_ptr= my_thread_get_THR_MALLOC();
  sql_mode_t old_sql_mode= thd->variables.sql_mode;
  DBUG_ENTER("grant_load");

  thd->variables.sql_mode&= ~MODE_PAD_CHAR_TO_FULL_LENGTH;

  (void) my_hash_init(&column_priv_hash, &my_charset_utf8_bin,
                      0, 0,
                      get_grant_table,
                      free_grant_table, 0,
                      key_memory_acl_memex);

  t_table = tables[0].table;
  c_table = tables[1].table;
  error= t_table->file->ha_index_init(0, 1);
  DBUG_EXECUTE_IF("wl7158_grant_load_1",
                  t_table->file->ha_index_end();
                  error= HA_ERR_LOCK_DEADLOCK;);
  if (error)
  {
    acl_print_ha_error(error);
    goto end_index_init;
  }
  t_table->use_all_columns();
  c_table->use_all_columns();

  error= t_table->file->ha_index_first(t_table->record[0]);
  DBUG_ASSERT(t_table->file->ht->db_type == DB_TYPE_NDBCLUSTER ||
              error != HA_ERR_LOCK_DEADLOCK);
  DBUG_ASSERT(t_table->file->ht->db_type == DB_TYPE_NDBCLUSTER ||
              error != HA_ERR_LOCK_WAIT_TIMEOUT);
  DBUG_EXECUTE_IF("wl7158_grant_load_2",
                  error= HA_ERR_LOCK_DEADLOCK;);
  if (error)
  {
    if (error == HA_ERR_END_OF_FILE)
      return_val= 0; // Return Ok.
    else
      acl_print_ha_error(error);
  }
  else
  {
    memex_ptr= &memex;
    my_thread_set_THR_MALLOC(&memex_ptr);
    do
    {
      GRANT_TABLE *mem_check;
      mem_check= new (memex_ptr) GRANT_TABLE(t_table);

      if (!mem_check)
      {
        /* This could only happen if we are out memory */
        goto end_unlock;
      }

      if (mem_check->init(c_table))
      {
        delete mem_check;
        goto end_unlock;
      }

      if (check_no_resolve)
      {
        if (hostname_requires_resolving(mem_check->host.get_host()))
        {
          sql_print_warning("'tables_priv' entry '%s %s@%s' "
                            "ignored in --skip-name-resolve mode.",
                            mem_check->tname,
                            mem_check->user ? mem_check->user : "",
                            mem_check->host.get_host() ?
                            mem_check->host.get_host() : "");
        }
      }

      if (! mem_check->ok())
        delete mem_check;
      else if (my_hash_insert(&column_priv_hash,(uchar*) mem_check))
      {
        delete mem_check;
        goto end_unlock;
      }
      error= t_table->file->ha_index_next(t_table->record[0]);
      DBUG_ASSERT(t_table->file->ht->db_type == DB_TYPE_NDBCLUSTER ||
                  error != HA_ERR_LOCK_DEADLOCK);
      DBUG_ASSERT(t_table->file->ht->db_type == DB_TYPE_NDBCLUSTER ||
                  error != HA_ERR_LOCK_WAIT_TIMEOUT);
      DBUG_EXECUTE_IF("wl7158_grant_load_3",
                      error= HA_ERR_LOCK_DEADLOCK;);
      if (error)
      {
        if (error != HA_ERR_END_OF_FILE)
          acl_print_ha_error(error);
        else
          return_val= 0;
        goto end_unlock;
      }

    }
    while (true);
  }

end_unlock:
  t_table->file->ha_index_end();
  my_thread_set_THR_MALLOC(save_mem_root_ptr);
end_index_init:
  thd->variables.sql_mode= old_sql_mode;
  DBUG_RETURN(return_val);
}


/**
  @brief Helper function to grant_reload. Reloads procs_priv table is it
    exists.

  @param thd A pointer to the thread handler object.
  @param table A pointer to the table list.

  @see grant_reload

  @return Error state
    @retval FALSE Success
    @retval TRUE An error has occurred.
*/

static my_bool grant_reload_procs_priv(THD *thd, TABLE_LIST *table)
{
  HASH old_proc_priv_hash, old_func_priv_hash;
  my_bool return_val= FALSE;
  DBUG_ENTER("grant_reload_procs_priv");

  /* Save a copy of the current hash if we need to undo the grant load */
  old_proc_priv_hash= proc_priv_hash;
  old_func_priv_hash= func_priv_hash;

  if ((return_val= grant_load_procs_priv(table->table)))
  {
    /* Error; Reverting to old hash */
    DBUG_PRINT("error",("Reverting to old privileges"));
    my_hash_free(&proc_priv_hash);
    my_hash_free(&func_priv_hash);
    proc_priv_hash= old_proc_priv_hash;
    func_priv_hash= old_func_priv_hash;
  }
  else
  {
    my_hash_free(&old_proc_priv_hash);
    my_hash_free(&old_func_priv_hash);
  }

  DBUG_RETURN(return_val);
}


/**
  @brief Reload information about table and column level privileges if possible

  @param thd Current thread

  Locked tables are checked by acl_reload() and doesn't have to be checked
  in this call.
  This function is also used for initialization of structures responsible
  for table/column-level privilege checking.

  @return Error state
    @retval FALSE Success
    @retval TRUE  Error
*/

my_bool grant_reload(THD *thd)
{
  TABLE_LIST tables[3];
  HASH old_column_priv_hash;
  MEM_ROOT old_mem;
  my_bool return_val= 1;
  Acl_cache_lock_guard acl_cache_lock(thd,
                                      Acl_cache_lock_mode::WRITE_MODE);
  
  DBUG_ENTER("grant_reload");

  /* Don't do anything if running with --skip-grant-tables */
  if (!initialized)
    DBUG_RETURN(0);

  /*
    Acquiring strong MDL lock allows to avoid deadlock and timeout errors
    from SE level.
  */
  tables[0].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("tables_priv"),
                           "tables_priv", TL_READ, MDL_SHARED_READ_ONLY);
  tables[1].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("columns_priv"),
                           "columns_priv", TL_READ, MDL_SHARED_READ_ONLY);
  tables[2].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("procs_priv"),
                           "procs_priv", TL_READ, MDL_SHARED_READ_ONLY);

  tables[0].next_local= tables[0].next_global= tables+1;
  tables[1].next_local= tables[1].next_global= tables+2;
  tables[0].open_type= tables[1].open_type= tables[2].open_type= OT_BASE_ONLY;

  if (open_and_lock_tables(thd, tables, MYSQL_LOCK_IGNORE_TIMEOUT))
  {
    if (! is_expected_or_transient_error(thd))
    {
      sql_print_error("Fatal error: Can't open and lock privilege tables: %s",
                      thd->get_stmt_da()->message_text());
    }
    goto end;
  }

  if (!acl_cache_lock.lock())
    goto end;

  old_column_priv_hash= column_priv_hash;

  /*
    Create a new memory pool but save the current memory pool to make an undo
    opertion possible in case of failure.
  */
  old_mem= std::move(memex);
  init_sql_alloc(key_memory_acl_memex,
                 &memex, ACL_ALLOC_BLOCK_SIZE, 0);
  /*
    tables[2].table i.e. procs_priv can be null if we are working with
    pre 4.1 privilage tables
  */
  if ((return_val= (grant_load(thd, tables) ||
                    grant_reload_procs_priv(thd, &tables[2]))
     ))
  {                                             // Error. Revert to old hash
    DBUG_PRINT("error",("Reverting to old privileges"));
    my_hash_free(&column_priv_hash);
    free_root(&memex,MYF(0));
    column_priv_hash= old_column_priv_hash;     /* purecov: deadcode */
    memex= std::move(old_mem);                  /* purecov: deadcode */
  }
  else
  {                                             //Reload successful
    my_hash_free(&old_column_priv_hash);
    free_root(&old_mem,MYF(0));
    grant_version++;
    get_global_acl_cache()->increase_version();
  }

end:
  commit_and_close_mysql_tables(thd);
  DBUG_RETURN(return_val);
}


void acl_update_user(const char *user, const char *host,
                     enum SSL_type ssl_type,
                     const char *ssl_cipher,
                     const char *x509_issuer,
                     const char *x509_subject,
                     USER_RESOURCES  *mqh,
                     ulong privileges,
                     const LEX_CSTRING &plugin,
                     const LEX_CSTRING &auth,
		     MYSQL_TIME password_change_time,
                     LEX_ALTER password_life,
                     ulong what_is_set)
{
  DBUG_ENTER("acl_update_user");
  DBUG_ASSERT(assert_acl_cache_write_lock(current_thd));
  for (ACL_USER *acl_user= acl_users->begin();
       acl_user != acl_users->end(); ++acl_user)
  {
    if ((!acl_user->user && !user[0]) ||
        (acl_user->user && !strcmp(user,acl_user->user)))
    {
      if ((!acl_user->host.get_host() && !host[0]) ||
          (acl_user->host.get_host() &&
          !my_strcasecmp(system_charset_info, host, acl_user->host.get_host())))
      {
        if (plugin.length > 0)
        {
          acl_user->plugin.str= plugin.str;
          acl_user->plugin.length = plugin.length;
          optimize_plugin_compare_by_pointer(&acl_user->plugin);
          if (!auth_plugin_is_built_in(acl_user->plugin.str))
            acl_user->plugin.str= strmake_root(&global_acl_memory,
                                               plugin.str, plugin.length);
          /* Update auth string only when specified in ALTER/GRANT */
          if (auth.str)
          {
            if (auth.length == 0)
              acl_user->auth_string.str= const_cast<char*>("");
            else
              acl_user->auth_string.str= strmake_root(&global_acl_memory,
                              auth.str, auth.length);
            acl_user->auth_string.length= auth.length;
            set_user_salt(acl_user);
            acl_user->password_last_changed= password_change_time;
          }
        }
        DBUG_PRINT("info",("Updates global privilege for %s@%s to %lu",
           acl_user->user, acl_user->host.get_host(), privileges));
        acl_user->access=privileges;
        if (mqh->specified_limits & USER_RESOURCES::QUERIES_PER_HOUR)
          acl_user->user_resource.questions=mqh->questions;
        if (mqh->specified_limits & USER_RESOURCES::UPDATES_PER_HOUR)
          acl_user->user_resource.updates=mqh->updates;
        if (mqh->specified_limits & USER_RESOURCES::CONNECTIONS_PER_HOUR)
          acl_user->user_resource.conn_per_hour= mqh->conn_per_hour;
        if (mqh->specified_limits & USER_RESOURCES::USER_CONNECTIONS)
          acl_user->user_resource.user_conn= mqh->user_conn;
        if (ssl_type != SSL_TYPE_NOT_SPECIFIED)
        {
          acl_user->ssl_type= ssl_type;
          acl_user->ssl_cipher= (ssl_cipher ? strdup_root(&global_acl_memory,
                                                    ssl_cipher) :        0);
          acl_user->x509_issuer= (x509_issuer ? strdup_root(&global_acl_memory,
                                                      x509_issuer) : 0);
          acl_user->x509_subject= (x509_subject ?
                                   strdup_root(&global_acl_memory, x509_subject) : 0);
        }
        /* update details related to password lifetime, password expiry */
        if (password_life.update_password_expired_column ||
            what_is_set & PLUGIN_ATTR)
          acl_user->password_expired= password_life.update_password_expired_column;
        if (!password_life.update_password_expired_column &&
            password_life.update_password_expired_fields)
        {
          if (!password_life.use_default_password_lifetime)
          {
            acl_user->password_lifetime= password_life.expire_after_days;
            acl_user->use_default_password_lifetime= false;
          }
          else
            acl_user->use_default_password_lifetime= true;
        }

        if (password_life.update_account_locked_column)
        {
          acl_user->account_locked = password_life.account_locked;
        }

        /* Update role graph  */
        std::string authid_role= create_authid_str_from(acl_user);
        Role_index_map:: iterator it= g_authid_to_vertex->find(authid_role);
        if (it !=g_authid_to_vertex->end())
        {
          boost::property_map<Granted_roles_graph,
                            boost::vertex_acl_user_t>::type user_pacl_user;
          user_pacl_user= boost::get(boost::vertex_acl_user_t(),
                                     *g_granted_roles);
          boost::put(user_pacl_user, it->second, *acl_user);
        }
        /* search complete: */
        break;
      }
    }
  }
  DBUG_VOID_RETURN;
}


void acl_insert_user(const char *user, const char *host,
                     enum SSL_type ssl_type,
                     const char *ssl_cipher,
                     const char *x509_issuer,
                     const char *x509_subject,
                     USER_RESOURCES *mqh,
                     ulong privileges,
                     const LEX_CSTRING &plugin,
                     const LEX_CSTRING &auth,
		     MYSQL_TIME password_change_time,
                     LEX_ALTER password_life)
{
  DBUG_ENTER("acl_insert_user");
  ACL_USER acl_user;

  DBUG_ASSERT(assert_acl_cache_write_lock(current_thd));
  /*
     All accounts can authenticate per default. This will change when
     we add a new field to the user table.

     Currently this flag is only set to false when authentication is attempted
     using an unknown user name.
  */
  acl_user.can_authenticate= true;

  acl_user.user= *user ? strdup_root(&global_acl_memory,user) : 0;
  acl_user.host.update_hostname(*host ? strdup_root(&global_acl_memory, host) : 0);
  if (plugin.str[0])
  {
    acl_user.plugin= plugin;
    optimize_plugin_compare_by_pointer(&acl_user.plugin);
    if (!auth_plugin_is_built_in(acl_user.plugin.str))
      acl_user.plugin.str= strmake_root(&global_acl_memory,
                                        plugin.str, plugin.length);
    acl_user.auth_string.str= auth.str ?
      strmake_root(&global_acl_memory, auth.str,
                   auth.length) : const_cast<char*>("");
    acl_user.auth_string.length= auth.length;

    optimize_plugin_compare_by_pointer(&acl_user.plugin);
  }
  else
  {
    acl_user.plugin= native_password_plugin_name;
    acl_user.auth_string.str= const_cast<char*>("");
    acl_user.auth_string.length= 0;
  }

  acl_user.access= privileges;
  acl_user.user_resource= *mqh;
  acl_user.sort= get_sort(2,acl_user.host.get_host(), acl_user.user);
  //acl_user.hostname_length=(uint) strlen(host);
  acl_user.ssl_type=
    (ssl_type != SSL_TYPE_NOT_SPECIFIED ? ssl_type : SSL_TYPE_NONE);
  acl_user.ssl_cipher=
    ssl_cipher ? strdup_root(&global_acl_memory, ssl_cipher) : 0;
  acl_user.x509_issuer=
    x509_issuer ? strdup_root(&global_acl_memory, x509_issuer) : 0;
  acl_user.x509_subject=
    x509_subject ? strdup_root(&global_acl_memory, x509_subject) : 0;
  /* update details related to password lifetime, password expiry */
  acl_user.password_expired= password_life.update_password_expired_column;
  acl_user.password_lifetime= password_life.expire_after_days;
  acl_user.use_default_password_lifetime= password_life.use_default_password_lifetime;
  acl_user.password_last_changed= password_change_time;
  acl_user.account_locked= password_life.account_locked;

  set_user_salt(&acl_user);
  /* New user is not a role by default. */
  acl_user.is_role= false;
  acl_users->push_back(acl_user);
  if (acl_user.host.check_allow_all_hosts())
    allow_all_hosts=1;          // Anyone can connect /* purecov: tested */
  std::sort(acl_users->begin(), acl_users->end(), ACL_compare());

  /* Rebuild 'acl_check_hosts' since 'acl_users' has been modified */
  rebuild_check_host();
  DBUG_VOID_RETURN;
}


void acl_update_proxy_user(ACL_PROXY_USER *new_value, bool is_revoke)
{
  DBUG_ENTER("acl_update_proxy_user");
  DBUG_ASSERT(assert_acl_cache_write_lock(current_thd));
  for (ACL_PROXY_USER *acl_user= acl_proxy_users->begin();
       acl_user != acl_proxy_users->end(); ++acl_user)
  {
    if (acl_user->pk_equals(new_value))
    {
      if (is_revoke)
      {
        DBUG_PRINT("info", ("delting ACL_PROXY_USER"));
        acl_proxy_users->erase(acl_user);
      }
      else
      {
        DBUG_PRINT("info", ("updating ACL_PROXY_USER"));
        acl_user->set_data(new_value);
      }
      break;
    }
  }
  DBUG_VOID_RETURN;
}

void acl_update_db(const char *user, const char *host, const char *db,
                   ulong privileges)
{
  DBUG_ASSERT(assert_acl_cache_write_lock(current_thd));

  for (ACL_DB *acl_db= acl_dbs->begin(); acl_db < acl_dbs->end(); )
  {
    if ((!acl_db->user && !user[0]) ||
        (acl_db->user &&
        !strcmp(user,acl_db->user)))
    {
      if ((!acl_db->host.get_host() && !host[0]) ||
          (acl_db->host.get_host() &&
          !strcmp(host, acl_db->host.get_host())))
      {
        if ((!acl_db->db && !db[0]) ||
            (acl_db->db && !strcmp(db,acl_db->db)))
        {
          if (privileges)
            acl_db->access=privileges;
          else
          {
            acl_db= acl_dbs->erase(acl_db);
            // Don't increment loop variable.
            continue;
          }
        }
      }
    }
    ++acl_db;
  }
}


/*
  Insert a user/db/host combination into the global acl_cache

  SYNOPSIS
    acl_insert_db()
    user                User name
    host                Host name
    db                  Database name
    privileges          Bitmap of privileges

  NOTES
    Acl caches must be locked
*/

void acl_insert_db(const char *user, const char *host, const char *db,
                   ulong privileges)
{
  ACL_DB acl_db;
  DBUG_ASSERT(assert_acl_cache_write_lock(current_thd));
  acl_db.user= strdup_root(&global_acl_memory,user);
  acl_db.host.update_hostname(*host ? strdup_root(&global_acl_memory, host) : 0);
  acl_db.db= strdup_root(&global_acl_memory, db);
  acl_db.access= privileges;
  acl_db.sort= get_sort(3,acl_db.host.get_host(), acl_db.db, acl_db.user);
  acl_dbs->push_back(acl_db);
  std::sort(acl_dbs->begin(), acl_dbs->end(), ACL_compare());
}


void get_mqh(THD *thd, const char *user, const char *host, USER_CONN *uc)
{
  ACL_USER *acl_user;
  Acl_cache_lock_guard acl_cache_lock(thd,
                                      Acl_cache_lock_mode::READ_MODE);

  if (initialized && acl_cache_lock.lock(false) &&
      (acl_user= find_acl_user(host,user, FALSE)))
    uc->user_resources= acl_user->user_resource;
  else
    memset(&uc->user_resources, 0, sizeof(uc->user_resources));
}



/**
  Update the security context when updating the user

  Helper function.
  Update only if the security context is pointing to the same user and
  the user is not a proxied user for a different proxy user.
  And return true if the update happens (i.e. we're operating on the
  user account of the current user).
  Normalize the names for a safe compare.

  @param sctx           The security context to update
  @param acl_user_ptr   User account being updated
  @param expired        new value of the expiration flag
  @return               did the update happen ?
 */
bool
update_sctx_cache(Security_context *sctx, ACL_USER *acl_user_ptr, bool expired)
{
  const char *acl_host= acl_user_ptr->host.get_host();
  const char *acl_user= acl_user_ptr->user;
  const char *sctx_user= sctx->priv_user().str;
  const char *sctx_host= sctx->priv_host().str;

  /* If the user is connected as a proxied user, verify against proxy user */
  if (sctx->proxy_user().str && *sctx->proxy_user().str != '\0')
  {
    sctx_user = sctx->user().str;
  }

  if (!acl_host)
    acl_host= "";
  if(!acl_user)
    acl_user= "";
  if (!sctx_host)
    sctx_host= "";
  if (!sctx_user)
    sctx_user= "";

  if (!strcmp(acl_user, sctx_user) && !strcmp(acl_host, sctx_host))
  {
    sctx->set_password_expired(expired);
    return true;
  }

  return false;
}

struct Acl_hash_entry
{
  uint64 version;
  uchar * key;
  unsigned key_length;
  Acl_map *map;
};

const uchar *hash_key(const uchar *el, size_t *length)
{
  const Acl_hash_entry *entry=
    reinterpret_cast<const Acl_hash_entry *>(el);
  *length= entry->key_length;
  return const_cast<uchar * >(entry->key);
}


/**
  Allocate a new cache key based on active roles, current user and
  global cache version
 
  @param [out] out_key The resulting key
  @param [out] key_len Key length
  @param version Global Acl_cache version
  @param uid The authorization ID of the current user
  @param active_roles The active roles of the current user
 
  @return Success state
    @retval true OK
    @retval false Fatal error occurred.
*/

bool create_acl_cache_hash_key(uchar **out_key, unsigned *key_len,
                               uint64 version,
                               const Auth_id_ref &uid,
                               const List_of_auth_id_refs &active_roles)
{
  List_of_auth_id_refs::const_iterator it= active_roles.begin();
  uint32 active_roles_size= 0;
  for(; it != active_roles.end(); ++it)
  {
    active_roles_size += it->first.length + it->second.length + 2;
  }
  *key_len= uid.first.length + uid.second.length + 2 + sizeof(uint64) +
                  active_roles_size;
  *out_key= (uchar *)my_malloc(key_memory_acl_map_cache, *key_len,
                               MYF(MY_WME));
  if (out_key == NULL)
    return false;
  memcpy(*out_key, uid.first.str, uid.first.length);
  *(*out_key + uid.first.length)= '@';
  memcpy(*out_key + uid.first.length + 1, uid.second.str, uid.second.length);
  uint offset= uid.first.length + uid.second.length + 1;
  /* Separator between version and role */
  *(*out_key + offset)= '`';
  ++offset;
  memcpy(*out_key + offset, reinterpret_cast<void *>(&version), sizeof(uint64));
  it= active_roles.begin();
  offset += sizeof(uint64);
  
  for(; it != active_roles.end(); ++it)
  {
    memcpy(*out_key + offset, it->first.str, it->first.length);
    *(*out_key + offset + it->first.length)= '@';
    memcpy(*out_key + offset + it->first.length + 1, it->second.str,
                       it->second.length);
    offset += it->first.length + it->second.length + 1;
    /* Separator between roles */
    *(*out_key + offset)= '`';
    ++offset;
  }
  DBUG_ASSERT(((offset - *key_len) == 0));
  return true;
}

Acl_cache::Acl_cache() : m_role_graph_version(0L)
{
  const char* category= "sql";
  int count;
  count= array_elements(all_acl_cache_mutexes);
  mysql_mutex_register(category, all_acl_cache_mutexes, count);
  lf_hash_init(&m_cache, sizeof(Acl_hash_entry), LF_HASH_UNIQUE,
               0, /* key offset */
               0, /* key length not used */
               hash_key,
               &my_charset_bin);
  mysql_mutex_init(key_LOCK_acl_cache_flush, &m_cache_flush_mutex,
                   MY_MUTEX_INIT_SLOW);
}

Acl_cache::~Acl_cache()
{
  mysql_mutex_destroy(&m_cache_flush_mutex);
  lf_hash_destroy(&m_cache);
}

Acl_map::Acl_map(Security_context *sctx, uint64 ver)  :
  m_reference_count(0), m_version(ver)
{
  DBUG_ENTER("Acl_map::Acl_map");
  Acl_cache_lock_guard acl_cache_lock(current_thd,
                                      Acl_cache_lock_mode::READ_MODE);
  if (!acl_cache_lock.lock(false))
  {
    DBUG_PRINT("error",("Acl_map could not be constructed for user %s@%s => "
               "Could not lock Acl caches.",
               sctx->priv_user().str,
               sctx->priv_host().str));
    DBUG_VOID_RETURN;
  }
  m_global_acl= 0;
  const ACL_USER *acl_user=
    find_acl_user(sctx->priv_host().str,
                  sctx->priv_user().str,
                  true);
  if (acl_user == 0)
  {
    DBUG_PRINT("error",("Acl_map could not be constructed for user %s@%s => "
                        "No such user",
                        sctx->priv_user().str,
                        sctx->priv_host().str));
    DBUG_VOID_RETURN;
  }
  List_of_granted_roles granted_roles;
  get_privilege_access_maps(const_cast<ACL_USER *> (acl_user),
                            sctx->get_active_roles(),
                            &m_global_acl,
                            &m_db_acls,
                            &m_db_wild_acls,
                            &m_table_acls,
                            &m_sp_acls,
                            &m_func_acls,
                            &granted_roles,
                            &m_with_admin_acls);
  DBUG_VOID_RETURN;
}

Acl_map::~Acl_map()
{
    // Db_access_map is automatically destroyed and cleaned up.
}

Acl_map::Acl_map(const Acl_map &map)
{
  // An Acl_map should not be copied
  DBUG_ASSERT(false);
}

Acl_map::Acl_map(const Acl_map &&map)
{
  operator=(map);
}

Acl_map &Acl_map::operator=(Acl_map &&map)
{
  m_db_acls= std::move(map.m_db_acls);
  m_global_acl= map.m_global_acl;
  m_reference_count= map.m_reference_count;
  m_table_acls= std::move(map.m_table_acls);
  m_sp_acls= std::move(map.m_sp_acls);
  m_func_acls= std::move(map.m_func_acls);
  m_with_admin_acls= std::move(map.m_with_admin_acls);
  m_version= map.m_version;
  map.m_reference_count= 0;
  return *this;
}

Acl_map &
Acl_map::operator=(const Acl_map &map)
{
  return *this;
}

ulong Acl_map::global_acl()
{
  return m_global_acl;
}

Db_access_map *
Acl_map::db_acls()
{
  return &m_db_acls;
}

Db_access_map *
Acl_map::db_wild_acls()
{
  return &m_db_wild_acls;
}

Table_access_map *
Acl_map::table_acls()
{
  return &m_table_acls;
}

Grant_acl_set *
Acl_map::grant_acls()
{
  return &m_with_admin_acls;
}

SP_access_map *
Acl_map::sp_acls()
{
  return &m_sp_acls;
}

SP_access_map *
Acl_map::func_acls()
{
  return &m_func_acls;
}

void
Acl_map::increase_reference_count()
{
  my_atomic_add32(&m_reference_count, 1);
}

void
Acl_map::decrease_reference_count()
{
  my_atomic_add32(&m_reference_count, -1);
}

void Acl_cache::increase_version()
{
  DBUG_ENTER("Acl_cache::increase_version");
  my_atomic_add64((volatile int64 *)&m_role_graph_version, 1);
  flush_cache();
  DBUG_VOID_RETURN;
}

uint64 Acl_cache::version()
{
  uint64 version= (uint64)my_atomic_load64((volatile int64 *)&m_role_graph_version);
  return version;
}

int32 Acl_cache::size()
{
  uint32 size= (uint32)my_atomic_load32((volatile int32 *)&m_cache.count);
  return size;
}

/**
  Finds an Acl_map entry in the Acl_cache and increase its reference count.
  If no Acl_map is located, a new one is created with reference count one.
  The Acl_map is returned to the caller.

  @param sctx The target Security_context
  @param uid The target authid
  @param active_roles A list of active roles

  @return A pointer to an Acl_map
    @retval !NULL Success
    @retval NULL A fatal OOM error happened.
*/

Acl_map *
Acl_cache::checkout_acl_map(Security_context *sctx, Auth_id_ref &uid,
                            List_of_auth_id_refs &active_roles)
{
  DBUG_ENTER("Acl_cache::checkout_acl_map");
  // CREATE KEY
  uchar *key; // allocated by create_hash_key and released by Acl_cache::flush_cache
  unsigned key_len;
  uint64 version= my_atomic_load64((volatile int64 *)&m_role_graph_version);
  if (!create_acl_cache_hash_key(&key, &key_len, version, uid,
                                 active_roles))
  {
    /* OOM happened */
    active_roles.clear();
    return 0;
  }
  LF_PINS *pins= lf_hash_get_pins(&m_cache);
  Acl_hash_entry *entry= (Acl_hash_entry *)lf_hash_search(&m_cache, pins,
                                                          key,
                                                          key_len);
  if (entry == 0 || entry == MY_LF_ERRPTR)
  {
    lf_hash_search_unpin(pins);
    Acl_map *map= create_acl_map(version, sctx); // deleted in cache_flusher
    Acl_hash_entry new_entry;
    new_entry.version= version;
    new_entry.map= map;
    new_entry.key= key;
    new_entry.key_length= key_len;
    int rc= lf_hash_insert(&m_cache, pins, &new_entry); // shallow copy of entry
    if (rc != 0)
    {
      /* There was a duplicate; throw away the allocated memory */
      lf_hash_put_pins(pins);
      my_free(key);
      delete map;
      DBUG_PRINT("info",("Someone else checked out the cache key"));
      /* Potentially dangerous to dive here? */
      DBUG_RETURN(checkout_acl_map(sctx, uid, active_roles));
    }
    map->increase_reference_count();
    lf_hash_put_pins(pins);
    DBUG_PRINT("info",("Checked out new privilege map. Key= %s", key));
    DBUG_RETURN(map);
  }
  Acl_map *map= entry->map;
  map->increase_reference_count();
  lf_hash_search_unpin(pins);
  lf_hash_put_pins(pins);
  my_free(key);
  DBUG_PRINT("info",("Checked out old privilege map. Key= %s", key));
  DBUG_RETURN(map);
}

void Acl_cache::return_acl_map(Acl_map *map)
{
  map->decrease_reference_count();
}

/**
  This global is protected by the Acl_cache::m_cache_flush_mutex and used when
  iterating the Acl_map hash in Acl_cache::flush_cache
  @see Acl_cache::flush_cache
*/
uint64 l_cache_flusher_global_version;

/**
  Utility function for removing all items from the hash.
  @param ptr A pointer to a Acl_hash_entry
  @return Always 0 with the intention that this causes the hash_search function
   to iterate every single element in the hash.
*/
static int cache_flusher(const uchar *ptr)
{
  DBUG_ENTER("cache_flusher");
  const Acl_hash_entry *entry= reinterpret_cast<const Acl_hash_entry *>(ptr);
  if (entry != 0)
  {
    if (entry->map->reference_count() == 0 &&
        entry->map->version() < l_cache_flusher_global_version)
      DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}

void Acl_cache::flush_cache()
{
  DBUG_ENTER("flush_cache");
  LF_PINS *pins= lf_hash_get_pins(&m_cache);
  Acl_hash_entry *entry= 0;
  mysql_mutex_lock(&m_cache_flush_mutex);
  l_cache_flusher_global_version= version();
  do 
  {
    entry=
      static_cast<Acl_hash_entry*>(lf_hash_random_match(&m_cache,
                                                        pins,
                                                        &cache_flusher, 0));
    if (entry && !lf_hash_delete(&m_cache, pins, entry->key, entry->key_length))
    {
      // Hash element is removed from cache; safe to delete
      my_free(entry->key);
      delete entry->map;
    }
    lf_hash_search_unpin(pins);
  } while (entry != 0);
  lf_hash_put_pins(pins);
  mysql_mutex_unlock(&m_cache_flush_mutex);
  DBUG_VOID_RETURN;
}

Acl_map *
Acl_cache::create_acl_map(uint64 version, Security_context *sctx)
{
  Acl_map *map= new Acl_map(sctx, version);
  return map;
}

void* Acl_map::operator new(size_t size)
{
  return my_malloc(key_memory_acl_map_cache, size, MYF(0));
}

void Acl_map::operator delete(void* p)
{
  my_free(p);
}
        
void init_acl_cache()
{
  g_default_roles= new Default_roles;
  roles_init_graph();
  g_acl_cache= new Acl_cache();
}


/**
  Shutdown the global Acl_cache system which was only initialized if the
  rwlocks were initialized.
  @see acl_init()
*/

void shutdown_acl_cache()
{
  if (!acl_cache_initialized)
    return;

  /* This should clean up all remaining Acl_cache items */
  g_acl_cache->increase_version();
  DBUG_ASSERT(g_acl_cache->size() == 0);
  delete g_acl_cache;
  delete g_default_roles;
  g_acl_cache= NULL;
  g_default_roles= NULL;
  roles_delete_graph();
}

#endif /* NO_EMBEDDED_ACCESS_CHECKS */

/* Constants used by Acl_cache_lock_guard */
static const ulong ACL_CACHE_LOCK_TIMEOUT= 3600UL;
static const MDL_key ACL_CACHE_KEY(MDL_key::ACL_CACHE, "", "");

/**
  Internal_error_hanlder subclass to suppress ER_LOCK_DEADLOCK and
  ER_LOCK_WAIT_TIMEOUT.
  Instead, we will use Acl_cache_lock_guard::lock()
  to raise ER_CANNOT_LOCK_USER_MANAGEMENT_CACHES error.
*/
class Acl_cache_error_handler : public Internal_error_handler
{
public:

  /**
    Handle an error condition

    @param [in] thd           THD handle
    @param [in] sql_errno     Error raised by MDL subsystem
    @param [in] sqlstate      SQL state. Unused.
    @param [in] level         Severity level. Unused.
    @param [in] msg           Message string. Unused.
  */

  virtual bool handle_condition(THD *thd,
                                uint sql_errno,
                                const char *sqlstate MY_ATTRIBUTE((unused)),
                                Sql_condition::enum_severity_level *level MY_ATTRIBUTE((unused)),
                                const char *msg MY_ATTRIBUTE((unused)))
  {
    return (sql_errno == ER_LOCK_DEADLOCK ||
            sql_errno == ER_LOCK_WAIT_TIMEOUT);
  }
};


/*
  MDL_release_locks_visitor subclass to release MDL for ACL_CACHE.
*/
class Release_acl_cache_locks : public MDL_release_locks_visitor
{
public:
  /**
    Lock releaser.
    Check details of given key and see it is of type ACL_CACHE
    and if key name it matches with m_partition. If so, release it.

    @param [in] ticket      MDL Ticket returned by MDL subsystem

    @returns Whether ticket matches our criteria or not
      @retval true  Ticket matches
      @retval false Ticket does not match
  */

  virtual bool release(MDL_ticket *ticket)
  {
    return ticket->get_key()->mdl_namespace() == MDL_key::ACL_CACHE;
  }
};


/**
  Acl_cache_lock_guard constructor.

  @param [in] thd             THD Handle.
  @param [in] mode            Lock mode

*/

Acl_cache_lock_guard::Acl_cache_lock_guard(THD *thd,
                                           Acl_cache_lock_mode mode)
  : m_thd(thd),
    m_mode(mode),
    m_locked(false)
{
  DBUG_ASSERT(thd);
};


/**
  Explicitly take lock on Acl_cache_lock_cache object.
  If cache was already locked, just return.

  @param [in] raise_error  Whether to raise error if we fail to acquire lock

  @returns status of lock
    @retval true Lock was acquired/already acquired.
    @retval false There was some problem acquiring lock.
*/

bool
Acl_cache_lock_guard::lock(bool raise_error) /* = true */
{
  DBUG_ASSERT(!m_locked);

  if (already_locked())
    return true;

  /* If we do not have MDL, we should not be holding LOCK_open */
  mysql_mutex_assert_not_owner(&LOCK_open);

  MDL_request lock_request;
  MDL_REQUEST_INIT_BY_KEY(&lock_request, &ACL_CACHE_KEY,
                          m_mode == Acl_cache_lock_mode::READ_MODE ?
                          MDL_SHARED : MDL_EXCLUSIVE,
                          MDL_EXPLICIT);
  Acl_cache_error_handler handler;
  m_thd->push_internal_handler(&handler);
  m_locked= !m_thd->mdl_context.acquire_lock(&lock_request,
                                             ACL_CACHE_LOCK_TIMEOUT);
  m_thd->pop_internal_handler();

  if (!m_locked && raise_error)
    my_error(ER_CANNOT_LOCK_USER_MANAGEMENT_CACHES, MYF(0));
  return m_locked;
}


/**
  Explicitly unlock all acquired locks.
*/

void
Acl_cache_lock_guard::unlock()
{
  /*
    It is possible that we did not take any lock. E.g.
    1. Function 1 : Take shared lock on ACL caches
    2. Function 1 : Take Lock_open
    3. Call Function 2
    4. Function 2 : Try to acquire shared lock on ACL
    caches again

    In such cases, at 4, we do not actually take
    MDL because it will be in conflict with LOCK_open

    If unlock() is called as part of destructor or
    directly in Function 2 above, we should
    not release any locks because we never acquired
    any in the first place.
  */
  if (m_locked)
  {
    Release_acl_cache_locks lock_visitor;
    m_thd->mdl_context.release_locks(&lock_visitor);
    m_locked= false;
  }
}


/**
  Check whether lock is already obtained or not.

  @returns lock status
    @retval true  Lock has already been taken.
    @retval false No lock is taken with this Acl_cache_lock_guard object
*/

bool
Acl_cache_lock_guard::already_locked()
{
  return
    m_thd->mdl_context.owns_equal_or_stronger_lock(MDL_key::ACL_CACHE,
                                                   "", "",
                                                   m_mode == Acl_cache_lock_mode::READ_MODE ?
                                                   MDL_SHARED : MDL_EXCLUSIVE);
}


/**
  Assert that thread owns MDL_SHARED on partition specific to the thread

  @param [in] thd    Thread for which lock is to be checked

  @returns thread owns required lock or not
    @retval true    Thread owns lock
    @retval false   Thread does not own lock
*/

bool assert_acl_cache_read_lock(THD *thd)
{
  return thd->mdl_context.owns_equal_or_stronger_lock(MDL_key::ACL_CACHE,
                                                      "", "",
                                                      MDL_SHARED);
}


/**
  Assert that thread owns MDL_EXCLUSIVE on all partitions

  @param [in] thd    Thread for which lock is to be checked

  @returns thread owns required lock or not
    @retval true    Thread owns lock
    @retval false   Thread does not own lock
*/

bool assert_acl_cache_write_lock(THD *thd)
{
  return thd->mdl_context.owns_equal_or_stronger_lock(MDL_key::ACL_CACHE,
                                                      "", "",
                                                      MDL_EXCLUSIVE);
}
