GSM (GNOME/GObject State Machine)
=================================

This is an experimental finite state machine (FSM) implementation for GObject.

If you have use for it, or any comments about how to improve the API, these
are welcome. You are also welcome to write something entirely new based on this.

The state machine has the following properties:
* A set of states, defined by GLib enum type
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
* Events are processed one at a time and only when the machines state is
  stable. i.e. updating an input and fireing an event at the same time will
  first result in the input changes to be completely processed.
* At startup the machine is in the initial state; no "state-enter" signal is
  currently emitted.
* Added transitions (edges) are tested to be orthogonal to all existing ones.
* DOT file generation is available


Further improvements:
* Allow finer control of when/how the state machine is updated
* Allow conditionals with multiple conditions being turned on. Right now each
  input is mapped to a set of boolean conditionals of which only one can be
  TRUE at a time.
  However, we only need a unique mapping from one conditional to the state of
  all other conditionals. So we could define conditionals such as
  "`time::>1min`", "`time::>2min`", "`time::>3min`" and e.g. the "`!time::>2`"
  conditional represents the set "`time::>1`,`!time::>2`,`time::!>3`".
* Add proper support for groups of states (an implicit ALL group already
  exists). A group is defined as a set of states with one leader state.
* Possibly add loop detection (i.e. an input condition that always updates)


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
