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
#ifndef PENGINE_AUTILS__H
#  define PENGINE_AUTILS__H

/* Constraint helper functions */
extern rsc_colocation_t *invert_constraint(rsc_colocation_t * constraint);

extern rsc_to_node_t *copy_constraint(rsc_to_node_t * constraint);

extern rsc_to_node_t *rsc2node_new(const char *id, resource_t * rsc, int weight,
                                   const char *discovery_mode, node_t * node,
                                   pe_working_set_t * data_set);

extern void pe_free_rsc_to_node(GListPtr constraints);
extern void pe_free_ordering(GListPtr constraints);

extern gboolean rsc_colocation_new(const char *id, const char *node_attr, int score,
                                   resource_t * rsc_lh, resource_t * rsc_rh,
                                   const char *state_lh, const char *state_rh,
                                   pe_working_set_t * data_set);

extern gboolean rsc_ticket_new(const char *id, resource_t * rsc_lh, ticket_t * ticket,
                               const char *state_lh, const char *loss_policy,
                               pe_working_set_t * data_set);

extern gint sort_node_weight(gconstpointer a, gconstpointer b, gpointer data_set);

extern gboolean can_run_resources(const node_t * node);
extern gboolean native_assign_node(resource_t * rsc, GListPtr candidates, node_t * chosen,
                                   gboolean force);
void native_deallocate(resource_t * rsc);

extern void log_action(unsigned int log_level, const char *pre_text,
                       action_t * action, gboolean details);

extern gboolean can_run_any(GHashTable * nodes);
extern resource_t *find_compatible_child(resource_t * local_child, resource_t * rsc,
                                         enum rsc_role_e filter, gboolean current);


enum filter_colocation_res {
    influence_nothing = 0,
    influence_rsc_location,
    influence_rsc_priority,
};

extern enum filter_colocation_res
filter_colocation_constraint(resource_t * rsc_lh, resource_t * rsc_rh,
                             rsc_colocation_t * constraint, gboolean preview);

extern int compare_capacity(const node_t * node1, const node_t * node2);
extern void calculate_utilization(GHashTable * current_utilization,
                                  GHashTable * utilization, gboolean plus);

extern void process_utilization(resource_t * rsc, node_t ** prefer, pe_working_set_t * data_set);

#  define STONITH_UP "stonith_up"
#  define STONITH_DONE "stonith_complete"
#  define ALL_STOPPED "all_stopped"
#  define LOAD_STOPPED "load_stopped"

#endif
