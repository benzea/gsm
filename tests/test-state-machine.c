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


  g_value_init (&value, G_TYPE_BOOLEAN);
  g_value_set_boolean (&value, FALSE);
  gsm_state_machine_set_input_value (sm, "bool-in", &value);
  g_assert_cmpint (gsm_state_machine_get_state (sm), ==, TEST_STATE_A);
  g_assert_cmpint (counter_state_enter_a, ==, 1);
  g_assert_cmpint (counter_state_enter_b, ==, 0);
  g_assert_cmpint (counter_state_exit_a, ==, 0);
  g_assert_cmpint (counter_state_exit_b, ==, 0);

  gsm_state_machine_set_input_value (sm, "bool-in", &value);
  g_assert_cmpint (gsm_state_machine_get_state (sm), ==, TEST_STATE_A);
  g_assert_cmpint (counter_state_enter_a, ==, 1);
  g_assert_cmpint (counter_state_enter_b, ==, 0);
  g_assert_cmpint (counter_state_exit_a, ==, 0);
  g_assert_cmpint (counter_state_exit_b, ==, 0);


  g_value_set_boolean (&value, TRUE);
  gsm_state_machine_set_input_value (sm, "bool-in", &value);
  g_assert_cmpint (gsm_state_machine_get_state (sm), ==, TEST_STATE_B);
  g_assert_cmpint (counter_state_enter_a, ==, 1);
  g_assert_cmpint (counter_state_enter_b, ==, 1);
  g_assert_cmpint (counter_state_exit_a, ==, 1);
  g_assert_cmpint (counter_state_exit_b, ==, 0);

  gsm_state_machine_set_input_value (sm, "bool-in", &value);
  g_assert_cmpint (gsm_state_machine_get_state (sm), ==, TEST_STATE_B);
  g_assert_cmpint (counter_state_enter_a, ==, 1);
  g_assert_cmpint (counter_state_enter_b, ==, 1);
  g_assert_cmpint (counter_state_exit_a, ==, 1);
  g_assert_cmpint (counter_state_exit_b, ==, 0);


  g_value_set_boolean (&value, FALSE);
  gsm_state_machine_set_input_value (sm, "bool-in", &value);
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

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/gsm-state-machine/init",
                   test_init);

  g_test_add_func ("/gsm-state-machine/simple-machine",
                   test_simple_machine);

  g_test_run ();
}
