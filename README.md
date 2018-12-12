GSM (GNOME/GObject State Machine)
=================================

This is an experimental finite state machine (FSM) implementation for GObject.

If you have use for it, or any comments about how to improve the API, these
are welcome. You are also welcome to write something entirely new based on this.

The state machine has the following properties:
* A set of states, defined by GLib enum type
* States can be nested inside groups
* A set of inputs
* A set of outputs, mapped to an input or fixed value for each state
* A set of boolean conditionals, input values are mapped to a set of conditionals
* A set of events that can be triggered
* A set of transitions between states, each can depend on a single event and any number of conditionals

The state machine is currently automatically updated from an idle handler. It
only ever does one transition per idle loop iteration and currently runs with
the default idle priority in the default main context.

Properties:
* state-type: The GType of the state enum (construct only)
* state: current state (read only)
* running: Whether the state machine is updating (default: false)

Signals fired:
* state-enter: A state is entered (detail: state name)
* state-exit: A state is left (detail: state name)
* output-changed: Output was updated (detail: output name)
* input-changed: Input was updated (detail: input name)

Other notes:
* The enum cannot contain negative values (these are reserved for groups) and
  the initial state is defined as 0.
* Inputs of type Enum and Boolean can currently be converted into conditionals
* Enum conditionals can have a lesser equal/greater equal type. This means that
  all lesser/greater states will also be set for matching. To make this explicit,
  the user must prefix the matches with the `<=`/`>` prefix for lesser equal
  and `>`/`<=` prefix for greater equal types. Example:

  * A an input `user-idle` to mesure user activity could have the states
    `active`, `1min`, `5min` with the type lesser equal. Meaningful matches
    would be `>=user-idle::1min`, `>=user-idle::5min` and `<user-idle::1min`,
    `<user-idle::5min` (valid but useless are `>=user-idle::active` and
    `<user-idle::active`).

  * An input `fuel-level` might be used in greater equal mode to measure the
    fill level, with values of `empty`, `1quarter`, `half`, `3quarter` and
    `full`. Meaningful matches would be `>=fuel-level::1quarter`,
    `>=fuel-level::half`, `>=fuel-level::3quarter`, `>=fuel-level::full` and
    `<fuel-level::1quarter`, `<fuel-level::half`, `fuel-level::3quarter`,
    `<fuel-level::full`.

* Events are processed one at a time and only when the machines state is
  stable. i.e. updating an input and fireing an event at the same time will
  first result in the input changes to be completely processed.
* At startup the machine is in the initial state; no "state-enter" signal is
  currently emitted.
* Added transitions (edges) are tested to be orthogonal to all existing ones.
* DOT file generation is available


Further improvements:
* Allow finer control of when/how the state machine is updated
* Possibly add loop detection (i.e. an input condition that always updates)
* Review the lesser equal/greater equal conditional types.
* Clean up the code a lot


Example
-------

Enum in header (with GLib mkenums):
```
typedef enum {
  TEST_STATE_INIT,
  TEST_STATE_A,
  TEST_STATE_B,
  TEST_STATE_C,
} TestStateMachine;
```

Example defintion:
```
  GsmStateMachine *sm = NULL;

  sm = gsm_state_machine_new (TEST_TYPE_STATE_MACHINE);

  gsm_state_machine_add_input (sm,
                               g_param_spec_boolean ("bool-in", "BoolIn", "A test input boolean", FALSE, 0));
  gsm_state_machine_create_default_condition (sm, "bool-in");

  /* An input/output pair */
  gsm_state_machine_add_input (sm,
                               g_param_spec_double ("float", "Float", "A float input", 0, 100, 0, 0));
  gsm_state_machine_add_output (sm,
                               g_param_spec_double ("float", "Float", "A float output", 0, 100, 0, 0));


  gsm_state_machine_add_event (sm, "event");

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

  gsm_state_machine_add_edge (sm,
                              TEST_STATE_B,
                              TEST_STATE_C,
                              "event",
                              NULL);

  gsm_state_machine_set_running (sm, TRUE);

  /* Map output depending on state */
  gsm_state_machine_map_output (sm, TEST_STATE_A, "float", "float");
  gsm_state_machine_map_output (sm, TEST_STATE_B, "float", "float");
  gsm_state_machine_set_output (sm, TEST_STATE_C, "float", (gdouble) 40.0);



  g_object_connect (sm,
                    "swapped-signal::state-enter::a", state_a_entered, self,
                    "swapped-signal::state-exit::a", state_a_left, self,
                    "swapped-signal::output-changed::float", state_a_left, self,
                    NULL);

```

Example updates:
```
  gsm_state_machine_set_input (sm, "float", (gdouble) 10.0);

  gsm_state_machine_set_input (sm, "bool-in", FALSE);

  gsm_state_machine_set_queue_event (sm, "event");
```
