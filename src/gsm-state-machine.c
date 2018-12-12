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

#include <gobject/gvaluecollector.h>
#include "gsm-state-machine.h"

typedef struct _GsmStateMachineState GsmStateMachineState;

typedef struct
{
  GType       state_type;

  gint        state;

  GArray     *events;
  GPtrArray  *input_conditions;

  GArray     *active_conditions;
  GQuark      active_event;

  GList      *pending_events;

  GHashTable *inputs;
  GHashTable *outputs;
  GArray     *outputs_quark;

  GPtrArray  *current_outputs;

  GHashTable *states;
  GsmStateMachineState *all_state;
  gint        last_group;

  gboolean    running;
  guint       idle_source_id;
} GsmStateMachinePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GsmStateMachine, gsm_state_machine, G_TYPE_OBJECT)
#define GSM_STATE_MACHINE_PRIVATE(obj) gsm_state_machine_get_instance_private (obj)

static void gsm_state_machine_internal_queue_update (GsmStateMachine *state_machine);


enum {
  PROP_0,
  PROP_STATE,
  PROP_STATE_TYPE,
  PROP_RUNNING,
  N_PROPS
};

static GParamSpec *properties [N_PROPS];

enum {
  SIGNAL_STATE_ENTER,
  SIGNAL_STATE_EXIT,

  SIGNAL_INPUT_CHANGED,
  SIGNAL_OUTPUT_CHANGED,

  N_SIGNALS,
};
static guint signals [N_SIGNALS];

/* An input condition with one or more virtual conditions */
typedef struct
{
  GsmConditionType type;
  GsmConditionFunc getter;

  GQuark input;
  GArray *conditions;
  GArray *conditions_neg;
} GsmStateMachineCondition;

static GsmStateMachineCondition*
gsm_state_machine_condition_new ()
{
  GsmStateMachineCondition* res = g_new0 (GsmStateMachineCondition, 1);

  res->conditions = g_array_new (FALSE, TRUE, sizeof(GQuark));
  res->conditions_neg = g_array_new (FALSE, TRUE, sizeof(GQuark));

  return res;
}

static void
gsm_state_machine_input_condition_destroy (GsmStateMachineCondition* condition)
{
  g_array_unref (condition->conditions);
  g_array_unref (condition->conditions_neg);
  g_free (condition);
}

static GsmStateMachineCondition*
gsm_state_machine_condition_from_quark (GsmStateMachine *state_machine,
                                        GQuark           condition)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  g_autofree gchar *str_free;
  gchar *str;
  gchar *separator;
  GQuark input;

  str = str_free = g_strdup (g_quark_to_string (condition));
  separator = strchr(str, ':');
  if (separator)
    *separator = '\0';
  while (str[0] == '!' || str[0] == '<' || str[0] == '>' || str[0] == '=')
    str++;

  input = g_quark_try_string (str);

  for (gint i = 0; i < priv->input_conditions->len; i++)
    {
      GsmStateMachineCondition *cond;
      cond = g_ptr_array_index (priv->input_conditions, i);
      if (cond->input == input)
        return cond;
    }

  return NULL;
}

/* A value in the inputs/outputs dictionaries */
typedef struct
{
  guint         idx;
  GParamSpec   *pspec;
  GValue        value;
} GsmStateMachineValue;

static GsmStateMachineValue*
gsm_state_machine_value_new ()
{
  return g_new0 (GsmStateMachineValue, 1);
}

static void
gsm_state_machine_value_destroy (GsmStateMachineValue *value)
{
  g_clear_pointer (&value->pspec, g_param_spec_unref);
  g_value_reset (&value->value);
  g_free (value);
}



typedef struct
{
  gint    target_state;

  GQuark event;
  GArray *conditions;
} GsmStateMachineTransition;

static GsmStateMachineTransition*
gsm_state_machine_transition_new ()
{
  GsmStateMachineTransition *res;

  res = g_new0 (GsmStateMachineTransition, 1);
  res->conditions = g_array_new (FALSE, TRUE, sizeof(GQuark));

  return res;
}

static void
gsm_state_machine_transition_destroy (GsmStateMachineTransition *transition)
{
  g_array_unref (transition->conditions);
  g_free (transition);
}


struct _GsmStateMachineState
{
  GsmStateMachineState *parent;
  GsmStateMachineState *leader;
  GPtrArray            *all_children;

  gint          value;
  GQuark        nick;

  /* This points either into a GsmStateMachineValue associated with an input or output,
   * or it points into owned_values for a constant. */
  GPtrArray    *outputs;
  GPtrArray    *owned_values;

  GPtrArray    *transitions;
};


static gint
_condition_cmp (gconstpointer a, gconstpointer b)
{
  const GQuark *qa, *qb;

  qa = a;
  qb = b;

  if (*qa < *qb)
    return -1;
  if (*qa > *qb)
    return 1;

  return 0;
}

static void
_condition_expand_positive (GQuark active, GsmStateMachineCondition *condition, GArray *target)
{
  gboolean found;
  gboolean lesser, greater;

  /* Active may be 0 if this is a boolean (i.e. only one value), in which case it means
   * it is the negated value; do a direct exit */
  if (active == 0)
    {
      g_assert (condition->conditions->len == 1);

      g_array_append_val (target, g_array_index (condition->conditions_neg, GQuark, 0));
      return;
    }

  switch (condition->type)
    {
    case GSM_CONDITION_TYPE_EQ:
      lesser = FALSE;
      greater = FALSE;
      break;
    case GSM_CONDITION_TYPE_GEQ:
      lesser = TRUE;
      greater = FALSE;
      break;
    case GSM_CONDITION_TYPE_LEQ:
      lesser = FALSE;
      greater = TRUE;
      break;
    }

  found = FALSE;
  for (guint j = 0; j < condition->conditions->len; j++)
    {
      gboolean cond_state;
      gboolean this = FALSE;

      if (g_array_index (condition->conditions, GQuark, j) == active)
        {
          g_assert (found == FALSE);
          found = TRUE;
          this = TRUE;
        }

      if (this)
        cond_state = TRUE;
      else if (found)
        cond_state = greater;
      else
        cond_state = lesser;

      if (cond_state)
        g_array_append_val (target, g_array_index (condition->conditions, GQuark, j));
      else
        g_array_append_val (target, g_array_index (condition->conditions_neg, GQuark, j));
    }

  g_assert (found == TRUE);
}

static void
_condition_expand_no_overlap (GQuark active, GsmStateMachineCondition *condition, GArray *target)
{
  gboolean negated;
  gint idx;
  gboolean supress_same_state;
  gboolean equal, lesser, greater;

  for (idx = 0; idx < condition->conditions->len; idx++)
    {
      if (g_array_index (condition->conditions, GQuark, idx) == active)
        {
          negated = TRUE;
          break;
        }
      if (g_array_index (condition->conditions_neg, GQuark, idx) == active)
        {
          negated = FALSE;
          break;
        }
    }
  g_assert (idx < condition->conditions->len);

  /* For the lesser/greater equal cases the non-negated states must be
   * supressed as they always imply an overlap. */
  switch (condition->type)
    {
    case GSM_CONDITION_TYPE_EQ:
      equal = TRUE;
      lesser = FALSE;
      greater = FALSE;
      supress_same_state = FALSE;
      break;
    case GSM_CONDITION_TYPE_GEQ:
      equal = TRUE;
      lesser = TRUE;
      greater = FALSE;
      supress_same_state = TRUE;
      break;
    case GSM_CONDITION_TYPE_LEQ:
      equal = TRUE;
      lesser = FALSE;
      greater = TRUE;
      supress_same_state = TRUE;
      break;
    }

  if (negated)
    {
      equal = !equal;
      lesser = !lesser;
      greater = !greater;
    }

  for (guint j = 0; j < condition->conditions->len; j++)
    {
      gboolean cond_state;

      if (j == idx)
        cond_state = equal;
      else if (j > idx)
        cond_state = greater;
      else
        cond_state = lesser;

      if (!supress_same_state || cond_state != negated)
        {
          if (cond_state)
            g_array_append_val (target, g_array_index (condition->conditions, GQuark, j));
          else
            g_array_append_val (target, g_array_index (condition->conditions_neg, GQuark, j));
        }
    }
}

typedef gboolean (GsmConditionsCompareFunc) (GArray *set, GArray *conditions);

static gboolean
_conditions_is_subset (GArray *set, GArray *conditions)
{
  gint i, j;
  /* Assume both sets are sorted, i.e. we only need to check that each
   * element in conditions is included in set. */

  for (i = 0, j = 0; i < conditions->len; i++)
    {
      GQuark condition = g_array_index (conditions, GQuark, i);

      while ((j < set->len) && (g_array_index (set, GQuark, j) < condition))
        j++;

      if ((j >= set->len) || (condition != g_array_index (set, GQuark, j)))
        return FALSE;
    }

  return TRUE;
}

static gboolean
_conditions_is_disjunct (GArray *set, GArray *conditions)
{
  gint i, j;
  /* Assume both sets are sorted, i.e. we only need to check that each
   * element in conditions is included in set. */

  for (i = 0, j = 0; i < conditions->len; i++)
    {
      GQuark condition = g_array_index (conditions, GQuark, i);

      while ((j < set->len) && (g_array_index (set, GQuark, j) < condition))
        j++;

      if ((j < set->len) && (condition == g_array_index (set, GQuark, j)))
        return FALSE;
    }

  return TRUE;
}

static gboolean
_machine_has_condition (GsmStateMachine *state_machine, GQuark condition)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  GsmStateMachineCondition *input_cond;

  for (guint i = 0; i < priv->input_conditions->len; i++)
    {
      input_cond = g_ptr_array_index (priv->input_conditions, i);

      for (guint j = 0; j < input_cond->conditions->len; j++)
        {
          if (condition == g_array_index (input_cond->conditions, GQuark, j))
            return TRUE;

          if (condition == g_array_index (input_cond->conditions_neg, GQuark, j))
            return TRUE;
        }
    }

  return FALSE;
}

static gboolean
_machine_has_event (GsmStateMachine *state_machine, GQuark event)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);

  for (guint i = 0; i < priv->events->len; i++)
    {
      GQuark known_event = g_array_index (priv->events, GQuark, i);

      if (known_event == event)
        return TRUE;
    }

  return FALSE;
}

static void
gsm_state_machine_state_ensure_outputs (GsmStateMachineState *state, GHashTable *outputs)
{
  gint count = g_hash_table_size (outputs);

  if (!state->outputs)
    state->outputs = g_ptr_array_new_full (count, NULL);

  if (state->outputs->len == count)
    return;

  g_ptr_array_set_size (state->outputs, count);
}

static void
_value_free (gpointer data)
{
  g_value_reset ((GValue*) data);
  g_free (data);
}

static GsmStateMachineState*
gsm_state_machine_state_new (GQuark nick, gint value)
{
  GsmStateMachineState *res = g_new0 (GsmStateMachineState, 1);

  /* We know that nick is from an GEnumValue */
  res->nick = nick;
  res->value = value;
  res->owned_values = g_ptr_array_new_with_free_func (_value_free);
  res->transitions = g_ptr_array_new_with_free_func ((GDestroyNotify) gsm_state_machine_transition_destroy);

  return res;
}

static GsmStateMachineTransition*
gsm_state_machine_real_find_transition (GsmStateMachineState      *state,
                                        GQuark                     event,
                                        GArray                    *conditions,
                                        GsmConditionsCompareFunc   test_func)
{
  for (guint i = 0; i < state->transitions->len; i++)
    {
      GsmStateMachineTransition *item = g_ptr_array_index (state->transitions, i);

      /* Cannot match if the events differ. */
      if (event != item->event)
        continue;

      if (test_func (conditions, item->conditions))
        return item;
    }

  return NULL;
}

static GsmStateMachineTransition*
gsm_state_machine_find_transition (GsmStateMachineState      *state,
                                   GQuark                     event,
                                   GArray                    *conditions,
                                   GsmConditionsCompareFunc   test_func,
                                   GsmStateMachineState     **in_state)
{
  do
    {
      GsmStateMachineTransition* res;
      res = gsm_state_machine_real_find_transition (state, event, conditions, test_func);
      if (res)
        {
          if (in_state)
            *in_state = state;
          return res;
        }

      state = state->parent;
    }
  while (state);

  return NULL;
}

static GsmStateMachineTransition*
gsm_state_machine_children_find_transition (GsmStateMachineState      *state,
                                            GQuark                     event,
                                            GArray                    *conditions,
                                            GsmConditionsCompareFunc   test_func,
                                            GsmStateMachineState     **in_state)
{
  GsmStateMachineTransition* res;

  res = gsm_state_machine_real_find_transition (state, event, conditions, test_func);
  if (res)
    {
      if (in_state)
        *in_state = state;
      return res;
    }

  if (!state->all_children)
    return NULL;

  for (guint i = 0; i < state->all_children->len; i++)
    {
      res = gsm_state_machine_children_find_transition (g_ptr_array_index (state->all_children, i), event, conditions, test_func, in_state);
      if (res)
        return res;
    }

  return NULL;
}

static void
gsm_state_machine_state_add_transition (GsmStateMachine            *state_machine,
                                        GsmStateMachineState       *state,
                                        GsmStateMachineTransition  *transition)
{
  GsmStateMachineState *in_state = NULL;
  g_autoptr(GArray) conditions_neg = NULL;

  conditions_neg = g_array_new (FALSE, FALSE, sizeof(GQuark));

  /* XXX: This is relatively slow unfortunately; but also executed seldomly! */
  for (guint i = 0; i < transition->conditions->len; i++)
    {
      GQuark cond = g_array_index (transition->conditions, GQuark, i);
      GsmStateMachineCondition *condition = gsm_state_machine_condition_from_quark (state_machine, cond);

      _condition_expand_no_overlap (cond, condition, conditions_neg);
    }
  g_array_sort (conditions_neg, _condition_cmp);

  if (gsm_state_machine_find_transition (state, transition->event, conditions_neg, _conditions_is_disjunct, &in_state) ||
      gsm_state_machine_children_find_transition (state, transition->event, conditions_neg, _conditions_is_disjunct, &in_state))
    {
       g_critical ("Transition added to state \"%s\" conflicts with one in state \"%s\"",
                   g_quark_to_string (state->nick),
                   g_quark_to_string (in_state->nick));
       gsm_state_machine_transition_destroy (transition);
       return;
    }

  g_ptr_array_add (state->transitions, transition);
}

static void
gsm_state_machine_state_reparent (GsmStateMachineState *state, GsmStateMachineState *new_parent)
{
  /* The states must be sibblings for this to work. */
  g_assert (state->parent == NULL || state->parent == new_parent->parent);
  /* The new parent must not be a final state. */
  g_assert (new_parent->value < 0);

  /* The new group state might still be empty, if yes, setup the leader */
  if (new_parent->leader == NULL)
    {
      g_assert (new_parent->all_children == NULL);
      new_parent->leader = state;
      new_parent->all_children = g_ptr_array_new ();
    }

  if (state->parent)
    g_assert (g_ptr_array_remove (state->parent->all_children, state));

  g_ptr_array_add (new_parent->all_children, state);
  state->parent = new_parent;
}

static void
gsm_state_machine_state_destroy (GsmStateMachineState *state)
{
  g_clear_pointer (&state->outputs, g_ptr_array_unref);
  g_clear_pointer (&state->owned_values, g_ptr_array_unref);
  g_clear_pointer (&state->transitions, g_ptr_array_unref);
  g_clear_pointer (&state->all_children, g_ptr_array_unref);

  g_free (state);
}

/**
 * gsm_state_machine_new:
 * @state_type: The #GType of the state enum.
 *
 * Create a new #GsmStateMachine.
 *
 * Returns: (transfer full): a newly created #GsmStateMachine
 */
GsmStateMachine *
gsm_state_machine_new (GType state_type)
{
  return g_object_new (GSM_TYPE_STATE_MACHINE,
                       "state-type", state_type,
                       NULL);
}

static void
gsm_state_machine_finalize (GObject *object)
{
  GsmStateMachine *self = (GsmStateMachine *)object;
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (self);

  g_clear_pointer (&priv->events, g_array_unref);
  g_clear_pointer (&priv->inputs, g_hash_table_unref);
  g_clear_pointer (&priv->outputs, g_hash_table_unref);

  g_clear_pointer (&priv->current_outputs, g_ptr_array_unref);

  g_clear_pointer (&priv->states, g_hash_table_unref);

  g_clear_pointer (&priv->input_conditions, g_ptr_array_unref);
  g_clear_pointer (&priv->active_conditions, g_array_unref);
  g_clear_pointer (&priv->outputs_quark, g_array_unref);

  if (priv->idle_source_id)
    g_source_remove (priv->idle_source_id);

  G_OBJECT_CLASS (gsm_state_machine_parent_class)->finalize (object);
}

static void
gsm_state_machine_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  GsmStateMachine *self = GSM_STATE_MACHINE (object);

  switch (prop_id)
    {
    case PROP_STATE:
      g_value_set_int (value, gsm_state_machine_get_state (self));
      break;

    case PROP_STATE_TYPE:
      g_value_set_gtype (value, gsm_state_machine_get_state_type (self));
      break;

    case PROP_RUNNING:
      g_value_set_boolean (value, gsm_state_machine_get_running (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gsm_state_machine_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  GsmStateMachine *self = GSM_STATE_MACHINE (object);
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (self);
  g_autoptr(GEnumClass) enum_class = NULL;


  switch (prop_id)
    {
    case PROP_STATE_TYPE:
      g_assert (priv->state_type == G_TYPE_NONE);
      priv->state_type = g_value_get_gtype (value);
      g_assert (G_TYPE_IS_ENUM (priv->state_type));

      /* Ensure the states dictionary is filled */
      enum_class = G_ENUM_CLASS (g_type_class_ref (g_value_get_gtype (value)));
      if (!g_enum_get_value (enum_class, 0))
        g_error ("Enum must contain a value of 0 for the initial state.");

      priv->all_state = gsm_state_machine_state_new (g_quark_from_static_string ("all"), -1);
      gsm_state_machine_state_ensure_outputs (priv->all_state, priv->outputs);
      priv->last_group = -1;
      g_hash_table_insert (priv->states,
                           GINT_TO_POINTER (-1),
                           priv->all_state);

      for (guint i = 0; i < enum_class->n_values; i++)
        {
          GsmStateMachineState *state;
          GEnumValue *enum_value = &enum_class->values[i];

          if (enum_value->value < 0)
            g_error ("Negative values are reserved by the state machine and cannot be used in the state enum type.");

          state = gsm_state_machine_state_new (g_quark_from_static_string (enum_value->value_nick),
                                               enum_value->value);
          gsm_state_machine_state_reparent (state, priv->all_state);
          g_hash_table_insert (priv->states,
                               GINT_TO_POINTER (enum_value->value),
                               state);

          /* We may not add the zero state first, so just set it like this. */
          if (state->value == 0)
            priv->all_state->leader = state;
        }

      g_assert (priv->all_state->leader);

      break;

    case PROP_RUNNING:
      gsm_state_machine_set_running (self, g_value_get_boolean (value));

      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gsm_state_machine_class_init (GsmStateMachineClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gsm_state_machine_finalize;
  object_class->get_property = gsm_state_machine_get_property;
  object_class->set_property = gsm_state_machine_set_property;


  properties[PROP_STATE] =
    g_param_spec_int ("state", "State",
                      "The current state of the state machine",
                      G_MININT,
                      G_MAXINT,
                      0,
                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  properties[PROP_STATE_TYPE] =
    g_param_spec_gtype ("state-type", "StateType",
                        "The type of the state enum",
                        G_TYPE_ENUM,
                        G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_RUNNING] =
    g_param_spec_boolean ("running", "Running",
                          "Whether the state machine is updating from an idle handler",
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);

  signals[SIGNAL_STATE_ENTER] =
    g_signal_new ("state-enter", GSM_TYPE_STATE_MACHINE, G_SIGNAL_DETAILED,
                  G_STRUCT_OFFSET (GsmStateMachineClass, state_enter),
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 3,
                  G_TYPE_INT,
                  G_TYPE_INT,
                  G_TYPE_BOOLEAN);

  signals[SIGNAL_STATE_EXIT] =
    g_signal_new ("state-exit", GSM_TYPE_STATE_MACHINE, G_SIGNAL_DETAILED,
                  G_STRUCT_OFFSET (GsmStateMachineClass, state_exit),
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 3,
                  G_TYPE_INT,
                  G_TYPE_INT,
                  G_TYPE_BOOLEAN);

  signals[SIGNAL_INPUT_CHANGED] =
    g_signal_new ("input-changed", GSM_TYPE_STATE_MACHINE, G_SIGNAL_DETAILED,
                  G_STRUCT_OFFSET (GsmStateMachineClass, input_changed),
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_STRING,
                  G_TYPE_VALUE);

  signals[SIGNAL_OUTPUT_CHANGED] =
    g_signal_new ("output-changed", GSM_TYPE_STATE_MACHINE, G_SIGNAL_DETAILED,
                  G_STRUCT_OFFSET (GsmStateMachineClass, output_changed),
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 4,
                  G_TYPE_STRING,
                  G_TYPE_VALUE,
                  G_TYPE_BOOLEAN,
                  G_TYPE_BOOLEAN);
}


static void
gsm_state_machine_init (GsmStateMachine *self)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (self);

  priv->state_type = G_TYPE_NONE;

  priv->input_conditions = g_ptr_array_new_with_free_func ((GDestroyNotify) gsm_state_machine_input_condition_destroy);

  priv->events = g_array_new (FALSE, TRUE, sizeof (GQuark));
  priv->inputs = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) gsm_state_machine_value_destroy);
  priv->outputs = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) gsm_state_machine_value_destroy);

  priv->current_outputs = g_ptr_array_new ();

  priv->outputs_quark = g_array_new (FALSE, TRUE, sizeof (GQuark));

  priv->active_conditions = g_array_new (TRUE, TRUE, sizeof (GQuark));

  priv->states = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) gsm_state_machine_state_destroy);
}

static void
gsm_state_machine_internal_update_conditionals (GsmStateMachine *state_machine)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);

  g_array_set_size (priv->active_conditions, 0);

  for (guint i = 0; i < priv->input_conditions->len; i++)
    {
      GsmStateMachineCondition *condition;
      GValue value = G_VALUE_INIT;
      GQuark active;

      condition = g_ptr_array_index (priv->input_conditions, i);

      gsm_state_machine_get_input_value (state_machine, g_quark_to_string (condition->input), &value);
      active = condition->getter (condition->input, condition->type, &value);

      _condition_expand_positive (active, condition, priv->active_conditions);
    }

  g_array_sort (priv->active_conditions, _condition_cmp);
}

static void
gsm_state_machine_internal_update_outputs (GsmStateMachine *state_machine, GsmStateMachineState *sm_state_real)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  g_autoptr(GPtrArray)  old_outputs = NULL;
  gboolean output_missing;

  old_outputs = priv->current_outputs;
  priv->current_outputs = g_ptr_array_sized_new (old_outputs->len);
  g_ptr_array_set_size (priv->current_outputs, old_outputs->len);

  do
    {
      output_missing = TRUE;

      if (sm_state_real->outputs)
        {
          output_missing = FALSE;

          for (guint i = 0; i < priv->current_outputs->len; i++)
            {
              if (g_ptr_array_index (priv->current_outputs, i) != NULL)
                continue;

              /* The state might not have the full array. */
              if (i < sm_state_real->outputs->len)
                g_ptr_array_index (priv->current_outputs, i) = g_ptr_array_index (sm_state_real->outputs, i);

              if (g_ptr_array_index (priv->current_outputs, i) == NULL)
                output_missing = TRUE;
            }
        }

      sm_state_real = sm_state_real->parent;
    }
  while (output_missing);

  for (guint i = 0; i < priv->current_outputs->len; i++)
    {
      GValue *old_value;
      GValue *new_value;

      old_value = g_ptr_array_index (old_outputs, i);
      new_value = g_ptr_array_index (priv->current_outputs, i);

      /* Values point to the same object, nothing can have changed. */
      if (old_value == new_value)
        continue;

      g_signal_emit (state_machine,
                     signals[SIGNAL_OUTPUT_CHANGED],
                     g_array_index (priv->outputs_quark, GQuark, i),
                     g_quark_to_string (g_array_index (priv->outputs_quark, GQuark, i)),
                     new_value, TRUE);
    }
}

static gboolean
gsm_state_machine_internal_set_state (GsmStateMachine *state_machine, gint target_state)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  gint old_state;
  GsmStateMachineState *sm_state_old;
  GsmStateMachineState *sm_state_new;
  GsmStateMachineState *sm_state_real;

  old_state = priv->state;
  sm_state_old = g_hash_table_lookup (priv->states, GINT_TO_POINTER (old_state));
  g_assert (sm_state_old);

  sm_state_new = g_hash_table_lookup (priv->states, GINT_TO_POINTER (target_state));
  g_assert (sm_state_new);

  sm_state_real = sm_state_new;
  while (sm_state_real->leader)
    sm_state_real = sm_state_real->leader;

  if (sm_state_old == sm_state_real)
    return FALSE;

  target_state = sm_state_real->value;

  g_signal_emit (state_machine,
                 signals[SIGNAL_STATE_EXIT],
                 sm_state_old->nick,
                 old_state, target_state);

  g_debug ("Doing transition from state \"%s\" to state \"%s\" (\"%s\")",
           g_quark_to_string (sm_state_old->nick),
           g_quark_to_string (sm_state_real->nick),
           sm_state_new != sm_state_real ? g_quark_to_string (sm_state_new->nick) : "-");

  priv->state = target_state;
  g_object_notify_by_pspec (G_OBJECT (state_machine), properties[PROP_STATE]);

  gsm_state_machine_internal_update_outputs (state_machine, sm_state_real);

  g_signal_emit (state_machine,
                 signals[SIGNAL_STATE_ENTER],
                 sm_state_new->nick,
                 target_state, old_state);

  /* We may need further updates */
  gsm_state_machine_internal_queue_update (state_machine);

  return TRUE;
}

static gboolean
gsm_state_machine_internal_get_next_state (GsmStateMachine *state_machine, gint start_state, gint *new_state)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  GsmStateMachineState *sm_state = NULL;
  GsmStateMachineTransition *transition;

  sm_state = g_hash_table_lookup (priv->states, GINT_TO_POINTER (start_state));
  g_assert (sm_state);

  transition = gsm_state_machine_find_transition (sm_state, priv->active_event, priv->active_conditions, _conditions_is_subset, NULL);
  if (transition)
    {
      *new_state = transition->target_state;
      return TRUE;
    }

  return FALSE;
}

static void
gsm_state_machine_internal_update (GsmStateMachine *state_machine)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  gint next_state;
  gboolean transitioned;

  gsm_state_machine_internal_update_conditionals (state_machine);

  transitioned = gsm_state_machine_internal_get_next_state (state_machine, priv->state, &next_state);
  if (transitioned)
    transitioned = gsm_state_machine_internal_set_state (state_machine, next_state);

  if (!transitioned)
    {
      /* The state machine is currently stable, we can execute an event if one is pending */
      if (!priv->pending_events)
        return;

      priv->active_event = GPOINTER_TO_INT (priv->pending_events->data);
      priv->pending_events = g_list_delete_link (priv->pending_events, priv->pending_events);

      /* Re-check if the event caused a transition. */
      transitioned = gsm_state_machine_internal_get_next_state (state_machine, priv->state, &next_state);
      priv->active_event = 0;

      if (transitioned)
        gsm_state_machine_internal_set_state (state_machine, next_state);
    }
}

static gboolean
gsm_state_machine_internal_idle_update (gpointer user_data)
{
  GsmStateMachine *state_machine = GSM_STATE_MACHINE (user_data);
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);

  priv->idle_source_id = 0;

  gsm_state_machine_internal_update (state_machine);

  return G_SOURCE_REMOVE;
}

static void
gsm_state_machine_internal_queue_update (GsmStateMachine *state_machine)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);

  if (!priv->running)
    return;

  if (priv->idle_source_id)
    return;
  priv->idle_source_id = g_idle_add (gsm_state_machine_internal_idle_update, state_machine);
}

gint
gsm_state_machine_get_state (GsmStateMachine  *state_machine)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);

  return priv->state;
}

GType
gsm_state_machine_get_state_type (GsmStateMachine  *state_machine)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);

  return priv->state_type;
}

gboolean
gsm_state_machine_get_running (GsmStateMachine  *state_machine)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);

  return priv->running;
}

void
gsm_state_machine_set_running (GsmStateMachine  *state_machine,
                               gboolean          running)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);

  priv->running = running;

  if (priv->running)
    gsm_state_machine_internal_queue_update (state_machine);
  else
    {
      if (priv->idle_source_id)
        g_source_remove (priv->idle_source_id);
      priv->idle_source_id = 0;
    }
}

void
gsm_state_machine_add_event (GsmStateMachine  *state_machine,
                             const gchar      *event)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  GQuark event_quark = g_quark_from_string (event);

  if (_machine_has_condition (state_machine, event_quark) || _machine_has_event (state_machine, event_quark))
    {
      g_critical ("A condition or event with the name %s already exists", event);
      return;
    }

  g_array_append_val (priv->events, event_quark);
}

void
gsm_state_machine_queue_event (GsmStateMachine  *state_machine,
                               const gchar      *event)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  GQuark event_quark = g_quark_try_string (event);

  if (!event_quark || !_machine_has_event (state_machine, event_quark))
    {
      g_critical ("The event %s has not been registered\n", event);
      return;
    }

  priv->pending_events = g_list_append (priv->pending_events, GINT_TO_POINTER (event_quark));

  gsm_state_machine_internal_queue_update (state_machine);
}

void
gsm_state_machine_add_input (GsmStateMachine  *state_machine,
                             GParamSpec       *pspec)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  GsmStateMachineValue *value = NULL;

  g_assert (g_hash_table_lookup (priv->inputs, pspec->name) == NULL);

  value = gsm_state_machine_value_new ();
  value->pspec = g_param_spec_ref_sink (pspec);
  value->idx   = g_hash_table_size (priv->inputs);
  g_value_init (&value->value, G_PARAM_SPEC_VALUE_TYPE (value->pspec));
  g_value_copy (g_param_spec_get_default_value (pspec), &value->value);

  g_hash_table_insert (priv->inputs, (gpointer) pspec->name, value);
}

void
gsm_state_machine_add_output (GsmStateMachine  *state_machine,
                              GParamSpec       *pspec)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  GsmStateMachineValue *value = NULL;
  GQuark quark;

  g_assert (g_hash_table_lookup (priv->outputs, pspec->name) == NULL);

  value = gsm_state_machine_value_new ();
  value->pspec = g_param_spec_ref_sink (pspec);
  value->idx   = g_hash_table_size (priv->outputs);
  g_value_init (&value->value, G_PARAM_SPEC_VALUE_TYPE (pspec));
  g_value_copy (g_param_spec_get_default_value (pspec), &value->value);

  g_hash_table_insert (priv->outputs, (gpointer) pspec->name, value);

  /* Set default value in global table and the ALL group */
  g_assert (priv->current_outputs->len == value->idx);
  g_ptr_array_add (priv->current_outputs, &value->value);

  g_assert (priv->all_state->outputs->len == value->idx);
  g_ptr_array_add (priv->all_state->outputs, &value->value);

  quark = g_quark_from_static_string (pspec->name);
  g_array_append_val (priv->outputs_quark, quark);
}

void
gsm_state_machine_map_output (GsmStateMachine  *state_machine,
                              gint              state,
                              const gchar      *output,
                              const gchar      *input)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  GsmStateMachineState *sm_state = NULL;
  GsmStateMachineValue *output_value;
  GsmStateMachineValue *input_value;

  sm_state = g_hash_table_lookup (priv->states, GINT_TO_POINTER (state));
  g_assert (sm_state);

  gsm_state_machine_state_ensure_outputs (sm_state, priv->outputs);

  output_value = g_hash_table_lookup (priv->outputs, output);
  input_value = g_hash_table_lookup (priv->inputs, input);

  g_ptr_array_remove_fast (sm_state->owned_values, sm_state->outputs->pdata[output_value->idx]);
  sm_state->outputs->pdata[output_value->idx] = (gpointer) &input_value->value;
}

#if 0
void             gsm_state_machine_map_output_default  (GsmStateMachine  *state_machine,
                                                        const gchar      *output,
                                                        const gchar      *input);
#endif

#if 0
void             gsm_state_machine_get_input           (GsmStateMachine  *state_machine,
                                                        const gchar      *output,
                                                        ...);
#endif

void
gsm_state_machine_get_input_value (GsmStateMachine  *state_machine,
                                   const gchar      *input,
                                   GValue           *out)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  GsmStateMachineValue *input_value;

  input_value = g_hash_table_lookup (priv->inputs, input);

  g_value_init (out, G_VALUE_TYPE (&input_value->value));
  g_value_copy (&input_value->value, out);
}

void
gsm_state_machine_set_input (GsmStateMachine  *state_machine,
                             const gchar      *input,
                             ...)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  GsmStateMachineValue *input_value;
  g_autofree gchar *error = NULL;
  GValue value;
  va_list var_args;

  input_value = g_hash_table_lookup (priv->inputs, input);
  g_assert (input_value);

  va_start (var_args, input);

  G_VALUE_COLLECT_INIT (&value, input_value->pspec->value_type, var_args,
                        0, &error);
  if (error)
    {
      g_warning ("%s: %s", G_STRFUNC, error);
      g_value_unset (&value);
    }
  else
    {
      gsm_state_machine_set_input_value (state_machine, input, &value);
    }
  va_end (var_args);
}

void
gsm_state_machine_set_input_value (GsmStateMachine  *state_machine,
                                   const gchar      *input,
                                   const GValue     *value)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  GsmStateMachineValue *input_value;

  input_value = g_hash_table_lookup (priv->inputs, input);

  g_value_copy (value, &input_value->value);

  g_signal_emit (state_machine, signals[SIGNAL_INPUT_CHANGED], g_quark_from_string (input), input, value);

  for (guint i = 0; i < priv->current_outputs->len; i++)
    {
      /* Output value was updated if the pointers are identical. */
      if (&input_value->value != g_ptr_array_index (priv->current_outputs, i))
        continue;

      g_signal_emit (state_machine,
                     signals[SIGNAL_OUTPUT_CHANGED],
                     g_array_index (priv->outputs_quark, GQuark, i),
                     g_quark_to_string (g_array_index (priv->outputs_quark, GQuark, i)),
                     &input_value->value, FALSE, FALSE);
    }

  gsm_state_machine_internal_queue_update (state_machine);
}


#if 0
void             gsm_state_machine_get_output          (GsmStateMachine  *state_machine,
                                                        const gchar      *output,
                                                        ...);
#endif

void
gsm_state_machine_get_output_value (GsmStateMachine  *state_machine,
                                    const gchar      *output,
                                    GValue           *out)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  GsmStateMachineValue *output_value;

  output_value = g_hash_table_lookup (priv->outputs, output);

  g_value_init (out, G_VALUE_TYPE (&output_value->value));
  g_value_copy (g_ptr_array_index (priv->current_outputs, output_value->idx), out);
}

void
gsm_state_machine_set_output (GsmStateMachine  *state_machine,
                              gint              state,
                              const gchar      *output,
                              ...)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  GsmStateMachineValue *output_value;
  g_autofree gchar *error = NULL;
  GValue value;
  va_list var_args;

  output_value = g_hash_table_lookup (priv->outputs, output);
  g_assert (output_value);

  va_start (var_args, output);

  G_VALUE_COLLECT_INIT (&value, output_value->pspec->value_type, var_args,
                        0, &error);
  if (error)
    {
      g_warning ("%s: %s", G_STRFUNC, error);
      g_value_unset (&value);
    }
  else
    {
      gsm_state_machine_set_output_value (state_machine, state, output, &value);
    }
  va_end (var_args);
}

void
gsm_state_machine_set_output_value (GsmStateMachine  *state_machine,
                                    gint              state,
                                    const gchar      *output,
                                    const GValue     *value)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  GsmStateMachineState *sm_state;
  GsmStateMachineValue *output_value;
  GValue *new;

  output_value = g_hash_table_lookup (priv->outputs, output);

  sm_state = g_hash_table_lookup (priv->states, GINT_TO_POINTER (state));
  g_assert (sm_state);

  gsm_state_machine_state_ensure_outputs (sm_state, priv->outputs);

  output_value = g_hash_table_lookup (priv->outputs, output);

  g_ptr_array_remove_fast (sm_state->owned_values, sm_state->outputs->pdata[output_value->idx]);

  new = g_new0 (GValue, 1);
  g_ptr_array_add (sm_state->owned_values, new);

  g_value_init (new, G_PARAM_SPEC_VALUE_TYPE (output_value->pspec));
  g_value_copy (value, new);
  sm_state->outputs->pdata[output_value->idx] = new;

  while (sm_state->leader)
    sm_state = sm_state->leader;

  /* If we are currently in this state, then the output may have changed */
  if (state == priv->state)
    gsm_state_machine_internal_update_outputs (state_machine, sm_state);
}



void
gsm_state_machine_create_condition (GsmStateMachine      *state_machine,
                                    const gchar          *input,
                                    const GStrv           conditions,
                                    GsmConditionType      type,
                                    GsmConditionFunc      func)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  GsmStateMachineCondition *condition = NULL;
  guint conditions_len = g_strv_length (conditions);

  condition = gsm_state_machine_condition_new ();
  condition->type = type;
  condition->input = g_quark_from_string (input);
  condition->getter = func;

  for (guint i = 0; i < conditions_len; i++)
    {
      g_autofree gchar *cond = NULL;
      g_autofree gchar *cond_neg = NULL;
      GQuark quark;
      GQuark quark_neg;

      switch (type)
        {
        case GSM_CONDITION_TYPE_EQ:
          cond = g_strdup (conditions[i]);
          cond_neg = g_strdup_printf ("!%s", conditions[i]);
          break;
        case GSM_CONDITION_TYPE_GEQ:
          cond = g_strdup_printf (">=%s", conditions[i]);
          cond_neg = g_strdup_printf ("<%s", conditions[i]);
          break;
        case GSM_CONDITION_TYPE_LEQ:
          cond = g_strdup_printf ("<=%s", conditions[i]);
          cond_neg = g_strdup_printf (">%s", conditions[i]);
          break;
        }

      quark = g_quark_from_string (cond);
      quark_neg = g_quark_from_string (cond_neg);

      g_array_append_val (condition->conditions, quark);
      g_array_append_val (condition->conditions_neg, quark_neg);
    }

  g_ptr_array_add (priv->input_conditions, condition);
}

static GQuark
_state_machine_boolean_condition (GQuark condition, GsmConditionType type, const GValue *value)
{
  if (g_value_get_boolean (value))
    return condition;
  else
    return 0;
}

static GQuark
_state_machine_enum_condition (GQuark condition, GsmConditionType type, const GValue *value)
{
  GEnumClass *enum_class = G_ENUM_CLASS (g_type_class_peek (value->g_type));
  GEnumValue *enum_class_value;
  gint enum_value = g_value_get_enum (value);
  const gchar *value_nick;
  g_autofree gchar *detailed_condition;
  const gchar *prefix;

  enum_class_value = g_enum_get_value (enum_class, enum_value);
  value_nick = enum_class_value->value_nick;

  /* XXX: This is kinda ugly, right? Maybe we need some sort of API change ... */
  switch (type)
    {
    case GSM_CONDITION_TYPE_EQ:
      prefix = "";
      break;
    case GSM_CONDITION_TYPE_GEQ:
      prefix = ">=";
      break;
    case GSM_CONDITION_TYPE_LEQ:
      prefix = "<=";
      break;
    }
  detailed_condition = g_strdup_printf ("%s%s::%s", prefix, g_quark_to_string (condition), value_nick);

  return g_quark_try_string (detailed_condition);
}

void
gsm_state_machine_create_default_condition (GsmStateMachine      *state_machine,
                                            const gchar          *input,
                                            GsmConditionType      type)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  GsmStateMachineValue *input_value;
  g_autoptr(GPtrArray) conditions = NULL;

  input_value = g_hash_table_lookup (priv->inputs, input);

  if (g_type_is_a (G_PARAM_SPEC_VALUE_TYPE (input_value->pspec), G_TYPE_BOOLEAN))
    {
      conditions = g_ptr_array_new ();
      g_ptr_array_add (conditions, (gchar*) input);
      g_ptr_array_add (conditions, NULL);

      gsm_state_machine_create_condition (state_machine,
                                          input,
                                          (const GStrv) conditions->pdata,
                                          type,
                                          _state_machine_boolean_condition);
    }
  else if (g_type_is_a (G_PARAM_SPEC_VALUE_TYPE (input_value->pspec), G_TYPE_ENUM))
    {
      GEnumClass *enum_class = G_ENUM_CLASS (g_type_class_peek (G_PARAM_SPEC_VALUE_TYPE (input_value->pspec)));
      guint i;

      conditions = g_ptr_array_new_with_free_func (g_free);

      for (i = 0; i < enum_class->n_values; i++)
        {
          gchar *condition = g_strconcat (input, "::", enum_class->values[i].value_nick, NULL);
          g_ptr_array_add (conditions, (gchar*) condition);
        }

      g_ptr_array_add (conditions, NULL);

      gsm_state_machine_create_condition (state_machine,
                                          input,
                                          (const GStrv) conditions->pdata,
                                          type,
                                          _state_machine_enum_condition);
    }
  else
    {
      g_assert_not_reached ();
    }
}

void
gsm_state_machine_add_edge (GsmStateMachine  *state_machine,
                            gint              start_state,
                            gint              target_state,
                            ...)
{
  va_list var_args;

  va_start (var_args, target_state);
  gsm_state_machine_add_edge_valist (state_machine,
                                     start_state,
                                     target_state,
                                     var_args);
  va_end (var_args);
}

void
gsm_state_machine_add_edge_valist (GsmStateMachine  *state_machine,
                                   gint              start_state,
                                   gint              target_state,
                                   va_list           var_args)
{
  const gchar *condition;
  g_autoptr(GPtrArray) conditions = NULL;

  conditions = g_ptr_array_new ();
  condition = va_arg (var_args, const gchar*);

  while (condition)
    {
      g_ptr_array_add (conditions, (gchar*) condition);

      condition = va_arg (var_args, const gchar*);
    }

  g_ptr_array_add (conditions, NULL);

  gsm_state_machine_add_edge_strv (state_machine,
                                   start_state,
                                   target_state,
                                   (GStrv) conditions->pdata);
}

void
gsm_state_machine_add_edge_strv (GsmStateMachine  *state_machine,
                                 gint              start_state,
                                 gint              target_state,
                                 const GStrv       conditions)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  GsmStateMachineState *sm_state;
  GsmStateMachineTransition *transition;
  guint conditions_len = g_strv_length (conditions);

  g_return_if_fail (start_state != target_state);

  sm_state = g_hash_table_lookup (priv->states, GINT_TO_POINTER (start_state));
  g_assert (sm_state);

  /* Check the target state (or group) exists */
  g_assert (g_hash_table_lookup (priv->states, GINT_TO_POINTER (target_state)));

  transition = gsm_state_machine_transition_new ();
  transition->target_state = target_state;

  /* Build the conditions quark list */
  for (gint i = 0; i < conditions_len; i++)
    {
      GQuark condition = g_quark_from_string (conditions[i]);

      if (!_machine_has_condition (state_machine, condition))
        {
          if (!_machine_has_event (state_machine, condition))
            {
              g_critical ("Neither condition nor event \"%s\" is known for the state machine, defined edge will never execute",
                          g_quark_to_string (condition));
            }
          else
            {
              if (transition->event)
                g_critical ("Tried to add second event %s, will keep using %s",
                            g_quark_to_string (condition), g_quark_to_string (transition->event));
              else
                transition->event = condition;
            }
        }
      else
        g_array_append_val (transition->conditions, condition);
    }

  g_array_sort (transition->conditions, _condition_cmp);

  gsm_state_machine_state_add_transition (state_machine, sm_state, transition);
}

gint
gsm_state_machine_create_group (GsmStateMachine  *state_machine,
                                const gchar*      name,
                                gint              count,
                                gint              first_child,
                                ...)
{
  gint res;
  gint i;
  gint *children = g_malloc_n (count, sizeof (gint));
  va_list var_args;

  va_start (var_args, first_child);

  children[0] = first_child;
  for (i = 1; i < count; i++)
    children[i] = va_arg (var_args, gint);

  res = gsm_state_machine_create_group_array (state_machine, name, count, children);

  va_end (var_args);
  g_free (children);

  return res;
}

gint
gsm_state_machine_create_group_array (GsmStateMachine  *state_machine,
                                      const gchar*      name,
                                      gint              count,
                                      gint             *children)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  GsmStateMachineState *group;
  GsmStateMachineState *leader;
  g_assert (count > 0);
  g_assert (children);

  priv->last_group--;

  group = gsm_state_machine_state_new (g_quark_from_string (name), priv->last_group);
  leader = g_hash_table_lookup (priv->states, GINT_TO_POINTER (children[0]));

  /* Put the new group on the same level as the leader, then move the leader. */
  gsm_state_machine_state_reparent (group, leader->parent);
  gsm_state_machine_state_reparent (leader, group);

  /* And move all the other ones. */
  for (guint i = 1; i < count; i++)
    {
      GsmStateMachineState *state;

      state = g_hash_table_lookup (priv->states, GINT_TO_POINTER (children[i]));
      gsm_state_machine_state_reparent (state, group);
    }

  g_hash_table_insert (priv->states, GINT_TO_POINTER (group->value), group);

  return group->value;
}

void
_add_nodes_to_dot (GsmStateMachine      *state_machine,
                   GsmStateMachineState *state,
                   GPtrArray            *chunks)
{
  if (state->value >= 0)
    {
      if (state->parent->leader == state)
        g_ptr_array_add (chunks, g_strdup_printf ("  \"%s\" [shape=ellipse,color=green,pos=\"0,0!\"];", g_quark_to_string (state->nick)));
      else
        g_ptr_array_add (chunks, g_strdup_printf ("  \"%s\" [shape=ellipse];", g_quark_to_string (state->nick)));
    }
  else
    {
      g_ptr_array_add (chunks, g_strdup_printf ("  subgraph \"cluster_%s\" {", g_quark_to_string (state->nick)));
      g_ptr_array_add (chunks, g_strdup_printf ("    label = \"%s\";", g_quark_to_string (state->nick)));

      for (gint i = 0; i < state->all_children->len; i++)
        _add_nodes_to_dot (state_machine, g_ptr_array_index (state->all_children, i), chunks);

       g_ptr_array_add (chunks, g_strdup_printf ("  }"));
  	}
}

void
_add_transitions_to_dot (GsmStateMachine      *state_machine,
                         GsmStateMachineState *state,
                         GPtrArray            *chunks)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);

  for (guint i = 0; i < state->transitions->len; i++)
    {
      GsmStateMachineTransition *transition = g_ptr_array_index (state->transitions, i);
      GsmStateMachineState *target = g_hash_table_lookup (priv->states, GINT_TO_POINTER (transition->target_state));
      GsmStateMachineState *real_target, *real_state;
      g_autoptr(GPtrArray) conditions = g_ptr_array_new ();
      g_autofree gchar *label = NULL;

      real_target = target;
      while (real_target->leader)
        real_target = real_target->leader;

      /* Ignore tranistions to ourselves */
      if (state == real_target)
        continue;

      real_state = state;
      while (real_state->leader)
        real_state = real_state->leader;

      if (transition->event)
        g_ptr_array_add (conditions, (gpointer) g_quark_to_string (transition->event));

      for (guint j = 0; j < transition->conditions->len; j++)
        g_ptr_array_add (conditions, (gpointer) g_quark_to_string (g_array_index (transition->conditions, GQuark, j)));

      g_ptr_array_add (conditions, NULL);
      label = g_strjoinv (" &\n", (GStrv) conditions->pdata);

      g_ptr_array_add (chunks,
                       g_strdup_printf ("  \"%s\" -> \"%s\" [ label = \"%s\",color=\"%s%s%s%s%s\"];",
                                        g_quark_to_string (real_state->nick),
                                        g_quark_to_string (real_target->nick),
                                        label,
                                        transition->event ? "red" : "black",
                                        state->value < 0 ? "\",ltail=\"cluster_" : "",
                                        state->value < 0 ? g_quark_to_string (state->nick) : "",
                                        target->value < 0 ? "\",lhead=\"cluster_" : "",
                                        target->value < 0 ? g_quark_to_string (target->nick) : ""));
    }

  if (state->value < 0)
    {
      for (gint i = 0; i < state->all_children->len; i++)
        _add_transitions_to_dot (state_machine, g_ptr_array_index (state->all_children, i), chunks);
    }
}

void
gsm_state_machine_to_dot_file (GsmStateMachine  *state_machine,
                               gchar            *filename)
{
  GsmStateMachinePrivate *priv = GSM_STATE_MACHINE_PRIVATE (state_machine);
  const gchar *path = g_getenv ("GSM_STATE_MACHINE_DOT_DIR");
  g_autofree gchar *file = NULL;
  g_autofree gchar *contents = NULL;
  g_autoptr(GPtrArray) chunks = NULL;

  if (!path)
    return;

  file = g_build_filename (path, filename, NULL);

  chunks = g_ptr_array_new_with_free_func (g_free);

  g_ptr_array_add (chunks, g_strdup ("digraph finite_state_machine {"));
  g_ptr_array_add (chunks, g_strdup ("  compound=true;"));

  _add_nodes_to_dot (state_machine, priv->all_state, chunks);
  _add_transitions_to_dot (state_machine, priv->all_state, chunks);

  g_ptr_array_add (chunks, g_strdup ("}"));
  g_ptr_array_add (chunks, NULL);

  contents = g_strjoinv ("\n", (GStrv) chunks->pdata);
  g_file_set_contents (file, contents, strlen (contents), NULL);
}
