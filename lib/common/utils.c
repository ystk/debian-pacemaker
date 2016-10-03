/*
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <crm_internal.h>
#include <dlfcn.h>

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include <sys/param.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>
#include <pwd.h>
#include <time.h>
#include <libgen.h>
#include <signal.h>

#include <qb/qbdefs.h>

#include <crm/crm.h>
#include <crm/lrmd.h>
#include <crm/services.h>
#include <crm/msg_xml.h>
#include <crm/cib/internal.h>
#include <crm/common/xml.h>
#include <crm/common/util.h>
#include <crm/common/ipc.h>
#include <crm/common/iso8601.h>
#include <crm/common/mainloop.h>
#include <crm/attrd.h>
#include <libxml2/libxml/relaxng.h>

#ifndef MAXLINE
#  define MAXLINE 512
#endif

#ifdef HAVE_GETOPT_H
#  include <getopt.h>
#endif

#ifndef PW_BUFFER_LEN
#  define PW_BUFFER_LEN		500
#endif

CRM_TRACE_INIT_DATA(common);

gboolean crm_config_error = FALSE;
gboolean crm_config_warning = FALSE;
char *crm_system_name = NULL;

int node_score_red = 0;
int node_score_green = 0;
int node_score_yellow = 0;
int node_score_infinity = INFINITY;

static struct crm_option *crm_long_options = NULL;
static const char *crm_app_description = NULL;
static char *crm_short_options = NULL;
static const char *crm_app_usage = NULL;

int
crm_exit(int rc)
{
    mainloop_cleanup();

#if HAVE_LIBXML2
    crm_trace("cleaning up libxml");
    crm_xml_cleanup();
#endif

    crm_trace("exit %d", rc);
    qb_log_fini();

    free(crm_short_options);
    free(crm_system_name);

    exit(ABS(rc)); /* Always exit with a positive value so that it can be passed to crm_error
                    *
                    * Otherwise the system wraps it around and people
                    * have to jump through hoops figuring out what the
                    * error was
                    */
    return rc;     /* Can never happen, but allows return crm_exit(rc)
                    * where "return rc" was used previously - which
                    * keeps compilers happy.
                    */
}

gboolean
check_time(const char *value)
{
    if (crm_get_msec(value) < 5000) {
        return FALSE;
    }
    return TRUE;
}

gboolean
check_timer(const char *value)
{
    if (crm_get_msec(value) < 0) {
        return FALSE;
    }
    return TRUE;
}

gboolean
check_boolean(const char *value)
{
    int tmp = FALSE;

    if (crm_str_to_boolean(value, &tmp) != 1) {
        return FALSE;
    }
    return TRUE;
}

gboolean
check_number(const char *value)
{
    errno = 0;
    if (value == NULL) {
        return FALSE;

    } else if (safe_str_eq(value, MINUS_INFINITY_S)) {

    } else if (safe_str_eq(value, INFINITY_S)) {

    } else {
        crm_int_helper(value, NULL);
    }

    if (errno != 0) {
        return FALSE;
    }
    return TRUE;
}

gboolean
check_quorum(const char *value)
{
    if (safe_str_eq(value, "stop")) {
        return TRUE;

    } else if (safe_str_eq(value, "freeze")) {
        return TRUE;

    } else if (safe_str_eq(value, "ignore")) {
        return TRUE;

    } else if (safe_str_eq(value, "suicide")) {
        return TRUE;
    }
    return FALSE;
}

gboolean
check_script(const char *value)
{
    struct stat st;

    if(safe_str_eq(value, "/dev/null")) {
        return TRUE;
    }

    if(stat(value, &st) != 0) {
        crm_err("Script %s does not exist", value);
        return FALSE;
    }

    if(S_ISREG(st.st_mode) == 0) {
        crm_err("Script %s is not a regular file", value);
        return FALSE;
    }

    if( (st.st_mode & (S_IXUSR | S_IXGRP )) == 0) {
        crm_err("Script %s is not executable", value);
        return FALSE;
    }

    return TRUE;
}

gboolean
check_utilization(const char *value)
{
    char *end = NULL;
    long number = strtol(value, &end, 10);

    if(end && end[0] != '%') {
        return FALSE;
    } else if(number < 0) {
        return FALSE;
    }

    return TRUE;
}

int
char2score(const char *score)
{
    int score_f = 0;

    if (score == NULL) {

    } else if (safe_str_eq(score, MINUS_INFINITY_S)) {
        score_f = -node_score_infinity;

    } else if (safe_str_eq(score, INFINITY_S)) {
        score_f = node_score_infinity;

    } else if (safe_str_eq(score, "+" INFINITY_S)) {
        score_f = node_score_infinity;

    } else if (safe_str_eq(score, "red")) {
        score_f = node_score_red;

    } else if (safe_str_eq(score, "yellow")) {
        score_f = node_score_yellow;

    } else if (safe_str_eq(score, "green")) {
        score_f = node_score_green;

    } else {
        score_f = crm_parse_int(score, NULL);
        if (score_f > 0 && score_f > node_score_infinity) {
            score_f = node_score_infinity;

        } else if (score_f < 0 && score_f < -node_score_infinity) {
            score_f = -node_score_infinity;
        }
    }

    return score_f;
}

char *
score2char_stack(int score, char *buf, size_t len)
{
    if (score >= node_score_infinity) {
        strncpy(buf, INFINITY_S, 9);
    } else if (score <= -node_score_infinity) {
        strncpy(buf, MINUS_INFINITY_S , 10);
    } else {
        return crm_itoa_stack(score, buf, len);
    }

    return buf;
}

char *
score2char(int score)
{
    if (score >= node_score_infinity) {
        return strdup(INFINITY_S);

    } else if (score <= -node_score_infinity) {
        return strdup("-" INFINITY_S);
    }
    return crm_itoa(score);
}

const char *
cluster_option(GHashTable * options, gboolean(*validate) (const char *),
               const char *name, const char *old_name, const char *def_value)
{
    const char *value = NULL;

    CRM_ASSERT(name != NULL);

    if (options != NULL) {
        value = g_hash_table_lookup(options, name);
    }

    if (value == NULL && old_name && options != NULL) {
        value = g_hash_table_lookup(options, old_name);
        if (value != NULL) {
            crm_config_warn("Using deprecated name '%s' for"
                            " cluster option '%s'", old_name, name);
            g_hash_table_insert(options, strdup(name), strdup(value));
            value = g_hash_table_lookup(options, old_name);
        }
    }

    if (value == NULL) {
        crm_trace("Using default value '%s' for cluster option '%s'", def_value, name);

        if (options == NULL) {
            return def_value;

        } else if(def_value == NULL) {
            return def_value;
        }

        g_hash_table_insert(options, strdup(name), strdup(def_value));
        value = g_hash_table_lookup(options, name);
    }

    if (validate && validate(value) == FALSE) {
        crm_config_err("Value '%s' for cluster option '%s' is invalid."
                       "  Defaulting to %s", value, name, def_value);
        g_hash_table_replace(options, strdup(name), strdup(def_value));
        value = g_hash_table_lookup(options, name);
    }

    return value;
}

const char *
get_cluster_pref(GHashTable * options, pe_cluster_option * option_list, int len, const char *name)
{
    int lpc = 0;
    const char *value = NULL;
    gboolean found = FALSE;

    for (lpc = 0; lpc < len; lpc++) {
        if (safe_str_eq(name, option_list[lpc].name)) {
            found = TRUE;
            value = cluster_option(options,
                                   option_list[lpc].is_valid,
                                   option_list[lpc].name,
                                   option_list[lpc].alt_name, option_list[lpc].default_value);
        }
    }
    CRM_CHECK(found, crm_err("No option named: %s", name));
    return value;
}

void
config_metadata(const char *name, const char *version, const char *desc_short,
                const char *desc_long, pe_cluster_option * option_list, int len)
{
    int lpc = 0;

    fprintf(stdout, "<?xml version=\"1.0\"?>"
            "<!DOCTYPE resource-agent SYSTEM \"ra-api-1.dtd\">\n"
            "<resource-agent name=\"%s\">\n"
            "  <version>%s</version>\n"
            "  <longdesc lang=\"en\">%s</longdesc>\n"
            "  <shortdesc lang=\"en\">%s</shortdesc>\n"
            "  <parameters>\n", name, version, desc_long, desc_short);

    for (lpc = 0; lpc < len; lpc++) {
        if (option_list[lpc].description_long == NULL && option_list[lpc].description_short == NULL) {
            continue;
        }
        fprintf(stdout, "    <parameter name=\"%s\" unique=\"0\">\n"
                "      <shortdesc lang=\"en\">%s</shortdesc>\n"
                "      <content type=\"%s\" default=\"%s\"/>\n"
                "      <longdesc lang=\"en\">%s%s%s</longdesc>\n"
                "    </parameter>\n",
                option_list[lpc].name,
                option_list[lpc].description_short,
                option_list[lpc].type,
                option_list[lpc].default_value,
                option_list[lpc].description_long ? option_list[lpc].
                description_long : option_list[lpc].description_short,
                option_list[lpc].values ? "  Allowed values: " : "",
                option_list[lpc].values ? option_list[lpc].values : "");
    }
    fprintf(stdout, "  </parameters>\n</resource-agent>\n");
}

void
verify_all_options(GHashTable * options, pe_cluster_option * option_list, int len)
{
    int lpc = 0;

    for (lpc = 0; lpc < len; lpc++) {
        cluster_option(options,
                       option_list[lpc].is_valid,
                       option_list[lpc].name,
                       option_list[lpc].alt_name, option_list[lpc].default_value);
    }
}

char *
crm_concat(const char *prefix, const char *suffix, char join)
{
    int len = 0;
    char *new_str = NULL;

    CRM_ASSERT(prefix != NULL);
    CRM_ASSERT(suffix != NULL);
    len = strlen(prefix) + strlen(suffix) + 2;

    new_str = malloc(len);
    if(new_str) {
        sprintf(new_str, "%s%c%s", prefix, join, suffix);
        new_str[len - 1] = 0;
    }
    return new_str;
}

char *
generate_hash_key(const char *crm_msg_reference, const char *sys)
{
    char *hash_key = crm_concat(sys ? sys : "none", crm_msg_reference, '_');

    crm_trace("created hash key: (%s)", hash_key);
    return hash_key;
}


char *
crm_itoa_stack(int an_int, char *buffer, size_t len)
{
    if (buffer != NULL) {
        snprintf(buffer, len, "%d", an_int);
    }

    return buffer;
}

char *
crm_itoa(int an_int)
{
    int len = 32;
    char *buffer = NULL;

    buffer = malloc(len + 1);
    if (buffer != NULL) {
        snprintf(buffer, len, "%d", an_int);
    }

    return buffer;
}

int
crm_user_lookup(const char *name, uid_t * uid, gid_t * gid)
{
    int rc = -1;
    char *buffer = NULL;
    struct passwd pwd;
    struct passwd *pwentry = NULL;

    buffer = calloc(1, PW_BUFFER_LEN);
    getpwnam_r(name, &pwd, buffer, PW_BUFFER_LEN, &pwentry);
    if (pwentry) {
        rc = 0;
        if (uid) {
            *uid = pwentry->pw_uid;
        }
        if (gid) {
            *gid = pwentry->pw_gid;
        }
        crm_trace("Cluster user %s has uid=%d gid=%d", name, pwentry->pw_uid, pwentry->pw_gid);

    } else {
        crm_err("Cluster user %s does not exist", name);
    }

    free(buffer);
    return rc;
}

static int
crm_version_helper(const char *text, char **end_text)
{
    int atoi_result = -1;

    CRM_ASSERT(end_text != NULL);

    errno = 0;

    if (text != NULL && text[0] != 0) {
        atoi_result = (int)strtol(text, end_text, 10);

        if (errno == EINVAL) {
            crm_err("Conversion of '%s' %c failed", text, text[0]);
            atoi_result = -1;
        }
    }
    return atoi_result;
}

/*
 * version1 < version2 : -1
 * version1 = version2 :  0
 * version1 > version2 :  1
 */
int
compare_version(const char *version1, const char *version2)
{
    int rc = 0;
    int lpc = 0;
    char *ver1_copy = NULL, *ver2_copy = NULL;
    char *rest1 = NULL, *rest2 = NULL;

    if (version1 == NULL && version2 == NULL) {
        return 0;
    } else if (version1 == NULL) {
        return -1;
    } else if (version2 == NULL) {
        return 1;
    }

    ver1_copy = strdup(version1);
    ver2_copy = strdup(version2);
    rest1 = ver1_copy;
    rest2 = ver2_copy;

    while (1) {
        int digit1 = 0;
        int digit2 = 0;

        lpc++;

        if (rest1 == rest2) {
            break;
        }

        if (rest1 != NULL) {
            digit1 = crm_version_helper(rest1, &rest1);
        }

        if (rest2 != NULL) {
            digit2 = crm_version_helper(rest2, &rest2);
        }

        if (digit1 < digit2) {
            rc = -1;
            break;

        } else if (digit1 > digit2) {
            rc = 1;
            break;
        }

        if (rest1 != NULL && rest1[0] == '.') {
            rest1++;
        }
        if (rest1 != NULL && rest1[0] == 0) {
            rest1 = NULL;
        }

        if (rest2 != NULL && rest2[0] == '.') {
            rest2++;
        }
        if (rest2 != NULL && rest2[0] == 0) {
            rest2 = NULL;
        }
    }

    free(ver1_copy);
    free(ver2_copy);

    if (rc == 0) {
        crm_trace("%s == %s (%d)", version1, version2, lpc);
    } else if (rc < 0) {
        crm_trace("%s < %s (%d)", version1, version2, lpc);
    } else if (rc > 0) {
        crm_trace("%s > %s (%d)", version1, version2, lpc);
    }

    return rc;
}

gboolean do_stderr = FALSE;

void
g_hash_destroy_str(gpointer data)
{
    free(data);
}

#include <sys/types.h>
/* #include <stdlib.h> */
/* #include <limits.h> */

long long
crm_int_helper(const char *text, char **end_text)
{
    long long result = -1;
    char *local_end_text = NULL;
    int saved_errno = 0;

    errno = 0;

    if (text != NULL) {
#ifdef ANSI_ONLY
        if (end_text != NULL) {
            result = strtol(text, end_text, 10);
        } else {
            result = strtol(text, &local_end_text, 10);
        }
#else
        if (end_text != NULL) {
            result = strtoll(text, end_text, 10);
        } else {
            result = strtoll(text, &local_end_text, 10);
        }
#endif

        saved_errno = errno;
/* 		CRM_CHECK(errno != EINVAL); */
        if (errno == EINVAL) {
            crm_err("Conversion of %s failed", text);
            result = -1;

        } else if (errno == ERANGE) {
            crm_err("Conversion of %s was clipped: %lld", text, result);

        } else if (errno != 0) {
            crm_perror(LOG_ERR, "Conversion of %s failed:", text);
        }

        if (local_end_text != NULL && local_end_text[0] != '\0') {
            crm_err("Characters left over after parsing '%s': '%s'", text, local_end_text);
        }

        errno = saved_errno;
    }
    return result;
}

int
crm_parse_int(const char *text, const char *default_text)
{
    int atoi_result = -1;

    if (text != NULL) {
        atoi_result = crm_int_helper(text, NULL);
        if (errno == 0) {
            return atoi_result;
        }
    }

    if (default_text != NULL) {
        atoi_result = crm_int_helper(default_text, NULL);
        if (errno == 0) {
            return atoi_result;
        }

    } else {
        crm_err("No default conversion value supplied");
    }

    return -1;
}

gboolean
safe_str_neq(const char *a, const char *b)
{
    if (a == b) {
        return FALSE;

    } else if (a == NULL || b == NULL) {
        return TRUE;

    } else if (strcasecmp(a, b) == 0) {
        return FALSE;
    }
    return TRUE;
}

gboolean
crm_is_true(const char *s)
{
    gboolean ret = FALSE;

    if (s != NULL) {
        crm_str_to_boolean(s, &ret);
    }
    return ret;
}

int
crm_str_to_boolean(const char *s, int *ret)
{
    if (s == NULL) {
        return -1;

    } else if (strcasecmp(s, "true") == 0
               || strcasecmp(s, "on") == 0
               || strcasecmp(s, "yes") == 0 || strcasecmp(s, "y") == 0 || strcasecmp(s, "1") == 0) {
        *ret = TRUE;
        return 1;

    } else if (strcasecmp(s, "false") == 0
               || strcasecmp(s, "off") == 0
               || strcasecmp(s, "no") == 0 || strcasecmp(s, "n") == 0 || strcasecmp(s, "0") == 0) {
        *ret = FALSE;
        return 1;
    }
    return -1;
}

#ifndef NUMCHARS
#  define	NUMCHARS	"0123456789."
#endif

#ifndef WHITESPACE
#  define	WHITESPACE	" \t\n\r\f"
#endif

unsigned long long
crm_get_interval(const char *input)
{
    unsigned long long msec = 0;

    if (input == NULL) {
        return msec;

    } else if (input[0] != 'P') {
        long long tmp = crm_get_msec(input);

        if(tmp > 0) {
            msec = tmp;
        }

    } else {
        crm_time_t *interval = crm_time_parse_duration(input);

        msec = 1000 * crm_time_get_seconds(interval);
        crm_time_free(interval);
    }

    return msec;
}

long long
crm_get_msec(const char *input)
{
    const char *cp = input;
    const char *units;
    long long multiplier = 1000;
    long long divisor = 1;
    long long msec = -1;
    char *end_text = NULL;

    /* double dret; */

    if (input == NULL) {
        return msec;
    }

    cp += strspn(cp, WHITESPACE);
    units = cp + strspn(cp, NUMCHARS);
    units += strspn(units, WHITESPACE);

    if (strchr(NUMCHARS, *cp) == NULL) {
        return msec;
    }

    if (strncasecmp(units, "ms", 2) == 0 || strncasecmp(units, "msec", 4) == 0) {
        multiplier = 1;
        divisor = 1;
    } else if (strncasecmp(units, "us", 2) == 0 || strncasecmp(units, "usec", 4) == 0) {
        multiplier = 1;
        divisor = 1000;
    } else if (strncasecmp(units, "s", 1) == 0 || strncasecmp(units, "sec", 3) == 0) {
        multiplier = 1000;
        divisor = 1;
    } else if (strncasecmp(units, "m", 1) == 0 || strncasecmp(units, "min", 3) == 0) {
        multiplier = 60 * 1000;
        divisor = 1;
    } else if (strncasecmp(units, "h", 1) == 0 || strncasecmp(units, "hr", 2) == 0) {
        multiplier = 60 * 60 * 1000;
        divisor = 1;
    } else if (*units != EOS && *units != '\n' && *units != '\r') {
        return msec;
    }

    msec = crm_int_helper(cp, &end_text);
    if (msec > LLONG_MAX/multiplier) {
        /* arithmetics overflow while multiplier/divisor mutually exclusive */
        return LLONG_MAX;
    }
    msec *= multiplier;
    msec /= divisor;
    /* dret += 0.5; */
    /* msec = (long long)dret; */
    return msec;
}

char *
generate_op_key(const char *rsc_id, const char *op_type, int interval)
{
    int len = 35;
    char *op_id = NULL;

    CRM_CHECK(rsc_id != NULL, return NULL);
    CRM_CHECK(op_type != NULL, return NULL);

    len += strlen(op_type);
    len += strlen(rsc_id);
    op_id = malloc(len);
    CRM_CHECK(op_id != NULL, return NULL);
    sprintf(op_id, "%s_%s_%d", rsc_id, op_type, interval);
    return op_id;
}

gboolean
parse_op_key(const char *key, char **rsc_id, char **op_type, int *interval)
{
    char *notify = NULL;
    char *mutable_key = NULL;
    char *mutable_key_ptr = NULL;
    int len = 0, offset = 0, ch = 0;

    CRM_CHECK(key != NULL, return FALSE);

    *interval = 0;
    len = strlen(key);
    offset = len - 1;

    crm_trace("Source: %s", key);

    while (offset > 0 && isdigit(key[offset])) {
        int digits = len - offset;

        ch = key[offset] - '0';
        CRM_CHECK(ch < 10, return FALSE);
        CRM_CHECK(ch >= 0, return FALSE);
        while (digits > 1) {
            digits--;
            ch = ch * 10;
        }
        *interval += ch;
        offset--;
    }

    crm_trace("  Interval: %d", *interval);
    CRM_CHECK(key[offset] == '_', return FALSE);

    mutable_key = strdup(key);
    mutable_key[offset] = 0;
    offset--;

    while (offset > 0 && key[offset] != '_') {
        offset--;
    }

    CRM_CHECK(key[offset] == '_', free(mutable_key);
              return FALSE);

    mutable_key_ptr = mutable_key + offset + 1;

    crm_trace("  Action: %s", mutable_key_ptr);

    *op_type = strdup(mutable_key_ptr);

    mutable_key[offset] = 0;
    offset--;

    CRM_CHECK(mutable_key != mutable_key_ptr, free(mutable_key);
              return FALSE);

    notify = strstr(mutable_key, "_post_notify");
    if (notify && safe_str_eq(notify, "_post_notify")) {
        notify[0] = 0;
    }

    notify = strstr(mutable_key, "_pre_notify");
    if (notify && safe_str_eq(notify, "_pre_notify")) {
        notify[0] = 0;
    }

    crm_trace("  Resource: %s", mutable_key);
    *rsc_id = mutable_key;

    return TRUE;
}

char *
generate_notify_key(const char *rsc_id, const char *notify_type, const char *op_type)
{
    int len = 12;
    char *op_id = NULL;

    CRM_CHECK(rsc_id != NULL, return NULL);
    CRM_CHECK(op_type != NULL, return NULL);
    CRM_CHECK(notify_type != NULL, return NULL);

    len += strlen(op_type);
    len += strlen(rsc_id);
    len += strlen(notify_type);
    if(len > 0) {
        op_id = malloc(len);
    }
    if (op_id != NULL) {
        sprintf(op_id, "%s_%s_notify_%s_0", rsc_id, notify_type, op_type);
    }
    return op_id;
}

char *
generate_transition_magic_v202(const char *transition_key, int op_status)
{
    int len = 80;
    char *fail_state = NULL;

    CRM_CHECK(transition_key != NULL, return NULL);

    len += strlen(transition_key);

    fail_state = malloc(len);
    if (fail_state != NULL) {
        snprintf(fail_state, len, "%d:%s", op_status, transition_key);
    }
    return fail_state;
}

char *
generate_transition_magic(const char *transition_key, int op_status, int op_rc)
{
    int len = 80;
    char *fail_state = NULL;

    CRM_CHECK(transition_key != NULL, return NULL);

    len += strlen(transition_key);

    fail_state = malloc(len);
    if (fail_state != NULL) {
        snprintf(fail_state, len, "%d:%d;%s", op_status, op_rc, transition_key);
    }
    return fail_state;
}

gboolean
decode_transition_magic(const char *magic, char **uuid, int *transition_id, int *action_id,
                        int *op_status, int *op_rc, int *target_rc)
{
    int res = 0;
    char *key = NULL;
    gboolean result = TRUE;

    CRM_CHECK(magic != NULL, return FALSE);
    CRM_CHECK(op_rc != NULL, return FALSE);
    CRM_CHECK(op_status != NULL, return FALSE);

    key = calloc(1, strlen(magic) + 1);
    res = sscanf(magic, "%d:%d;%s", op_status, op_rc, key);
    if (res != 3) {
        crm_warn("Only found %d items in: '%s'", res, magic);
        free(key);
        return FALSE;
    }

    CRM_CHECK(decode_transition_key(key, uuid, transition_id, action_id, target_rc), result = FALSE);

    free(key);
    return result;
}

char *
generate_transition_key(int transition_id, int action_id, int target_rc, const char *node)
{
    int len = 40;
    char *fail_state = NULL;

    CRM_CHECK(node != NULL, return NULL);

    len += strlen(node);

    fail_state = malloc(len);
    if (fail_state != NULL) {
        snprintf(fail_state, len, "%d:%d:%d:%-*s", action_id, transition_id, target_rc, 36, node);
    }
    return fail_state;
}

gboolean
decode_transition_key(const char *key, char **uuid, int *transition_id, int *action_id,
                      int *target_rc)
{
    int res = 0;
    gboolean done = FALSE;

    CRM_CHECK(uuid != NULL, return FALSE);
    CRM_CHECK(target_rc != NULL, return FALSE);
    CRM_CHECK(action_id != NULL, return FALSE);
    CRM_CHECK(transition_id != NULL, return FALSE);

    *uuid = calloc(1, 37);
    res = sscanf(key, "%d:%d:%d:%36s", action_id, transition_id, target_rc, *uuid);
    switch (res) {
        case 4:
            /* Post Pacemaker 0.6 */
            done = TRUE;
            break;
        case 3:
        case 2:
            /* this can be tricky - the UUID might start with an integer */

            /* Until Pacemaker 0.6 */
            done = TRUE;
            *target_rc = -1;
            res = sscanf(key, "%d:%d:%36s", action_id, transition_id, *uuid);
            if (res == 2) {
                *action_id = -1;
                res = sscanf(key, "%d:%36s", transition_id, *uuid);
                CRM_CHECK(res == 2, done = FALSE);

            } else if (res != 3) {
                CRM_CHECK(res == 3, done = FALSE);
            }
            break;

        case 1:
            /* Prior to Heartbeat 2.0.8 */
            done = TRUE;
            *action_id = -1;
            *target_rc = -1;
            res = sscanf(key, "%d:%36s", transition_id, *uuid);
            CRM_CHECK(res == 2, done = FALSE);
            break;
        default:
            crm_crit("Unhandled sscanf result (%d) for %s", res, key);
    }

    if (strlen(*uuid) != 36) {
        crm_warn("Bad UUID (%s) in sscanf result (%d) for %s", *uuid, res, key);
    }

    if (done == FALSE) {
        crm_err("Cannot decode '%s' rc=%d", key, res);

        free(*uuid);
        *uuid = NULL;
        *target_rc = -1;
        *action_id = -1;
        *transition_id = -1;
    }

    return done;
}

void
filter_action_parameters(xmlNode * param_set, const char *version)
{
    char *key = NULL;
    char *timeout = NULL;
    char *interval = NULL;

    const char *attr_filter[] = {
        XML_ATTR_ID,
        XML_ATTR_CRM_VERSION,
        XML_LRM_ATTR_OP_DIGEST,
    };

    gboolean do_delete = FALSE;
    int lpc = 0;
    static int meta_len = 0;

    if (meta_len == 0) {
        meta_len = strlen(CRM_META);
    }

    if (param_set == NULL) {
        return;
    }

    for (lpc = 0; lpc < DIMOF(attr_filter); lpc++) {
        xml_remove_prop(param_set, attr_filter[lpc]);
    }

    key = crm_meta_name(XML_LRM_ATTR_INTERVAL);
    interval = crm_element_value_copy(param_set, key);
    free(key);

    key = crm_meta_name(XML_ATTR_TIMEOUT);
    timeout = crm_element_value_copy(param_set, key);

    if (param_set) {
        xmlAttrPtr xIter = param_set->properties;

        while (xIter) {
            const char *prop_name = (const char *)xIter->name;

            xIter = xIter->next;
            do_delete = FALSE;
            if (strncasecmp(prop_name, CRM_META, meta_len) == 0) {
                do_delete = TRUE;
            }

            if (do_delete) {
                xml_remove_prop(param_set, prop_name);
            }
        }
    }

    if (crm_get_msec(interval) > 0 && compare_version(version, "1.0.8") > 0) {
        /* Re-instate the operation's timeout value */
        if (timeout != NULL) {
            crm_xml_add(param_set, key, timeout);
        }
    }

    free(interval);
    free(timeout);
    free(key);
}

extern bool crm_is_daemon;

/* coverity[+kill] */
void
crm_abort(const char *file, const char *function, int line,
          const char *assert_condition, gboolean do_core, gboolean do_fork)
{
    int rc = 0;
    int pid = 0;
    int status = 0;

    /* Implied by the parent's error logging below */
    /* crm_write_blackbox(0); */

    if(crm_is_daemon == FALSE) {
        /* This is a command line tool - do not fork */

        /* crm_add_logfile(NULL);   * Record it to a file? */
        crm_enable_stderr(TRUE); /* Make sure stderr is enabled so we can tell the caller */
        do_fork = FALSE;         /* Just crash if needed */
    }

    if (do_core == FALSE) {
        crm_err("%s: Triggered assert at %s:%d : %s", function, file, line, assert_condition);
        return;

    } else if (do_fork) {
        pid = fork();

    } else {
        crm_err("%s: Triggered fatal assert at %s:%d : %s", function, file, line, assert_condition);
    }

    if (pid == -1) {
        crm_crit("%s: Cannot create core for non-fatal assert at %s:%d : %s",
                 function, file, line, assert_condition);
        return;

    } else if(pid == 0) {
        /* Child process */
        abort();
        return;
    }

    /* Parent process */
    crm_err("%s: Forked child %d to record non-fatal assert at %s:%d : %s",
            function, pid, file, line, assert_condition);
    crm_write_blackbox(SIGTRAP, NULL);

    do {
        rc = waitpid(pid, &status, 0);
        if(rc == pid) {
            return; /* Job done */
        }

    } while(errno == EINTR);

    if (errno == ECHILD) {
        /* crm_mon does this */
        crm_trace("Cannot wait on forked child %d - SIGCHLD is probably set to SIG_IGN", pid);
        return;
    }
    crm_perror(LOG_ERR, "Cannot wait on forked child %d", pid);
}

int
crm_pid_active(long pid, const char *daemon)
{
    static int have_proc_pid = 0;

    if(have_proc_pid == 0) {
        char proc_path[PATH_MAX], exe_path[PATH_MAX];

        /* check to make sure pid hasn't been reused by another process */
        snprintf(proc_path, sizeof(proc_path), "/proc/%lu/exe", (long unsigned int)getpid());

        have_proc_pid = 1;
        if(readlink(proc_path, exe_path, PATH_MAX - 1) < 0) {
            have_proc_pid = -1;
        }
    }

    if (pid <= 0) {
        return -1;

    } else if (kill(pid, 0) < 0 && errno == ESRCH) {
        return 0;

    } else if(daemon == NULL || have_proc_pid == -1) {
        return 1;

    } else {
        int rc = 0;
        char proc_path[PATH_MAX], exe_path[PATH_MAX], myexe_path[PATH_MAX];

        /* check to make sure pid hasn't been reused by another process */
        snprintf(proc_path, sizeof(proc_path), "/proc/%lu/exe", pid);

        rc = readlink(proc_path, exe_path, PATH_MAX - 1);
        if (rc < 0) {
            crm_perror(LOG_ERR, "Could not read from %s", proc_path);
            return 0;
        }

        exe_path[rc] = 0;

        if(daemon[0] != '/') {
            rc = snprintf(myexe_path, sizeof(proc_path), CRM_DAEMON_DIR"/%s", daemon);
            myexe_path[rc] = 0;
        } else {
            rc = snprintf(myexe_path, sizeof(proc_path), "%s", daemon);
            myexe_path[rc] = 0;
        }
        
        if (strcmp(exe_path, myexe_path) == 0) {
            return 1;
        }
    }

    return 0;
}

#define	LOCKSTRLEN	11

int
crm_read_pidfile(const char *filename)
{
    int fd;
    long pid = -1;
    char buf[LOCKSTRLEN + 1];

    if ((fd = open(filename, O_RDONLY)) < 0) {
        goto bail;
    }

    if (read(fd, buf, sizeof(buf)) < 1) {
        goto bail;
    }

    if (sscanf(buf, "%lu", &pid) > 0) {
        if (pid <= 0) {
            pid = -ESRCH;
        }
    }

  bail:
    if (fd >= 0) {
        close(fd);
    }
    return pid;
}

int
crm_pidfile_inuse(const char *filename, long mypid, const char *daemon)
{
    long pid = 0;
    struct stat sbuf;
    char buf[LOCKSTRLEN + 1];
    int rc = -ENOENT, fd = 0;

    if ((fd = open(filename, O_RDONLY)) >= 0) {
        if (fstat(fd, &sbuf) >= 0 && sbuf.st_size < LOCKSTRLEN) {
            sleep(2);           /* if someone was about to create one,
                                 * give'm a sec to do so
                                 */
        }
        if (read(fd, buf, sizeof(buf)) > 0) {
            if (sscanf(buf, "%lu", &pid) > 0) {
                crm_trace("Got pid %lu from %s\n", pid, filename);
                if (pid <= 1) {
                    /* Invalid pid */
                    rc = -ENOENT;
                    unlink(filename);

                } else if (mypid && pid == mypid) {
                    /* In use by us */
                    rc = pcmk_ok;

                } else if (crm_pid_active(pid, daemon) == FALSE) {
                    /* Contains a stale value */
                    unlink(filename);
                    rc = -ENOENT;

                } else if (mypid && pid != mypid) {
                    /* locked by existing process - give up */
                    rc = -EEXIST;
                }
            }
        }
        close(fd);
    }
    return rc;
}

static int
crm_lock_pidfile(const char *filename, const char *name)
{
    long mypid = 0;
    int fd = 0, rc = 0;
    char buf[LOCKSTRLEN + 1];

    mypid = (unsigned long)getpid();

    rc = crm_pidfile_inuse(filename, 0, name);
    if (rc == -ENOENT) {
        /* exists but the process is not active */

    } else if (rc != pcmk_ok) {
        /* locked by existing process - give up */
        return rc;
    }

    if ((fd = open(filename, O_CREAT | O_WRONLY | O_EXCL, 0644)) < 0) {
        /* Hmmh, why did we fail? Anyway, nothing we can do about it */
        return -errno;
    }

    snprintf(buf, sizeof(buf), "%*lu\n", LOCKSTRLEN - 1, mypid);
    rc = write(fd, buf, LOCKSTRLEN);
    close(fd);

    if (rc != LOCKSTRLEN) {
        crm_perror(LOG_ERR, "Incomplete write to %s", filename);
        return -errno;
    }

    return crm_pidfile_inuse(filename, mypid, name);
}

void
crm_make_daemon(const char *name, gboolean daemonize, const char *pidfile)
{
    int rc;
    long pid;
    const char *devnull = "/dev/null";

    if (daemonize == FALSE) {
        return;
    }

    /* Check before we even try... */
    rc = crm_pidfile_inuse(pidfile, 1, name);
    if(rc < pcmk_ok && rc != -ENOENT) {
        pid = crm_read_pidfile(pidfile);
        crm_err("%s: already running [pid %ld in %s]", name, pid, pidfile);
        printf("%s: already running [pid %ld in %s]\n", name, pid, pidfile);
        crm_exit(rc);
    }

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "%s: could not start daemon\n", name);
        crm_perror(LOG_ERR, "fork");
        crm_exit(EINVAL);

    } else if (pid > 0) {
        crm_exit(pcmk_ok);
    }

    rc = crm_lock_pidfile(pidfile, name);
    if(rc < pcmk_ok) {
        crm_err("Could not lock '%s' for %s: %s (%d)", pidfile, name, pcmk_strerror(rc), rc);
        printf("Could not lock '%s' for %s: %s (%d)\n", pidfile, name, pcmk_strerror(rc), rc);
        crm_exit(rc);
    }

    umask(S_IWGRP | S_IWOTH | S_IROTH);

    close(STDIN_FILENO);
    (void)open(devnull, O_RDONLY);      /* Stdin:  fd 0 */
    close(STDOUT_FILENO);
    (void)open(devnull, O_WRONLY);      /* Stdout: fd 1 */
    close(STDERR_FILENO);
    (void)open(devnull, O_WRONLY);      /* Stderr: fd 2 */
}

char *
crm_strip_trailing_newline(char *str)
{
    int len;

    if (str == NULL) {
        return str;
    }

    for (len = strlen(str) - 1; len >= 0 && str[len] == '\n'; len--) {
        str[len] = '\0';
    }

    return str;
}

gboolean
crm_str_eq(const char *a, const char *b, gboolean use_case)
{
    if (use_case) {
        return g_strcmp0(a, b) == 0;

        /* TODO - Figure out which calls, if any, really need to be case independent */
    } else if (a == b) {
        return TRUE;

    } else if (a == NULL || b == NULL) {
        /* shouldn't be comparing NULLs */
        return FALSE;

    } else if (strcasecmp(a, b) == 0) {
        return TRUE;
    }
    return FALSE;
}

char *
crm_meta_name(const char *field)
{
    int lpc = 0;
    int max = 0;
    char *crm_name = NULL;

    CRM_CHECK(field != NULL, return NULL);
    crm_name = crm_concat(CRM_META, field, '_');

    /* Massage the names so they can be used as shell variables */
    max = strlen(crm_name);
    for (; lpc < max; lpc++) {
        switch (crm_name[lpc]) {
            case '-':
                crm_name[lpc] = '_';
                break;
        }
    }
    return crm_name;
}

const char *
crm_meta_value(GHashTable * hash, const char *field)
{
    char *key = NULL;
    const char *value = NULL;

    key = crm_meta_name(field);
    if (key) {
        value = g_hash_table_lookup(hash, key);
        free(key);
    }

    return value;
}

static struct option *
crm_create_long_opts(struct crm_option *long_options)
{
    struct option *long_opts = NULL;

#ifdef HAVE_GETOPT_H
    int index = 0, lpc = 0;

    /*
     * A previous, possibly poor, choice of '?' as the short form of --help
     * means that getopt_long() returns '?' for both --help and for "unknown option"
     *
     * This dummy entry allows us to differentiate between the two in crm_get_option()
     * and exit with the correct error code
     */
    long_opts = realloc_safe(long_opts, (index + 1) * sizeof(struct option));
    long_opts[index].name = "__dummmy__";
    long_opts[index].has_arg = 0;
    long_opts[index].flag = 0;
    long_opts[index].val = '_';
    index++;

    for (lpc = 0; long_options[lpc].name != NULL; lpc++) {
        if (long_options[lpc].name[0] == '-') {
            continue;
        }

        long_opts = realloc_safe(long_opts, (index + 1) * sizeof(struct option));
        /*fprintf(stderr, "Creating %d %s = %c\n", index,
         * long_options[lpc].name, long_options[lpc].val);      */
        long_opts[index].name = long_options[lpc].name;
        long_opts[index].has_arg = long_options[lpc].has_arg;
        long_opts[index].flag = long_options[lpc].flag;
        long_opts[index].val = long_options[lpc].val;
        index++;
    }

    /* Now create the list terminator */
    long_opts = realloc_safe(long_opts, (index + 1) * sizeof(struct option));
    long_opts[index].name = NULL;
    long_opts[index].has_arg = 0;
    long_opts[index].flag = 0;
    long_opts[index].val = 0;
#endif

    return long_opts;
}

void
crm_set_options(const char *short_options, const char *app_usage, struct crm_option *long_options,
                const char *app_desc)
{
    if (short_options) {
        crm_short_options = strdup(short_options);

    } else if (long_options) {
        int lpc = 0;
        int opt_string_len = 0;
        char *local_short_options = NULL;

        for (lpc = 0; long_options[lpc].name != NULL; lpc++) {
            if (long_options[lpc].val && long_options[lpc].val != '-' && long_options[lpc].val < UCHAR_MAX) {
                local_short_options = realloc_safe(local_short_options, opt_string_len + 4);
                local_short_options[opt_string_len++] = long_options[lpc].val;
                /* getopt(3) says: Two colons mean an option takes an optional arg; */
                if (long_options[lpc].has_arg == optional_argument) {
                    local_short_options[opt_string_len++] = ':';
                }
                if (long_options[lpc].has_arg >= required_argument) {
                    local_short_options[opt_string_len++] = ':';
                }
                local_short_options[opt_string_len] = 0;
            }
        }
        crm_short_options = local_short_options;
        crm_trace("Generated short option string: '%s'", local_short_options);
    }

    if (long_options) {
        crm_long_options = long_options;
    }
    if (app_desc) {
        crm_app_description = app_desc;
    }
    if (app_usage) {
        crm_app_usage = app_usage;
    }
}

int
crm_get_option(int argc, char **argv, int *index)
{
    return crm_get_option_long(argc, argv, index, NULL);
}

int
crm_get_option_long(int argc, char **argv, int *index, const char **longname)
{
#ifdef HAVE_GETOPT_H
    static struct option *long_opts = NULL;

    if (long_opts == NULL && crm_long_options) {
        long_opts = crm_create_long_opts(crm_long_options);
    }

    *index = 0;
    if (long_opts) {
        int flag = getopt_long(argc, argv, crm_short_options, long_opts, index);

        switch (flag) {
            case 0:
                if (long_opts[*index].val) {
                    return long_opts[*index].val;
                } else if (longname) {
                    *longname = long_opts[*index].name;
                } else {
                    crm_notice("Unhandled option --%s", long_opts[*index].name);
                    return flag;
                }
            case -1:           /* End of option processing */
                break;
            case ':':
                crm_trace("Missing argument");
                crm_help('?', 1);
                break;
            case '?':
                crm_help('?', *index ? 0 : 1);
                break;
        }
        return flag;
    }
#endif

    if (crm_short_options) {
        return getopt(argc, argv, crm_short_options);
    }

    return -1;
}

int
crm_help(char cmd, int exit_code)
{
    int i = 0;
    FILE *stream = (exit_code ? stderr : stdout);

    if (cmd == 'v' || cmd == '$') {
        fprintf(stream, "Pacemaker %s\n", PACEMAKER_VERSION);
        fprintf(stream, "Written by Andrew Beekhof\n");
        goto out;
    }

    if (cmd == '!') {
        fprintf(stream, "Pacemaker %s (Build: %s): %s\n", PACEMAKER_VERSION, BUILD_VERSION, CRM_FEATURES);
        goto out;
    }

    fprintf(stream, "%s - %s\n", crm_system_name, crm_app_description);

    if (crm_app_usage) {
        fprintf(stream, "Usage: %s %s\n", crm_system_name, crm_app_usage);
    }

    if (crm_long_options) {
        fprintf(stream, "Options:\n");
        for (i = 0; crm_long_options[i].name != NULL; i++) {
            if (crm_long_options[i].flags & pcmk_option_hidden) {

            } else if (crm_long_options[i].flags & pcmk_option_paragraph) {
                fprintf(stream, "%s\n\n", crm_long_options[i].desc);

            } else if (crm_long_options[i].flags & pcmk_option_example) {
                fprintf(stream, "\t#%s\n\n", crm_long_options[i].desc);

            } else if (crm_long_options[i].val == '-' && crm_long_options[i].desc) {
                fprintf(stream, "%s\n", crm_long_options[i].desc);

            } else {
                /* is val printable as char ? */
                if (crm_long_options[i].val && crm_long_options[i].val <= UCHAR_MAX) {
                    fprintf(stream, " -%c,", crm_long_options[i].val);
                } else {
                    fputs("    ", stream);
                }
                fprintf(stream, " --%s%s\t%s\n", crm_long_options[i].name,
                        crm_long_options[i].has_arg == optional_argument ? "[=value]" :
                        crm_long_options[i].has_arg == required_argument ? "=value" : "",
                        crm_long_options[i].desc ? crm_long_options[i].desc : "");
            }
        }

    } else if (crm_short_options) {
        fprintf(stream, "Usage: %s - %s\n", crm_system_name, crm_app_description);
        for (i = 0; crm_short_options[i] != 0; i++) {
            int has_arg = no_argument /* 0 */;

            if (crm_short_options[i + 1] == ':') {
                if (crm_short_options[i + 2] == ':')
                    has_arg = optional_argument /* 2 */;
                else
                    has_arg = required_argument /* 1 */;
            }

            fprintf(stream, " -%c %s\n", crm_short_options[i],
                    has_arg == optional_argument ? "[value]" :
                    has_arg == required_argument ? "{value}" : "");
            i += has_arg;
        }
    }

    fprintf(stream, "\nReport bugs to %s\n", PACKAGE_BUGREPORT);

  out:
    return crm_exit(exit_code);
}

void cib_ipc_servers_init(qb_ipcs_service_t **ipcs_ro,
        qb_ipcs_service_t **ipcs_rw,
        qb_ipcs_service_t **ipcs_shm,
        struct qb_ipcs_service_handlers *ro_cb,
        struct qb_ipcs_service_handlers *rw_cb)
{
    *ipcs_ro = mainloop_add_ipc_server(cib_channel_ro, QB_IPC_NATIVE, ro_cb);
    *ipcs_rw = mainloop_add_ipc_server(cib_channel_rw, QB_IPC_NATIVE, rw_cb);
    *ipcs_shm = mainloop_add_ipc_server(cib_channel_shm, QB_IPC_SHM, rw_cb);

    if (*ipcs_ro == NULL || *ipcs_rw == NULL || *ipcs_shm == NULL) {
        crm_err("Failed to create cib servers: exiting and inhibiting respawn.");
        crm_warn("Verify pacemaker and pacemaker_remote are not both enabled.");
        crm_exit(DAEMON_RESPAWN_STOP);
    }
}

void cib_ipc_servers_destroy(qb_ipcs_service_t *ipcs_ro,
        qb_ipcs_service_t *ipcs_rw,
        qb_ipcs_service_t *ipcs_shm)
{
    qb_ipcs_destroy(ipcs_ro);
    qb_ipcs_destroy(ipcs_rw);
    qb_ipcs_destroy(ipcs_shm);
}

qb_ipcs_service_t *
crmd_ipc_server_init(struct qb_ipcs_service_handlers *cb)
{
    return mainloop_add_ipc_server(CRM_SYSTEM_CRMD, QB_IPC_NATIVE, cb);
}

void
attrd_ipc_server_init(qb_ipcs_service_t **ipcs, struct qb_ipcs_service_handlers *cb)
{
    *ipcs = mainloop_add_ipc_server(T_ATTRD, QB_IPC_NATIVE, cb);

    if (*ipcs == NULL) {
        crm_err("Failed to create attrd servers: exiting and inhibiting respawn.");
        crm_warn("Verify pacemaker and pacemaker_remote are not both enabled.");
        crm_exit(DAEMON_RESPAWN_STOP);
    }
}

void
stonith_ipc_server_init(qb_ipcs_service_t **ipcs, struct qb_ipcs_service_handlers *cb)
{
    *ipcs = mainloop_add_ipc_server("stonith-ng", QB_IPC_NATIVE, cb);

    if (*ipcs == NULL) {
        crm_err("Failed to create stonith-ng servers: exiting and inhibiting respawn.");
        crm_warn("Verify pacemaker and pacemaker_remote are not both enabled.");
        crm_exit(DAEMON_RESPAWN_STOP);
    }
}

int
attrd_update_delegate(crm_ipc_t * ipc, char command, const char *host, const char *name,
                      const char *value, const char *section, const char *set, const char *dampen,
                      const char *user_name, int options)
{
    int rc = -ENOTCONN;
    int max = 5;
    xmlNode *update = create_xml_node(NULL, __FUNCTION__);

    static gboolean connected = TRUE;
    static crm_ipc_t *local_ipc = NULL;
    static enum crm_ipc_flags flags = crm_ipc_flags_none;

    if (ipc == NULL && local_ipc == NULL) {
        local_ipc = crm_ipc_new(T_ATTRD, 0);
        flags |= crm_ipc_client_response;
        connected = FALSE;
    }

    if (ipc == NULL) {
        ipc = local_ipc;
    }

    /* remap common aliases */
    if (safe_str_eq(section, "reboot")) {
        section = XML_CIB_TAG_STATUS;

    } else if (safe_str_eq(section, "forever")) {
        section = XML_CIB_TAG_NODES;
    }

    crm_xml_add(update, F_TYPE, T_ATTRD);
    crm_xml_add(update, F_ORIG, crm_system_name);

    if (name == NULL && command == 'U') {
        command = 'R';
    }

    switch (command) {
        case 'u':
            crm_xml_add(update, F_ATTRD_TASK, ATTRD_OP_UPDATE);
            crm_xml_add(update, F_ATTRD_REGEX, name);
            break;
        case 'D':
        case 'U':
        case 'v':
            crm_xml_add(update, F_ATTRD_TASK, ATTRD_OP_UPDATE);
            crm_xml_add(update, F_ATTRD_ATTRIBUTE, name);
            break;
        case 'R':
            crm_xml_add(update, F_ATTRD_TASK, ATTRD_OP_REFRESH);
            break;
        case 'Q':
            crm_xml_add(update, F_ATTRD_TASK, ATTRD_OP_QUERY);
            crm_xml_add(update, F_ATTRD_ATTRIBUTE, name);
            break;
        case 'C':
            crm_xml_add(update, F_ATTRD_TASK, ATTRD_OP_PEER_REMOVE);
            break;
    }

    crm_xml_add(update, F_ATTRD_VALUE, value);
    crm_xml_add(update, F_ATTRD_DAMPEN, dampen);
    crm_xml_add(update, F_ATTRD_SECTION, section);
    crm_xml_add(update, F_ATTRD_HOST, host);
    crm_xml_add(update, F_ATTRD_SET, set);
    crm_xml_add_int(update, F_ATTRD_IS_REMOTE, is_set(options, attrd_opt_remote));
    crm_xml_add_int(update, F_ATTRD_IS_PRIVATE, is_set(options, attrd_opt_private));
#if ENABLE_ACL
    if (user_name) {
        crm_xml_add(update, F_ATTRD_USER, user_name);
    }
#endif

    while (max > 0) {
        if (connected == FALSE) {
            crm_info("Connecting to cluster... %d retries remaining", max);
            connected = crm_ipc_connect(ipc);
        }

        if (connected) {
            rc = crm_ipc_send(ipc, update, flags, 0, NULL);
        } else {
            crm_perror(LOG_INFO, "Connection to cluster attribute manager failed");
        }

        if (ipc != local_ipc) {
            break;

        } else if (rc > 0) {
            break;

        } else if (rc == -EAGAIN || rc == -EALREADY) {
            sleep(5 - max);
            max--;

        } else {
            crm_ipc_close(ipc);
            connected = FALSE;
            sleep(5 - max);
            max--;
        }
    }

    free_xml(update);
    if (rc > 0) {
        crm_debug("Sent update: %s=%s for %s", name, value, host ? host : "localhost");
        rc = pcmk_ok;

    } else {
        crm_debug("Could not send update %s=%s for %s: %s (%d)", name, value,
                  host ? host : "localhost", pcmk_strerror(rc), rc);
    }
    return rc;
}

#define FAKE_TE_ID	"xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
static void
append_digest(lrmd_event_data_t * op, xmlNode * update, const char *version, const char *magic,
              int level)
{
    /* this will enable us to later determine that the
     *   resource's parameters have changed and we should force
     *   a restart
     */
    char *digest = NULL;
    xmlNode *args_xml = NULL;

    if (op->params == NULL) {
        return;
    }

    args_xml = create_xml_node(NULL, XML_TAG_PARAMS);
    g_hash_table_foreach(op->params, hash2field, args_xml);
    filter_action_parameters(args_xml, version);
    digest = calculate_operation_digest(args_xml, version);

#if 0
    if (level < get_crm_log_level()
        && op->interval == 0 && crm_str_eq(op->op_type, CRMD_ACTION_START, TRUE)) {
        char *digest_source = dump_xml_unformatted(args_xml);

        do_crm_log(level, "Calculated digest %s for %s (%s). Source: %s\n",
                   digest, ID(update), magic, digest_source);
        free(digest_source);
    }
#endif
    crm_xml_add(update, XML_LRM_ATTR_OP_DIGEST, digest);

    free_xml(args_xml);
    free(digest);
}

int
rsc_op_expected_rc(lrmd_event_data_t * op)
{
    int rc = 0;

    if (op && op->user_data) {
        int dummy = 0;
        char *uuid = NULL;

        decode_transition_key(op->user_data, &uuid, &dummy, &dummy, &rc);
        free(uuid);
    }
    return rc;
}

gboolean
did_rsc_op_fail(lrmd_event_data_t * op, int target_rc)
{
    switch (op->op_status) {
        case PCMK_LRM_OP_CANCELLED:
        case PCMK_LRM_OP_PENDING:
            return FALSE;
            break;

        case PCMK_LRM_OP_NOTSUPPORTED:
        case PCMK_LRM_OP_TIMEOUT:
        case PCMK_LRM_OP_ERROR:
            return TRUE;
            break;

        default:
            if (target_rc != op->rc) {
                return TRUE;
            }
    }

    return FALSE;
}

xmlNode *
create_operation_update(xmlNode * parent, lrmd_event_data_t * op, const char * caller_version,
                        int target_rc, const char * node, const char * origin, int level)
{
    char *key = NULL;
    char *magic = NULL;
    char *op_id = NULL;
    char *op_id_additional = NULL;
    char *local_user_data = NULL;
    const char *exit_reason = NULL;

    xmlNode *xml_op = NULL;
    const char *task = NULL;
    gboolean dc_munges_migrate_ops = (compare_version(caller_version, "3.0.3") < 0);
    gboolean dc_needs_unique_ops = (compare_version(caller_version, "3.0.6") < 0);

    CRM_CHECK(op != NULL, return NULL);
    do_crm_log(level, "%s: Updating resource %s after %s op %s (interval=%d)",
               origin, op->rsc_id, op->op_type, services_lrm_status_str(op->op_status),
               op->interval);

    crm_trace("DC version: %s", caller_version);

    task = op->op_type;
    /* remap the task name under various scenarios
     * this makes life easier for the PE when trying determine the current state
     */
    if (crm_str_eq(task, "reload", TRUE)) {
        if (op->op_status == PCMK_LRM_OP_DONE) {
            task = CRMD_ACTION_START;
        } else {
            task = CRMD_ACTION_STATUS;
        }

    } else if (dc_munges_migrate_ops && crm_str_eq(task, CRMD_ACTION_MIGRATE, TRUE)) {
        /* if the migrate_from fails it will have enough info to do the right thing */
        if (op->op_status == PCMK_LRM_OP_DONE) {
            task = CRMD_ACTION_STOP;
        } else {
            task = CRMD_ACTION_STATUS;
        }

    } else if (dc_munges_migrate_ops
               && op->op_status == PCMK_LRM_OP_DONE
               && crm_str_eq(task, CRMD_ACTION_MIGRATED, TRUE)) {
        task = CRMD_ACTION_START;
    }

    key = generate_op_key(op->rsc_id, task, op->interval);
    if (dc_needs_unique_ops && op->interval > 0) {
        op_id = strdup(key);

    } else if (crm_str_eq(task, CRMD_ACTION_NOTIFY, TRUE)) {
        const char *n_type = crm_meta_value(op->params, "notify_type");
        const char *n_task = crm_meta_value(op->params, "notify_operation");

        CRM_LOG_ASSERT(n_type != NULL);
        CRM_LOG_ASSERT(n_task != NULL);
        op_id = generate_notify_key(op->rsc_id, n_type, n_task);

        /* these are not yet allowed to fail */
        op->op_status = PCMK_LRM_OP_DONE;
        op->rc = 0;

    } else if (did_rsc_op_fail(op, target_rc)) {
        op_id = generate_op_key(op->rsc_id, "last_failure", 0);
        if (op->interval == 0) {
            /* Ensure 'last' gets updated too in case recording-pending="true" */
            op_id_additional = generate_op_key(op->rsc_id, "last", 0);
        }
        exit_reason = op->exit_reason;

    } else if (op->interval > 0) {
        op_id = strdup(key);

    } else {
        op_id = generate_op_key(op->rsc_id, "last", 0);
    }

  again:
    xml_op = find_entity(parent, XML_LRM_TAG_RSC_OP, op_id);
    if (xml_op == NULL) {
        xml_op = create_xml_node(parent, XML_LRM_TAG_RSC_OP);
    }

    if (op->user_data == NULL) {
        crm_debug("Generating fake transition key for:"
                  " %s_%s_%d %d from %s",
                  op->rsc_id, op->op_type, op->interval, op->call_id, origin);
        local_user_data = generate_transition_key(-1, op->call_id, target_rc, FAKE_TE_ID);
        op->user_data = local_user_data;
    }

    if(magic == NULL) {
        magic = generate_transition_magic(op->user_data, op->op_status, op->rc);
    }

    crm_xml_add(xml_op, XML_ATTR_ID, op_id);
    crm_xml_add(xml_op, XML_LRM_ATTR_TASK_KEY, key);
    crm_xml_add(xml_op, XML_LRM_ATTR_TASK, task);
    crm_xml_add(xml_op, XML_ATTR_ORIGIN, origin);
    crm_xml_add(xml_op, XML_ATTR_CRM_VERSION, caller_version);
    crm_xml_add(xml_op, XML_ATTR_TRANSITION_KEY, op->user_data);
    crm_xml_add(xml_op, XML_ATTR_TRANSITION_MAGIC, magic);
    crm_xml_add(xml_op, XML_LRM_ATTR_EXIT_REASON, exit_reason);
    crm_xml_add(xml_op, XML_LRM_ATTR_TARGET, node); /* For context during triage */

    crm_xml_add_int(xml_op, XML_LRM_ATTR_CALLID, op->call_id);
    crm_xml_add_int(xml_op, XML_LRM_ATTR_RC, op->rc);
    crm_xml_add_int(xml_op, XML_LRM_ATTR_OPSTATUS, op->op_status);
    crm_xml_add_int(xml_op, XML_LRM_ATTR_INTERVAL, op->interval);

    if (compare_version("2.1", caller_version) <= 0) {
        if (op->t_run || op->t_rcchange || op->exec_time || op->queue_time) {
            crm_trace("Timing data (%s_%s_%d): last=%lu change=%lu exec=%lu queue=%lu",
                      op->rsc_id, op->op_type, op->interval,
                      op->t_run, op->t_rcchange, op->exec_time, op->queue_time);

            if (op->interval == 0) {
                /* The values are the same for non-recurring ops */
                crm_xml_add_int(xml_op, XML_RSC_OP_LAST_RUN, op->t_run);
                crm_xml_add_int(xml_op, XML_RSC_OP_LAST_CHANGE, op->t_run);

            } else if(op->t_rcchange) {
                /* last-run is not accurate for recurring ops */
                crm_xml_add_int(xml_op, XML_RSC_OP_LAST_CHANGE, op->t_rcchange);

            } else {
                /* ...but is better than nothing otherwise */
                crm_xml_add_int(xml_op, XML_RSC_OP_LAST_CHANGE, op->t_run);
            }

            crm_xml_add_int(xml_op, XML_RSC_OP_T_EXEC, op->exec_time);
            crm_xml_add_int(xml_op, XML_RSC_OP_T_QUEUE, op->queue_time);
        }
    }

    if (crm_str_eq(op->op_type, CRMD_ACTION_MIGRATE, TRUE)
        || crm_str_eq(op->op_type, CRMD_ACTION_MIGRATED, TRUE)) {
        /*
         * Record migrate_source and migrate_target always for migrate ops.
         */
        const char *name = XML_LRM_ATTR_MIGRATE_SOURCE;

        crm_xml_add(xml_op, name, crm_meta_value(op->params, name));

        name = XML_LRM_ATTR_MIGRATE_TARGET;
        crm_xml_add(xml_op, name, crm_meta_value(op->params, name));
    }

    append_digest(op, xml_op, caller_version, magic, LOG_DEBUG);

    if (op_id_additional) {
        free(op_id);
        op_id = op_id_additional;
        op_id_additional = NULL;
        goto again;
    }

    if (local_user_data) {
        free(local_user_data);
        op->user_data = NULL;
    }
    free(magic);
    free(op_id);
    free(key);
    return xml_op;
}

bool
pcmk_acl_required(const char *user) 
{
#if ENABLE_ACL
    if(user == NULL || strlen(user) == 0) {
        crm_trace("no user set");
        return FALSE;

    } else if (strcmp(user, CRM_DAEMON_USER) == 0) {
        return FALSE;

    } else if (strcmp(user, "root") == 0) {
        return FALSE;
    }
    crm_trace("acls required for %s", user);
    return TRUE;
#else
    crm_trace("acls not supported");
    return FALSE;
#endif
}

#if ENABLE_ACL
char *
uid2username(uid_t uid)
{
    struct passwd *pwent = getpwuid(uid);

    if (pwent == NULL) {
        crm_perror(LOG_ERR, "Cannot get password entry of uid: %d", uid);
        return NULL;

    } else {
        return strdup(pwent->pw_name);
    }
}

const char *
crm_acl_get_set_user(xmlNode * request, const char *field, const char *peer_user)
{
    /* field is only checked for backwards compatibility */
    static const char *effective_user = NULL;
    const char *requested_user = NULL;
    const char *user = NULL;

    if(effective_user == NULL) {
        effective_user = uid2username(geteuid());
    }

    requested_user = crm_element_value(request, XML_ACL_TAG_USER);
    if(requested_user == NULL) {
        requested_user = crm_element_value(request, field);
    }

    if (is_privileged(effective_user) == FALSE) {
        /* We're not running as a privileged user, set or overwrite any existing value for $XML_ACL_TAG_USER */
        user = effective_user;

    } else if(peer_user == NULL && requested_user == NULL) {
        /* No user known or requested, use 'effective_user' and make sure one is set for the request */
        user = effective_user;

    } else if(peer_user == NULL) {
        /* No user known, trusting 'requested_user' */
        user = requested_user;

    } else if (is_privileged(peer_user) == FALSE) {
        /* The peer is not a privileged user, set or overwrite any existing value for $XML_ACL_TAG_USER */
        user = peer_user;

    } else if (requested_user == NULL) {
        /* Even if we're privileged, make sure there is always a value set */
        user = peer_user;

    } else {
        /* Legal delegation to 'requested_user' */
        user = requested_user;
    }

    /* Yes, pointer comparision */
    if(user != crm_element_value(request, XML_ACL_TAG_USER)) {
        crm_xml_add(request, XML_ACL_TAG_USER, user);
    }

    if(field != NULL && user != crm_element_value(request, field)) {
        crm_xml_add(request, field, user);
    }

    return requested_user;
}

void
determine_request_user(const char *user, xmlNode * request, const char *field)
{
    /* Get our internal validation out of the way first */
    CRM_CHECK(user != NULL && request != NULL && field != NULL, return);

    /* If our peer is a privileged user, we might be doing something on behalf of someone else */
    if (is_privileged(user) == FALSE) {
        /* We're not a privileged user, set or overwrite any existing value for $field */
        crm_xml_replace(request, field, user);

    } else if (crm_element_value(request, field) == NULL) {
        /* Even if we're privileged, make sure there is always a value set */
        crm_xml_replace(request, field, user);

/*  } else { Legal delegation */
    }

    crm_trace("Processing msg as user '%s'", crm_element_value(request, field));
}
#endif

/*
 * This re-implements g_str_hash as it was prior to glib2-2.28:
 *
 *   http://git.gnome.org/browse/glib/commit/?id=354d655ba8a54b754cb5a3efb42767327775696c
 *
 * Note that the new g_str_hash is presumably a *better* hash (it's actually
 * a correct implementation of DJB's hash), but we need to preserve existing
 * behaviour, because the hash key ultimately determines the "sort" order
 * when iterating through GHashTables, which affects allocation of scores to
 * clone instances when iterating through rsc->allowed_nodes.  It (somehow)
 * also appears to have some minor impact on the ordering of a few
 * pseudo_event IDs in the transition graph.
 */
guint
g_str_hash_traditional(gconstpointer v)
{
    const signed char *p;
    guint32 h = 0;

    for (p = v; *p != '\0'; p++)
        h = (h << 5) - h + *p;

    return h;
}

guint
crm_strcase_hash(gconstpointer v)
{
    const signed char *p;
    guint32 h = 0;

    for (p = v; *p != '\0'; p++)
        h = (h << 5) - h + g_ascii_tolower(*p);

    return h;
}

void *
find_library_function(void **handle, const char *lib, const char *fn, gboolean fatal)
{
    char *error;
    void *a_function;

    if (*handle == NULL) {
        *handle = dlopen(lib, RTLD_LAZY);
    }

    if (!(*handle)) {
        crm_err("%sCould not open %s: %s", fatal ? "Fatal: " : "", lib, dlerror());
        if (fatal) {
            crm_exit(DAEMON_RESPAWN_STOP);
        }
        return NULL;
    }

    a_function = dlsym(*handle, fn);
    if ((error = dlerror()) != NULL) {
        crm_err("%sCould not find %s in %s: %s", fatal ? "Fatal: " : "", fn, lib, error);
        if (fatal) {
            crm_exit(DAEMON_RESPAWN_STOP);
        }
    }

    return a_function;
}

char *
add_list_element(char *list, const char *value)
{
    int len = 0;
    int last = 0;

    if (value == NULL) {
        return list;
    }
    if (list) {
        last = strlen(list);
    }
    len = last + 2;             /* +1 space, +1 EOS */
    len += strlen(value);
    list = realloc_safe(list, len);
    sprintf(list + last, " %s", value);
    return list;
}

void *
convert_const_pointer(const void *ptr)
{
    /* Worst function ever */
    return (void *)ptr;
}

#ifdef HAVE_UUID_UUID_H
#  include <uuid/uuid.h>
#endif

char *
crm_generate_uuid(void)
{
    unsigned char uuid[16];
    char *buffer = malloc(37);  /* Including NUL byte */

    uuid_generate(uuid);
    uuid_unparse(uuid, buffer);
    return buffer;
}

#include <md5.h>

char *
crm_md5sum(const char *buffer)
{
    int lpc = 0, len = 0;
    char *digest = NULL;
    unsigned char raw_digest[MD5_DIGEST_SIZE];

    if (buffer == NULL) {
        buffer = "";
    }
    len = strlen(buffer);

    crm_trace("Beginning digest of %d bytes", len);
    digest = malloc(2 * MD5_DIGEST_SIZE + 1);
    if(digest) {
        md5_buffer(buffer, len, raw_digest);
        for (lpc = 0; lpc < MD5_DIGEST_SIZE; lpc++) {
            sprintf(digest + (2 * lpc), "%02x", raw_digest[lpc]);
        }
        digest[(2 * MD5_DIGEST_SIZE)] = 0;
        crm_trace("Digest %s.", digest);

    } else {
        crm_err("Could not create digest");
    }
    return digest;
}

#include <time.h>
#include <bzlib.h>

bool
crm_compress_string(const char *data, int length, int max, char **result, unsigned int *result_len)
{
    int rc;
    char *compressed = NULL;
    char *uncompressed = strdup(data);
    struct timespec after_t;
    struct timespec before_t;

    if(max == 0) {
        max = (length * 1.1) + 600; /* recomended size */
    }

#ifdef CLOCK_MONOTONIC
    clock_gettime(CLOCK_MONOTONIC, &before_t);
#endif

    /* coverity[returned_null] Ignore */
    compressed = malloc(max);

    *result_len = max;
    rc = BZ2_bzBuffToBuffCompress(compressed, result_len, uncompressed, length, CRM_BZ2_BLOCKS, 0,
                                  CRM_BZ2_WORK);

    free(uncompressed);

    if (rc != BZ_OK) {
        crm_err("Compression of %d bytes failed: %s (%d)", length, bz2_strerror(rc), rc);
        free(compressed);
        return FALSE;
    }

#ifdef CLOCK_MONOTONIC
    clock_gettime(CLOCK_MONOTONIC, &after_t);

    crm_info("Compressed %d bytes into %d (ratio %d:1) in %dms",
             length, *result_len, length / (*result_len),
             (after_t.tv_sec - before_t.tv_sec) * 1000 + (after_t.tv_nsec -
                                                          before_t.tv_nsec) / 1000000);
#else
    crm_info("Compressed %d bytes into %d (ratio %d:1)",
             length, *result_len, length / (*result_len));
#endif

    *result = compressed;
    return TRUE;
}

#ifdef HAVE_GNUTLS_GNUTLS_H
void
crm_gnutls_global_init(void)
{
    signal(SIGPIPE, SIG_IGN);
    gnutls_global_init();
}
#endif

