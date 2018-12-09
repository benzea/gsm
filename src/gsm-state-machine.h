/* gsm-state-machine.c
 *
 * Copyright 2018 Benjamin Berg <bberg@redhat.com>
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 3 of the
 * License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "gsm.h"
#include <glib-object.h>

G_BEGIN_DECLS

/** GsmStateMachineConditionFunc()
 *
 * Converts a #GValue value to a GQuark as used by the state machine.
 */
typedef GQuark (*GsmStateMachineConditionFunc) (GQuark condition, const GValue *value);


#define GSM_TYPE_STATE_MACHINE (gsm_state_machine_get_type())

G_DECLARE_DERIVABLE_TYPE (GsmStateMachine, gsm_state_machine, GSM, STATE_MACHINE, GObject)

struct _GsmStateMachineClass
{
  GObjectClass parent_class;

  /*< public >*/
  void (*state_enter) (gint new_state, gint old_state, gboolean intermediate);
  void (*state_exit) (gint old_state, gint target_state, gboolean intermediate);

  void (*input_changed) (const gchar *name, GValue value);
  void (*output_changed) (const gchar *name, GValue value, gboolean state_change, gboolean intermediate);
};

GsmStateMachine *gsm_state_machine_new                 (GType state_type);

gint             gsm_state_machine_get_state           (GsmStateMachine  *state_machine);
GType            gsm_state_machine_get_state_type      (GsmStateMachine  *state_machine);


void             gsm_state_machine_add_input           (GsmStateMachine  *state_machine,
                                                        GParamSpec       *pspec);
void             gsm_state_machine_add_output          (GsmStateMachine  *state_machine,
                                                        GParamSpec       *pspec);

void             gsm_state_machine_map_output          (GsmStateMachine  *state_machine,
                                                        gint              state,
                                                        const gchar      *output,
                                                        const gchar      *input);
#if 0
void             gsm_state_machine_map_output_default  (GsmStateMachine  *state_machine,
                                                        const gchar      *output,
                                                        const gchar      *input);
#endif

#if 0
void             gsm_state_machine_get_input           (GsmStateMachine  *state_machine,
                                                        const gchar      *input,
                                                        ...);
#endif
void             gsm_state_machine_get_input_value     (GsmStateMachine  *state_machine,
                                                        const gchar      *input,
                                                        GValue           *out);

#if 0
void             gsm_state_machine_set_input           (GsmStateMachine  *state_machine,
                                                        gint              state,
                                                        const gchar      *input,
                                                        ...);
#endif
void             gsm_state_machine_set_input_value     (GsmStateMachine  *state_machine,
                                                        const gchar      *input,
                                                        const GValue     *value);

#if 0
void             gsm_state_machine_get_output          (GsmStateMachine  *state_machine,
                                                        const gchar      *output,
                                                        ...);
#endif
void             gsm_state_machine_get_output_value    (GsmStateMachine  *state_machine,
                                                        const gchar      *output,
                                                        GValue           *out);

#if 0
void             gsm_state_machine_set_output          (GsmStateMachine  *state_machine,
                                                        gint              state,
                                                        const gchar      *output,
                                                        ...);
#endif
void             gsm_state_machine_set_output_value    (GsmStateMachine  *state_machine,
                                                        gint              state,
                                                        const gchar      *output,
                                                        const GValue     *value);


void             gst_state_machine_create_condition    (GsmStateMachine  *state_machine,
                                                        const gchar      *input,
                                                        const GStrv       conditions,
                                                        GsmStateMachineConditionFunc func);

void             gst_state_machine_create_default_condition (GsmStateMachine  *state_machine,
                                                             const gchar      *input);


void             gsm_state_machine_add_edge            (GsmStateMachine  *state_machine,
                                                        gint              start_state,
                                                        gint              target_state,
                                                        ...) G_GNUC_NULL_TERMINATED;

void             gsm_state_machine_add_edge_valist     (GsmStateMachine  *state_machine,
                                                        gint              start_state,
                                                        gint              target_state,
                                                        va_list           var_args);

void             gsm_state_machine_add_edge_strv       (GsmStateMachine  *state_machine,
                                                        gint              start_state,
                                                        gint              target_state,
                                                        const GStrv       conditions);


G_END_DECLS
