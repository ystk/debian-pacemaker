/*
 * Copyright (C) 2009 Andrew Beekhof <andrew@beekhof.net>
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
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/utsname.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/common/ipc.h>
#include <crm/cluster/internal.h>
#include <crm/common/mainloop.h>

#include <crm/stonith-ng.h>
#include <crm/cib.h>
#include <crm/pengine/status.h>

#include <crm/common/xml.h>


/* *INDENT-OFF* */
static struct crm_option long_options[] = {
    {"help",        0, 0, '?', "\tThis text"},
    {"version",     0, 0, '$', "\tVersion information"  },
    {"verbose",     0, 0, 'V', "\tIncrease debug output"},
    {"quiet",       0, 0, 'q', "\tPrint only essential output"},

    {"-spacer-",    0, 0, '-', "Commands:"},
    {"list",            1, 0, 'l', "List devices that can terminate the specified host"},
    {"list-registered", 0, 0, 'L', "List all registered devices"},
    {"list-installed",  0, 0, 'I', "List all installed devices"},

    {"-spacer-",    0, 0, '-', ""},
    {"metadata",    0, 0, 'M', "Check the device's metadata"},
    {"query",       1, 0, 'Q', "Check the device's status"},
    {"fence",       1, 0, 'F', "Fence the named host"},
    {"unfence",     1, 0, 'U', "Unfence the named host"},
    {"reboot",      1, 0, 'B', "Reboot the named host"},
    {"confirm",     1, 0, 'C', "Confirm the named host is now safely down"},
    {"history",     1, 0, 'H', "Retrieve last fencing operation"},
    {"last",        1, 0, 'h', "Indicate when the named node was last fenced. Optional: --as-node-id"},

    {"-spacer-",    0, 0, '-', ""},
    {"register",    1, 0, 'R', "Register the named stonith device. Requires: --agent, optional: --option"},
    {"deregister",  1, 0, 'D', "De-register the named stonith device"},

    {"register-level",    1, 0, 'r',
     "Register a stonith level for the named target, specified as one of:\n\t"
     "NAME, @PATTERN, or ATTR=VALUE. Requires: --index, one or more --device entries"},
    {"deregister-level",  1, 0, 'd', "De-register a stonith level for the named target\n\t"
     "Target is specified as for --register-level. Requires: --index"},

    {"-spacer-",    0, 0, '-', ""},
    {"-spacer-",    0, 0, '-', "Options and modifiers:"},
    {"agent",       1, 0, 'a', "The agent (eg. fence_xvm) to instantiate when calling with --register"},
    {"env-option",  1, 0, 'e'},
    {"option",      1, 0, 'o'},
    {"tag",         1, 0, 'T', "Identify fencing operations with the specified tag.\n\t"
     "Useful when there are multiple entities that might be invoking stonith_admin(8)"},

    {"device",      1, 0, 'v', "A device to associate with a given host and stonith level"},
    {"index",       1, 0, 'i', "The stonith level (1-9)"},

    {"timeout",     1, 0, 't', "Operation timeout in seconds"},
    {"as-node-id",  0, 0, 'n', "(Advanced) The supplied node is the corosync nodeid (Only for use with --last)" },
    {"tolerance",   1, 0,   0, "(Advanced) Do nothing if an equivalent --fence request succeeded less than N seconds earlier" },

    {"list-all",    0, 0, 'L', "legacy alias for --list-registered"},

    {0, 0, 0, 0}
};
/* *INDENT-ON* */

int st_opts = st_opt_sync_call | st_opt_allow_suicide;

GMainLoop *mainloop = NULL;
struct {
    stonith_t *st;
    const char *target;
    const char *action;
    char *name;
    int timeout;
    int tolerance;
    int rc;
} async_fence_data;

static int
try_mainloop_connect(void)
{
    stonith_t *st = async_fence_data.st;
    int tries = 10;
    int i = 0;
    int rc = 0;

    for (i = 0; i < tries; i++) {
        crm_debug("Connecting as %s", async_fence_data.name);
        rc = st->cmds->connect(st, async_fence_data.name, NULL);

        if (!rc) {
            crm_debug("stonith client connection established");
            return 0;
        } else {
            crm_debug("stonith client connection failed");
        }
        sleep(1);
    }

    crm_err("Couldn not connect to stonithd. \n");
    return -1;
}

static void
notify_callback(stonith_t * st, stonith_event_t * e)
{
    if (e->result != pcmk_ok) {
        return;
    }

    if (safe_str_eq(async_fence_data.target, e->target) &&
        safe_str_eq(async_fence_data.action, e->action)) {

        async_fence_data.rc = e->result;
        g_main_loop_quit(mainloop);
    }
}

static void
fence_callback(stonith_t * stonith, stonith_callback_data_t * data)
{
    async_fence_data.rc = data->rc;

    g_main_loop_quit(mainloop);
}

static gboolean
async_fence_helper(gpointer user_data)
{
    stonith_t *st = async_fence_data.st;
    int call_id = 0;

    if (try_mainloop_connect()) {
        g_main_loop_quit(mainloop);
        return TRUE;
    }

    st->cmds->register_notification(st, T_STONITH_NOTIFY_FENCE, notify_callback);

    call_id = st->cmds->fence(st,
                              st_opt_allow_suicide,
                              async_fence_data.target,
                              async_fence_data.action,
                              async_fence_data.timeout, async_fence_data.tolerance);

    if (call_id < 0) {
        g_main_loop_quit(mainloop);
        return TRUE;
    }

    st->cmds->register_callback(st,
                                call_id,
                                async_fence_data.timeout,
                                st_opt_timeout_updates, NULL, "callback", fence_callback);

    return TRUE;
}

static int
mainloop_fencing(stonith_t * st, const char *target, const char *action, int timeout, int tolerance)
{
    crm_trigger_t *trig;

    async_fence_data.st = st;
    async_fence_data.target = target;
    async_fence_data.action = action;
    async_fence_data.timeout = timeout;
    async_fence_data.tolerance = tolerance;
    async_fence_data.rc = -1;

    trig = mainloop_add_trigger(G_PRIORITY_HIGH, async_fence_helper, NULL);
    mainloop_set_trigger(trig);

    mainloop = g_main_new(FALSE);
    g_main_run(mainloop);

    return async_fence_data.rc;
}

static int
handle_level(stonith_t *st, char *target, int fence_level,
             stonith_key_value_t *devices, bool added)
{
    char *node = NULL;
    char *pattern = NULL;
    char *name = NULL;
    char *value = strchr(target, '=');

    /* Determine if targeting by attribute, node name pattern or node name */
    if (value != NULL)  {
        name = target;
        *value++ = '\0';
    } else if (*target == '@') {
        pattern = target + 1;
    } else {
        node = target;
    }

    /* Register or unregister level as appropriate */
    if (added) {
        return st->cmds->register_level_full(st, st_opts, node, pattern,
                                             name, value, fence_level,
                                             devices);
    }
    return st->cmds->remove_level_full(st, st_opts, node, pattern,
                                       name, value, fence_level);
}

int
main(int argc, char **argv)
{
    int flag;
    int rc = 0;
    int quiet = 0;
    int verbose = 0;
    int argerr = 0;
    int timeout = 120;
    int option_index = 0;
    int fence_level = 0;
    int no_connect = 0;
    int tolerance = 0;
    int as_nodeid = FALSE;

    char *name = NULL;
    char *value = NULL;
    char *target = NULL;
    const char *agent = NULL;
    const char *device = NULL;
    const char *longname = NULL;

    char action = 0;
    stonith_t *st = NULL;
    stonith_key_value_t *params = NULL;
    stonith_key_value_t *devices = NULL;
    stonith_key_value_t *dIter = NULL;

    crm_log_cli_init("stonith_admin");
    crm_set_options(NULL, "mode [options]", long_options,
                    "Provides access to the stonith-ng API.\n"
                    "\nAllows the administrator to add/remove/list devices, check device and host status and fence hosts\n");

    async_fence_data.name = strdup(crm_system_name);

    while (1) {
        flag = crm_get_option_long(argc, argv, &option_index, &longname);
        if (flag == -1)
            break;

        switch (flag) {
            case 'V':
                verbose = 1;
                crm_bump_log_level(argc, argv);
                break;
            case '$':
            case '?':
                crm_help(flag, EX_OK);
                break;
            case 'I':
                no_connect = 1;
                /* fall through */
            case 'L':
                action = flag;
                break;
            case 'q':
                quiet = 1;
                break;
            case 'Q':
            case 'R':
            case 'D':
                action = flag;
                device = optarg;
                break;
            case 'T':
                free(async_fence_data.name);
                async_fence_data.name = crm_strdup_printf("%s.%s", crm_system_name, optarg);
                break;
            case 'a':
                agent = optarg;
                break;
            case 'l':
                target = optarg;
                action = 'L';
                break;
            case 'M':
                no_connect = 1;
                action = flag;
                break;
            case 't':
                timeout = crm_atoi(optarg, NULL);
                break;
            case 'B':
            case 'F':
            case 'U':
                /* using mainloop here */
                no_connect = 1;
                /* fall through */
            case 'C':
                /* Always log the input arguments */
                crm_log_args(argc, argv);
                target = optarg;
                action = flag;
                break;
            case 'n':
                as_nodeid = TRUE;
                break;
            case 'h':
            case 'H':
            case 'r':
            case 'd':
                target = optarg;
                action = flag;
                break;
            case 'i':
                fence_level = crm_atoi(optarg, NULL);
                break;
            case 'v':
                devices = stonith_key_value_add(devices, NULL, optarg);
                break;
            case 'o':
                crm_info("Scanning: -o %s", optarg);
                rc = sscanf(optarg, "%m[^=]=%m[^=]", &name, &value);
                if (rc != 2) {
                    crm_err("Invalid option: -o %s", optarg);
                    ++argerr;
                } else {
                    crm_info("Got: '%s'='%s'", name, value);
                    params = stonith_key_value_add(params, name, value);
                }
                free(value); value = NULL;
                free(name); name = NULL;
                break;
            case 'e':
                {
                    char *key = crm_concat("OCF_RESKEY", optarg, '_');
                    const char *env = getenv(key);

                    if (env == NULL) {
                        crm_err("Invalid option: -e %s", optarg);
                        ++argerr;
                    } else {
                        crm_info("Got: '%s'='%s'", optarg, env);
                        params = stonith_key_value_add(params, optarg, env);
                    }
                }
                break;
            case 0:
                if (safe_str_eq("tolerance", longname)) {
                    tolerance = crm_get_msec(optarg) / 1000;    /* Send in seconds */
                }
                break;
            default:
                ++argerr;
                break;
        }
    }

    if (optind > argc) {
        ++argerr;
    }

    if (argerr) {
        crm_help('?', EX_USAGE);
    }

    crm_debug("Create");
    st = stonith_api_new();
    crm_debug("Created");

    if (!no_connect) {
        crm_debug("Connecting as %s", async_fence_data.name);
        rc = st->cmds->connect(st, async_fence_data.name, NULL);

        crm_debug("Connect: %d", rc);

        if (rc < 0) {
            goto done;
        }
    }

    switch (action) {
        case 'I':
            rc = st->cmds->list_agents(st, st_opt_sync_call, NULL, &devices, timeout);
            for (dIter = devices; dIter; dIter = dIter->next) {
                fprintf(stdout, " %s\n", dIter->value);
            }
            if (rc == 0) {
                fprintf(stderr, "No devices found\n");

            } else if (rc > 0) {
                fprintf(stderr, "%d devices found\n", rc);
                rc = 0;
            }
            stonith_key_value_freeall(devices, 1, 1);
            break;
        case 'L':
            rc = st->cmds->query(st, st_opts, target, &devices, timeout);
            for (dIter = devices; dIter; dIter = dIter->next) {
                fprintf(stdout, " %s\n", dIter->value);
            }
            if (rc == 0) {
                fprintf(stderr, "No devices found\n");
            } else if (rc > 0) {
                fprintf(stderr, "%d devices found\n", rc);
                rc = 0;
            }
            stonith_key_value_freeall(devices, 1, 1);
            break;
        case 'Q':
            rc = st->cmds->monitor(st, st_opts, device, timeout);
            if (rc < 0) {
                rc = st->cmds->list(st, st_opts, device, NULL, timeout);
            }
            break;
        case 'R':
            rc = st->cmds->register_device(st, st_opts, device, "stonith-ng", agent, params);
            break;
        case 'D':
            rc = st->cmds->remove_device(st, st_opts, device);
            break;
        case 'd':
        case 'r':
            rc = handle_level(st, target, fence_level, devices, action == 'r');
            break;
        case 'M':
            if (agent == NULL) {
                printf("Please specify an agent to query using -a,--agent [value]\n");
                return -1;
            } else {
                char *buffer = NULL;

                rc = st->cmds->metadata(st, st_opt_sync_call, agent, NULL, &buffer, timeout);
                if (rc == pcmk_ok) {
                    printf("%s\n", buffer);
                }
                free(buffer);
            }
            break;
        case 'C':
            rc = st->cmds->confirm(st, st_opts, target);
            break;
        case 'B':
            rc = mainloop_fencing(st, target, "reboot", timeout, tolerance);
            break;
        case 'F':
            rc = mainloop_fencing(st, target, "off", timeout, tolerance);
            break;
        case 'U':
            rc = mainloop_fencing(st, target, "on", timeout, tolerance);
            break;
        case 'h':
            {
                time_t when = 0;

                if(as_nodeid) {
                    uint32_t nodeid = atol(target);
                    when = stonith_api_time(nodeid, NULL, FALSE);
                } else {
                    when = stonith_api_time(0, target, FALSE);
                }
                if(when) {
                    printf("Node %s last kicked at: %s\n", target, ctime(&when));
                } else {
                    printf("Node %s has never been kicked\n", target);
                }
            }
            break;
        case 'H':
            {
                stonith_history_t *history, *hp, *latest = NULL;

                rc = st->cmds->history(st, st_opts, target, &history, timeout);
                for (hp = history; hp; hp = hp->next) {
                    char *action_s = NULL;
                    time_t complete = hp->completed;

                    if (hp->state == st_done) {
                        latest = hp;
                    }

                    if (quiet || !verbose) {
                        continue;
                    } else if (hp->action == NULL) {
                        action_s = strdup("unknown");
                    } else if (hp->action[0] != 'r') {
                        action_s = crm_concat("turn", hp->action, ' ');
                    } else {
                        action_s = strdup(hp->action);
                    }

                    if (hp->state == st_failed) {
                        printf("%s failed to %s node %s on behalf of %s from %s at %s\n",
                               hp->delegate ? hp->delegate : "We", action_s, hp->target,
                               hp->client, hp->origin, ctime(&complete));

                    } else if (hp->state == st_done && hp->delegate) {
                        printf("%s was able to %s node %s on behalf of %s from %s at %s\n",
                               hp->delegate, action_s, hp->target,
                               hp->client, hp->origin, ctime(&complete));

                    } else if (hp->state == st_done) {
                        printf("We were able to %s node %s on behalf of %s from %s at %s\n",
                               action_s, hp->target, hp->client, hp->origin, ctime(&complete));
                    } else {
                        printf("%s at %s wishes to %s node %s - %d %d\n",
                               hp->client, hp->origin, action_s, hp->target, hp->state, hp->completed);
                    }

                    free(action_s);
                }

                if (latest) {
                    if (quiet) {
                        printf("%d\n", latest->completed);
                    } else {
                        char *action_s = NULL;
                        time_t complete = latest->completed;

                        if (latest->action == NULL) {
                            action_s = strdup("unknown");
                        } else if (latest->action[0] != 'r') {
                            action_s = crm_concat("turn", latest->action, ' ');
                        } else {
                            action_s = strdup(latest->action);
                        }

                        printf("%s was able to %s node %s on behalf of %s from %s at %s\n",
                               latest->delegate ? latest->delegate : "We", action_s, latest->target,
                               latest->client, latest->origin, ctime(&complete));

                        free(action_s);
                    }
                }
                break;
            }                   /* closing bracket for -H case */
    }                           /* closing bracket for switch case */

  done:
    free(async_fence_data.name);
    crm_info("Command returned: %s (%d)", pcmk_strerror(rc), rc);
    if (rc < 0) {
        printf("Command failed: %s\n", pcmk_strerror(rc));
    }

    stonith_key_value_freeall(params, 1, 1);
    st->cmds->disconnect(st);
    crm_debug("Disconnect: %d", rc);

    crm_debug("Destroy");
    stonith_api_delete(st);

    return rc;
}
