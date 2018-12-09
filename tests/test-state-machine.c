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


#include <glib.h>
#include <glib/gi18n.h>
#include "gsm-state-machine.h"
#include "test-state-machine.h"
#include "test-enum-types.h"

static void
test_init (void)
{
  G_GNUC_UNUSED g_autoptr(GsmStateMachine) sm = NULL;

  sm = gsm_state_machine_new (TEST_TYPE_STATE_MACHINE);
}

static void
count_signal (gint *counter)
{
  *counter += 1;
}

static void
test_simple_machine (void)
{
  GMainContext *ctx = g_main_context_default ();
  g_autoptr(GsmStateMachine) sm = NULL;
  g_auto(GValue) value = G_VALUE_INIT;
  gint counter_state_enter_a = 0;
  gint counter_state_exit_a = 0;
  gint counter_state_enter_b = 0;
  gint counter_state_exit_b = 0;

  sm = gsm_state_machine_new (TEST_TYPE_STATE_MACHINE);

  gsm_state_machine_add_input (sm,
                               g_param_spec_boolean ("bool-in", "BoolIn", "A test input boolean", FALSE, 0));
  gst_state_machine_create_default_condition (sm, "bool-in");

  gsm_state_machine_add_edge (sm,
                              TEST_STATE_INIT,
                              TEST_STATE_A,
                              NULL);

  gsm_state_machine_add_edge (sm,
                              TEST_STATE_A,
                              TEST_STATE_B,
                              "bool-in",
                              NULL);

  gsm_state_machine_add_edge (sm,
                              TEST_STATE_B,
                              TEST_STATE_A,
                              "!bool-in",
                              NULL);

  g_object_connect (sm,
                    "swapped-signal::state-enter::a", count_signal, &counter_state_enter_a,
                    "swapped-signal::state-exit::a", count_signal, &counter_state_exit_a,
                    "swapped-signal::state-enter::b", count_signal, &counter_state_enter_b,
                    "swapped-signal::state-exit::b", count_signal, &counter_state_exit_b,
                    NULL);

  gsm_state_machine_set_running (sm, TRUE);

  g_value_init (&value, G_TYPE_BOOLEAN);
  g_value_set_boolean (&value, FALSE);
  gsm_state_machine_set_input_value (sm, "bool-in", &value);
  while (g_main_context_iteration (ctx, FALSE)) {}
  g_assert_cmpint (gsm_state_machine_get_state (sm), ==, TEST_STATE_A);
  g_assert_cmpint (counter_state_enter_a, ==, 1);
  g_assert_cmpint (counter_state_enter_b, ==, 0);
  g_assert_cmpint (counter_state_exit_a, ==, 0);
  g_assert_cmpint (counter_state_exit_b, ==, 0);

  gsm_state_machine_set_input_value (sm, "bool-in", &value);
  while (g_main_context_iteration (ctx, FALSE)) {}
  g_assert_cmpint (gsm_state_machine_get_state (sm), ==, TEST_STATE_A);
  g_assert_cmpint (counter_state_enter_a, ==, 1);
  g_assert_cmpint (counter_state_enter_b, ==, 0);
  g_assert_cmpint (counter_state_exit_a, ==, 0);
  g_assert_cmpint (counter_state_exit_b, ==, 0);


  g_value_set_boolean (&value, TRUE);
  gsm_state_machine_set_input_value (sm, "bool-in", &value);
  while (g_main_context_iteration (ctx, FALSE)) {}
  g_assert_cmpint (gsm_state_machine_get_state (sm), ==, TEST_STATE_B);
  g_assert_cmpint (counter_state_enter_a, ==, 1);
  g_assert_cmpint (counter_state_enter_b, ==, 1);
  g_assert_cmpint (counter_state_exit_a, ==, 1);
  g_assert_cmpint (counter_state_exit_b, ==, 0);

  gsm_state_machine_set_input_value (sm, "bool-in", &value);
  while (g_main_context_iteration (ctx, FALSE)) {}
  g_assert_cmpint (gsm_state_machine_get_state (sm), ==, TEST_STATE_B);
  g_assert_cmpint (counter_state_enter_a, ==, 1);
  g_assert_cmpint (counter_state_enter_b, ==, 1);
  g_assert_cmpint (counter_state_exit_a, ==, 1);
  g_assert_cmpint (counter_state_exit_b, ==, 0);


  g_value_set_boolean (&value, FALSE);
  gsm_state_machine_set_input_value (sm, "bool-in", &value);
  while (g_main_context_iteration (ctx, FALSE)) {}
  g_assert_cmpint (gsm_state_machine_get_state (sm), ==, TEST_STATE_A);
  g_assert_cmpint (counter_state_enter_a, ==, 2);
  g_assert_cmpint (counter_state_enter_b, ==, 1);
  g_assert_cmpint (counter_state_exit_a, ==, 1);
  g_assert_cmpint (counter_state_exit_b, ==, 1);

  gsm_state_machine_set_input_value (sm, "bool-in", &value);
  g_assert_cmpint (gsm_state_machine_get_state (sm), ==, TEST_STATE_A);
  g_assert_cmpint (counter_state_enter_a, ==, 2);
  g_assert_cmpint (counter_state_enter_b, ==, 1);
  g_assert_cmpint (counter_state_exit_a, ==, 1);
  g_assert_cmpint (counter_state_exit_b, ==, 1);
}

static void
test_groups (void)
{
  GMainContext *ctx = g_main_context_default ();
  g_autoptr(GsmStateMachine) sm = NULL;

  sm = gsm_state_machine_new (TEST_TYPE_STATE_MACHINE);

  gsm_state_machine_add_input (sm,
                               g_param_spec_boolean ("bool-in", "BoolIn", "A test input boolean", FALSE, 0));
  gst_state_machine_create_default_condition (sm, "bool-in");

  gsm_state_machine_add_edge (sm,
                              GSM_STATES_ALL,
                              TEST_STATE_A,
                              NULL);

  gsm_state_machine_set_running (sm, TRUE);
  while (g_main_context_iteration (ctx, FALSE)) {}
  g_assert_cmpint (gsm_state_machine_get_state (sm), ==, TEST_STATE_A);
}

static void
test_orthogonal_transitions (void)
{
  g_autoptr(GsmStateMachine) sm = NULL;

  sm = gsm_state_machine_new (TEST_TYPE_STATE_MACHINE);

  gsm_state_machine_add_input (sm,
                               g_param_spec_boolean ("bool", "Bool1", "A test input boolean", FALSE, 0));
  gst_state_machine_create_default_condition (sm, "bool");

  gsm_state_machine_add_input (sm,
                               g_param_spec_enum ("enum", "Enum", "A test input enum", TEST_TYPE_STATE_MACHINE, 0, 0));
  gst_state_machine_create_default_condition (sm, "enum");

  gsm_state_machine_add_edge (sm,
                              TEST_STATE_INIT,
                              TEST_STATE_A,
                              "bool",
                              NULL);

  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*not mutally exclusive*");
  /* Not possible to add as a "bool" transition already exists */
  gsm_state_machine_add_edge (sm,
                              TEST_STATE_INIT,
                              TEST_STATE_A,
                              "enum::a",
                              NULL);
  g_test_assert_expected_messages ();

  gsm_state_machine_add_edge (sm,
                              TEST_STATE_INIT,
                              TEST_STATE_A,
                              "enum::b",
                              "!bool",
                              NULL);

  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*not mutally exclusive*");
  /* Not possible, overlaps with previous */
  gsm_state_machine_add_edge (sm,
                              TEST_STATE_INIT,
                              TEST_STATE_A,
                              "!enum::a",
                              "!bool",
                              NULL);
  g_test_assert_expected_messages ();

  /* Possible, no overlap with "enum::b" */
  gsm_state_machine_add_edge (sm,
                              TEST_STATE_INIT,
                              TEST_STATE_A,
                              "enum::init",
                              "!bool",
                              NULL);
}


static void
test_output (void)
{
  GMainContext *ctx = g_main_context_default ();
  g_autoptr(GsmStateMachine) sm = NULL;
  g_auto(GValue) value = G_VALUE_INIT;
  g_auto(GValue) value_bool = G_VALUE_INIT;
  gint counter_output_updated = 0;

  sm = gsm_state_machine_new (TEST_TYPE_STATE_MACHINE);

  gsm_state_machine_add_input (sm,
                               g_param_spec_boolean ("bool", "Bool1", "A test input boolean", FALSE, 0));
  gst_state_machine_create_default_condition (sm, "bool");

  gsm_state_machine_add_input (sm,
                               g_param_spec_float ("float", "Float", "A float input", 0, 100, 0, 0));
  gsm_state_machine_add_output (sm,
                               g_param_spec_float ("float", "Float", "A float output", 0, 100, 0, 0));

  g_object_connect (sm,
                    "swapped-signal::output-changed::float", count_signal, &counter_output_updated,
                    NULL);

  gsm_state_machine_map_output (sm, TEST_STATE_A, "float", "float");
  g_value_init (&value, G_TYPE_FLOAT);
  g_value_set_float (&value, 20);
  gsm_state_machine_set_input_value (sm, "float", &value);

  /* Set value to 10 in initial state and check that it comes through */
  g_value_set_float (&value, 10);
  gsm_state_machine_set_output_value (sm, TEST_STATE_INIT, "float", &value);
  g_value_unset (&value);
  gsm_state_machine_get_output_value (sm, "float", &value);
  g_assert_cmpfloat (g_value_get_float (&value), ==, 10);
  g_assert_cmpint (counter_output_updated, ==, 1);

  gsm_state_machine_add_edge (sm,
                              TEST_STATE_INIT, TEST_STATE_A,
                              NULL);

  gsm_state_machine_add_edge (sm,
                              TEST_STATE_A, TEST_STATE_B,
                              "bool", NULL);

  gsm_state_machine_add_edge (sm,
                              TEST_STATE_B, TEST_STATE_A,
                              "!bool", NULL);


  gsm_state_machine_set_running (sm, TRUE);

  g_value_init (&value_bool, G_TYPE_BOOLEAN);
  g_value_set_boolean (&value_bool, TRUE);
  gsm_state_machine_set_input_value (sm, "bool", &value_bool);
  while (g_main_context_iteration (ctx, FALSE)) {}

  /* Back to the default value after the two transitions, that is
   * INIT -> A -> B */
  g_value_unset (&value);
  gsm_state_machine_get_output_value (sm, "float", &value);
  g_assert_cmpfloat (g_value_get_float (&value), ==, 0);
  g_assert_cmpint (counter_output_updated, ==, 3);


  g_value_set_boolean (&value_bool, FALSE);
  gsm_state_machine_set_input_value (sm, "bool", &value_bool);
  while (g_main_context_iteration (ctx, FALSE)) {}

  /* State A has the "float" input as output, which is currently 20 */
  g_value_unset (&value);
  gsm_state_machine_get_output_value (sm, "float", &value);
  g_assert_cmpfloat (g_value_get_float (&value), ==, 20);
  g_assert_cmpint (counter_output_updated, ==, 4);

  /* Set input to 30 and see it change */
  g_value_set_float (&value, 30);
  gsm_state_machine_set_input_value (sm, "float", &value);
  while (g_main_context_iteration (ctx, FALSE)) {}
  g_value_unset (&value);
  gsm_state_machine_get_output_value (sm, "float", &value);
  g_assert_cmpfloat (g_value_get_float (&value), ==, 30);
  g_assert_cmpint (counter_output_updated, ==, 5);

}

static void
test_events (void)
{
  GMainContext *ctx = g_main_context_default ();
  g_autoptr(GsmStateMachine) sm = NULL;
  g_auto(GValue) value = G_VALUE_INIT;

  sm = gsm_state_machine_new (TEST_TYPE_STATE_MACHINE);

  gsm_state_machine_add_input (sm,
                               g_param_spec_boolean ("bool", "Bool", "A test input boolean", FALSE, 0));
  gst_state_machine_create_default_condition (sm, "bool");

  gsm_state_machine_add_event (sm, "event");


  gsm_state_machine_add_edge (sm,
                              TEST_STATE_INIT, TEST_STATE_A,
                              "bool",
                              NULL);

  gsm_state_machine_add_edge (sm,
                              TEST_STATE_A, TEST_STATE_INIT,
                              "!bool",
                              NULL);

  gsm_state_machine_add_edge (sm,
                              TEST_STATE_A, TEST_STATE_B,
                              "event", NULL);

  gsm_state_machine_add_edge (sm,
                              TEST_STATE_B, TEST_STATE_A,
                              NULL);

  gsm_state_machine_set_running (sm, TRUE);

  g_main_context_iteration (ctx, FALSE);
  g_value_init (&value, G_TYPE_BOOLEAN);
  g_value_set_boolean (&value, TRUE);
  gsm_state_machine_set_input_value (sm, "bool", &value);
  gsm_state_machine_queue_event (sm, "event");

  /* First we switch to A */
  g_main_context_iteration (ctx, FALSE);
  g_assert_cmpint (gsm_state_machine_get_state (sm), ==, TEST_STATE_A);

  /* There we stop and the event goes to B */
  g_main_context_iteration (ctx, FALSE);
  g_assert_cmpint (gsm_state_machine_get_state (sm), ==, TEST_STATE_B);

  /* Then we automatically go back to A */
  g_main_context_iteration (ctx, FALSE);
  g_assert_cmpint (gsm_state_machine_get_state (sm), ==, TEST_STATE_A);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/gsm-state-machine/init",
                   test_init);

  g_test_add_func ("/gsm-state-machine/simple-machine",
                   test_simple_machine);

  g_test_add_func ("/gsm-state-machine/groups",
                   test_groups);

  g_test_add_func ("/gsm-state-machine/orthogonal-transitions",
                   test_orthogonal_transitions);

  g_test_add_func ("/gsm-state-machine/output",
                   test_output);

  g_test_add_func ("/gsm-state-machine/events",
                   test_events);

  g_test_run ();
}
