/*
 * Copyright (C) 2004-2015 Andrew Beekhof <andrew@beekhof.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <crm_internal.h>

#include <sys/param.h>

#include <crm/crm.h>

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/utsname.h>

#include <crm/msg_xml.h>
#include <crm/services.h>
#include <crm/lrmd.h>
#include <crm/common/util.h>
#include <crm/common/xml.h>
#include <crm/common/ipc.h>
#include <crm/common/mainloop.h>

#include <crm/cib/internal.h>
#include <crm/pengine/status.h>
#include <crm/pengine/internal.h>
#include <../lib/pengine/unpack.h>
#include <../pengine/pengine.h>
#include <crm/stonith-ng.h>

void clean_up(int rc);
void crm_diff_update(const char *event, xmlNode * msg);
gboolean mon_refresh_display(gpointer user_data);
int cib_connect(gboolean full);
void mon_st_callback(stonith_t * st, stonith_event_t * e);
static char *get_node_display_name(node_t *node);

/*
 * Definitions indicating which items to print
 */

#define mon_show_times      (0x0001U)
#define mon_show_stack      (0x0002U)
#define mon_show_dc         (0x0004U)
#define mon_show_count      (0x0008U)
#define mon_show_nodes      (0x0010U)
#define mon_show_resources  (0x0020U)
#define mon_show_attributes (0x0040U)
#define mon_show_failcounts (0x0080U)
#define mon_show_operations (0x0100U)
#define mon_show_tickets    (0x0200U)
#define mon_show_bans       (0x0400U)

#define mon_show_headers    (mon_show_times | mon_show_stack | mon_show_dc | mon_show_count)
#define mon_show_default    (mon_show_headers | mon_show_nodes | mon_show_resources)
#define mon_show_all        (mon_show_default | mon_show_attributes | mon_show_failcounts \
                     | mon_show_operations | mon_show_tickets | mon_show_bans)

unsigned int show = mon_show_default;

/*
 * Definitions indicating how to output
 */

enum mon_output_format_e {
    mon_output_none,
    mon_output_monitor,
    mon_output_plain,
    mon_output_console,
    mon_output_xml,
    mon_output_html,
    mon_output_cgi
} output_format = mon_output_console;

char *output_filename = NULL;   /* if sending output to a file, its name */

/* other globals */
char *xml_file = NULL;
char *pid_file = NULL;
char *snmp_target = NULL;
char *snmp_community = NULL;

gboolean group_by_node = FALSE;
gboolean inactive_resources = FALSE;
int reconnect_msec = 5000;
gboolean daemonize = FALSE;
GMainLoop *mainloop = NULL;
guint timer_id = 0;
GList *attr_list = NULL;

const char *crm_mail_host = NULL;
const char *crm_mail_prefix = NULL;
const char *crm_mail_from = NULL;
const char *crm_mail_to = NULL;
const char *external_agent = NULL;
const char *external_recipient = NULL;

cib_t *cib = NULL;
stonith_t *st = NULL;
xmlNode *current_cib = NULL;

gboolean one_shot = FALSE;
gboolean has_warnings = FALSE;
gboolean print_timing = FALSE;
gboolean watch_fencing = FALSE;
gboolean print_brief = FALSE;
gboolean print_pending = FALSE;
gboolean print_clone_detail = FALSE;

/* FIXME allow, detect, and correctly interpret glob pattern or regex? */
const char *print_neg_location_prefix = "";

/* Never display node attributes whose name starts with one of these prefixes */
#define FILTER_STR { "shutdown", "terminate", "standby", "fail-count",  \
                     "last-failure", "probe_complete", "#", NULL }

long last_refresh = 0;
crm_trigger_t *refresh_trigger = NULL;

/*
 * 1.3.6.1.4.1.32723 has been assigned to the project by IANA
 * http://www.iana.org/assignments/enterprise-numbers
 */
#define PACEMAKER_PREFIX "1.3.6.1.4.1.32723"
#define PACEMAKER_TRAP_PREFIX PACEMAKER_PREFIX ".1"

#define snmp_crm_trap_oid   PACEMAKER_TRAP_PREFIX
#define snmp_crm_oid_node   PACEMAKER_TRAP_PREFIX ".1"
#define snmp_crm_oid_rsc    PACEMAKER_TRAP_PREFIX ".2"
#define snmp_crm_oid_task   PACEMAKER_TRAP_PREFIX ".3"
#define snmp_crm_oid_desc   PACEMAKER_TRAP_PREFIX ".4"
#define snmp_crm_oid_status PACEMAKER_TRAP_PREFIX ".5"
#define snmp_crm_oid_rc     PACEMAKER_TRAP_PREFIX ".6"
#define snmp_crm_oid_trc    PACEMAKER_TRAP_PREFIX ".7"

/* Define exit codes for monitoring-compatible output */
#define MON_STATUS_OK   (0)
#define MON_STATUS_WARN (1)

/* Convenience macro for prettifying output (e.g. "node" vs "nodes") */
#define s_if_plural(i) (((i) == 1)? "" : "s")

#if CURSES_ENABLED
#  define print_dot() if (output_format == mon_output_console) { \
	printw(".");				\
	clrtoeol();				\
	refresh();				\
    } else {					\
	fprintf(stdout, ".");			\
    }
#else
#  define print_dot() fprintf(stdout, ".");
#endif

#if CURSES_ENABLED
#  define print_as(fmt, args...) if (output_format == mon_output_console) { \
	printw(fmt, ##args);				\
	clrtoeol();					\
	refresh();					\
    } else {						\
	fprintf(stdout, fmt, ##args);			\
    }
#else
#  define print_as(fmt, args...) fprintf(stdout, fmt, ##args);
#endif

static void
blank_screen(void)
{
#if CURSES_ENABLED
    int lpc = 0;

    for (lpc = 0; lpc < LINES; lpc++) {
        move(lpc, 0);
        clrtoeol();
    }
    move(0, 0);
    refresh();
#endif
}

static gboolean
mon_timer_popped(gpointer data)
{
    int rc = pcmk_ok;

#if CURSES_ENABLED
    if (output_format == mon_output_console) {
        clear();
        refresh();
    }
#endif

    if (timer_id > 0) {
        g_source_remove(timer_id);
    }

    print_as("Reconnecting...\n");
    rc = cib_connect(TRUE);

    if (rc != pcmk_ok) {
        timer_id = g_timeout_add(reconnect_msec, mon_timer_popped, NULL);
    }
    return FALSE;
}

static void
mon_cib_connection_destroy(gpointer user_data)
{
    print_as("Connection to the CIB terminated\n");
    if (cib) {
        cib->cmds->signoff(cib);
        timer_id = g_timeout_add(reconnect_msec, mon_timer_popped, NULL);
    }
    return;
}

/*
 * Mainloop signal handler.
 */
static void
mon_shutdown(int nsig)
{
    clean_up(EX_OK);
}

#if ON_DARWIN
#  define sighandler_t sig_t
#endif

#if CURSES_ENABLED
#  ifndef HAVE_SIGHANDLER_T
typedef void (*sighandler_t) (int);
#  endif
static sighandler_t ncurses_winch_handler;
static void
mon_winresize(int nsig)
{
    static int not_done;
    int lines = 0, cols = 0;

    if (!not_done++) {
        if (ncurses_winch_handler)
            /* the original ncurses WINCH signal handler does the
             * magic of retrieving the new window size;
             * otherwise, we'd have to use ioctl or tgetent */
            (*ncurses_winch_handler) (SIGWINCH);
        getmaxyx(stdscr, lines, cols);
        resizeterm(lines, cols);
        mainloop_set_trigger(refresh_trigger);
    }
    not_done--;
}
#endif

int
cib_connect(gboolean full)
{
    int rc = pcmk_ok;
    static gboolean need_pass = TRUE;

    CRM_CHECK(cib != NULL, return -EINVAL);

    if (getenv("CIB_passwd") != NULL) {
        need_pass = FALSE;
    }

    if (watch_fencing && st == NULL) {
        st = stonith_api_new();
    }

    if (watch_fencing && st->state == stonith_disconnected) {
        crm_trace("Connecting to stonith");
        rc = st->cmds->connect(st, crm_system_name, NULL);
        if (rc == pcmk_ok) {
            crm_trace("Setting up stonith callbacks");
            st->cmds->register_notification(st, T_STONITH_NOTIFY_FENCE, mon_st_callback);
        }
    }

    if (cib->state != cib_connected_query && cib->state != cib_connected_command) {
        crm_trace("Connecting to the CIB");
        if ((output_format == mon_output_console) && need_pass && (cib->variant == cib_remote)) {
            need_pass = FALSE;
            print_as("Password:");
        }

        rc = cib->cmds->signon(cib, crm_system_name, cib_query);

        if (rc != pcmk_ok) {
            return rc;
        }

        rc = cib->cmds->query(cib, NULL, &current_cib, cib_scope_local | cib_sync_call);
        if (rc == pcmk_ok) {
            mon_refresh_display(NULL);
        }

        if (rc == pcmk_ok && full) {
            if (rc == pcmk_ok) {
                rc = cib->cmds->set_connection_dnotify(cib, mon_cib_connection_destroy);
                if (rc == -EPROTONOSUPPORT) {
                    print_as
                        ("Notification setup not supported, won't be able to reconnect after failure");
                    if (output_format == mon_output_console) {
                        sleep(2);
                    }
                    rc = pcmk_ok;
                }

            }

            if (rc == pcmk_ok) {
                cib->cmds->del_notify_callback(cib, T_CIB_DIFF_NOTIFY, crm_diff_update);
                rc = cib->cmds->add_notify_callback(cib, T_CIB_DIFF_NOTIFY, crm_diff_update);
            }

            if (rc != pcmk_ok) {
                print_as("Notification setup failed, could not monitor CIB actions");
                if (output_format == mon_output_console) {
                    sleep(2);
                }
                clean_up(-rc);
            }
        }
    }
    return rc;
}

/* *INDENT-OFF* */
static struct crm_option long_options[] = {
    /* Top-level Options */
    {"help",           0, 0, '?', "\tThis text"},
    {"version",        0, 0, '$', "\tVersion information"  },
    {"verbose",        0, 0, 'V', "\tIncrease debug output"},
    {"quiet",          0, 0, 'Q', "\tDisplay only essential output" },

    {"-spacer-",	1, 0, '-', "\nModes:"},
    {"as-html",        1, 0, 'h', "\tWrite cluster status to the named html file"},
    {"as-xml",         0, 0, 'X', "\t\tWrite cluster status as xml to stdout. This will enable one-shot mode."},
    {"web-cgi",        0, 0, 'w', "\t\tWeb mode with output suitable for cgi"},
    {"simple-status",  0, 0, 's', "\tDisplay the cluster status once as a simple one line output (suitable for nagios)"},
    {"snmp-traps",     1, 0, 'S', "\tSend SNMP traps to this station", !ENABLE_SNMP},
    {"snmp-community", 1, 0, 'C', "Specify community for SNMP traps(default is NULL)", !ENABLE_SNMP},
    {"mail-to",        1, 0, 'T', "\tSend Mail alerts to this user.  See also --mail-from, --mail-host, --mail-prefix", !ENABLE_ESMTP},

    {"-spacer-",	1, 0, '-', "\nDisplay Options:"},
    {"group-by-node",  0, 0, 'n', "\tGroup resources by node"     },
    {"inactive",       0, 0, 'r', "\t\tDisplay inactive resources"  },
    {"failcounts",     0, 0, 'f', "\tDisplay resource fail counts"},
    {"operations",     0, 0, 'o', "\tDisplay resource operation history" },
    {"timing-details", 0, 0, 't', "\tDisplay resource operation history with timing details" },
    {"tickets",        0, 0, 'c', "\t\tDisplay cluster tickets"},
    {"watch-fencing",  0, 0, 'W', "\tListen for fencing events. For use with --external-agent, --mail-to and/or --snmp-traps where supported"},
    {"neg-locations",  2, 0, 'L', "Display negative location constraints [optionally filtered by id prefix]"},
    {"show-node-attributes", 0, 0, 'A', "Display node attributes" },
    {"hide-headers",   0, 0, 'D', "\tHide all headers" },
    {"show-detail",    0, 0, 'R', "\tShow more details (node IDs, individual clone instances)" },
    {"brief",          0, 0, 'b', "\t\tBrief output" },
    {"pending",        0, 0, 'j', "\t\tDisplay pending state if 'record-pending' is enabled" },

    {"-spacer-",	1, 0, '-', "\nAdditional Options:"},
    {"interval",       1, 0, 'i', "\tUpdate frequency in seconds" },
    {"one-shot",       0, 0, '1', "\t\tDisplay the cluster status once on the console and exit"},
    {"disable-ncurses",0, 0, 'N', "\tDisable the use of ncurses", !CURSES_ENABLED},
    {"daemonize",      0, 0, 'd', "\tRun in the background as a daemon"},
    {"pid-file",       1, 0, 'p', "\t(Advanced) Daemon pid file location"},
    {"mail-from",      1, 0, 'F', "\tMail alerts should come from the named user", !ENABLE_ESMTP},
    {"mail-host",      1, 0, 'H', "\tMail alerts should be sent via the named host", !ENABLE_ESMTP},
    {"mail-prefix",    1, 0, 'P', "Subjects for mail alerts should start with this string", !ENABLE_ESMTP},
    {"external-agent",    1, 0, 'E', "A program to run when resource operations take place."},
    {"external-recipient",1, 0, 'e', "A recipient for your program (assuming you want the program to send something to someone)."},


    {"xml-file",       1, 0, 'x', NULL, pcmk_option_hidden},

    {"-spacer-",	1, 0, '-', "\nExamples:", pcmk_option_paragraph},
    {"-spacer-",	1, 0, '-', "Display the cluster status on the console with updates as they occur:", pcmk_option_paragraph},
    {"-spacer-",	1, 0, '-', " crm_mon", pcmk_option_example},
    {"-spacer-",	1, 0, '-', "Display the cluster status on the console just once then exit:", pcmk_option_paragraph},
    {"-spacer-",	1, 0, '-', " crm_mon -1", pcmk_option_example},
    {"-spacer-",	1, 0, '-', "Display your cluster status, group resources by node, and include inactive resources in the list:", pcmk_option_paragraph},
    {"-spacer-",	1, 0, '-', " crm_mon --group-by-node --inactive", pcmk_option_example},
    {"-spacer-",	1, 0, '-', "Start crm_mon as a background daemon and have it write the cluster status to an HTML file:", pcmk_option_paragraph},
    {"-spacer-",	1, 0, '-', " crm_mon --daemonize --as-html /path/to/docroot/filename.html", pcmk_option_example},
    {"-spacer-",	1, 0, '-', "Start crm_mon and export the current cluster status as xml to stdout, then exit.:", pcmk_option_paragraph},
    {"-spacer-",	1, 0, '-', " crm_mon --as-xml", pcmk_option_example},
    {"-spacer-",	1, 0, '-', "Start crm_mon as a background daemon and have it send email alerts:", pcmk_option_paragraph|!ENABLE_ESMTP},
    {"-spacer-",	1, 0, '-', " crm_mon --daemonize --mail-to user@example.com --mail-host mail.example.com", pcmk_option_example|!ENABLE_ESMTP},
    {"-spacer-",	1, 0, '-', "Start crm_mon as a background daemon and have it send SNMP alerts:", pcmk_option_paragraph|!ENABLE_SNMP},
    {"-spacer-",	1, 0, '-', " crm_mon --daemonize --snmp-traps snmptrapd.example.com", pcmk_option_example|!ENABLE_SNMP},

    {NULL, 0, 0, 0}
};
/* *INDENT-ON* */

#if CURSES_ENABLED
static const char *
get_option_desc(char c)
{
    int lpc;

    for (lpc = 0; long_options[lpc].name != NULL; lpc++) {

        if (long_options[lpc].name[0] == '-')
            continue;

        if (long_options[lpc].val == c) {
            const char * tab = NULL;
            tab = strrchr(long_options[lpc].desc, '\t');
            return tab ? ++tab : long_options[lpc].desc;
        }
    }

    return NULL;
}

#define print_option_help(option, condition) \
    print_as("%c %c: \t%s\n", ((condition)? '*': ' '), option, get_option_desc(option));

static gboolean
detect_user_input(GIOChannel *channel, GIOCondition condition, gpointer unused)
{
    int c;
    gboolean config_mode = FALSE;

    while (1) {

        /* Get user input */
        c = getchar();

        switch (c) {
            case 'c':
                show ^= mon_show_tickets;
                break;
            case 'f':
                show ^= mon_show_failcounts;
                break;
            case 'n':
                group_by_node = ! group_by_node;
                break;
            case 'o':
                show ^= mon_show_operations;
                if ((show & mon_show_operations) == 0) {
                    print_timing = 0;
                }
                break;
            case 'r':
                inactive_resources = ! inactive_resources;
                break;
            case 'R':
                print_clone_detail = ! print_clone_detail;
                break;
            case 't':
                print_timing = ! print_timing;
                if (print_timing) {
                    show |= mon_show_operations;
                }
                break;
            case 'A':
                show ^= mon_show_attributes;
                break;
            case 'L':
                show ^= mon_show_bans;
                break;
            case 'D':
                /* If any header is shown, clear them all, otherwise set them all */
                if (show & mon_show_headers) {
                    show &= ~mon_show_headers;
                } else {
                    show |= mon_show_headers;
                }
                break;
            case 'b':
                print_brief = ! print_brief;
                break;
            case 'j':
                print_pending = ! print_pending;
                break;
            case '?':
                config_mode = TRUE;
                break;
            default:
                goto refresh;
        }

        if (!config_mode)
            goto refresh;

        blank_screen();

        print_as("Display option change mode\n");
        print_as("\n");
        print_option_help('c', show & mon_show_tickets);
        print_option_help('f', show & mon_show_failcounts);
        print_option_help('n', group_by_node);
        print_option_help('o', show & mon_show_operations);
        print_option_help('r', inactive_resources);
        print_option_help('t', print_timing);
        print_option_help('A', show & mon_show_attributes);
        print_option_help('L', show & mon_show_bans);
        print_option_help('D', (show & mon_show_headers) == 0);
        print_option_help('R', print_clone_detail);
        print_option_help('b', print_brief);
        print_option_help('j', print_pending);
        print_as("\n");
        print_as("Toggle fields via field letter, type any other key to return");
    }

refresh:
    mon_refresh_display(NULL);
    return TRUE;
}
#endif

int
main(int argc, char **argv)
{
    int flag;
    int argerr = 0;
    int exit_code = 0;
    int option_index = 0;

    pid_file = strdup("/tmp/ClusterMon.pid");
    crm_log_cli_init("crm_mon");
    crm_set_options(NULL, "mode [options]", long_options,
                    "Provides a summary of cluster's current state."
                    "\n\nOutputs varying levels of detail in a number of different formats.\n");

#if !defined (ON_DARWIN) && !defined (ON_BSD)
    /* prevent zombies */
    signal(SIGCLD, SIG_IGN);
#endif

    if (strcmp(crm_system_name, "crm_mon.cgi") == 0) {
        output_format = mon_output_cgi;
        one_shot = TRUE;
    }

    while (1) {
        flag = crm_get_option(argc, argv, &option_index);
        if (flag == -1)
            break;

        switch (flag) {
            case 'V':
                crm_bump_log_level(argc, argv);
                break;
            case 'Q':
                show &= ~mon_show_times;
                break;
            case 'i':
                reconnect_msec = crm_get_msec(optarg);
                break;
            case 'n':
                group_by_node = TRUE;
                break;
            case 'r':
                inactive_resources = TRUE;
                break;
            case 'W':
                watch_fencing = TRUE;
                break;
            case 'd':
                daemonize = TRUE;
                break;
            case 't':
                print_timing = TRUE;
                show |= mon_show_operations;
                break;
            case 'o':
                show |= mon_show_operations;
                break;
            case 'f':
                show |= mon_show_failcounts;
                break;
            case 'A':
                show |= mon_show_attributes;
                break;
            case 'L':
                show |= mon_show_bans;
                print_neg_location_prefix = optarg? optarg : "";
                break;
            case 'D':
                show &= ~mon_show_headers;
                break;
            case 'b':
                print_brief = TRUE;
                break;
            case 'j':
                print_pending = TRUE;
                break;
            case 'R':
                print_clone_detail = TRUE;
                break;
            case 'c':
                show |= mon_show_tickets;
                break;
            case 'p':
                free(pid_file);
                if(optarg == NULL) {
                    return crm_help(flag, EX_USAGE);
                }
                pid_file = strdup(optarg);
                break;
            case 'x':
                if(optarg == NULL) {
                    return crm_help(flag, EX_USAGE);
                }
                xml_file = strdup(optarg);
                one_shot = TRUE;
                break;
            case 'h':
                if(optarg == NULL) {
                    return crm_help(flag, EX_USAGE);
                }
                output_format = mon_output_html;
                output_filename = strdup(optarg);
                umask(S_IWGRP | S_IWOTH);
                break;
            case 'X':
                output_format = mon_output_xml;
                one_shot = TRUE;
                break;
            case 'w':
                output_format = mon_output_cgi;
                one_shot = TRUE;
                break;
            case 's':
                output_format = mon_output_monitor;
                one_shot = TRUE;
                break;
            case 'S':
                snmp_target = optarg;
                break;
            case 'T':
                crm_mail_to = optarg;
                break;
            case 'F':
                crm_mail_from = optarg;
                break;
            case 'H':
                crm_mail_host = optarg;
                break;
            case 'P':
                crm_mail_prefix = optarg;
                break;
            case 'E':
                external_agent = optarg;
                break;
            case 'e':
                external_recipient = optarg;
                break;
            case '1':
                one_shot = TRUE;
                break;
            case 'N':
                if (output_format == mon_output_console) {
                    output_format = mon_output_plain;
                }
                break;
            case 'C':
                snmp_community = optarg;
                break;
            case '$':
            case '?':
                return crm_help(flag, EX_OK);
                break;
            default:
                printf("Argument code 0%o (%c) is not (?yet?) supported\n", flag, flag);
                ++argerr;
                break;
        }
    }

    if (optind < argc) {
        printf("non-option ARGV-elements: ");
        while (optind < argc)
            printf("%s ", argv[optind++]);
        printf("\n");
    }
    if (argerr) {
        return crm_help('?', EX_USAGE);
    }

    /* XML output always prints everything */
    if (output_format == mon_output_xml) {
        show = mon_show_all;
        print_timing = TRUE;
    }

    if (one_shot) {
        if (output_format == mon_output_console) {
            output_format = mon_output_plain;
        }

    } else if (daemonize) {
        if ((output_format == mon_output_console) || (output_format == mon_output_plain)) {
            output_format = mon_output_none;
        }
        crm_enable_stderr(FALSE);

        if ((output_format != mon_output_html) && (output_format != mon_output_xml)
            && !snmp_target && !crm_mail_to && !external_agent) {
            printf
                ("Looks like you forgot to specify one or more of: --as-html, --as-xml, --mail-to, --snmp-target, --external-agent\n");
            return crm_help('?', EX_USAGE);
        }

        crm_make_daemon(crm_system_name, TRUE, pid_file);

    } else if (output_format == mon_output_console) {
#if CURSES_ENABLED
        initscr();
        cbreak();
        noecho();
        crm_enable_stderr(FALSE);
#else
        one_shot = TRUE;
        output_format = mon_output_plain;
        printf("Defaulting to one-shot mode\n");
        printf("You need to have curses available at compile time to enable console mode\n");
#endif
    }

    crm_info("Starting %s", crm_system_name);
    if (xml_file != NULL) {
        current_cib = filename2xml(xml_file);
        mon_refresh_display(NULL);
        return exit_code;
    }

    if (current_cib == NULL) {
        cib = cib_new();

        do {
            if (!one_shot) {
                print_as("Attempting connection to the cluster...\n");
            }
            exit_code = cib_connect(!one_shot);

            if (one_shot) {
                break;

            } else if (exit_code != pcmk_ok) {
                sleep(reconnect_msec / 1000);
#if CURSES_ENABLED
                if (output_format == mon_output_console) {
                    clear();
                    refresh();
                }
#endif
            }

        } while (exit_code == -ENOTCONN);

        if (exit_code != pcmk_ok) {
            if (output_format == mon_output_monitor) {
                printf("CLUSTER WARN: Connection to cluster failed: %s\n", pcmk_strerror(exit_code));
                clean_up(MON_STATUS_WARN);
            } else {
                print_as("\nConnection to cluster failed: %s\n", pcmk_strerror(exit_code));
            }
            if (output_format == mon_output_console) {
                sleep(2);
            }
            clean_up(-exit_code);
        }
    }

    if (one_shot) {
        return exit_code;
    }

    mainloop = g_main_new(FALSE);

    mainloop_add_signal(SIGTERM, mon_shutdown);
    mainloop_add_signal(SIGINT, mon_shutdown);
#if CURSES_ENABLED
    if (output_format == mon_output_console) {
        ncurses_winch_handler = signal(SIGWINCH, mon_winresize);
        if (ncurses_winch_handler == SIG_DFL ||
            ncurses_winch_handler == SIG_IGN || ncurses_winch_handler == SIG_ERR)
            ncurses_winch_handler = NULL;
        g_io_add_watch(g_io_channel_unix_new(STDIN_FILENO), G_IO_IN, detect_user_input, NULL);
    }
#endif
    refresh_trigger = mainloop_add_trigger(G_PRIORITY_LOW, mon_refresh_display, NULL);

    g_main_run(mainloop);
    g_main_destroy(mainloop);

    crm_info("Exiting %s", crm_system_name);

    clean_up(0);
    return 0;                   /* never reached */
}

#define mon_warn(fmt...) do {			\
	if (!has_warnings) {			\
	    print_as("CLUSTER WARN:");		\
	} else {				\
	    print_as(",");			\
	}					\
	print_as(fmt);				\
	has_warnings = TRUE;			\
    } while(0)

static int
count_resources(pe_working_set_t * data_set, resource_t * rsc)
{
    int count = 0;
    GListPtr gIter = NULL;

    if (rsc == NULL) {
        gIter = data_set->resources;
    } else if (rsc->children) {
        gIter = rsc->children;
    } else {
        return is_not_set(rsc->flags, pe_rsc_orphan);
    }

    for (; gIter != NULL; gIter = gIter->next) {
        count += count_resources(data_set, gIter->data);
    }
    return count;
}

/*!
 * \internal
 * \brief Print one-line status suitable for use with monitoring software
 *
 * \param[in] data_set  Working set of CIB state
 *
 * \note This function's output (and the return code when the program exits)
 *       should conform to https://www.monitoring-plugins.org/doc/guidelines.html
 */
static void
print_simple_status(pe_working_set_t * data_set)
{
    GListPtr gIter = NULL;
    int nodes_online = 0;
    int nodes_standby = 0;
    int nodes_maintenance = 0;

    if (data_set->dc_node == NULL) {
        mon_warn(" No DC");
    }

    for (gIter = data_set->nodes; gIter != NULL; gIter = gIter->next) {
        node_t *node = (node_t *) gIter->data;

        if (node->details->standby && node->details->online) {
            nodes_standby++;
        } else if (node->details->maintenance && node->details->online) {
            nodes_maintenance++;
        } else if (node->details->online) {
            nodes_online++;
        } else {
            mon_warn(" offline node: %s", node->details->uname);
        }
    }

    if (!has_warnings) {
        int nresources = count_resources(data_set, NULL);

        print_as("CLUSTER OK: %d node%s online", nodes_online, s_if_plural(nodes_online));
        if (nodes_standby > 0) {
            print_as(", %d standby node%s", nodes_standby, s_if_plural(nodes_standby));
        }
        if (nodes_maintenance > 0) {
            print_as(", %d maintenance node%s", nodes_maintenance, s_if_plural(nodes_maintenance));
        }
        print_as(", %d resource%s configured", nresources, s_if_plural(nresources));
    }

    print_as("\n");
}

/*!
 * \internal
 * \brief Print a [name]=[value][units] pair, optionally using time string
 *
 * \param[in] stream      File stream to display output to
 * \param[in] name        Name to display
 * \param[in] value       Value to display (or NULL to convert time instead)
 * \param[in] units       Units to display (or NULL for no units)
 * \param[in] epoch_time  Epoch time to convert if value is NULL
 */
static void
print_nvpair(FILE *stream, const char *name, const char *value,
             const char *units, time_t epoch_time)
{
    /* print name= */
    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            print_as(" %s=", name);
            break;

        case mon_output_html:
        case mon_output_cgi:
        case mon_output_xml:
            fprintf(stream, " %s=", name);
            break;

        default:
            break;
    }

    /* If we have a value (and optionally units), print it */
    if (value) {
        switch (output_format) {
            case mon_output_plain:
            case mon_output_console:
                print_as("%s%s", value, (units? units : ""));
                break;

            case mon_output_html:
            case mon_output_cgi:
                fprintf(stream, "%s%s", value, (units? units : ""));
                break;

            case mon_output_xml:
                fprintf(stream, "\"%s%s\"", value, (units? units : ""));
                break;

            default:
                break;
        }

    /* Otherwise print user-friendly time string */
    } else {
        char *date_str, *c;

        date_str = asctime(localtime(&epoch_time));
        for (c = date_str; c != '\0'; ++c) {
            if (*c == '\n') {
                *c = '\0';
                break;
            }
        }
        switch (output_format) {
            case mon_output_plain:
            case mon_output_console:
                print_as("'%s'", date_str);
                break;

            case mon_output_html:
            case mon_output_cgi:
            case mon_output_xml:
                fprintf(stream, "\"%s\"", date_str);
                break;

            default:
                break;
        }
    }
}

/*!
 * \internal
 * \brief Print whatever is needed to start a node section
 *
 * \param[in] stream     File stream to display output to
 * \param[in] node       Node to print
 */
static void
print_node_start(FILE *stream, node_t *node)
{
    char *node_name;

    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            node_name = get_node_display_name(node);
            print_as("* Node %s:\n", node_name);
            free(node_name);
            break;

        case mon_output_html:
        case mon_output_cgi:
            node_name = get_node_display_name(node);
            fprintf(stream, "  <h3>Node: %s</h3>\n  <ul>\n", node_name);
            free(node_name);
            break;

        case mon_output_xml:
            fprintf(stream, "        <node name=\"%s\">\n", node->details->uname);
            break;

        default:
            break;
    }
}

/*!
 * \internal
 * \brief Print whatever is needed to end a node section
 *
 * \param[in] stream     File stream to display output to
 */
static void
print_node_end(FILE *stream)
{
    switch (output_format) {
        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, "  </ul>\n");
            break;

        case mon_output_xml:
            fprintf(stream, "        </node>\n");
            break;

        default:
            break;
    }
}

/*!
 * \internal
 * \brief Print heading for resource history
 *
 * \param[in] stream      File stream to display output to
 * \param[in] data_set    Current state of CIB
 * \param[in] node        Node that ran this resource
 * \param[in] rsc         Resource to print
 * \param[in] rsc_id      ID of resource to print
 * \param[in] all         Whether to print every resource or just failed ones
 */
static void
print_rsc_history_start(FILE *stream, pe_working_set_t *data_set, node_t *node,
                        resource_t *rsc, const char *rsc_id, gboolean all)
{
    time_t last_failure = 0;
    int failcount = rsc? get_failcount_full(node, rsc, &last_failure, FALSE, NULL, data_set) : 0;

    if (!all && !failcount && (last_failure <= 0)) {
        return;
    }

    /* Print resource ID */
    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            print_as("   %s:", rsc_id);
            break;

        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, "   <li>%s:", rsc_id);
            break;

        case mon_output_xml:
            fprintf(stream, "            <resource_history id=\"%s\"", rsc_id);
            break;

        default:
            break;
    }

    /* If resource is an orphan, that's all we can say about it */
    if (rsc == NULL) {
        switch (output_format) {
            case mon_output_plain:
            case mon_output_console:
                print_as(" orphan");
                break;

            case mon_output_html:
            case mon_output_cgi:
                fprintf(stream, " orphan");
                break;

            case mon_output_xml:
                fprintf(stream, " orphan=\"true\"");
                break;

            default:
                break;
        }

    /* If resource is not an orphan, print some details */
    } else if (all || failcount || (last_failure > 0)) {

        /* Print migration threshold */
        switch (output_format) {
            case mon_output_plain:
            case mon_output_console:
                print_as(" migration-threshold=%d", rsc->migration_threshold);
                break;

            case mon_output_html:
            case mon_output_cgi:
                fprintf(stream, " migration-threshold=%d", rsc->migration_threshold);
                break;

            case mon_output_xml:
                fprintf(stream, " orphan=\"false\" migration-threshold=\"%d\"",
                        rsc->migration_threshold);
                break;

            default:
                break;
        }

        /* Print fail count if any */
        if (failcount > 0) {
            switch (output_format) {
                case mon_output_plain:
                case mon_output_console:
                    print_as(" fail-count=%d", failcount);
                    break;

                case mon_output_html:
                case mon_output_cgi:
                    fprintf(stream, " fail-count=%d", failcount);
                    break;

                case mon_output_xml:
                    fprintf(stream, " fail-count=\"%d\"", failcount);
                    break;

                default:
                    break;
            }
        }

        /* Print last failure time if any */
        if (last_failure > 0) {
            print_nvpair(stream, "last-failure", NULL, NULL, last_failure);
        }
    }

    /* End the heading */
    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            print_as("\n");
            break;

        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, "\n    <ul>\n");
            break;

        case mon_output_xml:
            fprintf(stream, ">\n");
            break;

        default:
            break;
    }
}

/*!
 * \internal
 * \brief Print closing for resource history
 *
 * \param[in] stream      File stream to display output to
 */
static void
print_rsc_history_end(FILE *stream)
{
    switch (output_format) {
        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, "    </ul>\n   </li>\n");
            break;

        case mon_output_xml:
            fprintf(stream, "            </resource_history>\n");
            break;

        default:
            break;
    }
}

/*!
 * \internal
 * \brief Print operation history
 *
 * \param[in] stream      File stream to display output to
 * \param[in] data_set    Current state of CIB
 * \param[in] node        Node this operation is for
 * \param[in] xml_op      Root of XML tree describing this operation
 * \param[in] task        Task parsed from this operation's XML
 * \param[in] interval    Interval parsed from this operation's XML
 * \param[in] rc          Return code parsed from this operation's XML
 */
static void
print_op_history(FILE *stream, pe_working_set_t *data_set, node_t *node,
                 xmlNode *xml_op, const char *task, const char *interval, int rc)
{
    const char *value = NULL;
    const char *call = crm_element_value(xml_op, XML_LRM_ATTR_CALLID);

    /* Begin the operation description */
    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            print_as("    + (%s) %s:", call, task);
            break;

        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, "     <li>(%s) %s:", call, task);
            break;

        case mon_output_xml:
            fprintf(stream, "                <operation_history call=\"%s\" task=\"%s\"",
                    call, task);
            break;

        default:
            break;
    }

    /* Add name=value pairs as appropriate */
    if (safe_str_neq(interval, "0")) {
        print_nvpair(stream, "interval", interval, "ms", 0);
    }
    if (print_timing) {
        int int_value;
        const char *attr;

        attr = XML_RSC_OP_LAST_CHANGE;
        value = crm_element_value(xml_op, attr);
        if (value) {
            int_value = crm_parse_int(value, NULL);
            if (int_value > 0) {
                print_nvpair(stream, attr, NULL, NULL, int_value);
            }
        }

        attr = XML_RSC_OP_LAST_RUN;
        value = crm_element_value(xml_op, attr);
        if (value) {
            int_value = crm_parse_int(value, NULL);
            if (int_value > 0) {
                print_nvpair(stream, attr, NULL, NULL, int_value);
            }
        }

        attr = XML_RSC_OP_T_EXEC;
        value = crm_element_value(xml_op, attr);
        if (value) {
            print_nvpair(stream, attr, value, "ms", 0);
        }

        attr = XML_RSC_OP_T_QUEUE;
        value = crm_element_value(xml_op, attr);
        if (value) {
            print_nvpair(stream, attr, value, "ms", 0);
        }
    }

    /* End the operation description */
    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            print_as(" rc=%d (%s)\n", rc, services_ocf_exitcode_str(rc));
            break;

        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, " rc=%d (%s)</li>\n", rc, services_ocf_exitcode_str(rc));
            break;

        case mon_output_xml:
            fprintf(stream, " rc=\"%d\" rc_text=\"%s\" />\n", rc, services_ocf_exitcode_str(rc));
            break;

        default:
            break;
    }
}

/*!
 * \internal
 * \brief Print resource operation/failure history
 *
 * \param[in] stream      File stream to display output to
 * \param[in] data_set    Current state of CIB
 * \param[in] node        Node that ran this resource
 * \param[in] rsc_entry   Root of XML tree describing resource status
 * \param[in] operations  Whether to print operations or just failcounts
 */
static void
print_rsc_history(FILE *stream, pe_working_set_t *data_set, node_t *node,
                  xmlNode *rsc_entry, gboolean operations)
{
    GListPtr gIter = NULL;
    GListPtr op_list = NULL;
    gboolean printed = FALSE;
    const char *rsc_id = crm_element_value(rsc_entry, XML_ATTR_ID);
    resource_t *rsc = pe_find_resource(data_set->resources, rsc_id);
    xmlNode *rsc_op = NULL;

    /* If we're not showing operations, just print the resource failure summary */
    if (operations == FALSE) {
        print_rsc_history_start(stream, data_set, node, rsc, rsc_id, FALSE);
        print_rsc_history_end(stream);
        return;
    }

    /* Create a list of this resource's operations */
    for (rsc_op = __xml_first_child(rsc_entry); rsc_op != NULL; rsc_op = __xml_next(rsc_op)) {
        if (crm_str_eq((const char *)rsc_op->name, XML_LRM_TAG_RSC_OP, TRUE)) {
            op_list = g_list_append(op_list, rsc_op);
        }
    }
    op_list = g_list_sort(op_list, sort_op_by_callid);

    /* Print each operation */
    for (gIter = op_list; gIter != NULL; gIter = gIter->next) {
        xmlNode *xml_op = (xmlNode *) gIter->data;
        const char *task = crm_element_value(xml_op, XML_LRM_ATTR_TASK);
        const char *interval = crm_element_value(xml_op, XML_LRM_ATTR_INTERVAL);
        const char *op_rc = crm_element_value(xml_op, XML_LRM_ATTR_RC);
        int rc = crm_parse_int(op_rc, "0");

        /* Display 0-interval monitors as "probe" */
        if (safe_str_eq(task, CRMD_ACTION_STATUS) && safe_str_eq(interval, "0")) {
            task = "probe";
        }

        /* Ignore notifies and some probes */
        if (safe_str_eq(task, CRMD_ACTION_NOTIFY) || (safe_str_eq(task, "probe") && (rc == 7))) {
            continue;
        }

        /* If this is the first printed operation, print heading for resource */
        if (printed == FALSE) {
            printed = TRUE;
            print_rsc_history_start(stream, data_set, node, rsc, rsc_id, TRUE);
        }

        /* Print the operation */
        print_op_history(stream, data_set, node, xml_op, task, interval, rc);
    }

    /* Free the list we created (no need to free the individual items) */
    g_list_free(op_list);

    /* If we printed anything, close the resource */
    if (printed) {
        print_rsc_history_end(stream);
    }
}

/*!
 * \internal
 * \brief Print node operation/failure history
 *
 * \param[in] stream      File stream to display output to
 * \param[in] data_set    Current state of CIB
 * \param[in] node_state  Root of XML tree describing node status
 * \param[in] operations  Whether to print operations or just failcounts
 */
static void
print_node_history(FILE *stream, pe_working_set_t *data_set,
                   xmlNode *node_state, gboolean operations)
{
    node_t *node = pe_find_node_id(data_set->nodes, ID(node_state));
    xmlNode *lrm_rsc = NULL;
    xmlNode *rsc_entry = NULL;

    if (node && node->details && node->details->online) {
        print_node_start(stream, node);

        lrm_rsc = find_xml_node(node_state, XML_CIB_TAG_LRM, FALSE);
        lrm_rsc = find_xml_node(lrm_rsc, XML_LRM_TAG_RESOURCES, FALSE);

        /* Print history of each of the node's resources */
        for (rsc_entry = __xml_first_child(lrm_rsc); rsc_entry != NULL;
             rsc_entry = __xml_next(rsc_entry)) {

            if (crm_str_eq((const char *)rsc_entry->name, XML_LRM_TAG_RESOURCE, TRUE)) {
                print_rsc_history(stream, data_set, node, rsc_entry, operations);
            }
        }

        print_node_end(stream);
    }
}

/*!
 * \internal
 * \brief Print extended information about an attribute if appropriate
 *
 * \param[in] data_set  Working set of CIB state
 *
 * \return TRUE if extended information was printed, FALSE otherwise
 * \note Currently, extended information is only supported for ping/pingd
 *       resources, for which a message will be printed if connectivity is lost
 *       or degraded.
 */
static gboolean
print_attr_msg(FILE *stream, node_t * node, GListPtr rsc_list, const char *attrname, const char *attrvalue)
{
    GListPtr gIter = NULL;

    for (gIter = rsc_list; gIter != NULL; gIter = gIter->next) {
        resource_t *rsc = (resource_t *) gIter->data;
        const char *type = g_hash_table_lookup(rsc->meta, "type");

        if (rsc->children != NULL) {
            if (print_attr_msg(stream, node, rsc->children, attrname, attrvalue)) {
                return TRUE;
            }
        }

        if (safe_str_eq(type, "ping") || safe_str_eq(type, "pingd")) {
            const char *name = g_hash_table_lookup(rsc->parameters, "name");

            if (name == NULL) {
                name = "pingd";
            }

            /* To identify the resource with the attribute name. */
            if (safe_str_eq(name, attrname)) {
                int host_list_num = 0;
                int expected_score = 0;
                int value = crm_parse_int(attrvalue, "0");
                const char *hosts = g_hash_table_lookup(rsc->parameters, "host_list");
                const char *multiplier = g_hash_table_lookup(rsc->parameters, "multiplier");

                if(hosts) {
                    char **host_list = g_strsplit(hosts, " ", 0);
                    host_list_num = g_strv_length(host_list);
                    g_strfreev(host_list);
                }

                /* pingd multiplier is the same as the default value. */
                expected_score = host_list_num * crm_parse_int(multiplier, "1");

                switch (output_format) {
                    case mon_output_plain:
                    case mon_output_console:
                        if (value <= 0) {
                            print_as("\t: Connectivity is lost");
                        } else if (value < expected_score) {
                            print_as("\t: Connectivity is degraded (Expected=%d)", expected_score);
                        }
                        break;

                    case mon_output_html:
                    case mon_output_cgi:
                        if (value <= 0) {
                            fprintf(stream, " <b>(connectivity is lost)</b>");
                        } else if (value < expected_score) {
                            fprintf(stream, " <b>(connectivity is degraded -- expected %d)</b>",
                                    expected_score);
                        }
                        break;

                    case mon_output_xml:
                        fprintf(stream, " expected=\"%d\"", expected_score);
                        break;

                    default:
                        break;
                }
                return TRUE;
            }
        }
    }
    return FALSE;
}

static int
compare_attribute(gconstpointer a, gconstpointer b)
{
    int rc;

    rc = strcmp((const char *)a, (const char *)b);

    return rc;
}

static void
create_attr_list(gpointer name, gpointer value, gpointer data)
{
    int i;
    const char *filt_str[] = FILTER_STR;

    CRM_CHECK(name != NULL, return);

    /* filtering automatic attributes */
    for (i = 0; filt_str[i] != NULL; i++) {
        if (g_str_has_prefix(name, filt_str[i])) {
            return;
        }
    }

    attr_list = g_list_insert_sorted(attr_list, name, compare_attribute);
}

/* structure for passing multiple user data to g_list_foreach() */
struct mon_attr_data {
    FILE *stream;
    node_t *node;
};

static void
print_node_attribute(gpointer name, gpointer user_data)
{
    const char *value = NULL;
    struct mon_attr_data *data = (struct mon_attr_data *) user_data;

    value = g_hash_table_lookup(data->node->details->attrs, name);

    /* Print attribute name and value */
    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            print_as("    + %-32s\t: %-10s", (char *)name, value);
            break;

        case mon_output_html:
        case mon_output_cgi:
            fprintf(data->stream, "   <li>%s: %s",
                    (char *)name, value);
            break;

        case mon_output_xml:
            fprintf(data->stream,
                    "            <attribute name=\"%s\" value=\"%s\"",
                    (char *)name, value);
            break;

        default:
            break;
    }

    /* Print extended information if appropriate */
    print_attr_msg(data->stream, data->node, data->node->details->running_rsc,
                   name, value);

    /* Close out the attribute */
    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            print_as("\n");
            break;

        case mon_output_html:
        case mon_output_cgi:
            fprintf(data->stream, "</li>\n");
            break;

        case mon_output_xml:
            fprintf(data->stream, " />\n");
            break;

        default:
            break;
    }
}

static void
print_node_summary(FILE *stream, pe_working_set_t * data_set, gboolean operations)
{
    xmlNode *node_state = NULL;
    xmlNode *cib_status = get_object_root(XML_CIB_TAG_STATUS, data_set->input);

    /* Print heading */
    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            if (operations) {
                print_as("\nOperations:\n");
            } else {
                print_as("\nMigration Summary:\n");
            }
            break;

        case mon_output_html:
        case mon_output_cgi:
            if (operations) {
                fprintf(stream, " <hr />\n <h2>Operations</h2>\n");
            } else {
                fprintf(stream, " <hr />\n <h2>Migration Summary</h2>\n");
            }
            break;

        case mon_output_xml:
            fprintf(stream, "    <node_history>\n");
            break;

        default:
            break;
    }

    /* Print each node in the CIB status */
    for (node_state = __xml_first_child(cib_status); node_state != NULL;
         node_state = __xml_next(node_state)) {
        if (crm_str_eq((const char *)node_state->name, XML_CIB_TAG_STATE, TRUE)) {
            print_node_history(stream, data_set, node_state, operations);
        }
    }

    /* Close section */
    switch (output_format) {
        case mon_output_xml:
            fprintf(stream, "    </node_history>\n");
            break;

        default:
            break;
    }
}

static void
print_ticket(gpointer name, gpointer value, gpointer data)
{
    ticket_t *ticket = (ticket_t *) value;
    FILE *stream = (FILE *) data;

    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            print_as("* %s:\t%s%s", ticket->id,
                     (ticket->granted? "granted" : "revoked"),
                     (ticket->standby? " [standby]" : ""));
            break;

        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, "  <li>%s: %s%s", ticket->id,
                    (ticket->granted? "granted" : "revoked"),
                    (ticket->standby? " [standby]" : ""));
            break;

        case mon_output_xml:
            fprintf(stream, "        <ticket id=\"%s\" status=\"%s\" standby=\"%s\"",
                    ticket->id, (ticket->granted? "granted" : "revoked"),
                    (ticket->standby? "true" : "false"));
            break;

        default:
            break;
    }
    if (ticket->last_granted > -1) {
        print_nvpair(stdout, "last-granted", NULL, NULL, ticket->last_granted);
    }
    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            print_as("\n");
            break;

        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, "</li>\n");
            break;

        case mon_output_xml:
            fprintf(stream, " />\n");
            break;

        default:
            break;
    }
}

static void
print_cluster_tickets(FILE *stream, pe_working_set_t * data_set)
{
    /* Print section heading */
    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            print_as("\nTickets:\n");
            break;

        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, " <hr />\n <h2>Tickets</h2>\n <ul>\n");
            break;

        case mon_output_xml:
            fprintf(stream, "    <tickets>\n");
            break;

        default:
            break;
    }

    /* Print each ticket */
    g_hash_table_foreach(data_set->tickets, print_ticket, stream);

    /* Close section */
    switch (output_format) {
        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, " </ul>\n");
            break;

        case mon_output_xml:
            fprintf(stream, "    </tickets>\n");
            break;

        default:
            break;
    }
}

/*!
 * \internal
 * \brief Return human-friendly string representing node name
 *
 * The returned string will be in the format
 *    uname[@hostUname] [(nodeID)]
 * "@hostUname" will be printed if the node is a guest node.
 * "(nodeID)" will be printed if the node ID is different from the node uname,
 *  and detailed output has been requested.
 *
 * \param[in] node  Node to represent
 * \return Newly allocated string with representation of node name
 * \note It is the caller's responsibility to free the result with free().
 */
static char *
get_node_display_name(node_t *node)
{
    char *node_name;
    const char *node_host = NULL;
    const char *node_id = NULL;
    int name_len;

    CRM_ASSERT((node != NULL) && (node->details != NULL) && (node->details->uname != NULL));

    /* Host is displayed only if this is a guest node */
    if (is_container_remote_node(node)) {
        if (node->details->remote_rsc->running_on) {
            /* running_on is a list, but guest nodes will have exactly one entry
             * unless they are in the process of migrating, in which case they
             * will have two; either way, we can use the first item in the list
             */
            node_t *host_node = (node_t *) node->details->remote_rsc->running_on->data;

            if (host_node && host_node->details) {
                node_host = host_node->details->uname;
            }
        }
        if (node_host == NULL) {
            node_host = ""; /* so we at least get "uname@" to indicate guest */
        }
    }

    /* Node ID is displayed if different from uname and detail is requested */
    if (print_clone_detail && safe_str_neq(node->details->uname, node->details->id)) {
        node_id = node->details->id;
    }

    /* Determine name length */
    name_len = strlen(node->details->uname) + 1;
    if (node_host) {
        name_len += strlen(node_host) + 1; /* "@node_host" */
    }
    if (node_id) {
        name_len += strlen(node_id) + 3; /* + " (node_id)" */
    }

    /* Allocate and populate display name */
    node_name = malloc(name_len);
    CRM_ASSERT(node_name != NULL);
    strcpy(node_name, node->details->uname);
    if (node_host) {
        strcat(node_name, "@");
        strcat(node_name, node_host);
    }
    if (node_id) {
        strcat(node_name, " (");
        strcat(node_name, node_id);
        strcat(node_name, ")");
    }
    return node_name;
}

/*!
 * \internal
 * \brief Print a negative location constraint
 *
 * \param[in] stream     File stream to display output to
 * \param[in] node       Node affected by constraint
 * \param[in] location   Constraint to print
 */
static void print_ban(FILE *stream, node_t *node, rsc_to_node_t *location)
{
    char *node_name = NULL;

    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            node_name = get_node_display_name(node);
            print_as(" %s\tprevents %s from running %son %s\n",
                     location->id, location->rsc_lh->id,
                     ((location->role_filter == RSC_ROLE_MASTER)? "as Master " : ""),
                     node_name);
            break;

        case mon_output_html:
        case mon_output_cgi:
            node_name = get_node_display_name(node);
            fprintf(stream, "  <li>%s prevents %s from running %son %s</li>\n",
                     location->id, location->rsc_lh->id,
                     ((location->role_filter == RSC_ROLE_MASTER)? "as Master " : ""),
                     node_name);
            break;

        case mon_output_xml:
            fprintf(stream,
                    "        <ban id=\"%s\" resource=\"%s\" node=\"%s\" weight=\"%d\" master_only=\"%s\" />\n",
                    location->id, location->rsc_lh->id, node->details->uname, node->weight,
                    ((location->role_filter == RSC_ROLE_MASTER)? "true" : "false"));
            break;

        default:
            break;
    }
    free(node_name);
}

/*!
 * \internal
 * \brief Print section for negative location constraints
 *
 * \param[in] stream     File stream to display output to
 * \param[in] data_set   Working set corresponding to CIB status to display
 */
static void print_neg_locations(FILE *stream, pe_working_set_t *data_set)
{
    GListPtr gIter, gIter2;

    /* Print section heading */
    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            print_as("\nNegative Location Constraints:\n");
            break;

        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, " <hr />\n <h2>Negative Location Constraints</h2>\n <ul>\n");
            break;

        case mon_output_xml:
            fprintf(stream, "    <bans>\n");
            break;

        default:
            break;
    }

    /* Print each ban */
    for (gIter = data_set->placement_constraints; gIter != NULL; gIter = gIter->next) {
        rsc_to_node_t *location = (rsc_to_node_t *) gIter->data;
        if (!g_str_has_prefix(location->id, print_neg_location_prefix))
            continue;
        for (gIter2 = location->node_list_rh; gIter2 != NULL; gIter2 = gIter2->next) {
            node_t *node = (node_t *) gIter2->data;

            if (node->weight < 0) {
                print_ban(stream, node, location);
            }
        }
    }

    /* Close section */
    switch (output_format) {
        case mon_output_cgi:
        case mon_output_html:
            fprintf(stream, " </ul>\n");
            break;

        case mon_output_xml:
            fprintf(stream, "    </bans>\n");
            break;

        default:
            break;
    }
}

static void
crm_mon_get_parameters(resource_t *rsc, pe_working_set_t * data_set)
{
    get_rsc_attributes(rsc->parameters, rsc, NULL, data_set);
    crm_trace("Beekhof: unpacked params for %s (%d)", rsc->id, g_hash_table_size(rsc->parameters));
    if(rsc->children) {
        GListPtr gIter = NULL;

        for (gIter = rsc->children; gIter != NULL; gIter = gIter->next) {
            crm_mon_get_parameters(gIter->data, data_set);
        }
    }
}

/*!
 * \internal
 * \brief Print node attributes section
 *
 * \param[in] stream     File stream to display output to
 * \param[in] data_set   Working set of CIB state
 */
static void
print_node_attributes(FILE *stream, pe_working_set_t *data_set)
{
    GListPtr gIter = NULL;

    /* Print section heading */
    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            print_as("\nNode Attributes:\n");
            break;

        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, " <hr />\n <h2>Node Attributes</h2>\n");
            break;

        case mon_output_xml:
            fprintf(stream, "    <node_attributes>\n");
            break;

        default:
            break;
    }

    /* Unpack all resource parameters (it would be more efficient to do this
     * only when needed for the first time in print_attr_msg())
     */
    for (gIter = data_set->resources; gIter != NULL; gIter = gIter->next) {
        crm_mon_get_parameters(gIter->data, data_set);
    }

    /* Display each node's attributes */
    for (gIter = data_set->nodes; gIter != NULL; gIter = gIter->next) {
        struct mon_attr_data data;

        data.stream = stream;
        data.node = (node_t *) gIter->data;

        if (data.node && data.node->details && data.node->details->online) {
            print_node_start(stream, data.node);
            g_hash_table_foreach(data.node->details->attrs, create_attr_list, NULL);
            g_list_foreach(attr_list, print_node_attribute, &data);
            g_list_free(attr_list);
            attr_list = NULL;
            print_node_end(stream);
        }
    }

    /* Print section footer */
    switch (output_format) {
        case mon_output_xml:
            fprintf(stream, "    </node_attributes>\n");
            break;

        default:
            break;
    }
}

/*!
 * \internal
 * \brief Return resource display options corresponding to command-line choices
 *
 * \return Bitmask of pe_print_options suitable for resource print functions
 */
static int
get_resource_display_options(void)
{
    int print_opts;

    /* Determine basic output format */
    switch (output_format) {
        case mon_output_console:
            print_opts = pe_print_ncurses;
            break;
        case mon_output_html:
        case mon_output_cgi:
            print_opts = pe_print_html;
            break;
        case mon_output_xml:
            print_opts = pe_print_xml;
            break;
        default:
            print_opts = pe_print_printf;
            break;
    }

    /* Add optional display elements */
    if (print_pending) {
        print_opts |= pe_print_pending;
    }
    if (print_clone_detail) {
        print_opts |= pe_print_clone_details;
    }
    if (!inactive_resources) {
        print_opts |= pe_print_clone_active;
    }
    if (print_brief) {
        print_opts |= pe_print_brief;
    }
    return print_opts;
}

/*!
 * \internal
 * \brief Return human-friendly string representing current time
 *
 * \return Current time as string (as by ctime() but without newline) on success
 *         or "Could not determine current time" on error
 * \note The return value points to a statically allocated string which might be
 *       overwritten by subsequent calls to any of the C library date and time functions.
 */
static const char *
crm_now_string(void)
{
    time_t a_time = time(NULL);
    char *since_epoch = ctime(&a_time);

    if ((a_time == (time_t) -1) || (since_epoch == NULL)) {
        return "Could not determine current time";
    }
    since_epoch[strlen(since_epoch) - 1] = EOS; /* trim newline */
    return (since_epoch);
}

/*!
 * \internal
 * \brief Print header for cluster summary if needed
 *
 * \param[in] stream     File stream to display output to
 */
static void
print_cluster_summary_header(FILE *stream)
{
    switch (output_format) {
        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, " <h2>Cluster Summary</h2>\n <p>\n");
            break;

        case mon_output_xml:
            fprintf(stream, "    <summary>\n");
            break;

        default:
            break;
    }
}

/*!
 * \internal
 * \brief Print footer for cluster summary if needed
 *
 * \param[in] stream     File stream to display output to
 */
static void
print_cluster_summary_footer(FILE *stream)
{
    switch (output_format) {
        case mon_output_cgi:
        case mon_output_html:
            fprintf(stream, " </p>\n");
            break;

        case mon_output_xml:
            fprintf(stream, "    </summary>\n");
            break;

        default:
            break;
    }
}

/*!
 * \internal
 * \brief Print times the display was last updated and CIB last changed
 *
 * \param[in] stream     File stream to display output to
 * \param[in] data_set   Working set of CIB state
 */
static void
print_cluster_times(FILE *stream, pe_working_set_t *data_set)
{
    const char *last_written = crm_element_value(data_set->input, XML_CIB_ATTR_WRITTEN);
    const char *user = crm_element_value(data_set->input, XML_ATTR_UPDATE_USER);
    const char *client = crm_element_value(data_set->input, XML_ATTR_UPDATE_CLIENT);
    const char *origin = crm_element_value(data_set->input, XML_ATTR_UPDATE_ORIG);

    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            print_as("Last updated: %s", crm_now_string());
            print_as("\t\tLast change: %s", last_written ? last_written : "");
            if (user) {
                print_as(" by %s", user);
            }
            if (client) {
                print_as(" via %s", client);
            }
            if (origin) {
                print_as(" on %s", origin);
            }
            print_as("\n");
            break;

        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, " <b>Last updated: %s</b><br/>\n", crm_now_string());
            fprintf(stream, " <b>Last change:</b> %s", last_written ? last_written : "");
            if (user) {
                fprintf(stream, " by %s", user);
            }
            if (client) {
                fprintf(stream, " via %s", client);
            }
            if (origin) {
                fprintf(stream, " on %s", origin);
            }
            fprintf(stream, "<br/>\n");
            break;

        case mon_output_xml:
            fprintf(stream, "        <last_update time=\"%s\" />\n", crm_now_string());
            fprintf(stream, "        <last_change time=\"%s\" user=\"%s\" client=\"%s\" origin=\"%s\" />\n",
                    last_written ? last_written : "", user ? user : "",
                    client ? client : "", origin ? origin : "");
            break;

        default:
            break;
    }
}

/*!
 * \internal
 * \brief Print cluster stack
 *
 * \param[in] stream     File stream to display output to
 * \param[in] stack_s    Stack name
 */
static void
print_cluster_stack(FILE *stream, const char *stack_s)
{
    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            print_as("Stack: %s\n", stack_s);
            break;

        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, " <b>Stack:</b> %s<br/>\n", stack_s);
            break;

        case mon_output_xml:
            fprintf(stream, "        <stack type=\"%s\" />\n", stack_s);
            break;

        default:
            break;
    }
}

/*!
 * \internal
 * \brief Print current DC and its version
 *
 * \param[in] stream     File stream to display output to
 * \param[in] data_set   Working set of CIB state
 */
static void
print_cluster_dc(FILE *stream, pe_working_set_t *data_set)
{
    node_t *dc = data_set->dc_node;
    xmlNode *dc_version = get_xpath_object("//nvpair[@name='dc-version']",
                                           data_set->input, LOG_DEBUG);
    const char *dc_version_s = dc_version?
                               crm_element_value(dc_version, XML_NVPAIR_ATTR_VALUE)
                               : NULL;
    const char *quorum = crm_element_value(data_set->input, XML_ATTR_HAVE_QUORUM);
    char *dc_name = dc? get_node_display_name(dc) : NULL;

    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            print_as("Current DC: ");
            if (dc) {
                print_as("%s (version %s) - partition %s quorum\n",
                         dc_name, (dc_version_s? dc_version_s : "unknown"),
                         (crm_is_true(quorum) ? "with" : "WITHOUT"));
            } else {
                print_as("NONE\n");
            }
            break;

        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, " <b>Current DC:</b> ");
            if (dc) {
                fprintf(stream, "%s (version %s) - partition %s quorum",
                        dc_name, (dc_version_s? dc_version_s : "unknown"),
                        (crm_is_true(quorum)? "with" : "<font color=\"red\"><b>WITHOUT</b></font>"));
            } else {
                fprintf(stream, "<font color=\"red\"><b>NONE</b></font>");
            }
            fprintf(stream, "<br/>\n");
            break;

        case mon_output_xml:
            fprintf(stream,  "        <current_dc ");
            if (dc) {
                fprintf(stream,
                        "present=\"true\" version=\"%s\" name=\"%s\" id=\"%s\" with_quorum=\"%s\"",
                        (dc_version_s? dc_version_s : ""), dc->details->uname, dc->details->id,
                        (crm_is_true(quorum) ? "true" : "false"));
            } else {
                fprintf(stream, "present=\"false\"");
            }
            fprintf(stream, " />\n");
            break;

        default:
            break;
    }
    free(dc_name);
}

/*!
 * \internal
 * \brief Print counts of configured nodes and resources
 *
 * \param[in] stream     File stream to display output to
 * \param[in] data_set   Working set of CIB state
 * \param[in] stack_s    Stack name
 */
static void
print_cluster_counts(FILE *stream, pe_working_set_t *data_set, const char *stack_s)
{
    int nnodes = g_list_length(data_set->nodes);
    int nresources = count_resources(data_set, NULL);
    xmlNode *quorum_node = get_xpath_object("//nvpair[@name='" XML_ATTR_EXPECTED_VOTES "']",
                                            data_set->input, LOG_DEBUG);
    const char *quorum_votes = quorum_node?
                               crm_element_value(quorum_node, XML_NVPAIR_ATTR_VALUE)
                               : "unknown";

    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            print_as("%d node%s and %d resource%s configured",
                     nnodes, s_if_plural(nnodes),
                     nresources, s_if_plural(nresources));
            if (stack_s && strstr(stack_s, "classic openais") != NULL) {
                print_as(", %s expected votes", quorum_votes);
            }
            print_as("\n\n");
            break;

        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, " %d node%s configured", nnodes, s_if_plural(nnodes));
            if (stack_s && strstr(stack_s, "classic openais") != NULL) {
                fprintf(stream, " (%s expected votes)", quorum_votes);
            }
            fprintf(stream, "<br/>\n");
            fprintf(stream, " %d resource%s configured<br/>\n",
                    nresources, s_if_plural(nresources));
            break;

        case mon_output_xml:
            fprintf(stream,
                    "        <nodes_configured number=\"%d\" expected_votes=\"%s\" />\n",
                    g_list_length(data_set->nodes), quorum_votes);
            fprintf(stream,
                    "        <resources_configured number=\"%d\" />\n",
                    count_resources(data_set, NULL));
            break;

        default:
            break;
    }
}

/*!
 * \internal
 * \brief Print cluster-wide options
 *
 * \param[in] stream     File stream to display output to
 * \param[in] data_set   Working set of CIB state
 *
 * \note Currently this is only implemented for HTML and XML output, and
 *       prints only a few options. If there is demand, more could be added.
 */
static void
print_cluster_options(FILE *stream, pe_working_set_t *data_set)
{
    switch (output_format) {
        case mon_output_html:
            fprintf(stream, " </p>\n <h3>Config Options</h3>\n");
            fprintf(stream, " <table>\n");
            fprintf(stream, "  <tr><th>STONITH of failed nodes</th><td>%s</td></tr>\n",
                    is_set(data_set->flags, pe_flag_stonith_enabled)? "enabled" : "disabled");

            fprintf(stream, "  <tr><th>Cluster is</th><td>%ssymmetric</td></tr>\n",
                    is_set(data_set->flags, pe_flag_symmetric_cluster)? "" : "a");

            fprintf(stream, "  <tr><th>No Quorum Policy</th><td>");
            switch (data_set->no_quorum_policy) {
                case no_quorum_freeze:
                    fprintf(stream, "Freeze resources");
                    break;
                case no_quorum_stop:
                    fprintf(stream, "Stop ALL resources");
                    break;
                case no_quorum_ignore:
                    fprintf(stream, "Ignore");
                    break;
                case no_quorum_suicide:
                    fprintf(stream, "Suicide");
                    break;
            }
            fprintf(stream, "</td></tr>\n </table>\n <p>\n");
            break;

        case mon_output_xml:
            fprintf(stream, "        <cluster_options");
            fprintf(stream, " stonith-enabled=\"%s\"",
                    is_set(data_set->flags, pe_flag_stonith_enabled)?
                    "true" : "false");
            fprintf(stream, " symmetric-cluster=\"%s\"",
                    is_set(data_set->flags, pe_flag_symmetric_cluster)?
                    "true" : "false");
            fprintf(stream, " no-quorum-policy=\"");
            switch (data_set->no_quorum_policy) {
                case no_quorum_freeze:
                    fprintf(stream, "freeze");
                    break;
                case no_quorum_stop:
                    fprintf(stream, "stop");
                    break;
                case no_quorum_ignore:
                    fprintf(stream, "ignore");
                    break;
                case no_quorum_suicide:
                    fprintf(stream, "suicide");
                    break;
            }
            fprintf(stream, "\" />\n");
            break;

        default:
            break;
    }
}

/*!
 * \internal
 * \brief Get the name of the stack in use (or "unknown" if not available)
 *
 * \param[in] data_set   Working set of CIB state
 *
 * \return String representing stack name
 */
static const char *
get_cluster_stack(pe_working_set_t *data_set)
{
    xmlNode *stack = get_xpath_object("//nvpair[@name='cluster-infrastructure']",
                                      data_set->input, LOG_DEBUG);
    return stack? crm_element_value(stack, XML_NVPAIR_ATTR_VALUE) : "unknown";
}

/*!
 * \internal
 * \brief Print a summary of cluster-wide information
 *
 * \param[in] stream     File stream to display output to
 * \param[in] data_set   Working set of CIB state
 */
static void
print_cluster_summary(FILE *stream, pe_working_set_t *data_set)
{
    const char *stack_s = get_cluster_stack(data_set);
    gboolean header_printed = FALSE;

    if (show & mon_show_times) {
        if (header_printed == FALSE) {
            print_cluster_summary_header(stream);
            header_printed = TRUE;
        }
        print_cluster_times(stream, data_set);
    }

    if (show & mon_show_stack) {
        if (header_printed == FALSE) {
            print_cluster_summary_header(stream);
            header_printed = TRUE;
        }
        print_cluster_stack(stream, stack_s);
    }

    /* Always print DC if none, even if not requested */
    if ((data_set->dc_node == NULL) || (show & mon_show_dc)) {
        if (header_printed == FALSE) {
            print_cluster_summary_header(stream);
            header_printed = TRUE;
        }
        print_cluster_dc(stream, data_set);
    }

    if (show & mon_show_count) {
        if (header_printed == FALSE) {
            print_cluster_summary_header(stream);
            header_printed = TRUE;
        }
        print_cluster_counts(stream, data_set, stack_s);
    }

    /* There is not a separate option for showing cluster options, so show with
     * stack for now; a separate option could be added if there is demand
     */
    if (show & mon_show_stack) {
        print_cluster_options(stream, data_set);
    }

    if (header_printed) {
        print_cluster_summary_footer(stream);
    }
}

/*!
 * \internal
 * \brief Print a failed action
 *
 * \param[in] stream     File stream to display output to
 * \param[in] xml_op     Root of XML tree describing failed action
 */
static void
print_failed_action(FILE *stream, xmlNode *xml_op)
{
    const char *op_key = crm_element_value(xml_op, XML_LRM_ATTR_TASK_KEY);
    const char *op_key_attr = "op_key";
    const char *last = crm_element_value(xml_op, XML_RSC_OP_LAST_CHANGE);
    const char *node = crm_element_value(xml_op, XML_ATTR_UNAME);
    const char *call = crm_element_value(xml_op, XML_LRM_ATTR_CALLID);
    const char *exit_reason = crm_element_value(xml_op, XML_LRM_ATTR_EXIT_REASON);
    int rc = crm_parse_int(crm_element_value(xml_op, XML_LRM_ATTR_RC), "0");
    int status = crm_parse_int(crm_element_value(xml_op, XML_LRM_ATTR_OPSTATUS), "0");
    char *exit_reason_cleaned;

    /* If no op_key was given, use id instead */
    if (op_key == NULL) {
        op_key = ID(xml_op);
        op_key_attr = "id";
    }

    /* If no exit reason was given, use "none" */
    if (exit_reason == NULL) {
        exit_reason = "none";
    }

    /* Print common action information */
    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            print_as("* %s on %s '%s' (%d): call=%s, status=%s, exitreason='%s'",
                     op_key, node, services_ocf_exitcode_str(rc), rc,
                     call, services_lrm_status_str(status), exit_reason);
            break;

        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, "  <li>%s on %s '%s' (%d): call=%s, status=%s, exitreason='%s'",
                     op_key, node, services_ocf_exitcode_str(rc), rc,
                     call, services_lrm_status_str(status), exit_reason);
            break;

        case mon_output_xml:
            exit_reason_cleaned = crm_xml_escape(exit_reason);
            fprintf(stream, "        <failure %s=\"%s\" node=\"%s\"",
                    op_key_attr, op_key, node);
            fprintf(stream, " exitstatus=\"%s\" exitreason=\"%s\" exitcode=\"%d\"",
                    services_ocf_exitcode_str(rc), exit_reason_cleaned, rc);
            fprintf(stream, " call=\"%s\" status=\"%s\"",
                    call, services_lrm_status_str(status));
            free(exit_reason_cleaned);
            break;

        default:
            break;
    }

    /* If last change was given, print timing information as well */
    if (last) {
        time_t run_at = crm_parse_int(last, "0");
        char *run_at_s = ctime(&run_at);

        if (run_at_s) {
            run_at_s[24] = 0; /* Overwrite the newline */
        }

        switch (output_format) {
            case mon_output_plain:
            case mon_output_console:
                print_as(",\n    last-rc-change='%s', queued=%sms, exec=%sms",
                         run_at_s? run_at_s : "",
                         crm_element_value(xml_op, XML_RSC_OP_T_QUEUE),
                         crm_element_value(xml_op, XML_RSC_OP_T_EXEC));
                break;

            case mon_output_html:
            case mon_output_cgi:
                fprintf(stream, " last-rc-change='%s', queued=%sms, exec=%sms",
                        run_at_s? run_at_s : "",
                        crm_element_value(xml_op, XML_RSC_OP_T_QUEUE),
                        crm_element_value(xml_op, XML_RSC_OP_T_EXEC));
                break;

            case mon_output_xml:
                fprintf(stream,
                        " last-rc-change=\"%s\" queued=\"%s\" exec=\"%s\" interval=\"%d\" task=\"%s\"",
                        run_at_s? run_at_s : "",
                        crm_element_value(xml_op, XML_RSC_OP_T_QUEUE),
                        crm_element_value(xml_op, XML_RSC_OP_T_EXEC),
                        crm_parse_int(crm_element_value(xml_op, XML_LRM_ATTR_INTERVAL), "0"),
                        crm_element_value(xml_op, XML_LRM_ATTR_TASK));
                break;

            default:
                break;
        }
    }

    /* End the action listing */
    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            print_as("\n");
            break;

        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, "</li>\n");
            break;

        case mon_output_xml:
            fprintf(stream, " />\n");
            break;

        default:
            break;
    }
}

/*!
 * \internal
 * \brief Print a section for failed actions
 *
 * \param[in] stream     File stream to display output to
 * \param[in] data_set   Working set of CIB state
 */
static void
print_failed_actions(FILE *stream, pe_working_set_t *data_set)
{
    xmlNode *xml_op = NULL;

    /* Print section heading */
    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            print_as("\nFailed Actions:\n");
            break;

        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, " <hr />\n <h2>Failed Actions</h2>\n <ul>\n");
            break;

        case mon_output_xml:
            fprintf(stream, "    <failures>\n");
            break;

        default:
            break;
    }

    /* Print each failed action */
    for (xml_op = __xml_first_child(data_set->failed); xml_op != NULL;
         xml_op = __xml_next(xml_op)) {
        print_failed_action(stream, xml_op);
    }

    /* End section */
    switch (output_format) {
        case mon_output_plain:
        case mon_output_console:
            print_as("\n");
            break;

        case mon_output_html:
        case mon_output_cgi:
            fprintf(stream, " </ul>\n");
            break;

        case mon_output_xml:
            fprintf(stream, "    </failures>\n");
            break;

        default:
            break;
    }
}

/*!
 * \internal
 * \brief Print cluster status to screen
 *
 * This uses the global display preferences set by command-line options
 * to display cluster status in a human-friendly way.
 *
 * \param[in] data_set   Working set of CIB state
 */
static void
print_status(pe_working_set_t * data_set)
{
    GListPtr gIter = NULL;
    int print_opts = get_resource_display_options();

    /* space-separated lists of node names */
    char *online_nodes = NULL;
    char *online_remote_nodes = NULL;
    char *online_guest_nodes = NULL;
    char *offline_nodes = NULL;
    char *offline_remote_nodes = NULL;

    if (output_format == mon_output_console) {
        blank_screen();
    }
    print_cluster_summary(stdout, data_set);

    /* Gather node information (and print if in bad state or grouping by node) */
    for (gIter = data_set->nodes; gIter != NULL; gIter = gIter->next) {
        node_t *node = (node_t *) gIter->data;
        const char *node_mode = NULL;
        char *node_name = get_node_display_name(node);

        /* Get node mode */
        if (node->details->unclean) {
            if (node->details->online) {
                node_mode = "UNCLEAN (online)";

            } else if (node->details->pending) {
                node_mode = "UNCLEAN (pending)";

            } else {
                node_mode = "UNCLEAN (offline)";
            }

        } else if (node->details->pending) {
            node_mode = "pending";

        } else if (node->details->standby_onfail && node->details->online) {
            node_mode = "standby (on-fail)";

        } else if (node->details->standby) {
            if (node->details->online) {
                node_mode = "standby";
            } else {
                node_mode = "OFFLINE (standby)";
            }

        } else if (node->details->maintenance) {
            if (node->details->online) {
                node_mode = "maintenance";
            } else {
                node_mode = "OFFLINE (maintenance)";
            }

        } else if (node->details->online) {
            node_mode = "online";
            if (group_by_node == FALSE) {
                if (is_container_remote_node(node)) {
                    online_guest_nodes = add_list_element(online_guest_nodes, node_name);
                } else if (is_baremetal_remote_node(node)) {
                    online_remote_nodes = add_list_element(online_remote_nodes, node_name);
                } else {
                    online_nodes = add_list_element(online_nodes, node_name);
                }
                free(node_name);
                continue;
            }
        } else {
            node_mode = "OFFLINE";
            if (group_by_node == FALSE) {
                if (is_baremetal_remote_node(node)) {
                    offline_remote_nodes = add_list_element(offline_remote_nodes, node_name);
                } else if (is_container_remote_node(node)) {
                    /* ignore offline guest nodes */
                } else {
                    offline_nodes = add_list_element(offline_nodes, node_name);
                }
                free(node_name);
                continue;
            }
        }

        /* If we get here, node is in bad state, or we're grouping by node */

        /* Print the node name and status */
        if (is_container_remote_node(node)) {
            print_as("Guest");
        } else if (is_baremetal_remote_node(node)) {
            print_as("Remote");
        }
        print_as("Node %s: %s\n", node_name, node_mode);

        /* If we're grouping by node, print its resources */
        if (group_by_node) {
            if (print_brief) {
                print_rscs_brief(node->details->running_rsc, "\t", print_opts | pe_print_rsconly,
                                 stdout, FALSE);
            } else {
                GListPtr gIter2 = NULL;

                for (gIter2 = node->details->running_rsc; gIter2 != NULL; gIter2 = gIter2->next) {
                    resource_t *rsc = (resource_t *) gIter2->data;

                    rsc->fns->print(rsc, "\t", print_opts | pe_print_rsconly, stdout);
                }
            }
        }
        free(node_name);
    }

    /* If we're not grouping by node, summarize nodes by status */
    if (online_nodes) {
        print_as("Online: [%s ]\n", online_nodes);
        free(online_nodes);
    }
    if (offline_nodes) {
        print_as("OFFLINE: [%s ]\n", offline_nodes);
        free(offline_nodes);
    }
    if (online_remote_nodes) {
        print_as("RemoteOnline: [%s ]\n", online_remote_nodes);
        free(online_remote_nodes);
    }
    if (offline_remote_nodes) {
        print_as("RemoteOFFLINE: [%s ]\n", offline_remote_nodes);
        free(offline_remote_nodes);
    }
    if (online_guest_nodes) {
        print_as("GuestOnline: [%s ]\n", online_guest_nodes);
        free(online_guest_nodes);
    }

    /* If we haven't already displayed resources grouped by node,
     * or we need to print inactive resources, print a resources section */
    if (group_by_node == FALSE || inactive_resources) {

        /* If we're printing inactive resources, display a heading */
        if (inactive_resources) {
            if (group_by_node == FALSE) {
                print_as("\nFull list of resources:\n");
            } else {
                print_as("\nInactive resources:\n");
            }
        }
        print_as("\n");

        /* If we haven't already printed resources grouped by node,
         * and brief output was requested, print resource summary */
        if (print_brief && group_by_node == FALSE) {
            print_rscs_brief(data_set->resources, NULL, print_opts, stdout,
                             inactive_resources);
        }

        /* For each resource, display it if appropriate */
        for (gIter = data_set->resources; gIter != NULL; gIter = gIter->next) {
            resource_t *rsc = (resource_t *) gIter->data;

            /* Complex resources may have some sub-resources active and some inactive */
            gboolean is_active = rsc->fns->active(rsc, TRUE);
            gboolean partially_active = rsc->fns->active(rsc, FALSE);

            /* Always ignore inactive orphan resources (deleted but not yet gone from CIB) */
            if (is_set(rsc->flags, pe_rsc_orphan) && (is_active == FALSE)) {
                continue;
            }

            /* If we already printed resources grouped by node,
             * only print inactive resources, if that was requested */
            if (group_by_node == TRUE) {
                if ((is_active == FALSE) && inactive_resources) {
                    rsc->fns->print(rsc, NULL, print_opts, stdout);
                }
                continue;
            }

            /* Otherwise, print resource if it's at least partially active
             * or we're displaying inactive resources,
             * except for primitive resources already counted in a brief summary */
            if (!(print_brief && (rsc->variant == pe_native))
                && (partially_active || inactive_resources)) {
                    rsc->fns->print(rsc, NULL, print_opts, stdout);
            }
        }
    }

    /* print Node Attributes section if requested */
    if (show & mon_show_attributes) {
        print_node_attributes(stdout, data_set);
    }

    /* If requested, print resource operations (which includes failcounts)
     * or just failcounts
     */
    if (show & (mon_show_operations | mon_show_failcounts)) {
        print_node_summary(stdout, data_set,
                           ((show & mon_show_operations)? TRUE : FALSE));
    }

    /* If there were any failed actions, print them */
    if (xml_has_children(data_set->failed)) {
        print_failed_actions(stdout, data_set);
    }

    /* Print tickets if requested */
    if (show & mon_show_tickets) {
        print_cluster_tickets(stdout, data_set);
    }

    /* Print negative location constraints if requested */
    if (show & mon_show_bans) {
        print_neg_locations(stdout, data_set);
    }

#if CURSES_ENABLED
    if (output_format == mon_output_console) {
        refresh();
    }
#endif
}

/*!
 * \internal
 * \brief Print cluster status in XML format
 *
 * \param[in] data_set   Working set of CIB state
 */
static void
print_xml_status(pe_working_set_t * data_set)
{
    FILE *stream = stdout;
    GListPtr gIter = NULL;
    int print_opts = get_resource_display_options();

    fprintf(stream, "<?xml version=\"1.0\"?>\n");
    fprintf(stream, "<crm_mon version=\"%s\">\n", VERSION);

    print_cluster_summary(stream, data_set);

    /*** NODES ***/
    fprintf(stream, "    <nodes>\n");
    for (gIter = data_set->nodes; gIter != NULL; gIter = gIter->next) {
        node_t *node = (node_t *) gIter->data;
        const char *node_type = "unknown";

        switch (node->details->type) {
            case node_member:
                node_type = "member";
                break;
            case node_remote:
                node_type = "remote";
                break;
            case node_ping:
                node_type = "ping";
                break;
        }

        fprintf(stream, "        <node name=\"%s\" ", node->details->uname);
        fprintf(stream, "id=\"%s\" ", node->details->id);
        fprintf(stream, "online=\"%s\" ", node->details->online ? "true" : "false");
        fprintf(stream, "standby=\"%s\" ", node->details->standby ? "true" : "false");
        fprintf(stream, "standby_onfail=\"%s\" ", node->details->standby_onfail ? "true" : "false");
        fprintf(stream, "maintenance=\"%s\" ", node->details->maintenance ? "true" : "false");
        fprintf(stream, "pending=\"%s\" ", node->details->pending ? "true" : "false");
        fprintf(stream, "unclean=\"%s\" ", node->details->unclean ? "true" : "false");
        fprintf(stream, "shutdown=\"%s\" ", node->details->shutdown ? "true" : "false");
        fprintf(stream, "expected_up=\"%s\" ", node->details->expected_up ? "true" : "false");
        fprintf(stream, "is_dc=\"%s\" ", node->details->is_dc ? "true" : "false");
        fprintf(stream, "resources_running=\"%d\" ", g_list_length(node->details->running_rsc));
        fprintf(stream, "type=\"%s\" ", node_type);
        if (is_container_remote_node(node)) {
            fprintf(stream, "id_as_resource=\"%s\" ", node->details->remote_rsc->container->id);
        }

        if (group_by_node) {
            GListPtr lpc2 = NULL;

            fprintf(stream, ">\n");
            for (lpc2 = node->details->running_rsc; lpc2 != NULL; lpc2 = lpc2->next) {
                resource_t *rsc = (resource_t *) lpc2->data;

                rsc->fns->print(rsc, "            ", print_opts | pe_print_rsconly, stream);
            }
            fprintf(stream, "        </node>\n");
        } else {
            fprintf(stream, "/>\n");
        }
    }
    fprintf(stream, "    </nodes>\n");

    /*** RESOURCES ***/
    if (group_by_node == FALSE || inactive_resources) {
        fprintf(stream, "    <resources>\n");
        for (gIter = data_set->resources; gIter != NULL; gIter = gIter->next) {
            resource_t *rsc = (resource_t *) gIter->data;
            gboolean is_active = rsc->fns->active(rsc, TRUE);
            gboolean partially_active = rsc->fns->active(rsc, FALSE);

            if (is_set(rsc->flags, pe_rsc_orphan) && is_active == FALSE) {
                continue;

            } else if (group_by_node == FALSE) {
                if (partially_active || inactive_resources) {
                    rsc->fns->print(rsc, "        ", print_opts, stream);
                }

            } else if (is_active == FALSE && inactive_resources) {
                rsc->fns->print(rsc, "        ", print_opts, stream);
            }
        }
        fprintf(stream, "    </resources>\n");
    }

    /* print Node Attributes section if requested */
    if (show & mon_show_attributes) {
        print_node_attributes(stream, data_set);
    }

    /* If requested, print resource operations (which includes failcounts)
     * or just failcounts
     */
    if (show & (mon_show_operations | mon_show_failcounts)) {
        print_node_summary(stream, data_set,
                           ((show & mon_show_operations)? TRUE : FALSE));
    }

    /* If there were any failed actions, print them */
    if (xml_has_children(data_set->failed)) {
        print_failed_actions(stream, data_set);
    }

    /* Print tickets if requested */
    if (show & mon_show_tickets) {
        print_cluster_tickets(stream, data_set);
    }

    /* Print negative location constraints if requested */
    if (show & mon_show_bans) {
        print_neg_locations(stream, data_set);
    }

    fprintf(stream, "</crm_mon>\n");
    fflush(stream);
    fclose(stream);
}

/*!
 * \internal
 * \brief Print cluster status in HTML format (with HTTP headers if CGI)
 *
 * \param[in] data_set   Working set of CIB state
 * \param[in] filename   Name of file to write HTML to (ignored if CGI)
 *
 * \return 0 on success, -1 on error
 */
static int
print_html_status(pe_working_set_t * data_set, const char *filename)
{
    FILE *stream;
    GListPtr gIter = NULL;
    char *filename_tmp = NULL;
    int print_opts = get_resource_display_options();

    if (output_format == mon_output_cgi) {
        stream = stdout;
        fprintf(stream, "Content-type: text/html\n\n");

    } else {
        filename_tmp = crm_concat(filename, "tmp", '.');
        stream = fopen(filename_tmp, "w");
        if (stream == NULL) {
            crm_perror(LOG_ERR, "Cannot open %s for writing", filename_tmp);
            free(filename_tmp);
            return -1;
        }
    }

    fprintf(stream, "<html>\n");
    fprintf(stream, " <head>\n");
    fprintf(stream, "  <title>Cluster status</title>\n");
    fprintf(stream, "  <meta http-equiv=\"refresh\" content=\"%d\">\n", reconnect_msec / 1000);
    fprintf(stream, " </head>\n");
    fprintf(stream, "<body>\n");

    print_cluster_summary(stream, data_set);

    /*** NODE LIST ***/

    fprintf(stream, " <hr />\n <h2>Node List</h2>\n");
    fprintf(stream, "<ul>\n");
    for (gIter = data_set->nodes; gIter != NULL; gIter = gIter->next) {
        node_t *node = (node_t *) gIter->data;
        char *node_name = get_node_display_name(node);

        fprintf(stream, "<li>Node: %s: ", node_name);
        if (node->details->standby_onfail && node->details->online) {
            fprintf(stream, "<font color=\"orange\">standby (on-fail)</font>\n");
        } else if (node->details->standby && node->details->online) {
            fprintf(stream, "<font color=\"orange\">standby</font>\n");
        } else if (node->details->standby) {
            fprintf(stream, "<font color=\"red\">OFFLINE (standby)</font>\n");
        } else if (node->details->maintenance && node->details->online) {
            fprintf(stream, "<font color=\"blue\">maintenance</font>\n");
        } else if (node->details->maintenance) {
            fprintf(stream, "<font color=\"red\">OFFLINE (maintenance)</font>\n");
        } else if (node->details->online) {
            fprintf(stream, "<font color=\"green\">online</font>\n");
        } else {
            fprintf(stream, "<font color=\"red\">OFFLINE</font>\n");
        }
        if (print_brief && group_by_node) {
            fprintf(stream, "<ul>\n");
            print_rscs_brief(node->details->running_rsc, NULL, print_opts | pe_print_rsconly,
                             stream, FALSE);
            fprintf(stream, "</ul>\n");

        } else if (group_by_node) {
            GListPtr lpc2 = NULL;

            fprintf(stream, "<ul>\n");
            for (lpc2 = node->details->running_rsc; lpc2 != NULL; lpc2 = lpc2->next) {
                resource_t *rsc = (resource_t *) lpc2->data;

                fprintf(stream, "<li>");
                rsc->fns->print(rsc, NULL, print_opts | pe_print_rsconly, stream);
                fprintf(stream, "</li>\n");
            }
            fprintf(stream, "</ul>\n");
        }
        fprintf(stream, "</li>\n");
        free(node_name);
    }
    fprintf(stream, "</ul>\n");

    if (group_by_node && inactive_resources) {
        fprintf(stream, "<h2>Inactive Resources</h2>\n");

    } else if (group_by_node == FALSE) {
        fprintf(stream, " <hr />\n <h2>Resource List</h2>\n");
    }

    if (group_by_node == FALSE || inactive_resources) {
        if (print_brief && group_by_node == FALSE) {
            print_rscs_brief(data_set->resources, NULL, print_opts, stream,
                             inactive_resources);
        }

        for (gIter = data_set->resources; gIter != NULL; gIter = gIter->next) {
            resource_t *rsc = (resource_t *) gIter->data;
            gboolean is_active = rsc->fns->active(rsc, TRUE);
            gboolean partially_active = rsc->fns->active(rsc, FALSE);

            if (print_brief && group_by_node == FALSE
                && rsc->variant == pe_native) {
                continue;
            }

            if (is_set(rsc->flags, pe_rsc_orphan) && is_active == FALSE) {
                continue;

            } else if (group_by_node == FALSE) {
                if (partially_active || inactive_resources) {
                    rsc->fns->print(rsc, NULL, print_opts, stream);
                }

            } else if (is_active == FALSE && inactive_resources) {
                rsc->fns->print(rsc, NULL, print_opts, stream);
            }
        }
    }

    /* print Node Attributes section if requested */
    if (show & mon_show_attributes) {
        print_node_attributes(stream, data_set);
    }

    /* If requested, print resource operations (which includes failcounts)
     * or just failcounts
     */
    if (show & (mon_show_operations | mon_show_failcounts)) {
        print_node_summary(stream, data_set,
                           ((show & mon_show_operations)? TRUE : FALSE));
    }

    /* If there were any failed actions, print them */
    if (xml_has_children(data_set->failed)) {
        print_failed_actions(stream, data_set);
    }

    /* Print tickets if requested */
    if (show & mon_show_tickets) {
        print_cluster_tickets(stream, data_set);
    }

    /* Print negative location constraints if requested */
    if (show & mon_show_bans) {
        print_neg_locations(stream, data_set);
    }

    fprintf(stream, "</body>\n");
    fprintf(stream, "</html>\n");
    fflush(stream);
    fclose(stream);

    if (output_format != mon_output_cgi) {
        if (rename(filename_tmp, filename) != 0) {
            crm_perror(LOG_ERR, "Unable to rename %s->%s", filename_tmp, filename);
        }
        free(filename_tmp);
    }
    return 0;
}

#if ENABLE_SNMP
#  include <net-snmp/net-snmp-config.h>
#  include <net-snmp/snmpv3_api.h>
#  include <net-snmp/agent/agent_trap.h>
#  include <net-snmp/library/snmp_client.h>
#  include <net-snmp/library/mib.h>
#  include <net-snmp/library/snmp_debug.h>

#  define add_snmp_field(list, oid_string, value) do {			\
	oid name[MAX_OID_LEN];						\
        size_t name_length = MAX_OID_LEN;				\
	if (snmp_parse_oid(oid_string, name, &name_length)) {		\
	    int s_rc = snmp_add_var(list, name, name_length, 's', (value)); \
	    if(s_rc != 0) {						\
		crm_err("Could not add %s=%s rc=%d", oid_string, value, s_rc); \
	    } else {							\
		crm_trace("Added %s=%s", oid_string, value);		\
	    }								\
	} else {							\
	    crm_err("Could not parse OID: %s", oid_string);		\
	}								\
    } while(0)								\

#  define add_snmp_field_int(list, oid_string, value) do {		\
	oid name[MAX_OID_LEN];						\
        size_t name_length = MAX_OID_LEN;				\
	if (snmp_parse_oid(oid_string, name, &name_length)) {		\
	    if(NULL == snmp_pdu_add_variable(				\
		   list, name, name_length, ASN_INTEGER,		\
		   (u_char *) & value, sizeof(value))) {		\
		crm_err("Could not add %s=%d", oid_string, value);	\
	    } else {							\
		crm_trace("Added %s=%d", oid_string, value);		\
	    }								\
	} else {							\
	    crm_err("Could not parse OID: %s", oid_string);		\
	}								\
    } while(0)								\

static int
snmp_input(int operation, netsnmp_session * session, int reqid, netsnmp_pdu * pdu, void *magic)
{
    return 1;
}

static netsnmp_session *
crm_snmp_init(const char *target, char *community)
{
    static netsnmp_session *session = NULL;

#  ifdef NETSNMPV53
    char target53[128];

    snprintf(target53, sizeof(target53), "%s:162", target);
#  endif

    if (session) {
        return session;
    }

    if (target == NULL) {
        return NULL;
    }

    if (get_crm_log_level() > LOG_INFO) {
        char *debug_tokens = strdup("run:shell,snmptrap,tdomain");

        debug_register_tokens(debug_tokens);
        snmp_set_do_debugging(1);
    }

    session = calloc(1, sizeof(netsnmp_session));
    snmp_sess_init(session);
    session->version = SNMP_VERSION_2c;
    session->callback = snmp_input;
    session->callback_magic = NULL;

    if (community) {
        session->community_len = strlen(community);
        session->community = (unsigned char *)community;
    }

    session = snmp_add(session,
#  ifdef NETSNMPV53
                       netsnmp_tdomain_transport(target53, 0, "udp"),
#  else
                       netsnmp_transport_open_client("snmptrap", target),
#  endif
                       NULL, NULL);

    if (session == NULL) {
        snmp_sess_perror("Could not create snmp transport", session);
    }
    return session;
}

#endif

static int
send_snmp_trap(const char *node, const char *rsc, const char *task, int target_rc, int rc,
               int status, const char *desc)
{
    int ret = 1;

#if ENABLE_SNMP
    static oid snmptrap_oid[] = { 1, 3, 6, 1, 6, 3, 1, 1, 4, 1, 0 };
    static oid sysuptime_oid[] = { 1, 3, 6, 1, 2, 1, 1, 3, 0 };

    netsnmp_pdu *trap_pdu;
    netsnmp_session *session = crm_snmp_init(snmp_target, snmp_community);

    trap_pdu = snmp_pdu_create(SNMP_MSG_TRAP2);
    if (!trap_pdu) {
        crm_err("Failed to create SNMP notification");
        return SNMPERR_GENERR;
    }

    if (1) {
        /* send uptime */
        char csysuptime[20];
        time_t now = time(NULL);

        sprintf(csysuptime, "%ld", now);
        snmp_add_var(trap_pdu, sysuptime_oid, sizeof(sysuptime_oid) / sizeof(oid), 't', csysuptime);
    }

    /* Indicate what the trap is by setting snmpTrapOid.0 */
    ret =
        snmp_add_var(trap_pdu, snmptrap_oid, sizeof(snmptrap_oid) / sizeof(oid), 'o',
                     snmp_crm_trap_oid);
    if (ret != 0) {
        crm_err("Failed set snmpTrapOid.0=%s", snmp_crm_trap_oid);
        return ret;
    }

    /* Add extries to the trap */
    if (rsc) {
        add_snmp_field(trap_pdu, snmp_crm_oid_rsc, rsc);
    }
    add_snmp_field(trap_pdu, snmp_crm_oid_node, node);
    add_snmp_field(trap_pdu, snmp_crm_oid_task, task);
    add_snmp_field(trap_pdu, snmp_crm_oid_desc, desc);

    add_snmp_field_int(trap_pdu, snmp_crm_oid_rc, rc);
    add_snmp_field_int(trap_pdu, snmp_crm_oid_trc, target_rc);
    add_snmp_field_int(trap_pdu, snmp_crm_oid_status, status);

    /* Send and cleanup */
    ret = snmp_send(session, trap_pdu);
    if (ret == 0) {
        /* error */
        snmp_sess_perror("Could not send SNMP trap", session);
        snmp_free_pdu(trap_pdu);
        ret = SNMPERR_GENERR;
    } else {
        ret = SNMPERR_SUCCESS;
    }
#else
    crm_err("Sending SNMP traps is not supported by this installation");
#endif
    return ret;
}

#if ENABLE_ESMTP
#  include <auth-client.h>
#  include <libesmtp.h>

static void
print_recipient_status(smtp_recipient_t recipient, const char *mailbox, void *arg)
{
    const smtp_status_t *status;

    status = smtp_recipient_status(recipient);
    printf("%s: %d %s", mailbox, status->code, status->text);
}

static void
event_cb(smtp_session_t session, int event_no, void *arg, ...)
{
    int *ok;
    va_list alist;

    va_start(alist, arg);
    switch (event_no) {
        case SMTP_EV_CONNECT:
        case SMTP_EV_MAILSTATUS:
        case SMTP_EV_RCPTSTATUS:
        case SMTP_EV_MESSAGEDATA:
        case SMTP_EV_MESSAGESENT:
        case SMTP_EV_DISCONNECT:
            break;

        case SMTP_EV_WEAK_CIPHER:{
                int bits = va_arg(alist, long);
                ok = va_arg(alist, int *);

                crm_debug("SMTP_EV_WEAK_CIPHER, bits=%d - accepted.", bits);
                *ok = 1;
                break;
            }
        case SMTP_EV_STARTTLS_OK:
            crm_debug("SMTP_EV_STARTTLS_OK - TLS started here.");
            break;

        case SMTP_EV_INVALID_PEER_CERTIFICATE:{
                long vfy_result = va_arg(alist, long);
                ok = va_arg(alist, int *);

                /* There is a table in handle_invalid_peer_certificate() of mail-file.c */
                crm_err("SMTP_EV_INVALID_PEER_CERTIFICATE: %ld", vfy_result);
                *ok = 1;
                break;
            }
        case SMTP_EV_NO_PEER_CERTIFICATE:
            ok = va_arg(alist, int *);

            crm_debug("SMTP_EV_NO_PEER_CERTIFICATE - accepted.");
            *ok = 1;
            break;
        case SMTP_EV_WRONG_PEER_CERTIFICATE:
            ok = va_arg(alist, int *);

            crm_debug("SMTP_EV_WRONG_PEER_CERTIFICATE - accepted.");
            *ok = 1;
            break;
        case SMTP_EV_NO_CLIENT_CERTIFICATE:
            ok = va_arg(alist, int *);

            crm_debug("SMTP_EV_NO_CLIENT_CERTIFICATE - accepted.");
            *ok = 1;
            break;
        default:
            crm_debug("Got event: %d - ignored.\n", event_no);
    }
    va_end(alist);
}
#endif

#define BODY_MAX 2048

#if ENABLE_ESMTP
static void
crm_smtp_debug(const char *buf, int buflen, int writing, void *arg)
{
    char type = 0;
    int lpc = 0, last = 0, level = *(int *)arg;

    if (writing == SMTP_CB_HEADERS) {
        type = 'H';
    } else if (writing) {
        type = 'C';
    } else {
        type = 'S';
    }

    for (; lpc < buflen; lpc++) {
        switch (buf[lpc]) {
            case 0:
            case '\n':
                if (last > 0) {
                    do_crm_log(level, "   %.*s", lpc - last, buf + last);
                } else {
                    do_crm_log(level, "%c: %.*s", type, lpc - last, buf + last);
                }
                last = lpc + 1;
                break;
        }
    }
}
#endif

static int
send_custom_trap(const char *node, const char *rsc, const char *task, int target_rc, int rc,
                 int status, const char *desc)
{
    pid_t pid;

    /*setenv needs chars, these are ints */
    char *rc_s = crm_itoa(rc);
    char *status_s = crm_itoa(status);
    char *target_rc_s = crm_itoa(target_rc);

    crm_debug("Sending external notification to '%s' via '%s'", external_recipient, external_agent);

    if(rsc) {
        setenv("CRM_notify_rsc", rsc, 1);
    }
    setenv("CRM_notify_recipient", external_recipient, 1);
    setenv("CRM_notify_node", node, 1);
    setenv("CRM_notify_task", task, 1);
    setenv("CRM_notify_desc", desc, 1);
    setenv("CRM_notify_rc", rc_s, 1);
    setenv("CRM_notify_target_rc", target_rc_s, 1);
    setenv("CRM_notify_status", status_s, 1);

    pid = fork();
    if (pid == -1) {
        crm_perror(LOG_ERR, "notification fork() failed.");
    }
    if (pid == 0) {
        /* crm_debug("notification: I am the child. Executing the nofitication program."); */
        execl(external_agent, external_agent, NULL);
    }

    crm_trace("Finished running custom notification program '%s'.", external_agent);
    free(target_rc_s);
    free(status_s);
    free(rc_s);
    return 0;
}

static int
send_smtp_trap(const char *node, const char *rsc, const char *task, int target_rc, int rc,
               int status, const char *desc)
{
#if ENABLE_ESMTP
    smtp_session_t session;
    smtp_message_t message;
    auth_context_t authctx;
    struct sigaction sa;

    int len = 25; /* Note: Check extra padding on the Subject line below */
    int noauth = 1;
    int smtp_debug = LOG_DEBUG;
    char crm_mail_body[BODY_MAX];
    char *crm_mail_subject = NULL;

    memset(&sa, 0, sizeof(struct sigaction));

    if (node == NULL) {
        node = "-";
    }
    if (rsc == NULL) {
        rsc = "-";
    }
    if (desc == NULL) {
        desc = "-";
    }

    if (crm_mail_to == NULL) {
        return 1;
    }

    if (crm_mail_host == NULL) {
        crm_mail_host = "localhost:25";
    }

    if (crm_mail_prefix == NULL) {
        crm_mail_prefix = "Cluster notification";
    }

    crm_debug("Sending '%s' mail to %s via %s", crm_mail_prefix, crm_mail_to, crm_mail_host);

    len += strlen(crm_mail_prefix);
    len += strlen(task);
    len += strlen(rsc);
    len += strlen(node);
    len += strlen(desc);
    len++;

    crm_mail_subject = calloc(1, len);
    /* If you edit this line, ensure you allocate enough memory for it by altering 'len' above */
    snprintf(crm_mail_subject, len, "%s - %s event for %s on %s: %s\r\n", crm_mail_prefix, task,
             rsc, node, desc);

    len = 0;
    len += snprintf(crm_mail_body + len, BODY_MAX - len, "\r\n%s\r\n", crm_mail_prefix);
    len += snprintf(crm_mail_body + len, BODY_MAX - len, "====\r\n\r\n");
    if (rc == target_rc) {
        len += snprintf(crm_mail_body + len, BODY_MAX - len,
                        "Completed operation %s for resource %s on %s\r\n", task, rsc, node);
    } else {
        len += snprintf(crm_mail_body + len, BODY_MAX - len,
                        "Operation %s for resource %s on %s failed: %s\r\n", task, rsc, node, desc);
    }

    len += snprintf(crm_mail_body + len, BODY_MAX - len, "\r\nDetails:\r\n");
    len += snprintf(crm_mail_body + len, BODY_MAX - len,
                    "\toperation status: (%d) %s\r\n", status, services_lrm_status_str(status));
    if (status == PCMK_LRM_OP_DONE) {
        len += snprintf(crm_mail_body + len, BODY_MAX - len,
                        "\tscript returned: (%d) %s\r\n", rc, services_ocf_exitcode_str(rc));
        len += snprintf(crm_mail_body + len, BODY_MAX - len,
                        "\texpected return value: (%d) %s\r\n", target_rc,
                        services_ocf_exitcode_str(target_rc));
    }

    auth_client_init();
    session = smtp_create_session();
    message = smtp_add_message(session);

    smtp_starttls_enable(session, Starttls_ENABLED);

    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGPIPE, &sa, NULL);

    smtp_set_server(session, crm_mail_host);

    authctx = auth_create_context();
    auth_set_mechanism_flags(authctx, AUTH_PLUGIN_PLAIN, 0);

    smtp_set_eventcb(session, event_cb, NULL);

    /* Now tell libESMTP it can use the SMTP AUTH extension.
     */
    if (!noauth) {
        crm_debug("Adding authentication context");
        smtp_auth_set_context(session, authctx);
    }

    if (crm_mail_from == NULL) {
        struct utsname us;
        char auto_from[BODY_MAX];

        CRM_ASSERT(uname(&us) == 0);
        snprintf(auto_from, BODY_MAX, "crm_mon@%s", us.nodename);
        smtp_set_reverse_path(message, auto_from);

    } else {
        /* NULL is ok */
        smtp_set_reverse_path(message, crm_mail_from);
    }

    smtp_set_header(message, "To", NULL /*phrase */ , NULL /*addr */ ); /* "Phrase" <addr> */
    smtp_add_recipient(message, crm_mail_to);

    /* Set the Subject: header and override any subject line in the message headers. */
    smtp_set_header(message, "Subject", crm_mail_subject);
    smtp_set_header_option(message, "Subject", Hdr_OVERRIDE, 1);

    smtp_set_message_str(message, crm_mail_body);
    smtp_set_monitorcb(session, crm_smtp_debug, &smtp_debug, 1);

    if (smtp_start_session(session)) {
        char buf[128];
        int rc = smtp_errno();

        crm_err("SMTP server problem: %s (%d)", smtp_strerror(rc, buf, sizeof buf), rc);

    } else {
        char buf[128];
        int rc = smtp_errno();
        const smtp_status_t *smtp_status = smtp_message_transfer_status(message);

        if (rc != 0) {
            crm_err("SMTP server problem: %s (%d)", smtp_strerror(rc, buf, sizeof buf), rc);
        }
        crm_info("Send status: %d %s", smtp_status->code, crm_str(smtp_status->text));
        smtp_enumerate_recipients(message, print_recipient_status, NULL);
    }

    smtp_destroy_session(session);
    auth_destroy_context(authctx);
    auth_client_exit();
#endif
    return 0;
}

static void
handle_rsc_op(xmlNode * xml, const char *node_id)
{
    int rc = -1;
    int status = -1;
    int action = -1;
    int interval = 0;
    int target_rc = -1;
    int transition_num = -1;
    gboolean notify = TRUE;

    char *rsc = NULL;
    char *task = NULL;
    const char *desc = NULL;
    const char *magic = NULL;
    const char *id = NULL;
    char *update_te_uuid = NULL;
    const char *node = NULL;

    xmlNode *n = xml;
    xmlNode * rsc_op = xml;

    if(strcmp((const char*)xml->name, XML_LRM_TAG_RSC_OP) != 0) {
        xmlNode *cIter;

        for(cIter = xml->children; cIter; cIter = cIter->next) {
            handle_rsc_op(cIter, node_id);
        }

        return;
    }

    id = crm_element_value(rsc_op, XML_LRM_ATTR_TASK_KEY);
    if (id == NULL) {
        /* Compatability with <= 1.1.5 */
        id = ID(rsc_op);
    }

    magic = crm_element_value(rsc_op, XML_ATTR_TRANSITION_MAGIC);
    if (magic == NULL) {
        /* non-change */
        return;
    }

    if (FALSE == decode_transition_magic(magic, &update_te_uuid, &transition_num, &action,
                                         &status, &rc, &target_rc)) {
        crm_err("Invalid event %s detected for %s", magic, id);
        return;
    }

    if (parse_op_key(id, &rsc, &task, &interval) == FALSE) {
        crm_err("Invalid event detected for %s", id);
        goto bail;
    }

    node = crm_element_value(rsc_op, XML_LRM_ATTR_TARGET);

    while (n != NULL && safe_str_neq(XML_CIB_TAG_STATE, TYPE(n))) {
        n = n->parent;
    }

    if(node == NULL && n) {
        node = crm_element_value(n, XML_ATTR_UNAME);
    }

    if (node == NULL && n) {
        node = ID(n);
    }

    if (node == NULL) {
        node = node_id;
    }

    if (node == NULL) {
        crm_err("No node detected for event %s (%s)", magic, id);
        goto bail;
    }

    /* look up where we expected it to be? */
    desc = pcmk_strerror(pcmk_ok);
    if (status == PCMK_LRM_OP_DONE && target_rc == rc) {
        crm_notice("%s of %s on %s completed: %s", task, rsc, node, desc);
        if (rc == PCMK_OCF_NOT_RUNNING) {
            notify = FALSE;
        }

    } else if (status == PCMK_LRM_OP_DONE) {
        desc = services_ocf_exitcode_str(rc);
        crm_warn("%s of %s on %s failed: %s", task, rsc, node, desc);

    } else {
        desc = services_lrm_status_str(status);
        crm_warn("%s of %s on %s failed: %s", task, rsc, node, desc);
    }

    if (notify && snmp_target) {
        send_snmp_trap(node, rsc, task, target_rc, rc, status, desc);
    }
    if (notify && crm_mail_to) {
        send_smtp_trap(node, rsc, task, target_rc, rc, status, desc);
    }
    if (notify && external_agent) {
        send_custom_trap(node, rsc, task, target_rc, rc, status, desc);
    }
  bail:
    free(update_te_uuid);
    free(rsc);
    free(task);
}

static gboolean
mon_trigger_refresh(gpointer user_data)
{
    mainloop_set_trigger(refresh_trigger);
    return FALSE;
}

#define NODE_PATT "/lrm[@id="
static char *get_node_from_xpath(const char *xpath) 
{
    char *nodeid = NULL;
    char *tmp = strstr(xpath, NODE_PATT);

    if(tmp) {
        tmp += strlen(NODE_PATT);
        tmp += 1;

        nodeid = strdup(tmp);
        tmp = strstr(nodeid, "\'");
        CRM_ASSERT(tmp);
        tmp[0] = 0;
    }
    return nodeid;
}

static void crm_diff_update_v2(const char *event, xmlNode * msg) 
{
    xmlNode *change = NULL;
    xmlNode *diff = get_message_xml(msg, F_CIB_UPDATE_RESULT);

    for (change = __xml_first_child(diff); change != NULL; change = __xml_next(change)) {
        const char *name = NULL;
        const char *op = crm_element_value(change, XML_DIFF_OP);
        const char *xpath = crm_element_value(change, XML_DIFF_PATH);
        xmlNode *match = NULL;
        const char *node = NULL;

        if(op == NULL) {
            continue;

        } else if(strcmp(op, "create") == 0) {
            match = change->children;

        } else if(strcmp(op, "move") == 0) {
            continue;

        } else if(strcmp(op, "delete") == 0) {
            continue;

        } else if(strcmp(op, "modify") == 0) {
            match = first_named_child(change, XML_DIFF_RESULT);
            if(match) {
                match = match->children;
            }
        }

        if(match) {
            name = (const char *)match->name;
        }

        crm_trace("Handling %s operation for %s %p, %s", op, xpath, match, name);
        if(xpath == NULL) {
            /* Version field, ignore */

        } else if(name == NULL) {
            crm_debug("No result for %s operation to %s", op, xpath);
            CRM_ASSERT(strcmp(op, "delete") == 0 || strcmp(op, "move") == 0);

        } else if(strcmp(name, XML_TAG_CIB) == 0) {
            xmlNode *state = NULL;
            xmlNode *status = first_named_child(match, XML_CIB_TAG_STATUS);

            for (state = __xml_first_child(status); state != NULL; state = __xml_next(state)) {
                node = crm_element_value(state, XML_ATTR_UNAME);
                if (node == NULL) {
                    node = ID(state);
                }
                handle_rsc_op(state, node);
            }

        } else if(strcmp(name, XML_CIB_TAG_STATUS) == 0) {
            xmlNode *state = NULL;

            for (state = __xml_first_child(match); state != NULL; state = __xml_next(state)) {
                node = crm_element_value(state, XML_ATTR_UNAME);
                if (node == NULL) {
                    node = ID(state);
                }
                handle_rsc_op(state, node);
            }

        } else if(strcmp(name, XML_CIB_TAG_STATE) == 0) {
            node = crm_element_value(match, XML_ATTR_UNAME);
            if (node == NULL) {
                node = ID(match);
            }
            handle_rsc_op(match, node);

        } else if(strcmp(name, XML_CIB_TAG_LRM) == 0) {
            node = ID(match);
            handle_rsc_op(match, node);

        } else if(strcmp(name, XML_LRM_TAG_RESOURCES) == 0) {
            char *local_node = get_node_from_xpath(xpath);

            handle_rsc_op(match, local_node);
            free(local_node);

        } else if(strcmp(name, XML_LRM_TAG_RESOURCE) == 0) {
            char *local_node = get_node_from_xpath(xpath);

            handle_rsc_op(match, local_node);
            free(local_node);

        } else if(strcmp(name, XML_LRM_TAG_RSC_OP) == 0) {
            char *local_node = get_node_from_xpath(xpath);

            handle_rsc_op(match, local_node);
            free(local_node);

        } else {
            crm_err("Ignoring %s operation for %s %p, %s", op, xpath, match, name);
        }
    }
}

static void crm_diff_update_v1(const char *event, xmlNode * msg) 
{
    /* Process operation updates */
    xmlXPathObject *xpathObj = xpath_search(msg,
                                            "//" F_CIB_UPDATE_RESULT "//" XML_TAG_DIFF_ADDED
                                            "//" XML_LRM_TAG_RSC_OP);
    int lpc = 0, max = numXpathResults(xpathObj);

    for (lpc = 0; lpc < max; lpc++) {
        xmlNode *rsc_op = getXpathResult(xpathObj, lpc);

        handle_rsc_op(rsc_op, NULL);
    }
    freeXpathObject(xpathObj);
}

void
crm_diff_update(const char *event, xmlNode * msg)
{
    int rc = -1;
    long now = time(NULL);
    static bool stale = FALSE;
    static int updates = 0;
    static mainloop_timer_t *refresh_timer = NULL;
    xmlNode *diff = get_message_xml(msg, F_CIB_UPDATE_RESULT);

    print_dot();

    if(refresh_timer == NULL) {
        refresh_timer = mainloop_timer_add("refresh", 2000, FALSE, mon_trigger_refresh, NULL);
    }

    if (current_cib != NULL) {
        rc = xml_apply_patchset(current_cib, diff, TRUE);

        switch (rc) {
            case -pcmk_err_diff_resync:
            case -pcmk_err_diff_failed:
                crm_notice("[%s] Patch aborted: %s (%d)", event, pcmk_strerror(rc), rc);
                free_xml(current_cib); current_cib = NULL;
                break;
            case pcmk_ok:
                updates++;
                break;
            default:
                crm_notice("[%s] ABORTED: %s (%d)", event, pcmk_strerror(rc), rc);
                free_xml(current_cib); current_cib = NULL;
        }
    }

    if (current_cib == NULL) {
        crm_trace("Re-requesting the full cib");
        cib->cmds->query(cib, NULL, &current_cib, cib_scope_local | cib_sync_call);
    }

    if (crm_mail_to || snmp_target || external_agent) {
        int format = 0;
        crm_element_value_int(diff, "format", &format);
        switch(format) {
            case 1:
                crm_diff_update_v1(event, msg);
                break;
            case 2:
                crm_diff_update_v2(event, msg);
                break;
            default:
                crm_err("Unknown patch format: %d", format);
        }
    }

    if (current_cib == NULL) {
        if(!stale) {
            print_as("--- Stale data ---");
        }
        stale = TRUE;
        return;
    }

    stale = FALSE;
    /* Refresh
     * - immediately if the last update was more than 5s ago
     * - every 10 updates
     * - at most 2s after the last update
     */
    if ((now - last_refresh) > (reconnect_msec / 1000)) {
        mainloop_set_trigger(refresh_trigger);
        mainloop_timer_stop(refresh_timer);
        updates = 0;

    } else if(updates > 10) {
        mainloop_set_trigger(refresh_trigger);
        mainloop_timer_stop(refresh_timer);
        updates = 0;

    } else {
        mainloop_timer_start(refresh_timer);
    }
}

gboolean
mon_refresh_display(gpointer user_data)
{
    xmlNode *cib_copy = copy_xml(current_cib);
    pe_working_set_t data_set;

    last_refresh = time(NULL);

    if (cli_config_update(&cib_copy, NULL, FALSE) == FALSE) {
        if (cib) {
            cib->cmds->signoff(cib);
        }
        print_as("Upgrade failed: %s", pcmk_strerror(-pcmk_err_schema_validation));
        if (output_format == mon_output_console) {
            sleep(2);
        }
        clean_up(EX_USAGE);
        return FALSE;
    }

    set_working_set_defaults(&data_set);
    data_set.input = cib_copy;
    cluster_status(&data_set);

    /* Unpack constraints if any section will need them
     * (tickets may be referenced in constraints but not granted yet,
     * and bans need negative location constraints) */
    if (show & (mon_show_bans | mon_show_tickets)) {
        xmlNode *cib_constraints = get_object_root(XML_CIB_TAG_CONSTRAINTS, data_set.input);
        unpack_constraints(cib_constraints, &data_set);
    }

    switch (output_format) {
        case mon_output_html:
        case mon_output_cgi:
            if (print_html_status(&data_set, output_filename) != 0) {
                fprintf(stderr, "Critical: Unable to output html file\n");
                clean_up(EX_USAGE);
            }
            break;

        case mon_output_xml:
            print_xml_status(&data_set);
            break;

        case mon_output_monitor:
            print_simple_status(&data_set);
            if (has_warnings) {
                clean_up(MON_STATUS_WARN);
            }
            break;

        case mon_output_plain:
        case mon_output_console:
            print_status(&data_set);
            break;

        case mon_output_none:
            break;
    }

    cleanup_calculations(&data_set);
    return TRUE;
}

void
mon_st_callback(stonith_t * st, stonith_event_t * e)
{
    char *desc = crm_strdup_printf("Operation %s requested by %s for peer %s: %s (ref=%s)",
                                 e->operation, e->origin, e->target, pcmk_strerror(e->result),
                                 e->id);

    if (snmp_target) {
        send_snmp_trap(e->target, NULL, e->operation, pcmk_ok, e->result, 0, desc);
    }
    if (crm_mail_to) {
        send_smtp_trap(e->target, NULL, e->operation, pcmk_ok, e->result, 0, desc);
    }
    if (external_agent) {
        send_custom_trap(e->target, NULL, e->operation, pcmk_ok, e->result, 0, desc);
    }
    free(desc);
}

/*
 * De-init ncurses, signoff from the CIB and deallocate memory.
 */
void
clean_up(int rc)
{
#if ENABLE_SNMP
    netsnmp_session *session = crm_snmp_init(NULL, NULL);

    if (session) {
        snmp_close(session);
        snmp_shutdown("snmpapp");
    }
#endif

#if CURSES_ENABLED
    if (output_format == mon_output_console) {
        output_format = mon_output_plain;
        echo();
        nocbreak();
        endwin();
    }
#endif

    if (cib != NULL) {
        cib->cmds->signoff(cib);
        cib_delete(cib);
        cib = NULL;
    }

    free(output_filename);
    free(xml_file);
    free(pid_file);

    if (rc >= 0) {
        crm_exit(rc);
    }
    return;
}
