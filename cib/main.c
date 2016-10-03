/*
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
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
#include <errno.h>
#include <fcntl.h>

#include <crm/crm.h>
#include <crm/cib/internal.h>
#include <crm/msg_xml.h>
#include <crm/cluster/internal.h>

#include <crm/common/xml.h>

#include <crm/common/mainloop.h>

#include <cibio.h>
#include <callbacks.h>
#include <pwd.h>
#include <grp.h>
#include "common.h"

#if HAVE_LIBXML2
#  include <libxml/parser.h>
#endif

#ifdef HAVE_GETOPT_H
#  include <getopt.h>
#endif

#if HAVE_BZLIB_H
#  include <bzlib.h>
#endif

extern int init_remote_listener(int port, gboolean encrypted);
gboolean cib_shutdown_flag = FALSE;
int cib_status = pcmk_ok;

crm_cluster_t crm_cluster;

#if SUPPORT_HEARTBEAT
oc_ev_t *cib_ev_token;
ll_cluster_t *hb_conn = NULL;
extern void oc_ev_special(const oc_ev_t *, oc_ev_class_t, int);
gboolean cib_register_ha(ll_cluster_t * hb_cluster, const char *client_name);
#else
void *hb_conn = NULL;
#endif

GMainLoop *mainloop = NULL;
const char *cib_root = NULL;
char *cib_our_uname = NULL;
gboolean preserve_status = FALSE;
gboolean cib_writes_enabled = TRUE;
int remote_fd = 0;
int remote_tls_fd = 0;

int cib_init(void);
void cib_shutdown(int nsig);
gboolean startCib(const char *filename);
extern int write_cib_contents(gpointer p);

GHashTable *config_hash = NULL;
GHashTable *local_notify_queue = NULL;

char *channel1 = NULL;
char *channel2 = NULL;
char *channel3 = NULL;
char *channel4 = NULL;
char *channel5 = NULL;

#define OPTARGS	"maswr:V?"
void cib_cleanup(void);

static void
cib_enable_writes(int nsig)
{
    crm_info("(Re)enabling disk writes");
    cib_writes_enabled = TRUE;
}

static void
log_cib_client(gpointer key, gpointer value, gpointer user_data)
{
    crm_info("Client %s", crm_client_name(value));
}

/* *INDENT-OFF* */
static struct crm_option long_options[] = {
    /* Top-level Options */
    {"help",    0, 0, '?', "\tThis text"},
    {"verbose", 0, 0, 'V', "\tIncrease debug output"},

    {"per-action-cib", 0, 0, 'a', "\tAdvanced use only"},
    {"stand-alone",    0, 0, 's', "\tAdvanced use only"},
    {"disk-writes",    0, 0, 'w', "\tAdvanced use only"},
    {"cib-root",       1, 0, 'r', "\tAdvanced use only"},

    {0, 0, 0, 0}
};
/* *INDENT-ON* */

int
main(int argc, char **argv)
{
    int flag;
    int rc = 0;
    int index = 0;
    int argerr = 0;
    struct passwd *pwentry = NULL;

    crm_log_preinit(NULL, argc, argv);
    crm_set_options(NULL, "[options]",
                    long_options, "Daemon for storing and replicating the cluster configuration");

    crm_peer_init();

    mainloop_add_signal(SIGTERM, cib_shutdown);
    mainloop_add_signal(SIGPIPE, cib_enable_writes);

    cib_writer = mainloop_add_trigger(G_PRIORITY_LOW, write_cib_contents, NULL);

    while (1) {
        flag = crm_get_option(argc, argv, &index);
        if (flag == -1)
            break;

        switch (flag) {
            case 'V':
                crm_bump_log_level(argc, argv);
                break;
            case 's':
                stand_alone = TRUE;
                preserve_status = TRUE;
                cib_writes_enabled = FALSE;

                pwentry = getpwnam(CRM_DAEMON_USER);
                CRM_CHECK(pwentry != NULL,
                          crm_perror(LOG_ERR, "Invalid uid (%s) specified", CRM_DAEMON_USER);
                          return 100);

                rc = setgid(pwentry->pw_gid);
                if (rc < 0) {
                    crm_perror(LOG_ERR, "Could not set group to %d", pwentry->pw_gid);
                    return 100;
                }

                rc = initgroups(CRM_DAEMON_GROUP, pwentry->pw_gid);
                if (rc < 0) {
                    crm_perror(LOG_ERR, "Could not setup groups for user %d", pwentry->pw_uid);
                    return 100;
                }

                rc = setuid(pwentry->pw_uid);
                if (rc < 0) {
                    crm_perror(LOG_ERR, "Could not set user to %d", pwentry->pw_uid);
                    return 100;
                }
                break;
            case '?':          /* Help message */
                crm_help(flag, EX_OK);
                break;
            case 'w':
                cib_writes_enabled = TRUE;
                break;
            case 'r':
                cib_root = optarg;
                break;
            case 'm':
                cib_metadata();
                return 0;
            default:
                ++argerr;
                break;
        }
    }
    if (argc - optind == 1 && safe_str_eq("metadata", argv[optind])) {
        cib_metadata();
        return 0;
    }

    if (optind > argc) {
        ++argerr;
    }

    if (argerr) {
        crm_help('?', EX_USAGE);
    }

    crm_log_init(NULL, LOG_INFO, TRUE, FALSE, argc, argv, FALSE);
    if (cib_root == NULL) {
        char *path = crm_strdup_printf("%s/cib.xml", CRM_CONFIG_DIR);
        char *legacy = crm_strdup_printf("%s/cib.xml", CRM_LEGACY_CONFIG_DIR);

        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
            cib_root = CRM_CONFIG_DIR;

        } else if (g_file_test(legacy, G_FILE_TEST_EXISTS)) {
            cib_root = CRM_LEGACY_CONFIG_DIR;
            crm_notice("Using legacy config location: %s", cib_root);

        } else {
            cib_root = CRM_CONFIG_DIR;
            crm_notice("Using new config location: %s", cib_root);
        }

        free(legacy);
        free(path);

    } else {
        crm_notice("Using custom config location: %s", cib_root);
    }

    if (crm_is_writable(cib_root, NULL, CRM_DAEMON_USER, CRM_DAEMON_GROUP, FALSE) == FALSE) {
        crm_err("Bad permissions on %s. Terminating", cib_root);
        fprintf(stderr, "ERROR: Bad permissions on %s. See logs for details\n", cib_root);
        fflush(stderr);
        return 100;
    }

    /* read local config file */
    rc = cib_init();

    CRM_CHECK(crm_hash_table_size(client_connections) == 0,
              crm_warn("Not all clients gone at exit"));
    g_hash_table_foreach(client_connections, log_cib_client, NULL);
    cib_cleanup();

#if SUPPORT_HEARTBEAT
    if (hb_conn) {
        hb_conn->llc_ops->delete(hb_conn);
    }
#endif

    crm_info("Done");
    return rc;
}

void
cib_cleanup(void)
{
    crm_peer_destroy();
    if (local_notify_queue) {
        g_hash_table_destroy(local_notify_queue);
    }
    crm_client_cleanup();
    g_hash_table_destroy(config_hash);
    free(cib_our_uname);
    free(channel1);
    free(channel2);
    free(channel3);
    free(channel4);
    free(channel5);
}

unsigned long cib_num_ops = 0;
const char *cib_stat_interval = "10min";
unsigned long cib_num_local = 0, cib_num_updates = 0, cib_num_fail = 0;
unsigned long cib_bad_connects = 0, cib_num_timeouts = 0;

#if SUPPORT_HEARTBEAT
gboolean ccm_connect(void);

static void
ccm_connection_destroy(gpointer user_data)
{
    crm_err("CCM connection failed... blocking while we reconnect");
    CRM_ASSERT(ccm_connect());
    return;
}

static void *ccm_library = NULL;

gboolean
ccm_connect(void)
{
    gboolean did_fail = TRUE;
    int num_ccm_fails = 0;
    int max_ccm_fails = 30;
    int ret;
    int cib_ev_fd;

    int (*ccm_api_register) (oc_ev_t ** token) =
        find_library_function(&ccm_library, CCM_LIBRARY, "oc_ev_register", 1);

    int (*ccm_api_set_callback) (const oc_ev_t * token,
                                 oc_ev_class_t class,
                                 oc_ev_callback_t * fn,
                                 oc_ev_callback_t ** prev_fn) =
        find_library_function(&ccm_library, CCM_LIBRARY, "oc_ev_set_callback", 1);

    void (*ccm_api_special) (const oc_ev_t *, oc_ev_class_t, int) =
        find_library_function(&ccm_library, CCM_LIBRARY, "oc_ev_special", 1);
    int (*ccm_api_activate) (const oc_ev_t * token, int *fd) =
        find_library_function(&ccm_library, CCM_LIBRARY, "oc_ev_activate", 1);
    int (*ccm_api_unregister) (oc_ev_t * token) =
        find_library_function(&ccm_library, CCM_LIBRARY, "oc_ev_unregister", 1);

    static struct mainloop_fd_callbacks ccm_fd_callbacks = {
        .dispatch = cib_ccm_dispatch,
        .destroy = ccm_connection_destroy,
    };

    while (did_fail) {
        did_fail = FALSE;
        crm_info("Registering with CCM...");
        ret = (*ccm_api_register) (&cib_ev_token);
        if (ret != 0) {
            did_fail = TRUE;
        }

        if (did_fail == FALSE) {
            crm_trace("Setting up CCM callbacks");
            ret = (*ccm_api_set_callback) (cib_ev_token, OC_EV_MEMB_CLASS,
                                           cib_ccm_msg_callback, NULL);
            if (ret != 0) {
                crm_warn("CCM callback not set");
                did_fail = TRUE;
            }
        }
        if (did_fail == FALSE) {
            (*ccm_api_special) (cib_ev_token, OC_EV_MEMB_CLASS, 0);

            crm_trace("Activating CCM token");
            ret = (*ccm_api_activate) (cib_ev_token, &cib_ev_fd);
            if (ret != 0) {
                crm_warn("CCM Activation failed");
                did_fail = TRUE;
            }
        }

        if (did_fail) {
            num_ccm_fails++;
            (*ccm_api_unregister) (cib_ev_token);

            if (num_ccm_fails < max_ccm_fails) {
                crm_warn("CCM Connection failed %d times (%d max)", num_ccm_fails, max_ccm_fails);
                sleep(3);

            } else {
                crm_err("CCM Activation failed %d (max) times", num_ccm_fails);
                return FALSE;
            }
        }
    }

    crm_debug("CCM Activation passed... all set to go!");
    mainloop_add_fd("heartbeat-ccm", G_PRIORITY_MEDIUM, cib_ev_fd, cib_ev_token, &ccm_fd_callbacks);

    return TRUE;
}
#endif

#if SUPPORT_COROSYNC
static void
cib_cs_dispatch(cpg_handle_t handle,
                 const struct cpg_name *groupName,
                 uint32_t nodeid, uint32_t pid, void *msg, size_t msg_len)
{
    uint32_t kind = 0;
    xmlNode *xml = NULL;
    const char *from = NULL;
    char *data = pcmk_message_common_cs(handle, nodeid, pid, msg, &kind, &from);

    if(data == NULL) {
        return;
    }
    if (kind == crm_class_cluster) {
        xml = string2xml(data);
        if (xml == NULL) {
            crm_err("Invalid XML: '%.120s'", data);
            free(data);
            return;
        }
        crm_xml_add(xml, F_ORIG, from);
        /* crm_xml_add_int(xml, F_SEQ, wrapper->id); */
        cib_peer_callback(xml, NULL);
    }

    free_xml(xml);
    free(data);
}

static void
cib_cs_destroy(gpointer user_data)
{
    if (cib_shutdown_flag) {
        crm_info("Corosync disconnection complete");
    } else {
        crm_err("Corosync connection lost!  Exiting.");
        terminate_cib(__FUNCTION__, -1);
    }
}
#endif

static void
cib_peer_update_callback(enum crm_status_type type, crm_node_t * node, const void *data)
{
    switch (type) {
        case crm_status_processes:
            if (legacy_mode && is_not_set(node->processes, crm_get_cluster_proc())) {
                uint32_t old = data? *(const uint32_t *)data : 0;

                if ((node->processes ^ old) & crm_proc_cpg) {
                    crm_info("Attempting to disable legacy mode after %s left the cluster",
                             node->uname);
                    legacy_mode = FALSE;
                }
            }
            break;

        case crm_status_uname:
        case crm_status_rstate:
        case crm_status_nstate:
            if (cib_shutdown_flag && (crm_active_peers() < 2)
                && crm_hash_table_size(client_connections) == 0) {

                crm_info("No more peers");
                terminate_cib(__FUNCTION__, 1);
            }
            break;
    }
}

#if SUPPORT_HEARTBEAT
static void
cib_ha_connection_destroy(gpointer user_data)
{
    if (cib_shutdown_flag) {
        crm_info("Heartbeat disconnection complete... exiting");
        terminate_cib(__FUNCTION__, 0);
    } else {
        crm_err("Heartbeat connection lost!  Exiting.");
        terminate_cib(__FUNCTION__, -1);
    }
}
#endif

int
cib_init(void)
{
    if (is_openais_cluster()) {
#if SUPPORT_COROSYNC
        crm_cluster.destroy = cib_cs_destroy;
        crm_cluster.cpg.cpg_deliver_fn = cib_cs_dispatch;
        crm_cluster.cpg.cpg_confchg_fn = pcmk_cpg_membership;
#endif
    } else if (is_heartbeat_cluster()) {
#if SUPPORT_HEARTBEAT
        crm_cluster.hb_dispatch = cib_ha_peer_callback;
        crm_cluster.destroy = cib_ha_connection_destroy;
#endif
    }

    config_hash =
        g_hash_table_new_full(crm_str_hash, g_str_equal, g_hash_destroy_str, g_hash_destroy_str);

    if (startCib("cib.xml") == FALSE) {
        crm_crit("Cannot start CIB... terminating");
        crm_exit(ENODATA);
    }

    if (stand_alone == FALSE) {
        if (crm_cluster_connect(&crm_cluster) == FALSE) {
            crm_crit("Cannot sign in to the cluster... terminating");
            crm_exit(DAEMON_RESPAWN_STOP);
        }
        cib_our_uname = crm_cluster.uname;
        if (is_openais_cluster()) {
            crm_set_status_callback(&cib_peer_update_callback);
        }
#if SUPPORT_HEARTBEAT
        if (is_heartbeat_cluster()) {

            gboolean was_error = FALSE;

            hb_conn = crm_cluster.hb_conn;
            if (was_error == FALSE) {
                if (HA_OK !=
                    hb_conn->llc_ops->set_cstatus_callback(hb_conn, cib_client_status_callback,
                                                           hb_conn)) {

                    crm_err("Cannot set cstatus callback: %s", hb_conn->llc_ops->errmsg(hb_conn));
                    was_error = TRUE;
                }
            }

            if (was_error == FALSE) {
                was_error = (ccm_connect() == FALSE);
            }

            if (was_error == FALSE) {
                /* Async get client status information in the cluster */
                crm_info("Requesting the list of configured nodes");
                hb_conn->llc_ops->client_status(hb_conn, NULL, CRM_SYSTEM_CIB, -1);
            }
        }
#endif

    } else {
        cib_our_uname = strdup("localhost");
    }

    cib_ipc_servers_init(&ipcs_ro,
                         &ipcs_rw,
                         &ipcs_shm,
                         &ipc_ro_callbacks,
                         &ipc_rw_callbacks);

    if (stand_alone) {
        cib_is_master = TRUE;
    }

    /* Create the mainloop and run it... */
    mainloop = g_main_new(FALSE);
    crm_info("Starting %s mainloop", crm_system_name);
    g_main_run(mainloop);

    /* If main loop returned, clean up and exit. We disconnect in case
     * terminate_cib() was called with fast=1.
     */
    crm_cluster_disconnect(&crm_cluster);
    cib_ipc_servers_destroy(ipcs_ro, ipcs_rw, ipcs_shm);

    return crm_exit(pcmk_ok);
}

gboolean
startCib(const char *filename)
{
    gboolean active = FALSE;
    xmlNode *cib = readCibXmlFile(cib_root, filename, !preserve_status);

    CRM_ASSERT(cib != NULL);

    if (activateCibXml(cib, TRUE, "start") == 0) {
        int port = 0;
        const char *port_s = NULL;

        active = TRUE;

        cib_read_config(config_hash, cib);

        port_s = crm_element_value(cib, "remote-tls-port");
        if (port_s) {
            port = crm_parse_int(port_s, "0");
            remote_tls_fd = init_remote_listener(port, TRUE);
        }

        port_s = crm_element_value(cib, "remote-clear-port");
        if (port_s) {
            port = crm_parse_int(port_s, "0");
            remote_fd = init_remote_listener(port, FALSE);
        }

        crm_info("CIB Initialization completed successfully");
    }

    return active;
}
