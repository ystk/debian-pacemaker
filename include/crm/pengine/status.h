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
#ifndef PENGINE_STATUS__H
#  define PENGINE_STATUS__H

#  include <glib.h>
#  include <crm/common/iso8601.h>
#  include <crm/pengine/common.h>

typedef struct node_s pe_node_t;
typedef struct node_s node_t;
typedef struct pe_action_s action_t;
typedef struct pe_action_s pe_action_t;
typedef struct resource_s resource_t;
typedef struct ticket_s ticket_t;

typedef enum no_quorum_policy_e {
    no_quorum_freeze,
    no_quorum_stop,
    no_quorum_ignore,
    no_quorum_suicide
} no_quorum_policy_t;

enum node_type {
    node_ping,
    node_member,
    node_remote
};

enum pe_restart {
    pe_restart_restart,
    pe_restart_ignore
};

enum pe_find {
    pe_find_renamed = 0x001,
    pe_find_clone = 0x004,
    pe_find_current = 0x008,
    pe_find_inactive = 0x010,
};

#  define pe_flag_have_quorum		0x00000001ULL
#  define pe_flag_symmetric_cluster	0x00000002ULL
#  define pe_flag_is_managed_default	0x00000004ULL
#  define pe_flag_maintenance_mode	0x00000008ULL

#  define pe_flag_stonith_enabled	0x00000010ULL
#  define pe_flag_have_stonith_resource	0x00000020ULL
#  define pe_flag_enable_unfencing	0x00000040ULL

#  define pe_flag_stop_rsc_orphans	0x00000100ULL
#  define pe_flag_stop_action_orphans	0x00000200ULL
#  define pe_flag_stop_everything	0x00000400ULL

#  define pe_flag_start_failure_fatal	0x00001000ULL
#  define pe_flag_remove_after_stop	0x00002000ULL

#  define pe_flag_startup_probes	0x00010000ULL
#  define pe_flag_have_status		0x00020000ULL
#  define pe_flag_have_remote_nodes	0x00040000ULL

#  define pe_flag_quick_location  	0x00100000ULL
#  define pe_flag_sanitized             0x00200000ULL

typedef struct pe_working_set_s {
    xmlNode *input;
    crm_time_t *now;

    /* options extracted from the input */
    char *dc_uuid;
    node_t *dc_node;
    const char *stonith_action;
    const char *placement_strategy;

    unsigned long long flags;

    int stonith_timeout;
    int default_resource_stickiness;
    no_quorum_policy_t no_quorum_policy;

    GHashTable *config_hash;
    GHashTable *tickets;
    GHashTable *singletons; /* Actions for which there can be only one - ie. fence nodeX */

    GListPtr nodes;
    GListPtr resources;
    GListPtr placement_constraints;
    GListPtr ordering_constraints;
    GListPtr colocation_constraints;
    GListPtr ticket_constraints;

    GListPtr actions;
    xmlNode *failed;
    xmlNode *op_defaults;
    xmlNode *rsc_defaults;

    /* stats */
    int num_synapse;
    int max_valid_nodes;
    int order_id;
    int action_id;

    /* final output */
    xmlNode *graph;

    GHashTable *template_rsc_sets;
    const char *localhost;
    GHashTable *tags;

} pe_working_set_t;

struct node_shared_s {
    const char *id;
    const char *uname;
/* Make all these flags into a bitfield one day */
    gboolean online;
    gboolean standby;
    gboolean standby_onfail;
    gboolean pending;
    gboolean unclean;
    gboolean unseen;
    gboolean shutdown;
    gboolean expected_up;
    gboolean is_dc;

    int num_resources;
    GListPtr running_rsc;       /* resource_t* */
    GListPtr allocated_rsc;     /* resource_t* */

    resource_t *remote_rsc;

    GHashTable *attrs;          /* char* => char* */
    enum node_type type;

    GHashTable *utilization;

    /*! cache of calculated rsc digests for this node. */
    GHashTable *digest_cache;

    gboolean maintenance;
    gboolean rsc_discovery_enabled;
    gboolean remote_requires_reset;
    gboolean remote_was_fenced;
};

struct node_s {
    int weight;
    gboolean fixed;
    int count;
    struct node_shared_s *details;
    int rsc_discover_mode;
};

#  include <crm/pengine/complex.h>

#  define pe_rsc_orphan		0x00000001ULL
#  define pe_rsc_managed	0x00000002ULL
#  define pe_rsc_block          0x00000004ULL   /* Further operations are prohibited due to failure policy */
#  define pe_rsc_orphan_container_filler	0x00000008ULL

#  define pe_rsc_notify		0x00000010ULL
#  define pe_rsc_unique		0x00000020ULL
#  define pe_rsc_fence_device   0x00000040ULL

#  define pe_rsc_provisional	0x00000100ULL
#  define pe_rsc_allocating	0x00000200ULL
#  define pe_rsc_merging	0x00000400ULL
#  define pe_rsc_munging	0x00000800ULL

#  define pe_rsc_try_reload     0x00001000ULL
#  define pe_rsc_reload         0x00002000ULL

#  define pe_rsc_failed		0x00010000ULL
#  define pe_rsc_shutdown	0x00020000ULL
#  define pe_rsc_runnable	0x00040000ULL
#  define pe_rsc_start_pending	0x00080000ULL

#  define pe_rsc_starting       0x00100000ULL
#  define pe_rsc_stopping       0x00200000ULL
#  define pe_rsc_migrating      0x00400000ULL
#  define pe_rsc_allow_migrate  0x00800000ULL

#  define pe_rsc_failure_ignored 0x01000000ULL
#  define pe_rsc_unexpectedly_running 0x02000000ULL
#  define pe_rsc_maintenance	 0x04000000ULL

#  define pe_rsc_needs_quorum	 0x10000000ULL
#  define pe_rsc_needs_fencing	 0x20000000ULL
#  define pe_rsc_needs_unfencing 0x40000000ULL
#  define pe_rsc_have_unfencing  0x80000000ULL

enum pe_graph_flags {
    pe_graph_none = 0x00000,
    pe_graph_updated_first = 0x00001,
    pe_graph_updated_then = 0x00002,
    pe_graph_disable = 0x00004,
};

/* *INDENT-OFF* */
enum pe_action_flags {
    pe_action_pseudo = 0x00001,
    pe_action_runnable = 0x00002,
    pe_action_optional = 0x00004,
    pe_action_print_always = 0x00008,

    pe_action_have_node_attrs = 0x00010,
    pe_action_failure_is_fatal = 0x00020, /* no longer used, here for API compatibility */
    pe_action_implied_by_stonith = 0x00040,
    pe_action_migrate_runnable =   0x00080,

    pe_action_dumped = 0x00100,
    pe_action_processed = 0x00200,
    pe_action_clear = 0x00400,
    pe_action_dangle = 0x00800,

    pe_action_requires_any = 0x01000, /* This action requires one or mre of its dependencies to be runnable
                                       * We use this to clear the runnable flag before checking dependencies
                                       */
    pe_action_reschedule = 0x02000,
    pe_action_tracking = 0x04000,
};
/* *INDENT-ON* */

struct resource_s {
    char *id;
    char *clone_name;
    xmlNode *xml;
    xmlNode *orig_xml;
    xmlNode *ops_xml;

    resource_t *parent;
    void *variant_opaque;
    enum pe_obj_types variant;
    resource_object_functions_t *fns;
    resource_alloc_functions_t *cmds;

    enum rsc_recovery_type recovery_type;
    enum pe_restart restart_type;

    int priority;
    int stickiness;
    int sort_index;
    int failure_timeout;
    int effective_priority;
    int migration_threshold;

    gboolean is_remote_node;

    unsigned long long flags;

    GListPtr rsc_cons_lhs;      /* rsc_colocation_t* */
    GListPtr rsc_cons;          /* rsc_colocation_t* */
    GListPtr rsc_location;      /* rsc_to_node_t*    */
    GListPtr actions;           /* action_t*         */
    GListPtr rsc_tickets;       /* rsc_ticket*       */

    node_t *allocated_to;
    GListPtr running_on;        /* node_t*   */
    GHashTable *known_on;       /* node_t*   */
    GHashTable *allowed_nodes;  /* node_t*   */

    enum rsc_role_e role;
    enum rsc_role_e next_role;

    GHashTable *meta;
    GHashTable *parameters;
    GHashTable *utilization;

    GListPtr children;          /* resource_t*   */
    GListPtr dangling_migrations;       /* node_t*       */

    node_t *partial_migration_target;
    node_t *partial_migration_source;

    resource_t *container;
    GListPtr fillers;

    char *pending_task;

    const char *isolation_wrapper;
    gboolean exclusive_discover;
    int remote_reconnect_interval;
};

struct pe_action_s {
    int id;
    int priority;

    resource_t *rsc;
    node_t *node;
    xmlNode *op_entry;

    char *task;
    char *uuid;
    char *cancel_task;

    enum pe_action_flags flags;
    enum rsc_start_requirement needs;
    enum action_fail_response on_fail;
    enum rsc_role_e fail_role;

    action_t *pre_notify;
    action_t *pre_notified;
    action_t *post_notify;
    action_t *post_notified;

    int seen_count;

    GHashTable *meta;
    GHashTable *extra;

    /* 
     * These two varables are associated with the constraint logic
     * that involves first having one or more actions runnable before
     * then allowing this action to execute.
     *
     * These varables are used with features such as 'clone-min' which
     * requires at minimum X number of cloned instances to be running
     * before an order dependency can run. Another option that uses
     * this is 'require-all=false' in ordering constrants. This option
     * says "only required one instance of a resource to start before
     * allowing dependencies to start" basicall require-all=false is
     * the same as clone-min=1.
     */

    /* current number of known runnable actions in the before list. */
    int runnable_before;
    /* the number of "before" runnable actions required for this action
     * to be considered runnable */ 
    int required_runnable_before;

    GListPtr actions_before;    /* action_warpper_t* */
    GListPtr actions_after;     /* action_warpper_t* */
};

struct ticket_s {
    char *id;
    gboolean granted;
    time_t last_granted;
    gboolean standby;
    GHashTable *state;
};

typedef struct tag_s {
    char *id;
    GListPtr refs;
} tag_t;

enum pe_link_state {
    pe_link_not_dumped,
    pe_link_dumped,
    pe_link_dup,
};

/* *INDENT-OFF* */
enum pe_ordering {
    pe_order_none                  = 0x0,       /* deleted */
    pe_order_optional              = 0x1,       /* pure ordering, nothing implied */
    pe_order_apply_first_non_migratable = 0x2,  /* Only apply this constraint's ordering if first is not migratable. */

    pe_order_implies_first         = 0x10,      /* If 'then' is required, ensure 'first' is too */
    pe_order_implies_then          = 0x20,      /* If 'first' is required, ensure 'then' is too */
    pe_order_implies_first_master  = 0x40,      /* Imply 'first' is required when 'then' is required and then's rsc holds Master role. */

    /* first requires then to be both runnable and migrate runnable. */
    pe_order_implies_first_migratable  = 0x80,

    pe_order_runnable_left         = 0x100,     /* 'then' requires 'first' to be runnable */

    pe_order_pseudo_left           = 0x200,     /* 'then' can only be pseudo if 'first' is runnable */
    pe_order_implies_then_on_node  = 0x400,     /* If 'first' is required on 'nodeX',
                                                 * ensure instances of 'then' on 'nodeX' are too.
                                                 * Only really useful if 'then' is a clone and 'first' is not
                                                 */

    pe_order_restart               = 0x1000,    /* 'then' is runnable if 'first' is optional or runnable */
    pe_order_stonith_stop          = 0x2000,    /* only applies if the action is non-pseudo */
    pe_order_serialize_only        = 0x4000,    /* serialize */

    pe_order_implies_first_printed = 0x10000,   /* Like ..implies_first but only ensures 'first' is printed, not manditory */
    pe_order_implies_then_printed  = 0x20000,   /* Like ..implies_then but only ensures 'then' is printed, not manditory */

    pe_order_asymmetrical          = 0x100000,  /* Indicates asymmetrical one way ordering constraint. */
    pe_order_load                  = 0x200000,  /* Only relevant if... */
    pe_order_one_or_more           = 0x400000,  /* 'then' is only runnable if one or more of it's dependencies are too */
    pe_order_anti_colocation       = 0x800000,

    pe_order_preserve              = 0x1000000, /* Hack for breaking user ordering constraints with container resources */
    pe_order_trace                 = 0x4000000, /* test marker */
};
/* *INDENT-ON* */

typedef struct action_wrapper_s action_wrapper_t;
struct action_wrapper_s {
    enum pe_ordering type;
    enum pe_link_state state;
    action_t *action;
};

const char *rsc_printable_id(resource_t *rsc);
gboolean cluster_status(pe_working_set_t * data_set);
void set_working_set_defaults(pe_working_set_t * data_set);
void cleanup_calculations(pe_working_set_t * data_set);
resource_t *pe_find_resource(GListPtr rsc_list, const char *id_rh);
node_t *pe_find_node(GListPtr node_list, const char *uname);
node_t *pe_find_node_id(GListPtr node_list, const char *id);
node_t *pe_find_node_any(GListPtr node_list, const char *id, const char *uname);
GListPtr find_operations(const char *rsc, const char *node, gboolean active_filter,
                         pe_working_set_t * data_set);
#endif
