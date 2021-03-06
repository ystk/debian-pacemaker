/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <crm_internal.h>

#include <pengine.h>
#include <crm/pengine/rules.h>
#include <lib/pengine/utils.h>
#include <crm/msg_xml.h>
#include <allocate.h>
#include <utils.h>

#define DELETE_THEN_REFRESH 1 /* The crmd will remove the resource from the CIB itself, making this redundant */

#define VARIANT_NATIVE 1
#include <lib/pengine/variant.h>

void native_rsc_colocation_rh_must(resource_t *rsc_lh, gboolean update_lh,
				   resource_t *rsc_rh, gboolean update_rh);

void native_rsc_colocation_rh_mustnot(resource_t *rsc_lh, gboolean update_lh,
				      resource_t *rsc_rh, gboolean update_rh);

void Recurring(resource_t *rsc, action_t *start, node_t *node,
			      pe_working_set_t *data_set);
void RecurringOp(resource_t *rsc, action_t *start, node_t *node,
		 xmlNode *operation, pe_working_set_t *data_set);
void pe_post_notify(
	resource_t *rsc, node_t *node, action_t *op, 
	notify_data_t *n_data, pe_working_set_t *data_set);

void NoRoleChange  (resource_t *rsc, node_t *current, node_t *next, pe_working_set_t *data_set);
gboolean DeleteRsc (resource_t *rsc, node_t *node, gboolean optional, pe_working_set_t *data_set);
gboolean StopRsc   (resource_t *rsc, node_t *next, gboolean optional, pe_working_set_t *data_set);
gboolean StartRsc  (resource_t *rsc, node_t *next, gboolean optional, pe_working_set_t *data_set);
gboolean DemoteRsc (resource_t *rsc, node_t *next, gboolean optional, pe_working_set_t *data_set);
gboolean PromoteRsc(resource_t *rsc, node_t *next, gboolean optional, pe_working_set_t *data_set);
gboolean RoleError (resource_t *rsc, node_t *next, gboolean optional, pe_working_set_t *data_set);
gboolean NullOp    (resource_t *rsc, node_t *next, gboolean optional, pe_working_set_t *data_set);

enum rsc_role_e rsc_state_matrix[RSC_ROLE_MAX][RSC_ROLE_MAX] = {
/* Current State */	
/*    Next State:  Unknown 	    Stopped	      Started	        Slave	          Master */
/* Unknown */	{ RSC_ROLE_UNKNOWN, RSC_ROLE_STOPPED, RSC_ROLE_STOPPED, RSC_ROLE_STOPPED, RSC_ROLE_STOPPED, },
/* Stopped */	{ RSC_ROLE_STOPPED, RSC_ROLE_STOPPED, RSC_ROLE_STARTED, RSC_ROLE_SLAVE,   RSC_ROLE_SLAVE, },
/* Started */	{ RSC_ROLE_STOPPED, RSC_ROLE_STOPPED, RSC_ROLE_STARTED, RSC_ROLE_SLAVE,   RSC_ROLE_MASTER, },
/* Slave */	{ RSC_ROLE_STOPPED, RSC_ROLE_STOPPED, RSC_ROLE_UNKNOWN, RSC_ROLE_SLAVE,   RSC_ROLE_MASTER, },
/* Master */	{ RSC_ROLE_STOPPED, RSC_ROLE_SLAVE,   RSC_ROLE_UNKNOWN, RSC_ROLE_SLAVE,   RSC_ROLE_MASTER, },
};

gboolean (*rsc_action_matrix[RSC_ROLE_MAX][RSC_ROLE_MAX])(resource_t*,node_t*,gboolean,pe_working_set_t*) = {
/* Current State */	
/*    Next State: Unknown	Stopped		Started		Slave		Master */
/* Unknown */	{ RoleError,	StopRsc,	RoleError,	RoleError,	RoleError,  },
/* Stopped */	{ RoleError,	NullOp,		StartRsc,	StartRsc,	RoleError,  },
/* Started */	{ RoleError,	StopRsc,	NullOp,		NullOp,		PromoteRsc, },
/* Slave */	{ RoleError,	StopRsc,	RoleError,	NullOp,		PromoteRsc, },
/* Master */	{ RoleError,	RoleError,	RoleError,	DemoteRsc,	NullOp,     },
};


static gboolean
native_choose_node(resource_t *rsc)
{
	/*
	  1. Sort by weight
	  2. color.chosen_node = the node (of those with the highest wieght)
				   with the fewest resources
	  3. remove color.chosen_node from all other colors
	*/
	GListPtr nodes = NULL;
	node_t *chosen = NULL;

	int lpc = 0;
	int multiple = 0;
	int length = g_list_length(rsc->allowed_nodes);

	if(is_not_set(rsc->flags, pe_rsc_provisional)) {
		return rsc->allocated_to?TRUE:FALSE;
	}
	
	crm_debug_3("Choosing node for %s from %d candidates",
		    rsc->id, length);

	if(rsc->allowed_nodes) {
	    rsc->allowed_nodes = g_list_sort(rsc->allowed_nodes, sort_node_weight);
	    nodes = rsc->allowed_nodes;
	    chosen = g_list_nth_data(nodes, 0);

	    if(chosen
	       && chosen->weight > 0
	       && can_run_resources(chosen)) {
		node_t *running = g_list_nth_data(rsc->running_on, 0);
		if(can_run_resources(running) == FALSE) {
		    running = NULL;
		}
		
		for(lpc = 1; lpc < length; lpc++) {
		    node_t *tmp = g_list_nth_data(nodes, lpc);
		    if(tmp->weight == chosen->weight) {
			multiple++;
			if(running && tmp->details == running->details) {
			    /* prefer the existing node if scores are equal */
			    chosen = tmp;
			}
		    }
		}
	    }
	}

	if(multiple > 1) {
		int log_level = LOG_INFO;
		char *score = score2char(chosen->weight);
		if(chosen->weight >= INFINITY) {
			log_level = LOG_WARNING;
		}
		
		do_crm_log(log_level, "%d nodes with equal score (%s) for"
			   " running %s resources.  Chose %s.",
			   multiple, score, rsc->id, chosen->details->uname);
		crm_free(score);
	}
	
	
	return native_assign_node(rsc, nodes, chosen, FALSE);
}

int node_list_attr_score(GListPtr list, const char *attr, const char *value) 
{
    int best_score = -INFINITY;
    const char *best_node = NULL;

    if(attr == NULL) {
	attr = "#"XML_ATTR_UNAME;
    }

    slist_iter(node, node_t, list, lpc,
	       int weight = node->weight;
	       if(can_run_resources(node) == FALSE) {
		   weight = -INFINITY;
	       }
	       if(weight > best_score || best_node == NULL) {
		   const char *tmp = g_hash_table_lookup(node->details->attrs, attr);
		   if(safe_str_eq(value, tmp)) {
		       best_score = weight;
		       best_node = node->details->uname;
		   }
	       }
	);

    if(safe_str_neq(attr, "#"XML_ATTR_UNAME)) {
	crm_info("Best score for %s=%s was %s with %d",
		attr, value, best_node?best_node:"<none>", best_score);
    }
    
    return best_score;
}


static void
node_list_update(GListPtr list1, GListPtr list2, const char *attr, int factor)
{
    int score = 0;
    if(attr == NULL) {
	attr = "#"XML_ATTR_UNAME;
    }
    
    slist_iter(
	node, node_t, list1, lpc,
	
	CRM_CHECK(node != NULL, continue);
	score = node_list_attr_score(list2, attr, g_hash_table_lookup(node->details->attrs, attr));
	
	if(factor < 0 && score < 0) {
	    /* Negative preference for a node with a negative score
	     * should not become a positive preference
	     *
	     * TODO: Decide if we want to filter only if weight == -INFINITY
	     *
	     */
	    continue;
	}
	crm_debug_2("%s: %d + %d*%d",
		    node->details->uname, node->weight, factor, score);
	node->weight = merge_weights(factor*score, node->weight);
	);
}

GListPtr
native_merge_weights(
    resource_t *rsc, const char *rhs, GListPtr nodes, const char *attr, int factor, gboolean allow_rollback) 
{
    GListPtr work = NULL;
    int multiplier = 1;
    if(factor < 0) {
	multiplier = -1;
    }

    if(is_set(rsc->flags, pe_rsc_merging)) {
	crm_info("%s: Breaking dependency loop at %s", rhs, rsc->id);
	return nodes;
    }

    set_bit(rsc->flags, pe_rsc_merging);
    crm_debug_2("%s: Combining scores from %s", rhs, rsc->id);

    work = node_list_dup(nodes, FALSE, FALSE);
    node_list_update(work, rsc->allowed_nodes, attr, factor);
    
    if(allow_rollback && can_run_any(work) == FALSE) {
	crm_info("%s: Rolling back scores from %s", rhs, rsc->id);
	slist_destroy(node_t, n, work, crm_free(n));
	clear_bit(rsc->flags, pe_rsc_merging);
	return nodes;
    }

    if(can_run_any(work)) {
	slist_iter(
	    constraint, rsc_colocation_t, rsc->rsc_cons_lhs, lpc,

	    crm_debug_2("Applying %s", constraint->id);
	    work = constraint->rsc_lh->cmds->merge_weights(
		constraint->rsc_lh, rhs, work,
		constraint->node_attribute, 
		multiplier*constraint->score/INFINITY, allow_rollback);
	    );
    }

    slist_destroy(node_t, n, nodes, crm_free(n));
    clear_bit(rsc->flags, pe_rsc_merging);
    return work;
}

node_t *
native_color(resource_t *rsc, pe_working_set_t *data_set)
{
        int alloc_details = scores_log_level+1;
	const char *class = crm_element_value(rsc->xml, XML_AGENT_ATTR_CLASS);
	if(rsc->parent && is_not_set(rsc->parent->flags, pe_rsc_allocating)) {
		/* never allocate children on their own */
		crm_debug("Escalating allocation of %s to its parent: %s",
			  rsc->id, rsc->parent->id);
		rsc->parent->cmds->color(rsc->parent, data_set);
	}
	
	if(is_not_set(rsc->flags, pe_rsc_provisional)) {
		return rsc->allocated_to;
	}

	if(is_set(rsc->flags, pe_rsc_allocating)) {
		crm_debug("Dependency loop detected involving %s", rsc->id);
		return NULL;
	}

	set_bit(rsc->flags, pe_rsc_allocating);
	print_resource(alloc_details, "Allocating: ", rsc, FALSE);
	dump_node_scores(alloc_details, rsc, "Pre-allloc", rsc->allowed_nodes);

	slist_iter(
		constraint, rsc_colocation_t, rsc->rsc_cons, lpc,

		GListPtr archive = NULL;
		resource_t *rsc_rh = constraint->rsc_rh;
		crm_debug_2("%s: Pre-Processing %s (%s, %d, %s)",
			    rsc->id, constraint->id, rsc_rh->id,
			    constraint->score, role2text(constraint->role_lh));
		if(constraint->role_lh >= RSC_ROLE_MASTER
		   || (constraint->score < 0 && constraint->score > -INFINITY)) {
		    archive = node_list_dup(rsc->allowed_nodes, FALSE, FALSE);
		}
		rsc_rh->cmds->color(rsc_rh, data_set);
		rsc->cmds->rsc_colocation_lh(rsc, rsc_rh, constraint);	
		if(archive && can_run_any(rsc->allowed_nodes) == FALSE) {
		    crm_info("%s: Rolling back scores from %s", rsc->id, rsc_rh->id);
		    pe_free_shallow_adv(rsc->allowed_nodes, TRUE);
		    rsc->allowed_nodes = archive;
		    archive = NULL;
		}
		pe_free_shallow_adv(archive, TRUE);
	    );	

	dump_node_scores(alloc_details, rsc, "Post-coloc", rsc->allowed_nodes);

	slist_iter(
	    constraint, rsc_colocation_t, rsc->rsc_cons_lhs, lpc,
	    
	    rsc->allowed_nodes = constraint->rsc_lh->cmds->merge_weights(
		constraint->rsc_lh, rsc->id, rsc->allowed_nodes,
		constraint->node_attribute, constraint->score/INFINITY, TRUE);
	    );
	
	print_resource(LOG_DEBUG_2, "Allocating: ", rsc, FALSE);
	if(rsc->next_role == RSC_ROLE_STOPPED) {
		crm_debug_2("Making sure %s doesn't get allocated", rsc->id);
		/* make sure it doesnt come up again */
		resource_location(
			rsc, NULL, -INFINITY, XML_RSC_ATTR_TARGET_ROLE, data_set);
	}

	dump_node_scores(show_scores?0:scores_log_level, rsc, __PRETTY_FUNCTION__, rsc->allowed_nodes);
	if(is_set(data_set->flags, pe_flag_stonith_enabled)
	       && is_set(data_set->flags, pe_flag_have_stonith_resource) == FALSE) {
	    clear_bit(rsc->flags, pe_rsc_managed);
	}

	if(is_not_set(rsc->flags, pe_rsc_managed)) {
	    const char *reason = NULL;
	    node_t *assign_to = NULL;
	    if(rsc->running_on == NULL) {
		reason = "inactive";
	    } else if(rsc->role == RSC_ROLE_MASTER) {
		assign_to = rsc->running_on->data;
		reason = "master";
	    } else if(is_set(rsc->flags, pe_rsc_failed)) {
		reason = "failed";		
	    } else {
		assign_to = rsc->running_on->data;
		reason = "active";
	    }
	    crm_info("Unmanaged resource %s allocated to %s: %s", rsc->id,
		     assign_to?assign_to->details->uname:"'nowhere'", reason);
	    native_assign_node(rsc, NULL, assign_to, TRUE);

	} else if(is_set(data_set->flags, pe_flag_stop_everything)
		  && safe_str_neq(class, "stonith")) {
	    crm_debug("Forcing %s to stop", rsc->id);
	    native_assign_node(rsc, NULL, NULL, TRUE);

	} else if(is_set(rsc->flags, pe_rsc_provisional)
	   && native_choose_node(rsc) ) {
		crm_debug_3("Allocated resource %s to %s",
			    rsc->id, rsc->allocated_to->details->uname);

	} else if(rsc->allocated_to == NULL) {
		if(is_not_set(rsc->flags, pe_rsc_orphan)) {
			crm_info("Resource %s cannot run anywhere", rsc->id);
		} else if(rsc->running_on != NULL) {
			crm_info("Stopping orphan resource %s", rsc->id);
		}
		
	} else {
		crm_debug("Pre-Allocated resource %s to %s",
			  rsc->id, rsc->allocated_to->details->uname);
	}
	
	clear_bit(rsc->flags, pe_rsc_allocating);
	print_resource(LOG_DEBUG_3, "Allocated ", rsc, TRUE);

	return rsc->allocated_to;
}

static gboolean is_op_dup(
    resource_t *rsc, const char *name, const char *interval) 
{
    gboolean dup = FALSE;
    const char *id = NULL;
    const char *value = NULL;
    xml_child_iter_filter(
	rsc->ops_xml, operation, "op",
	value = crm_element_value(operation, "name");
	if(safe_str_neq(value, name)) {
	    continue;
	}
	
	value = crm_element_value(operation, XML_LRM_ATTR_INTERVAL);
	if(value == NULL) {
	    value = "0";
	}
	
	if(safe_str_neq(value, interval)) {
	    continue;
	}

	if(id == NULL) {
	    id = ID(operation);
	    
	} else {
	    crm_config_err("Operation %s is a duplicate of %s", ID(operation), id);
	    crm_config_err("Do not use the same (name, interval) combination more than once per resource");
	    dup = TRUE;
	}
	);
    
    return dup;
}

void
RecurringOp(resource_t *rsc, action_t *start, node_t *node,
	    xmlNode *operation, pe_working_set_t *data_set) 
{
	char *key = NULL;
	const char *name = NULL;
	const char *value = NULL;
	const char *interval = NULL;
	const char *node_uname = NULL;

	unsigned long long interval_ms = 0;
	action_t *mon = NULL;
	gboolean is_optional = TRUE;
	GListPtr possible_matches = NULL;
	
	crm_debug_2("Creating recurring action %s for %s in role %s",
		    ID(operation), rsc->id, role2text(rsc->next_role));
	
	if(node != NULL) {
		node_uname = node->details->uname;
	}

	interval = crm_element_value(operation, XML_LRM_ATTR_INTERVAL);
	interval_ms = crm_get_interval(interval);
	
	if(interval_ms == 0) {
	    return;
	}
	
	name = crm_element_value(operation, "name");
	if(is_op_dup(rsc, name, interval)) {
	    return;
	}

	key = generate_op_key(rsc->id, name, interval_ms);
	if(find_rsc_op_entry(rsc, key) == NULL) {
	    /* disabled */
	    return;
	}
	
	if(start != NULL) {
		crm_debug_3("Marking %s %s due to %s",
			    key, start->optional?"optional":"manditory",
			    start->uuid);
		is_optional = start->optional;
	} else {
		crm_debug_2("Marking %s optional", key);
		is_optional = TRUE;
	}

	/* start a monitor for an already active resource */
	possible_matches = find_actions_exact(rsc->actions, key, node);
	if(possible_matches == NULL) {
		is_optional = FALSE;
		crm_debug_3("Marking %s manditory: not active", key);
	} else {
		g_list_free(possible_matches);
	}
	
	value = crm_element_value(operation, "role");
	if((rsc->next_role == RSC_ROLE_MASTER && value == NULL)
	   || (value != NULL && text2role(value) != rsc->next_role)) {
		int log_level = LOG_DEBUG_2;
		const char *result = "Ignoring";
		if(is_optional) {
			char *local_key = crm_strdup(key);
			log_level = LOG_INFO;
			result = "Cancelling";
			/* its running : cancel it */
			
			mon = custom_action(
				rsc, local_key, RSC_CANCEL, node,
				FALSE, TRUE, data_set);

			crm_free(mon->task);
			mon->task = crm_strdup(RSC_CANCEL);
			add_hash_param(mon->meta, XML_LRM_ATTR_INTERVAL, interval);
			add_hash_param(mon->meta, XML_LRM_ATTR_TASK, name);

			local_key = NULL;

			switch(rsc->role) {
			    case RSC_ROLE_SLAVE:
			    case RSC_ROLE_STARTED:
				if(rsc->next_role == RSC_ROLE_MASTER) {
				    local_key = promote_key(rsc);

				} else if(rsc->next_role == RSC_ROLE_STOPPED) {
				    local_key = stop_key(rsc);
				}
				
				break;
			    case RSC_ROLE_MASTER:
				local_key = demote_key(rsc);
				break;
			    default:
				break;
			}

			if(local_key) {
			    custom_action_order(rsc, NULL, mon, rsc, local_key, NULL,
						pe_order_runnable_left, data_set);
			}
			
			mon = NULL;
		}
		
		do_crm_log(log_level, "%s action %s (%s vs. %s)",
			   result , key, value?value:role2text(RSC_ROLE_SLAVE),
			   role2text(rsc->next_role));

		crm_free(key);
		key = NULL;
		return;
	}		
		
	mon = custom_action(rsc, key, name, node,
			    is_optional, TRUE, data_set);
	key = mon->uuid;
	if(is_optional) {
		crm_debug_2("%s\t   %s (optional)",
			    crm_str(node_uname), mon->uuid);
	}
	
	if(start == NULL || start->runnable == FALSE) {
		crm_debug("%s\t   %s (cancelled : start un-runnable)",
			  crm_str(node_uname), mon->uuid);
		mon->runnable = FALSE;
		
	} else if(node == NULL
		  || node->details->online == FALSE
		  || node->details->unclean) {
		crm_debug("%s\t   %s (cancelled : no node available)",
			  crm_str(node_uname), mon->uuid);
		mon->runnable = FALSE;
		
	} else if(mon->optional == FALSE) {
	    crm_notice(" Start recurring %s (%llus) for %s on %s", mon->task, interval_ms/1000, rsc->id, crm_str(node_uname));
	}

	if(rsc->next_role == RSC_ROLE_MASTER) {
	    char *running_master = crm_itoa(EXECRA_RUNNING_MASTER);
	    add_hash_param(mon->meta, XML_ATTR_TE_TARGET_RC, running_master);
	    crm_free(running_master);
	}
	    
	if(node == NULL || is_set(rsc->flags, pe_rsc_managed)) {
	    custom_action_order(rsc, start_key(rsc), NULL,
			    NULL, crm_strdup(key), mon,
			    pe_order_implies_right|pe_order_runnable_left, data_set);
	
	    if(rsc->next_role == RSC_ROLE_MASTER) {
		custom_action_order(
			rsc, promote_key(rsc), NULL,
			rsc, NULL, mon,
			pe_order_optional|pe_order_runnable_left, data_set);

	    } else if(rsc->role == RSC_ROLE_MASTER) {
		custom_action_order(
			rsc, demote_key(rsc), NULL,
			rsc, NULL, mon,
			pe_order_optional|pe_order_runnable_left, data_set);
	    }
	}
}

void
Recurring(resource_t *rsc, action_t *start, node_t *node,
			 pe_working_set_t *data_set) 
{
    if(is_not_set(data_set->flags, pe_flag_maintenance_mode)) {	
	xml_child_iter_filter(
		rsc->ops_xml, operation, "op",
		RecurringOp(rsc, start, node, operation, data_set);		
		);
    }
}

void native_create_actions(resource_t *rsc, pe_working_set_t *data_set)
{
	action_t *start = NULL;
	node_t *chosen = NULL;
	enum rsc_role_e role = RSC_ROLE_UNKNOWN;
	enum rsc_role_e next_role = RSC_ROLE_UNKNOWN;

	crm_debug_2("Createing actions for %s: %s->%s", rsc->id,
		    role2text(rsc->role), role2text(rsc->next_role));
	
	chosen = rsc->allocated_to;
	if(chosen != NULL && rsc->next_role == RSC_ROLE_UNKNOWN) {
	    rsc->next_role = RSC_ROLE_STARTED;

	} else if(rsc->next_role == RSC_ROLE_UNKNOWN) {
	    rsc->next_role = RSC_ROLE_STOPPED;
	}

	get_rsc_attributes(rsc->parameters, rsc, chosen, data_set);
	
	if(g_list_length(rsc->running_on) > 1) {
 		if(rsc->recovery_type == recovery_stop_start) {
			pe_proc_warn("Attempting recovery of resource %s", rsc->id);
			if(rsc->role == RSC_ROLE_MASTER) {
			    DemoteRsc(rsc, NULL, FALSE, data_set);
			}
			StopRsc(rsc, NULL, FALSE, data_set);
			rsc->role = RSC_ROLE_STOPPED;
		}
		
	} else if(rsc->running_on != NULL) {
		node_t *current = rsc->running_on->data;
		NoRoleChange(rsc, current, chosen, data_set);

	} else if(rsc->role == RSC_ROLE_STOPPED && rsc->next_role == RSC_ROLE_STOPPED) {
		char *key = start_key(rsc);
		GListPtr possible_matches = find_actions(rsc->actions, key, NULL);
		slist_iter(
			action, action_t, possible_matches, lpc,
			action->optional = TRUE;
/*			action->pseudo = TRUE; */
			);
		g_list_free(possible_matches);
		crm_debug_2("Stopping a stopped resource");
		crm_free(key);
		goto do_recurring;
		
	} else if(rsc->role != RSC_ROLE_STOPPED) {
	    /* A cheap trick to account for the fact that Master/Slave groups may not be
	     * completely running when we set their role to Slave
	     */
	    crm_debug_2("Resetting %s.role = %s (was %s)",
			rsc->id, role2text(RSC_ROLE_STOPPED), role2text(rsc->role));
	    rsc->role = RSC_ROLE_STOPPED;
	}

	role = rsc->role;

	while(role != rsc->next_role) {
		next_role = rsc_state_matrix[role][rsc->next_role];
		crm_debug_2("Executing: %s->%s (%s)",
			  role2text(role), role2text(next_role), rsc->id);
		if(rsc_action_matrix[role][next_role](
			   rsc, chosen, FALSE, data_set) == FALSE) {
			break;
		}
		role = next_role;
	}

  do_recurring:
	if(rsc->next_role != RSC_ROLE_STOPPED || is_set(rsc->flags, pe_rsc_managed) == FALSE) {
		start = start_action(rsc, chosen, TRUE);
		Recurring(rsc, start, chosen, data_set);
	}
}

void native_internal_constraints(resource_t *rsc, pe_working_set_t *data_set)
{
	int type = pe_order_optional;
	const char *class = crm_element_value(rsc->xml, XML_AGENT_ATTR_CLASS);
	action_t *all_stopped = get_pseudo_op(ALL_STOPPED, data_set);

	if(rsc->variant == pe_native) {
		type |= pe_order_implies_right;
	}

	if(rsc->parent == NULL || rsc->parent->variant == pe_group) {
		type |= pe_order_restart;
	}
	
	new_rsc_order(rsc, RSC_STOP,   rsc, RSC_START,   type, data_set);
	new_rsc_order(rsc, RSC_DEMOTE, rsc, RSC_STOP,    pe_order_demote_stop, data_set);
	new_rsc_order(rsc, RSC_START,  rsc, RSC_PROMOTE, pe_order_runnable_left, data_set);
	new_rsc_order(rsc, RSC_DELETE, rsc, RSC_START,   pe_order_optional, data_set);	

	if(is_not_set(rsc->flags, pe_rsc_managed)) {
		crm_debug_3("Skipping fencing constraints for unmanaged resource: %s", rsc->id);
		return;
	} 

	if(rsc->variant == pe_native && safe_str_neq(class, "stonith")) {
	    custom_action_order(
		rsc, stop_key(rsc), NULL,
		NULL, crm_strdup(all_stopped->task), all_stopped,
		pe_order_implies_right|pe_order_runnable_left, data_set);
	}

}

void native_rsc_colocation_lh(
	resource_t *rsc_lh, resource_t *rsc_rh, rsc_colocation_t *constraint)
{
	if(rsc_lh == NULL) {
		pe_err("rsc_lh was NULL for %s", constraint->id);
		return;

	} else if(constraint->rsc_rh == NULL) {
		pe_err("rsc_rh was NULL for %s", constraint->id);
		return;
	}
	
	crm_debug_2("Processing colocation constraint between %s and %s",
		    rsc_lh->id, rsc_rh->id);
	
	rsc_rh->cmds->rsc_colocation_rh(rsc_lh, rsc_rh, constraint);		
}

static gboolean
filter_colocation_constraint(
	resource_t *rsc_lh, resource_t *rsc_rh, rsc_colocation_t *constraint)
{
	int level = LOG_DEBUG_4;
	if(constraint->score == 0){
		return FALSE;
	}

	if(constraint->score > 0
	   && constraint->role_lh != RSC_ROLE_UNKNOWN
	   && constraint->role_lh != rsc_lh->next_role) {
		do_crm_log_unlikely(level, "LH: Skipping constraint: \"%s\" state filter",
			    role2text(constraint->role_rh));
		return FALSE;
	}
	
	if(constraint->score > 0
	   && constraint->role_rh != RSC_ROLE_UNKNOWN
	   && constraint->role_rh != rsc_rh->next_role) {
		do_crm_log_unlikely(level, "RH: Skipping constraint: \"%s\" state filter",
			    role2text(constraint->role_rh));
		return FALSE;
	}

	if(constraint->score < 0
	   && constraint->role_lh != RSC_ROLE_UNKNOWN
	   && constraint->role_lh == rsc_lh->next_role) {
		do_crm_log_unlikely(level, "LH: Skipping -ve constraint: \"%s\" state filter",
			    role2text(constraint->role_rh));
		return FALSE;
	}
	
	if(constraint->score < 0
	   && constraint->role_rh != RSC_ROLE_UNKNOWN
	   && constraint->role_rh == rsc_rh->next_role) {
		do_crm_log_unlikely(level, "RH: Skipping -ve constraint: \"%s\" state filter",
			    role2text(constraint->role_rh));
		return FALSE;
	}

	return TRUE;
}

static void
colocation_match(
	resource_t *rsc_lh, resource_t *rsc_rh, rsc_colocation_t *constraint) 
{
	const char *tmp = NULL;
	const char *value = NULL;
	const char *attribute = "#id";

	GListPtr work = NULL;
	gboolean do_check = FALSE;

	if(constraint->node_attribute != NULL) {
		attribute = constraint->node_attribute;
	}

	if(rsc_rh->allocated_to) {
		value = g_hash_table_lookup(
			rsc_rh->allocated_to->details->attrs, attribute);
		do_check = TRUE;

	} else if(constraint->score < 0) {
		/* nothing to do:
		 *   anti-colocation with something thats not running
		 */
		return;
	}

	work = node_list_dup(rsc_lh->allowed_nodes, FALSE, FALSE);
	
	slist_iter(
		node, node_t, work, lpc,
		tmp = g_hash_table_lookup(node->details->attrs, attribute);
		if(do_check && safe_str_eq(tmp, value)) {
		    if(constraint->score < INFINITY) {
			crm_debug_2("%s: %s.%s += %d", constraint->id, rsc_lh->id,
				  node->details->uname, constraint->score);
			node->weight = merge_weights(
				constraint->score, node->weight);
		    }

		} else if(do_check == FALSE || constraint->score >= INFINITY) {
			crm_debug_2("%s: %s.%s -= %d (%s)", constraint->id, rsc_lh->id,
				    node->details->uname, constraint->score, do_check?"failed":"unallocated");
			node->weight = merge_weights(-constraint->score, node->weight);
		}
		);

	if(can_run_any(work)
	   || constraint->score <= -INFINITY
	   || constraint->score >= INFINITY) {
		slist_destroy(node_t, node, rsc_lh->allowed_nodes, crm_free(node));
		rsc_lh->allowed_nodes = work;
		work = NULL;

	} else {
	    char *score = score2char(constraint->score);
	    crm_info("%s: Rolling back scores from %s (%d, %s)",
		     rsc_lh->id, rsc_rh->id, do_check, score);
	    crm_free(score);
	}

	slist_destroy(node_t, node, work, crm_free(node));
}

void native_rsc_colocation_rh(
	resource_t *rsc_lh, resource_t *rsc_rh, rsc_colocation_t *constraint)
{
	crm_debug_2("%sColocating %s with %s (%s, weight=%d)",
		    constraint->score >= 0?"":"Anti-",
		    rsc_lh->id, rsc_rh->id, constraint->id, constraint->score);
	
	if(filter_colocation_constraint(rsc_lh, rsc_rh, constraint) == FALSE) {
		return;
	}
	
	if(is_set(rsc_rh->flags, pe_rsc_provisional)) {
		return;

	} else if(is_not_set(rsc_lh->flags, pe_rsc_provisional)) {
		/* error check */
		struct node_shared_s *details_lh;
		struct node_shared_s *details_rh;
		if((constraint->score > -INFINITY) && (constraint->score < INFINITY)) {
			return;
		}

		details_rh = rsc_rh->allocated_to?rsc_rh->allocated_to->details:NULL;
		details_lh = rsc_lh->allocated_to?rsc_lh->allocated_to->details:NULL;
		
		if(constraint->score == INFINITY && details_lh != details_rh) {
			crm_err("%s and %s are both allocated"
				" but to different nodes: %s vs. %s",
				rsc_lh->id, rsc_rh->id,
				details_lh?details_lh->uname:"n/a",
				details_rh?details_rh->uname:"n/a");

		} else if(constraint->score == -INFINITY && details_lh == details_rh) {
			crm_err("%s and %s are both allocated"
				" but to the SAME node: %s",
				rsc_lh->id, rsc_rh->id,
				details_rh?details_rh->uname:"n/a");
		}
		
		return;
		
	} else {
		colocation_match(rsc_lh, rsc_rh, constraint);
	}
}

static GListPtr find_actions_by_task(GListPtr actions, resource_t *rsc, const char *original_key)
{
    GListPtr list = NULL;

    list = find_actions(actions, original_key, NULL);
    if(list == NULL) {
	/* we're potentially searching a child of the original resource */
	char *key = NULL;
	char *tmp = NULL;
	char *task = NULL;
	int interval = 0;
	if(parse_op_key(original_key, &tmp, &task, &interval)) {
	    key = generate_op_key(rsc->id, task, interval);
	    /* crm_err("looking up %s instead of %s", key, original_key); */
	    /* slist_iter(action, action_t, actions, lpc, */
	    /* 	       crm_err("  - %s", action->uuid)); */
	    list = find_actions(actions, key, NULL);
	    
	} else {
	    crm_err("search key: %s", original_key);
	}	

	crm_free(key);
	crm_free(tmp);
	crm_free(task);
    }

    return list;
}

void native_rsc_order_lh(resource_t *lh_rsc, order_constraint_t *order, pe_working_set_t *data_set)
{
	GListPtr lh_actions = NULL;
	action_t *lh_action = order->lh_action;
	resource_t *rh_rsc = order->rh_rsc;

	crm_debug_3("Processing LH of ordering constraint %d", order->id);
	CRM_ASSERT(lh_rsc != NULL);
	
	if(lh_action != NULL) {
		lh_actions = g_list_append(NULL, lh_action);

	} else if(lh_action == NULL) {
		lh_actions = find_actions_by_task(
		    lh_rsc->actions, lh_rsc, order->lh_action_task);
	}

	if(lh_actions == NULL && lh_rsc != rh_rsc) {
		char *key = NULL;
		char *rsc_id = NULL;
		char *op_type = NULL;
		int interval = 0;
		
		crm_debug_4("No LH-Side (%s/%s) found for constraint %d with %s - creating",
			    lh_rsc->id, order->lh_action_task,
			    order->id, order->rh_action_task);

		parse_op_key(
			order->lh_action_task, &rsc_id, &op_type, &interval);

		key = generate_op_key(lh_rsc->id, op_type, interval);

		lh_action = custom_action(lh_rsc, key, op_type,
					  NULL, TRUE, TRUE, data_set);

		if(lh_rsc->fns->state(lh_rsc, TRUE) == RSC_ROLE_STOPPED
		   && safe_str_eq(op_type, RSC_STOP)) {
			lh_action->pseudo = TRUE;
			lh_action->runnable = TRUE;
		}
		
		lh_actions = g_list_append(NULL, lh_action);

		crm_free(op_type);
		crm_free(rsc_id);
	}

	slist_iter(
		lh_action_iter, action_t, lh_actions, lpc,

		if(rh_rsc == NULL && order->rh_action) {
			rh_rsc = order->rh_action->rsc;
		}
		if(rh_rsc) {
			rh_rsc->cmds->rsc_order_rh(
				lh_action_iter, rh_rsc, order);

		} else if(order->rh_action) {
		    order_actions(
				lh_action_iter, order->rh_action, order->type); 

		}
		);

	pe_free_shallow_adv(lh_actions, FALSE);
}

void native_rsc_order_rh(
	action_t *lh_action, resource_t *rsc, order_constraint_t *order)
{
	GListPtr rh_actions = NULL;
	action_t *rh_action = NULL;

	CRM_CHECK(rsc != NULL, return);
	CRM_CHECK(order != NULL, return);

	rh_action = order->rh_action;
	crm_debug_3("Processing RH of ordering constraint %d", order->id);

	if(rh_action != NULL) {
		rh_actions = g_list_append(NULL, rh_action);

	} else if(rsc != NULL) {
		rh_actions = find_actions_by_task(
		    rsc->actions, rsc, order->rh_action_task);
	}

	if(rh_actions == NULL) {
		crm_debug_4("No RH-Side (%s/%s) found for constraint..."
			    " ignoring", rsc->id,order->rh_action_task);
		if(lh_action) {
			crm_debug_4("LH-Side was: %s", lh_action->uuid);
		}
		return;
	}
	
	slist_iter(
		rh_action_iter, action_t, rh_actions, lpc,

		if(lh_action) {
			order_actions(lh_action, rh_action_iter, order->type); 
			
		} else if(order->type & pe_order_implies_right) {
			rh_action_iter->runnable = FALSE;
			crm_warn("Unrunnable %s 0x%.6x", rh_action_iter->uuid, order->type);
		} else {
			crm_warn("neither %s 0x%.6x", rh_action_iter->uuid, order->type);
		}
		
		);

	pe_free_shallow_adv(rh_actions, FALSE);
}

void native_rsc_location(resource_t *rsc, rsc_to_node_t *constraint)
{
	GListPtr or_list;

	crm_debug_2("Applying %s (%s) to %s", constraint->id,
		    role2text(constraint->role_filter), rsc->id);

	/* take "lifetime" into account */
	if(constraint == NULL) {
		pe_err("Constraint is NULL");
		return;

	} else if(rsc == NULL) {
		pe_err("LHS of rsc_to_node (%s) is NULL", constraint->id);
		return;

	} else if(constraint->role_filter > 0
		  && constraint->role_filter != rsc->next_role) {
		crm_debug("Constraint (%s) is not active (role : %s)",
			  constraint->id, role2text(constraint->role_filter));
		return;
		
	} else if(is_active(constraint) == FALSE) {
		crm_debug_2("Constraint (%s) is not active", constraint->id);
		return;
	}
    
	if(constraint->node_list_rh == NULL) {
		crm_debug_2("RHS of constraint %s is NULL", constraint->id);
		return;
	}
	or_list = node_list_or(
		rsc->allowed_nodes, constraint->node_list_rh, FALSE);
		
	pe_free_shallow(rsc->allowed_nodes);
	rsc->allowed_nodes = or_list;
	slist_iter(node, node_t, or_list, lpc,
		   crm_debug_3("%s + %s : %d", rsc->id, node->details->uname, node->weight);
		);
}

void native_expand(resource_t *rsc, pe_working_set_t *data_set)
{
	crm_debug_3("Processing actions from %s", rsc->id);

	slist_iter(
		action, action_t, rsc->actions, lpc,
		crm_debug_4("processing action %d for rsc=%s",
			  action->id, rsc->id);
		graph_element_from_action(action, data_set);
		);

	slist_iter(
	    child_rsc, resource_t, rsc->children, lpc,
	    
	    child_rsc->cmds->expand(child_rsc, data_set);
	    );
}


void
LogActions(resource_t *rsc, pe_working_set_t *data_set)
{
    node_t *next = NULL;
    node_t *current = NULL;
    gboolean moving = FALSE;
    
    if(rsc->children) {
	slist_iter(
	    child_rsc, resource_t, rsc->children, lpc,
	    LogActions(child_rsc, data_set);
	    );
	return;
    }
    
    next = rsc->allocated_to;
    if(rsc->running_on) {
	current = rsc->running_on->data;
	if(rsc->role == RSC_ROLE_STOPPED) {
	    /*
	     * This can occur when resources are being recovered
	     * We fiddle with the current role in native_create_actions()
	     */
	    rsc->role = RSC_ROLE_STARTED;
	}
    }

    if(current == NULL && is_set(rsc->flags, pe_rsc_orphan)) {
	/* Don't log stopped orphans */
	return;
    }
    
    if(is_not_set(rsc->flags, pe_rsc_managed)
       || (current == NULL && next == NULL)) {
	crm_notice("Leave resource %s\t(%s%s)",
		   rsc->id, role2text(rsc->role), is_not_set(rsc->flags, pe_rsc_managed)?" unmanaged":"");
	return;
    }

    if(current != NULL && next != NULL
       && safe_str_neq(current->details->id, next->details->id)) {
	moving = TRUE;
    }
    
    if(rsc->role == rsc->next_role) {
	action_t *start = NULL;
	char *key = start_key(rsc);
	GListPtr possible_matches = find_actions(rsc->actions, key, next);
	crm_free(key);

	if(possible_matches) {
	    start = possible_matches->data;
	    g_list_free(possible_matches);
	}

	key = generate_op_key(rsc->id, CRMD_ACTION_MIGRATED, 0);
	possible_matches = find_actions(rsc->actions, key, next);
	crm_free(key);
	
	CRM_CHECK(next != NULL,);
	if(next == NULL) {
	} else if(possible_matches) {
	    crm_notice("Migrate resource %s\t(%s %s -> %s)",
		       rsc->id, role2text(rsc->role), current->details->uname, next->details->uname);
	    g_list_free(possible_matches);
	    
	} else if(start == NULL || start->optional) {
	    crm_notice("Leave resource %s\t(%s %s)",
		       rsc->id, role2text(rsc->role), next->details->uname);
	    
	} else if(moving && current) {
	    crm_notice("Move resource %s\t(%s %s -> %s)",
		       rsc->id, role2text(rsc->role), current->details->uname, next->details->uname);
	    
	} else if(is_set(rsc->flags, pe_rsc_failed)) {
	    crm_notice("Recover resource %s\t(%s %s)",
		       rsc->id, role2text(rsc->role), next->details->uname);
	    
	} else if(start && start->runnable == FALSE) {
	    crm_notice("Stop resource %s\t(%s %s)",
		       rsc->id, role2text(rsc->role), next->details->uname);

	} else {
	    crm_notice("Restart resource %s\t(%s %s)",
		       rsc->id, role2text(rsc->role), next->details->uname);
	}
	
	return;
    }

    if(rsc->role > RSC_ROLE_SLAVE && rsc->role > rsc->next_role) {
	CRM_CHECK(current != NULL,);
	if(current != NULL) {
	    crm_notice("Demote %s\t(%s -> %s %s)", rsc->id,
		       role2text(rsc->role), role2text(rsc->next_role),
		       current->details->uname);
	}
    }

    if(rsc->next_role == RSC_ROLE_STOPPED || moving) {
	CRM_CHECK(current != NULL,);
	slist_iter(node, node_t, rsc->running_on, lpc,
		   crm_notice("Stop resource %s\t(%s)", rsc->id, node->details->uname));
    }

    if(rsc->role == RSC_ROLE_STOPPED || moving) {
	CRM_CHECK(next != NULL,);
	if(next != NULL) {
	    crm_notice("Start %s\t(%s)", rsc->id, next->details->uname);
	}
    }    

    if(rsc->next_role > RSC_ROLE_SLAVE && rsc->role < rsc->next_role) {
	CRM_CHECK(next != NULL,);
	crm_notice("Promote %s\t(%s -> %s %s)", rsc->id,
		   role2text(rsc->role), role2text(rsc->next_role),
		   next->details->uname);
    }
}

void
NoRoleChange(resource_t *rsc, node_t *current, node_t *next,
	     pe_working_set_t *data_set)
{
	action_t *stop = NULL;
	action_t *start = NULL;		
	GListPtr possible_matches = NULL;

	crm_debug_2("Executing: %s (role=%s)", rsc->id, role2text(rsc->next_role));
	
	if(current == NULL || next == NULL) {
	    return;
	}
	
	if(is_set(rsc->flags, pe_rsc_failed)
	   || safe_str_neq(current->details->id, next->details->id)) {

		if(rsc->next_role > RSC_ROLE_STARTED) {
		    gboolean optional = TRUE;
		    if(rsc->role == RSC_ROLE_MASTER) {
			optional = FALSE;
		    }
		    DemoteRsc(rsc, current, optional, data_set);
		}
		if(rsc->role == RSC_ROLE_MASTER) {
			DemoteRsc(rsc, current, FALSE, data_set);
		}
		StopRsc(rsc, current, FALSE, data_set);
		StartRsc(rsc, next, FALSE, data_set);
		if(rsc->next_role == RSC_ROLE_MASTER) {
		    PromoteRsc(rsc, next, FALSE, data_set);
		}

		possible_matches = find_recurring_actions(rsc->actions, next);
		slist_iter(match, action_t, possible_matches, lpc,
			   if(match->optional == FALSE) {
				   crm_debug("Fixing recurring action: %s",
					     match->uuid);
				   match->optional = TRUE;
			   }
			);
		g_list_free(possible_matches);
		
	} else if(is_set(rsc->flags, pe_rsc_start_pending)) {
		start = start_action(rsc, next, TRUE);
		if(start->runnable) {
			/* wait for StartRsc() to be called */
			rsc->role = RSC_ROLE_STOPPED;
		} else {
			/* wait for StopRsc() to be called */
			rsc->next_role = RSC_ROLE_STOPPED;
		}
		
	} else {
	    
		stop = stop_action(rsc, current, TRUE);
		start = start_action(rsc, next, TRUE);
		stop->optional = start->optional;
		if(rsc->next_role > RSC_ROLE_STARTED) {
		    DemoteRsc(rsc, current, start->optional, data_set);
		}
		StopRsc(rsc, current, start->optional, data_set);
		StartRsc(rsc, current, start->optional, data_set);
		if(rsc->next_role == RSC_ROLE_MASTER) {
			PromoteRsc(rsc, next, start->optional, data_set);
		}
		
		if(start->runnable == FALSE) {
			rsc->next_role = RSC_ROLE_STOPPED;
		}
	}
}


gboolean
StopRsc(resource_t *rsc, node_t *next, gboolean optional, pe_working_set_t *data_set)
{
	action_t *stop = NULL;
	const char *class = crm_element_value(rsc->xml, XML_AGENT_ATTR_CLASS);

	crm_debug_2("Executing: %s", rsc->id);

	if(rsc->next_role == RSC_ROLE_STOPPED
	   && rsc->variant == pe_native
	   && safe_str_eq(class, "stonith")) {
	    action_t *all_stopped = get_pseudo_op(ALL_STOPPED, data_set);
	    custom_action_order(
		NULL, crm_strdup(all_stopped->task), all_stopped,
		rsc, stop_key(rsc), NULL,
		pe_order_implies_left|pe_order_stonith_stop, data_set);
	}
	
	slist_iter(
		current, node_t, rsc->running_on, lpc,
		stop = stop_action(rsc, current, optional);

		if(is_set(data_set->flags, pe_flag_remove_after_stop)) {
			DeleteRsc(rsc, current, optional, data_set);
		}
		);
	
	return TRUE;
}


gboolean
StartRsc(resource_t *rsc, node_t *next, gboolean optional, pe_working_set_t *data_set)
{
	action_t *start = NULL;
	
	crm_debug_2("Executing: %s", rsc->id);
	start = start_action(rsc, next, TRUE);
	if(start->runnable && optional == FALSE) {
		start->optional = FALSE;
	}		
	return TRUE;
}

gboolean
PromoteRsc(resource_t *rsc, node_t *next, gboolean optional, pe_working_set_t *data_set)
{
	char *key = NULL;
	gboolean runnable = TRUE;
	GListPtr action_list = NULL;
	crm_debug_2("Executing: %s", rsc->id);

	CRM_CHECK(rsc->next_role == RSC_ROLE_MASTER,
		  crm_err("Next role: %s", role2text(rsc->next_role));
		  return FALSE);

	CRM_CHECK(next != NULL, return FALSE);

	key = start_key(rsc);
	action_list = find_actions_exact(rsc->actions, key, next);
	crm_free(key);

	slist_iter(start, action_t, action_list, lpc,
		   if(start->runnable == FALSE) {
			   runnable = FALSE;
		   }
		);

	g_list_free(action_list);

	if(runnable) {
		promote_action(rsc, next, optional);
		return TRUE;
	} 

	crm_debug("%s\tPromote %s (canceled)", next->details->uname, rsc->id);

	key = promote_key(rsc);
	action_list = find_actions_exact(rsc->actions, key, next);
	crm_free(key);

	slist_iter(promote, action_t, action_list, lpc,
		   promote->runnable = FALSE;
		);
	
	g_list_free(action_list);
	return TRUE;
}

gboolean
DemoteRsc(resource_t *rsc, node_t *next, gboolean optional, pe_working_set_t *data_set)
{
	crm_debug_2("Executing: %s", rsc->id);

/* 	CRM_CHECK(rsc->next_role == RSC_ROLE_SLAVE, return FALSE); */
	slist_iter(
		current, node_t, rsc->running_on, lpc,
		demote_action(rsc, current, optional);
		);
	return TRUE;
}

gboolean
RoleError(resource_t *rsc, node_t *next, gboolean optional, pe_working_set_t *data_set)
{
	crm_debug("Executing: %s", rsc->id);
	CRM_CHECK(FALSE, return FALSE);
	return FALSE;
}

gboolean
NullOp(resource_t *rsc, node_t *next, gboolean optional, pe_working_set_t *data_set)
{
	crm_debug_2("Executing: %s", rsc->id);
	return FALSE;
}

gboolean
DeleteRsc(resource_t *rsc, node_t *node, gboolean optional, pe_working_set_t *data_set)
{
	action_t *delete = NULL;
#if DELETE_THEN_REFRESH
 	action_t *refresh = NULL;
#endif
	if(is_set(rsc->flags, pe_rsc_failed)) {
		crm_debug_2("Resource %s not deleted from %s: failed",
			    rsc->id, node->details->uname);
		return FALSE;
		
	} else if(node == NULL) {
		crm_debug_2("Resource %s not deleted: NULL node", rsc->id);
		return FALSE;
		
	} else if(node->details->unclean || node->details->online == FALSE) {
		crm_debug_2("Resource %s not deleted from %s: unrunnable",
			    rsc->id, node->details->uname);
		return FALSE;
	}
	
	crm_notice("Removing %s from %s",
		 rsc->id, node->details->uname);
	
	delete = delete_action(rsc, node, optional);
	
	new_rsc_order(rsc, RSC_STOP, rsc, RSC_DELETE, 
		      optional?pe_order_implies_right:pe_order_implies_left, data_set);
	
#if DELETE_THEN_REFRESH
	refresh = custom_action(
		NULL, crm_strdup(CRM_OP_LRM_REFRESH), CRM_OP_LRM_REFRESH,
		node, FALSE, TRUE, data_set);

	add_hash_param(refresh->meta, XML_ATTR_TE_NOWAIT, XML_BOOLEAN_TRUE);

	order_actions(delete, refresh, pe_order_optional);
#endif
	
	return TRUE;
}

#include <../lib/pengine/unpack.h>

gboolean
native_create_probe(resource_t *rsc, node_t *node, action_t *complete,
		    gboolean force, pe_working_set_t *data_set) 
{
	char *key = NULL;
	char *target_rc = NULL;
	action_t *probe = NULL;
	node_t *running = NULL;
	resource_t *top = uber_parent(rsc);

	CRM_CHECK(node != NULL, return FALSE);
	
	if(rsc->children) {
	    gboolean any_created = FALSE;
	    
	    slist_iter(
		child_rsc, resource_t, rsc->children, lpc,
		
		any_created = child_rsc->cmds->create_probe(
		    child_rsc, node, complete, force, data_set) || any_created;
		);

	    return any_created;
	}

	if(is_set(rsc->flags, pe_rsc_orphan)) {
		crm_debug_2("Skipping orphan: %s", rsc->id);
		return FALSE;
	}
	
	running = pe_find_node_id(rsc->known_on, node->details->id);
	if(force == FALSE && running != NULL) {
		/* we already know the status of the resource on this node */
		crm_debug_3("Skipping active: %s", rsc->id);
		return FALSE;
	}

	if(running == NULL && is_set(top->flags, pe_rsc_unique) == FALSE) {
	    /* Annoyingly we also need to check any other clone instances
	     * Clumsy, but it will work.
	     *
	     * An alternative would be to update known_on for every peer
	     * during process_rsc_state()
	     */

	    char *clone_id = clone_zero(rsc->id);
	    resource_t *peer = pe_find_resource(top->children, clone_id);

	    while(peer && running == NULL) {
		running = pe_find_node_id(peer->known_on, node->details->id);
		if(force == FALSE && running != NULL) {
		    /* we already know the status of the resource on this node */
		    crm_debug_3("Skipping active clone: %s", rsc->id);
		    crm_free(clone_id);
		    return FALSE;
		}
		clone_id = increment_clone(clone_id);
		peer = pe_find_resource(data_set->resources, clone_id);
	    }
	    
	    crm_free(clone_id);
	}

	key = generate_op_key(rsc->id, RSC_STATUS, 0);
	probe = custom_action(rsc, key, RSC_STATUS, node,
			      FALSE, TRUE, data_set);
	probe->optional = FALSE;
	
	running = pe_find_node_id(rsc->running_on, node->details->id);
	if(running == NULL) {
		target_rc = crm_itoa(EXECRA_NOT_RUNNING);

	} else if(rsc->role == RSC_ROLE_MASTER) {
		target_rc = crm_itoa(EXECRA_RUNNING_MASTER);
	}

	if(target_rc != NULL) {
		add_hash_param(probe->meta, XML_ATTR_TE_TARGET_RC, target_rc);
		crm_free(target_rc);
	}
	
	crm_debug("Probing %s on %s (%s)", rsc->id, node->details->uname, role2text(rsc->role));
	order_actions(probe, complete, pe_order_implies_right);
	
	return TRUE;
}

static void
native_start_constraints(
	resource_t *rsc,  action_t *stonith_op, gboolean is_stonith,
	pe_working_set_t *data_set)
{
	node_t *target = stonith_op?stonith_op->node:NULL;

	if(is_stonith) {
		char *key = start_key(rsc);
		action_t *ready = get_pseudo_op(STONITH_UP, data_set);

		crm_debug_2("Ordering %s action before stonith events", key);
		custom_action_order(
		    rsc, key, NULL,
		    NULL, crm_strdup(ready->task), ready,
		    pe_order_optional, data_set);
		
	} else {
		action_t *all_stopped = get_pseudo_op(ALL_STOPPED, data_set);
		action_t *stonith_done = get_pseudo_op(STONITH_DONE, data_set);
		slist_iter(action, action_t, rsc->actions, lpc2,
			   if(action->needs == rsc_req_stonith) {
			       order_actions(stonith_done, action, pe_order_implies_left);

			   } else if(target != NULL
			      && safe_str_eq(action->task, RSC_START)
			      && NULL == pe_find_node_id(
				      rsc->known_on, target->details->id)) {
				   /* if known == NULL, then we dont know if
				    *   the resource is active on the node
				    *   we're about to shoot
				    *
				    * in this case, regardless of action->needs,
				    *   the only safe option is to wait until
				    *   the node is shot before doing anything
				    *   to with the resource
				    *
				    * its analogous to waiting for all the probes
				    *   for rscX to complete before starting rscX
				    *
				    * the most likely explaination is that the
				    *   DC died and took its status with it
				    */
				   
				   crm_info("Ordering %s after %s recovery",
					    action->uuid, target->details->uname);
				   order_actions(all_stopped, action,
						 pe_order_implies_left|pe_order_runnable_left);
			   }
			   
			);
	}
}

static void
native_stop_constraints(
	resource_t *rsc,  action_t *stonith_op, gboolean is_stonith,
	pe_working_set_t *data_set)
{
	char *key = NULL;
	GListPtr action_list = NULL;
	resource_t *top = uber_parent(rsc);
	
	key = stop_key(rsc);
	action_list = find_actions(rsc->actions, key, stonith_op->node);
	crm_free(key);

	/* add the stonith OP as a stop pre-req and the mark the stop
	 * as a pseudo op - since its now redundant
	 */
	
	slist_iter(
	    action, action_t, action_list, lpc2,

	    resource_t *parent = NULL;
	    if(action->node->details->online
	       && action->node->details->unclean == FALSE
	       && is_set(rsc->flags, pe_rsc_failed)) {
		continue;
	    }

	    if(is_set(rsc->flags, pe_rsc_failed)) {
		crm_warn("Stop of failed resource %s is"
			 " implicit after %s is fenced",
			 rsc->id, action->node->details->uname);
	    } else {
		crm_info("%s is implicit after %s is fenced",
			 action->uuid, action->node->details->uname);
	    }

	    /* the stop would never complete and is
	     * now implied by the stonith operation
	     */
	    action->pseudo = TRUE;
	    action->runnable = TRUE;
	    action->implied_by_stonith = TRUE;
	    
	    if(is_stonith == FALSE) {
		action_t *parent_stop = find_first_action(top->actions, NULL, RSC_STOP, NULL);
		
		order_actions(stonith_op, action, pe_order_optional);
		order_actions(stonith_op, parent_stop, pe_order_optional);
	    }

	    if(is_set(rsc->flags, pe_rsc_notify)) {
		/* Create a second notification that will be delivered
		 *   immediately after the node is fenced
		 *
		 * Basic problem:
		 * - C is a clone active on the node to be shot and stopping on another
		 * - R is a resource that depends on C
		 *
		 * + C.stop depends on R.stop
		 * + C.stopped depends on STONITH
		 * + C.notify depends on C.stopped
		 * + C.healthy depends on C.notify
		 * + R.stop depends on C.healthy
		 *
		 * The extra notification here changes
		 *  + C.healthy depends on C.notify
		 * into: 
		 *  + C.healthy depends on C.notify'
		 *  + C.notify' depends on STONITH'
		 * thus breaking the loop
		 */
		notify_data_t *n_data = create_notification_boundaries(rsc, RSC_STOP, NULL, stonith_op, data_set);
		crm_info("Creating secondary notification for %s", action->uuid);

		collect_notification_data(rsc, TRUE, FALSE, n_data);
		g_hash_table_insert(n_data->keys, crm_strdup("notify_stop_resource"), crm_strdup(rsc->id));
		g_hash_table_insert(n_data->keys, crm_strdup("notify_stop_uname"), crm_strdup(action->node->details->uname));
		create_notifications(uber_parent(rsc), n_data, data_set);
		free_notification_data(n_data);
	    }
	    
	    /* find the top-most resource */
	    parent = rsc->parent;
	    while(parent != NULL && parent->parent != NULL) {
		parent = parent->parent;
	    }

	    if(parent) {
		crm_debug_2("Re-creating actions for %s", parent->id);
		parent->cmds->create_actions(parent, data_set);
		
		/* make sure we dont mess anything up in create_actions */
		CRM_CHECK(action->pseudo, action->pseudo = TRUE);
		CRM_CHECK(action->runnable, action->runnable = TRUE);
	    }
/* From Bug #1601, successful fencing must be an input to a failed resources stop action.

   However given group(rA, rB) running on nodeX and B.stop has failed, 
   A := stop healthy resource (rA.stop)
   B := stop failed resource (pseudo operation B.stop)
   C := stonith nodeX
   A requires B, B requires C, C requires A
   This loop would prevent the cluster from making progress.

   This block creates the "C requires A" dependency and therefore must (at least
   for now) be disabled.

   Instead, run the block above and treat all resources on nodeX as B would be
   (marked as a pseudo op depending on the STONITH).

   TODO: Break the "A requires B" dependency in update_action() and re-enable this block
   
		} else if(is_stonith == FALSE) {
			crm_info("Moving healthy resource %s"
				 " off %s before fencing",
				 rsc->id, node->details->uname);
			
			 * stop healthy resources before the
			 * stonith op
			 *
			custom_action_order(
				rsc, stop_key(rsc), NULL,
				NULL,crm_strdup(CRM_OP_FENCE),stonith_op,
				pe_order_optional, data_set);
*/
	    );
	
	g_list_free(action_list);

	key = demote_key(rsc);
	action_list = find_actions(rsc->actions, key, stonith_op->node);
	crm_free(key);
	
	slist_iter(
		action, action_t, action_list, lpc2,
		if(action->node->details->online == FALSE || is_set(rsc->flags, pe_rsc_failed)) {
			crm_info("Demote of failed resource %s is"
				 " implict after %s is fenced",
				 rsc->id, action->node->details->uname);
			/* the stop would never complete and is
			 * now implied by the stonith operation
			 */
			action->pseudo = TRUE;
			action->runnable = TRUE;
			if(is_stonith == FALSE) {
			    order_actions(stonith_op, action, pe_order_optional);
			}
		}
		);	
	
	g_list_free(action_list);
}

void
complex_stonith_ordering(
	resource_t *rsc,  action_t *stonith_op, pe_working_set_t *data_set)
{
	gboolean is_stonith = FALSE;
	const char *class = crm_element_value(rsc->xml, XML_AGENT_ATTR_CLASS);

	if(rsc->children) {
	    slist_iter(
		child_rsc, resource_t, rsc->children, lpc,

		child_rsc->cmds->stonith_ordering(
		    child_rsc, stonith_op, data_set);
		);
	    return;
	}
	
	if(is_not_set(rsc->flags, pe_rsc_managed)) {
		crm_debug_3("Skipping fencing constraints for unmanaged resource: %s", rsc->id);
		return;
	} 

	if(stonith_op != NULL && safe_str_eq(class, "stonith")) {
		is_stonith = TRUE;
	}
	
	/* Start constraints */
	native_start_constraints(rsc,  stonith_op, is_stonith, data_set);
 
	/* Stop constraints */
	native_stop_constraints(rsc,  stonith_op, is_stonith, data_set);
}

#define ALLOW_WEAK_MIGRATION 0

enum stack_activity 
{
    stack_stable   = 0,
    stack_starting = 1,
    stack_stopping = 2,
    stack_middle   = 4,
};

static enum stack_activity
find_clone_activity_on(resource_t *rsc, resource_t *target, node_t *node, const char *type) 
{
    int mode = stack_stable;
    action_t *active = NULL;

    if(target->children) {
	slist_iter(
	    child, resource_t, target->children, lpc,
	    mode |= find_clone_activity_on(rsc, child, node, type);
	    );
	return mode;
    }

    active = find_first_action(target->actions, NULL, CRMD_ACTION_START, NULL);
    if(active && active->optional == FALSE && active->pseudo == FALSE) {
	crm_debug("%s: found scheduled %s action (%s)", rsc->id, active->uuid, type);
	mode |= stack_starting;
    }

    active = find_first_action(target->actions, NULL, CRMD_ACTION_STOP, node);
    if(active && active->optional == FALSE && active->pseudo == FALSE) {
	crm_debug("%s: found scheduled %s action (%s)", rsc->id, active->uuid, type);
	mode |= stack_stopping;
    }

    return mode;
}

static enum stack_activity
check_stack_element(resource_t *rsc, resource_t *other_rsc, const char *type) 
{
    resource_t *other_p = uber_parent(other_rsc);
    if(other_rsc == NULL || other_rsc == rsc) {
	return stack_stable;

    } else if(other_p->variant == pe_native) {
	crm_notice("Cannot migrate %s due to dependency on %s (%s)",
		   rsc->id, other_rsc->id, type);
	return stack_middle;

    } else if(other_rsc == rsc->parent) {
	int mode = 0;
	slist_iter(constraint, rsc_colocation_t, other_rsc->rsc_cons, lpc,
		   if(constraint->score > 0) {
		       mode |= check_stack_element(rsc, constraint->rsc_rh, type);
		   }
	    );
	return mode;
	    
    } else if(other_p->variant == pe_group) {
	crm_notice("Cannot migrate %s due to dependency on group %s (%s)",
		   rsc->id, other_rsc->id, type);
	return stack_middle;

    } /* else: >= clone */
    
    /*
      
## Assumption
A depends on clone(B)

## Resource Activity During Move

	N1		N2		N3
	---             ---		---
t0	A.stop
t1	B.stop				B.stop
t2			B.start		B.start
t3			A.start

## Resource Activity During Migration

       	N1		N2		N3
	---             ---		---
t0			B.start		B.start
t1	A.stop (1)
t2			A.start (2)
t3	B.stop				B.stop

Node 1: Rewritten to be a migrate-to operation
Node 2: Rewritten to be a migrate-from operation

# Constraints

The following constraints already exist in the system.
The 'ok' and 'fail' column refers to whether they still hold for migration.

a) A.stop  -> A.start - ok
b) B.stop  -> B.start - fail

c) A.stop  -> B.stop  - ok
d) B.start -> A.start - ok

e) B.stop  -> A.start - fail
f) A.stop  -> B.start - fail

## Scenarios
B unchanged             - ok
B stopping only         - fail - possible after fixing 'e'
B starting only         - fail - possible after fixing 'f'
B stoping and starting  - fail - constraint 'b' is unfixable
B restarting only on N2 - fail - as-per previous only rarer
      
     */
    
    /* Only allow migration when the clone is either stable, only starting or only stopping */
    return find_clone_activity_on(rsc, other_rsc, NULL, type);
}

static gboolean
at_stack_bottom(resource_t *rsc) 
{
    char *key = NULL;
    action_t *start = NULL;
    action_t *other = NULL;
    int mode = stack_stable;
    GListPtr action_list = NULL;

    key = start_key(rsc);
    action_list = find_actions(rsc->actions, key, NULL);
    crm_free(key);
    
    crm_debug_3("%s: processing", rsc->id);
    CRM_CHECK(action_list != NULL, return FALSE);
    
    start = action_list->data;
    g_list_free(action_list);

    slist_iter(
	constraint, rsc_colocation_t, rsc->rsc_cons, lpc,

	resource_t *target = constraint->rsc_rh;
	crm_debug_4("Checking %s: %s == %s (%d)", constraint->id, rsc->id, target->id, constraint->score);
	if(constraint->score > 0) {
	    mode |= check_stack_element(rsc, target, "coloc");
	    if(mode & stack_middle) {
		return FALSE;

	    } else if((mode & stack_stopping) && (mode & stack_starting)) {
		crm_notice("Cannot migrate %s due to colocation activity (last was %s)",
			   rsc->id, target->id);
		return FALSE;
	    }
	}
	);

    slist_iter(
	other_w, action_wrapper_t, start->actions_before, lpc,
	other = other_w->action;

#if ALLOW_WEAK_MIGRATION
	if((other_w->type & pe_order_implies_right) == 0) {
	    crm_debug_3("%s: depends on %s (optional ordering)",
			rsc->id, other->uuid);
	    continue;
	}	
#endif 

	crm_debug_2("%s: Checking %s ordering", rsc->id, other->uuid);

	if(other->optional == FALSE) {
	    mode |= check_stack_element(rsc, other->rsc, "order");
	    if(mode & stack_middle) {
		return FALSE;

	    } else if((mode & stack_stopping) && (mode & stack_starting)) {
		crm_notice("Cannot migrate %s due to ordering activity (last was %s)",
			   rsc->id, other->rsc->id);
		return FALSE;
	    }
	}
	
	);

    return TRUE;
}

void
complex_migrate_reload(resource_t *rsc, pe_working_set_t *data_set)
{
	char *key = NULL;
	int level = LOG_DEBUG;
	GListPtr action_list = NULL;
	
	action_t *stop = NULL;
	action_t *start = NULL;
	action_t *other = NULL;
	action_t *action = NULL;
	const char *value = NULL;

	if(rsc->children) {
	    slist_iter(
		child_rsc, resource_t, rsc->children, lpc,
		
		child_rsc->cmds->migrate_reload(child_rsc, data_set);
		);
	    other = NULL;
	    return;
	} else if(rsc->variant > pe_native) {
	    return;
	}

	do_crm_log_unlikely(level+1, "Processing %s", rsc->id);
	
	if(is_not_set(rsc->flags, pe_rsc_managed)
	   || is_set(rsc->flags, pe_rsc_failed)
	   || is_set(rsc->flags, pe_rsc_start_pending)
	   || rsc->next_role < RSC_ROLE_STARTED
	   || g_list_length(rsc->running_on) != 1) {
	    do_crm_log_unlikely(
		level+1, "%s: general resource state: flags=0x%.16llx",
		rsc->id, rsc->flags);
	    return;
	}

	value = g_hash_table_lookup(rsc->meta, XML_OP_ATTR_ALLOW_MIGRATE);
	if(crm_is_true(value)) {
	    set_bit(rsc->flags, pe_rsc_can_migrate);	
	}	

	if(rsc->next_role > RSC_ROLE_SLAVE) {
	    clear_bit(rsc->flags, pe_rsc_can_migrate);	
	    do_crm_log_unlikely(
		level+1, "%s: resource role: role=%s", rsc->id, role2text(rsc->next_role));
	}
	
	key = start_key(rsc);
	action_list = find_actions(rsc->actions, key, NULL);
	crm_free(key);
	
	if(action_list == NULL) {
		do_crm_log_unlikely(level, "%s: no start action", rsc->id);
		return;
	}
	
	start = action_list->data;
	g_list_free(action_list);

	if(is_not_set(rsc->flags, pe_rsc_can_migrate)
	   && start->allow_reload_conversion == FALSE) {
		do_crm_log_unlikely(level+1, "%s: no need to continue", rsc->id);
		return;
	}
	
	key = stop_key(rsc);
	action_list = find_actions(rsc->actions, key, NULL);
	crm_free(key);
	
	if(action_list == NULL) {
		do_crm_log_unlikely(level, "%s: no stop action", rsc->id);
		return;
	}
	
	stop = action_list->data;
	g_list_free(action_list);
	
	action = start;
	if(action->pseudo
	   || action->optional
	   || action->node == NULL
	   || action->runnable == FALSE) {
		do_crm_log_unlikely(level, "%s: %s", rsc->id, action->task);
		return;
	}
	
	action = stop;
	if(action->pseudo
	   || action->optional
	   || action->node == NULL
	   || action->runnable == FALSE) {
		do_crm_log_unlikely(level, "%s: %s", rsc->id, action->task);
		return;
	}
	
	if(is_set(rsc->flags, pe_rsc_can_migrate)) {
	    if(start->node == NULL
	       || stop->node == NULL
	       || stop->node->details == start->node->details) {
		clear_bit(rsc->flags, pe_rsc_can_migrate);

	    } else if(at_stack_bottom(rsc) == FALSE) {
		clear_bit(rsc->flags, pe_rsc_can_migrate);
	    }
	}

	if(is_set(rsc->flags, pe_rsc_can_migrate)) {
	    action_t *to = NULL;
	    action_t *from = NULL;
	    crm_info("Migrating %s from %s to %s", rsc->id,
			 stop->node->details->uname,
			 start->node->details->uname);
		
		crm_free(stop->uuid);
		crm_free(stop->task);
		stop->task = crm_strdup(RSC_MIGRATE);
		stop->uuid = generate_op_key(rsc->id, stop->task, 0);
		add_hash_param(stop->meta, "migrate_source",
			       stop->node->details->uname);
		add_hash_param(stop->meta, "migrate_target",
			       start->node->details->uname);

		/* Create the correct ordering ajustments based on find_clone_activity_on(); */
		
		slist_iter(
		    constraint, rsc_colocation_t, rsc->rsc_cons, lpc,
		    
		    resource_t *target = constraint->rsc_rh;
		    crm_info("Repairing %s: %s == %s (%d)", constraint->id, rsc->id, target->id, constraint->score);

		    if(constraint->score > 0) {
			int mode = check_stack_element(rsc, target, "coloc");
			action_t *clone_stop = find_first_action(target->actions, NULL, RSC_STOP, NULL);
			action_t *clone_start = find_first_action(target->actions, NULL, RSC_STARTED, NULL);

			CRM_ASSERT(clone_stop != NULL);
			CRM_ASSERT(clone_start != NULL);
			CRM_ASSERT((mode & stack_middle) == 0);
			CRM_ASSERT(((mode & stack_stopping) && (mode & stack_starting)) == 0);
			
			if(mode & stack_stopping) {
			    action_t *clone_stop = find_first_action(target->actions, NULL, RSC_STOP, NULL);
			    action_t *clone_start = find_first_action(target->actions, NULL, RSC_STARTED, NULL);
			    crm_debug("Creating %s.start -> %s.stop ordering", rsc->id, target->id);

			    order_actions(start, clone_stop, pe_order_optional);

			    slist_iter(
				other_w, action_wrapper_t, start->actions_before, lpc2,
				/* Needed if the clone's started pseudo-action ever gets printed in the graph */ 
				if(other_w->action == clone_start) {
				    crm_debug("Breaking %s -> %s ordering", other_w->action->uuid, start->uuid);
				    other_w->type = pe_order_none;
				}
				);
			    
			} else if(mode & stack_starting) {
			    crm_debug("Creating %s.started -> %s.stop ordering", target->id, rsc->id);

			    order_actions(clone_start, stop, pe_order_optional);
			    
			    slist_iter(
				other_w, action_wrapper_t, clone_stop->actions_before, lpc2,
				/* Needed if the clone's stop pseudo-action ever gets printed in the graph */ 
				if(other_w->action == stop) {
				    crm_debug("Breaking %s -> %s ordering", other_w->action->uuid, clone_stop->uuid);
				    other_w->type = pe_order_none;
				}
				);
			}
		    }
		    );

		crm_free(start->uuid);
		crm_free(start->task);
		start->task = crm_strdup(RSC_MIGRATED);
		start->uuid = generate_op_key(rsc->id, start->task, 0);
		add_hash_param(start->meta, "migrate_source_uuid", stop->node->details->id);
		add_hash_param(start->meta, "migrate_source", stop->node->details->uname);
		add_hash_param(start->meta, "migrate_target", start->node->details->uname);

		/* Anything that needed stop to complete, now also needs start to have completed */
 		slist_iter(
			other_w, action_wrapper_t, stop->actions_after, lpc,
			other = other_w->action;
			if(other->optional || other->rsc != NULL) {
			    continue;
			}
			crm_debug("Ordering %s before %s (stop)", start->uuid, other_w->action->uuid);
			order_actions(start, other, other_w->type);
		    );

		/* Stop also needs anything that the start needed to have completed too */
 		slist_iter(
			other_w, action_wrapper_t, start->actions_before, lpc,
			other = other_w->action;
			if(other->rsc == NULL) {
			    /* nothing */
			} else if(other->optional || other->rsc == rsc || other->rsc == rsc->parent) {
			    continue;
			}
			crm_debug("Ordering %s before %s (start)", other_w->action->uuid, stop->uuid);
			order_actions(other, stop, other_w->type);
		    );

		/* Overwrite any op-specific params with those from the migrate ops */
		from = custom_action(
		    rsc, crm_strdup(start->uuid), start->task, start->node,
		    FALSE, FALSE, data_set);
		g_hash_table_foreach(start->meta, append_hashtable, from->meta);
		g_hash_table_destroy(start->meta);
		start->meta = from->meta;
		from->meta = NULL;
		pe_free_action(from);

		to = custom_action(
		    rsc, crm_strdup(stop->uuid), stop->task, stop->node,
		    FALSE, FALSE, data_set);
		g_hash_table_foreach(stop->meta, append_hashtable, to->meta);
		g_hash_table_destroy(stop->meta);
		stop->meta = to->meta;
		to->meta = NULL;
		pe_free_action(to);
		
	} else if(start && stop
		  && start->allow_reload_conversion
		  && stop->node->details == start->node->details) {
		action_t *rewrite = NULL;

		start->pseudo = TRUE; /* easier than trying to delete it from the graph */

		action = NULL;
		key = promote_key(rsc);
		action_list = find_actions(rsc->actions, key, NULL);
		if(action_list) {
		    action = action_list->data;
		}
		if(action && action->optional == FALSE) {
		    action->pseudo = TRUE;
		}
		g_list_free(action_list);
		crm_free(key);

		action = NULL;
		key = demote_key(rsc);
		action_list = find_actions(rsc->actions, key, NULL);
		if(action_list) {
		    action = action_list->data;
		}
		g_list_free(action_list);
		crm_free(key);

		if(action && action->optional == FALSE) {
		    rewrite = action;
		    stop->pseudo = TRUE;
		    
		} else {
		    rewrite = stop;
		}
		crm_info("Rewriting %s of %s on %s as a reload",
			 rewrite->task, rsc->id, stop->node->details->uname);
		
		crm_free(rewrite->uuid);
		crm_free(rewrite->task);
		rewrite->task = crm_strdup("reload");
		rewrite->uuid = generate_op_key(rsc->id, rewrite->task, 0);
		
	} else {
		do_crm_log_unlikely(level+1, "%s nothing to do", rsc->id);
	}
}

void native_append_meta(resource_t *rsc, xmlNode *xml)
{
    char *value = g_hash_table_lookup(rsc->meta, XML_RSC_ATTR_INCARNATION);
    if(value) {
	char *name = NULL;
	name = crm_meta_name(XML_RSC_ATTR_INCARNATION);
	crm_xml_add(xml, name, value);
	crm_free(name);
    }
}

